"""Where a FAT32 file keeps its data, FAT entry and directory entry, in disk
sectors, for IRON-style targeted faults. pyfatfs parses the volume.
"""

from __future__ import annotations

import contextlib
import struct
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any, Final, Literal

from pyfatfs.PyFat import PyFat

SECTOR: Final = 512
MBR_SIGNATURE: Final = b"\x55\xaa"
# The first partition entry's starting LBA, in the MBR's partition table.
FIRST_LBA: Final = 0x1C6
FAT32_ENTRY: Final = 4

type Region = Literal["data", "fat", "entry"]
REGIONS: Final[tuple[Region, ...]] = ("data", "fat", "entry")


def partition_start(mbr: bytes) -> int:
    """Byte offset of the first partition, from a disk's first sector."""
    if mbr[510:512] != MBR_SIGNATURE:
        raise ValueError("no MBR signature")
    (lba,) = struct.unpack_from("<I", mbr, FIRST_LBA)
    return int(lba) * SECTOR


def short_name(name: str) -> bytes:
    """The 11 bytes FAT stores for an 8.3 name: `fuzz1.bin` is `FUZZ1   BIN`."""
    stem, _, ext = name.upper().partition(".")
    return f"{stem:<8}{ext:<3}".encode()


@dataclass(frozen=True, slots=True)
class Extent:
    """One file's first data sector, its FAT entry's sector and its directory
    entry's sector."""

    data: int
    fat: int
    entry: int


@dataclass(frozen=True, slots=True)
class Layout:
    """Extents of the root-level files, keyed by their 8.3 name as stored."""

    files: Mapping[bytes, Extent]

    def sector(self, region: Region, name: str) -> int | None:
        """Where `region` of file `name` starts, or None if it has no data."""
        if (extent := self.files.get(short_name(name))) is None:
            return None
        match region:
            case "data":
                return extent.data
            case "fat":
                return extent.fat
            case "entry":
                return extent.entry


def layout(image: Path) -> Layout:
    """Read a FAT32 disk image's layout. Only FAT32 is read, because that is
    what `packages.disk` formats."""
    with image.open("rb") as disk:
        start = partition_start(disk.read(SECTOR))
    raw = image.read_bytes()
    with contextlib.closing(PyFat(offset=start)) as volume:
        volume.open(str(image), read_only=True)
        if volume.fat_type != PyFat.FAT_TYPE_FAT32:
            raise ValueError(f"{image}: FAT{volume.fat_type}, expected FAT32")
        if (root := volume.root_dir) is None:
            raise ValueError(f"{image}: no root directory")
        return Layout(MappingProxyType(dict(_extents(volume, root, raw, start))))


def _extents(volume: Any, root: Any, raw: bytes, start: int) -> list[tuple[bytes, Extent]]:
    bpb = volume.bpb_header
    fat_start = start + bpb["BPB_RsvdSecCnt"] * bpb["BPB_BytsPerSec"]

    def sector(offset: int) -> int:
        return offset // SECTOR

    def cluster_sectors(first: int) -> list[int]:
        """Every sector of a cluster chain, in order."""
        per_cluster = volume.bytes_per_cluster // SECTOR
        return [
            sector(start + volume.get_data_cluster_address(c)) + i
            for c in volume.get_cluster_chain(first)
            for i in range(per_cluster)
        ]

    directory = cluster_sectors(bpb["BPB_RootClus"])
    _, files, _ = root.get_entries()
    extents = []
    for entry in files:
        if (cluster := entry.get_cluster()) < 2:
            continue
        name = short_name(entry.get_short_name())
        holder = next(
            (s for s in directory if name in _entries(raw[s * SECTOR : (s + 1) * SECTOR])),
            None,
        )
        if holder is None:
            raise ValueError(f"no directory entry holds {name!r}")
        extents.append(
            (
                name,
                Extent(
                    data=sector(start + volume.get_data_cluster_address(cluster)),
                    fat=sector(fat_start + cluster * FAT32_ENTRY),
                    entry=holder,
                ),
            )
        )
    return extents


def _entries(directory_sector: bytes) -> set[bytes]:
    """The 8.3 names a directory sector's 32-byte entries hold."""
    return {directory_sector[i : i + 11] for i in range(0, len(directory_sector), 32)}
