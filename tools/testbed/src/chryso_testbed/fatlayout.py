"""Where a FAT32 file keeps its data, FAT entry and directory entry, in disk
sectors, for IRON-style targeted faults. pyfatfs parses the volume.
"""

from __future__ import annotations

import contextlib
import struct
from collections.abc import Iterator, Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any, Final, Literal

from pyfatfs.PyFat import PyFat

from chryso_testbed.renaming import Record

SECTOR: Final = 512
MBR_SIGNATURE: Final = b"\x55\xaa"
# The first partition entry's starting LBA, in the MBR's partition table.
FIRST_LBA: Final = 0x1C6
FAT32_ENTRY: Final = 4
ENTRY_MASK: Final = 0x0FFFFFFF  # the top four bits of a FAT32 entry are reserved
END_OF_CHAIN: Final = 0x0FFFFFF8
FIRST_CLUSTER: Final = 2
ENTRY: Final = 32  # bytes of a directory entry
DELETED: Final = 0xE5
LONG_NAME: Final = 0x0F  # the attribute byte of an LFN entry
NO_LFN: Final = 0xFFFFFFFF  # FatFs's blk_ofs for an entry with no long name
# Boot sector fields: bytes per sector and reserved sectors.
BPB_BYTS_PER_SEC: Final = 0x0B
BPB_RSVD_SEC_CNT: Final = 0x0E

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


@contextlib.contextmanager
def _volume(image: Path) -> Iterator[tuple[Any, Any, bytes, int]]:
    """The open FAT32 volume, its root directory, the raw image and the
    partition's byte offset."""
    raw = image.read_bytes()
    start = partition_start(raw[:SECTOR])
    with contextlib.closing(PyFat(offset=start)) as volume:
        volume.open(str(image), read_only=True)
        if volume.fat_type != PyFat.FAT_TYPE_FAT32:
            raise ValueError(f"{image}: FAT{volume.fat_type}, expected FAT32")
        if (root := volume.root_dir) is None:
            raise ValueError(f"{image}: no root directory")
        yield volume, root, raw, start


def layout(image: Path) -> Layout:
    """Read a FAT32 disk image's layout. Only FAT32 is read, because that is
    what `packages.disk` formats."""
    with _volume(image) as (volume, root, raw, start):
        return Layout(MappingProxyType(dict(_extents(volume, root, raw, start))))


type Chain = tuple[int, ...] | None  # None: broken


def survey(image: Path) -> tuple[Mapping[bytes, bytes | None], Mapping[bytes, Chain]]:
    """Each root-level file's bytes and cluster chain, keyed by 8.3 name, in
    one read of the image. Both are None where the chain is broken (a free,
    bad or out-of-range link, or a loop), as a crash can leave an entry on
    clusters already freed."""
    with _volume(image) as (volume, root, raw, start):
        view, size = memoryview(raw), volume.bytes_per_cluster
        fat = _fat(volume, raw, start)

        def cluster(c: int) -> memoryview:
            at = start + volume.get_data_cluster_address(c)
            return view[at : at + size]  # a view: the join below is the one copy

        _, files, _ = root.get_entries()
        found: dict[bytes, bytes | None] = {}
        links: dict[bytes, Chain] = {}
        for entry in files:
            name = short_name(entry.get_short_name())
            links[name] = chain = _chain(fat, entry.get_cluster())
            found[name] = None if chain is None else b"".join(map(cluster, chain))[: entry.filesize]
        return MappingProxyType(found), MappingProxyType(links)


def contents(image: Path) -> Mapping[bytes, bytes | None]:
    """Each root-level file's bytes, or None where its chain is broken."""
    return survey(image)[0]


def chains(image: Path) -> Mapping[bytes, Chain]:
    """Each root-level file's cluster chain, or None where it is broken."""
    return survey(image)[1]


@dataclass(frozen=True, slots=True)
class Root:
    """The root directory's raw sectors in chain order, its first cluster, the
    sector size, the last reserved sector, where the rename record lives, and
    the root's cluster chain."""

    cluster: int
    sector_size: int
    sectors: tuple[bytes, ...]
    record: bytes
    clusters: tuple[int, ...] = ()

    def free_slots(self) -> int:
        """Entries a new name could take: never used (0) or deleted (0xE5)."""
        raw = b"".join(self.sectors)
        return sum(raw[at] in (0, DELETED) for at in range(0, len(raw), ENTRY))


def root(image: Path) -> Root:
    with _volume(image) as (volume, _, raw, start):
        bpb = volume.bpb_header
        size, first = bpb["BPB_BytsPerSec"], bpb["BPB_RootClus"]
        record_at = start + (bpb["BPB_RsvdSecCnt"] - 1) * size
        chain = _chain(_fat(volume, raw, start), first) or ()
        sectors: list[bytes] = []
        for cluster in chain:
            at = start + volume.get_data_cluster_address(cluster)
            sectors += [
                raw[at + k : at + k + size] for k in range(0, volume.bytes_per_cluster, size)
            ]
        return Root(first, size, tuple(sectors), raw[record_at : record_at + size], chain)


