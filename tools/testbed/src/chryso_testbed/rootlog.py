"""Root's event ring, read back, and an oracle checking it against the
decisions Root printed on the console.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from enum import StrEnum

from pydantic import BaseModel, ConfigDict

from chryso_testbed import frames
from chryso_testbed.faults import Driver, Fault, Restart, Step
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted


class Kind(StrEnum):
    UNSET = "unset"
    BOOT = "boot"
    FAULT = "fault"
    RESTART = "restart"
    STOP = "stop"
    GIVEUP = "giveup"
    QUARANTINE = "quarantine"
    RECLAIM = "reclaim"
    CONTROL = "control"
    SPEC_REJECT = "spec_reject"


class RootEvent(BaseModel):
    """One ring entry. For a restart `a` is the child's lifetime count, for a
    give-up `detail` is the reason, for a control event the request."""

    model_config = ConfigDict(frozen=True, extra="forbid")

    index: int
    ticks: int
    kind: Kind
    child: int
    detail: str
    a: int
    b: int


class Ring(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid")

    head: int
    dropped: int
    lost: int


def read(result: frames.Pass) -> tuple[tuple[RootEvent, ...], Ring]:
    """The events and ring summary of one `CTL_EVENTS` run."""
    events = tuple(
        RootEvent.model_validate(dict(f.fields)) for f in result.frames if f.kind == "event"
    )
    rings = [dict(f.fields) for f in result.frames if f.kind == "ring"]
    if len(rings) != 1 or "error" in rings[0]:
        raise ValueError(f"no readable ring summary: {rings}")
    return events, Ring.model_validate(rings[0])


def disagreements(
    decisions: Sequence[tuple[Step, Outcome]],
    events: Iterable[RootEvent],
    children: Mapping[Driver, int],
) -> list[str]:
    """Where Root's console decisions and its ring disagree, for the drivers
    in `children`. Empty when they tell the same story.

    `decisions` are the run's fault and restart requests with what the
    console said, in order. Requests Root ignored (gone) leave no event."""
    problems = []
    for driver, child in children.items():
        mine = [e for e in events if e.child == child]
        said = [
            (s, o) for s, o in decisions if isinstance(s, Fault | Restart) and s.driver == driver
        ]
        restarts = [o.count for _, o in said if isinstance(o, Restarted)]
        published = [e.a for e in mine if e.kind == Kind.RESTART]
        if restarts != published:
            problems.append(f"{driver}: console restarts {restarts}, ring restarts {published}")
        gave_up = sum(isinstance(o, GaveUp) for _, o in said)
        giveups = [e for e in mine if e.kind == Kind.GIVEUP]
        if gave_up != len(giveups) or any(e.detail != "budget_exhausted" for e in giveups):
            problems.append(f"{driver}: console gave up {gave_up} times, ring {giveups}")
        injected = sum(isinstance(s, Fault) and isinstance(o, Restarted | GaveUp) for s, o in said)
        controls = [e for e in mine if e.kind == Kind.CONTROL and e.detail == "fault_inject"]
        faults = [e for e in mine if e.kind == Kind.FAULT]
        if injected != len(controls) or injected != len(faults):
            problems.append(
                f"{driver}: {injected} accepted injections, ring {len(controls)} fault_inject "
                f"and {len(faults)} fault events"
            )
        elif any(c.index > f.index for c, f in zip(controls, faults, strict=True)):
            problems.append(f"{driver}: a fault event precedes its injection")
    return problems
