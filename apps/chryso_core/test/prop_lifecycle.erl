-module(prop_lifecycle).
-moduledoc """
`chryso_lifecycle` against the shipped `chryso_root`, as a `proper_statem`.
The SUT is `lifecycle_world`. The model holds the oracle's bookkeeping, and
`postcondition/3` judges each step:

- **unknown postpones**: no command before the controller has a known page.
- **never resurrect**: no command for a driver a page showed gone.
- **sent once**: one controller incarnation resends a command only after a
  deadline or `rate_limited`, the cases where the page cannot move.
- **at most once**: each want causes at most one restart Root accepts.

After the run, fair rounds must leave no open want (**converges**).

| Planted defect | Caught by |
|---|---|
| no wait for a newer page after `ok` | sent once |
| planning while nothing is known | unknown postpones |
| `rate_limited` given up for good | converges |
| `merge` keeping the older page | converges |
""".
-include_lib("proper/include/proper.hrl").

-behaviour(proper_statem).
-export([initial_state/0, command/1, precondition/2, postcondition/3, next_state/3]).
-export([
    prop_lifecycle/0,
    prop_duplicate_reads_change_nothing/0,
    prop_a_held_want_needs_no_command/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(W, lifecycle_world).

prop_lifecycle() ->
    ?FORALL(
        Cmds,
        commands(?MODULE),
        begin
            ok = ?W:reset(),
            erase(broken_law),
            {History, State, Result} = run_commands(?MODULE, Cmds),
            Open = ?W:open_wants(),
            ?WHENFAIL(
                io:format("law=~p~nresult=~p~nopen=~p~nhistory=~p~n", [
                    get(broken_law), Result, Open, History
                ]),
                aggregate(
                    classes(State),
                    aggregate(command_names(Cmds), Result =:= ok andalso Open =:= [])
                )
            )
        end
    ).

%% Reading the same page twice in a row, or an older page after it, changes
%% neither effects nor state.
prop_duplicate_reads_change_nothing() ->
    ?FORALL(
        Cmds,
        commands(?MODULE),
        begin
            ok = ?W:reset(),
            _ = run_commands(?MODULE, Cmds),
            Page = ?W:page(),
            {_, S1} = chryso_lifecycle:step({observed, Page}, ?W:controller()),
            {E2, S2} = chryso_lifecycle:step({observed, Page}, S1),
            {E3, S3} = chryso_lifecycle:step({observed, chryso_observe:unknown()}, S1),
            E2 =:= [] andalso S2 =:= S1 andalso E3 =:= [] andalso S3 =:= S1
        end
    ).

%% Recovery: a fresh controller given wants that already hold sends nothing.
prop_a_held_want_needs_no_command() ->
    ?FORALL(
        Cmds,
        commands(?MODULE),
        begin
            ok = ?W:reset(),
            _ = run_commands(?MODULE, Cmds),
            _ = ?W:open_wants(),
            Inputs = [{observed, ?W:page()} | [{want, C, T} || C := T <- ?W:last_restarts()]],
            {Effects, _} = lists:foldl(
                fun(I, {Acc, S}) ->
                    {E, S1} = chryso_lifecycle:step(I, S),
                    {Acc ++ E, S1}
                end,
                {[], chryso_lifecycle:new(?W:params())},
                Inputs
            ),
            [E || {command, _, _, _} = E <- Effects] =:= []
        end
    ).

%% --- proper_statem callbacks -----------------------------------------------------

initial_state() ->
    #{seen => false, gone => [], sent => [], resendable => [], accepted => #{}}.

command(_State) ->
    frequency([
        {4, {call, ?W, fault, [driver()]}},
        {4, {call, ?W, want, [driver()]}},
        {6, {call, ?W, scan, []}},
        {2, {call, ?W, stale_scan, []}},
        {1, {call, ?W, unknown_scan, []}},
        {5, {call, ?W, deliver, []}},
        {2, {call, ?W, lose, []}},
        {1, {call, ?W, drop, []}},
        {2, {call, ?W, deadline, []}},
        {3, {call, ?W, tick, [integer(0, 600)]}},
        {1, {call, ?W, restart_ctl, []}},
        {1, {call, ?W, flood, [integer(1, 6)]}}
    ]).

%% Every world event is always possible.
precondition(_State, _Call) ->
    true.

postcondition(S, _Call, #{emitted := Emitted, accepted := Accepted, known := Known, gone := Gone}) ->
    #{seen := Seen, gone := WasGone, sent := Sent, resendable := Late, accepted := Counts} = S,
    Checks = [
        {unknown_postpones, Emitted =:= [] orelse Seen orelse Known},
        {never_resurrect, [C || {C, _} <- Emitted, lists:member(C, WasGone ++ Gone)] =:= []},
        {sent_once,
            unique(Emitted) andalso
                [X || X <- Emitted, lists:member(X, Sent), not lists:member(X, Late)] =:= []},
        {at_most_once, unique(Accepted) andalso [P || P <- Accepted, maps:is_key(P, Counts)] =:= []}
    ],
    case [Law || {Law, false} <- Checks] of
        [] ->
            true;
        Broken ->
            put(broken_law, Broken),
            false
    end.

next_state(S, _Res, {call, ?W, restart_ctl, []}) ->
    S#{seen := false, sent := [], resendable := []};
next_state(S, #{} = Res, _Call) ->
    #{emitted := Emitted, accepted := Accepted, known := Known, gone := Gone, resendable := D} =
        Res,
    #{seen := Seen, gone := WasGone, sent := Sent, resendable := Late, accepted := Counts} = S,
    S#{
        resendable := Late ++ D,
        seen := Seen orelse Known,
        gone := lists:usort(WasGone ++ Gone),
        sent := Sent ++ Emitted,
        accepted := lists:foldl(fun(P, M) -> M#{P => 1} end, Counts, Accepted)
    };
next_state(S, _Symbolic, _Call) ->
    %% Generation runs on symbolic results, which carry nothing to learn from.
    S.

%% --- helpers -------------------------------------------------------------------

%% What the run reached, so a pass over trivial histories shows up as such.
classes(#{accepted := Accepted, gone := Gone, resendable := Resendable}) ->
    [
        case map_size(Accepted) of
            0 -> no_accepted_restart;
            _ -> accepted_restart
        end,
        case Gone of
            [] -> no_driver_gone;
            _ -> driver_gone
        end,
        case Resendable of
            [] -> no_resend_case;
            _ -> resend_case
        end
    ].

driver() ->
    elements([0, 1, 2, 3]).

unique(L) ->
    length(L) =:= length(lists:usort(L)).
