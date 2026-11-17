"""Fault programs and the model that judges them.

Runs are simulated by drawing outcomes the model allows. Laws build the
state they need instead of waiting for it. Each caught a planted defect:

| Planted defect | Caught by |
|---|---|
| a latent read error on present data allows a clean read | test_a_latent_read_error_... |
| Disarm leaves the fault armed | test_disarm_forgets_the_fault |
| a write fault lets a get fail | test_write_faults_never_change_what_a_get_returns |
| a read fault on data lets a put fail | test_a_put_never_reads_file_data |
| a rewrite leaves a corrupt file corrupt | test_rewriting_corrupted_data_heals_it |
| Root restarts once past the budget | test_a_driver_is_restarted_budget_times_... |
| a ping leaves FatFs's window dirty | test_steps_of_different_components_commute |
| a reset keeps Root's budgets or the armed fault | test_a_reset_forgets_root_and_keeps_the_disk |
| a crashed write leaves its file held | test_a_crashed_write_changes_only_its_file |
| a crashed rewrite may read as anything | test_a_crashed_write_changes_only_its_file |
| a reset is offered on a damaged volume | test_steer.test_no_reset_is_offered_on_a_damaged_volume |
| the rename scenario aims at a held target | test_steer.test_the_rename_scenario_moves_... |
| a rename leaves its source held | test_a_renamed_file_reads_under_its_new_name |
| a rename onto a present name replaces it | test_a_rename_onto_a_present_name_is_refused_... |
| a rename leaves a data fault on the old name | test_a_read_error_follows_its_file_through_... |
| a rename drops every other file | test_changes_to_distinct_files_commute |
| a remove keeps the fault on the freed data | test_a_removed_file_reads_as_absent |
| an overwrite expects a get's read errors | test_an_overwrite_never_reads_file_data |
| only a get may fail on a dirty window | test_a_dirty_window_may_fail_any_volume_step |
| a want leaves Root's budget unspent | test_a_want_is_a_restart_through_the_orchestrator |
| a rewant allows a second restart | test_applying_a_want_again_changes_nothing |
| a look marks its driver wanted | test_a_look_changes_nothing |
| a look reads what is wanted | test_a_look_reads_root_alone |
| a gone driver looks ready once wanted | test_a_gone_driver_looks_gone_until_a_reset |
| the budget floor off by one | test_a_driver_looks_degraded_exactly_when_its_budget_is_spent |
| a want without a status page commands | test_without_a_status_page_nothing_is_known_or_commanded |
| a want of blk dirties FatFs's window | test_a_want_commutes_with_what_a_restart_commutes_with |
| a reset keeps the wants | test_a_reset_forgets_every_want |

The pre-split model ran in lockstep with this one before it was deleted.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, replace
from typing import Final, Literal, get_args

import pytest
from hypothesis import event, find, given, settings
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.allowed import Allowed
from chryso_testbed.blkdebug import PLAN, Expected
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.fatlayout import Region
from chryso_testbed.faults import (
    CLEAN_DISK,
    DRIVERS,
    FILES,
    KEPT,
    PROGRAM,
    STEP,
    Arm,
    Corrupt,
    CrashPut,
    Disarm,
    Driver,
    Fault,
    Get,
    Look,
    Model,
    Overwrite,
    Program,
    Put,
    Remove,
    Rename,
    Reset,
    Restart,
    Rewant,
    Step,
    Want,
    expect,
    owner,
    trim,
)
from chryso_testbed.inject import (
    condition_outcome,
    decision,
    get_outcome,
    put_outcome,
    want_outcome,
)
from chryso_testbed.lifecycles import FaultLifecycle, FaultRecord
from chryso_testbed.outcomes import (
    Condition,
    ConditionName,
    GaveUp,
    Outcome,
    Reason,
    Restarted,
    Seed,
    Status,
    render,
)

BUDGET: Final = 3
# Outcomes for steps whose model admits anything.
UNIVERSE: Final[tuple[Outcome, ...]] = (
    Status.OK, Status.EIO, Status.ERROR, Status.ENOENT, Status.DIFFERS,
    Seed(0), Seed(255), Status.PRESENT, Status.ABSENT,
)  # fmt: skip

steps = from_schema(STEP.json_schema()).map(STEP.validate_python)
programs = st.lists(steps, max_size=12).map(tuple)
disks = from_schema(PLAN_SCHEMA).map(PLAN.validate_python).map(Expected.of)


@dataclass(frozen=True, slots=True)
class Seen:
    """One step of a simulated run: what the model allowed, what happened,
    and the model before it."""

    step: Step
    allowed: Allowed[Outcome]
    outcome: Outcome
    before: Model


def outcome(allowed: Allowed[Outcome]) -> st.SearchStrategy[Outcome]:
    choices = UNIVERSE if allowed.anything else sorted(allowed.values, key=render)
    return st.sampled_from(choices)


@st.composite
def runs(draw: st.DrawFn, *, observable: bool = True) -> list[Seen]:
    """A program, and for each step what was allowed, what happened and the
    model before it. `observable` is false for an image with no status page."""
    model = Model.start(BUDGET, draw(disks), observable=observable)
    trace: list[Seen] = []
    for step in draw(programs):
        allowed = model.expect(step)
        seen = draw(outcome(allowed))
        trace.append(Seen(step, allowed, seen, model))
        model = model.after(step, seen)
    return trace


@st.composite
def states(draw: st.DrawFn) -> Model:
    """A model the fuzz can reach: the state after a simulated run."""
    trace = draw(runs())
    if not trace:
        return Model.start(BUDGET)
    last = trace[-1]
    return last.before.after(last.step, last.outcome)


def with_fault(
    model: Model, op: Literal["read", "write"], region: Region, file: int, once: bool
) -> Model:
    fault = FaultLifecycle(FaultRecord()).evolved(
        "aimed", op=op, region=region, file=file, once=once
    )
    return replace(model, storage=replace(model.storage, fault=fault))


def holding(model: Model, file: int, seed: int) -> Model | None:
    """`model` after a successful write of `file`, or None where a write
    cannot be reported ok (the disk is gone or the volume damaged)."""
    put = Put(kind="put", file=file, seed=seed)
    if Status.OK not in model.expect(put) or model.storage.damaged:
        return None
    return model.after(put, Status.OK)


def without_fault(model: Model) -> Model:
    return replace(model, storage=replace(model.storage, fault=FaultLifecycle(FaultRecord())))


# --- programs ----------------------------------------------------------------------


@given(program=programs)
def test_a_program_round_trips(program: Program) -> None:
    assert PROGRAM.validate_python(PROGRAM.dump_python(program)) == program


@given(program=programs, budget=st.integers(0, 4))
def test_trim_keeps_serial_and_timer_within_budget(program: Program, budget: int) -> None:
    """Trim only drops serial and timer requests past the budget, keeping order."""
    trimmed = trim(program, budget)
    spent = Counter(s.driver for s in trimmed if isinstance(s, Fault | Restart))
    assert all(spent[d] <= budget for d in KEPT)
    dropped = Counter(program) - Counter(trimmed)
    assert all(isinstance(s, Fault | Restart) and s.driver in KEPT for s in dropped)
    assert trim(trimmed, budget) == trimmed
    it = iter(program)
    assert all(step in it for step in trimmed)


# --- composition -------------------------------------------------------------------


storage_steps = steps.filter(lambda s: owner(s) == "storage")
net_steps = steps.filter(lambda s: owner(s) == "net")


@given(
    model=states(),
    pair=st.tuples(storage_steps, net_steps).flatmap(st.permutations),
    data=st.data(),
)
def test_steps_of_different_components_commute(
    model: Model, pair: list[Step], data: st.DataObject
) -> None:
    """Only Root's availability crosses components, so with no driver
    recovering a storage step and a network step can run in either order."""
    a, b = pair
    model = replace(model, root=model.root.settled(DRIVERS))
    seen_a = data.draw(outcome(model.expect(a)))
    seen_b = data.draw(outcome(model.expect(b)))
    assert model.after(b, seen_b).expect(a) == model.expect(a)
    assert model.after(a, seen_a).expect(b) == model.expect(b)
    one = model.after(a, seen_a).after(b, seen_b)
    other = model.after(b, seen_b).after(a, seen_a)
    assert one.key() == other.key()


@given(trace=runs())
def test_every_step_allows_something(trace: list[Seen]) -> None:
    assert all(row.allowed.anything or row.allowed.values for row in trace)


# --- Root ----------------------------------------------------------------------------


def between_resets(trace: list[Seen]) -> list[list[Seen]]:
    """The trace cut at every reset, since Root starts over at each."""
    stretches: list[list[Seen]] = [[]]
    for row in trace:
        if isinstance(row.step, Reset | CrashPut):
            stretches.append([])
        else:
            stretches[-1].append(row)
    return stretches


@given(trace=runs())
def test_restarts_count_up_then_give_up_once_then_stay_gone(trace: list[Seen]) -> None:
    """Per driver, between resets: restarts numbered 1 to the budget, then one
    give-up, then gone. Faults, debug restarts and wants share the count, and
    each takes the path its request names."""
    paths = {Fault: "restart", Restart: "debug-restart", Want: "ctl-restart"}
    for stretch in between_resets(trace):
        seen: dict[str, list[Outcome]] = {}
        for step, result in ((row.step, row.outcome) for row in stretch):
            if isinstance(step, Fault | Restart | Want):
                seen.setdefault(step.driver, []).append(result)
                if isinstance(result, Restarted):
                    assert result.path == paths[type(step)]
        for outcomes in seen.values():
            restarts = [o for o in outcomes if isinstance(o, Restarted)]
            assert [r.count for r in restarts] == list(range(1, len(restarts) + 1))
            expected: list[Outcome] = [
                *restarts,
                *[GaveUp("budget-exhausted")] * (len(outcomes) > BUDGET),
                *[Status.GONE] * max(0, len(outcomes) - BUDGET - 1),
            ]
            assert outcomes == expected


@given(
    driver=st.sampled_from(["serial", "timer", "blk", "eth"]),
    kinds=st.lists(st.sampled_from(["fault", "restart", "want"]), max_size=2 * BUDGET + 2),
)
def test_a_driver_is_restarted_budget_times_then_given_up_on(
    driver: Driver, kinds: list[Literal["fault", "restart", "want"]]
) -> None:
    """Root's answers are deterministic, so following them for any run of
    requests to one driver gives exactly that sequence, whoever asks."""
    model = Model.start(BUDGET)
    answers = []
    for kind in kinds:
        step = STEP.validate_python({"kind": kind, "driver": driver})
        (only,) = model.expect(step).values
        answers.append(only)
        model = model.after(step, only)
    counts = [a.count for a in answers if isinstance(a, Restarted)]
    assert counts == list(range(1, min(len(kinds), BUDGET) + 1))
    assert answers[BUDGET : BUDGET + 1] == [GaveUp("budget-exhausted")][: len(kinds) - BUDGET]
    assert all(a == Status.GONE for a in answers[BUDGET + 1 :])


@given(
    start=states(), held_file=st.integers(0, 3), other=st.integers(0, 3), seed=st.integers(0, 255)
)
def test_without_the_block_driver_no_data_reads_back(
    start: Model, held_file: int, other: int, seed: int
) -> None:
    """Once Root gives up on blk, every read fails, a missing file's included:
    the FAT server answers nothing from its cached window."""
    if (model := holding(without_fault(start), held_file, seed)) is None:
        return
    blk = Fault(kind="fault", driver="blk")
    while model.root.up("blk"):
        (only,) = model.expect(blk).values
        model = model.after(blk, only)
    for f in (held_file, other):
        assert model.expect(Get(kind="get", file=f)).values == {Status.EIO, Status.ERROR}


# --- the recovery window -----------------------------------------------------------

recoverable = st.sampled_from(["blk", "eth"])


@given(start=states(), driver=recoverable, kind=st.sampled_from(["fault", "restart"]), then=steps)
def test_an_unsettled_request_only_widens_the_next_step(
    start: Model,
    driver: Literal["blk", "eth"],
    kind: Literal["fault", "restart"],
    then: Step,
) -> None:
    """Not waiting for the driver adds outcomes to the next step and nothing
    else: Root spends the same budget, and after that step the two runs are
    the same model again."""
    model = replace(start, root=start.root.settled(DRIVERS))
    settled = STEP.validate_python({"kind": kind, "driver": driver})
    unsettled = STEP.validate_python({"kind": kind, "driver": driver, "settle": False})
    assert model.expect(settled) == model.expect(unsettled)
    (seen,) = model.expect(settled).values
    waited, hurried = model.after(settled, seen), model.after(unsettled, seen)
    assert waited.root.chart(driver).model.spent == hurried.root.chart(driver).model.spent
    if isinstance(then, Fault | Restart) and then.driver == driver and not then.settle:
        return
    assert (
        waited.expect(then).values <= hurried.expect(then).values or hurried.expect(then).anything
    )
    for outcome in sorted(waited.expect(then).values, key=render):
        assert waited.after(then, outcome).key() == hurried.after(then, outcome).key()


# --- resets --------------------------------------------------------------------------

reset = Reset(kind="reset")


@given(model=states())
def test_a_reset_is_idempotent(model: Model) -> None:
    once = model.after(reset, Status.OK)
    assert once.after(reset, Status.OK).key() == once.key()


@given(start=states(), file=st.integers(0, 3), seed=st.integers(0, 255))
def test_a_reset_forgets_root_and_keeps_the_disk(start: Model, file: int, seed: int) -> None:
    """Budgets, give-ups, the armed fault and FatFs's window start over. What
    a sync put on disk stays."""
    if (written := holding(start, file, seed)) is None:
        return
    model = with_fault(written, "read", "fat", file, once=False)
    rebooted = model.after(reset, Status.OK)
    assert rebooted.root.key() == ()
    assert rebooted.storage.armed is None
    assert not rebooted.storage.suspect
    assert rebooted.expect(Get(kind="get", file=file)) == expect(Seed(seed))


@given(model=states(), file=st.integers(0, 3), seed=st.integers(0, 255), after=st.integers(0, 200))
def test_a_crashed_write_changes_only_its_file(
    model: Model, file: int, seed: int, after: int
) -> None:
    """A crash mid-rewrite tears only its own file, which then reads as before,
    empty (differs) or whole with the new seed, and nothing else."""
    was = model.storage.file(file)
    crashed = model.after(
        CrashPut(kind="crash-put", file=file, seed=seed, after_ms=after), Status.OK
    )
    rebooted = model.after(reset, Status.OK)
    chart = crashed.storage.file(file)
    assert chart.at == ("unknown" if was.at in {"unknown", "torn"} else "torn")
    for other in set(range(4)) - {file}:
        assert crashed.storage.file(other).at == rebooted.storage.file(other).at
    if chart.at == "torn" and not crashed.storage.damaged:
        before: dict[str, Outcome] = {
            "absent": Status.ENOENT,
            "held": Seed(was.model.seed or 0),
            "corrupt": Status.DIFFERS,
        }
        assert crashed.expect(Get(kind="get", file=file)) == expect(
            before[was.at], Status.DIFFERS, Seed(seed)
        )


# --- storage -------------------------------------------------------------------------


@given(trace=runs())
def test_a_file_written_ok_reads_back_until_something_disturbs_it(trace: list[Seen]) -> None:
    """With the disk up and settled, no fault armed and nothing left dirty, a read returns
    the last seed written to that file, or enoent if none was."""
    for step, allowed, before in ((row.step, row.allowed, row.before) for row in trace):
        storage = before.storage
        if (
            not isinstance(step, Get)
            or not before.root.up("blk")
            or before.root.recovering("blk")
            or storage.suspect
            or storage.damaged
            or storage.armed is not None
        ):
            continue
        chart = storage.file(step.file)
        match chart.at:
            case "absent":
                assert allowed == expect(Status.ENOENT)
            case "held":
                assert allowed == expect(Seed(chart.model.seed or 0))


files = st.integers(0, 3)
volume_steps: st.SearchStrategy[Step] = st.one_of(
    st.builds(Put, kind=st.just("put"), file=files, seed=st.integers(0, 255)),
    st.builds(Overwrite, kind=st.just("overwrite"), file=files, seed=st.integers(0, 255)),
    st.builds(Get, kind=st.just("get"), file=files),
    st.builds(Remove, kind=st.just("remove"), file=files),
    st.builds(Rename, kind=st.just("rename"), src=files, dst=files),
)


@given(start=states(), file=files, seed=st.integers(0, 255), step=volume_steps)
def test_a_dirty_window_may_fail_any_volume_step(
    start: Model, file: int, seed: int, step: Step
) -> None:
    """A put failed by a write fault leaves FatFs's window dirty, and every
    later load retries it: once the fault is gone, any step that loads a
    sector may still report eio, a lookup of a missing file included."""
    put = Put(kind="put", file=file, seed=seed)
    faulted = with_fault(start, "write", "data", file, once=False)
    if Status.EIO not in faulted.expect(put):
        return  # the volume refuses the put some other way
    dirty = without_fault(faulted.after(put, Status.EIO))
    assert dirty.storage.suspect
    assert Status.EIO in dirty.expect(step)


regions: st.SearchStrategy[Region] = st.sampled_from(["data", "fat", "entry"])


@given(model=states(), file=files, region=regions, target=files, once=st.booleans())
def test_write_faults_never_change_what_a_get_returns(
    model: Model, file: int, region: Region, target: int, once: bool
) -> None:
    get = Get(kind="get", file=file)
    faulted = with_fault(model, "write", region, target, once)
    assert faulted.expect(get) == without_fault(model).expect(get)


@given(model=states(), file=files, seed=st.integers(0, 255), target=files, once=st.booleans())
def test_a_put_never_reads_file_data(
    model: Model, file: int, seed: int, target: int, once: bool
) -> None:
    put = Put(kind="put", file=file, seed=seed)
    faulted = with_fault(model, "read", "data", target, once)
    assert faulted.expect(put) == without_fault(model).expect(put)


@given(
    start=states(),
    op=st.sampled_from(["read", "write"]),
    region=regions,
    file=files,
    once=st.booleans(),
    step=steps,
)
def test_disarm_forgets_the_fault(
    start: Model, op: Literal["read", "write"], region: Region, file: int, once: bool, step: Step
) -> None:
    model = with_fault(start, op, region, file, once)
    disarmed = model.after(Disarm(kind="disarm"), Status.OK)
    # Disarm is a step like any other, so it also settles recovering drivers.
    plain = without_fault(model)
    assert disarmed.expect(step) == replace(plain, root=plain.root.settled(DRIVERS)).expect(step)


@given(start=states(), file=files, seed=st.integers(0, 255), once=st.booleans())
def test_a_latent_read_error_on_present_data_is_always_reported(
    start: Model, file: int, seed: int, once: bool
) -> None:
    """Strict, unlike the shared FAT and directory sectors: a read error on a
    present file's data is reported, and a sticky one stays."""
    if (model := holding(start, file, seed)) is None:
        return
    storage = model.storage
    arm = Arm(kind="arm", op="read", region="data", file=file, once=once)
    assert model.expect(arm) == expect(Status.PRESENT)
    armed = model.after(arm, Status.PRESENT)
    get = Get(kind="get", file=file)
    assert armed.expect(get) == expect(Status.EIO)
    after = armed.after(get, Status.EIO)
    assert (after.storage.armed is None) == (once and not storage.suspect)


