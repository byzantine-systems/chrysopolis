"""Fault programs and the model that judges them.

A run is a blkdebug plan plus a `Program` of workload and fault steps.
`Model` is a product of components (Root, storage, network) built on the
charts in `lifecycles.py`: `expect` gives the outcomes a step may have,
`after` folds in the one observed. `trim` keeps serial and timer under
budget so the console and clock survive.
"""

from __future__ import annotations

import functools
from collections import Counter
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field, replace
from typing import Annotated, Any, Final, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter

from chryso_testbed.blkdebug import Expected
from chryso_testbed.fatlayout import Region
from chryso_testbed.lifecycles import (
    DriverLifecycle,
    DriverRecord,
    FaultLifecycle,
    FaultRecord,
    FileLifecycle,
    FileRecord,
    FlagRecord,
    VolumeLifecycle,
    WindowLifecycle,
)
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted, Seed, Status

type Driver = Literal["serial", "timer", "blk", "eth"]
DRIVERS: Final[tuple[Driver, ...]] = ("serial", "timer", "blk", "eth")
KEPT: Final[frozenset[Driver]] = frozenset({"serial", "timer"})
FILES: Final = 4


class _Step(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)


class Fault(_Step):
    """Root resumes the driver at address 0, a genuine fault. Unsettled, the next step
    lands while the driver is still recovering."""

    kind: Literal["fault"]
    driver: Driver
    settle: bool = True


class Restart(_Step):
    """Root restarts the healthy driver on request. Unsettled, the next step
    lands while the driver is still recovering."""

    kind: Literal["restart"]
    driver: Driver
    settle: bool = True


class Unplug(_Step):
    """The NIC's link is down for `ms`, then back up."""

    kind: Literal["unplug"]
    ms: int = Field(ge=0, le=1500)


class Pause(_Step):
    """The VM is stopped for `ms`, then resumed."""

    kind: Literal["pause"]
    ms: int = Field(ge=0, le=1000)


class Put(_Step):
    """Write a file as 8 KiB of one byte, then sync and close it."""

    kind: Literal["put"]
    file: int = Field(ge=0, lt=FILES)
    seed: int = Field(ge=0, le=255)


class Get(_Step):
    """Read a file back."""

    kind: Literal["get"]
    file: int = Field(ge=0, lt=FILES)


class Ping(_Step):
    """The guest dials the host and sends its tag."""

    kind: Literal["ping"]


class Arm(_Step):
    """Fail the guest's reads or writes of one region of a file with EIO."""

    kind: Literal["arm"]
    op: Literal["read", "write"]
    region: Region
    file: int = Field(ge=0, lt=FILES)
    once: bool


class Disarm(_Step):
    """Take the armed fault out."""

    kind: Literal["disarm"]


class Corrupt(_Step):
    """Overwrite the first sector of one region of a file, behind the guest."""

    kind: Literal["corrupt"]
    region: Region
    file: int = Field(ge=0, lt=FILES)


class Reset(_Step):
    """Reset the whole system, as a power cycle that keeps the disk."""

    kind: Literal["reset"]


class CrashPut(_Step):
    """Start writing a file, and reset after `after_ms` without waiting."""

    kind: Literal["crash-put"]
    file: int = Field(ge=0, lt=FILES)
    seed: int = Field(ge=0, le=255)
    after_ms: int = Field(ge=0, le=200)


type Step = Annotated[
    Fault | Restart | Unplug | Pause | Put | Get | Ping | Arm | Disarm | Corrupt | Reset | CrashPut,
    Field(discriminator="kind"),
]
# Generation draws at most 16 steps. Lineage cuts may insert a driver's whole
# budget of faults around one.
type Program = Annotated[tuple[Step, ...], Field(max_length=32)]
STEP: Final = TypeAdapter[Step](Step)
PROGRAM: Final = TypeAdapter[Program](Program)


def trim(program: Program, budget: int) -> Program:
    """Drop serial and timer requests past their budget, so neither gives up."""

    def kept(steps: Iterable[Step]) -> Iterator[Step]:
        spent = Counter[Driver]()
        for step in steps:
            match step:
                case Fault(driver=d) | Restart(driver=d) if d in KEPT:
                    spent[d] += 1
                    if spent[d] > budget:
                        continue
            yield step

    return tuple(kept(program))


@dataclass(frozen=True, slots=True)
class Expect:
    """The outcomes a step may have. `anything` admits every outcome."""

    outcomes: frozenset[Outcome] = frozenset()
    anything: bool = False

    def __contains__(self, outcome: object) -> bool:
        return self.anything or outcome in self.outcomes

    def __or__(self, other: Expect) -> Expect:
        return Expect(self.outcomes | other.outcomes, self.anything or other.anything)


