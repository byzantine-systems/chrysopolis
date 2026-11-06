"""Laws of the durability oracle over write spans in a log. Planted defects:
a `durable` that trusts a write still in flight, or one that judges "started"
at the stable point rather than at the crash, which the fuzz found once crash
states kept part of an epoch, both fail
test_a_finished_write_before_the_stable_point_must_survive."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from chryso_testbed.crashcheck import Content, Span, allowed, durable
from chryso_testbed.faults import CrashPut, Put
from chryso_testbed.outcomes import Outcome, Status

files = st.integers(0, 3)
seeds = st.integers(0, 255)
writes = st.one_of(
    st.builds(Put, kind=st.just("put"), file=files, seed=seeds),
    st.builds(CrashPut, kind=st.just("crash-put"), file=files, seed=seeds, after_ms=st.just(0)),
)


@st.composite
def histories(draw: st.DrawFn) -> list[Span]:
    """Writes one after another in the log, each with an outcome."""
    spans, at = [], 512
    for step in draw(st.lists(writes, max_size=8)):
        # A put always logs a write. A crash may cut a crash-put before any.
        size = draw(st.integers(0 if isinstance(step, CrashPut) else 1, 4)) * 512
        outcome: Outcome = draw(st.sampled_from([Status.OK, Status.EIO]))
        spans.append(Span(step, outcome, at, at + size))
        at += size
    return spans


@given(spans=histories(), data=st.data())
def test_a_finished_write_before_the_stable_point_must_survive(
    spans: list[Span], data: st.DataObject
) -> None:
    """A file keeps the seed of its last write that started before the crash
    exactly when that write finished ok by the stable point."""
    end = (spans[-1].end if spans else 512) + 512
    stable = data.draw(st.integers(0, end))
    crash = data.draw(st.integers(stable, end))
    promised = durable(spans, stable, crash)
    written = [(s.step, s) for s in spans if isinstance(s.step, Put | CrashPut)]
    for file in {step.file for step, _ in written}:
        started = [(step, s) for step, s in written if step.file == file and s.start < crash]
        if not started:
            assert file not in promised
            continue
        step, last = started[-1]
        kept = isinstance(step, Put) and last.outcome == Status.OK and last.end <= stable
        assert promised[file] == (step.seed if kept else None)


@given(spans=histories())
def test_nothing_is_promised_before_the_log_starts(spans: list[Span]) -> None:
    assert durable(spans, 512) == {}


@given(spans=histories(), data=st.data())
def test_a_later_crash_never_promises_more(spans: list[Span], data: st.DataObject) -> None:
    """Moving the crash later, with the stable point fixed, only takes promises
    away: more writes may be in flight, none more finished."""
    end = (spans[-1].end if spans else 512) + 512
    stable = data.draw(st.integers(0, end))
    early = data.draw(st.integers(stable, end))
    late = data.draw(st.integers(early, end))
    before, after = durable(spans, stable, early), durable(spans, stable, late)
    assert {f for f, s in after.items() if s is not None} <= {
        f for f, s in before.items() if s is not None
    }


def may(*contents: Content) -> frozenset[Content]:
    return frozenset(contents)


def put(file: int, seed: int, start: int, end: int, outcome: Outcome = Status.OK) -> Span:
    return Span(Put(kind="put", file=file, seed=seed), outcome, start, end)


def test_allowed_contents_anchor_the_meaning() -> None:
    """A stable write is what the file holds. A rewrite in flight leaves it as
    before, empty or new. A first write in flight leaves no entry, an empty
    one or the new file. A failed write leaves anything."""
    first, again = put(1, 5, 512, 1024), put(1, 6, 1024, 2048)
    assert allowed([first], 1024) == {1: may(5)}
    assert allowed([first, again], 1024, 1536) == {1: may(5, "empty", 6)}
    assert allowed([put(2, 7, 512, 1024)], 600, 700) == {2: may("absent", "empty", 7)}
    assert allowed([put(3, 8, 512, 1024, Status.EIO)], 2048) == {3: None}
    failed_then_ok = [put(3, 8, 512, 1024, Status.EIO), put(3, 9, 1024, 2048)]
    assert allowed(failed_then_ok, 2048) == {3: may(9)}


def covers(wide: frozenset[Content] | None, narrow: frozenset[Content] | None) -> bool:
    """`wide` allows everything `narrow` does. None allows anything."""
    return wide is None or (narrow is not None and narrow <= wide)


@given(spans=histories(), data=st.data())
def test_a_later_crash_only_widens_what_a_file_may_hold(
    spans: list[Span], data: st.DataObject
) -> None:
    """With the stable point fixed, a later crash keeps every content an
    earlier one allowed: the writes it adds are in flight, never finished."""
    end = (spans[-1].end if spans else 512) + 512
    stable = data.draw(st.integers(0, end))
    early = data.draw(st.integers(stable, end))
    late = data.draw(st.integers(early, end))
    before, after = allowed(spans, stable, early), allowed(spans, stable, late)
    for file, may in before.items():
        assert covers(after[file], may)
