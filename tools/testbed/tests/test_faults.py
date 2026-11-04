"""Fault programs and the model that judges them.

A run is simulated by drawing each step's outcome from what the model
allows, so these properties hold for every run the real fuzz can accept.
"""

from __future__ import annotations

from collections import Counter
from typing import Final

from hypothesis import given
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import PLAN, Expected
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.faults import (
    KEPT,
    PROGRAM,
    STEP,
    Expect,
    Fault,
    Get,
    Model,
    Program,
    Put,
    Restart,
    Step,
    trim,
)
from chryso_testbed.inject import get_outcome, put_outcome

BUDGET: Final = 3
# Outcomes the interpreter can name, for steps whose model admits anything.
UNIVERSE: Final = ("ok", "eio", "error", "enoent", "differs", "seed:0", "seed:255")

programs = st.lists(from_schema(STEP.json_schema()).map(STEP.validate_python), max_size=12).map(
    tuple
)
disks = from_schema(PLAN_SCHEMA).map(PLAN.validate_python).map(Expected.of)


def outcome(allowed: Expect) -> st.SearchStrategy[str]:
    return st.sampled_from(UNIVERSE if allowed.anything else sorted(allowed.outcomes))


@st.composite
def runs(draw: st.DrawFn) -> list[tuple[Step, Expect, str, Model]]:
    """A program, and for each step what was allowed, what happened and the
    model before it."""
    model = Model(BUDGET, draw(disks))
    trace = []
    for step in draw(programs):
        allowed = model.expect(step)
        seen = draw(outcome(allowed))
        trace.append((step, allowed, seen, model))
        model = model.after(step, seen)
    return trace


def requests(trace: list[tuple[Step, Expect, str, Model]]) -> dict[str, list[str]]:
    """Each driver's outcomes, in order."""
    seen: dict[str, list[str]] = {}
    for step, _, result, _ in trace:
        if isinstance(step, Fault | Restart):
            seen.setdefault(step.driver, []).append(result)
    return seen


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


@given(trace=runs())
def test_every_step_allows_something(trace: list[tuple[Step, Expect, str, Model]]) -> None:
    assert all(allowed.anything or allowed.outcomes for _, allowed, _, _ in trace)


@given(trace=runs())
def test_restarts_count_up_then_give_up_once_then_stay_gone(
    trace: list[tuple[Step, Expect, str, Model]],
) -> None:
    """Per driver: restarts numbered 1 to the budget, then one give-up, then
    gone forever. Faults and debug restarts share the count."""
    for outcomes in requests(trace).values():
        restarts = [o for o in outcomes if o.partition(":")[0] in {"restart", "debug-restart"}]
        assert [int(o.partition(":")[2]) for o in restarts] == list(range(1, len(restarts) + 1))
        expected = (
            restarts
            + ["giveup:budget-exhausted"] * (len(outcomes) > BUDGET)
            + ["gone"] * max(0, len(outcomes) - BUDGET - 1)
        )
        assert outcomes == expected


@given(trace=runs())
def test_a_restart_takes_the_path_its_request_names(
    trace: list[tuple[Step, Expect, str, Model]],
) -> None:
    for step, _, result, _ in trace:
        kind = result.partition(":")[0]
        if kind in {"restart", "debug-restart"}:
            assert kind == ("restart" if isinstance(step, Fault) else "debug-restart")


@given(trace=runs())
def test_a_file_written_ok_reads_back_until_something_disturbs_it(
    trace: list[tuple[Step, Expect, str, Model]],
) -> None:
    """With the disk up and no failed write, a read returns the last seed
    written to that file, or enoent if none was."""
    for step, allowed, _, before in trace:
        if not isinstance(step, Get) or "blk" in before.gone or before.suspect:
            continue
        if step.file not in before.files:
            assert allowed == Expect(frozenset({"enoent"}))
        elif (seed := before.files[step.file]) is not None:
            assert allowed == Expect(frozenset({f"seed:{seed}"}))


@given(disk=disks, seed=st.integers(0, 255), file=st.integers(0, 3))
def test_a_disk_that_takes_writes_reads_back_what_was_written(
    disk: Expected, seed: int, file: int
) -> None:
    """A put the model allows to work, once it works, is what the next get must return."""
    put, get = Put(kind="put", file=file, seed=seed), Get(kind="get", file=file)
    model = Model(BUDGET, disk)
    assert ("ok" in model.expect(put)) == (disk.reads != {"error"} or not disk.write_fails)
    if "ok" in model.expect(put):
        assert model.after(put, "ok").expect(get) == Expect(frozenset({f"seed:{seed}"}))


steps = st.sampled_from(["ok", "{error,eio}", "{error,enospc}", "{error,enosys}"])


@given(write=steps, sync=steps, close=steps)
def test_a_put_is_ok_only_when_every_step_is(write: str, sync: str, close: str) -> None:
    named = put_outcome(f"{{{write},{sync},{close}}}")
    errors = {write, sync, close} - {"ok"}
    assert named == ("ok" if not errors else "eio" if errors == {"{error,eio}"} else "error")


@given(seed=st.integers(0, 255), reason=st.from_regex(r"[a-z]{1,8}", fullmatch=True))
def test_get_outcomes_name_seeds_and_errors(seed: int, reason: str) -> None:
    assert get_outcome(f"{{ok,{seed}}}") == f"seed:{seed}"
    assert get_outcome(f"{{error,{reason}}}") in {"enoent", "eio", "error"}
    assert (get_outcome(f"{{error,{reason}}}") == "error") == (reason not in {"enoent", "eio"})