def expect(*outcomes: Outcome) -> Expect:
    return Expect(frozenset(outcomes))


ANYTHING: Final = Expect(anything=True)
CLEAN_DISK: Final = Expected(write_fails=False, reads=frozenset({"ok"}))
OK, EIO, ERROR = Status.OK, Status.EIO, Status.ERROR


# Charts are only ever evolved into copies, never changed in place, so every
# model can share one fresh chart per kind. Building one costs far more than
# a lookup, and generation asks for absent files and idle drivers constantly.
@functools.cache
def _fresh_driver(budget: int) -> DriverLifecycle:
    return DriverLifecycle(DriverRecord(budget))


@functools.cache
def _fresh_file() -> FileLifecycle:
    return FileLifecycle(FileRecord())


@dataclass(frozen=True, slots=True, eq=False)
class RootModel:
    """Root's restart policy, one `DriverLifecycle` per driver."""

    budget: int
    drivers: Mapping[Driver, DriverLifecycle] = field(default_factory=dict)

    def chart(self, driver: Driver) -> DriverLifecycle:
        found = self.drivers.get(driver)
        return found if found is not None else _fresh_driver(self.budget)

    def up(self, driver: Driver) -> bool:
        return self.chart(driver).at != "gone"

    @property
    def gone(self) -> frozenset[Driver]:
        return frozenset(d for d, chart in self.drivers.items() if chart.at == "gone")

    def expect(self, step: Fault | Restart) -> Expect:
        chart = self.chart(step.driver)
        if chart.at == "gone":
            return expect(Status.GONE)
        if not chart.has_budget():
            return expect(GaveUp("budget-exhausted"))
        path: Literal["restart", "debug-restart"] = (
            "restart" if isinstance(step, Fault) else "debug-restart"
        )
        return expect(Restarted(path, chart.model.spent + 1))

    def after(self, step: Fault | Restart, outcome: Outcome) -> RootModel:
        match outcome:
            case Restarted():
                event = "restarted" if step.settle else "restarted_unsettled"
            case GaveUp():
                event = "gave_up"
            case _:
                event = "ignored"
        chart = self.chart(step.driver).evolved(event)
        return replace(self, drivers={**self.drivers, step.driver: chart})

    def recovering(self, driver: Driver) -> bool:
        return self.chart(driver).at == "recovering"

    def settled(self, drivers: Iterable[Driver]) -> RootModel:
        """Every driver in `drivers` that was recovering, back to running."""
        charts = {
            d: c.evolved("settled") if d in drivers and c.at == "recovering" else c
            for d, c in self.drivers.items()
        }
        return replace(self, drivers=charts)

    def key(self) -> tuple[object, ...]:
        return tuple(sorted((d, c.at, c.model.spent) for d, c in self.drivers.items()))


type StorageStep = Put | Get | Arm | Disarm | Corrupt


def _fault() -> FaultLifecycle:
    return FaultLifecycle(FaultRecord())


def _volume() -> VolumeLifecycle:
    return VolumeLifecycle(FlagRecord())


def _window() -> WindowLifecycle:
    return WindowLifecycle(FlagRecord())


