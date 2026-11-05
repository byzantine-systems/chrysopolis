"""What a crash at a point in the write log must have kept, and the check of a
crash image against it. A write reported ok before the point must read back.
"""

from __future__ import annotations

import os
import struct
import subprocess
import warnings
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Final

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


def durable(spans: Iterable[Span], point: int) -> Mapping[int, int | None]:
    """Each written file's seed a crash at log offset `point` must keep, or
    None where anything may survive: a write still in flight at the point, or
    a last write that failed or was cut short."""
    files: dict[int, int | None] = {}
    for span in spans:
        if span.start >= point or not isinstance(span.step, Put | CrashPut):
            continue
        done = span.end <= point and isinstance(span.step, Put) and span.outcome == Status.OK
        files[span.step.file] = span.step.seed if done else None
    return files


@dataclass(frozen=True, slots=True)
class Verdict:
    """A crash image against what it must hold: broken promises, and the files
    whose chain a crash broke, which only a promise turns into a problem.
    `diverged` says the crash fell between the writes to the two FAT copies."""

    problems: tuple[str, ...]
    broken: tuple[str, ...]
    diverged: bool = False


def judge(image: Path, expected: Mapping[int, int | None]) -> Verdict:
    """Where a crash image breaks `expected`: an unreadable volume, or a file
    a finished write promised that is missing, different or broken."""
    try:
        with warnings.catch_warnings(record=True) as warned:
            warnings.simplefilter("always")
            found = contents(image)
    except (PyFATException, ValueError, OSError, struct.error, IndexError) as error:
        return Verdict((f"volume unreadable: {error!r}",), ())
    diverged = any("FATs differ" in str(w.message) for w in warned)
    broken = tuple(name.decode() for name, data in found.items() if data is None)
    wrong = []
    for file, seed in expected.items():
        if seed is None:
            continue
        held = found.get(short_name(f"fuzz{file}.bin"))
        if held != bytes([seed]) * FUZZ_SIZE:
            got = "missing" if held is None else f"{len(held)} bytes, not all {seed}"
            wrong.append(f"fuzz{file}.bin: {got}")
    return Verdict(tuple(wrong), broken, diverged)


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
