-module(prop_orchestrator).
-moduledoc """
The running tree against `chryso_fake_root`, as a `proper_statem`: the
process-level twin of `prop_lifecycle`. Faults, torn reads, lost replies and
crashes of the port or the controller interleave with wants.

Laws, checked after every command:

- **at most once**: Root accepts no more restarts of a driver from commands
  than wants were made for it.
- **replay**: the decision log replays through the pure step.
- **fresh**: after a BEAM halt the new incarnation wants nothing. Wants are
  counted across incarnations, so a halt that resends breaks at most once.

After the run, with fair rounds, every want of a driver Root still runs holds.

| Planted defect | Caught by |
|---|---|
| the controller sends every command twice | replay |
| wants not kept across a controller restart | converges |
| the port makes up `refused` when another opener holds the channel | converges |
| the controller re-asserts every driver at startup | fresh |
""".
-behaviour(proper_statem).
-include_lib("proper/include/proper.hrl").

-export([prop_orchestrator/0]).
-export([initial_state/0, command/1, precondition/2, postcondition/3, next_state/3]).
-export([want/1, fault/1, tear/1, drop/0, busy/0, kill/1, halt/0, scan/0, expire/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(DRIVERS, [0, 1, 2, 3]).
-define(DEADLINE, 30).
-define(CONFIG, #{
    drivers => ?DRIVERS,
    scan_ms => 600000,
    deadline_ms => ?DEADLINE,
    backoff_ms => {10, 40},
    settle_ms => 0,
    floor => 0
}).

prop_orchestrator() ->
    ?FORALL(
        Cmds,
        commands(?MODULE),
        begin
            Sup = start(),
            {History, State, Result} = run_commands(?MODULE, Cmds),
            Converged = converged(),
            stop(Sup),
            ?WHENFAIL(
                io:format("history: ~p~nstate: ~p~nresult: ~p~nconverged: ~p~n", [
                    History, State, Result, Converged
                ]),
                aggregate(
                    command_names(Cmds),
                    Result =:= ok andalso Converged =:= []
                )
            )
        end
    ).

%% --- model ----------------------------------------------------------------------

%% Wants made per driver: all the laws need, since Root holds the rest. Kills
%% stay under each supervisor's intensity (port 3, controller 5, in 10 s),
%% so neither gives up.
%% Halts stay within the fake Root's BEAM budget (2), so Root never gives the
%% BEAM up.
initial_state() ->
    #{wants => #{}, tear => false, kills => kills(), halts => 0}.

kills() ->
    #{chryso_root_port => 0, chryso_lifecycle_ctl => 0}.

command(#{tear := Tear}) ->
    frequency([
        {4, {call, ?MODULE, want, [elements(?DRIVERS)]}},
        {2, {call, ?MODULE, fault, [elements(?DRIVERS)]}},
        {1, {call, ?MODULE, tear, [not Tear]}},
        {1, {call, ?MODULE, drop, []}},
        {1, {call, ?MODULE, busy, []}},
        {1, {call, ?MODULE, kill, [elements([chryso_root_port, chryso_lifecycle_ctl])]}},
        {1, {call, ?MODULE, halt, []}},
        {4, {call, ?MODULE, scan, []}},
        {2, {call, ?MODULE, expire, []}}
    ]).

precondition(#{kills := Kills}, {call, _, kill, [Name]}) ->
    maps:get(Name, Kills) < limit(Name);
precondition(#{halts := Halts}, {call, _, halt, []}) ->
    Halts < 2;
precondition(_State, _Call) ->
    true.

limit(chryso_root_port) -> 3;
limit(chryso_lifecycle_ctl) -> 5.

next_state(#{wants := Wants} = State, _Result, {call, _, want, [C]}) ->
    State#{wants := Wants#{C => maps:get(C, Wants, 0) + 1}};
next_state(State, _Result, {call, _, tear, [On]}) ->
    State#{tear := On};
next_state(#{halts := Halts} = State, _Result, {call, _, halt, []}) ->
    %% A fresh tree, so its supervisors' intensity counts start over.
    State#{halts := Halts + 1, kills := kills()};
next_state(#{kills := Kills} = State, _Result, {call, _, kill, [Name]}) ->
    State#{kills := Kills#{Name := maps:get(Name, Kills) + 1}};
next_state(State, _Result, _Call) ->
    State.

postcondition(#{wants := Wants}, {call, _, Name, _}, _Result) ->
    Accepted = chryso_fake_root:accepted(),
    %% A want made by this very command counts too.
    Made = fun(C) -> maps:get(C, Wants, 0) + 1 end,
    Once = lists:all(fun(C) -> length([x || {D, _} <- Accepted, D =:= C]) =< Made(C) end, ?DRIVERS),
    Replay =
        case Name of
            kill -> true;
            _ -> chryso_orchestrator:replay() =:= match
        end,
    Fresh =
        case Name of
            halt -> chryso_orchestrator:wanted() =:= #{};
            _ -> true
        end,
    case [Law || {Law, false} <- [{at_most_once, Once}, {replay, Replay}, {fresh, Fresh}]] of
        [] ->
            true;
        Broken ->
            io:format("broken: ~p~n", [Broken]),
            false
    end.

%% --- the system under test ------------------------------------------------------

want(C) ->
    chryso_orchestrator:want(C).

fault(C) ->
    chryso_fake_root:fault(C).

tear(On) ->
    chryso_fake_root:tear(On).

drop() ->
    chryso_fake_root:drop_next().

busy() ->
    chryso_fake_root:busy_next().

%% Kill one process and wait for its replacement.
kill(Name) ->
    Old = whereis(Name),
    exit(Old, kill),
    wait_new(Name, Old, 200).

%% A BEAM restart: the VM's processes and persistent terms are gone, Root
%% counts a BEAM restart, and the application starts again from Root's page.
halt() ->
    Sup = whereis(chryso_orchestrator_sup),
    stop_tree(Sup),
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    resume = chryso_fake_root:halt(),
    {ok, New} = chryso_orchestrator_sup:start_link(chryso_fake_root, ?CONFIG),
    unlink(New),
    ok.

scan() ->
    ok = chryso_root_port:sync(1000),
    chryso_orchestrator:scan().

expire() ->
    timer:sleep(?DEADLINE + 5),
    scan().

%% --- helpers --------------------------------------------------------------------

start() ->
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    {ok, _} = chryso_fake_root:start(),
    {ok, Sup} = chryso_orchestrator_sup:start_link(chryso_fake_root, ?CONFIG),
    unlink(Sup),
    Sup.

stop(_Sup) ->
    %% The tree a halt started may have replaced the first one.
    stop_tree(whereis(chryso_orchestrator_sup)),
    ok = chryso_fake_root:stop(),
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    ok.

%% Fair rounds (reads whole, deadlines pass, replies flow), then the wants of
%% live drivers that do not hold.
converged() ->
    ok = chryso_fake_root:tear(false),
    lists:foreach(fun(_) -> _ = expire() end, lists:seq(1, 12)),
    Wants = chryso_orchestrator:wanted(),
    States = chryso_fake_root:states(),
    Last = chryso_fake_root:last(),
    [{C, T} || C := T <- Wants, maps:get(C, States) =:= live, maps:get(C, Last, 0) < T].

wait_new(_Name, _Old, 0) ->
    error(no_replacement);
wait_new(Name, Old, N) ->
    case whereis(Name) of
        Pid when is_pid(Pid), Pid =/= Old ->
            ok;
        _ ->
            timer:sleep(5),
            wait_new(Name, Old, N - 1)
    end.

stop_tree(Sup) when is_pid(Sup) ->
    Ref = monitor(process, Sup),
    exit(Sup, shutdown),
    receive
        {'DOWN', Ref, process, Sup, _} -> ok
    end;
stop_tree(undefined) ->
    ok.
