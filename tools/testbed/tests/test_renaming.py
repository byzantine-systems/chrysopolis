"""Laws of a rename under crashes, with the intent record a mount finishes.

The observation is `Volume.names`: every live name and its chain. Crash states
are enumerated in full for each drawn layout. Each planted design is rejected
by its predicted law:

| Planted design | Rejected by |
|---|---|
| no record (the fork before this change) | `atomic` |
| the record in the same epoch as the new entry | `atomic` |
| recovery clears the record in the removal's epoch | `recovery_survives_crashes` |
| an empty file's twin matched by cluster 0 | `never_lost` |
| recovery frees the old entry's chain | `atomic` |
| a new directory sector linked as it is zeroed (upstream) | `atomic`, `create_atomic` |

And in the model itself:

| Planted defect | Killed by |
|---|---|
| `Plan.crashes` keeps whole epochs only | the record-with-entry and clear-with-removal searches |
| `Record.decode` skips the CRC | test_no_single_corrupted_byte_decodes_as_a_record |
| `placement` grows into a new sector, never a leftover one | the links-before-zeroing searches |

Non-law: a crashed rename of an empty file may leave both names. It has no
chain to share, so no record is written for it.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass, replace
from typing import Final

import pytest
from hypothesis import find, given, settings
from hypothesis import strategies as st
from hypothesis.errors import NoSuchExample

from chryso_testbed.renaming import (
    FIXED,
    SIZE,
    Design,
    Entry,
    Plan,
    Record,
    Volume,
    create,
    placement,
    put,
    recover,
    rename,
    settled,
)

OTHERS: Final = ("c", "d", "e", "f")


@dataclass(frozen=True, slots=True)
class Case:
    """A volume and a rename of P/a to `dst`/b."""

    volume: Volume
    dst: str

    def plan(self, design: Design = FIXED) -> Plan:
        return rename(self.volume, ("P", "a"), (self.dst, "b"), design)

    def ends(self, design: Design = FIXED) -> tuple[Mapping[str, object], Mapping[str, object]]:
        return self.volume.names(), self.plan(design).done(self.volume).names()

    def kind(self) -> str:
        at = placement(self.volume, self.dst)
        sector, _ = at.slot
        (old, _), _ = self.volume.locate("P", "a")
        if at.zero:
            return "grows-into-leftovers" if any(self.volume.cells(sector)) else "grows"
        if self.dst != "P":
            return "other-dir"
        return "same-sector" if sector == old else "other-sector"


@st.composite
def cases(draw: st.DrawFn, *, empty: bool = False, leftovers: bool | None = None) -> Case:
    """Directories P (one or two sectors) and Q (one), one to three slots a
    sector. Other slots hold files, empty files, deleted entries or nothing.
    The source `a` sits at a drawn slot of P, and `b` is free everywhere.

    With `leftovers` (drawn when None), the free sector a directory would
    grow into holds old entries. Their names and clusters are drawn from the
    live ones too, the renamed file's included: a leftover on its chain is
    what a mount's twin scan must never get to see."""
    width = draw(st.integers(1, 3))
    p = draw(st.integers(1, 2))
    slots = [(s, i) for s in range(p + 1) for i in range(width)]
    src = draw(st.sampled_from(slots[: p * width]))
    clusters = iter(range(2, 100))
    source = Entry("a", 0 if empty else next(clusters))
    cells: dict[tuple[int, int], Entry | None] = {src: source}
    others = iter(OTHERS * 2)
    for slot in slots:
        if slot == src:
            continue
        name = next(others)
        match draw(st.sampled_from(["free", "file", "empty", "deleted"])):
            case "file":
                cells[slot] = Entry(name, next(clusters))
            case "empty":
                cells[slot] = Entry(name, 0)
            case "deleted":
                cells[slot] = Entry(name, next(clusters), live=False)
            case _:
                cells[slot] = None
    sectors = [tuple(cells.get((s, i)) for i in range(width)) for s in range(p + 1)]
    if draw(st.booleans()) if leftovers is None else leftovers:
        old = st.builds(
            Entry,
            name=st.sampled_from(("a", "b", *OTHERS)),
            cluster=st.sampled_from([source.cluster, *range(50, 54)]),
            live=st.booleans(),
        )
        sectors.append(tuple(draw(st.lists(old, min_size=width, max_size=width))))
    volume = Volume(sectors=tuple(sectors), dirs=(("P", tuple(range(p))), ("Q", (p,))))
    return Case(volume, draw(st.sampled_from(["P", "Q"])))


