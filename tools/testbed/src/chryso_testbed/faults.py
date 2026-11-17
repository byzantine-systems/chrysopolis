"""Fault programs and the model that judges them.

A run is a blkdebug plan plus a `Program` of workload and fault steps.
`Model` is a product of components (Root, storage, network, orchestrator)
built on the charts in `lifecycles.py`: `expect` gives the outcomes a step
may have, `after` folds in the one observed. `trim` keeps serial and timer
under budget so the console and clock survive.

The orchestrator's steps (`Want`, `Rewant`, `Look`) obey these laws, each
stated over `expect` and `key()` and tested in `tests/test_faults.py`:

1. A `Want` is a settled `Restart` routed through the orchestrator, total
   under one renaming: `debug-restart` becomes `ctl-restart`, and `GONE`
   (Root ignored the request) becomes `UNASKED` (the orchestrator never
   sent it). Root's state after is the same either way.
2. A want that already holds, or can never hold, asks nothing: `Rewant`,
   and a `Want` of a gone driver, allow only `UNASKED` and leave Root as it
   was.
3. `Look` is an observation: with no driver recovering, it changes nothing.
4. `Look` is level-triggered: a function of Root's chart for the driver
   only, never of history (`condition`, the table `chryso_condition` holds).
5. With no status page, nothing is known and nothing is commanded.
6. Derived from 1 to 3: a `Want` commutes with what a `Restart` commutes
   with, and `Look` and `Rewant` commute with every step.
7. A reset forgets every want, as it forgets Root's budgets.
8. A flood is invisible to the restart algebra: `Want(flood=True)` allows
   what `Want` allows and leaves an equal model. Root's token bucket is not
   modelled because a rate-limited command has no effect, and the flood has
   stopped before the want is judged.
9. A fault inside a want is the only restart: `Absorb` is a settled `Fault`
   whose want then holds, so Root spends one restart and is asked nothing.

Non-laws: a fault restart after a want's stamp also satisfies it (the probe
reads Root before it stamps and each `Want` waits, so a program never lands
there). `Look` reports the page it read, not proof the driver
re-initialised.
"""

from __future__ import annotations

import functools
from collections import Counter
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field, replace
from typing import Annotated, Any, Final, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter

from chryso_testbed.allowed import Allowed
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
from chryso_testbed.outcomes import Condition, GaveUp, Outcome, Restarted, Seed, Status

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


class Overwrite(_Step):
    """Write 8 KiB of one byte over a file in place, without truncating it,
    then sync and close it. Whole sectors, so nothing is read first."""

    kind: Literal["overwrite"]
    file: int = Field(ge=0, lt=FILES)
    seed: int = Field(ge=0, le=255)


class Remove(_Step):
    """Delete a file."""

    kind: Literal["remove"]
    file: int = Field(ge=0, lt=FILES)


class Rename(_Step):
    """Rename file `src` to `dst`. FatFs refuses a `dst` that exists, and
    renaming a file onto its own name succeeds and changes nothing."""

    kind: Literal["rename"]
    src: int = Field(ge=0, lt=FILES)
    dst: int = Field(ge=0, lt=FILES)


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


class Want(_Step):
    """Ask the orchestrator for one restart of the driver after the page it
    reads now, and wait until the want holds or Root gives the driver up.
    With `flood`, another process floods Root with SYNC commands until Root
    rate-limits it, and the flood stops before the want is settled."""

    kind: Literal["want"]
    driver: Driver
    flood: bool = False


class Rewant(_Step):
    """Apply again the newest want made for the driver since boot. With none
    it applies nothing, so a shrunk program is always legal."""

    kind: Literal["rewant"]
    driver: Driver


class Absorb(_Step):
    """A fault that lands after a want's stamp and before its command: the
    probe holds the command device, makes the want, faults the driver, waits
    for Root's decision on the page, then releases. The want then holds by the
    fault's restart, so the orchestrator never asks."""

    kind: Literal["absorb"]
    driver: Literal["blk", "eth"]