@dataclass(frozen=True, slots=True, eq=False)
class StorageModel:
    """The block backend: FatFs over virtio-blk, with boot-time and runtime
    disk faults. `disk` is what the boot's blkdebug plan lets the guest
    report, and the lifecycles carry the rest: each file's, the armed
    fault's, the volume's metadata and FatFs's sector window."""

    disk: Expected = CLEAN_DISK
    files: Mapping[int, FileLifecycle] = field(default_factory=dict)
    fault: FaultLifecycle = field(default_factory=_fault)
    volume: VolumeLifecycle = field(default_factory=_volume)
    window: WindowLifecycle = field(default_factory=_window)

    def file(self, f: int) -> FileLifecycle:
        found = self.files.get(f)
        return found if found is not None else _fresh_file()

    @property
    def armed(self) -> FaultRecord | None:
        return self.fault.model if self.fault.at != "disarmed" else None

    @property
    def damaged(self) -> bool:
        return self.volume.at == "damaged"

    @property
    def suspect(self) -> bool:
        return self.window.at == "dirty"

    def expect(self, step: StorageStep, *, up: bool) -> Expect:
        """`up` is whether Root still runs the block driver."""
        base = self._unfaulted(step, up=up)
        if isinstance(step, Put | Get) and self.damaged and up:
            return ANYTHING
        if (armed := self.armed) is None or not isinstance(step, Put | Get):
            return base
        same_data = armed.region == "data" and armed.file == step.file
        match step, armed.op:
            # The strict cases: a read error on a file's data is reported,
            # and a put never reads data, nor a get write.
            case Get(file=f), "read" if same_data:
                if self.fault.at in {"maybe_spent", "adrift"}:
                    return base | expect(EIO)
                return expect(EIO) if self.file(f).at in {"held", "corrupt"} else ANYTHING
            case Get(), "write":
                return base
            case Get(), "read" if armed.region == "data":
                return base
            case Put(), "read" if armed.region == "data":
                return base
            # FAT and directory sectors are shared and cached, so a fault
            # there may or may not surface.
            case _:
                return base | expect(EIO)

    def _present(self, f: int) -> Expect:
        """What locating file `f` on disk may find."""
        if self.damaged:
            return ANYTHING
        match self.file(f).at:
            case "absent":
                return expect(Status.ABSENT)
            case "unknown" | "torn":
                return expect(Status.PRESENT, Status.ABSENT)
            case _:
                return expect(Status.PRESENT)

    def _unfaulted(self, step: StorageStep, *, up: bool) -> Expect:
        match step:
            case Put() | Get() if not up:
                return expect(EIO, ERROR)
            case Put() if not self.disk.write_fails:
                return expect(OK)
            case Put():
                # A permanent rule fails every write, one-shot rules only some.
                return expect(EIO) if self.disk.reads == {"error"} else expect(OK, EIO)
            case Get(file=f):
                chart = self.file(f)
                match chart.at:
                    case "absent":
                        held = expect(Status.ENOENT)
                    case "unknown":
                        held = ANYTHING
                    case "torn":
                        held = expect(
                            *chart.model.before, Status.DIFFERS, Seed(chart.model.seed or 0)
                        )
                    case "corrupt":
                        held = expect(Status.DIFFERS)
                    case _:
                        held = expect(Seed(chart.model.seed or 0))
                return (held | expect(EIO)) if self.suspect else held
            case Arm(file=f) | Corrupt(file=f):
                return self._present(f)
            case Disarm():
                return expect(OK)

    def after(self, step: StorageStep, outcome: Outcome) -> StorageModel:
        model = self._spend(step, outcome)
        match step, outcome:
            case Arm(op=op, region=r, file=f, once=once), Status.PRESENT:
                fault = model.fault.evolved("aimed", op=op, region=r, file=f, once=once)
                return replace(model, fault=fault)
            case Arm() | Disarm(), _:
                return replace(model, fault=model.fault.evolved("removed"))
            case Corrupt(region="data", file=f), Status.PRESENT:
                return model._file(f, "corrupted")
            case Corrupt(), Status.PRESENT:
                return replace(model, volume=model.volume.evolved("metadata_corrupted"))
            case Put(file=f, seed=s), Status.OK:
                return model._file(f, "written", seed=s)._moved(f)
            case Put(file=f), _:
                armed = model.armed
                dirty = "error" in model.disk.reads or (armed is not None and armed.op == "write")
                failed = model._file(f, "write_failed")._moved(f)
                return (
                    replace(failed, window=failed.window.evolved("left_dirty")) if dirty else failed
                )
            case _:
                return model

    def crashed(self, f: int, seed: int) -> StorageModel:
        """A rewrite of `f` cut by a reset: it reads as before, empty or new."""
        chart = self.file(f)
        before: Outcome
        match chart.at:
            case "absent":
                before = Status.ENOENT
            case "held":
                before = Seed(chart.model.seed or 0)
            case "corrupt":
                before = Status.DIFFERS
            case _:  # unknown or torn: the chart drops to unknown and ignores it
                before = Status.ERROR
        return self._file(f, "cut", seed=seed, before=frozenset({before}))

    def rebooted(self) -> StorageModel:
        """After a reset: the disk keeps its files and any damage, while the
        armed fault (disarmed first) and FatFs's window are gone."""
        return replace(self, fault=_fault(), window=_window())

    def _moved(self, f: int) -> StorageModel:
        """A rewrite of `f` truncates it, so FatFs may give it new clusters:
        a fault on its data or FAT entry may no longer touch it."""
        armed = self.armed
        if armed is None or armed.file != f or armed.region not in {"data", "fat"}:
            return self
        return replace(self, fault=self.fault.evolved("moved"))

    def _file(self, f: int, event: str, **data: Any) -> StorageModel:
        return replace(self, files={**self.files, f: self.file(f).evolved(event, **data)})

    def _spend(self, step: StorageStep, outcome: Outcome) -> StorageModel:
        """After an EIO a one-shot fault is spent for certain only when it was
        the sole possible cause: a get of its file's data with no dirty
        sector pending. Otherwise it may or may not have fired."""
        armed = self.armed
        if outcome != EIO or armed is None or not armed.once:
            return self
        certain = (
            self.fault.at != "adrift"
            and isinstance(step, Get)
            and armed.op == "read"
            and armed.region == "data"
            and armed.file == step.file
            and not self.suspect
        )
        return replace(self, fault=self.fault.evolved("spent" if certain else "maybe_fired"))

    def key(self) -> tuple[object, ...]:
        armed = self.armed
        # File numbers are unique, so sorting never compares past the first field.
        files = sorted((f, c.at, c.model.seed, c.model.before) for f, c in self.files.items())
        return (
            self.disk,
            tuple(files),
            self.fault.at,
            None if armed is None else (armed.op, armed.region, armed.file, armed.once),
            self.volume.at,
            self.window.at,
        )


