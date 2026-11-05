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
| a reset is offered on a damaged volume | test_steer.test_no_reset_is_offered_on_a_damaged_volume |

The pre-split model ran in lockstep with this one before it was deleted.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import replace
from typing import Final, Literal

from hypothesis import given
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import PLAN, Expected
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.fatlayout import Region
from chryso_testbed.faults import (
    DRIVERS,
    KEPT,
    PROGRAM,
    STEP,
    Arm,
    Corrupt,
    CrashPut,
    Disarm,
    Expect,
    Fault,
    Get,
    Model,
    Program,
    Put,
    Reset,
    Restart,
    Step,
    owner,
    trim,
)
from chryso_testbed.inject import decision, get_outcome, put_outcome
from chryso_testbed.lifecycles import FaultLifecycle, FaultRecord
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted, Seed, Status, render

BUDGET: Final = 3
# Outcomes for steps whose model admits anything.
UNIVERSE: Final[tuple[Outcome, ...]] = (
    Status.OK, Status.EIO, Status.ERROR, Status.ENOENT, Status.DIFFERS,
    Seed(0), Seed(255), Status.PRESENT, Status.ABSENT,
)  # fmt: skip

steps = from_schema(STEP.json_schema()).map(STEP.validate_python)
programs = st.lists(steps, max_size=12).map(tuple)
disks = from_schema(PLAN_SCHEMA).map(PLAN.validate_python).map(Expected.of)
type Trace = list[tuple[Step, Expect, Outcome, Model]]


def outcome(allowed: Expect) -> st.SearchStrategy[Outcome]:
    choices = UNIVERSE if allowed.anything else sorted(allowed.outcomes, key=render)
    return st.sampled_from(choices)


@st.composite
def runs(draw: st.DrawFn) -> Trace:
    """A program, and for each step what was allowed, what happened and the
    model before it."""
    model = Model.start(BUDGET, draw(disks))
    trace: Trace = []
    for step in draw(programs):
        allowed = model.expect(step)
        seen = draw(outcome(allowed))
        trace.append((step, allowed, seen, model))
        model = model.after(step, seen)
    return trace


@st.composite
def states(draw: st.DrawFn) -> Model:
    """A model the fuzz can reach: the state after a simulated run."""
    trace = draw(runs())
    if not trace:
        return Model.start(BUDGET)
    step, _, seen, before = trace[-1]
    return before.after(step, seen)


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
def test_every_step_allows_something(trace: Trace) -> None:
    assert all(allowed.anything or allowed.outcomes for _, allowed, _, _ in trace)


# --- Root ----------------------------------------------------------------------------


def between_resets(trace: Trace) -> list[Trace]:
    """The trace cut at every reset, since Root starts over at each."""
    stretches: list[Trace] = [[]]
    for entry in trace:
        if isinstance(entry[0], Reset | CrashPut):
            stretches.append([])
        else:
            stretches[-1].append(entry)
    return stretches


@given(trace=runs())
def test_restarts_count_up_then_give_up_once_then_stay_gone(trace: Trace) -> None:
    """Per driver, between resets: restarts numbered 1 to the budget, then one
    give-up, then gone. Faults and debug restarts share the count, and each
    takes the path its request names."""
    for stretch in between_resets(trace):
        seen: dict[str, list[Outcome]] = {}
        for step, _, result, _ in stretch:
            if isinstance(step, Fault | Restart):
                seen.setdefault(step.driver, []).append(result)
                if isinstance(result, Restarted):
                    path = "restart" if isinstance(step, Fault) else "debug-restart"
                    assert result.path == path
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
    kinds=st.lists(st.sampled_from([Fault, Restart]), max_size=2 * BUDGET + 2),
)
def test_a_driver_is_restarted_budget_times_then_given_up_on(
    driver: Literal["serial", "timer", "blk", "eth"], kinds: list[type[Fault] | type[Restart]]
) -> None:
    """Root's answers are deterministic, so following them for any run of
    requests to one driver gives exactly that sequence."""
    model = Model.start(BUDGET)
    answers = []
    for kind in kinds:
        step = kind(kind="fault" if kind is Fault else "restart", driver=driver)  # type: ignore[arg-type]
        (only,) = model.expect(step).outcomes
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
    """Once Root gives up on blk, reading a file's data fails, though a
    lookup FatFs can answer from its cached directory sector still finds a
    missing file missing."""
    if (model := holding(without_fault(start), held_file, seed)) is None:
        return
    blk = Fault(kind="fault", driver="blk")
    while model.root.up("blk"):
        (only,) = model.expect(blk).outcomes
        model = model.after(blk, only)
    assert model.expect(Get(kind="get", file=held_file)).outcomes == {Status.EIO, Status.ERROR}
    if model.storage.file(other).at == "absent":
        assert Status.ENOENT in model.expect(Get(kind="get", file=other))