class Look(_Step):
    """Read Root, then the orchestrator's condition for the driver."""

    kind: Literal["look"]
    driver: Driver


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
    Fault
    | Restart
    | Unplug
    | Pause
    | Put
    | Overwrite
    | Get
    | Remove
    | Rename
    | Ping
    | Arm
    | Disarm
    | Corrupt
    | Want
    | Rewant
    | Absorb
    | Look
    | Reset
    | CrashPut,
    Field(discriminator="kind"),
]
# Generation draws at most 16 steps. Lineage cuts may insert a driver's whole
# budget of faults around one.
type Program = Annotated[tuple[Step, ...], Field(max_length=32)]
STEP: Final = TypeAdapter[Step](Step)
PROGRAM: Final = TypeAdapter[Program](Program)


def trim(program: Program, budget: int) -> Program:
    """Drop serial and timer requests past their budget, so neither gives up.
    A want spends a restart as a request does, so it counts too."""

    def kept(steps: Iterable[Step]) -> Iterator[Step]:
        spent = Counter[Driver]()
        for step in steps:
            match step:
                case Fault(driver=d) | Restart(driver=d) | Want(driver=d) if d in KEPT:
                    spent[d] += 1
                    if spent[d] > budget:
                        continue
            yield step

    return tuple(kept(program))


def expect(*outcomes: Outcome) -> Allowed[Outcome]:
    """The outcomes a step may have, typed as outcomes whatever their kinds."""
    return Allowed.of(*outcomes)


ANYTHING: Final[Allowed[Outcome]] = Allowed.everything()
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

    def expect(self, step: Fault | Restart) -> Allowed[Outcome]:
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


type RowState = Literal["unset", "live", "stopped", "gone", "quarantined"]


def condition(state: RowState, lifetime: int, budget: int) -> Condition:
    """The orchestrator's condition for one Root row, as `chryso_condition`
    classifies it on the guest (`settle_ms` 0, `floor` 0, every want held).
    That scope is why it never answers `restarting` or `recovering`: each
    `Want` settles before the next step, and a zero settle window has no
    recovery to report."""
    match state:
        case "gone":
            return Condition("gone", "gave_up")
        case "live" if budget - lifetime <= 0:
            return Condition("degraded", "low_budget")
        case "live":
            return Condition("ready", "settled")
        case "stopped" | "quarantined" | "unset":
            return Condition("degraded", state)


def _as_ctl(outcome: Outcome) -> Outcome:
    """Root's answer to a debug restart, as the orchestrator's want ends: the
    same decision under Root's word `ctl-restart`, and no command at all where
    Root would ignore one."""
    match outcome:
        case Restarted(count=n):
            return Restarted("ctl-restart", n)
        case Status.GONE:
            return Status.UNASKED
        case _:
            return outcome


def _as_debug(outcome: Outcome) -> Outcome:
    """The inverse of `_as_ctl`."""
    match outcome:
        case Restarted(count=n):
            return Restarted("debug-restart", n)
        case Status.UNASKED:
            return Status.GONE
        case _:
            return outcome


@dataclass(frozen=True, slots=True)
class OrchModel:
    """What the orchestrator holds that Root's page cannot show: the drivers
    wanted since boot. `observable` is fixed by the image: false where Root
    publishes no status page."""

    wanted: frozenset[Driver] = frozenset()
    observable: bool = True

    def rebooted(self) -> OrchModel:
        return replace(self, wanted=frozenset())

    def key(self) -> tuple[object, ...]:
        return (tuple(sorted(self.wanted)), self.observable)