@given(
    start=states(),
    file=files,
    seed=st.integers(0, 255),
    region=st.sampled_from(["data", "fat"]),
    again=st.integers(0, 255),
)
def test_a_rewrite_sets_a_data_fault_adrift(
    start: Model, file: int, seed: int, region: Literal["data", "fat"], again: int
) -> None:
    """Found by the fuzz: rewriting a file may move its data to new clusters,
    so a sticky read error armed on the old sector need not fail the next
    read. The model must allow both."""
    if (model := holding(without_fault(start), file, seed)) is None:
        return
    arm = Arm(kind="arm", op="read", region=region, file=file, once=False)
    armed = model.after(arm, Status.PRESENT)
    rewritten = armed.after(Put(kind="put", file=file, seed=again), Status.OK)
    allowed = rewritten.expect(Get(kind="get", file=file))
    assert Seed(again) in allowed
    assert Status.EIO in allowed


@given(start=states(), file=files, seed=st.integers(0, 255))
def test_rewriting_corrupted_data_heals_it(start: Model, file: int, seed: int) -> None:
    if (model := holding(without_fault(start), file, seed)) is None:
        return
    corrupted = model.after(Corrupt(kind="corrupt", region="data", file=file), Status.PRESENT)
    healed = corrupted.after(Put(kind="put", file=file, seed=seed), Status.OK)
    assert Seed(seed) in healed.expect(Get(kind="get", file=file))
    assert Status.DIFFERS not in healed.expect(Get(kind="get", file=file))


