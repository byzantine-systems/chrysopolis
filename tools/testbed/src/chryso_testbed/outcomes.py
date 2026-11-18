"""What a step can be seen to do, typed so a misspelt outcome fails to check."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Literal, TypeIs, get_args


class Status(Enum):
    """Outcomes that carry nothing else."""

    OK = "ok"
    EIO = "eio"
    ERROR = "error"  # any other error, or a crashed probe
    ENOENT = "enoent"
    EEXIST = "eexist"  # a rename onto a name that exists
    DIFFERS = "differs"  # a file read back with the wrong contents
    GONE = "gone"  # Root ignored a request for a driver it gave up on
    UNASKED = "unasked"  # the orchestrator sent Root no command
    PRESENT = "present"  # a disk fault found its target
    ABSENT = "absent"  # the target file has no data on disk
    UNREACHABLE = "unreachable"  # the guest could not reach the host


@dataclass(frozen=True, slots=True)
class Seed:
    """A file read back whole, every byte `value`."""

    value: int


@dataclass(frozen=True, slots=True)
class Restarted:
    """Root restarted a driver, through a fault, a debug request or a command
    from the orchestrator. `path` is Root's own word for it."""

    path: Literal["restart", "debug-restart", "ctl-restart"]
    count: int


@dataclass(frozen=True, slots=True)
class GaveUp:
    """Root stopped a driver for good."""

    reason: str


type ConditionName = Literal["unknown", "gone", "restarting", "recovering", "degraded", "ready"]
type Reason = Literal[
    "no_observation",
    "gave_up",
    "restart_requested",
    "rate_limited",
    "restarted",
    "low_budget",
    "stopped",
    "quarantined",
    "unset",
    "settled",
]


@dataclass(frozen=True, slots=True)
class Condition:
    """A driver's condition as the orchestrator reports it, the closed sets
    of `chryso_condition`."""

    name: ConditionName
    reason: Reason

    @classmethod
    def parse(cls, name: str, reason: str) -> Condition | None:
        """The condition a probe reported, or None outside the closed sets."""
        if _is_name(name) and _is_reason(reason):
            return cls(name, reason)
        return None


def _is_name(value: str) -> TypeIs[ConditionName]:
    return value in get_args(ConditionName.__value__)


def _is_reason(value: str) -> TypeIs[Reason]:
    return value in get_args(Reason.__value__)


type BootProfile = Literal["start", "orch"]


@dataclass(frozen=True, slots=True)
class Booted:
    """The boot script a new BEAM incarnation took, as `boot_info` reports it."""

    profile: BootProfile


type Outcome = Status | Seed | Restarted | GaveUp | Condition | Booted


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
        case Condition(name=n, reason=r):
            return f"{n}:{r}"
        case Booted(profile=p):
            return f"boot:{p}"
