"""What a step can be seen to do, typed so a misspelt outcome fails to check."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Literal


class Status(Enum):
    """Outcomes that carry nothing else."""

    OK = "ok"
    EIO = "eio"
    ERROR = "error"  # any other error, or a crashed probe
    ENOENT = "enoent"
    DIFFERS = "differs"  # a file read back with the wrong contents
    GONE = "gone"  # Root ignored a request for a driver it gave up on
    PRESENT = "present"  # a disk fault found its target
    ABSENT = "absent"  # the target file has no data on disk
    UNREACHABLE = "unreachable"  # the guest could not reach the host


@dataclass(frozen=True, slots=True)
class Seed:
    """A file read back whole, every byte `value`."""

    value: int


@dataclass(frozen=True, slots=True)
class Restarted:
    """Root restarted a driver, through a fault or a debug request."""

    path: Literal["restart", "debug-restart"]
    count: int


@dataclass(frozen=True, slots=True)
class GaveUp:
    """Root stopped a driver for good."""

    reason: str


type Outcome = Status | Seed | Restarted | GaveUp


def render(outcome: Outcome) -> str:
    """A short name, for reports and Hypothesis events."""
    match outcome:
        case Status():
            return outcome.value
        case Seed(value=v):
            return f"seed:{v}"
        case Restarted(path=p, count=c):
            return f"{p}:{c}"
        case GaveUp(reason=r):
            return f"giveup:{r}"