type StorageStep = Put | Overwrite | Get | Remove | Rename | Arm | Disarm | Corrupt


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

    def expect(self, step: StorageStep, *, up: bool) -> Allowed[Outcome]:
        """`up` is whether Root still runs the block driver."""
        base = self._unfaulted(step, up=up)
        if self.suspect and isinstance(step, Put | Overwrite | Get | Remove | Rename):
            # FatFs retries a dirty window before every later load, so even a
            # lookup that would find nothing may fail first.
            base |= expect(EIO)
        if isinstance(step, Put | Overwrite | Get | Remove | Rename) and self.damaged and up:
            return ANYTHING
        if (armed := self.armed) is None or not isinstance(
            step, Put | Overwrite | Get | Remove | Rename
        ):
            return base
        if isinstance(step, Remove | Rename):
            # Neither reads data, but both read FAT and directory sectors.
            return base if armed.region == "data" else base | expect(EIO)
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
            case Put() | Overwrite(), "read" if armed.region == "data":
                return base
            # FAT and directory sectors are shared and cached, so a fault
            # there may or may not surface.
            case _:
                return base | expect(EIO)

    def _present(self, f: int) -> Allowed[Outcome]:
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

    def _presence(self, f: int) -> frozenset[bool]:
        """Whether file `f` may be on the volume."""
        chart = self.file(f)
        match chart.at:
            case "absent":
                return frozenset({False})
            case "unknown":
                return frozenset({False, True})
            case "torn":
                return frozenset({True, Status.ENOENT in chart.model.before})
            case _:
                return frozenset({True})

    def _writes(self) -> Allowed[Outcome]:
        """What a write the volume accepts reports under the boot plan. A
        permanent rule fails every write, one-shot rules only some."""
        if not self.disk.write_fails:
            return expect(OK)
        return expect(EIO) if self.disk.reads == {"error"} else expect(OK, EIO)

    def _unfaulted(self, step: StorageStep, *, up: bool) -> Allowed[Outcome]:
        match step:
            case Put() | Overwrite() | Get() | Remove() | Rename() if not up:
                return expect(EIO, ERROR)
            case Put():
                return self._writes()
            case Overwrite(file=f) | Remove(file=f):
                present = self._presence(f)
                allowed = self._writes() if True in present else expect()
                if False in present:
                    allowed |= expect(Status.ENOENT)
                return allowed
            case Rename(src=s, dst=d) if s == d:
                present = self._presence(s)
                allowed = expect(OK) if True in present else expect()
                if False in present:
                    allowed |= expect(Status.ENOENT)
                return allowed
            case Rename(src=s, dst=d):
                allowed = expect()
                for src in self._presence(s):
                    for dst in self._presence(d):
                        if not src:
                            allowed |= expect(Status.ENOENT)
                        elif dst:
                            allowed |= expect(Status.EEXIST)
                        else:
                            allowed |= self._writes()
                return allowed
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
                return held
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
                return model._file(f, "write_failed")._moved(f)._dirtied()
            # In place: the data keeps its clusters, so a fault keeps its target.
            case Overwrite(file=f, seed=s), Status.OK:
                return model._file(f, "written", seed=s)
            case Overwrite() | Remove(), Status.ENOENT:
                return model
            case Overwrite(file=f), _:
                return model._file(f, "write_failed")._dirtied()
            # The freed clusters no longer belong to the file.
            case Remove(file=f), Status.OK:
                return model._file(f, "deleted")._moved(f)
            case Remove(file=f), _:
                return model._file(f, "write_failed")._dirtied()
            case Rename(src=s, dst=d), Status.OK if s != d:
                return model._renamed(s, d)
            case Rename(src=s, dst=d), Status.OK:
                return model
            case Rename(), Status.ENOENT | Status.EEXIST:
                return model
            case Rename(src=s, dst=d), _:
                return model._file(s, "write_failed")._file(d, "write_failed")._dirtied()
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

    def _dirtied(self) -> StorageModel:
        """After a failed write FatFs keeps its dirty sector when the disk or
        an armed fault fails writes."""
        armed = self.armed
        if "error" in self.disk.reads or (armed is not None and armed.op == "write"):
            return replace(self, window=self.window.evolved("left_dirty"))
        return self

    def _renamed(self, src: int, dst: int) -> StorageModel:
        """`dst` takes `src`'s file and its clusters, and `src` is gone. A fault
        on the clusters follows the file, one on its old entry is adrift."""
        files = {**self.files, dst: self.file(src), src: _fresh_file()}
        model = replace(self, files=files)
        if (armed := self.armed) is None or armed.file != src:
            return model
        if armed.region == "entry":
            return replace(model, fault=model.fault.evolved("moved"))
        return replace(model, fault=model.fault.evolved("followed", file=dst))

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

    def expect(self, step: NetStep, *, up: bool) -> Allowed[Outcome]:
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


