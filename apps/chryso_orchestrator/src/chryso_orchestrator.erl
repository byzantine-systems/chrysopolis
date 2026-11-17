-module(chryso_orchestrator).
-moduledoc """
The orchestrator's API. Every call has a finite timeout and exits the caller
when the controller does not answer in time.
""".

-export([status/0, want/1, want/2, wanted/0, decisions/0, replay/0, scan/0]).

-define(TIMEOUT, 5000).

-doc """
Each watched driver's `{Condition, Reason}`, and the page they come from
(`root_generation`, `beam_incarnation`, `seq`), or `unknown`.
""".
-spec status() -> map().
status() ->
    chryso_lifecycle_ctl:status(?TIMEOUT).

-doc "Want `Child` restarted after the newest page known. Returns the Root tick used.".
-spec want(Child :: non_neg_integer()) ->
    {ok, non_neg_integer()} | {error, no_observation | not_watched}.
want(Child) ->
    chryso_lifecycle_ctl:want(Child, next, ?TIMEOUT).

-doc "Want `Child` restarted at or after Root tick `T`. Repeating a want is a no-op.".
-spec want(Child :: non_neg_integer(), T :: non_neg_integer()) ->
    {ok, non_neg_integer()} | {error, no_observation | not_watched}.
want(Child, T) ->
    chryso_lifecycle_ctl:want(Child, T, ?TIMEOUT).

-doc "Per driver, the newest Root tick wanted. Applying one again with `want/2` is a no-op.".
-spec wanted() -> chryso_plan:wants().
wanted() ->
    chryso_lifecycle_ctl:wanted(?TIMEOUT).

-doc "The decision log, oldest first, and how many entries the bound dropped.".
-spec decisions() -> {[chryso_decisions:entry()], non_neg_integer()}.
decisions() ->
    chryso_lifecycle_ctl:decisions(?TIMEOUT).

-doc "`match` when the decision log replays through the pure step.".
-spec replay() -> match | {mismatch, pos_integer()}.
replay() ->
    chryso_lifecycle_ctl:replay(?TIMEOUT).

-doc "Read Root now.".
-spec scan() -> ok.
scan() ->
    chryso_lifecycle_ctl:scan(?TIMEOUT).
