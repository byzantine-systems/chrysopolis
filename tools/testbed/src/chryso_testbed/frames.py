"""Console frame reader, the Python twin of chryso_frame (apps/chryso_core).

    @@CHRYSO|<run>|n=<i>|k=<kind>|<key>=<value>...|len=<L>|crc=<8 hex>@@

L counts the bytes after the marker up to "|len=". The CRC covers the body
through the len field. Golden vectors hold both readers to the same answers.

step() reads one byte:

    state               input            next                    event
    Outside(m)          marker byte m    Outside(m+1) / Inside   -
    Outside(m)          other            Outside(KMP fallback)   -
    Inside(b, plain)    "@"              Inside(b, at)           -
    Inside(b, at)       "@"              Outside(2)              Frame or Damaged
    Inside(b, at)       other            Inside(b+"@"+c, plain)  -
    Inside(b, plain)    other            Inside(b+c, plain)      -
    Inside, at the cap  any              Outside(0)              Damaged(oversize)

Closing leaves the matcher at 2 because "@@" also starts a marker. A frame
that lost its end closes on the next marker, and the next frame still parses.
A closed body is checked for syntax, then length, then crc. The first failure
is the reason.

feed() skips bytes that cannot change the state. feed_bytewise() is the
reference, and the tests hold feed() to it.

The log is untrusted. Values stay str, state is capped at MAX_BODY, and work
is linear.
"""

from __future__ import annotations

import enum
import zlib
from typing import Final, Literal, NamedTuple

MARKER: Final = b"@@CHRYSO|"
MAX_BODY: Final = 1024
MAX_RUN: Final = 48
_AT: Final = 0x40


class Reason(enum.Enum):
    OVERSIZE = "oversize"
    SYNTAX = "syntax"
    LENGTH = "length"
    CRC = "crc"


class Frame(NamedTuple):
    run: str
    n: int
    kind: str
    fields: tuple[tuple[str, str], ...]

    def get(self, key: str) -> str:
        for k, v in self.fields:
            if k == key:
                return v
        raise KeyError(f"{self.run} n={self.n} has no field {key!r}")


class Damaged(NamedTuple):
    run: str
    reason: Reason


class Pending(NamedTuple):
    pass


class Outside(NamedTuple):
    matched: int


class Inside(NamedTuple):
    body: bytes
    after: Literal["plain", "at"]


type Event = Frame | Damaged | Pending
type State = Outside | Inside

START: Final[State] = Outside(0)


class Pass(NamedTuple):
    frames: tuple[Frame, ...]
    unexpected: int


class Fail(NamedTuple):
    kind: Literal["damaged", "lost"]
    run: str
    detail: str


type Verdict = Pending | Pass | Fail


class FrameError(AssertionError):
    """A probe run that was damaged or lost on the console."""


# --- the machine ---------------------------------------------------------------


def advance(matched: int, c: int) -> int:
    """KMP fallback, by brute force over the nine marker bytes."""
    seen = MARKER[:matched] + bytes([c])
    for k in range(min(matched + 1, len(MARKER)), -1, -1):
        if seen.endswith(MARKER[:k]):
            return k
    raise AssertionError("unreachable: k = 0 always matches")


def step(c: int, state: State) -> tuple[list[Event], State]:
    match state:
        case Outside(matched=matched):
            m = advance(matched, c)
            return [], (Inside(b"", "plain") if m == len(MARKER) else Outside(m))
        case Inside(body=body) if len(body) >= MAX_BODY:
            return [Damaged(guess(body), Reason.OVERSIZE)], Outside(0)
        case Inside(body=body, after="plain") if c == _AT:
            return [], Inside(body, "at")
        case Inside(body=body, after="at") if c == _AT:
            return [close(body)], Outside(2)
        case Inside(body=body, after="at"):
            return [], Inside(body + b"@" + bytes([c]), "plain")
        case Inside(body=body, after="plain"):
            return [], Inside(body + bytes([c]), "plain")
    raise AssertionError(f"unreachable reader state {state!r}")


def feed_bytewise(data: bytes, state: State) -> tuple[list[Event], State]:
    """The reference: step() over every byte."""
    events: list[Event] = []
    for c in data:
        out, state = step(c, state)
        events.extend(out)
    return events, state


def feed(data: bytes, state: State) -> tuple[list[Event], State]:
    """feed_bytewise(), skipping bytes that cannot change the state.

    Outside(0) moves only on "@". In a plain body, bytes before the next "@"
    are appended in one slice, up to the cap.
    """
    events: list[Event] = []
    i, n = 0, len(data)
    while i < n:
        match state:
            case Outside(matched=0):
                j = data.find(b"@", i)
                if j < 0:
                    return events, state
                i = j
            case Inside(body=body, after="plain") if len(body) < MAX_BODY:
                room = MAX_BODY - len(body)
                j = data.find(b"@", i, i + room)
                stop = j if j >= 0 else min(n, i + room)
                if stop > i:
                    state = Inside(body + data[i:stop], "plain")
                    i = stop
                    continue
                # The next byte is "@": one step() below.
        out, state = step(data[i], state)
        events.extend(out)
        i += 1
    return events, state


