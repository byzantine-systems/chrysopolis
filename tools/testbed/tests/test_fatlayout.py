"""The FAT32 layout reader against mtools and the raw boot sector, never
pyfatfs itself. Planted defects:

| Planted defect | Caught by |
|---|---|
| a chain walk with no loop guard | test_a_looping_chain_reads_as_broken (MemoryError) |
| a `judge` that ignores shared clusters | test_two_entries_on_one_cluster_are_a_problem_... |
| a `judge` without the cut rename's joint rule | test_a_cut_rename_must_keep_its_file_... |
| `intent_for` with `blk_ofs = dptr` | test_a_long_name_record_starts_at_its_first_lfn_entry |
| the ghost written at slot 1 | test_a_leftover_linked_unzeroed_shows_the_ghost |
| the free search from cluster 2 whatever the hint | test_the_dirtied_clusters_are_the_ones_... |
| `free_slots` counting only never-used entries | test_free_slots_count_never_used_and_... |
| `accounting` not walking subdirectories | test_every_cluster_is_owned_free_bad_or_lost |
| `accounting` leaving free clusters out of `free` | test_every_cluster_is_owned_free_bad_or_lost |
"""

from __future__ import annotations

import re
import shutil
import struct
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import Final

import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from chryso_testbed.crashcheck import (
    Content,
    Move,
    after_mount,
    as_volume,
    fsck,
    judge,
    judge_after_mount,
)
from chryso_testbed.fatlayout import (
    ENTRY,
    GHOST,
    NO_LFN,
    SECTOR,
    accounting,
    chains,
    contents,
    dirty_free_clusters,
    entry_cluster,
    intent_for,
    layout,
    partition_start,
    root,
    sfn_checksum,
    short_name,
    write_record,
)
from chryso_testbed.renaming import Record

START_LBA: Final = 2048
# FAT32 needs 65525 clusters: 40 MiB at 512-byte sectors, 320 MiB (sparse) at 4 KiB.
VOLUME_BYTES: Final = {512: 40 * 1024 * 1024, 4096: 320 * 1024 * 1024}
sector_sizes = st.sampled_from(sorted(VOLUME_BYTES))  # packages.disk uses 4096
FREE: Final = 0

needs_tools = pytest.mark.skipif(
    not all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mshowfat")),
    reason="needs dosfstools and mtools",
)
names = st.from_regex(r"[a-z][a-z0-9]{0,7}\.bin", fullmatch=True)
file_bytes = st.binary(min_size=1, max_size=4 * SECTOR)


def mbr(start_lba: int, sectors: int) -> bytes:
    """One FAT32 (LBA) partition entry and the signature, nothing else."""
    entry = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0C, b"\0\0\0", start_lba, sectors)
    return bytes(0x1BE) + entry + bytes(48) + b"\x55\xaa"


