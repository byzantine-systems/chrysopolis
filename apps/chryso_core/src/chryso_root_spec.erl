-module(chryso_root_spec).
-moduledoc """
Which committed spec Root uses: the semantic half of `root_spec_consume`
(`src/pd/root/spec.c`). The page arrives as the generated codec classifies
it (`t:page/0`), so byte checks stay in the codec and this module holds only
the retention rule.

The active bank wins whenever it can be used. The other bank is read only
when the active one is empty or rejected. A bank is used when it is newer
than the retained generation, or equal with identical content.

Laws, observed through the retained policy:

- **rejection identity**: an outcome other than `applied` leaves the
  retained policy unchanged.
- **monotone**: the retained generation never decreases (an older bank is
  `regressed`).
- **idempotence**: consuming the same page twice applies at most once. The
  second result is never `applied`.
- **no merge**: an equal generation with different content is `conflict`
  and is never applied.

A rejection is reported (`record => true`) once per (reason, generation,
bank), as Root logs it.
""".

-export([new/0, consume/2, budget/4, retained/1]).
-export_type([candidate/0, page/0, reason/0, outcome/0, policy/0, state/0, result/0]).

-doc "Why a header or bank was not used.".
-type reason() ::
    unset
    | header
    | bank_torn
    | bank_checksum
    | bank_malformed
    | undeclared_child
    | regressed
    | conflict
    | unstable_copy.

-doc """
One bank as the codec sees it: never sealed, rejected by the checker, or a
sealed generation with its per-child budgets and desired states.
""".
-type candidate() ::
    empty
    | {rejected, reason(), non_neg_integer()}
    | {bank, pos_integer(), [non_neg_integer()], [non_neg_integer()]}.

-doc "The page: unmapped or zero, a bad header, or the active bank and the other one.".
-type page() :: absent | header | {banks, 0 | 1, candidate(), candidate()}.

-doc "What a refresh did.".
-type outcome() :: absent | applied | unchanged | kept.

-doc "A policy Root applied. Generation 0 is the compiled-in policy.".
-type policy() :: #{
    generation := non_neg_integer(),
    bank := 0 | 1,
    budgets := [non_neg_integer()],
    desired := [non_neg_integer()]
}.

-doc "The retained policy and the last reported rejection.".
-opaque state() :: #{
    retained := policy(),
    last := {reason(), non_neg_integer(), 0 | 1}
}.

-doc "One refresh's result, as Root publishes and logs it.".
-type result() :: #{outcome := outcome(), reason := reason(), record := boolean()}.

-type kind() :: empty | rejected | same | newer | skipped.

-doc "The compiled-in policy, nothing reported yet.".
-spec new() -> state().
new() ->
    #{
        retained => #{generation => 0, bank => 0, budgets => [], desired => []},
        last => {unset, 0, 0}
    }.

-doc "The policy currently retained.".
-spec retained(state()) -> policy().
retained(#{retained := Retained}) ->
    Retained.

-doc "One refresh of the page.".
-spec consume(page(), state()) -> {result(), state()}.
consume(absent, State) ->
    finish(absent, unset, 0, 0, State);
consume(header, State) ->
    finish(kept, header, 0, 0, State);
consume({banks, Active, ActiveBank, OtherBank}, #{retained := Retained} = State) ->
    {FirstKind, FirstReason, FirstGen, FirstPolicy} = classify(ActiveBank, Active, Retained),
    {SecondKind, _, _, SecondPolicy} =
        case FirstKind of
            K when K =:= empty; K =:= rejected -> classify(OtherBank, 1 - Active, Retained);
            _ -> {skipped, unset, 0, none}
        end,
    {Outcome, Choice} = select(FirstKind, SecondKind),
    State1 =
        case {Outcome, Choice} of
            {applied, active} -> State#{retained := FirstPolicy};
            {applied, other} -> State#{retained := SecondPolicy};
            _ -> State
        end,
    %% Only the active bank's rejection is reported. A fallback bank
    %% mid-write is the normal state of a commit in progress.
    {Reason, Gen} =
        case FirstKind of
            rejected -> {FirstReason, FirstGen};
            _ -> {unset, 0}
        end,
    finish(Outcome, Reason, Gen, Active, State1).

-doc "The lifetime ceiling the retained policy gives `Child`.".
-spec budget(state(), non_neg_integer(), non_neg_integer(), pos_integer()) -> non_neg_integer().
budget(#{retained := #{generation := 0}}, _Child, Compiled, HardMax) ->
    chryso_root_policy:budget(none, Compiled, HardMax);
budget(#{retained := #{budgets := Budgets}}, Child, Compiled, HardMax) ->
    Entry =
        case Child < length(Budgets) of
            true -> lists:nth(Child + 1, Budgets);
            false -> 0
        end,
    chryso_root_policy:budget(Entry, Compiled, HardMax).

-spec classify(candidate(), 0 | 1, policy()) ->
    {kind(), reason(), non_neg_integer(), policy() | none}.
classify(empty, _Bank, _Retained) ->
    {empty, unset, 0, none};
classify({rejected, Reason, Gen}, _Bank, _Retained) ->
    {rejected, Reason, Gen, none};
classify({bank, Gen, _, _}, _Bank, #{generation := RetGen}) when Gen < RetGen ->
    {rejected, regressed, Gen, none};
classify({bank, Gen, Budgets, Desired}, Bank, #{generation := RetGen} = Retained) ->
    Policy = #{generation => Gen, bank => Bank, budgets => Budgets, desired => Desired},
    case Gen =:= RetGen of
        false ->
            {newer, unset, Gen, Policy};
        true ->
            case same_content(Policy, Retained) of
                true -> {same, unset, Gen, Policy};
                false -> {rejected, conflict, Gen, none}
            end
    end.

-spec same_content(policy(), policy()) -> boolean().
same_content(#{budgets := B, desired := D}, #{budgets := B, desired := D}) -> true;
same_content(_, _) -> false.

%% Root's selections table, cell for cell. The active bank is always read,
%% so the C table's "active skipped" row has no counterpart here.
-spec select(empty | rejected | same | newer, kind()) -> {outcome(), none | active | other}.
select(newer, _) -> {applied, active};
select(same, _) -> {unchanged, active};
select(empty, same) -> {unchanged, other};
select(empty, newer) -> {applied, other};
select(empty, _) -> {absent, none};
select(rejected, same) -> {unchanged, other};
select(rejected, newer) -> {applied, other};
select(rejected, _) -> {kept, none}.

-spec finish(outcome(), reason(), non_neg_integer(), 0 | 1, state()) -> {result(), state()}.
finish(Outcome, unset, _Gen, _Bank, State) ->
    {#{outcome => Outcome, reason => unset, record => false}, State#{last := {unset, 0, 0}}};
finish(Outcome, Reason, Gen, Bank, #{last := Last} = State) ->
    Key = {Reason, Gen, Bank},
    {#{outcome => Outcome, reason => Reason, record => Key =/= Last}, State#{last := Key}}.
