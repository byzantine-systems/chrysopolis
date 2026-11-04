"""The disk-fault model: plans, their QEMU form, and what they let the guest report."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import EIO, PLAN, SCHEMA, Delay, Expected, Plan, WriteError, shaped

# Every key QEMU's QAPI BlkdebugInjectErrorOptions accepts.
QAPI_KEYS = {"event", "state", "iotype", "errno", "delay-ns", "sector", "once", "immediately"}

plans = from_schema(SCHEMA).map(PLAN.validate_python)
shapes = st.sampled_from(["clean", "transient", "any"])
delays = from_schema(Delay.model_json_schema()).map(Delay.model_validate)


@given(plan=plans)
def test_a_plan_round_trips_through_its_qemu_form(plan: Plan) -> None:
    """What QEMU receives validates back to the same plan, in QAPI names only."""
    dumped = PLAN.dump_python(plan)
    assert PLAN.validate_python(dumped) == plan
    assert all(rule.keys() <= QAPI_KEYS for rule in dumped)
    assert all("errno" in rule and "iotype" in rule for rule in dumped)


@given(plan=plans, shape=shapes)
def test_a_shaped_plan_stays_valid(plan: Plan, shape: str) -> None:
    """Shaping only drops rules or sets once, so the result still validates."""
    reshaped = shaped(plan, shape)
    assert PLAN.validate_python(PLAN.dump_python(reshaped)) == reshaped


@given(plan=plans)
def test_a_clean_plan_expects_everything_to_work(plan: Plan) -> None:
    assert Expected.of(shaped(plan, "clean")) == Expected(False, frozenset({"ok"}))


@given(plan=plans)
def test_a_transient_plan_never_expects_reads_to_fail_for_good(plan: Plan) -> None:
    """Without a permanent error some read outcome is ok."""
    assert "ok" in Expected.of(shaped(plan, "transient")).reads


@given(plan=plans, extra=st.lists(delays, max_size=3), at=st.integers(0, 4))
def test_delays_never_change_what_is_expected(plan: Plan, extra: list[Delay], at: int) -> None:
    """A delay succeeds after waiting, so adding one anywhere changes nothing
    the guest may report."""
    assert Expected.of((*plan[:at], *extra, *plan[at:])) == Expected.of(plan)


@given(plan=plans)
def test_a_permanent_write_error_fails_writes_and_reads(plan: Plan) -> None:
    assert Expected.of((*plan, WriteError(errno=EIO))) == Expected(True, frozenset({"error"}))