def mtools(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def volume(at: Path, files: dict[str, bytes], sector_size: int = SECTOR) -> tuple[Path, str]:
    """A FAT32 volume of `sector_size`-byte sectors (one per cluster at 4 KiB)
    in an MBR partition at START_LBA, holding `files`."""
    image = at / "disk.img"
    size = VOLUME_BYTES[sector_size]
    with image.open("wb") as disk:
        disk.write(mbr(START_LBA, size // SECTOR))
        disk.truncate(START_LBA * SECTOR + size)
    per_cluster = ["-s", "1"] if sector_size > SECTOR else []
    mtools("mkfs.fat", "-F", "32", "-S", str(sector_size), *per_cluster,
           "--offset", str(START_LBA * SECTOR // sector_size), str(image))  # fmt: skip
    target = f"{image}@@{START_LBA * SECTOR}"
    for name, data in files.items():
        (at / name).write_bytes(data)
        mtools("mcopy", "-i", target, str(at / name), f"::/{name}")
    return image, target


def first_cluster(target: str, name: str) -> int:
    """mshowfat prints a chain as `::/a.bin <3-5>`."""
    chain = re.search(r"<(\d+)", mtools("mshowfat", "-i", target, f"::/{name}"))
    assert chain is not None
    return int(chain[1])


@given(start=st.integers(1, 2**31), sectors=st.integers(1, 2**31))
def test_partition_start_reads_the_first_entry(start: int, sectors: int) -> None:
    assert partition_start(mbr(start, sectors)) == start * SECTOR


@given(sector=st.binary(min_size=SECTOR, max_size=SECTOR).filter(lambda s: s[510:] != b"\x55\xaa"))
def test_a_sector_without_the_signature_is_not_an_mbr(sector: bytes) -> None:
    with pytest.raises(ValueError, match="MBR"):
        partition_start(sector)


@needs_tools
@settings(
    max_examples=15, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=1, max_size=4), sector_size=sector_sizes)
def test_each_extent_is_where_mtools_and_the_bpb_put_it(
    tmp_path: Path, files: dict[str, bytes], sector_size: int
) -> None:
    """Extents are 512-byte device sectors, whatever the volume's sector size."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files, sector_size)
    raw = image.read_bytes()
    boot = START_LBA * SECTOR
    per_sector, per_cluster, reserved, fats = struct.unpack_from("<HBHB", raw, boot + 0x0B)
    (fat_sectors,) = struct.unpack_from("<I", raw, boot + 0x24)
    assert per_sector == sector_size
    first_data_byte = boot + (reserved + fats * fat_sectors) * per_sector
    found = layout(image)

    def at(sector: int) -> bytes:
        return raw[sector * SECTOR : (sector + 1) * SECTOR]

    for name, data in files.items():
        cluster = first_cluster(target, name)
        data_sector = (first_data_byte + (cluster - 2) * per_cluster * per_sector) // SECTOR
        fat_byte = boot + reserved * per_sector + cluster * 4
        assert found.sector("data", name) == data_sector
        assert found.sector("fat", name) == fat_byte // SECTOR
        assert struct.unpack_from("<I", raw, fat_byte)[0] & 0x0FFFFFFF != FREE
        entry = found.sector("entry", name)
        assert entry is not None
        assert short_name(name) in at(entry)
        assert at(data_sector)[: len(data)] == data[:SECTOR]
    assert found.sector("data", "absent.bin") is None
    assert contents(image) == {short_name(name): data for name, data in files.items()}


@needs_tools
@settings(
    max_examples=10, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(
    files=st.dictionaries(names, file_bytes, min_size=2, max_size=4),
    data=st.data(),
    sector_size=sector_sizes,
)
def test_a_freed_cluster_breaks_only_its_file(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject, sector_size: int
) -> None:
    """A crash can leave an entry pointing at a freed cluster. That file
    reads as broken, and every other file still reads back."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files, sector_size)
    victim = data.draw(st.sampled_from(sorted(files)))
    free_first_cluster(image, target, victim, every_copy=True)
    found = contents(image)
    assert found[short_name(victim)] is None
    for name, payload in files.items():
        if name != victim:
            assert found[short_name(name)] == payload
    verdict = judge(image, {})
    assert verdict.broken == (short_name(victim).decode(),)
    assert verdict.problems, "a broken chain is a problem whatever was promised"
    assert not verdict.diverged


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=1, max_size=3), data=st.data())
def test_a_crash_between_the_fat_copies_is_reported(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject
) -> None:
    """A change to the first FAT alone reads as diverged copies."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files)
    victim = data.draw(st.sampled_from(sorted(files)))
    free_first_cluster(image, target, victim, every_copy=False)
    verdict = judge(image, {})
    assert verdict.diverged
    assert verdict.problems


def free_first_cluster(image: Path, target: str, victim: str, *, every_copy: bool) -> None:
    """Mark `victim`'s first cluster free in the first FAT, or in every copy
    as FatFs's mirrored writes would."""
    boot = START_LBA * SECTOR
    raw = image.read_bytes()
    (per_sector,) = struct.unpack_from("<H", raw, boot + 0x0B)
    (reserved,) = struct.unpack_from("<H", raw, boot + 0x0E)
    (copies,) = struct.unpack_from("<B", raw, boot + 0x10)
    (per_fat,) = struct.unpack_from("<I", raw, boot + 0x24)
    entry = first_cluster(target, victim) * 4
    with image.open("r+b") as disk:
        for copy in range(copies if every_copy else 1):
            disk.seek(boot + (reserved + copy * per_fat) * per_sector + entry)
            disk.write(bytes(4))  # FREE_CLUSTER


def chain_of(target: str, name: str) -> tuple[int, ...]:
    """mshowfat prints a chain as runs, `<3-5><9>`."""
    runs = re.findall(r"<(\d+)(?:-(\d+))?>", mtools("mshowfat", "-i", target, f"::/{name}"))
    return tuple(c for lo, hi in runs for c in range(int(lo), int(hi or lo) + 1))


def fat_entry(image: Path, cluster: int, value: int | None = None) -> int:
    """Read FAT entry `cluster` of the first copy, writing `value` first if given."""
    boot = START_LBA * SECTOR
    raw = image.read_bytes()
    (per_sector,) = struct.unpack_from("<H", raw, boot + 0x0B)
    (reserved,) = struct.unpack_from("<H", raw, boot + 0x0E)
    at = boot + reserved * per_sector + cluster * 4
    if value is not None:
        with image.open("r+b") as disk:
            disk.seek(at)
            disk.write(struct.pack("<I", value))
        return value
    return int(struct.unpack_from("<I", raw, at)[0]) & 0x0FFFFFFF


def point_entry(image: Path, name: str, cluster: int) -> None:
    """Point `name`'s directory entry at `cluster`, as a crash mid-rename or a
    cross-linked allocation could leave it."""
    raw = image.read_bytes()
    at = raw.index(short_name(name), START_LBA * SECTOR)
    with image.open("r+b") as disk:
        disk.seek(at + 20)
        disk.write(struct.pack("<H", cluster >> 16))
        disk.seek(at + 26)
        disk.write(struct.pack("<H", cluster & 0xFFFF))


@needs_tools
@settings(
    max_examples=10, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(
    files=st.dictionaries(
        names, st.binary(min_size=1, max_size=12 * SECTOR), min_size=1, max_size=4
    ),
    sector_size=sector_sizes,
)
def test_each_chain_is_the_one_mtools_reads(
    tmp_path: Path, files: dict[str, bytes], sector_size: int
) -> None:
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files, sector_size)
    found = chains(image)
    assert found == {short_name(n): chain_of(target, n) for n in files}
    assert not judge(image, {}).problems


@needs_tools
@settings(
    max_examples=10, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=1, max_size=4), data=st.data())
def test_a_looping_chain_reads_as_broken(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject
) -> None:
    """A crash can leave a FAT whose chain loops. Reading it must end, and the
    file reads as broken."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files, 4096)
    victim = data.draw(st.sampled_from(sorted(files)))
    chain = chain_of(target, victim)
    fat_entry(image, chain[-1], data.draw(st.sampled_from(chain)))
    assert chains(image)[short_name(victim)] is None
    verdict = judge(image, {})
    assert verdict.broken == (short_name(victim).decode(),)


@needs_tools
@settings(
    max_examples=10, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=2, max_size=4), data=st.data())
def test_two_entries_on_one_cluster_are_a_problem_unless_a_cut_rename(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject
) -> None:
    """Entries sharing clusters are a cross-link: a crash bug, except a rename
    the crash cut between registering the new name and removing the old."""
    for old in tmp_path.iterdir():
        old.unlink()
    named = {f"fuzz{i}.bin": payload for i, payload in enumerate(files.values())}
    image, target = volume(tmp_path, named, 4096)
    src, dst = data.draw(st.permutations(range(len(named))))[:2]
    point_entry(image, f"fuzz{dst}.bin", chain_of(target, f"fuzz{src}.bin")[0])
    plain = judge(image, {})
    assert any("share clusters" in p for p in plain.problems)
    assert not plain.crosslinked
    cut = judge(image, {}, cut=[Move(src, dst, frozenset({"garbage"}))])
    assert not any("share clusters" in p for p in cut.problems)
    assert cut.crosslinked == ((src, dst),)


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=1, max_size=2), seed=st.integers(0, 255))
def test_a_cut_rename_must_keep_its_file_under_one_name(
    tmp_path: Path, files: dict[str, bytes], seed: int
) -> None:
    """Each name may be absent alone, so only the joint rule sees the loss."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, _ = volume(tmp_path, files, 4096)  # fuzz names are not in `names`
    either: frozenset[Content] = frozenset({seed, "absent"})
    expected = {1: either, 2: either}
    assert not judge(image, expected).problems
    lost = judge(image, expected, cut=[Move(1, 2, frozenset({seed}))])
    assert lost.problems == ("fuzz1.bin -> fuzz2.bin: lost under both names",)


@needs_tools
@pytest.mark.parametrize("name", ["a_long_name.bin", "an_even_longer_file_name.bin"])
def test_a_long_name_record_starts_at_its_first_lfn_entry(tmp_path: Path, name: str) -> None:
    """mtools writes one LFN entry per 13 characters in front of the short
    entry, and a mount must remove them all, so blk_ofs is the first of them."""
    image, target = volume(tmp_path, {name: b"x" * 10}, 4096)
    raw = b"".join(root(image).sectors)
    cluster = chain_of(target, name)[0]
    dptr = next(
        at
        for at in range(0, len(raw), ENTRY)
        if raw[at] and raw[at + 11] != 0x0F and entry_cluster(raw[at : at + ENTRY]) == cluster
    )
    sfn = raw[dptr : dptr + 11]
    stored = f"{sfn[:8].decode().rstrip()}.{sfn[8:].decode().rstrip()}"
    lfn_entries = -(-len(name) // 13)
    record = intent_for(image, stored)
    assert record.dptr == dptr
    assert record.blk_ofs == dptr - lfn_entries * ENTRY
    assert raw[record.blk_ofs + 13] == sfn_checksum(sfn)
    assert record.cluster == cluster


@needs_tools
def test_a_short_name_record_has_no_lfn(tmp_path: Path) -> None:
    """A name stored upper case in 8.3 needs no long name entry."""
    image, target = volume(tmp_path, {"FUZZ1.BIN": b"x"}, 4096)
    record = intent_for(image, "FUZZ1.BIN")
    assert record.blk_ofs == NO_LFN
    assert record.cluster == chain_of(target, "FUZZ1.BIN")[0]


@needs_tools
def test_a_written_record_reads_back(tmp_path: Path) -> None:
    image, _ = volume(tmp_path, {"FUZZ1.BIN": b"x"}, 4096)
    record = intent_for(image, "FUZZ1.BIN")
    write_record(image, record)
    assert Record.decode(root(image).record) == record


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=1, max_size=4))
def test_the_model_reads_the_files_mtools_wrote(tmp_path: Path, files: dict[str, bytes]) -> None:
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files, 4096)
    model = as_volume(root(image))
    assert model.record is None
    assert model.names() == {
        f"root/{short_name(n).decode()}": first_cluster(target, n) for n in files
    }


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, min_size=2, max_size=4), data=st.data())
def test_a_mount_finishes_a_cut_rename_its_record_names(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject
) -> None:
    """Two names on one chain, as a rename cut after registering the new one:
    with the record the mount drops the old name, without it nothing does."""
    for old in tmp_path.iterdir():
        old.unlink()
    named = {f"fuzz{i}.bin": payload for i, payload in enumerate(files.values())}
    image, target = volume(tmp_path, named, 4096)
    src, dst = (f"fuzz{i}.bin" for i in data.draw(st.permutations(range(len(named))))[:2])
    cluster = chain_of(target, src)[0]
    point_entry(image, dst, cluster)
    assert judge_after_mount(image), "a cross-link with no record stays"
    assert short_name(src) in after_mount(image)
    record = intent_for(image, src)
    write_record(image, replace(record, cluster=record.cluster + 1))
    assert judge_after_mount(image), "a record on another chain finishes nothing"
    write_record(image, record)
    assert judge_after_mount(image) == ()
    mounted = after_mount(image)
    assert short_name(src) not in mounted
    assert short_name(dst) in mounted


