"""A crash model of renaming a file on FAT, with the intent record that lets a
mount finish a rename a crash cut.

A `Volume` holds directory sectors, the FAT's directory chains and the record.
A `Plan` is a sequence of flush epochs of whole-sector writes, as FatFs writes
its window. A crash keeps every earlier epoch and any subset of one, in log
order, as `crashlog` replays. Everything here is immutable and pure. The laws
in tests/test_renaming.py hold this model, and through it the FatFs fork.
"""

from __future__ import annotations

import struct
import zlib
from collections import Counter
from collections.abc import Iterator
from dataclasses import dataclass, field, replace
from itertools import combinations
from typing import Final, Literal, Protocol


@dataclass(frozen=True, slots=True)
class Entry:
    name: str
    cluster: int  # 0 for an empty file
    live: bool = True


@dataclass(frozen=True, slots=True)
class Intent:
    """A rename in flight: the old entry's (sector, index), its file and the
    new directory."""

    old: tuple[int, int]
    name: str
    cluster: int
    new_dir: str


@dataclass(frozen=True, slots=True)
class Volume:
    """Directory sectors by id, each directory's chain of them, the clusters
    freed and the intent record. A sector no chain names is free space, and
    may still hold leftover entries from an earlier use: a directory that
    grows into it shows them unless it is zeroed first."""

    sectors: tuple[tuple[Entry | None, ...], ...]
    dirs: tuple[tuple[str, tuple[int, ...]], ...]
    record: Intent | None = None
    freed: frozenset[int] = frozenset()

    def chain(self, d: str) -> tuple[int, ...]:
        return dict(self.dirs)[d]

    def cells(self, sector: int) -> tuple[Entry | None, ...]:
        return self.sectors[sector] if sector < len(self.sectors) else ()

    def entries(self, d: str) -> Iterator[tuple[tuple[int, int], Entry]]:
        """The live entries of directory `d` and their slots, in order."""
        for sector in self.chain(d):
            for i, entry in enumerate(self.cells(sector)):
                if entry is not None and entry.live:
                    yield (sector, i), entry

    def locate(self, d: str, name: str) -> tuple[tuple[int, int], Entry]:
        return next((slot, e) for slot, e in self.entries(d) if e.name == name)

    def names(self) -> dict[str, int | Literal["broken"]]:
        """The observation: every live name and its chain, broken once freed."""
        return {
            f"{d}/{e.name}": "broken" if e.cluster in self.freed else e.cluster
            for d, _ in self.dirs
            for _, e in self.entries(d)
        }

    def shared(self) -> set[int]:
        """Chains more than one live name sits on."""
        counts = Counter(c for c in self.names().values() if c != "broken" and c)
        return {c for c, n in counts.items() if n > 1}


# --- writes: immutable commands, each the whole of one sector ------------------------


class Write(Protocol):
    def onto(self, v: Volume) -> Volume: ...


@dataclass(frozen=True, slots=True)
class SectorWrite:
    sector: int
    cells: tuple[Entry | None, ...]

    def onto(self, v: Volume) -> Volume:
        sectors = list(v.sectors)
        sectors.extend([()] * (self.sector + 1 - len(sectors)))
        sectors[self.sector] = self.cells
        return replace(v, sectors=tuple(sectors))


@dataclass(frozen=True, slots=True)
class FatWrite:
    dirs: tuple[tuple[str, tuple[int, ...]], ...]
    freed: frozenset[int]

    def onto(self, v: Volume) -> Volume:
        return replace(v, dirs=self.dirs, freed=self.freed)


@dataclass(frozen=True, slots=True)
class RecordWrite:
    intent: Intent | None

    def onto(self, v: Volume) -> Volume:
        return replace(v, record=self.intent)


def put(v: Volume, slot: tuple[int, int], entry: Entry | None) -> SectorWrite:
    """The write that puts `entry` in `slot`, carrying the rest of its sector."""
    sector, i = slot
    cells = list(v.cells(sector))
    cells[i] = entry
    return SectorWrite(sector, tuple(cells))


@dataclass(frozen=True, slots=True)
class Plan:
    """Flush epochs of writes. The last one may have no flush after it."""

    epochs: tuple[tuple[Write, ...], ...] = field(default=())

    def writes(self) -> Iterator[Write]:
        return (w for epoch in self.epochs for w in epoch)

    def done(self, v: Volume) -> Volume:
        """Every epoch on the disk."""
        for w in self.writes():
            v = w.onto(v)
        return v

    def crashes(self, v: Volume) -> Iterator[Volume]:
        """Every state a crash may leave: each epoch's earlier epochs whole,
        plus any subset of it in log order. An epoch holds at most four
        writes here, so the subsets stay few."""
        for k, epoch in enumerate(self.epochs):
            start = Plan(self.epochs[:k]).done(v)
            for size in range(len(epoch) + 1):
                for kept in combinations(epoch, size):
                    yield Plan((kept,)).done(start)
        yield self.done(v)


# --- the protocols -------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Design:
    """The protocol's choices. `FIXED` is the fork's, the others are planted
    defects the laws must reject."""

    record: bool = True  # write an intent before renaming
    record_apart: bool = True  # flush it before the new entry
    record_empty: bool = False  # also for a file with no chain, matched by cluster 0
    clear_apart: bool = True  # recovery flushes the removal before clearing
    free_chain: bool = False  # recovery frees the old entry's chain
    grow_apart: bool = True  # a directory's new sector is zeroed and flushed before it is linked


FIXED: Final = Design()