def finish(state: State) -> list[Event]:
    """An open frame at the end of the log is pending, never damaged."""
    return [Pending()] if isinstance(state, Inside) else []


def as_bytes(text: str) -> bytes:
    """Console text as the bytes the guest wrote. Never raises."""
    return text.encode("utf-8", "surrogateescape")


def collect(log: bytes) -> list[Event]:
    events, state = feed(log, START)
    return events + finish(state)


# --- the probe verdict ---------------------------------------------------------


def observe(events: list[Event], run: str) -> Verdict:
    """Decide at the first damaged frame or at the run's verdict.

    Until then the run is pending, so a prefix never decides differently
    from the whole log.
    """
    seen: list[Frame] = []
    for event in events:
        match event:
            case Pending():
                return Pending()
            case Damaged(run=guessed, reason=reason):
                return Fail("damaged", guessed, reason.value)
            case Frame(run=r, kind="verdict") if r == run:
                return verdict(event, run, seen)
            case Frame(run=r) if r == run:
                seen.append(event)
    return Pending()


def verdict(final: Frame, run: str, seen: list[Frame]) -> Pass | Fail:
    expected = list(range(final.n))
    got = [f.n for f in seen]
    fields = dict(final.fields)
    count = digits(fields.get("frames", ""))
    unexpected = digits(fields.get("unexpected", ""))
    if count == final.n and unexpected is not None and got == expected:
        return Pass(tuple(seen), unexpected)
    missing = [n for n in expected if n not in got] if count == final.n else expected
    return Fail("lost", run, " ".join(f"n={n}" for n in missing))


def require(events: list[Event], run: str) -> Pending | Pass:
    """observe(), raising FrameError on a failed run."""
    result = observe(events, run)
    if isinstance(result, Fail):
        raise FrameError(f"{result.kind} {result.run} {result.detail}")
    return result


# --- closed bodies ---------------------------------------------------------------


def close(body: bytes) -> Event:
    parsed = parse(body)
    return parsed if isinstance(parsed, Frame) else Damaged(guess(body), parsed)


def parse(body: bytes) -> Frame | Reason:
    if any(c < 0x21 or c > 0x7E for c in body):
        return Reason.SYNTAX
    parts = body.split(b"|")
    if len(parts) < 5:
        return Reason.SYNTAX
    run, n_field, k_field, *rest, len_field, crc_field = parts
    if not (
        n_field.startswith(b"n=")
        and k_field.startswith(b"k=")
        and len_field.startswith(b"len=")
        and crc_field.startswith(b"crc=")
    ):
        return Reason.SYNTAX
    kind, crc = k_field[2:], crc_field[4:]
    n = digits(n_field[2:].decode("ascii"))
    declared = digits(len_field[4:].decode("ascii"))
    decoded = fields(rest)
    if not (
        is_run(run)
        and is_name(kind)
        and is_hex8(crc)
        and n is not None
        and declared is not None
        and decoded is not None
    ):
        return Reason.SYNTAX
    covered = len(body) - len(b"|crc=") - 8
    prefix = covered - len(b"|len=") - (len(len_field) - 4)
    if prefix != declared:
        return Reason.LENGTH
    if b"%08x" % zlib.crc32(body[:covered]) != crc:
        return Reason.CRC
    return Frame(run.decode("ascii"), n, kind.decode("ascii"), decoded)


def fields(raw: list[bytes]) -> tuple[tuple[str, str], ...] | None:
    out = []
    for field in raw:
        key, eq, value = field.partition(b"=")
        decoded = unescape(value)
        if not eq or not is_name(key) or decoded is None:
            return None
        out.append((key.decode("ascii"), decoded.decode("utf-8", "surrogateescape")))
    return tuple(out)


def unescape(value: bytes) -> bytes | None:
    out = bytearray()
    i = 0
    while i < len(value):
        c = value[i]
        if c == 0x25:  # %
            pair = value[i + 1 : i + 3]
            if len(pair) != 2 or any(h not in b"0123456789ABCDEF" for h in pair):
                return None
            out.append(int(pair, 16))
            i += 3
        elif c in b"@=":
            return None
        else:
            out.append(c)
            i += 1
    return bytes(out)


def digits(text: str) -> int | None:
    """Decimal without a sign or a redundant leading zero."""
    if text == "0":
        return 0
    if 1 <= len(text) <= 10 and text[0] in "123456789" and all(d in "0123456789" for d in text):
        return int(text)
    return None


def is_hex8(b: bytes) -> bool:
    return len(b) == 8 and all(c in b"0123456789abcdef" for c in b)


def is_run(b: bytes) -> bool:
    return 1 <= len(b) <= MAX_RUN and all(c in b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_." for c in b)


def is_name(b: bytes) -> bool:
    return len(b) >= 1 and all(c in b"abcdefghijklmnopqrstuvwxyz0123456789_" for c in b)


def guess(body: bytes) -> str:
    first = body.split(b"|", 1)[0]
    return first.decode("ascii") if is_run(first) else "?"
