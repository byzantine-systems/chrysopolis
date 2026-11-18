"""Laws of steered generation.

| Planted defect | Caught by |
|---|---|
| `PREFERENCE` without `EEXIST` (found by test_lineage) | test_a_refused_rename_has_a_nominal |
| `requestable` ignored for wants | test_no_want_spends_a_kept_driver_past_its_budget |
| `orchestrate` without its recovery-window step | test_orchestrate_reaches_every_case |
| `until` bounded by the budget alone | test_orchestrate_reaches_every_case |
"""

from __future__ import annotations

from typing import Final

from hypothesis import given
from hypothesis import strategies as st
from test_faults import BUDGET, disks, faulted, states, steps

from chryso_testbed.blkdebug import Expected
from chryso_testbed.faults import (
    KEPT,
    PROGRAM,
    Absorb,
    Arm,
    Corrupt,
    CrashPut,
    Driver,
    Fault,
    Look,
    Model,
    Put,
    Remove,
    Rename,
    Reset,
    Restart,
    Step,
    Want,
    trim,
)
from chryso_testbed.outcomes import Status
from chryso_testbed.steer import (
    MAX_STEPS,
    PREFERENCE,
    SCENARIOS,
    advance,
    enabled,
    faults_to,
    free,
    gone,
    has_file,
    nominal,
    programs,
    reach,
    requestable,
    simulate,
    writes,
)

AIMING: Final = ("latent", "corrupt-heal", "write-fault", "latent-overwrite", "rename-remove")
names = st.sampled_from(sorted(SCENARIOS))


def test_every_status_has_a_preference() -> None:
    """`nominal` falls back on PREFERENCE, so a step the model allows only an
    unlisted status would have no nominal outcome."""
    assert set(PREFERENCE) == set(Status)


def test_a_refused_rename_has_a_nominal() -> None:
    """FatFs refuses a rename onto an existing name, and that is all the
    model allows, so generation must still be able to step past it."""
    model = Model.start(BUDGET)
    for f in (0, 1):
        model = advance(model, Put(kind="put", file=f, seed=5))
    assert nominal(model, Rename(kind="rename", src=1, dst=0)) == Status.EEXIST


def takes_writes(disk: Expected) -> bool:
    """A plan under which some write can be reported ok."""
    return disk.reads != {"error"}


@given(model=states(), step=steps)
def test_the_assumed_outcome_is_always_allowed(model: Model, step: Step) -> None:
    assert nominal(model, step) in model.expect(step)


@given(model=states(), data=st.data())
def test_requests_only_go_where_root_will_answer_them(model: Model, data: st.DataObject) -> None:
    """No request to a driver Root gave up on, and serial and timer never
    past their budget, so the console and clock survive every program."""
    step = data.draw(enabled(model))
    match step:
        case Fault() | Restart():
            assert step.driver in requestable(model)
            assert model.root.up(step.driver)
        case Want(driver=d) if d in KEPT:
            assert d in requestable(model)
        case Absorb(driver=d):
            assert model.root.up(d)
        case _:
            pass


@given(name=names, disk=disks, data=st.data())
def test_every_scenario_draws_a_valid_program(
    name: str, disk: Expected, data: st.DataObject
) -> None:
    start = Model.start(BUDGET, disk)
    program = data.draw(programs(start, name))
    assert PROGRAM.validate_python(PROGRAM.dump_python(program)) == program
    assert len(program) <= MAX_STEPS
    assert trim(program, BUDGET) == program


@given(disk=disks, data=st.data())
def test_reach_writes_a_file_whenever_writes_can_work(disk: Expected, data: st.DataObject) -> None:
    start = Model.start(BUDGET, disk)
    _, model = data.draw(simulate(reach(has_file, writes, 3), start))
    # A damaged volume promises nothing, so a write may look ok there too.
    assert has_file(model) == takes_writes(disk) or model.storage.damaged


@given(data=st.data())
def test_reach_exhausts_a_driver(data: st.DataObject) -> None:
    scenario = reach(gone("blk"), faults_to(("blk",)), BUDGET + 1)
    steps_taken, model = data.draw(simulate(scenario, Model.start(BUDGET)))
    assert not model.root.up("blk")
    assert len(steps_taken) <= 2 * (BUDGET + 1)