@given(model=states(), file=files, region=regions, op=st.sampled_from(["read", "write"]))
def test_nothing_can_be_aimed_at_a_missing_file(
    model: Model, file: int, region: Region, op: Literal["read", "write"]
) -> None:
    if model.storage.damaged or model.storage.file(file).at != "absent":
        return
    arm = Arm(kind="arm", op=op, region=region, file=file, once=False)
    assert model.expect(arm) == expect(Status.ABSENT)
    assert model.after(arm, Status.ABSENT).storage.armed is None


# --- remove, rename and overwrite ----------------------------------------------------


def reads(model: Model) -> tuple[Allowed[Outcome], ...]:
    """The observation these laws compare: what a get of each file may return."""
    return tuple(model.expect(Get(kind="get", file=f)) for f in range(FILES))


def plain(model: Model) -> Model | None:
    """`model` with no armed fault, a clean window and boot plan and Root
    settled, or None where the block driver is gone or the volume damaged.
    A dirty window has its own law, test_a_dirty_window_may_fail_any_volume_step."""
    if not model.root.up("blk") or model.storage.damaged:
        return None
    storage = replace(model.storage.rebooted(), disk=CLEAN_DISK)
    return replace(model, root=model.root.settled(DRIVERS), storage=storage)


def absent(model: Model, file: int) -> Model:
    """A plain `model` with `file` removed, if it may be there."""
    remove = Remove(kind="remove", file=file)
    return model.after(remove, Status.OK) if Status.OK in model.expect(remove) else model


