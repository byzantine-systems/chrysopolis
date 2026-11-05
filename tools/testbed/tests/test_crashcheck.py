"""Laws of the durability oracle over write spans in a log. A planted
`durable` that trusts a write still in flight fails
test_a_finished_write_before_the_point_must_survive."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from chryso_testbed.crashcheck import Span, durable
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
        size = draw(st.integers(0, 4)) * 512
        outcome: Outcome = draw(st.sampled_from([Status.OK, Status.EIO]))
        spans.append(Span(step, outcome, at, at + size))
        at += size
    return spans


@given(spans=histories(), data=st.data())
def test_a_finished_write_before_the_point_must_survive(
    spans: list[Span], data: st.DataObject
) -> None:
    """The last write to each file that finished ok before the point, with
    nothing after it started yet, is what the crash must keep."""
    point = data.draw(st.integers(0, (spans[-1].end if spans else 512) + 512))
    promised = durable(spans, point)
    for file, seed in promised.items():
        mine = [
            (s.step, s)
            for s in spans
            if isinstance(s.step, Put | CrashPut) and s.step.file == file and s.start < point
        ]
        step, last = mine[-1]
        finished = isinstance(step, Put) and last.outcome == Status.OK and last.end <= point
        assert (seed is not None) == finished
        assert seed is None or seed == step.seed


@given(spans=histories())
def test_nothing_is_promised_before_the_log_starts(spans: list[Span]) -> None:
    assert durable(spans, 512) == {}