@given(name=st.sampled_from(AIMING), disk=disks, data=st.data())
def test_aimed_faults_find_their_target(name: str, disk: Expected, data: st.DataObject) -> None:
    """Each aiming scenario's aimed step hits a file that exists, as far as
    the nominal outcomes go. Random steps before it may miss."""
    if not takes_writes(disk):
        return
    model = Model.start(BUDGET, disk)
    hits = []
    for step in data.draw(programs(model, name)):
        if isinstance(step, Arm | Corrupt):
            hits.append(model.storage.damaged or model.expect(step).values == {Status.PRESENT})
        model = advance(model, step)
    assert any(hits)


@given(model=states(), file=st.integers(0, 3), seed=st.integers(0, 255), data=st.data())
def test_no_reset_is_offered_on_a_damaged_volume(
    model: Model, file: int, seed: int, data: st.DataObject
) -> None:
    """A damaged volume may not mount, and then the run could not continue."""
    put = Put(kind="put", file=file, seed=seed)
    if Status.OK not in model.expect(put) or model.storage.damaged:
        return
    corrupt = Corrupt(kind="corrupt", region="fat", file=file)
    damaged = model.after(put, Status.OK).after(corrupt, Status.PRESENT)
    assert damaged.storage.damaged
    for _ in range(20):
        assert not isinstance(data.draw(enabled(damaged)), Reset | CrashPut)


@given(disk=disks, data=st.data())
def test_the_rename_scenario_moves_a_file_then_removes_one(
    disk: Expected, data: st.DataObject
) -> None:
    """As far as the nominal outcomes go, its rename takes a present file to
    another name whenever one is free, and its remove finds a file, so
    neither is refused."""
    if not takes_writes(disk):
        return
    model = Model.start(BUDGET, disk)
    moved = could_move = removed = False
    for step in data.draw(programs(model, "rename-remove")):
        if isinstance(step, Rename | Remove) and not model.storage.damaged:
            allowed = model.expect(step).values
            # Not refused: a boot plan may still fail the write itself.
            ok = Status.OK in allowed and not allowed & {Status.ENOENT, Status.EEXIST}
            if isinstance(step, Rename):
                could_move |= any(f != step.src for f in free(model))
                moved |= ok and step.src != step.dst
            removed |= isinstance(step, Remove) and ok
        model = advance(model, step)
    assert moved or not could_move
    assert removed


@given(budget=st.sampled_from([BUDGET, 8]), data=st.data())
def test_orchestrate_reaches_every_case(budget: int, data: st.DataObject) -> None:
    """Every program `orchestrate` draws, at the tests' budget and the shipped
    one, holds a want, a look or want at a driver the model has recovering at
    that point, an absorb, eth driven past its budget, and a want of a driver
    Root gave up on, all within MAX_STEPS."""
    model = Model.start(budget)
    program = data.draw(programs(model, "orchestrate"))
    in_window = wanted_gone = eth_gone = False
    for step in program:
        if isinstance(step, Look | Want) and model.root.recovering(step.driver):
            in_window = True
        if isinstance(step, Want) and not model.root.up(step.driver):
            wanted_gone = True
        model = advance(model, step)
        eth_gone = eth_gone or not model.root.up("eth")
    assert any(isinstance(s, Want) for s in program)
    assert any(isinstance(s, Absorb) for s in program)
    assert in_window
    assert eth_gone
    assert wanted_gone


@given(kept=st.sampled_from(sorted(KEPT)), data=st.data())
def test_no_want_spends_a_kept_driver_past_its_budget(kept: str, data: st.DataObject) -> None:
    """With serial or timer at its budget, the steering never offers a want
    of it, so the console and the clock survive. The state is built, not left
    to the random draw, which seldom reaches it."""
    driver: Driver = "serial" if kept == "serial" else "timer"
    model = faulted(Model.start(BUDGET), driver, BUDGET)
    step = data.draw(enabled(model))
    if isinstance(step, Want):
        assert step.driver != driver