pairs = st.lists(files, min_size=2, max_size=2, unique=True)
seeds = st.integers(0, 255)


def change(file: int, other: int) -> st.SearchStrategy[Step]:
    """A data step on `file`, a rename onto `other` among them, each kind
    equally likely. The schema's own strategy rarely draws a rename."""
    return st.one_of(
        seeds.map(lambda s: Put(kind="put", file=file, seed=s)),
        seeds.map(lambda s: Overwrite(kind="overwrite", file=file, seed=s)),
        st.just(Get(kind="get", file=file)),
        st.just(Remove(kind="remove", file=file)),
        st.just(Rename(kind="rename", src=file, dst=other)),
    )


@st.composite
def disjoint_changes(draw: st.DrawFn) -> tuple[Step, Step]:
    """Two data steps touching disjoint files, built rather than filtered."""
    order = draw(st.permutations(range(FILES)))
    return draw(change(order[0], order[1])), draw(change(order[2], order[3]))


@given(start=states(), file=files)
def test_removing_an_absent_file_is_the_identity(start: Model, file: int) -> None:
    if (model := plain(start)) is None:
        return
    model = absent(model, file)
    remove = Remove(kind="remove", file=file)
    assert model.expect(remove) == expect(Status.ENOENT)
    assert reads(model.after(remove, Status.ENOENT)) == reads(model)


