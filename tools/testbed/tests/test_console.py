"""Console waits, failure patterns, frames and the incremental scan."""

from __future__ import annotations

import asyncio
import re
import zlib

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from chryso_testbed.console import Console, Scan, ScenarioFailure

LAW = settings(derandomize=True, database=None, deadline=None, max_examples=300)


def frame(run: str, n: int, kind: str, fields: str) -> bytes:
    prefix = f"{run}|n={n}|k={kind}|{fields}".encode()
    covered = prefix + b"|len=%d" % len(prefix)
    return b"@@CHRYSO|" + covered + b"|crc=%08x@@\r\n" % zlib.crc32(covered)


async def later(console: Console, *chunks: bytes) -> None:
    for chunk in chunks:
        await asyncio.sleep(0)
        console.append(chunk)
        async with console._changed:
            console._changed.notify_all()


def test_wait_sees_a_line_that_arrives_later() -> None:
    async def scenario() -> str:
        console = Console()
        feeder = asyncio.create_task(later(console, b"boot\r\nEshell V", b"16.1\r\n"))
        async with asyncio.timeout(5):
            hit = await console.wait(r"Eshell V([\d.]+)")
        await feeder
        return hit.groups[0] or ""

    assert asyncio.run(scenario()) == "16.1"


def test_failure_pattern_fails_fast() -> None:
    async def scenario() -> None:
        console = Console([r"MON\|ERROR"])
        feeder = asyncio.create_task(later(console, b"MON|ERROR fault\r\n"))
        await console.wait("never")
        await feeder

    with pytest.raises(ScenarioFailure, match=re.escape("failure 'MON|ERROR")):
        asyncio.run(asyncio.wait_for(scenario(), 5))


def test_deadline_records_the_interrupted_wait() -> None:
    console = Console()

    async def scenario() -> None:
        console.append(b"last words\r\n")
        async with asyncio.timeout(0.05):
            await console.wait("never")

    with pytest.raises(TimeoutError):
        asyncio.run(scenario())
    assert console.interrupted == "'never'"
    assert not console.waiting


def test_wait_count_counts_after_the_mark() -> None:
    async def scenario() -> int:
        console = Console()
        console.append(b"Eshell\r\n")
        mark = console.mark()
        feeder = asyncio.create_task(later(console, b"Eshell\r\n", b" Esh", b"ell\r\n"))
        async with asyncio.timeout(5):
            hits = await console.wait_count("Eshell", 2, since=mark)
        await feeder
        return len(hits)

    assert asyncio.run(scenario()) == 2


def test_await_run_passes_and_fails_on_damage() -> None:
    async def passing() -> int:
        console = Console()
        mark = console.mark()
        run = frame("R", 0, "step", "x=1") + frame("R", 1, "verdict", "frames=1|unexpected=0")
        feeder = asyncio.create_task(later(console, run[:20], run[20:]))
        async with asyncio.timeout(5):
            result = await console.await_run("R", since=mark)
        await feeder
        return len(result.frames)

    async def damaged() -> None:
        console = Console()
        mark = console.mark()
        good = frame("R", 0, "step", "x=1")
        console.append(good[:15] + b"rx=128" + good[15:])
        await console.await_run("R", since=mark)

    assert asyncio.run(passing()) == 1
    with pytest.raises(ScenarioFailure, match="damaged R"):
        asyncio.run(asyncio.wait_for(damaged(), 5))


def test_follow_yields_everything_then_stops() -> None:
    async def scenario() -> bytes:
        console = Console()
        reader = asyncio.StreamReader()
        reader.feed_data(b"one ")
        reader.feed_data(b"two")
        reader.feed_eof()
        pump = asyncio.create_task(console.pump(reader))
        got = b"".join([chunk async for chunk in console.follow()])
        await pump
        return got

    assert asyncio.run(asyncio.wait_for(scenario(), 5)) == b"one two"


LINES = st.lists(
    st.sampled_from([b"ab12", b"xab3y", b"noise", b"", b"ab", b"9ab9ab7"]), max_size=12
).map(lambda ls: b"\r\n".join(ls))


@LAW
@given(LINES, st.lists(st.integers(0, 200)))
def test_scan_equals_whole_search(log: bytes, cuts: list[int]) -> None:
    """The incremental scan finds what one search over the whole log finds."""
    scan = Scan(r"ab\d+", 0)
    buffer = bytearray()
    start = 0
    for cut in [*sorted({c for c in cuts if c <= len(log)}), len(log)]:
        buffer += log[start:cut]
        start = cut
        scan.advance(buffer)
    expected = [m.start() for m in re.finditer(rb"ab\d+", log)]
    assert [h.start for h in scan.hits] == expected
