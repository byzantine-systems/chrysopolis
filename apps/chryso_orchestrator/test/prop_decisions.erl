-module(prop_decisions).
-moduledoc """
Laws of `chryso_decisions`: a log of what `chryso_lifecycle:step/2` returned
replays to `match` at any bound, wrapped or not, and a log that differs from
what `step` returned is caught at the first entry that differs.

| Planted defect | Caught by |
|---|---|
| `Base` not moved past a dropped entry | prop_faithful_log_replays |
| `replay` comparing effects only, not the state after | prop_doctored_log_is_caught (a doctored state) |
""".
-include_lib("proper/include/proper.hrl").

-export([prop_faithful_log_replays/0, prop_doctored_log_is_caught/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_faithful_log_replays() ->
    ?FORALL(
        {Limit, Inputs},
        {integer(1, 8), inputs()},
        begin
            Ring = run(Limit, Inputs, fun(Effects, After) -> {Effects, After} end),
            Wrapped = chryso_decisions:dropped(Ring) > 0,
            aggregate([{wrapped, Wrapped}], chryso_decisions:replay(Ring) =:= match)
        end
    ).

%% One entry's effects or state is doctored. The replay names that entry.
prop_doctored_log_is_caught() ->
    ?FORALL(
        {Inputs, Pick, What},
        {non_empty(inputs()), nat(), elements([effects, state])},
        begin
            N = length(Inputs),
            At = Pick rem N + 1,
            Counter = counters:new(1, []),
            Ring = run(N, Inputs, fun(Effects, After) ->
                ok = counters:add(Counter, 1, 1),
                case {counters:get(Counter, 1) =:= At, What} of
                    {true, effects} ->
                        {[{arm, 999999, 0} | Effects], After};
                    {true, state} ->
                        {Effects, element(2, chryso_lifecycle:step({tick, 1 bsl 40}, After))};
                    {false, _} ->
                        {Effects, After}
                end
            end),
            aggregate([What], chryso_decisions:replay(Ring) =:= {mismatch, At})
        end
    ).

%% Step each input from new/1 and log what Doctor makes of the result. The
%% real state goes on, so only the logged entry differs.
run(Limit, Inputs, Doctor) ->
    S0 = chryso_lifecycle:new(lifecycle_world:params()),
    {_, Ring} = lists:foldl(
        fun(Input, {S, R}) ->
            {Effects, S1} = chryso_lifecycle:step(Input, S),
            {Logged, After} = Doctor(Effects, S1),
            {S1, chryso_decisions:log(Input, Logged, After, R)}
        end,
        {S0, chryso_decisions:new(Limit, S0)},
        Inputs
    ),
    Ring.

%% Inputs from prop_observe's pages, so the planning path runs.
inputs() ->
    list(
        frequency([
            {3, ?LET(P, prop_observe:page(), {observed, prop_observe:known(P)})},
            {2, ?LET({C, T}, {integer(0, 3), integer(0, 6000)}, {want, C, T})},
            {2, ?LET(N, integer(0, 6000), {tick, N})},
            {1,
                ?LET(
                    {R, Res},
                    {integer(1, 4), elements([ok, stale, rate_limited, refused])},
                    {reply, R, Res}
                )},
            {1, ?LET(R, integer(1, 4), {deadline, R})}
        ])
    ).