# --- the recovery window -----------------------------------------------------------

recoverable = st.sampled_from(["blk", "eth"])


@given(start=states(), driver=recoverable, request=st.sampled_from([Fault, Restart]), then=steps)
def test_an_unsettled_request_only_widens_the_next_step(
    start: Model,
    driver: Literal["blk", "eth"],
    request: type[Fault] | type[Restart],
    then: Step,
) -> None:
    """Not waiting for the driver adds outcomes to the next step and nothing
    else: Root spends the same budget, and after that step the two runs are
    the same model again."""
    model = replace(start, root=start.root.settled(DRIVERS))
    kind = "fault" if request is Fault else "restart"
    settled = request(kind=kind, driver=driver)  # type: ignore[arg-type]
    unsettled = request(kind=kind, driver=driver, settle=False)  # type: ignore[arg-type]
    assert model.expect(settled) == model.expect(unsettled)
    (seen,) = model.expect(settled).outcomes
    waited, hurried = model.after(settled, seen), model.after(unsettled, seen)
    assert waited.root.chart(driver).model.spent == hurried.root.chart(driver).model.spent
    if isinstance(then, Fault | Restart) and then.driver == driver and not then.settle:
        return
    assert (
        waited.expect(then).outcomes <= hurried.expect(then).outcomes
        or hurried.expect(then).anything
    )
    for outcome in sorted(waited.expect(then).outcomes, key=render):
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
    assert rebooted.expect(Get(kind="get", file=file)) == Expect(frozenset({Seed(seed)}))


@given(model=states(), file=st.integers(0, 3), seed=st.integers(0, 255), after=st.integers(0, 200))
def test_a_crashed_write_changes_only_its_file(
    model: Model, file: int, seed: int, after: int
) -> None:
    crashed = model.after(
        CrashPut(kind="crash-put", file=file, seed=seed, after_ms=after), Status.OK
    )
    rebooted = model.after(reset, Status.OK)
    assert crashed.storage.file(file).at == "unknown"
    for other in set(range(4)) - {file}:
        assert crashed.storage.file(other).at == rebooted.storage.file(other).at


# --- storage -------------------------------------------------------------------------


@given(trace=runs())
def test_a_file_written_ok_reads_back_until_something_disturbs_it(trace: Trace) -> None:
    """With the disk up and settled, no fault armed and nothing left dirty, a read returns
    the last seed written to that file, or enoent if none was."""
    for step, allowed, _, before in trace:
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
                assert allowed == Expect(frozenset({Status.ENOENT}))
            case "held":
                assert allowed == Expect(frozenset({Seed(chart.model.seed or 0)}))


files = st.integers(0, 3)
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
    assert model.expect(arm) == Expect(frozenset({Status.PRESENT}))
    armed = model.after(arm, Status.PRESENT)
    get = Get(kind="get", file=file)
    assert armed.expect(get) == Expect(frozenset({Status.EIO}))
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
    assert model.expect(arm) == Expect(frozenset({Status.ABSENT}))
    assert model.after(arm, Status.ABSENT).storage.armed is None


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
    path=st.sampled_from(["restart", "debug-restart"]),
    count=st.integers(1, 64),
    reason=st.from_regex(r"[a-z-]{1,20}", fullmatch=True),
)
def test_root_lines_parse_to_their_decision(
    path: Literal["restart", "debug-restart"], count: int, reason: str
) -> None:
    assert decision(path, str(count), None) == Restarted(path, count)
    assert decision("giveup", None, reason) == GaveUp(reason)
    assert decision("gone", None, None) == Status.GONE
