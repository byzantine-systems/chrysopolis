"""The smokes' programs against the model. A smoke shows a fix only if the
model makes its claim strict: an overwrite allowed to fail, or a remove or
rename allowed to be refused, would let the smoke pass on the old system.
Planted defects: an `Overwrite` that inherits a get's read-fault expectation
fails test_the_latent_overwrite_must_succeed, and a want of a gone driver that
may reach Root fails test_the_orch_driver_program_is_strict."""

from __future__ import annotations

from chryso_testbed.allowed import Allowed
from chryso_testbed.faults import Get, Halt, Model, Program, Put, Want, boot_profile, expect
from chryso_testbed.outcomes import Booted, Condition, GaveUp, Outcome, Restarted, Seed, Status
from chryso_testbed.scenarios.orch import ORCH_BOOT, orch_driver
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


def test_the_orch_driver_program_is_strict() -> None:
    """Every step of the orchestrator smoke allows exactly one outcome, so the
    smoke cannot pass on a system that does something else. The table is the
    model's fold of the program, with the parts that carry the orchestrator's
    laws pinned by hand."""
    parts = dict(orch_driver(BUDGET))
    folded = expectations(tuple(step for _, program in orch_driver(BUDGET) for step in program))
    assert all(a.only() is not None for a in folded)
    ready = expect(Condition("ready", "settled"))
    offset = len(parts["fault"]) + len(parts["ready"])
    want = folded[offset : offset + len(parts["want"])]
    assert want == [expect(Restarted("ctl-restart", 1)), expect(Status.UNASKED), ready]
    gone = folded[offset + len(parts["want"]) : offset + len(parts["want"]) + len(parts["gone"])]
    assert gone[BUDGET:] == [
        expect(Condition("degraded", "low_budget")),
        expect(GaveUp("budget-exhausted")),
        expect(Condition("gone", "gave_up")),
        expect(Status.UNASKED),
        expect(Condition("gone", "gave_up")),
    ]
    assert folded[-1] == expect(Restarted("ctl-restart", 2))


def test_a_want_settles_its_driver() -> None:
    """Law 1 makes a want a settled restart, so a read right after a want of
    the block driver must succeed, not be allowed to fail with `eio`."""
    _, want, get = expectations(
        (Put(kind="put", file=1, seed=4), Want(kind="want", driver="blk"), Get(kind="get", file=1))
    )
    assert want == expect(Restarted("ctl-restart", 1))
    assert get == expect(Seed(4))


def test_orch_restart_smoke_halts_into_safe_mode() -> None:
    """orch-restart-smoke on orch-image (BEAM budget 64, safe_below 62): the
    first incarnation boots orch, a halt keeps it orch, a want after it is the
    one ctl-restart, and the second halt boots start."""
    model = Model.start(BUDGET, boot=ORCH_BOOT)
    assert boot_profile(model.orch) == "orch"
    seen: list[Allowed[Outcome]] = []
    for step in (Halt(kind="halt"), Want(kind="want", driver="timer"), Halt(kind="halt")):
        allowed = model.expect(step)
        seen.append(allowed)
        model = model.after(step, nominal(model, step))
    assert seen == [
        expect(Booted("orch")),
        expect(Restarted("ctl-restart", 1)),
        expect(Booted("start")),
    ]