# --- leftover directory sectors in free clusters (W9) ------------------------------------


def fat_table(image: Path) -> list[int]:
    """Every entry of the first FAT copy, masked, read from the boot sector's
    own fields."""
    boot = START_LBA * SECTOR
    with image.open("rb") as disk:
        disk.seek(boot)
        bpb = disk.read(SECTOR)
        (per_sector,) = struct.unpack_from("<H", bpb, 0x0B)
        (reserved,) = struct.unpack_from("<H", bpb, 0x0E)
        (per_fat,) = struct.unpack_from("<I", bpb, 0x24)
        disk.seek(boot + reserved * per_sector)
        table = disk.read(per_fat * per_sector)
    return [e & 0x0FFFFFFF for (e,) in struct.iter_unpack("<I", table)]


def set_fat(image: Path, cluster: int, value: int) -> None:
    """Write FAT entry `cluster` in every copy, as FatFs's mirrored writes do."""
    boot = START_LBA * SECTOR
    with image.open("r+b") as disk:
        disk.seek(boot)
        bpb = disk.read(SECTOR)
        (per_sector,) = struct.unpack_from("<H", bpb, 0x0B)
        (reserved,) = struct.unpack_from("<H", bpb, 0x0E)
        (copies,) = struct.unpack_from("<B", bpb, 0x10)
        (per_fat,) = struct.unpack_from("<I", bpb, 0x24)
        for copy in range(copies):
            disk.seek(boot + (reserved + copy * per_fat) * per_sector + cluster * 4)
            disk.write(struct.pack("<I", value))


