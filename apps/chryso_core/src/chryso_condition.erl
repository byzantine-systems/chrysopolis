-module(chryso_condition).
-moduledoc """
A driver's condition and reason, like a Kubernetes status condition, from one
child row and the lifecycle's context.

| Condition | When |
|---|---|
| `unknown` | nothing known |
| `gone` | Root gave up on it |
| `restarting` | a want does not hold yet (`restart_requested`), or Root rate-limits it (`rate_limited`) |
| `recovering` | restarted less than `settle_ms` ago |
| `degraded` | `budget - lifetime =< floor` (`low_budget`), or stopped or quarantined |
| `ready` | otherwise |

Laws: unknown in, unknown out. A gone row is `gone`. The result depends on
the row and context only.

Non-law: `recovering` is a time window, not proof the driver re-initialised.
""".

-export([classify/2]).
-export_type([condition/0, reason/0, context/0]).

-doc "The closed set of conditions.".
-type condition() :: unknown | gone | restarting | recovering | degraded | ready.

-doc "Why a child has its condition.".
-type reason() ::
    no_observation
    | gave_up
    | restart_requested
    | rate_limited
    | restarted
    | low_budget
    | stopped
    | quarantined
    | unset
    | settled.

-doc """
What the lifecycle knows besides the row. `now` and `cntfrq` come from the
page, `settle_ms` and `floor` from the parameters. `requested` says a want of
the child does not hold yet, and `held` that Root's rate limit holds it back.
""".
-type context() :: #{
    now := non_neg_integer(),
    cntfrq := non_neg_integer(),
    settle_ms := non_neg_integer(),
    floor := non_neg_integer(),
    requested := boolean(),
    held := boolean()
}.

-doc "The condition and its reason for one child.".
-spec classify(Row :: unknown | chryso_observe:child_row(), Context :: context()) ->
    {condition(), reason()}.
classify(unknown, _Context) ->
    {unknown, no_observation};
classify(#{state := gone}, _Context) ->
    {gone, gave_up};
classify(#{state := live}, #{requested := true, held := true}) ->
    {restarting, rate_limited};
classify(#{state := live}, #{requested := true}) ->
    {restarting, restart_requested};
classify(#{state := live} = Row, Context) ->
    case recent(Row, Context) of
        true -> {recovering, restarted};
        false -> budget(Row, Context)
    end;
classify(#{state := State}, _Context) ->
    {degraded, State}.

%% Restarted less than settle_ms ago, in Root ticks. A zero cntfrq is a page
%% no reader would accept, so it never counts as recent.
-spec recent(chryso_observe:child_row(), context()) -> boolean().
recent(#{lifetime := 0}, _Context) ->
    false;
recent(_Row, #{cntfrq := 0}) ->
    false;
recent(#{last_restart := Then}, #{now := Now, cntfrq := F, settle_ms := Settle}) ->
    Now >= Then andalso (Now - Then) * 1000 < Settle * F.

-spec budget(chryso_observe:child_row(), context()) -> {condition(), reason()}.
budget(#{lifetime := L, budget := B}, #{floor := Floor}) when B - L =< Floor ->
    {degraded, low_budget};
budget(_Row, _Context) ->
    {ready, settled}.
