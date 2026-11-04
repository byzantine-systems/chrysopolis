"""The FAT32 layout reader against mtools and the raw boot sector, never
pyfatfs itself.
"""

from __future__ import annotations

import re
import shutil
import struct
import subprocess
from pathlib import Path
from typing import Final

import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from chryso_testbed.fatlayout import SECTOR, layout, partition_start, short_name

START_LBA: Final = 2048
VOLUME_BYTES: Final = 40 * 1024 * 1024  # mkfs.fat needs about 33 MiB for FAT32
FREE: Final = 0

needs_tools = pytest.mark.skipif(
    not all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mshowfat")),
    reason="needs dosfstools and mtools",
)
names = st.from_regex(r"[a-z][a-z0-9]{0,7}\.bin", fullmatch=True)
contents = st.binary(min_size=1, max_size=4 * SECTOR)


def mbr(start_lba: int, sectors: int) -> bytes:
    """One FAT32 (LBA) partition entry and the signature, nothing else."""
    entry = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0C, b"\0\0\0", start_lba, sectors)
    return bytes(0x1BE) + entry + bytes(48) + b"\x55\xaa"


def mtools(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def volume(at: Path, files: dict[str, bytes]) -> tuple[Path, str]:
    image = at / "disk.img"
    with image.open("wb") as disk:
        disk.write(mbr(START_LBA, VOLUME_BYTES // SECTOR))
        disk.truncate(START_LBA * SECTOR + VOLUME_BYTES)
    mtools("mkfs.fat", "-F", "32", "-S", "512", "--offset", str(START_LBA), str(image))
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
@given(files=st.dictionaries(names, contents, min_size=1, max_size=4))
def test_each_extent_is_where_mtools_and_the_bpb_put_it(
    tmp_path: Path, files: dict[str, bytes]
) -> None:
    for old in tmp_path.iterdir():
        old.unlink()
    image, target = volume(tmp_path, files)
    raw = image.read_bytes()
    boot = START_LBA * SECTOR
    per_sector, per_cluster, reserved, fats = struct.unpack_from("<HBHB", raw, boot + 0x0B)
    (fat_sectors,) = struct.unpack_from("<I", raw, boot + 0x24)
    assert per_sector == SECTOR
    first_data = START_LBA + reserved + fats * fat_sectors
    found = layout(image)

    def at(sector: int) -> bytes:
        return raw[sector * SECTOR : (sector + 1) * SECTOR]

    for name, data in files.items():
        cluster = first_cluster(target, name)
        data_sector = first_data + (cluster - 2) * per_cluster
        fat_byte = (START_LBA + reserved) * SECTOR + cluster * 4
        assert found.sector("data", name) == data_sector
        assert found.sector("fat", name) == fat_byte // SECTOR
        assert struct.unpack_from("<I", raw, fat_byte)[0] & 0x0FFFFFFF != FREE
        entry = found.sector("entry", name)
        assert entry is not None
        assert short_name(name) in at(entry)
        assert at(data_sector)[: len(data)] == data[:SECTOR]
    assert found.sector("data", "absent.bin") is None