def set_next_free(image: Path, hint: int) -> None:
    """Write FSInfo's next-free hint (FSI_Nxt_Free, byte 492 of the sector
    BPB_FSInfo names)."""
    boot = START_LBA * SECTOR
    with image.open("r+b") as disk:
        disk.seek(boot)
        bpb = disk.read(SECTOR)
        (per_sector,) = struct.unpack_from("<H", bpb, 0x0B)
        (fsinfo,) = struct.unpack_from("<H", bpb, 0x30)
        disk.seek(boot + fsinfo * per_sector + 492)
        disk.write(struct.pack("<I", hint))


def zero_cluster(image: Path, cluster: int) -> None:
    boot = START_LBA * SECTOR
    raw = image.read_bytes()
    per_sector, per_cluster, reserved, fats = struct.unpack_from("<HBHB", raw, boot + 0x0B)
    (per_fat,) = struct.unpack_from("<I", raw, boot + 0x24)
    data = boot + (reserved + fats * per_fat) * per_sector
    with image.open("r+b") as disk:
        disk.seek(data + (cluster - 2) * per_cluster * per_sector)
        disk.write(bytes(per_cluster * per_sector))


def grow_root(image: Path, cluster: int) -> None:
    """Link `cluster` to the end of the root's chain and end the chain there."""
    set_fat(image, root(image).clusters[-1], cluster)
    set_fat(image, cluster, 0x0FFFFFFF)


