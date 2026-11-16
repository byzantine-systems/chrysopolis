"""Where a FAT32 file keeps its data, FAT entry and directory entry, in disk
sectors, for IRON-style targeted faults, and what a crash image holds.

The volume is parsed here from the boot sector, the FAT and the directory
entries, read through a `Disk`: an image on file or a crash state built in
memory. A reader asks only for the sectors it needs, so judging a crash
state never reads the whole image. tests/test_fatlayout.py holds the parser
to mtools and, as a differential, to pyfatfs.
"""

from __future__ import annotations

import struct
from collections.abc import Iterator, Mapping
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from types import MappingProxyType
from typing import Final, Literal

from chryso_testbed.disk import Disk, FileDisk
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


# Boot sector fields past the two above (FAT32 layout).
BPB_SEC_PER_CLUS: Final = 0x0D
BPB_NUM_FATS: Final = 0x10
BPB_ROOT_ENT_CNT: Final = 0x11
BPB_FAT_SZ16: Final = 0x16
BPB_TOT_SEC32: Final = 0x20
BPB_FAT_SZ32: Final = 0x24
BPB_ROOT_CLUS: Final = 0x2C
MIN_FAT32_CLUSTERS: Final = 65525  # fewer makes the volume FAT12 or FAT16


@dataclass(frozen=True)
class _Volume:
    """A FAT32 volume on `disk`: its partition's byte offset and the boot
    sector fields every reader needs."""

    disk: Disk
    start: int
    sector: int
    per_cluster: int  # sectors
    reserved: int
    fats: int
    fat_sectors: int
    root_cluster: int
    total_sectors: int

    @classmethod
    def open(cls, image: Path | Disk) -> _Volume:
        disk = FileDisk(image) if isinstance(image, Path) else image
        start = partition_start(disk.read(0, SECTOR))
        boot = disk.read(start, SECTOR)
        if len(boot) < SECTOR:
            raise ValueError("no boot sector")
        sector, per_cluster, reserved, fats = struct.unpack_from("<HBHB", boot, BPB_BYTS_PER_SEC)
        (root_entries,) = struct.unpack_from("<H", boot, BPB_ROOT_ENT_CNT)
        (fat16,) = struct.unpack_from("<H", boot, BPB_FAT_SZ16)
        (total,) = struct.unpack_from("<I", boot, BPB_TOT_SEC32)
        (fat32,) = struct.unpack_from("<I", boot, BPB_FAT_SZ32)
        (root,) = struct.unpack_from("<I", boot, BPB_ROOT_CLUS)
        if sector not in (512, 1024, 2048, 4096) or per_cluster == 0 or fats == 0:
            raise ValueError(f"not a FAT boot sector: {sector}-byte sectors, {fats} FATs")
        volume = cls(disk, start, sector, per_cluster, reserved, fats, fat32, root, total)
        if root_entries or fat16 or not fat32 or volume.clusters < MIN_FAT32_CLUSTERS:
            raise ValueError(f"not FAT32: {volume.clusters} clusters")
        return volume

    @property
    def cluster_size(self) -> int:
        return self.per_cluster * self.sector

    @property
    def data_start(self) -> int:
        """The first data sector's byte offset on the disk."""
        return self.start + (self.reserved + self.fats * self.fat_sectors) * self.sector

    @property
    def clusters(self) -> int:
        """The data clusters the boot sector counts."""
        data = self.total_sectors - self.reserved - self.fats * self.fat_sectors
        return data // self.per_cluster

    def fat_copy(self, k: int) -> bytes:
        at = self.start + (self.reserved + k * self.fat_sectors) * self.sector
        return self.disk.read(at, self.fat_sectors * self.sector)

    @cached_property
    def fat(self) -> _Fat:
        """The first FAT copy, the one FatFs reads."""
        table = self.fat_copy(0)
        return _Fat(
            memoryview(table), min(self.clusters + FIRST_CLUSTER, len(table) // FAT32_ENTRY)
        )

    def cluster_at(self, c: int) -> int:
        """Cluster `c`'s byte offset on the disk."""
        return self.data_start + (c - FIRST_CLUSTER) * self.cluster_size

    def cluster(self, c: int) -> bytes:
        return self.disk.read(self.cluster_at(c), self.cluster_size)

    def entries(self, chain: tuple[int, ...]) -> Iterator[tuple[int, bytes]]:
        """A directory's entries in order, each with its byte offset on the
        disk, up to the first never-used one."""
        for c in chain:
            raw, at = self.cluster(c), self.cluster_at(c)
            for k in range(0, len(raw) - ENTRY + 1, ENTRY):
                if raw[k] == 0:
                    return
                yield at + k, raw[k : k + ENTRY]

    def files(self) -> Iterator[tuple[int, bytes]]:
        """The root directory's live short entries that name files, not
        directories, long names or the volume label."""
        chain = _chain(self.fat, self.root_cluster)
        if not chain.whole:
            raise ValueError("the root directory's chain is broken")
        for at, entry in self.entries(chain.clusters):
            attr = entry[11]
            # A long name entry's attribute (0x0F) has the label bit too, so
            # the mask below would drop it anyway. The test says so outright.
            if entry[0] == DELETED or attr & 0x3F == LONG_NAME:
                continue
            if not attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY):
                yield at, entry


