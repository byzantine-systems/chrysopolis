"""Laws of lineage-driven fault selection. Planted defects caught:

| Defect | Caught by |
|---|---|
| Berge keeps non-minimal sets | test_hitting_sets_are_exactly_the_minimal_ones |
| the recovery cut settles the driver | test_the_model_predicts_every_cut_can_break_its_success |
| recovering never settles | test_an_unsettled_request_only_widens_the_next_step |
| an unsettled restart spends twice | test_an_unsettled_request_only_widens_the_next_step |
| the ring oracle ignores give-ups | test_rootlog.test_a_missing_event_is_noticed |
"""

from __future__ import annotations

from itertools import combinations
from typing import Final

from hypothesis import given
from hypothesis import strategies as st
from test_faults import BUDGET

from chryso_testbed.faults import PROGRAM, Model, Program, Step, trim
from chryso_testbed.lineage import (
    Atom,
    Budget,
    Instance,
    Recovered,
    Sectors,
    Support,
    candidates,
    cut,
    hitting_sets,
    lineage,
    succeeded,
)
from chryso_testbed.outcomes import Outcome, Status
from chryso_testbed.steer import SCENARIOS, advance, nominal, programs

UNIVERSE: Final[tuple[Atom, ...]] = (
    Instance("blk", 0), Instance("blk", 1), Recovered("blk", 1), Budget("blk"),
    Instance("eth", 0), Budget("eth"), Sectors("data", 0), Sectors("fat", 0),
)  # fmt: skip
FAILURES: Final = {Status.EIO, Status.ERROR, Status.UNREACHABLE}

atoms = st.sampled_from(UNIVERSE)
families = st.lists(st.frozensets(atoms, min_size=1, max_size=4), min_size=1, max_size=4)


def hits(candidate: frozenset[Atom], supports: list[Support]) -> bool:
    return all(candidate & s for s in supports)


@given(supports=families, size=st.integers(1, 3))
def test_hitting_sets_are_exactly_the_minimal_ones(supports: list[Support], size: int) -> None:
    """Berge's algorithm against enumerating every set up to `size`."""
    found = set(hitting_sets(supports, size))
    every = [
        frozenset(c)
        for k in range(size + 1)
        for c in combinations(UNIVERSE, k)
        if hits(frozenset(c), supports)
    ]
    minimal = {c for c in every if not any(o < c for o in every)}
    assert found == minimal
    assert all(hits(h, supports) for h in found)


@st.composite
def runs(draw: st.DrawFn) -> tuple[Program, list[tuple[Step, Outcome, Model]]]:
    """A steered program and its run under nominal outcomes."""
    model = Model.start(BUDGET)
    program = draw(programs(model, draw(st.sampled_from(sorted(SCENARIOS)))))
    trace = []
    for step in program:
        seen = nominal(model, step)
        trace.append((step, seen, model))
        model = advance(model, step)
    return program, trace


@given(run=runs())
def test_every_cut_only_inserts_valid_steps(
    run: tuple[Program, list[tuple[Step, Outcome, Model]]],
) -> None:
    program, trace = run
    for _, _, edited in candidates(program, trace):
        assert PROGRAM.validate_python(PROGRAM.dump_python(edited)) == edited
        assert trim(edited, BUDGET) == edited
        it = iter(edited)
        assert all(step in it for step in program)


@given(run=runs())
def test_the_model_predicts_every_cut_can_break_its_success(
    run: tuple[Program, list[tuple[Step, Outcome, Model]]],
) -> None:
    """Replaying the edited program up to the cut step, the model allows that
    step to fail."""
    program, trace = run
    for index, hit, edited in candidates(program, trace):
        step, _, before = trace[index]
        inserted, _ = cut(hit, step, before)
        target = index + len(inserted)
        assert edited[target] == step
        model = Model.start(BUDGET)
        for earlier in edited[:target]:
            model = advance(model, earlier)
        allowed = model.expect(step)
        assert allowed.anything or allowed.outcomes & FAILURES, (hit, allowed)


@given(run=runs())
def test_a_driver_with_budget_takes_two_faults_to_break(
    run: tuple[Program, list[tuple[Step, Outcome, Model]]],
) -> None:
    """Redundancy in time: while Root has budget for a driver, no single
    fault on it cuts a success, since Root would restart it."""
    _, trace = run
    for step, seen, before in trace:
        if not succeeded(step, seen):
            continue
        for hit in hitting_sets(lineage(step, before)):
            for d in {a.driver for a in hit if not isinstance(a, Sectors)}:
                if before.root.chart(d).has_budget():
                    assert sum(not isinstance(a, Sectors) and a.driver == d for a in hit) == 2