needs_fsck = pytest.mark.skipif(shutil.which("fsck.fat") is None, reason="needs dosfstools")


@needs_tools
@needs_fsck
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(
    files=st.dictionaries(names, file_bytes, max_size=3),
    count=st.integers(1, 40),
    sector_size=sector_sizes,
)
def test_leftovers_in_free_clusters_are_invisible(
    tmp_path: Path, files: dict[str, bytes], count: int, sector_size: int
) -> None:
    """A free cluster may hold anything: dirtying the free ones changes no
    file, no chain, no root sector and no verdict, and fsck.fat stays clean."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, _ = volume(tmp_path, files, sector_size)
    before = contents(image), chains(image), root(image), judge(image, {})
    dirty_free_clusters(image, count)
    assert (contents(image), chains(image), root(image), judge(image, {})) == before
    assert fsck(image, tmp_path) == 0


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(files=st.dictionaries(names, file_bytes, max_size=3), sector_size=sector_sizes)
def test_a_leftover_linked_unzeroed_shows_the_ghost(
    tmp_path: Path, files: dict[str, bytes], sector_size: int
) -> None:
    """The root grown into a dirtied cluster without zeroing it shows the
    ghost on clusters the FAT does not hold. Zeroed first, nothing shows."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, _ = volume(tmp_path, files, sector_size)
    (grown,) = dirty_free_clusters(image, 1)
    unzeroed = tmp_path / "unzeroed.img"
    shutil.copyfile(image, unzeroed)
    grow_root(unzeroed, grown)
    zero_cluster(image, grown)
    grow_root(image, grown)
    ghost = GHOST.decode()
    assert judge(unzeroed, {}).problems == (f"{ghost}: entry on clusters the FAT does not hold",)
    assert judge(image, {}).problems == ()


