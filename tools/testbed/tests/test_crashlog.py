"""Write logs and crash states, against an encoder written from the format.
Planted defects caught:

| Defect | Caught by |
|---|---|
| parse stops at nr_entries | test_parse_reads_every_entry_whatever_the_superblock_counts |
| replay applies the newest write first | test_replay_applies_exactly_the_kept_writes |
| a crash state drops earlier epochs | test_crash_states_grow_epoch_by_epoch |
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Final

import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from chryso_testbed.crashlog import DISCARD, FLUSH, MAGIC, Log, parse, replay, stable, state

SECTOR: Final = 512
DISK_SECTORS: Final = 64

type Op = tuple[str, int, bytes]  # kind, sector, data (whole sectors)

writes = st.builds(
    lambda sector, n, byte: ("write", sector, bytes([byte]) * (n * SECTOR)),
    st.integers(0, 6),  # a narrow range, so writes overlap and order shows
    st.integers(1, 4),
    st.integers(0, 255),
)
ops = st.lists(
    st.one_of(writes, st.just(("flush", 0, b"")), st.just(("discard", 3, b""))), max_size=24
)


def encode(log_ops: list[Op], *, counted: int | None = None) -> bytes:
    """A log as blklogwrites lays it out. `counted` is the superblock's stale
    nr_entries, which a reader must not trust."""
    body = b""
    for kind, sector, data in log_ops:
        flags = {"write": 0, "flush": FLUSH, "discard": DISCARD}[kind]
        nr = len(data) // SECTOR if kind == "write" else (1 if kind == "discard" else 0)
        body += struct.pack("<QQQQ", sector, nr, flags, 0).ljust(SECTOR, b"\0") + data
    entries = len(log_ops) if counted is None else counted
    return struct.pack("<QQQI", MAGIC, 1, entries, SECTOR).ljust(SECTOR, b"\0") + body


@given(log_ops=ops, counted=st.integers(0, 3))
def test_parse_reads_every_entry_whatever_the_superblock_counts(
    log_ops: list[Op], counted: int
) -> None:
    log = parse(encode(log_ops, counted=counted))
    assert [(e.sector, e.data) for e in log.entries if e.write] == [
        (s, d) for k, s, d in log_ops if k == "write"
    ]
    assert len(log.flushes) == sum(k == "flush" for k, _, _ in log_ops)


@given(log_ops=ops.filter(bool), cut=st.integers(1, SECTOR * 4))
def test_a_torn_last_entry_is_dropped(log_ops: list[Op], cut: int) -> None:
    whole = encode(log_ops)
    log = parse(whole[: max(SECTOR, len(whole) - cut)])
    full = parse(whole)
    assert log.entries == full.entries[: len(log.entries)]


def test_a_foreign_file_is_not_a_log() -> None:
    with pytest.raises(ValueError, match="not a write log"):
        parse(bytes(SECTOR * 2))


@given(log_ops=ops, data=st.data())
def test_crash_states_grow_epoch_by_epoch(log_ops: list[Op], data: st.DataObject) -> None:
    """Completing an epoch is the next epoch's empty state, and any subset of
    an epoch sits between the two."""
    log = parse(encode(log_ops))
    for epoch in range(len(log.epochs) - 1):
        subset = (
            data.draw(st.sets(st.sampled_from(log.epochs[epoch]))) if log.epochs[epoch] else set()
        )
        assert state(log, epoch) <= state(log, epoch, subset) <= state(log, epoch + 1)
        assert state(log, epoch, log.epochs[epoch]) == state(log, epoch + 1)


@given(log_ops=ops, data=st.data())
def test_a_crash_keeps_everything_before_its_stable_point(
    log_ops: list[Op], data: st.DataObject
) -> None:
    """Every write logged before the stable point is in the crash state, and
    no write after it is guaranteed."""
    log = parse(encode(log_ops))
    epoch = data.draw(st.integers(0, len(log.epochs) - 1))
    point = stable(log, epoch)
    before = {i for i, e in enumerate(log.entries) if e.write and e.end <= point}
    assert before <= state(log, epoch)
    assert all(log.entries[i].end > point for i in state(log, epoch, log.epochs[epoch]) - before)


def reference(log: Log, keep: frozenset[int], base: bytes) -> bytes:
    """The disk after `keep`, applied one sector at a time."""
    disk = bytearray(base)
    for i in sorted(keep):
        e = log.entries[i]
        disk[e.sector * SECTOR : e.sector * SECTOR + len(e.data)] = e.data
    return bytes(disk)


@settings(suppress_health_check=[HealthCheck.function_scoped_fixture])
@given(log_ops=ops, base=st.binary(min_size=SECTOR, max_size=SECTOR), data=st.data())
def test_replay_applies_exactly_the_kept_writes(
    tmp_path: Path, log_ops: list[Op], base: bytes, data: st.DataObject
) -> None:
    log = parse(encode(log_ops))
    epoch = data.draw(st.integers(0, len(log.epochs) - 1))
    keep = state(log, epoch, data.draw(st.sets(st.sampled_from(log.epochs[epoch] or (0,)))))
    disk = base * DISK_SECTORS
    (tmp_path / "base.img").write_bytes(disk)
    out = replay(log, tmp_path / "base.img", tmp_path / "crash.img", keep)
    assert out.read_bytes() == reference(log, keep, disk)
