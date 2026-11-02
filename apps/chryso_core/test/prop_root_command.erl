-module(prop_root_command).
-moduledoc """
Laws of `chryso_root_command`. Observation: the decision (result, action,
values) and the tallies. Views are generated around one driver so the
restart path is reached often.
""".
-include_lib("proper/include/proper.hrl").

-export([
    prop_fenced_idempotence/0,
    prop_stale_has_no_effect/0,
    prop_only_accepted_restarts_act/0,
    prop_tallies_add_up/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(PP, 11).
-define(RESTART, 2).
-define(PRESENT, 16#2f).
-define(DRIVERS, 16#0f).

%% After complete/2, the same opcode and words get already_done with the
%% stored values and no second action.
prop_fenced_idempotence() ->
    ?FORALL(
        {Child, View, Values},
        {integer(0, 3), view(live), vector(4, integer(0, 99))},
        begin
            Args = fresh_args(Child, View),
            S0 = chryso_root_command:new(bucket()),
            {D1, S1} = chryso_root_command:decide(S0, View, ?RESTART, Args, 1000, true),
            #{result := ok, action := {restart, Child}} = D1,
            S2 = chryso_root_command:complete(S1, D1#{values := Values}),
            {D2, _} = chryso_root_command:decide(S2, View, ?RESTART, Args, 1000, true),
            maps:with([result, action, values], D2) =:=
                #{result => already_done, action => none, values => Values}
        end
    ).

%% Any precondition word that differs from Root's view is stale, with no
%% action, and the reply carries Root's current words.
prop_stale_has_no_effect() ->
    ?FORALL(
        {Child, View, Word, Delta},
        {integer(0, 3), view(live), integer(1, 3), integer(1, 5)},
        begin
            Args0 = fresh_args(Child, View),
            Args =
                lists:sublist(Args0, Word) ++
                    [lists:nth(Word + 1, Args0) + Delta] ++ lists:nthtail(Word + 1, Args0),
            {D, _} = chryso_root_command:decide(
                chryso_root_command:new(bucket()), View, ?RESTART, Args, 1000, true
            ),
            #{generation := G, seq := Seq, records := Records} = View,
            #{lifetime := L} = maps:get(Child, Records),
            maps:with([result, action, values], D) =:=
                #{result => stale, action => none, values => [G, Seq, L, 0]}
        end
    ).

%% Only an accepted restart_child carries an action.
prop_only_accepted_restarts_act() ->
    ?FORALL(
        {View, Op, Args},
        {view(any), opcode(), args()},
        begin
            {D, _} = chryso_root_command:decide(
                chryso_root_command:new(bucket()), View, Op, Args, 1000, true
            ),
            #{action := Action, result := Result} = D,
            collect(Result, Action =:= none orelse (Op =:= ?RESTART andalso Result =:= ok))
        end
    ).

%% Every call adds one to exactly one tally, through frame and decide.
prop_tallies_add_up() ->
    ?FORALL(
        {View, Calls},
        {view(any), list({opcode(), oneof([3, 4, 5]), args(), integer(0, 300)})},
        begin
            {_, S} = lists:foldl(
                fun({Op, Count, Args, Dt}, {Now, St}) ->
                    Now1 = Now + Dt,
                    case chryso_root_command:frame(St, ?PP, ?PP, Op, Count) of
                        {ok, St1} ->
                            {_, St2} = chryso_root_command:decide(St1, View, Op, Args, Now1, true),
                            {Now1, St2};
                        {_, St1} ->
                            {Now1, St1}
                    end
                end,
                {1000, chryso_root_command:new(bucket())},
                Calls
            ),
            {A, L, R} = chryso_root_command:tallies(S),
            A + L + R =:= length(Calls)
        end
    ).

%% --- generators -----------------------------------------------------------------

bucket() ->
    chryso_root_bucket:new(4, 400, 1000, true).

fresh_args(Child, #{generation := G, seq := Seq, records := Records}) ->
    #{lifetime := L} = maps:get(Child, Records),
    [Child, G, Seq, L].

view(Liveness) ->
    ?LET(
        {Seq, Lifetimes, States},
        {integer(2, 40), vector(6, integer(0, 5)), vector(6, state(Liveness))},
        #{
            abi => 1,
            generation => 1,
            incarnation => 1,
            cntfrq => 62500000,
            seq => Seq,
            publishing => true,
            present => ?PRESENT,
            drivers => ?DRIVERS,
            records => maps:from_list([
                {C, #{state => S, lifetime => L}}
             || {C, S, L} <- lists:zip3(lists:seq(0, 5), States, Lifetimes)
            ])
        }
    ).

state(live) -> live;
state(any) -> frequency([{4, live}, {1, gone}]).

opcode() ->
    frequency([{4, ?RESTART}, {2, 1}, {2, 7}, {1, integer(3, 6)}, {1, integer(8, 40)}]).

args() ->
    ?LET(
        {C, G, S, L},
        {integer(0, 7), integer(0, 2), integer(0, 40), integer(0, 5)},
        frequency([{3, [C, G, S, L]}, {1, [1, 0, 0, 0]}, {1, [0, 0, 0, 0]}])
    ).