type NetStep = Ping | Unplug | Pause


@dataclass(frozen=True, slots=True, eq=False)
class NetModel:
    """The network as the guest sees it. A cut link or a pause is restored
    before the step ends, so nothing outlives a step today."""

    def expect(self, step: NetStep, *, up: bool) -> Expect:
        """`up` is whether Root still runs the NIC driver."""
        match step:
            case Ping():
                return expect(OK) if up else expect(Status.UNREACHABLE)
            case Unplug() | Pause():
                return expect(OK)

    def after(self, step: NetStep, outcome: Outcome) -> NetModel:
        return self

    def key(self) -> tuple[object, ...]:
        return ()


type Component = Literal["root", "storage", "net", "system"]


def owner(step: Step) -> Component:
    """The component whose state a step reads and changes."""
    match step:
        case Fault() | Restart():
            return "root"
        case Put() | Get() | Arm() | Disarm() | Corrupt():
            return "storage"
        case Ping() | Unplug() | Pause():
            return "net"
        case Reset() | CrashPut():
            return "system"


@dataclass(frozen=True, slots=True, eq=False)
class Model:
    """The product of the components. Each step goes to its owner, and
    Root's availability is the only state the others read, as `chryso_root`
    composes the `chryso_core` units."""

    root: RootModel
    storage: StorageModel = field(default_factory=StorageModel)
    net: NetModel = field(default_factory=NetModel)

    @classmethod
    def start(cls, budget: int, disk: Expected = CLEAN_DISK) -> Model:
        return cls(RootModel(budget), StorageModel(disk))

    def rebooted(self) -> Model:
        """Root, every driver and the BEAM start over on the same disk."""
        return replace(self, root=RootModel(self.root.budget), storage=self.storage.rebooted())

    def key(self) -> tuple[object, ...]:
        """Equal keys mean equal lifecycles and data: the model's identity."""
        return (self.root.key(), self.storage.key(), self.net.key())

    def expect(self, step: Step) -> Expect:
        match step:
            case Fault() | Restart():
                return self.root.expect(step)
            case Put() | Get():
                allowed = self.storage.expect(step, up=self.root.up("blk"))
                # A driver still reconciling may refuse the request.
                return (allowed | expect(EIO, ERROR)) if self.root.recovering("blk") else allowed
            case Arm() | Disarm() | Corrupt():
                return self.storage.expect(step, up=self.root.up("blk"))
            case Ping():
                allowed = self.net.expect(step, up=self.root.up("eth"))
                return (
                    (allowed | expect(Status.UNREACHABLE))
                    if self.root.recovering("eth")
                    else allowed
                )
            case Unplug() | Pause():
                return self.net.expect(step, up=self.root.up("eth"))
            case Reset() | CrashPut():
                # A damaged volume may not mount, and then nothing boots.
                return ANYTHING if self.storage.damaged else expect(OK)

    def after(self, step: Step, outcome: Outcome) -> Model:
        """The step's own effect, then every driver that was recovering before
        it settles, as the interpreter waits for them after each step. An
        unsettled request to a recovering driver keeps it recovering."""
        match step:
            case Reset():
                return self.rebooted()
            case CrashPut(file=f, seed=s):
                return replace(self, storage=self.storage.crashed(f, s)).rebooted()
        recovering = {d for d in DRIVERS if self.root.recovering(d)}
        if isinstance(step, Fault | Restart) and not step.settle:
            recovering.discard(step.driver)
        match step:
            case Fault() | Restart():
                model = replace(self, root=self.root.after(step, outcome))
            case Put() | Get() | Arm() | Disarm() | Corrupt():
                model = replace(self, storage=self.storage.after(step, outcome))
            case Ping() | Unplug() | Pause():
                model = replace(self, net=self.net.after(step, outcome))
        return replace(model, root=model.root.settled(recovering)) if recovering else model
