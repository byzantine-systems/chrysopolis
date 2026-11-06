"""QEMU blklogwrites logs (dm-log-writes format), and the crash states they imply.

Writes between two flushes may reach the disk in any order, so a crash
state is every earlier epoch in full plus any subset of one epoch.
"""

from __future__ import annotations

import random
import shutil
import struct
from collections.abc import Iterable
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from typing import Final

MAGIC: Final = 0x6A736677736872
VERSION: Final = 1
FLUSH: Final = 1 << 0
FUA: Final = 1 << 1
DISCARD: Final = 1 << 2
MARK: Final = 1 << 3
FLAG_MASK: Final = FLUSH | FUA | DISCARD | MARK
SUPER: Final = struct.Struct("<QQQI")
HEADER: Final = struct.Struct("<QQQQ")


@dataclass(frozen=True, slots=True)
class Entry:
    """One logged request. `start` and `end` bound it in the log."""

    start: int
    end: int
    sector: int
    nr_sectors: int
    flags: int
    data: bytes

    @property
    def flush(self) -> bool:
        return bool(self.flags & FLUSH)

    @property
    def write(self) -> bool:
        return not self.flags & (FLUSH | DISCARD)


@dataclass(frozen=True)
class Log:
    sectorsize: int
    entries: tuple[Entry, ...]

    @cached_property
    def epochs(self) -> tuple[tuple[int, ...], ...]:
        """Indices of the writes between consecutive flushes. The last epoch
        holds writes no flush has covered yet."""
        epochs: list[list[int]] = [[]]
        for i, entry in enumerate(self.entries):
            if entry.flush:
                epochs.append([])
            elif entry.write:
                epochs[-1].append(i)
        return tuple(map(tuple, epochs))

    @cached_property
    def flushes(self) -> tuple[Entry, ...]:
        return tuple(e for e in self.entries if e.flush)


def parse(raw: bytes) -> Log:
    """Every complete entry. The walk goes past `nr_entries`, which counts
    entries only up to the last superblock update."""
    if not raw:
        return Log(512, ())  # nothing written yet: blklogwrites starts lazily
    magic, version, _, sectorsize = SUPER.unpack_from(raw, 0)
    if (magic, version) == (0, 0):
        sectorsize = 512  # never flushed: blklogwrites writes the superblock on a flush
    elif magic != MAGIC or version != VERSION:
        raise ValueError(f"not a write log: magic {magic:#x}, version {version}")
    entries, pos = [], sectorsize
    while pos + sectorsize <= len(raw):
        sector, nr_sectors, flags, _ = HEADER.unpack_from(raw, pos)
        if (sector, nr_sectors, flags) == (0, 0, 0):
            break
        if flags & ~FLAG_MASK:
            raise ValueError(f"unknown flags {flags:#x} at offset {pos}")
        data_at = pos + sectorsize
        end = data_at + (0 if flags & (FLUSH | DISCARD) else nr_sectors * sectorsize)
        if end > len(raw):
            break
        entries.append(Entry(pos, end, sector, nr_sectors, flags, raw[data_at:end]))
        pos = end
    return Log(sectorsize, tuple(entries))


def state(log: Log, epoch: int, subset: Iterable[int] = ()) -> frozenset[int]:
    """Every write of the epochs before `epoch`, plus `subset` of that one."""
    done = (i for writes in log.epochs[:epoch] for i in writes)
    return frozenset(done) | (frozenset(subset) & frozenset(log.epochs[epoch]))


def persisted(log: Log, epoch: int, seed: int) -> frozenset[int]:
    """A seeded subset of `epoch`'s writes that a crash let reach the disk, in
    any order. Seed 0 keeps none, so shrinking settles on the boundary."""
    if seed == 0 or epoch >= len(log.epochs):
        return frozenset()
    rng = random.Random(f"{seed}:{epoch}")
    writes = log.epochs[epoch]
    return frozenset(rng.sample(writes, rng.randint(0, len(writes))))


def crashed_at(log: Log, epoch: int, subset: Iterable[int] = ()) -> int:
    """Where the crash fell: the end of the last kept write, or the stable point."""
    if epoch >= len(log.epochs):
        return stable(log, epoch)
    kept = frozenset(subset) & frozenset(log.epochs[epoch])
    return max((log.entries[i].end for i in kept), default=stable(log, epoch))


def stable(log: Log, epoch: int) -> int:
    """The log offset up to which a crash during `epoch` keeps every write and
    flush: the end of the flush that closed the epoch before. Data a sync
    finished by then is on the crashed disk."""
    return log.flushes[epoch - 1].end if epoch else log.sectorsize


def replay(log: Log, base: Path, out: Path, keep: frozenset[int]) -> Path:
    """`base` with the kept writes applied in log order, written to `out`."""
    shutil.copyfile(base, out)
    with out.open("r+b") as disk:
        for i in sorted(keep):
            entry = log.entries[i]
            disk.seek(entry.sector * log.sectorsize)
            disk.write(entry.data)
    return out
