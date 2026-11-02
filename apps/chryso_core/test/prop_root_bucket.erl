-module(prop_root_bucket).
-moduledoc """
Laws of `chryso_root_bucket`. Observation: `take/3` results and `tokens/1`
over generated clock-sample sequences.
""".
-include_lib("proper/include/proper.hrl").

-export([prop_bounded/0, prop_off_absorbs/0, prop_rate/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

%% Tokens never exceed the capacity, whatever the samples.
prop_bounded() ->
    ?FORALL(
        {Cap, Refill, Samples},
        {integer(0, 8), integer(0, 800), list(sample())},
        begin
            {_, Held} = run(chryso_root_bucket:new(Cap, Refill, 1000, true), 1000, Samples),
            lists:all(fun(T) -> T =< Cap end, Held)
        end
    ).

%% After an invalid or backwards sample the bucket never refills: takes
%% succeed at most as many times as tokens were held at that point.
prop_off_absorbs() ->
    ?FORALL(
        {Cap, Before, After},
        {integer(1, 8), list(sample()), list(forward())},
        begin
            {_, _, B0} = steps(chryso_root_bucket:new(Cap, 400, 1000, true), 1000, Before),
            {_, B1} = chryso_root_bucket:take(0, false, B0),
            Held = chryso_root_bucket:tokens(B1),
            {Results, _} = run(B1, 1000, After),
            length([ok || ok <- Results]) =< Held
        end
    ).

%% While the clock runs forward, successes over an interval of E ticks are
%% at most capacity + E div interval.
prop_rate() ->
    ?FORALL(
        {Cap, Refill, Steps},
        {integer(1, 8), integer(1, 800), list(forward())},
        begin
            Start = 1000,
            {Results, End, _} = steps(
                chryso_root_bucket:new(Cap, Refill, Start, true), Start, Steps
            ),
            Interval = Refill div Cap + min(1, Refill rem Cap),
            Ok = length([ok || ok <- Results]),
            collect(Ok > Cap, Ok =< Cap + (End - Start) div Interval)
        end
    ).

%% --- helpers and generators ---------------------------------------------------

%% Results and tokens held after each sample.
run(B, Now, Samples) ->
    {Pairs, _, _} = lists:foldl(
        fun(S, {Acc, N, Bk}) ->
            {N1, Valid} = advance(N, S),
            {R, Bk1} = chryso_root_bucket:take(N1, Valid, Bk),
            {[{R, chryso_root_bucket:tokens(Bk1)} | Acc], N1, Bk1}
        end,
        {[], Now, B},
        Samples
    ),
    lists:unzip(lists:reverse(Pairs)).

%% Results, the final clock and the final bucket.
steps(B, Now, Samples) ->
    {Results, End, Last} = lists:foldl(
        fun(S, {Rs, N, Bk}) ->
            {N1, Valid} = advance(N, S),
            {R, Bk1} = chryso_root_bucket:take(N1, Valid, Bk),
            {[R | Rs], N1, Bk1}
        end,
        {[], Now, B},
        Samples
    ),
    {lists:reverse(Results), End, Last}.

advance(N, {forward, D}) -> {N + D, true};
advance(N, {backward, D}) -> {max(0, N - D), true};
advance(N, invalid) -> {N, false}.

forward() ->
    {forward, frequency([{3, 0}, {5, integer(1, 200)}, {1, integer(200, 2000)}])}.

sample() ->
    frequency([{8, forward()}, {1, {backward, integer(1, 500)}}, {1, invalid}]).
