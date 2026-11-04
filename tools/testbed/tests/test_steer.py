"""Laws of steered generation."""

from __future__ import annotations

from typing import Final

from hypothesis import given
from hypothesis import strategies as st
from test_faults import BUDGET, disks, states, steps

from chryso_testbed.blkdebug import Expected
from chryso_testbed.faults import PROGRAM, Arm, Corrupt, Fault, Model, Restart, Step, trim
from chryso_testbed.outcomes import Status
from chryso_testbed.steer import (
    MAX_STEPS,
    SCENARIOS,
    advance,
    enabled,
    faults_to,
    gone,
    has_file,
    nominal,
    programs,
    reach,
    requestable,
    simulate,
    writes,
)

AIMING: Final = ("latent", "corrupt-heal", "write-fault")
names = st.sampled_from(sorted(SCENARIOS))


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
    if isinstance(step, Fault | Restart):
        assert step.driver in requestable(model)
        assert model.root.up(step.driver)


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
            hits.append(model.storage.damaged or model.expect(step).outcomes == {Status.PRESENT})
        model = advance(model, step)
    assert any(hits)