def layout(image: Path | Disk) -> Layout:
    """Read a FAT32 disk image's layout. Only FAT32 is read, because that is
    what `packages.disk` formats."""
    v = _Volume.open(image)
    fat_start = v.start + v.reserved * v.sector
    files: dict[bytes, Extent] = {}
    for at, entry in v.files():
        if (cluster := entry_cluster(entry)) < FIRST_CLUSTER:
            continue
        files[entry[:11]] = Extent(
            data=v.cluster_at(cluster) // SECTOR,
            fat=(fat_start + cluster * FAT32_ENTRY) // SECTOR,
            entry=at // SECTOR,
        )
    return Layout(MappingProxyType(files))


@dataclass(frozen=True, slots=True)
class Chain:
    """A cluster chain as walked from its first cluster. A broken one (a free,
    bad or out-of-range link, or a loop) keeps the clusters walked before the
    break."""

    clusters: tuple[int, ...]
    whole: bool

    @property
    def owned(self) -> tuple[int, ...]:
        """The clusters the chain holds: all of them if whole, none if broken."""
        return self.clusters if self.whole else ()


def survey(image: Path | Disk) -> tuple[Mapping[bytes, bytes | None], Mapping[bytes, Chain]]:
    """Each root-level file's bytes and cluster chain, keyed by 8.3 name. The
    bytes are None where the chain is broken, as a crash can leave an entry on
    clusters already freed."""
    v = _Volume.open(image)
    found: dict[bytes, bytes | None] = {}
    links: dict[bytes, Chain] = {}
    for _, entry in v.files():
        name = entry[:11]
        links[name] = chain = _chain(v.fat, entry_cluster(entry))
        (size,) = struct.unpack_from("<I", entry, 28)
        found[name] = b"".join(map(v.cluster, chain.clusters))[:size] if chain.whole else None
    return MappingProxyType(found), MappingProxyType(links)


def fats_differ(image: Path | Disk) -> bool:
    """Whether any FAT copy differs from the first. packages.disk has one."""
    v = _Volume.open(image)
    first = v.fat_copy(0)
    return any(v.fat_copy(k) != first for k in range(1, v.fats))


def contents(image: Path | Disk) -> Mapping[bytes, bytes | None]:
    """Each root-level file's bytes, or None where its chain is broken."""
    return survey(image)[0]


def chains(image: Path | Disk) -> Mapping[bytes, Chain]:
    """Each root-level file's cluster chain."""
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


