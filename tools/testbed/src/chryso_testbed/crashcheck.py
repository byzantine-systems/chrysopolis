"""What a crash at a point in the write log may have left, and the check of a
crash image against it. A step reported ok before the point must hold, no
crash may leave a broken chain or two files on one cluster, and a cut rename
keeps its file under at least one name.
"""

from __future__ import annotations

import os
import struct
import subprocess
import warnings
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Literal

from pyfatfs import PyFATException

from chryso_testbed.fatlayout import SECTOR, Chain, partition_start, short_name, survey
from chryso_testbed.faults import CrashPut, Overwrite, Put, Remove, Rename, Step
from chryso_testbed.outcomes import Outcome, Status

FUZZ_SIZE: Final = 8192  # what chryso_fs:put/3 writes


@dataclass(frozen=True, slots=True)
class Span:
    """A step and the stretch of the write log it produced."""

    step: Step
    outcome: Outcome
    start: int
    end: int


# What a crash image holds for one file: a whole file of one seed, no entry,
# an empty entry, other bytes, or an entry on clusters the FAT lacks.
type Content = int | Literal["absent", "empty", "garbage", "broken"]


type May = frozenset[Content] | None  # None allows anything
ABSENT: Final[frozenset[Content]] = frozenset({"absent"})


def _union(a: May, b: May) -> May:
    return None if a is None or b is None else a | b


def allowed(spans: Iterable[Span], stable: int, crash: int | None = None) -> Mapping[int, May]:
    """What each file may hold after a crash, or None for anything. A step ok
    by `stable` is done. One started by `crash` but not stable may be done or
    not: a rewrite leaves the file as before, empty or new, an overwrite as
    before, new or part new (garbage), a remove as before or absent, and a
    rename `src` as before or absent and `dst` as before or `src`'s content.
    A failed step leaves anything, a refused one nothing changed."""
    crash = stable if crash is None else crash
    files: dict[int, May] = {}

    def before(f: int) -> May:
        return files.get(f, ABSENT)

    for span in spans:
        if span.start >= crash:
            continue
        done = span.outcome == Status.OK and span.end <= stable
        match span.step, span.outcome:
            case CrashPut(file=f, seed=s), _:
                files[f] = _union(before(f), frozenset({"empty", s}))
            case Put() | Overwrite() | Remove() | Rename(), Status.ENOENT | Status.EEXIST:
                continue
            case Put(file=f) | Overwrite(file=f) | Remove(file=f), outcome if outcome != Status.OK:
                files[f] = None
            case Rename(src=s, dst=d), outcome if outcome != Status.OK:
                files[s] = files[d] = None
            case Put(file=f, seed=s), _:
                files[f] = frozenset({s}) if done else _union(before(f), frozenset({"empty", s}))
            case Overwrite(file=f, seed=s), _:
                files[f] = frozenset({s}) if done else _union(before(f), frozenset({"garbage", s}))
            case Remove(file=f), _:
                files[f] = ABSENT if done else _union(before(f), ABSENT)
            case Rename(src=s, dst=d), _ if s != d:
                moved = before(s)
                files[s], files[d] = (
                    (ABSENT, moved) if done else (_union(moved, ABSENT), _union(before(d), moved))
                )
    return files


@dataclass(frozen=True, slots=True)
class Move:
    """A rename a crash may have cut: `content` is under `src`, `dst` or both."""

    src: int
    dst: int
    content: frozenset[Content]


def moves(spans: Iterable[Span], stable: int, crash: int | None = None) -> tuple[Move, ...]:
    """The renames in flight at the crash whose content is known and whose
    names no later step touches, for the joint rule `allowed` cannot state
    file by file."""
    crash = stable if crash is None else crash
    spans = [s for s in spans if s.start < crash]
    cut = []
    for i, span in enumerate(spans):
        match span.step:
            case Rename(src=s, dst=d) if (
                s != d
                and span.outcome == Status.OK
                and span.end > stable
                and not any({s, d} & touched(later.step) for later in spans[i + 1 :])
                and (content := allowed(spans[:i], stable, crash).get(s, ABSENT)) is not None
            ):
                cut.append(Move(s, d, content))
    return tuple(cut)


def touched(step: Step) -> set[int]:
    """The fuzz files a step may change."""
    match step:
        case Rename(src=s, dst=d):
            return {s, d}
        case Put(file=f) | CrashPut(file=f) | Overwrite(file=f) | Remove(file=f):
            return {f}
        case _:
            return set()


