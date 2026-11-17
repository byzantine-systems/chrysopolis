-module(chryso_orchestrator_tests).
-moduledoc """
The tree against `chryso_fake_root`. Scans are driven by hand (`scan/0`), so
the only clock a test waits on is the command deadline. Every case ends with
the decision log replaying through the pure step.

| Planted defect | Caught by |
|---|---|
| the controller sends every command twice | the replay in want_restarts_once and lost_reply_restarts_once. Root's fence makes the copy `already_done`, so the restart counts alone miss it |
""".
-include_lib("eunit/include/eunit.hrl").

-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(DEADLINE, 100).
-define(CONFIG, #{
    drivers => [0, 1, 2, 3],
    scan_ms => 600000,
    deadline_ms => ?DEADLINE,
    backoff_ms => {50, 400},
    settle_ms => 0,
    floor => 0
}).

orchestrator_test_() ->
    {foreach, fun setup/0, fun cleanup/1, [
        fun every_driver_ready/1,
        fun torn_page_is_unknown/1,
        fun want_restarts_once/1,
        fun lost_reply_restarts_once/1,
        fun port_crash_forgets_nothing/1,
        fun gone_is_never_commanded/1,
        fun crash_looping_port_stops_the_tree/1
    ]}.

setup() ->
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    {ok, _} = chryso_fake_root:start(),
    {ok, Sup} = chryso_orchestrator_sup:start_link(chryso_fake_root, ?CONFIG),
    unlink(Sup),
    Sup.

cleanup(Sup) ->
    Ref = monitor(process, Sup),
    exit(Sup, shutdown),
    receive
        {'DOWN', Ref, process, Sup, _} -> ok
    after 5000 -> error(sup_did_not_stop)
    end,
    ok = chryso_fake_root:stop(),
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    ok.

%% --- cases ----------------------------------------------------------------------

every_driver_ready(_) ->
    ok = chryso_orchestrator:scan(),
    #{conditions := Conditions, observed := Observed} = chryso_orchestrator:status(),
    [
        ?_assertEqual(#{C => {ready, settled} || C <- [0, 1, 2, 3]}, Conditions),
        ?_assertMatch(#{root_generation := 1, beam_incarnation := 1}, Observed),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

%% A torn read after a known page changes nothing. A controller whose first
%% read is torn knows nothing and commands nothing.
torn_page_is_unknown(_) ->
    ok = chryso_orchestrator:scan(),
    Known = chryso_orchestrator:status(),
    ok = chryso_fake_root:tear(true),
    ok = chryso_orchestrator:scan(),
    Kept = chryso_orchestrator:status(),
    Ctl = whereis(chryso_lifecycle_ctl),
    exit(Ctl, kill),
    ok = until(fun() -> fresh(chryso_lifecycle_ctl, Ctl) end),
    ok = chryso_orchestrator:scan(),
    #{conditions := Conditions, observed := Observed} = chryso_orchestrator:status(),
    Want = chryso_orchestrator:want(1),
    ok = chryso_fake_root:tear(false),
    [
        ?_assertEqual(Known, Kept),
        ?_assertEqual(#{C => {unknown, no_observation} || C <- [0, 1, 2, 3]}, Conditions),
        ?_assertEqual(unknown, Observed),
        ?_assertEqual({error, no_observation}, Want),
        ?_assertEqual([], chryso_fake_root:accepted()),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

want_restarts_once(_) ->
    ok = chryso_orchestrator:scan(),
    {ok, T} = chryso_orchestrator:want(1),
    ok = until(fun() -> length(chryso_fake_root:accepted()) =:= 1 end),
    ok = rounds(3),
    {ok, T} = chryso_orchestrator:want(1, T),
    ok = rounds(3),
    #{conditions := #{1 := Timer}} = chryso_orchestrator:status(),
    [
        ?_assertMatch([{1, _}], chryso_fake_root:accepted()),
        ?_assertEqual({ready, settled}, Timer),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

%% Root acts, the reply is lost, the deadline fires, and the next read shows
%% the restart, so nothing is resent.
lost_reply_restarts_once(_) ->
    ok = chryso_orchestrator:scan(),
    ok = chryso_fake_root:drop_next(),
    {ok, _} = chryso_orchestrator:want(1),
    ok = chryso_root_port:sync(1000),
    timer:sleep(2 * ?DEADLINE),
    ok = rounds(3),
    timer:sleep(2 * ?DEADLINE),
    ok = rounds(3),
    [
        ?_assertMatch([{1, _}], chryso_fake_root:accepted()),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

%% A port crash restarts the controller, which keeps the want and finds it
%% already satisfied.
port_crash_forgets_nothing(_) ->
    ok = chryso_orchestrator:scan(),
    {ok, _} = chryso_orchestrator:want(1),
    ok = until(fun() -> length(chryso_fake_root:accepted()) =:= 1 end),
    Ctl = whereis(chryso_lifecycle_ctl),
    exit(whereis(chryso_root_port), kill),
    ok = until(fun() -> fresh(chryso_lifecycle_ctl, Ctl) end),
    ok = rounds(3),
    #{conditions := #{1 := Timer}} = chryso_orchestrator:status(),
    [
        ?_assertEqual({ready, settled}, Timer),
        ?_assertMatch([{1, _}], chryso_fake_root:accepted()),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

%% Driver 3 has a budget of 1: the second fault gives it up.
gone_is_never_commanded(_) ->
    ok = chryso_orchestrator:scan(),
    resume = chryso_fake_root:fault(3),
    _ = chryso_fake_root:fault(3),
    ok = chryso_orchestrator:scan(),
    {ok, _} = chryso_orchestrator:want(3),
    ok = rounds(3),
    #{conditions := #{3 := Eth}} = chryso_orchestrator:status(),
    [
        ?_assertEqual({gone, gave_up}, Eth),
        ?_assertEqual([], chryso_fake_root:accepted()),
        ?_assertEqual(match, chryso_orchestrator:replay())
    ].

%% The chosen failure: past the top supervisor's intensity (3 in 10 s) the tree
%% shuts down instead of looping.
crash_looping_port_stops_the_tree(Sup) ->
    Ref = monitor(process, Sup),
    lists:foreach(
        fun(_) ->
            Port = whereis(chryso_root_port),
            exit(Port, kill),
            _ = (catch until(fun() -> fresh(chryso_root_port, Port) end, 20))
        end,
        lists:seq(1, 4)
    ),
    Down =
        receive
            {'DOWN', Ref, process, Sup, Reason} -> Reason
        after 2000 -> alive
        end,
    ?_assertEqual(shutdown, Down).

%% --- helpers --------------------------------------------------------------------

%% Let queued commands reach the fake, their replies reach the controller, and
%% the controller read the result.
rounds(0) ->
    ok;
rounds(N) ->
    ok = chryso_root_port:sync(1000),
    ok = chryso_orchestrator:scan(),
    rounds(N - 1).

until(Pred) ->
    until(Pred, 100).

until(_Pred, 0) ->
    error(timeout);
until(Pred, N) ->
    _ = catch chryso_root_port:sync(1000),
    _ = catch chryso_orchestrator:scan(),
    case Pred() of
        true ->
            ok;
        false ->
            timer:sleep(10),
            until(Pred, N - 1)
    end.

fresh(Name, Old) ->
    case whereis(Name) of
        undefined -> false;
        Old -> false;
        _ -> true
    end.