def root(image: Path | Disk) -> Root:
    v = _Volume.open(image)
    record = v.disk.read(v.start + (v.reserved - 1) * v.sector, v.sector)
    chain = _chain(v.fat, v.root_cluster).owned
    sectors: list[bytes] = []
    for cluster in chain:
        raw = v.cluster(cluster)
        sectors += [raw[k : k + v.sector] for k in range(0, v.cluster_size, v.sector)]
    return Root(v.root_cluster, v.sector, tuple(sectors), record, chain)


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
    v = _Volume.open(image)
    fat, size = v.fat, v.sector
    (fsinfo,) = struct.unpack_from("<H", v.disk.read(v.start + BPB_FS_INFO, 2))
    (hint,) = struct.unpack_from("<I", v.disk.read(v.start + fsinfo * size + FSI_NXT_FREE, 4))
    first = hint + 1 if FIRST_CLUSTER <= hint < fat.entries else FIRST_CLUSTER

    def free(c: int) -> bool:
        (link,) = struct.unpack_from("<I", fat.table, c * FAT32_ENTRY)
        return int(link) & ENTRY_MASK == 0

    candidates = [c for c in range(first, fat.entries) if free(c)]
    ghost_at = next(c for c in reversed(range(fat.entries)) if free(c))
    dirty = candidates[:count]
    if len(dirty) < count or ghost_at in dirty:
        raise ValueError(f"{image}: not {count} free clusters to dirty")
    at = {c: v.cluster_at(c) for c in dirty}
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


def accounting(image: Path | Disk) -> Accounting:
    """Classify every cluster. The walk covers the whole tree, since the store
    disk keeps OTP under subdirectories, and reads directories itself with a
    guard, since a crash can leave a directory chain that loops. A broken
    chain owns nothing: `judge` reports it."""
    v = _Volume.open(image)
    fat = v.fat
    links = [
        e & ENTRY_MASK for (e,) in struct.iter_unpack("<I", fat.table[: fat.entries * FAT32_ENTRY])
    ]
    owned: set[int] = set()
    pending = [v.root_cluster]
    while pending:
        chain = _chain(fat, pending.pop()).owned
        if owned.intersection(chain):
            continue  # a directory reached twice
        owned.update(chain)
        for _, entry in v.entries(chain):
            attr = entry[11]
            if entry[0] == DELETED or attr & 0x3F == LONG_NAME or attr & ATTR_VOLUME_ID:
                continue
            if attr & ATTR_DIRECTORY:
                if entry[0] != ord("."):  # "." and ".." point back up
                    pending.append(entry_cluster(entry))
            else:
                owned.update(_chain(fat, entry_cluster(entry)).owned)
    every = range(FIRST_CLUSTER, fat.entries)
    free = frozenset(c for c in every if links[c] == 0)
    bad = frozenset(c for c in every if links[c] == BAD_CLUSTER)
    held = frozenset(owned)
    return Accounting(held, free, bad, frozenset(every) - held - free - bad)


def cluster_bytes(image: Path | Disk, cluster: int) -> range:
    """The image's byte offsets that hold `cluster`."""
    v = _Volume.open(image)
    return range(v.cluster_at(cluster), v.cluster_at(cluster) + v.cluster_size)


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


def intent_for(image: Path | Disk, src: str) -> Record:
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


def _chain(fat: _Fat, first: int) -> Chain:
    """The clusters from `first` to the end mark, with a loop guard, since a
    crash can leave a cyclic FAT."""
    if first < FIRST_CLUSTER:
        return Chain((), whole=True)  # an empty file has no cluster
    chain: list[int] = []
    seen: set[int] = set()
    link = first
    while link < END_OF_CHAIN:
        if link < FIRST_CLUSTER or link >= fat.entries or link in seen:
            return Chain(tuple(chain), whole=False)
        seen.add(link)
        chain.append(link)
        (link,) = struct.unpack_from("<I", fat.table, link * FAT32_ENTRY)
        link &= ENTRY_MASK
    return Chain(tuple(chain), whole=True)
