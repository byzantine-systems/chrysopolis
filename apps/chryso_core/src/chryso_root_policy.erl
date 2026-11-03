-module(chryso_root_policy).
-moduledoc """
Root's per-child fault policy as a Mealy machine: `step/3` takes a child's
record and the budget that applies, and returns the action and the next
record. The same decision as `root_restart_plan` with the lifetime selector
(`src/pd/root/restart_plan.c`). Production Root has no timed window, so the
test-only `ROOT_TEST_BUDGET` window is not modelled.

Laws, observed through the action and the record:

- **absorption**: a `gone` record yields `ignore_gone` and stays unchanged.
- **monotone**: the lifetime count never decreases, and grows only on
  `resume`.
- **identity**: with no spec, `budget/3` is the compiled budget.
- **cap**: with a spec, the budget is `min(spec, hard_max)`. A spec entry of
  0 means the compiled budget.

Non-law: a spec may *raise* a budget, up to `hard_max`.
""".

-export([new/0, budget/3, step/3]).
-export_type([record/0, action/0]).

-doc "One child's restart record.".
-type record() :: #{state := live | gone, lifetime := non_neg_integer()}.

-doc "What Root does with a fault or an accepted restart request.".
-type action() ::
    resume
    | giveup_budget
    | giveup_no_entry
    | ignore_gone.

-doc "A live child that has never been restarted.".
-spec new() -> record().
new() ->
    #{state => live, lifetime => 0}.

-doc """
The lifetime ceiling for one child: `Compiled` when no spec applies or the
spec's entry is 0, otherwise the spec entry capped at `HardMax`.
""".
-spec budget(none | non_neg_integer(), non_neg_integer(), pos_integer()) -> non_neg_integer().
budget(none, Compiled, _HardMax) ->
    Compiled;
budget(0, Compiled, _HardMax) ->
    Compiled;
budget(SpecBudget, _Compiled, HardMax) ->
    min(SpecBudget, HardMax).

-doc """
Decide one restart request. Checks run in Root's order: gone, then the
budget, then the entry. `Entry` is the restart entry address. 0 means the
image carries none.
""".
-spec step(record(), non_neg_integer(), non_neg_integer()) -> {action(), record()}.
step(#{state := gone} = Record, _Budget, _Entry) ->
    {ignore_gone, Record};
step(#{lifetime := Lifetime} = Record, Budget, _Entry) when Lifetime >= Budget ->
    {giveup_budget, Record#{state := gone}};
step(Record, _Budget, 0) ->
    {giveup_no_entry, Record#{state := gone}};
step(#{lifetime := Lifetime} = Record, _Budget, _Entry) ->
    {resume, Record#{lifetime := Lifetime + 1}}.