@dataclass(frozen=True, slots=True)
class Placement:
    """Where a new entry of a directory goes. When the directory is full it
    grows by a sector: `zero` clears it and `link` adds it to the chain."""

    slot: tuple[int, int]
    zero: tuple[Write, ...] = ()
    link: tuple[Write, ...] = ()

    def epochs(self, entry: Write, design: Design) -> tuple[tuple[Write, ...], ...]:
        """The flush epochs that grow the directory and write `entry`. The
        fork zeroes and flushes first, upstream FatFs links in the same epoch."""
        if self.zero and design.grow_apart:
            return (self.zero, (*self.link, entry))
        return ((*self.link, *self.zero, entry),)

    def grown(self, v: Volume) -> Volume:
        return Plan((self.zero, self.link)).done(v)


def placement(v: Volume, d: str) -> Placement:
    """Where FatFs's dir_alloc puts a new entry of `d`: its first unused slot,
    or the first slot of a sector the directory grows into. That sector is the
    lowest one no chain names, leftovers and all, or a new one."""
    for sector in v.chain(d):
        for i, entry in enumerate(v.cells(sector)):
            if entry is None or not entry.live:
                return Placement((sector, i))
    chained = {s for _, chain in v.dirs for s in chain}
    new = next(s for s in range(len(v.sectors) + 1) if s not in chained)
    width = len(v.cells(v.chain(d)[0]))
    dirs = tuple((k, (*s, new) if k == d else s) for k, s in v.dirs)
    return Placement(
        (new, 0), zero=(SectorWrite(new, (None,) * width),), link=(FatWrite(dirs, v.freed),)
    )


def create(v: Volume, d: str, name: str, cluster: int, design: Design = FIXED) -> Plan:
    """A new entry `name` in `d`, as a create's directory writes."""
    at = placement(v, d)
    return Plan(at.epochs(put(at.grown(v), at.slot, Entry(name, cluster)), design))


def rename(v: Volume, src: tuple[str, str], dst: tuple[str, str], design: Design = FIXED) -> Plan:
    """FatFs's f_rename with the fork's flushes and, by `design`, the intent.
    The last epoch is the record's clear, which no flush follows."""
    old, entry = v.locate(*src)
    record = design.record and (entry.cluster != 0 or design.record_empty)
    intent = (RecordWrite(Intent(old, entry.name, entry.cluster, dst[0])),) if record else ()
    at = placement(v, dst[0])
    *growth, register = at.epochs(put(at.grown(v), at.slot, Entry(dst[1], entry.cluster)), design)
    removal = put(Plan((intent, *growth, register)).done(v), old, replace(entry, live=False))
    clear = (RecordWrite(None),) if record else ()
    if design.record_apart:
        head = (intent, *growth, register)
    elif growth:  # the growth's own flush carries the record out before the entry
        head = ((*intent, *growth[0]), register)
    else:
        head = ((*intent, *register),)
    return Plan((*head, (removal,), clear))


def recover(v: Volume, design: Design = FIXED) -> Plan:
    """The mount-time step: finish a cut rename whose two names share a chain,
    then clear the record."""
    if (intent := v.record) is None:
        return Plan()
    sector, i = intent.old
    cells = v.cells(sector)
    old = cells[i] if i < len(cells) else None
    twin = any(
        e.cluster == intent.cluster and s != intent.old for s, e in v.entries(intent.new_dir)
    )
    finish = (
        old == Entry(intent.name, intent.cluster)
        and (intent.cluster != 0 or design.record_empty)
        and twin
    )
    clear = (RecordWrite(None),)
    if not finish or old is None:
        return Plan((clear,))
    removal: tuple[Write, ...] = (put(v, intent.old, replace(old, live=False)),)
    if design.free_chain:
        removal = (*removal, FatWrite(v.dirs, v.freed | {intent.cluster}))
    return Plan((removal, clear) if design.clear_apart else ((*removal, *clear),))


def settled(v: Volume, design: Design = FIXED) -> Volume:
    """`v` after a mount whose recovery ran to the end."""
    return recover(v, design).done(v)


# --- the record on disk --------------------------------------------------------------

MAGIC: Final = b"CHRYRNM1"
VERSION: Final = 1
# magic, version, old dir, blk_ofs, dptr, short name, (pad), cluster, new dir
LAYOUT: Final = struct.Struct("<8sIIII11sxII")
CRC: Final = struct.Struct("<I")
SIZE: Final = LAYOUT.size + CRC.size


@dataclass(frozen=True, slots=True)
class Record:
    """The intent as the fork stores it, in FatFs's own coordinates."""

    old_dir: int  # the old directory's start cluster
    blk_ofs: int  # the old entry block's first byte (its first LFN entry)
    dptr: int  # the old short entry's byte offset in its directory
    sfn: bytes  # the old entry's 11-byte short name
    cluster: int  # the file's start cluster
    new_dir: int

    def encode(self, sector_size: int) -> bytes:
        body = LAYOUT.pack(
            MAGIC, VERSION, self.old_dir, self.blk_ofs, self.dptr, self.sfn,
            self.cluster, self.new_dir,
        )  # fmt: skip
        return (body + CRC.pack(zlib.crc32(body))).ljust(sector_size, b"\0")

    @classmethod
    def decode(cls, sector: bytes) -> Record | Literal["absent", "foreign", "corrupt"]:
        """All zero is no record. Anything not ours is foreign and never
        touched. Ours with a bad CRC or a nonzero tail is corrupt."""
        if not any(sector):
            return "absent"
        if len(sector) < SIZE or not sector.startswith(MAGIC):
            return "foreign"
        _, version, *fields = LAYOUT.unpack_from(sector)
        if version != VERSION:
            return "foreign"
        (crc,) = CRC.unpack_from(sector, LAYOUT.size)
        if crc != zlib.crc32(sector[: LAYOUT.size]) or any(sector[SIZE:]):
            return "corrupt"
        return cls(*fields)
