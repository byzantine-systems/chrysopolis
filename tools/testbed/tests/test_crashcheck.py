"""Laws of the durability oracle over write spans in a log. Planted defects:
a `durable` that trusts a write still in flight, or one that judges "started"
at the stable point rather than at the crash, which the fuzz found once crash
states kept part of an epoch, both fail
test_a_finished_write_before_the_stable_point_must_survive. An `allowed` that
drops "absent" for a remove in flight fails
test_an_unfinished_remove_may_leave_the_file_or_nothing, and a `moves` that
keeps a rename's joint rule after a later step on its names fails
test_renames_anchor_the_meaning."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from chryso_testbed.crashcheck import Content, Move, Span, allowed, durable, moves
from chryso_testbed.faults import CrashPut, Overwrite, Put, Remove, Rename, Step
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


# --- overwrite, remove and rename ---------------------------------------------------

settled = st.one_of(
    st.builds(Put, kind=st.just("put"), file=files, seed=seeds),
    st.builds(Overwrite, kind=st.just("overwrite"), file=files, seed=seeds),
    st.builds(Remove, kind=st.just("remove"), file=files),
    st.builds(Rename, kind=st.just("rename"), src=files, dst=files),
)
changes = st.one_of(settled, writes)
refused = st.sampled_from([Status.OK, Status.OK, Status.OK, Status.ENOENT, Status.EEXIST])
outcomes = st.sampled_from(
    [Status.OK, Status.OK, Status.OK, Status.ENOENT, Status.EEXIST, Status.EIO]
)


@st.composite
def change_histories(
    draw: st.DrawFn,
    steps: st.SearchStrategy[Step] = changes,
    results: st.SearchStrategy[Outcome] = outcomes,
) -> list[Span]:
    """Writes, overwrites, removes and renames one after another in the log."""
    spans, at = [], 512
    for step in draw(st.lists(steps, max_size=8)):
        size = draw(st.integers(0 if isinstance(step, CrashPut) else 1, 4)) * 512
        spans.append(Span(step, draw(results), at, at + size))
        at += size
    return spans


def replayed(spans: list[Span]) -> dict[int, Content]:
    """A plain sequential replay of ok steps, the oracle for a crash with
    nothing in flight."""
    held: dict[int, Content] = {}
    for span in spans:
        if span.outcome != Status.OK:
            continue
        match span.step:
            case Put(file=f, seed=s) | Overwrite(file=f, seed=s):
                held[f] = s
            case Remove(file=f):
                held[f] = "absent"
            case Rename(src=s, dst=d) if s != d:
                held[d], held[s] = held.get(s, "absent"), "absent"
    return held


@given(spans=change_histories(settled, refused))
def test_with_nothing_in_flight_a_crash_keeps_exactly_the_replay(spans: list[Span]) -> None:
    """Every step done by the stable point: each file holds what replaying the
    ok steps in order leaves, and no rename is cut."""
    stable = spans[-1].end if spans else 512
    found = allowed(spans, stable)
    assert dict(found) == {f: frozenset({c}) for f, c in replayed(spans).items()}
    assert moves(spans, stable) == ()


@given(spans=change_histories(), data=st.data())
def test_a_later_crash_only_widens_what_any_step_leaves(
    spans: list[Span], data: st.DataObject
) -> None:
    end = (spans[-1].end if spans else 512) + 512
    stable = data.draw(st.integers(0, end))
    early = data.draw(st.integers(stable, end))
    late = data.draw(st.integers(early, end))
    before, after = allowed(spans, stable, early), allowed(spans, stable, late)
    for file, may in before.items():
        assert covers(after[file], may)


@given(spans=change_histories(), file=files, data=st.data())
def test_an_unfinished_remove_may_leave_the_file_or_nothing(
    spans: list[Span], file: int, data: st.DataObject
) -> None:
    """A remove the crash cut may or may not have reached the disk."""
    start = spans[-1].end if spans else 512
    # A flush ends a span, so stable points fall on span boundaries.
    stable = data.draw(st.sampled_from([512, *(s.end for s in spans)]))
    remove = Span(Remove(kind="remove", file=file), Status.OK, start, start + 512)
    before = allowed(spans, stable, start).get(file, may("absent"))
    after = allowed([*spans, remove], stable, start + 1)[file]
    assert covers(after, before)
    assert covers(after, may("absent"))


def test_renames_anchor_the_meaning() -> None:
    """A stable rename moves the file. One the crash cut leaves it under
    either name, or both, and `moves` names it for the joint rule."""
    first = put(1, 5, 512, 1024)
    rename = Span(Rename(kind="rename", src=1, dst=2), Status.OK, 1024, 1536)
    assert allowed([first, rename], 1536) == {1: may("absent"), 2: may(5)}
    assert allowed([first, rename], 1024, 1100) == {1: may(5, "absent"), 2: may("absent", 5)}
    assert moves([first, rename], 1024, 1100) == (Move(1, 2, may(5)),)
    refused = Span(Rename(kind="rename", src=1, dst=2), Status.EEXIST, 1024, 1536)
    assert allowed([first, refused], 1536) == {1: may(5)}
    later = Span(Remove(kind="remove", file=2), Status.OK, 1536, 2048)
    assert moves([first, rename, later], 1024, 1600) == (), "a later step voids the joint rule"


def test_an_unfinished_overwrite_may_leave_part_of_each() -> None:
    first = put(1, 5, 512, 1024)
    over = Span(Overwrite(kind="overwrite", file=1, seed=6), Status.OK, 1024, 2048)
    assert allowed([first, over], 2048) == {1: may(6)}
    assert allowed([first, over], 1024, 1536) == {1: may(5, 6, "garbage")}