# --- the laws, as predicates the planted designs are searched against ----------------


def atomic(c: Case, design: Design = FIXED) -> bool:
    """After any crash and a mount, the names are those before or after."""
    before, after = c.ends(design)
    return all(
        settled(s, design).names() in (before, after) for s in c.plan(design).crashes(c.volume)
    )


def never_lost(c: Case, design: Design = FIXED) -> bool:
    """For an empty file: before, after, or both names, never fewer."""
    before, after = c.ends(design)
    allowed = (before, after, {**before, **after})
    return all(settled(s, design).names() in allowed for s in c.plan(design).crashes(c.volume))


def recovery_survives_crashes(c: Case, design: Design = FIXED) -> bool:
    """A crash during recovery, then another mount, ends where an
    uninterrupted recovery does."""
    for s in c.plan(design).crashes(c.volume):
        goal = settled(s, design).names()
        if any(settled(t, design).names() != goal for t in recover(s, design).crashes(s)):
            return False
    return True


def create_atomic(c: Case, design: Design = FIXED) -> bool:
    """A crashed create of `dst`/b leaves the names before or after, never a
    leftover one from the sector the directory grew into."""
    plan = create(c.volume, c.dst, "b", 60, design)
    ends = (c.volume.names(), plan.done(c.volume).names())
    return all(settled(s, design).names() in ends for s in plan.crashes(c.volume))


# --- laws on the fork's design --------------------------------------------------------


@given(c=cases())
def test_a_crashed_rename_recovers_to_before_or_after(c: Case) -> None:
    assert atomic(c)


@given(c=cases())
def test_a_finished_rename_is_after_with_nothing_left_to_recover(c: Case) -> None:
    _, after = c.ends()
    plan = c.plan()
    assert recover(plan.done(c.volume)) == Plan()
    unflushed = Plan(plan.epochs[:-1]).done(c.volume)  # the clear never reached the disk
    assert settled(unflushed).names() == after


@given(c=cases())
def test_a_crashed_recovery_recovers_the_same(c: Case) -> None:
    assert recovery_survives_crashes(c)


@given(c=cases())
def test_after_a_mount_no_two_names_share_a_chain(c: Case) -> None:
    assert all(not settled(s).shared() for s in c.plan().crashes(c.volume))


@given(c=cases())
def test_recovery_touches_only_the_renamed_file(c: Case) -> None:
    others = {k: v for k, v in c.volume.names().items() if k not in {"P/a", f"{c.dst}/b"}}
    for s in c.plan().crashes(c.volume):
        names = settled(s).names()
        assert {k: names.get(k) for k in others} == others


@given(c=cases())
def test_without_a_record_recovery_is_the_identity(c: Case) -> None:
    assert recover(c.volume) == Plan()


@given(c=cases(empty=True))
def test_an_empty_file_is_never_lost(c: Case) -> None:
    assert never_lost(c)


@given(c=cases())
def test_a_crashed_create_shows_no_leftover_name(c: Case) -> None:
    assert create_atomic(c)


@st.composite
def stale(draw: st.DrawFn) -> Volume:
    """A rename whose clear never reached the disk, then creates and removes,
    one maybe reusing the old slot under the old name and chain."""
    c = draw(cases())
    v = Plan(c.plan().epochs[:-1]).done(c.volume)
    for _ in range(draw(st.integers(0, 4))):
        live = [(d, e.name) for d, _ in v.dirs for _, e in v.entries(d)]
        if draw(st.booleans()) and live:
            slot, entry = v.locate(*draw(st.sampled_from(live)))
            v = put(v, slot, replace(entry, live=False)).onto(v)
            continue
        d = draw(st.sampled_from(["P", "Q"]))
        name = draw(st.sampled_from(("a", "b", *OTHERS)))
        if any(e.name == name for _, e in v.entries(d)):
            continue
        used = {e.cluster for dd, _ in v.dirs for _, e in v.entries(dd)}
        cluster = draw(st.sampled_from([0, *(x for x in range(2, 12) if x not in used)]))
        v = create(v, d, name, cluster).done(v)
    return v


