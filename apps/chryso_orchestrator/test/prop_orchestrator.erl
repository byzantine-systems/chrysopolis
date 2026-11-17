-module(prop_orchestrator).
-moduledoc """
The running tree against `chryso_fake_root`, as a `proper_statem`: the
process-level twin of `prop_lifecycle`. Faults, torn reads, lost replies and
crashes of the port or the controller interleave with wants.

Laws, checked after every command:

- **at most once**: Root accepts no more restarts of a driver from commands
  than wants were made for it.
- **replay**: the decision log replays through the pure step.

After the run, with fair rounds, every want of a driver Root still runs holds.

| Planted defect | Caught by |
|---|---|
| the controller sends every command twice | replay |
| wants not kept across a controller restart | converges |
| the port makes up `refused` when another opener holds the channel | converges |
""".
-behaviour(proper_statem).
-include_lib("proper/include/proper.hrl").

-export([prop_orchestrator/0]).
-export([initial_state/0, command/1, precondition/2, postcondition/3, next_state/3]).
-export([want/1, fault/1, tear/1, drop/0, busy/0, kill/1, scan/0, expire/0]).
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
initial_state() ->
    #{wants => #{}, tear => false, kills => #{chryso_root_port => 0, chryso_lifecycle_ctl => 0}}.

command(#{tear := Tear}) ->
    frequency([
        {4, {call, ?MODULE, want, [elements(?DRIVERS)]}},
        {2, {call, ?MODULE, fault, [elements(?DRIVERS)]}},
        {1, {call, ?MODULE, tear, [not Tear]}},
        {1, {call, ?MODULE, drop, []}},
        {1, {call, ?MODULE, busy, []}},
        {1, {call, ?MODULE, kill, [elements([chryso_root_port, chryso_lifecycle_ctl])]}},
        {4, {call, ?MODULE, scan, []}},
        {2, {call, ?MODULE, expire, []}}
    ]).

precondition(#{kills := Kills}, {call, _, kill, [Name]}) ->
    maps:get(Name, Kills) < limit(Name);
precondition(_State, _Call) ->
    true.

limit(chryso_root_port) -> 3;
limit(chryso_lifecycle_ctl) -> 5.

next_state(#{wants := Wants} = State, _Result, {call, _, want, [C]}) ->
    State#{wants := Wants#{C => maps:get(C, Wants, 0) + 1}};
next_state(State, _Result, {call, _, tear, [On]}) ->
    State#{tear := On};
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
    Once andalso Replay.

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

stop(Sup) ->
    Ref = monitor(process, Sup),
    exit(Sup, shutdown),
    receive
        {'DOWN', Ref, process, Sup, _} -> ok
    end,
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
