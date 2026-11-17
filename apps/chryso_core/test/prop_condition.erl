-module(prop_condition).
-moduledoc """
Laws of `chryso_condition`.

| Planted defect | Caught by |
|---|---|
| a gone row reported `restarting` while a want is open | prop_gone_dominates |
| `low_budget` at `budget - lifetime < floor` (off by one) | prop_floor_is_exact |
""".
-include_lib("proper/include/proper.hrl").

-export([
    prop_unknown_in_unknown_out/0,
    prop_gone_dominates/0,
    prop_floor_is_exact/0,
    prop_recent_restart_is_recovering/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_unknown_in_unknown_out() ->
    ?FORALL(C, context(), chryso_condition:classify(unknown, C) =:= {unknown, no_observation}).

prop_gone_dominates() ->
    ?FORALL(
        {Row, C},
        {row(), context()},
        chryso_condition:classify(Row#{state := gone}, C) =:= {gone, gave_up}
    ).

%% A live, settled, unrequested child is degraded for its budget exactly when
%% what is left is at or below the floor.
prop_floor_is_exact() ->
    ?FORALL(
        {Row, C},
        {row(), context()},
        begin
            %% Settled: the last restart is older than any settle window (500 ms).
            Settled = Row#{state := live, last_restart := 0},
            Quiet = C#{requested := false, held := false, now := 1000 + maps:get(now, C)},
            #{lifetime := L, budget := B} = Settled,
            #{floor := Floor} = Quiet,
            Got = chryso_condition:classify(Settled, Quiet),
            Want =
                case B - L =< Floor of
                    true -> {degraded, low_budget};
                    false -> {ready, settled}
                end,
            aggregate([element(1, Want)], Got =:= Want)
        end
    ).

%% A restart within the settle time is recovering. Ticks are milliseconds here
%% (cntfrq 1000), so the window is plain arithmetic.
prop_recent_restart_is_recovering() ->
    ?FORALL(
        {L, Since, Settle},
        {integer(1, 4), integer(0, 400), integer(1, 400)},
        begin
            Row = #{state => live, lifetime => L, budget => 9, last_restart => 1000},
            C = #{
                now => 1000 + Since,
                cntfrq => 1000,
                settle_ms => Settle,
                floor => 0,
                requested => false,
                held => false
            },
            Want =
                case Since < Settle of
                    true -> {recovering, restarted};
                    false -> {ready, settled}
                end,
            aggregate([element(1, Want)], chryso_condition:classify(Row, C) =:= Want)
        end
    ).

%% --- generators ---------------------------------------------------------------

row() ->
    ?LET(
        {State, L, B, T},
        {
            elements([live, gone, stopped, quarantined, unset]),
            integer(0, 4),
            integer(1, 5),
            integer(0, 5000)
        },
        #{state => State, lifetime => L, budget => B, last_restart => T}
    ).

context() ->
    ?LET(
        {Now, Settle, Floor, Requested, Held},
        {integer(0, 6000), integer(0, 500), integer(0, 2), boolean(), boolean()},
        #{
            now => Now,
            cntfrq => 1000,
            settle_ms => Settle,
            floor => Floor,
            requested => Requested,
            held => Held
        }
    ).
