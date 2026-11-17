-module(chryso_plan).
-moduledoc """
Commands as values. The slice has one: a driver restart fenced by the planning
page (`root_generation`, `seq`, the child's `lifetime`).

The spec is a restart stamp per driver: `want(C) = T` holds when the row's
`last_restart >= T`, so whether it holds is read off one page, like
Kubernetes' `restartedAt`.

Laws, through `commands/1`:

- **monoid**: `then` is associative with identity `none()`.
- **unknown plans nothing**, and no command addresses a child that is not live.
- **fixpoint**: when every want holds, `next` is `none()`.
- **bounded**: `next` plans at most one command.

Non-law: a fault restart after `T` also satisfies the want.
""".

-export([none/0, restart/2, then/2, commands/1, next/3]).
-export_type([plan/0, command/0, fence/0, wants/0]).

-doc "The words a restart carries besides the child.".
-type fence() :: #{
    root_generation := non_neg_integer(),
    seq := non_neg_integer(),
    lifetime := non_neg_integer()
}.

-doc "One command.".
-type command() :: {restart, Child :: non_neg_integer(), fence()}.

-doc "Per driver, the Root tick it must have restarted at or after.".
-type wants() :: #{non_neg_integer() => non_neg_integer()}.

-doc "A sequence of commands.".
-opaque plan() :: [command()].

-doc "The empty plan.".
-spec none() -> plan().
none() ->
    [].

-doc "A plan of one restart.".
-spec restart(Child :: non_neg_integer(), Fence :: fence()) -> plan().
restart(Child, Fence) ->
    [{restart, Child, Fence}].

-doc "`First`, then `Second`.".
-spec then(First :: plan(), Second :: plan()) -> plan().
then(First, Second) ->
    First ++ Second.

-doc "The commands, in order.".
-spec commands(Plan :: plan()) -> [command()].
commands(Plan) ->
    Plan.

-doc """
At most one restart: the lowest-numbered child whose want does not hold, that
is live on the page, and that `Blocked` does not name. `Blocked` is the
lifecycle's to fill: children it awaits a newer page for, holds back, or gave
up on.
""".
-spec next(
    Knowledge :: chryso_observe:knowledge(),
    Wants :: wants(),
    Blocked :: sets:set(non_neg_integer())
) -> plan().
next(Knowledge, Wants, Blocked) ->
    case chryso_observe:stamp(Knowledge) of
        unknown ->
            none();
        {Generation, Seq} ->
            Due = [
                {C, Row}
             || {C, T} <- lists:sort(maps:to_list(Wants)),
                not sets:is_element(C, Blocked),
                #{state := live, last_restart := Last} = Row <- [
                    chryso_observe:child(Knowledge, C)
                ],
                Last < T
            ],
            case Due of
                [] ->
                    none();
                [{C, #{lifetime := L}} | _] ->
                    restart(C, #{root_generation => Generation, seq => Seq, lifetime => L})
            end
    end.