@needs_tools
@settings(
    max_examples=10, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(
    files=st.dictionaries(names, file_bytes, max_size=3),
    data=st.data(),
    count=st.integers(1, 8),
    sector_size=sector_sizes,
)
def test_the_dirtied_clusters_are_the_ones_fatfs_takes_next(
    tmp_path: Path, files: dict[str, bytes], data: st.DataObject, count: int, sector_size: int
) -> None:
    """FatFs's create_chain(obj, 0) starts from last_clst, FSInfo's hint when
    it names a cluster, else 1, and takes the first free cluster above it."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, _ = volume(tmp_path, files, sector_size)
    fat = fat_table(image)
    top = max(c for c, link in enumerate(fat) if c >= 2 and link != FREE)
    # Past the last used cluster, so a free one lies at or below a valid hint.
    hint = data.draw(st.one_of(st.just(0xFFFFFFFF), st.integers(top + 1, top + 64)), "hint")
    set_next_free(image, hint)
    after = hint if 2 <= hint < len(fat) else 1
    want = [c for c in range(after + 1, len(fat)) if fat[c] == FREE][:count]
    assert dirty_free_clusters(image, count) == frozenset(want)


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(added=st.integers(1, 6), data=st.data())
def test_free_slots_count_never_used_and_deleted_entries(
    tmp_path: Path, added: int, data: st.DataObject
) -> None:
    """Each 8.3 name mtools adds takes one slot, and each it deletes gives one
    back as 0xE5, which a new name may reuse."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, {}, 4096)
    empty = root(image).free_slots()
    stored = [f"F{i}.BIN" for i in range(added)]  # upper case 8.3: no long name entry
    for name in stored:
        (tmp_path / name).write_bytes(b"x")
        mtools("mcopy", "-i", target, str(tmp_path / name), f"::/{name}")
    assert root(image).free_slots() == empty - added
    gone = data.draw(st.integers(1, added), "deleted")
    for name in stored[:gone]:
        mtools("mdel", "-i", target, f"::/{name}")
    assert root(image).free_slots() == empty - added + gone


@needs_tools
@settings(
    max_examples=5, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture]
)
@given(
    top=st.dictionaries(names, file_bytes, max_size=3),
    nested=st.dictionaries(names, file_bytes, min_size=1, max_size=3),
    data=st.data(),
    sector_size=sector_sizes,
)
def test_every_cluster_is_owned_free_bad_or_lost(
    tmp_path: Path,
    top: dict[str, bytes],
    nested: dict[str, bytes],
    data: st.DataObject,
    sector_size: int,
) -> None:
    """Conservation: the classes split the volume. On an image mtools wrote
    every allocated cluster is on a chain mtools reports, files in a
    subdirectory included, and an end mark planted on a free cluster is lost
    and nothing else is."""
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, top, sector_size)
    mtools("mmd", "-i", target, "::/sub")
    for name, payload in nested.items():
        (tmp_path / name).write_bytes(payload)
        mtools("mcopy", "-o", "-i", target, str(tmp_path / name), f"::/sub/{name}")
    files = [*top, *(f"sub/{n}" for n in nested), "sub"]
    chained = set(root(image).clusters).union(*(chain_of(target, f) for f in files))
    clean = accounting(image)
    assert clean.owned == chained
    assert clean.unowned == set()
    classes = (clean.owned, clean.free, clean.bad, clean.unowned)
    assert sum(map(len, classes)) == len(frozenset().union(*classes))  # pairwise disjoint
    fat = fat_table(image)
    first_free = next(c for c in range(2, len(fat)) if fat[c] == FREE)
    lost = data.draw(st.sets(st.integers(first_free, first_free + 32), max_size=4), "lost")
    lost = {c for c in lost if fat[c] == FREE}
    for c in lost:
        set_fat(image, c, 0x0FFFFFFF)
    after = accounting(image)
    assert after.unowned == lost
    assert after.owned == clean.owned
    assert after.free == clean.free - lost
