"""What a crash at a point in the write log may have left, and the check of a
crash image against it. A write reported ok before the point must read back,
and no crash may leave a broken chain.
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

from chryso_testbed.fatlayout import SECTOR, contents, partition_start, short_name
from chryso_testbed.faults import CrashPut, Put, Step
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


def allowed(
    spans: Iterable[Span], stable: int, crash: int | None = None
) -> Mapping[int, frozenset[Content] | None]:
    """What each file may hold after a crash, or None for anything. A write ok
    by `stable` is what it holds. One started by `crash` but not stable leaves
    it as before, empty or new. A failed write leaves anything."""
    crash = stable if crash is None else crash
    files: dict[int, frozenset[Content] | None] = {}
    for span in spans:
        if span.start >= crash or not isinstance(span.step, Put | CrashPut):
            continue
        f, seed = span.step.file, span.step.seed
        before = files.get(f, frozenset({"absent"}))
        if isinstance(span.step, Put) and span.outcome != Status.OK:
            files[f] = None
        elif isinstance(span.step, Put) and span.end <= stable:
            files[f] = frozenset({seed})
        else:
            files[f] = None if before is None else before | {"empty", seed}
    return files


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
    """A crash image's problems, its broken chains, and whether its FAT copies
    differ."""

    problems: tuple[str, ...]
    broken: tuple[str, ...]
    diverged: bool = False


def judge(image: Path, expected: Mapping[int, frozenset[Content] | None]) -> Verdict:
    """Where a crash image breaks `expected`, as `allowed` gives it."""
    try:
        with warnings.catch_warnings(record=True) as warned:
            warnings.simplefilter("always")
            found = contents(image)
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
    return Verdict(tuple(problems), broken, diverged)


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