# A fuzz name, so the probes and `crashcheck.held` read it with nothing new.
GHOST_FILE: Final = 200
GHOST: Final = short_name(f"fuzz{GHOST_FILE}.bin")
FSI_NXT_FREE: Final = 492  # FSInfo: the cluster FatFs allocates after
BPB_FS_INFO: Final = 0x30  # FAT32 boot sector: FSInfo's sector number


def dirty_free_clusters(image: Path, count: int) -> frozenset[int]:
    """Fill the first `count` free clusters FatFs would allocate next with a
    directory sector whose first entry is a live GHOST, and return them.

    A free cluster may hold anything, and the store image's are all zero,
    which hides a directory linked to a cluster before it is zeroed. Here a
    directory that grows into one unzeroed shows GHOST, on the highest
    free cluster, which the FAT does not hold. File data landing on these
    clusters overwrites them whole, so only a directory can expose them.
    Allocation starts after FSInfo's next-free hint, so the search does too."""
    with _volume(image) as (volume, _, raw, start):
        fat = _fat(volume, raw, start)
        size = volume.bpb_header["BPB_BytsPerSec"]
        (fsinfo,) = struct.unpack_from("<H", raw, start + BPB_FS_INFO)
        (hint,) = struct.unpack_from("<I", raw, start + fsinfo * size + FSI_NXT_FREE)
        first = hint + 1 if FIRST_CLUSTER <= hint < fat.entries else FIRST_CLUSTER

        def free(c: int) -> bool:
            (link,) = struct.unpack_from("<I", fat.table, c * FAT32_ENTRY)
            return int(link) & ENTRY_MASK == 0

        candidates = [c for c in range(first, fat.entries) if free(c)]
        ghost_at = next(c for c in reversed(range(fat.entries)) if free(c))
        dirty = candidates[:count]
        if len(dirty) < count or ghost_at in dirty:
            raise ValueError(f"{image}: not {count} free clusters to dirty")
        at = {c: start + volume.get_data_cluster_address(c) for c in dirty}
    entry = bytearray(ENTRY)
    entry[:11] = GHOST
    entry[11] = 0x20  # archive
    entry[20:22] = (ghost_at >> 16).to_bytes(2, "little")
    entry[26:28] = (ghost_at & 0xFFFF).to_bytes(2, "little")
    entry[28:32] = size.to_bytes(4, "little")
    sector = bytes(entry).ljust(size, b"\0")
    with image.open("r+b") as disk:
        for c in dirty:
            disk.seek(at[c])
            disk.write(sector)
    return frozenset(dirty)


ATTR_VOLUME_ID: Final = 0x08
ATTR_DIRECTORY: Final = 0x10
BAD_CLUSTER: Final = 0x0FFFFFF7


@dataclass(frozen=True, slots=True)
class Accounting:
    """Every cluster of a volume in exactly one class: on the chain of the
    root or of an entry under it, free, marked bad, or allocated with no entry
    reaching it (fsck.fat's lost clusters)."""

    owned: frozenset[int]
    free: frozenset[int]
    bad: frozenset[int]
    unowned: frozenset[int]


def accounting(image: Path) -> Accounting:
    """Classify every cluster. The walk covers the whole tree, since the store
    disk keeps OTP under subdirectories, and reads directories itself with a
    guard, since a crash can leave a directory chain that loops. A broken
    chain owns nothing: `judge` reports it."""
    with _volume(image) as (volume, _, raw, start):
        fat = _fat(volume, raw, start)

        def link(c: int) -> int:
            (value,) = struct.unpack_from("<I", fat.table, c * FAT32_ENTRY)
            return int(value) & ENTRY_MASK

        def directory(chain: tuple[int, ...]) -> Iterator[bytes]:
            for c in chain:
                at = start + volume.get_data_cluster_address(c)
                for k in range(at, at + volume.bytes_per_cluster, ENTRY):
                    if raw[k] == 0:
                        return  # past the last entry
                    yield raw[k : k + ENTRY]

        owned: set[int] = set()
        pending = [volume.bpb_header["BPB_RootClus"]]
        while pending:
            chain = _chain(fat, pending.pop()) or ()
            if owned.intersection(chain):
                continue  # a directory reached twice
            owned.update(chain)
            for entry in directory(chain):
                attr = entry[11]
                if entry[0] == DELETED or attr & 0x3F == LONG_NAME or attr & ATTR_VOLUME_ID:
                    continue
                if attr & ATTR_DIRECTORY:
                    if entry[0] != ord("."):  # "." and ".." point back up
                        pending.append(entry_cluster(entry))
                else:
                    owned.update(_chain(fat, entry_cluster(entry)) or ())
        every = range(FIRST_CLUSTER, fat.entries)
        free = frozenset(c for c in every if link(c) == 0)
        bad = frozenset(c for c in every if link(c) == BAD_CLUSTER)
        held = frozenset(owned)
        return Accounting(held, free, bad, frozenset(every) - held - free - bad)


def cluster_bytes(image: Path, cluster: int) -> range:
    """The image's byte offsets that hold `cluster`."""
    with _volume(image) as (volume, _, _, start):
        at = start + volume.get_data_cluster_address(cluster)
        return range(at, at + volume.bytes_per_cluster)


