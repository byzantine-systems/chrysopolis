"""Fault programs: what a fuzz run does to the guest, and what it may see.

A `Program` is a sequence of steps. Some are workload (write or read a
file, dial the host) and some are faults (fault or restart a driver through
Root, cut the link, pause the VM). Disk faults are not steps: a blkdebug
`Plan` is fixed at boot, so a run is a plan plus a program.

`Model` is the oracle, a Mealy machine over steps. `expect` says which
outcomes a step may have from the current state and `after` folds the
observed outcome in. It encodes Root's contract (one lifetime budget per
driver, shared by faults and debug restarts, then a give-up after which
every request is ignored) and the file system's (a write reported ok reads
back, a failed write leaves that file unknown). The interpreter in
`inject.py` turns a step into QMP calls and shell input and an observation
into one of the outcome strings below.

Serial and timer never give up in a program: without the console or the
clock the run could not report anything. `trim` enforces that.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field, replace
from typing import Annotated, Final, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter

from chryso_testbed.blkdebug import Expected

type Driver = Literal["serial", "timer", "blk", "eth"]
KEPT: Final[frozenset[Driver]] = frozenset({"serial", "timer"})
FILES: Final = 4


class _Step(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)


class Fault(_Step):
    """Root resumes the driver at address 0, a genuine fault."""

    kind: Literal["fault"]
    driver: Driver


class Restart(_Step):
    """Root restarts the healthy driver on request."""

    kind: Literal["restart"]
    driver: Driver


class Unplug(_Step):
    """The NIC's link is down for `ms`, then back up."""

    kind: Literal["unplug"]
    ms: int = Field(ge=0, le=3000)


class Pause(_Step):
    """The VM is stopped for `ms`, then resumed."""

    kind: Literal["pause"]
    ms: int = Field(ge=0, le=2000)


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


type Step = Annotated[
    Fault | Restart | Unplug | Pause | Put | Get | Ping, Field(discriminator="kind")
]
type Program = Annotated[tuple[Step, ...], Field(max_length=12)]
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

    outcomes: frozenset[str] = frozenset()
    anything: bool = False

    def __contains__(self, outcome: object) -> bool:
        return self.anything or outcome in self.outcomes

    def __or__(self, other: Expect) -> Expect:
        return Expect(self.outcomes | other.outcomes, self.anything or other.anything)


def expect(*outcomes: str) -> Expect:
    return Expect(frozenset(outcomes))


ANYTHING: Final = Expect(anything=True)
CLEAN_DISK: Final = Expected(write_fails=False, reads=frozenset({"ok"}))


@dataclass(frozen=True, slots=True)
class Model:
    """What the guest has been through so far, as far as outcomes depend on it.

    `disk` is what the boot's blkdebug plan lets the guest report. `files`
    maps a file to its seed, or to None once a failed write left it unknown.
    `suspect` is set by a failed write under a plan whose reads may then
    fail, because FatFs retries the dirty sector on later reads.
    """

    budget: int
    disk: Expected = CLEAN_DISK
    spent: Mapping[Driver, int] = field(default_factory=dict)
    gone: frozenset[Driver] = frozenset()
    files: Mapping[int, int | None] = field(default_factory=dict)
    suspect: bool = False

    def expect(self, step: Step) -> Expect:
        match step:
            case Fault(driver=d) | Restart(driver=d):
                if d in self.gone:
                    return expect("gone")
                if self.spent.get(d, 0) == self.budget:
                    return expect("giveup:budget-exhausted")
                path = "restart" if isinstance(step, Fault) else "debug-restart"
                return expect(f"{path}:{self.spent.get(d, 0) + 1}")
            case Put() if "blk" in self.gone:
                return expect("eio", "error")
            case Put() if not self.disk.write_fails:
                return expect("ok")
            case Put():
                # A permanent rule fails every write, one-shot rules only some.
                return expect("eio") if self.disk.reads == {"error"} else expect("ok", "eio")
            case Get() if "blk" in self.gone:
                return expect("eio", "error")
            case Get(file=f):
                if f not in self.files:
                    held = expect("enoent")
                elif (seed := self.files[f]) is None:
                    held = ANYTHING
                else:
                    held = expect(f"seed:{seed}")
                return (held | expect("eio")) if self.suspect else held
            case Ping():
                return expect("err") if "eth" in self.gone else expect("ok")
            case Unplug() | Pause():
                return expect("ok")

    def after(self, step: Step, outcome: str) -> Model:
        match step, outcome.partition(":")[0]:
            case Fault(driver=d) | Restart(driver=d), "restart" | "debug-restart":
                return replace(self, spent={**self.spent, d: self.spent.get(d, 0) + 1})
            case Fault(driver=d) | Restart(driver=d), "giveup":
                return replace(self, gone=self.gone | {d})
            case Put(file=f, seed=s), "ok":
                return replace(self, files={**self.files, f: s})
            case Put(file=f), _:
                suspect = self.suspect or "error" in self.disk.reads
                return replace(self, files={**self.files, f: None}, suspect=suspect)
            case _:
                return self