@given(start=states(), file=files, seed=st.integers(0, 255))
def test_a_removed_file_reads_as_absent(start: Model, file: int, seed: int) -> None:
    """Even with a sticky read error on its old data: the freed clusters are
    no longer the file's, so a get finds no entry and reads nothing."""
    if (plain_start := plain(start)) is None or (model := holding(plain_start, file, seed)) is None:
        return
    armed = model.after(Arm(kind="arm", op="read", region="data", file=file, once=False),
                        Status.PRESENT)  # fmt: skip
    removed = armed.after(Remove(kind="remove", file=file), Status.OK)
    allowed = removed.expect(Get(kind="get", file=file))
    assert not allowed.anything
    assert allowed.values <= {Status.ENOENT, Status.EIO}
    assert Status.ENOENT in allowed.values


@given(start=states(), file=files, seed=st.integers(0, 255))
def test_remove_is_idempotent(start: Model, file: int, seed: int) -> None:
    """remove; remove = remove, the second reporting ENOENT."""
    if (plain_start := plain(start)) is None or (model := holding(plain_start, file, seed)) is None:
        return
    remove = Remove(kind="remove", file=file)
    once = model.after(remove, Status.OK)
    assert once.expect(remove) == expect(Status.ENOENT)
    assert reads(once.after(remove, Status.ENOENT)) == reads(once)


@given(start=states(), pair=pairs, seed=st.integers(0, 255))
def test_a_rename_moves_what_a_get_reads(start: Model, pair: list[int], seed: int) -> None:
    """After a → b, b reads what a read, a reads as never written, and
    every other file reads as before."""
    src, dst = pair
    if (plain_start := plain(start)) is None:
        return
    if (model := holding(absent(plain_start, dst), src, seed)) is None:
        return
    rename = Rename(kind="rename", src=src, dst=dst)
    assert model.expect(rename) == expect(Status.OK)
    before, after = reads(model), reads(model.after(rename, Status.OK))
    gone = model.expect(Get(kind="get", file=dst))  # dst was absent
    assert after[dst] == before[src]
    assert after[src] == gone
    assert all(after[f] == before[f] for f in range(FILES) if f not in pair)


@given(start=states(), pair=pairs, seed=st.integers(0, 255))
def test_renaming_there_and_back_is_the_identity(start: Model, pair: list[int], seed: int) -> None:
    """a → b; b → a, observed through every get and the fault's target."""
    src, dst = pair
    if (plain_start := plain(start)) is None:
        return
    if (model := holding(absent(plain_start, dst), src, seed)) is None:
        return
    model = model.after(Arm(kind="arm", op="read", region="data", file=src, once=False),
                        Status.PRESENT)  # fmt: skip
    there = model.after(Rename(kind="rename", src=src, dst=dst), Status.OK)
    back = Rename(kind="rename", src=dst, dst=src)
    assert there.expect(back) == expect(Status.OK)
    returned = there.after(back, Status.OK)
    assert reads(returned) == reads(model)
    assert returned.storage.armed == model.storage.armed


@given(start=states(), pair=pairs, seeds=st.tuples(st.integers(0, 255), st.integers(0, 255)))
def test_a_rename_onto_a_present_name_is_refused_and_changes_nothing(
    start: Model, pair: list[int], seeds: tuple[int, int]
) -> None:
    src, dst = pair
    if (plain_start := plain(start)) is None:
        return
    if (held := holding(plain_start, dst, seeds[1])) is None:
        return
    if (model := holding(held, src, seeds[0])) is None:
        return
    rename = Rename(kind="rename", src=src, dst=dst)
    assert model.expect(rename) == expect(Status.EEXIST)
    assert reads(model.after(rename, Status.EEXIST)) == reads(model)


