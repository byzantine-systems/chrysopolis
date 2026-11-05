"""The disk-fault model: plans, their QEMU form, and what they let the guest report."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import (
    EIO,
    PLAN,
    SCHEMA,
    SHAPES,
    Delay,
    Expected,
    Plan,
    Shape,
    WriteError,
    shaped,
)

# Every key QEMU's QAPI BlkdebugInjectErrorOptions accepts.
QAPI_KEYS = {"event", "state", "iotype", "errno", "delay-ns", "sector", "once", "immediately"}

plans = from_schema(SCHEMA).map(PLAN.validate_python)
shapes = st.sampled_from(SHAPES)
delays = from_schema(Delay.model_json_schema()).map(Delay.model_validate)


@given(plan=plans)
def test_a_plan_round_trips_through_its_qemu_form(plan: Plan) -> None:
    """What QEMU receives validates back to the same plan, in QAPI names only."""
    dumped = PLAN.dump_python(plan)
    assert PLAN.validate_python(dumped) == plan
    assert all(rule.keys() <= QAPI_KEYS for rule in dumped)
    assert all("errno" in rule and "iotype" in rule for rule in dumped)


@given(plan=plans, shape=shapes)
def test_a_shaped_plan_stays_valid(plan: Plan, shape: Shape) -> None:
    """Shaping only drops rules or sets once, so the result still validates."""
    reshaped = shaped(plan, shape)
    assert PLAN.validate_python(PLAN.dump_python(reshaped)) == reshaped


@given(plan=plans)
def test_a_clean_plan_expects_everything_to_work(plan: Plan) -> None:
    assert Expected.of(shaped(plan, "clean")) == Expected(False, frozenset({"ok"}))


@given(plan=plans)
def test_a_single_plan_fails_one_write_and_reads_back(plan: Plan) -> None:
    """One one-shot error fails the write, and FatFs's retry makes later reads work."""
    assert Expected.of(shaped(plan, "single")) == Expected(True, frozenset({"ok"}))


@given(plan=plans, shape=shapes)
def test_shaping_twice_changes_nothing(plan: Plan, shape: Shape) -> None:
    """Every shape is a projection, so the fuzz draws each class directly."""
    assert shaped(shaped(plan, shape), shape) == shaped(plan, shape)


@given(plan=plans)
def test_a_transient_plan_never_expects_reads_to_fail_for_good(plan: Plan) -> None:
    """Without a permanent error some read outcome is ok."""
    assert "ok" in Expected.of(shaped(plan, "transient")).reads


@given(plan=plans, extra=st.lists(delays, max_size=3))
def test_delays_after_the_rules_change_nothing(plan: Plan, extra: list[Delay]) -> None:
    """A delay that comes after every rule never decides a write, so adding it
    changes nothing the guest may report."""
    assert Expected.of((*plan, *extra)) == Expected.of(plan)


def test_a_write_delay_before_a_write_error_lets_writes_through() -> None:
    """blkdebug's first matching rule decides (checked with qemu-io): a write
    delay ahead of a sticky write error lets writes succeed."""
    delay = Delay(errno=0, event="write_aio", iotype="write", delay_ns=0)
    expected = Expected.of((delay, WriteError(errno=EIO)))
    assert expected.reads == {"ok", "error"}


@given(plan=plans)
def test_a_permanent_write_error_fails_writes_and_reads(plan: Plan) -> None:
    assert Expected.of((WriteError(errno=EIO), *plan)) == Expected(True, frozenset({"error"}))
