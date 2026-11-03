"""The guest's serial console as an event-driven log.

A pump task appends what QEMU prints and wakes every waiter, so a wait ends
as soon as its line arrives. Every wait also watches the failure patterns,
so a fault ends the scenario at once instead of at its timeout.

Patterns match within one line. Each scan resumes at the start of the last
incomplete line, so a wake costs only the new bytes.

The console echoes typed lines, so a pattern must never match text the
scenario sends.
"""

from __future__ import annotations

import asyncio
import re
from bisect import bisect_left
from collections.abc import AsyncIterator, Callable, Iterable
from operator import attrgetter
from typing import Final, NamedTuple

from chryso_testbed import frames

TAIL: Final = 4000
CHUNK: Final = 4096


class ScenarioFailure(AssertionError):
    """A scenario expectation failed. The message ends with the console tail."""


class Mark(NamedTuple):
    """A console position: bytes seen and frame events seen."""

    offset: int
    event: int


class Hit(NamedTuple):
    start: int
    end: int
    text: str
    groups: tuple[str | None, ...]

    @classmethod
    def of(cls, m: re.Match[bytes]) -> Hit:
        return cls(
            m.start(),
            m.end(),
            m.group().decode("utf-8", "replace"),
            tuple(g.decode("utf-8", "replace") if g is not None else None for g in m.groups()),
        )


_START: Final = attrgetter("start")


def compile_pattern(pattern: str) -> re.Pattern[bytes]:
    return re.compile(pattern.encode(), re.MULTILINE)


class Scan:
    """Incremental search for one pattern over a growing log."""

    __slots__ = ("_pos", "_regex", "hits")

    def __init__(self, pattern: str, start: int) -> None:
        self._regex = compile_pattern(pattern)
        self._pos = start
        self.hits: list[Hit] = []

    def advance(self, log: bytearray) -> list[Hit]:
        """Every hit so far. Hits on the incomplete last line are found again next time."""
        del self.hits[bisect_left(self.hits, self._pos, key=_START) :]
        self.hits.extend(Hit.of(m) for m in self._regex.finditer(log, self._pos))
        self._pos = log.rfind(b"\n", self._pos) + 1 or self._pos
        return self.hits


class Console:
    def __init__(self, failures: Iterable[str] = ()) -> None:
        self._log = bytearray()
        self._events: list[frames.Event] = []
        self._state: frames.State = frames.START
        self._failures = [Scan(p, 0) for p in failures]
        self._fault: Hit | None = None
        self._closed = False
        self._changed = asyncio.Condition()
        self._writer: asyncio.StreamWriter | None = None
        self.waiting: set[str] = set()
        self.interrupted: str | None = None

    # --- input -------------------------------------------------------------------

    def append(self, chunk: bytes) -> None:
        """Add console bytes. Waiters see them at their next wake."""
        self._log += chunk
        events, self._state = frames.feed(chunk, self._state)
        self._events += events
        if self._fault is None:
            found = (hits[0] for scan in self._failures if (hits := scan.advance(self._log)))
            self._fault = next(found, None)

    async def pump(self, reader: asyncio.StreamReader) -> None:
        """Copy QEMU's serial output into the log until EOF."""
        try:
            while chunk := await reader.read(CHUNK):
                self.append(chunk)
                async with self._changed:
                    self._changed.notify_all()
        finally:
            self._closed = True
            async with self._changed:
                self._changed.notify_all()

    async def report(self, text: str) -> None:
        """Fail every current and later wait with a fault seen outside the console."""
        if self._fault is None:
            self._fault = Hit(len(self._log), len(self._log), text, ())
        async with self._changed:
            self._changed.notify_all()

    def attach(self, writer: asyncio.StreamWriter) -> None:
        self._writer = writer

    async def send(self, line: str) -> None:
        """Type one line at the guest console, ending it with CR."""
        if self._writer is None:
            raise RuntimeError("console has no input attached")
        self._writer.write(line.encode() + b"\r")
        await self._writer.drain()

    # --- observation ---------------------------------------------------------------

    def mark(self) -> Mark:
        return Mark(len(self._log), len(self._events))

    def raw(self) -> bytes:
        return bytes(self._log)

    def tail(self, size: int = TAIL) -> str:
        return self._log[-size:].decode("utf-8", "replace")

    def findall(self, pattern: str, since: Mark | None = None) -> list[Hit]:
        start = since.offset if since else 0
        return [Hit.of(m) for m in compile_pattern(pattern).finditer(self._log, start)]

    def search(self, pattern: str, since: Mark | None = None) -> Hit | None:
        m = compile_pattern(pattern).search(self._log, since.offset if since else 0)
        return Hit.of(m) if m else None

    async def follow(self, start: int = 0) -> AsyncIterator[bytes]:
        """Yield console bytes from `start` as they arrive, until EOF."""
        pos = start
        while await self._grown(pos):
            chunk = bytes(self._log[pos:])
            pos += len(chunk)
            yield chunk

    async def _grown(self, pos: int) -> bool:
        """Wait for bytes past `pos`. False once the console closed with none."""
        async with self._changed:
            await self._changed.wait_for(lambda: len(self._log) > pos or self._closed)
        return len(self._log) > pos

    def fail(self, message: str) -> ScenarioFailure:
        return ScenarioFailure(f"{message}\n--- console tail ---\n{self.tail()}")

    # --- waiting -------------------------------------------------------------------
    # Callers bound waits with asyncio.timeout. `interrupted` keeps the wait a
    # deadline cut short, so the report can say what never appeared.

    async def wait(self, pattern: str, *, since: Mark | None = None) -> Hit:
        """The first match at or after `since`."""
        scan = Scan(pattern, since.offset if since else 0)
        hits = await self._until(lambda: scan.advance(self._log) or None, repr(pattern))
        return hits[0]

    async def wait_count(self, pattern: str, count: int, *, since: Mark | None = None) -> list[Hit]:
        """All matches after `since`, once there are at least `count`."""
        scan = Scan(pattern, since.offset if since else 0)

        def enough() -> list[Hit] | None:
            hits = scan.advance(self._log)
            return hits if len(hits) >= count else None

        return await self._until(enough, f"{count}x {pattern!r}")

    async def await_run(self, run: str, *, since: Mark) -> frames.Pass:
        """A probe run's frames. A damaged or lost run fails at once."""

        def passed() -> frames.Pass | None:
            try:
                result = frames.require(self._events[since.event :], run)
            except frames.FrameError as err:
                raise self.fail(str(err)) from None
            return result if isinstance(result, frames.Pass) else None

        return await self._until(passed, f"run {run}")

    async def _until[T](self, probe: Callable[[], T | None], what: str) -> T:
        self.waiting.add(what)
        try:
            async with self._changed:
                while (result := probe()) is None:
                    if self._fault is not None:
                        raise self.fail(f"failure {self._fault.text!r} while waiting for {what}")
                    if self._closed:
                        raise self.fail(f"console closed while waiting for {what}")
                    await self._changed.wait()
                return result
        except asyncio.CancelledError:
            self.interrupted = what
            raise
        finally:
            self.waiting.discard(what)