@given(start=states(), pair=pairs, seed=st.integers(0, 255), once=st.booleans())
def test_a_read_error_follows_its_file_through_a_rename(
    start: Model, pair: list[int], seed: int, once: bool
) -> None:
    """The chain does not move, so a latent error on the data stays strict."""
    src, dst = pair
    if (plain_start := plain(start)) is None:
        return
    if (model := holding(absent(plain_start, dst), src, seed)) is None or model.storage.suspect:
        return
    armed = model.after(Arm(kind="arm", op="read", region="data", file=src, once=once),
                        Status.PRESENT)  # fmt: skip
    renamed = armed.after(Rename(kind="rename", src=src, dst=dst), Status.OK)
    assert renamed.expect(Get(kind="get", file=dst)) == expect(Status.EIO)


@given(start=states(), file=files, seed=st.integers(0, 255), again=st.integers(0, 255),
       once=st.booleans())  # fmt: skip
def test_an_overwrite_never_reads_file_data(
    start: Model, file: int, seed: int, again: int, once: bool
) -> None:
    """Whole sectors in place: a read error on the file's data cannot fail it."""
    if (plain_start := plain(start)) is None or (model := holding(plain_start, file, seed)) is None:
        return
    overwrite = Overwrite(kind="overwrite", file=file, seed=again)
    faulted = with_fault(model, "read", "data", file, once)
    assert faulted.expect(overwrite) == model.expect(overwrite) == expect(Status.OK)
    rewritten = faulted.after(overwrite, Status.OK)
    assert rewritten.storage.armed == faulted.storage.armed, "the data kept its clusters"


@given(
    start=states(),
    held=st.sets(files),
    ready=st.booleans(),
    pair=disjoint_changes(),
    data=st.data(),
)
def test_changes_to_distinct_files_commute(
    start: Model, held: set[int], ready: bool, pair: tuple[Step, Step], data: st.DataObject
) -> None:
    """With no fault and a clean disk, two storage steps on disjoint files
    run in either order to the same model. `held` files are written first.
    When `ready`, every rename's source is written and its target removed,
    or a rename would seldom succeed."""
    a, b = pair
    if (model := plain(start)) is None:
        return
    renames = [s for s in pair if isinstance(s, Rename)] if ready else []
    for f in held | {s.src for s in renames}:
        model = holding(model, f, f) or model
    for s in renames:
        model = absent(model, s.dst)
    seen_a = data.draw(outcome(model.expect(a)))
    seen_b = data.draw(outcome(model.expect(b)))
    assert model.after(b, seen_b).expect(a) == model.expect(a)
    assert model.after(a, seen_a).expect(b) == model.expect(b)
    assert (
        model.after(a, seen_a).after(b, seen_b).key()
        == model.after(b, seen_b).after(a, seen_a).key()
    )


def test_a_renamed_file_reads_under_its_new_name() -> None:
    """The anchor: identity laws alone pass for a model that ignores renames."""
    model = Model.start(BUDGET).after(Put(kind="put", file=1, seed=5), Status.OK)
    model = model.after(Rename(kind="rename", src=1, dst=2), Status.OK)
    assert model.expect(Get(kind="get", file=2)) == expect(Seed(5))
    assert model.expect(Get(kind="get", file=1)) == expect(Status.ENOENT)


# --- the orchestrator ------------------------------------------------------------------

drivers = st.sampled_from(DRIVERS)


def settled(model: Model) -> Model:
    """`model` with no driver recovering, the scope of laws 3 and 6."""
    return replace(model, root=model.root.settled(DRIVERS))


def as_root_word(outcome: Outcome, path: Literal["debug-restart", "ctl-restart"]) -> Outcome:
    """The same Root decision under another request's name."""
    return Restarted(path, outcome.count) if isinstance(outcome, Restarted) else outcome


def wanted(model: Model, driver: Driver, times: int) -> Model:
    """`model` after `times` wants of `driver`, each with the one outcome
    Root's determinism allows."""
    want = Want(kind="want", driver=driver)
    for _ in range(times):
        only = model.expect(want).only()
        assert only is not None
        model = model.after(want, only)
    return model


@given(model=states(), driver=drivers)
def test_a_want_is_a_restart_through_the_orchestrator(model: Model, driver: Driver) -> None:
    """Law 1, observed through `expect` and the components' keys: a want
    allows what a settled debug restart allows, under Root's word
    `ctl-restart`, and leaves Root, storage and network as that restart does."""
    want = Want(kind="want", driver=driver)
    restart = Restart(kind="restart", driver=driver)
    allowed = model.expect(want)
    debug = model.expect(restart)
    assert not allowed.anything
    assert not debug.anything
    assert allowed.values == {as_root_word(o, "ctl-restart") for o in debug.values}
    for seen in allowed.values:
        event(render(seen))
        via_ctl = model.after(want, seen)
        via_debug = model.after(restart, as_root_word(seen, "debug-restart"))
        assert via_ctl.root.key() == via_debug.root.key()
        assert via_ctl.storage.key() == via_debug.storage.key()
        assert via_ctl.net.key() == via_debug.net.key()


@given(model=states(), driver=drivers, first=st.booleans(), data=st.data())
def test_applying_a_want_again_changes_nothing(
    model: Model, driver: Driver, first: bool, data: st.DataObject
) -> None:
    """Law 2: re-applying the newest want, or none when nothing was wanted,
    allows only `OK` and leaves the model as it was."""
    model = settled(model)
    if first:
        want = Want(kind="want", driver=driver)
        model = model.after(want, data.draw(outcome(model.expect(want))))
    event(f"wanted={driver in model.orch.wanted}")
    rewant = Rewant(kind="rewant", driver=driver)
    assert model.expect(rewant) == expect(Status.OK)
    assert model.after(rewant, Status.OK).key() == model.key()


