"""The frame reader against the Erlang golden vectors, plus its laws.

Each vector must read the same whole and at every split point. A control run
passes and planted damage is refused. feed() must equal the bytewise
reference.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from chryso_testbed import frames

RUN = "V.RUN"
# Deterministic, with no example database in the read-only sandbox.
LAW = settings(derandomize=True, database=None, deadline=None, max_examples=300)


def vectors() -> Path:
    root = os.environ.get("CHRYSO_FRAME_VECTORS")
    if root is None:
        pytest.fail("CHRYSO_FRAME_VECTORS is not set")
    return Path(root)


def manifest() -> list[str]:
    lines = (vectors() / "manifest.txt").read_text().splitlines()
    if not lines or lines[0] != "chryso-frame-vectors 1":
        pytest.fail(f"unexpected manifest header {lines[:1]!r}")
    names, count = lines[1:-1], lines[-1]
    if count != f"count {len(names)}":
        pytest.fail(f"manifest count {count!r} disagrees with {len(names)} names")
    return names


def log(name: str) -> bytes:
    return (vectors() / f"{name}.log").read_bytes()


def as_json(event: frames.Event) -> Any:
    """An event in the vector's JSON shape."""
    match event:
        case frames.Frame(run=run, n=n, kind=kind, fields=fs):
            return {"frame": {"run": run, "n": n, "kind": kind, "fields": [list(f) for f in fs]}}
        case frames.Damaged(run=run, reason=reason):
            return {"damaged": {"run": run, "reason": reason.value}}
        case frames.Pending():
            return "pending"


def verdict_json(result: frames.Verdict) -> Any:
    match result:
        case frames.Pending():
            return "pending"
        case frames.Pass(frames=got, unexpected=unexpected):
            return {"pass": {"frames": len(got), "unexpected": unexpected}}
        case frames.Fail(kind="damaged", run=run, detail=reason):
            return {"fail": {"kind": "damaged", "run": run, "reason": reason}}
        case frames.Fail(kind="lost", run=run, detail=detail):
            missing = [int(part.removeprefix("n=")) for part in detail.split()]
            return {"fail": {"kind": "lost", "run": run, "missing": missing}}


def read_split(data: bytes, cuts: list[int], feed: Any = frames.feed) -> list[frames.Event]:
    state: frames.State = frames.START
    events: list[frames.Event] = []
    start = 0
    for cut in [*sorted(set(cuts)), len(data)]:
        out, state = feed(data[start:cut], state)
        events.extend(out)
        start = cut
    return events + frames.finish(state)


# --- golden vectors ------------------------------------------------------------------


def test_manifest_is_large_enough() -> None:
    assert len(manifest()) >= 60


@pytest.mark.parametrize("name", manifest() if "CHRYSO_FRAME_VECTORS" in os.environ else [])
def test_vector(name: str) -> None:
    expect = json.loads((vectors() / f"{name}.expect.json").read_text())
    data = log(name)
    events = frames.collect(data)
    assert [as_json(e) for e in events] == expect["events"]
    assert verdict_json(frames.observe(events, RUN)) == expect["observe"]
    assert read_split(data, [], frames.feed_bytewise) == events


@pytest.mark.parametrize("name", manifest() if "CHRYSO_FRAME_VECTORS" in os.environ else [])
def test_vector_every_split(name: str) -> None:
    data = log(name)
    whole = frames.collect(data)
    step = 1 if len(data) <= 400 else max(1, len(data) // 97)
    for cut in range(0, len(data) + 1, step):
        assert read_split(data, [cut]) == whole, cut


# --- planted damage --------------------------------------------------------------------


def test_control_then_planted() -> None:
    good = log("good")
    assert isinstance(frames.require(frames.collect(good), RUN), frames.Pass)

    at = good.index(b"|crc=") + len(b"|crc=")
    digit = b"1" if good[at : at + 1] == b"0" else b"0"
    flipped = good[:at] + digit + good[at + 1 :]
    with pytest.raises(frames.FrameError, match=r"^damaged V\.RUN crc$"):
        frames.require(frames.collect(flipped), RUN)


@pytest.mark.parametrize("fragment", [b"rx=128", b"|oktx=0\r\n", b"rx"])
def test_driver_fragment_refused(fragment: bytes) -> None:
    good = log("good")
    at = good.index(b"fresh") + 2
    with pytest.raises(frames.FrameError, match=r"^damaged V\.RUN (syntax|length)$"):
        frames.require(frames.collect(good[:at] + fragment + good[at:]), RUN)


def test_text_boundary() -> None:
    # The console arrives as str. Undecodable bytes must still read.
    text = log("good").decode("ascii") + "\udcff\r\n"
    assert frames.collect(frames.as_bytes(text)) == frames.collect(log("good"))


# --- laws ------------------------------------------------------------------------------

NOISE = st.binary(max_size=24).map(lambda b: b.replace(b"@", b""))
SAMPLES = ["good", "good_noisy", "truncated_then_next", "splice_pr81", "oversize"]
CONSOLE = st.lists(
    st.one_of(st.sampled_from(SAMPLES).map(log), NOISE, st.binary(max_size=64)),
    max_size=6,
).map(b"".join)


@LAW
@given(st.binary(max_size=2048))
def test_total(data: bytes) -> None:
    events = frames.collect(data)
    assert frames.Pending() not in events[:-1]


@LAW
@given(CONSOLE, st.lists(st.integers(0, 4000)))
def test_fast_feed_equals_reference(data: bytes, cuts: list[int]) -> None:
    cuts = [c for c in cuts if c <= len(data)]
    assert read_split(data, cuts) == read_split(data, [], frames.feed_bytewise)


@LAW
@given(st.lists(NOISE, max_size=4))
def test_noise_between_frames(noise: list[bytes]) -> None:
    lines = log("good").splitlines(keepends=True)
    padded = [*noise, *([b""] * len(lines))]
    mixed = b"".join(line + extra for line, extra in zip(lines, padded, strict=False))
    assert frames.collect(mixed) == frames.collect(log("good"))


@LAW
@given(NOISE.filter(bool), st.integers(0, 10_000))
def test_splice_never_accepted(segment: bytes, where: int) -> None:
    good = log("good")
    start = good.index(frames.MARKER) + len(frames.MARKER)
    end = good.index(b"@@", start)
    at = start + where % (end - start + 1)
    events = frames.collect(good[:at] + segment + good[at:])
    clean = frames.collect(good)
    assert all(e in clean for e in events if isinstance(e, frames.Frame))
    damaged = [e for e in events if isinstance(e, frames.Damaged)]
    assert len(damaged) == 1
    assert damaged[0].reason in (frames.Reason.SYNTAX, frames.Reason.LENGTH)


@LAW
@given(CONSOLE, st.integers(0, 4000))
def test_prefix_never_decides_differently(data: bytes, cut: int) -> None:
    short = frames.observe(frames.collect(data[:cut]), RUN)
    assert short == frames.Pending() or short == frames.observe(frames.collect(data), RUN)