def durable(
    spans: Iterable[Span], stable: int, crash: int | None = None
) -> Mapping[int, int | None]:
    """Each file's seed a crash must keep: `allowed` narrowed to one seed."""
    promised: dict[int, int | None] = {}
    for f, may in allowed(spans, stable, crash).items():
        only = next(iter(may)) if may is not None and len(may) == 1 else None
        promised[f] = only if isinstance(only, int) else None
    return promised


def held(found: Mapping[bytes, bytes | None], file: int) -> Content:
    """What the image holds for fuzz file `file`."""
    name = short_name(f"fuzz{file}.bin")
    if name not in found:
        return "absent"
    match found[name]:
        case None:
            return "broken"
        case b"":
            return "empty"
        case data if len(data) == FUZZ_SIZE and data == bytes([data[0]]) * FUZZ_SIZE:
            return data[0]
        case _:
            return "garbage"


@dataclass(frozen=True, slots=True)
class Verdict:
    """A crash image's problems, its broken chains, whether its FAT copies
    differ, and the renames a crash left on both names at once."""

    problems: tuple[str, ...]
    broken: tuple[str, ...]
    diverged: bool = False
    crosslinked: tuple[tuple[int, int], ...] = ()


def judge(image: Path, expected: Mapping[int, May], *, cut: Iterable[Move] = ()) -> Verdict:
    """Where a crash image breaks `expected`, as `allowed` gives it, or a cut
    rename, as `moves` gives it. Two entries on one cluster are a problem,
    except a cut rename's two names, which FatFs registers before it removes
    the old one: that cross-link is counted, not failed."""
    try:
        with warnings.catch_warnings(record=True) as warned:
            warnings.simplefilter("always")
            found, links = survey(image)
    except (PyFATException, ValueError, OSError, struct.error, IndexError) as error:
        return Verdict((f"volume unreadable: {error!r}",), ())
    diverged = any("FATs differ" in str(w.message) for w in warned)
    broken = tuple(name.decode() for name, data in found.items() if data is None)
    problems = [f"{name}: entry on clusters the FAT does not hold" for name in broken]
    if diverged:  # packages.disk has one FAT, so two copies that disagree are a fault
        problems.append("the FAT copies differ")
    for file, may in expected.items():
        if may is not None and (got := held(found, file)) not in may:
            problems.append(f"fuzz{file}.bin: holds {got!r}, allowed {sorted(map(str, may))}")
    cut = tuple(cut)
    for m in cut:
        if "absent" not in m.content and held(found, m.src) == held(found, m.dst) == "absent":
            problems.append(f"fuzz{m.src}.bin -> fuzz{m.dst}.bin: lost under both names")
    pairs = {frozenset({_fuzz(m.src), _fuzz(m.dst)}): (m.src, m.dst) for m in cut}
    crosslinked = []
    for a, b in _sharing(links):
        if (pair := pairs.get(frozenset({a, b}))) is not None:
            crosslinked.append(pair)
        else:
            problems.append(f"{a.decode()} and {b.decode()} share clusters")
    return Verdict(tuple(problems), broken, diverged, tuple(crosslinked))


def _fuzz(file: int) -> bytes:
    return short_name(f"fuzz{file}.bin")


def _sharing(links: Mapping[bytes, Chain]) -> list[tuple[bytes, bytes]]:
    """Every pair of entries whose chains meet, in name order."""
    owner: dict[int, bytes] = {}
    pairs: set[tuple[bytes, bytes]] = set()
    for name, chain in sorted(links.items()):
        for cluster in chain or ():
            if (other := owner.setdefault(cluster, name)) != name:
                pairs.add((other, name))
    return sorted(pairs)


def fsck(image: Path, scratch: Path) -> int:
    """fsck.fat's exit status on the image's partition, checked read-only.
    fsck.fat takes no offset, so the partition is copied out first, in the
    kernel, and removed after."""
    partition = scratch / "partition.img"
    with image.open("rb") as src, partition.open("wb") as dst:
        offset = partition_start(src.read(SECTOR))
        left = os.fstat(src.fileno()).st_size - offset
        while left > 0:
            copied = os.copy_file_range(src.fileno(), dst.fileno(), left, offset)
            if copied == 0:
                break
            offset, left = offset + copied, left - copied
    try:
        return subprocess.run(
            ["fsck.fat", "-n", str(partition)], capture_output=True, check=False
        ).returncode
    finally:
        partition.unlink()
