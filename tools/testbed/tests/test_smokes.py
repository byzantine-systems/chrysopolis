"""The smokes' programs against the model. A smoke shows a fix only if the
model makes its claim strict: an overwrite allowed to fail, or a remove or
rename allowed to be refused, would let the smoke pass on the old system.
Planted defect: an `Overwrite` that inherits a get's read-fault expectation
fails test_the_latent_overwrite_must_succeed."""

from __future__ import annotations

from chryso_testbed.allowed import Allowed
from chryso_testbed.faults import Model, Program, expect
from chryso_testbed.outcomes import Outcome, Seed, Status
from chryso_testbed.scenarios.restart import AFTER_RECOVERY, LATENT_OVERWRITE, RENAME, UNLINK
from chryso_testbed.steer import nominal

BUDGET = 3


def expectations(program: Program) -> list[Allowed[Outcome]]:
    """What the model allows at each step, run with its nominal outcomes."""
    model, allowed = Model.start(BUDGET), []
    for step in program:
        allowed.append(model.expect(step))
        model = model.after(step, nominal(model, step))
    return allowed


def test_the_latent_overwrite_must_succeed() -> None:
    _, arm, overwrite, _, get = expectations(LATENT_OVERWRITE)
    assert arm == expect(Status.PRESENT)
    assert overwrite == expect(Status.OK)
    assert get == expect(Seed(6))


def test_the_crash_smokes_start_from_a_held_file() -> None:
    """Before the crash point the remove and rename must succeed outright, so
    their crash states straddle a real change."""
    for program in (UNLINK, RENAME):
        put, change = expectations(program)
        assert put == expect(Status.OK)
        assert change == expect(Status.OK)


def test_after_recovery_only_the_new_name_holds_the_file() -> None:
    """On the booted crash image the old name must be gone and the new one
    must take a write. A chain both names still shared would read the new
    seed back under the old name, which the model does not allow."""
    put, old, new = expectations(AFTER_RECOVERY)
    assert put == expect(Status.OK)
    assert old == expect(Status.ENOENT)
    assert new == expect(Seed(7))