@given(model=states(), driver=drivers)
def test_a_look_changes_nothing(model: Model, driver: Driver) -> None:
    """Law 3, in its scope (no driver recovering): every outcome a look
    allows leaves the model as it was."""
    model = settled(model)
    look = Look(kind="look", driver=driver)
    for seen in model.expect(look).values:
        event(render(seen))
        assert model.after(look, seen).key() == model.key()


@given(model=states(), other=states(), driver=drivers)
def test_a_look_reads_root_alone(model: Model, other: Model, driver: Driver) -> None:
    """Law 4: a look is level-triggered. Two models with the same Root give
    the same answer, whatever else their histories left behind."""
    mixed = replace(other, root=model.root)
    look = Look(kind="look", driver=driver)
    assert mixed.expect(look) == model.expect(look)


@given(driver=drivers, extra=st.integers(0, 2), program=programs, data=st.data())
def test_a_gone_driver_looks_gone_until_a_reset(
    driver: Driver, extra: int, program: Program, data: st.DataObject
) -> None:
    """Law 4 at its absorbing state: once Root gives a driver up, a look
    shows `gone` after any later steps that are not a reset, wanted or not."""
    model = wanted(Model.start(BUDGET), driver, BUDGET + 1 + extra)
    look = Look(kind="look", driver=driver)
    for step in program:
        if isinstance(step, Reset | CrashPut):
            break
        model = model.after(step, data.draw(outcome(model.expect(step))))
        assert model.expect(look) == expect(Condition("gone", "gave_up"))


@given(driver=drivers, spent=st.integers(0, BUDGET))
def test_a_driver_looks_degraded_exactly_when_its_budget_is_spent(
    driver: Driver, spent: int
) -> None:
    """Law 4 at its boundary (`floor` 0): degraded at `spent == budget`,
    ready below it."""
    model = wanted(Model.start(BUDGET), driver, spent)
    want = Condition("degraded", "low_budget") if spent == BUDGET else Condition("ready", "settled")
    event(want.name)
    assert model.expect(Look(kind="look", driver=driver)) == expect(want)


@given(trace=runs(observable=False))
def test_without_a_status_page_nothing_is_known_or_commanded(trace: list[Seen]) -> None:
    """Law 5: with no status page a look is `unknown`, and a want fails
    without touching Root. Every step lets a recovering driver settle, so
    Root after the want is compared with Root settled."""
    for row in trace:
        match row.step:
            case Want():
                assert row.allowed == expect(Status.ERROR)
                after = row.before.after(row.step, row.outcome)
                assert after.root.key() == settled(row.before).root.key()
            case Look():
                assert row.allowed == expect(Condition("unknown", "no_observation"))


def commutes(model: Model, a: Step, seen_a: Outcome, b: Step, seen_b: Outcome) -> bool:
    """Whether `a` and `b` run in either order to the same model, each
    allowing the same outcomes either side of the other."""
    return (
        model.after(b, seen_b).expect(a) == model.expect(a)
        and model.after(a, seen_a).expect(b) == model.expect(b)
        and model.after(a, seen_a).after(b, seen_b).key()
        == model.after(b, seen_b).after(a, seen_a).key()
    )


@given(model=states(), driver=drivers, other=st.one_of(storage_steps, net_steps), data=st.data())
def test_a_want_commutes_with_what_a_restart_commutes_with(
    model: Model, driver: Driver, other: Step, data: st.DataObject
) -> None:
    """Law 6, derived from law 1 and checked: with no driver recovering, a
    want commutes with a storage or network step exactly when a debug restart
    of the same driver does."""
    model = settled(model)
    want = Want(kind="want", driver=driver)
    restart = Restart(kind="restart", driver=driver)
    seen = data.draw(outcome(model.expect(want)))
    seen_other = data.draw(outcome(model.expect(other)))
    via_ctl = commutes(model, want, seen, other, seen_other)
    via_debug = commutes(model, restart, as_root_word(seen, "debug-restart"), other, seen_other)
    event(f"commutes={via_ctl}")
    assert via_ctl == via_debug


def _settle(step: Step) -> Step:
    """The settled form of a request. An unsettled one leaves its driver
    recovering, outside law 3's scope."""
    match step:
        case Fault(driver=d):
            return Fault(kind="fault", driver=d)
        case Restart(driver=d):
            return Restart(kind="restart", driver=d)
        case _:
            return step


@given(
    model=states(),
    driver=drivers,
    look=st.booleans(),
    other=steps.map(_settle),
    data=st.data(),
)
def test_looks_and_rewants_commute_with_every_step(
    model: Model, driver: Driver, look: bool, other: Step, data: st.DataObject
) -> None:
    """Law 6, derived from laws 2 and 3: a look or a rewant runs before or
    after any settled step to the same model."""
    model = settled(model)
    mine: Step = Look(kind="look", driver=driver) if look else Rewant(kind="rewant", driver=driver)
    seen = data.draw(outcome(model.expect(mine)))
    after_other = data.draw(outcome(model.expect(other)))
    one = model.after(mine, seen).after(other, after_other)
    two = model.after(other, after_other)
    two = two.after(mine, data.draw(outcome(two.expect(mine))))
    assert model.after(mine, seen).expect(other) == model.expect(other)
    assert one.key() == two.key()