type Component = Literal["root", "storage", "net", "orch", "system"]


def owner(step: Step) -> Component:
    """The component whose state a step reads and changes."""
    match step:
        case Fault() | Restart() | Want() | Absorb():
            return "root"
        case Rewant() | Look():
            return "orch"
        case Put() | Overwrite() | Get() | Remove() | Rename() | Arm() | Disarm() | Corrupt():
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
    orch: OrchModel = field(default_factory=OrchModel)

    @classmethod
    def start(cls, budget: int, disk: Expected = CLEAN_DISK, *, observable: bool = True) -> Model:
        return cls(RootModel(budget), StorageModel(disk), orch=OrchModel(observable=observable))

    def rebooted(self) -> Model:
        """Root, every driver and the BEAM start over on the same disk."""
        return replace(
            self,
            root=RootModel(self.root.budget),
            storage=self.storage.rebooted(),
            orch=self.orch.rebooted(),
        )

    def key(self) -> tuple[object, ...]:
        """Equal keys mean equal lifecycles and data: the model's identity.
        The laws compare models by key rather than by observation. Every field
        `expect` reads is in the key, so equal keys give equal outcomes for
        every step: a stronger equality, sound for the laws. State that
        nothing observes must stay out of the key, or the laws would reject
        models no step can tell apart."""
        return (self.root.key(), self.storage.key(), self.net.key(), self.orch.key())

    def look(self, driver: Driver) -> Condition:
        """Law 4: the condition is a function of Root's chart only."""
        if not self.orch.observable:
            return Condition("unknown", "no_observation")
        chart = self.root.chart(driver)
        state: RowState = "gone" if chart.at == "gone" else "live"
        return condition(state, chart.model.spent, chart.model.budget)

    def expect(self, step: Step) -> Allowed[Outcome]:
        match step:
            case Fault() | Restart():
                return self.root.expect(step)
            case Put() | Overwrite() | Get() | Remove() | Rename():
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
            case Want(driver=d):
                if not self.orch.observable:
                    return expect(ERROR)
                restart = Restart(kind="restart", driver=d)
                return Allowed.of(*map(_as_ctl, self.root.expect(restart).values))
            case Rewant():
                return expect(Status.UNASKED)
            case Absorb(driver=d):
                return self.root.expect(Fault(kind="fault", driver=d))
            case Look(driver=d):
                return expect(self.look(d))
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
            case Put() | Overwrite() | Get() | Remove() | Rename() | Arm() | Disarm() | Corrupt():
                model = replace(self, storage=self.storage.after(step, outcome))
            case Ping() | Unplug() | Pause():
                model = replace(self, net=self.net.after(step, outcome))
            case Want(driver=d) if self.orch.observable:
                restart = Restart(kind="restart", driver=d)
                model = replace(
                    self,
                    root=self.root.after(restart, _as_debug(outcome)),
                    orch=replace(self.orch, wanted=self.orch.wanted | {d}),
                )
            case Absorb(driver=d):
                fault = Fault(kind="fault", driver=d)
                model = replace(
                    self,
                    root=self.root.after(fault, outcome),
                    orch=replace(self.orch, wanted=self.orch.wanted | {d}),
                )
            case Want() | Rewant() | Look():
                model = self
        return replace(model, root=model.root.settled(recovering)) if recovering else model