@given(v=stale())
def test_a_stale_record_changes_nothing(v: Volume) -> None:
    assert settled(v).names() == v.names()


# --- the planted designs, each found out ---------------------------------------------


@pytest.mark.parametrize(
    ("design", "law", "empty"),
    [
        (Design(record=False), atomic, False),
        (Design(record_apart=False), atomic, False),
        (Design(clear_apart=False), recovery_survives_crashes, False),
        (Design(record_empty=True), never_lost, True),
        (Design(free_chain=True), atomic, False),
        (Design(grow_apart=False), atomic, False),
        (Design(grow_apart=False), create_atomic, False),
    ],
    ids=[
        "no-record", "record-with-entry", "clear-with-removal", "empty-twin", "frees-chain",
        "links-before-zeroing", "create-links-before-zeroing",
    ],
)  # fmt: skip
def test_every_planted_design_is_rejected(
    design: Design, law: Callable[[Case, Design], bool], empty: bool
) -> None:
    found = find(
        cases(empty=empty),
        lambda c: not law(c, design),
        settings=settings(max_examples=2000, database=None),
    )
    assert law(found, FIXED), "the fork's design passes where the planted one fails"


def test_without_leftovers_linking_before_zeroing_goes_unseen() -> None:
    """Why the smoke dirties the disk first: on clean free space the upstream
    order breaks no law, so no crash state can show it."""
    with pytest.raises(NoSuchExample):
        find(
            cases(leftovers=False),
            lambda c: not atomic(c, Design(grow_apart=False)),
            settings=settings(max_examples=500, database=None),
        )


@pytest.mark.parametrize(
    "kind", ["same-sector", "other-sector", "other-dir", "grows", "grows-into-leftovers"]
)
def test_the_generator_reaches_every_kind_of_rename(kind: str) -> None:
    find(cases(), lambda c: c.kind() == kind, settings=settings(database=None))


def test_a_cut_rename_is_cross_linked_until_a_mount_finishes_it() -> None:
    """The anchor: identity laws alone pass for a model that never renames."""
    v = Volume(sectors=((Entry("a", 7),), (None,), (None,)), dirs=(("P", (0, 1)), ("Q", (2,))))
    intent, register, _, _ = rename(v, ("P", "a"), ("P", "b")).epochs
    cut = Plan((intent, register)).done(v)
    assert cut.names() == {"P/a": 7, "P/b": 7}
    assert cut.shared() == {7}
    assert settled(cut).names() == {"P/b": 7}


# --- the record's codec ---------------------------------------------------------------

PINNED: Final = bytes.fromhex(
    "43485259524e4d310100000002000000400000006000000046555a5a3120202042494e00"
    "0900000002000000d062de97"
)
words = st.integers(0, 2**32 - 1)
records = st.builds(
    Record, old_dir=words, blk_ofs=words, dptr=words,
    sfn=st.binary(min_size=11, max_size=11), cluster=words, new_dir=words,
)  # fmt: skip
sector_sizes = st.sampled_from([512, 4096])


@given(r=records, size=sector_sizes)
def test_decode_inverts_encode(r: Record, size: int) -> None:
    assert Record.decode(r.encode(size)) == r


@given(r=records, size=sector_sizes, data=st.data())
def test_no_single_corrupted_byte_decodes_as_a_record(
    r: Record, size: int, data: st.DataObject
) -> None:
    sector = bytearray(r.encode(size))
    # Half the flips land in the record itself, or the zero tail hides a weak check.
    at = data.draw(st.one_of(st.integers(0, SIZE - 1), st.integers(SIZE, size - 1)))
    sector[at] ^= data.draw(st.integers(1, 255))
    assert Record.decode(bytes(sector)) in {"foreign", "corrupt"}


@given(sector=st.binary(min_size=512, max_size=512))
def test_decode_is_total(sector: bytes) -> None:
    Record.decode(sector)


def test_the_pinned_record_is_shared_with_the_c_suite() -> None:
    r = Record(old_dir=2, blk_ofs=0x40, dptr=0x60, sfn=b"FUZZ1   BIN", cluster=9, new_dir=2)
    assert r.encode(4096)[:SIZE] == PINNED
    assert Record.decode(PINNED.ljust(4096, b"\0")) == r
    assert Record.decode(bytes(4096)) == "absent"
