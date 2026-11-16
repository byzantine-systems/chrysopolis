"""Where the layout reader gets its bytes: a disk image on file, or a base
disk with kept writes on top. A crash state is the second kind, so judging
one reads only the sectors the reader asks for, never the whole image."""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Protocol


class Disk(Protocol):
    """Bytes by offset. A read past the end returns what there is."""

    def read(self, offset: int, size: int) -> bytes: ...


@dataclass(frozen=True, slots=True)
class FileDisk:
    """A disk image on file, read where asked."""

    path: Path

    def read(self, offset: int, size: int) -> bytes:
        with self.path.open("rb") as image:
            image.seek(offset)
            return image.read(size)


@dataclass(frozen=True, slots=True)
class Overlay:
    """`base` with whole-unit writes on top. `units` maps a unit's index to
    the bytes the newest write left there. Past the end of `base` it reads
    zeros, as a sparse replay of the same writes would."""

    base: Disk
    unit: int
    units: Mapping[int, bytes]

    @classmethod
    def of(cls, base: Disk, unit: int, writes: Iterable[tuple[int, bytes]]) -> Overlay:
        """`base` after `writes`, each an offset and its bytes, in order."""
        units: dict[int, bytes] = {}
        for offset, data in writes:
            if offset % unit or len(data) % unit:
                raise ValueError(f"a write at {offset} of {len(data)} bytes is not in {unit}s")
            for k in range(0, len(data), unit):
                units[(offset + k) // unit] = data[k : k + unit]
        return cls(base, unit, MappingProxyType(units))

    def read(self, offset: int, size: int) -> bytes:
        out = bytearray(self.base.read(offset, size).ljust(size, b"\0"))
        for u in range(offset // self.unit, (offset + size + self.unit - 1) // self.unit):
            if (data := self.units.get(u)) is None:
                continue
            lo, hi = max(offset, u * self.unit), min(offset + size, (u + 1) * self.unit)
            out[lo - offset : hi - offset] = data[lo - u * self.unit : hi - u * self.unit]
        return bytes(out)
