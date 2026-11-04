"""The lifecycles the fault model tracks, as python-statemachine charts.

Events are observed outcomes, and a chart refuses one it has no transition
for. `evolved` copies before sending, so models stay immutable.
"""

from __future__ import annotations

import copy
from dataclasses import dataclass
from typing import Any, Literal, Self

from statemachine import State, StateChart

from chryso_testbed.fatlayout import Region


class _Chart[R](StateChart[R]):
    allow_event_without_transition = False

    def evolved(self, event: str, **data: Any) -> Self:
        """A copy of this chart after `event`. The original is untouched.
        `data` reaches the event's callbacks, untyped as the library has it."""
        twin = copy.deepcopy(self)
        twin.send(event, **data)
        return twin

    @property
    def at(self) -> str:
        return str(self.current_state_value)


@dataclass(slots=True)
class DriverRecord:
    budget: int
    spent: int = 0
    state: str | None = None


class DriverLifecycle(_Chart[DriverRecord]):
    """Root's view of one driver: restarted while its lifetime budget lasts,
    faults and debug restarts alike, then stopped for good. Every later
    request is ignored."""

    running = State(initial=True)
    gone = State()

    restarted = running.to(running, cond="has_budget", on="spend")
    gave_up = running.to(gone, unless="has_budget")
    ignored = gone.to(gone, internal=True)

    def has_budget(self) -> bool:
        return self.model.spent < self.model.budget

    def spend(self) -> None:
        self.model.spent += 1


@dataclass(slots=True)
class FileRecord:
    seed: int | None = None
    state: str | None = None


class FileLifecycle(_Chart[FileRecord]):
    """One fuzz file as the volume holds it. `held` reads back its seed,
    `unknown` follows a failed write, `corrupt` a data sector overwritten
    behind the guest. A write reported ok always makes it `held`."""

    absent = State(initial=True)
    held = State()
    unknown = State()
    corrupt = State()

    written = (
        absent.to(held, on="keep")
        | held.to(held, on="keep")
        | unknown.to(held, on="keep")
        | corrupt.to(held, on="keep")
    )
    write_failed = absent.to(unknown) | held.to(unknown) | unknown.to(unknown) | corrupt.to(unknown)
    # On a damaged volume a file the model holds absent can still be found.
    corrupted = held.to(corrupt) | corrupt.to(corrupt) | unknown.to(unknown) | absent.to(unknown)

    def keep(self, seed: int) -> None:
        self.model.seed = seed


@dataclass(slots=True)
class FaultRecord:
    op: Literal["read", "write"] | None = None
    region: Region | None = None
    file: int | None = None
    once: bool = False
    state: str | None = None


class FaultLifecycle(_Chart[FaultRecord]):
    """The runtime disk fault. Arming replaces any fault in place. A one-shot
    fault is `spent` only when it was the sole possible cause of an EIO,
    otherwise it becomes `maybe_spent` and promises nothing strict, until an
    EIO only it can explain spends it after all."""

    disarmed = State(initial=True)
    armed = State()
    maybe_spent = State()

    aimed = (
        disarmed.to(armed, on="aim") | armed.to(armed, on="aim") | maybe_spent.to(armed, on="aim")
    )
    removed = disarmed.to(disarmed) | armed.to(disarmed) | maybe_spent.to(disarmed)
    spent = armed.to(disarmed, cond="one_shot") | maybe_spent.to(disarmed, cond="one_shot")
    maybe_fired = armed.to(maybe_spent, cond="one_shot") | maybe_spent.to(maybe_spent)

    def aim(self, op: Literal["read", "write"], region: Region, file: int, once: bool) -> None:
        self.model.op, self.model.region, self.model.file, self.model.once = op, region, file, once

    def one_shot(self) -> bool:
        return self.model.once


@dataclass(slots=True)
class FlagRecord:
    state: str | None = None


class VolumeLifecycle(_Chart[FlagRecord]):
    """The volume's metadata. Once a FAT or directory sector is overwritten
    the volume promises nothing again."""

    intact = State(initial=True)
    damaged = State()

    metadata_corrupted = intact.to(damaged) | damaged.to(damaged)


class WindowLifecycle(_Chart[FlagRecord]):
    """FatFs's sector window. A failed write may leave it dirty, and FatFs
    retries a dirty window before every later load, so reads may fail too."""

    clean = State(initial=True)
    dirty = State()

    left_dirty = clean.to(dirty) | dirty.to(dirty)


def diagrams() -> dict[str, str]:
    """Every lifecycle as Mermaid, for the docs."""
    charts = {
        "driver": DriverLifecycle(DriverRecord(budget=8)),
        "file": FileLifecycle(FileRecord()),
        "fault": FaultLifecycle(FaultRecord()),
        "volume": VolumeLifecycle(FlagRecord()),
        "window": WindowLifecycle(FlagRecord()),
    }
    return {name: f"{chart:mermaid}" for name, chart in charts.items()}