def entry_cluster(raw: bytes) -> int:
    """A short entry's start cluster. FAT32 splits it in two little-endian
    words: the high one at byte 20 (DIR_FstClusHI) and the low one at byte 26
    (DIR_FstClusLO)."""
    return int.from_bytes(raw[20:22], "little") << 16 | int.from_bytes(raw[26:28], "little")


def sfn_checksum(sfn: bytes) -> int:
    """The checksum every LFN entry stores (byte 13) of the 11-byte short name
    it belongs to, as FatFs's sum_sfn computes it. It ties a long name to its
    short entry, so a stray LFN entry left by a deleted file is not taken as
    part of the next one."""
    total = 0
    for byte in sfn:
        # Rotate the 8-bit sum right by one, then add the next byte, mod 256.
        rotated = ((total & 1) << 7) | (total >> 1)
        total = (rotated + byte) & 0xFF
    return total


def intent_for(image: Path, src: str) -> Record:
    """The record the fork's f_rename writes to rename root file `src` within
    the root, derived from the image alone, never from the fork's own bytes,
    so comparing the two checks the C encoder.

    A FAT file is one short entry, possibly behind a run of long name (LFN)
    entries that spell its long name. FatFs's `dptr` is the short entry's byte
    offset in the directory. Its `blk_ofs` is the offset of the run's first
    LFN entry, or NO_LFN when there is none (FatFs stores `fuzz1.bin` with no
    LFN, through the case bits). A mount removes from `blk_ofs` to `dptr`."""
    r = root(image)
    raw = b"".join(r.sectors)  # the root directory, its sectors in chain order
    offsets = range(0, len(raw), ENTRY)
    want = short_name(src)

    def live(at: int) -> bool:
        # Byte 0 is 0 past the last entry, and 0xE5 for a deleted one.
        return raw[at] not in (0, DELETED)

    def is_long(at: int) -> bool:
        # Byte 11 is the attribute, which marks an LFN entry as 0x0F.
        return raw[at + 11] & 0x3F == LONG_NAME

    dptr = next(at for at in offsets if live(at) and not is_long(at) and raw[at : at + 11] == want)
    # Walk back over the live LFN entries that name this short entry.
    blk_ofs = dptr
    while blk_ofs > 0 and live(prev := blk_ofs - ENTRY) and is_long(prev):
        if raw[prev + 13] != sfn_checksum(want):
            break  # another file's long name
        blk_ofs = prev
    return Record(
        old_dir=r.cluster, blk_ofs=NO_LFN if blk_ofs == dptr else blk_ofs, dptr=dptr,
        sfn=want, cluster=entry_cluster(raw[dptr : dptr + ENTRY]), new_dir=r.cluster,
    )  # fmt: skip


def write_record(image: Path, record: Record) -> None:
    """Write `record` over the last reserved sector, where the fork keeps it.
    That sector is `BPB_RsvdSecCnt - 1` sectors into the partition, whose
    start the MBR gives. A booted mount then acts on it as on one the fork
    wrote, which is how the smoke checks the C decoder."""
    with image.open("r+b") as disk:
        start = partition_start(disk.read(SECTOR))
        disk.seek(start)
        boot = disk.read(SECTOR)
        (size,) = struct.unpack_from("<H", boot, BPB_BYTS_PER_SEC)
        (reserved,) = struct.unpack_from("<H", boot, BPB_RSVD_SEC_CNT)
        disk.seek(start + (reserved - 1) * size)
        disk.write(record.encode(size))


@dataclass(frozen=True, slots=True)
class _Fat:
    """The first FAT copy and the number of entries that name a cluster."""

    table: memoryview
    entries: int


def _fat(volume: Any, raw: bytes, start: int) -> _Fat:
    bpb = volume.bpb_header
    per_sector = bpb["BPB_BytsPerSec"]
    at = start + bpb["BPB_RsvdSecCnt"] * per_sector
    table = memoryview(raw)[at : at + bpb["BPB_FATSz32"] * per_sector]
    data_sectors = (
        bpb["BPB_TotSec32"] - (at - start) // per_sector - (bpb["BPB_NumFATs"] * bpb["BPB_FATSz32"])
    )
    clusters = data_sectors // bpb["BPB_SecPerClus"] + FIRST_CLUSTER
    return _Fat(table, min(clusters, len(table) // FAT32_ENTRY))


def _chain(fat: _Fat, first: int) -> Chain:
    """The clusters from `first` to the end mark. pyfatfs's own walk has no
    loop guard, and a crash can leave a cyclic FAT."""
    if first < FIRST_CLUSTER:
        return ()  # an empty file has no cluster
    chain: list[int] = []
    seen: set[int] = set()
    link = first
    while link < END_OF_CHAIN:
        if link < FIRST_CLUSTER or link >= fat.entries or link in seen:
            return None
        seen.add(link)
        chain.append(link)
        (link,) = struct.unpack_from("<I", fat.table, link * FAT32_ENTRY)
        link &= ENTRY_MASK
    return tuple(chain)


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