@given(model=states(), driver=drivers, data=st.data())
def test_a_reset_forgets_every_want(model: Model, driver: Driver, data: st.DataObject) -> None:
    """Law 7: after a reset nothing is wanted, so a rewant is the identity."""
    want = Want(kind="want", driver=driver)
    model = model.after(want, data.draw(outcome(model.expect(want))))
    model = model.after(Reset(kind="reset"), Status.OK)
    assert model.orch.wanted == frozenset()
    rewant = Rewant(kind="rewant", driver=driver)
    assert model.after(rewant, Status.OK).key() == model.key()


def test_a_wanted_driver_degrades_then_is_given_up_on() -> None:
    """The anchor: the laws above also hold for a look that allows
    everything, so pin what one driver shows along its whole budget."""
    eth: Driver = "eth"
    look, want = Look(kind="look", driver=eth), Want(kind="want", driver=eth)
    model = Model.start(BUDGET)
    assert model.expect(look) == expect(Condition("ready", "settled"))
    model = wanted(model, eth, BUDGET)
    assert model.expect(look) == expect(Condition("degraded", "low_budget"))
    assert model.expect(want) == expect(GaveUp("budget-exhausted"))
    model = model.after(want, GaveUp("budget-exhausted"))
    assert model.expect(look) == expect(Condition("gone", "gave_up"))
    assert model.expect(want) == expect(Status.GONE)
    assert model.expect(Rewant(kind="rewant", driver=eth)) == expect(Status.OK)


def _gave_up_a_want(trace: list[Seen]) -> bool:
    return any(isinstance(r.step, Want) and isinstance(r.outcome, GaveUp) for r in trace)


def _rewanted(trace: list[Seen]) -> bool:
    return any(isinstance(r.step, Rewant) and r.step.driver in r.before.orch.wanted for r in trace)


def _wanted_after_a_reset(trace: list[Seen]) -> bool:
    reset = next((i for i, r in enumerate(trace) if isinstance(r.step, Reset)), len(trace))
    return any(isinstance(r.step, Want) for r in trace[reset:])


def _looked_degraded(trace: list[Seen]) -> bool:
    return any(r.outcome == Condition("degraded", "low_budget") for r in trace)


REACH: Final[dict[str, Callable[[list[Seen]], bool]]] = {
    "a want given up": _gave_up_a_want,
    "a rewant of a wanted driver": _rewanted,
    "a want after a reset": _wanted_after_a_reset,
    "a look at a degraded driver": _looked_degraded,
}


@pytest.mark.parametrize("name", sorted(REACH))
def test_runs_reach_every_class(name: str) -> None:
    """`event` only reports, so require that `runs()` can reach each class
    the laws above depend on."""
    find(runs(), REACH[name], settings=settings(max_examples=20_000, database=None))


# --- the interpreter's parsing -------------------------------------------------------

terms = st.sampled_from(["ok", "{error,eio}", "{error,enospc}", "{error,enosys}"])


@given(write=terms, sync=terms, close=terms)
def test_a_put_is_ok_only_when_every_step_is(write: str, sync: str, close: str) -> None:
    named = put_outcome(f"{{{write},{sync},{close}}}")
    errors = {write, sync, close} - {"ok"}
    want = Status.OK if not errors else Status.EIO if errors == {"{error,eio}"} else Status.ERROR
    assert named == want


@given(seed=st.integers(0, 255), reason=st.from_regex(r"[a-z]{1,8}", fullmatch=True))
def test_get_outcomes_name_seeds_and_errors(seed: int, reason: str) -> None:
    assert get_outcome(f"{{ok,{seed}}}") == Seed(seed)
    named = get_outcome(f"{{error,{reason}}}")
    assert (named == Status.ERROR) == (reason not in {"enoent", "eio"})


@given(
    path=st.sampled_from(["restart", "debug-restart", "ctl-restart"]),
    count=st.integers(1, 64),
    reason=st.from_regex(r"[a-z-]{1,20}", fullmatch=True),
)
def test_root_lines_parse_to_their_decision(
    path: Literal["restart", "debug-restart", "ctl-restart"], count: int, reason: str
) -> None:
    assert decision(path, str(count), None) == Restarted(path, count)
    assert decision("giveup", None, reason) == GaveUp(reason)
    assert decision("gone", None, None) == Status.GONE


names = st.sampled_from(get_args(ConditionName.__value__))
reasons = st.sampled_from(get_args(Reason.__value__))
words = st.from_regex(r"[a-z_]{1,16}", fullmatch=True)


@given(name=names, reason=reasons, other=words)
def test_a_look_parses_exactly_the_closed_conditions(
    name: ConditionName, reason: Reason, other: str
) -> None:
    """Every pair in the closed sets parses to itself, and a word outside
    either set is ERROR, never a condition the model might allow."""
    assert condition_outcome(name, reason) == Condition(name, reason)
    if other not in get_args(ConditionName.__value__):
        assert condition_outcome(other, reason) == Status.ERROR
    if other not in get_args(Reason.__value__):
        assert condition_outcome(name, other) == Status.ERROR


@given(
    reported=st.sampled_from(["held", "gone", "timeout", "no_observation"]),
    root=st.one_of(
        st.none(),
        st.builds(Restarted, path=st.just("ctl-restart"), count=st.integers(1, 64)),
        st.just(GaveUp("budget-exhausted")),
    ),
)
def test_root_decides_a_want_and_the_probe_only_fills_in(
    reported: str, root: Outcome | None
) -> None:
    """Root's line is the outcome whenever it printed one. Without it, only a
    driver the probe saw gone is GONE, and anything else is ERROR."""
    got = want_outcome(reported, root)
    if root is not None:
        assert got == root
    else:
        assert got == (Status.GONE if reported == "gone" else Status.ERROR)
