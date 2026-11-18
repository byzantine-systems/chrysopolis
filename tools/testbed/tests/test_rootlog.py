"""The ring oracle against histories built the way Root publishes them,
as observed on the guest: injection, fault, restart or give-up per fault
request and per absorb, a bare restart per debug restart, a `restart_child`
control then the outcome per want Root answered, nothing once gone or never
asked.

| Planted defect | Caught by |
|---|---|
| a command counted for an `UNASKED` want | test_a_consistent_history_raises_nothing |
"""

from __future__ import annotations

from typing import Final, Literal

from hypothesis import given
from hypothesis import strategies as st

from chryso_testbed.faults import STEP, Absorb, Driver, Fault, Model, Step, Want
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted
from chryso_testbed.rootlog import Kind, RootEvent, disagreements

BUDGET: Final = 3
CHILDREN: Final[dict[Driver, int]] = {"blk": 2, "eth": 3}

type Request = Literal["fault", "restart", "want", "absorb"]

requests = st.lists(
    st.tuples(
        st.sampled_from(["blk", "eth"]), st.sampled_from(["fault", "restart", "want", "absorb"])
    ),
    max_size=12,
)


def history(
    chosen: list[tuple[Driver, Request]],
) -> tuple[list[tuple[Step, Outcome]], list[RootEvent]]:
    """Requests with Root's console answers, and the ring Root publishes."""
    model = Model.start(BUDGET)
    decisions: list[tuple[Step, Outcome]] = []
    events: list[RootEvent] = []

    def publish(kind: Kind, child: int, detail: str = "0", a: int = 0) -> None:
        events.append(
            RootEvent(
                index=len(events),
                ticks=len(events),
                kind=kind,
                child=child,
                detail=detail,
                a=a,
                b=0,
            )
        )

    for driver, request in chosen:
        step = STEP.validate_python({"kind": request, "driver": driver})
        (seen,) = model.expect(step).values
        child = CHILDREN[driver]
        answered = isinstance(seen, Restarted | GaveUp)
        if answered and isinstance(step, Fault | Absorb):
            publish(Kind.CONTROL, child, "fault_inject")
            publish(Kind.FAULT, child, "6")
        if answered and isinstance(step, Want):
            publish(Kind.CONTROL, child, "restart_child")
        if isinstance(seen, Restarted):
            publish(Kind.RESTART, child, a=seen.count)
        elif isinstance(seen, GaveUp):
            publish(Kind.GIVEUP, child, "budget_exhausted", a=BUDGET)
        decisions.append((step, seen))
        model = model.after(step, seen)
    return decisions, events


@given(chosen=requests)
def test_a_consistent_history_raises_nothing(
    chosen: list[tuple[Driver, Request]],
) -> None:
    decisions, events = history(chosen)
    assert disagreements(decisions, events, CHILDREN) == []


@given(chosen=requests.filter(bool), data=st.data())
def test_a_missing_event_is_noticed(
    chosen: list[tuple[Driver, Request]], data: st.DataObject
) -> None:
    decisions, events = history(chosen)
    if not events:
        return
    gone = data.draw(st.integers(0, len(events) - 1))
    assert disagreements(decisions, events[:gone] + events[gone + 1 :], CHILDREN)


@given(chosen=requests)
def test_a_fault_published_before_its_injection_is_noticed(
    chosen: list[tuple[Driver, Request]],
) -> None:
    decisions, events = history(chosen)
    pairs = [
        i
        for i in range(len(events) - 1)
        if events[i].kind == Kind.CONTROL and events[i + 1].kind == Kind.FAULT
    ]
    if not pairs:
        return
    i = pairs[0]
    swapped = [
        *events[:i],
        events[i + 1].model_copy(update={"index": events[i].index}),
        events[i].model_copy(update={"index": events[i + 1].index}),
        *events[i + 2 :],
    ]
    assert disagreements(decisions, swapped, CHILDREN)


@given(chosen=requests)
def test_a_command_with_no_outcome_after_it_is_noticed(
    chosen: list[tuple[Driver, Request]],
) -> None:
    """A `restart_child` control must be followed, for its child, by the
    restart or give-up it caused. Moving it to the end breaks that."""
    decisions, events = history(chosen)
    commands = [e for e in events if e.kind == Kind.CONTROL and e.detail == "restart_child"]
    if not commands:
        return
    moved = [e for e in events if e != commands[0]]
    moved = [e.model_copy(update={"index": i}) for i, e in enumerate(moved)]
    last = commands[0].model_copy(update={"index": len(moved)})
    assert disagreements(decisions, [*moved, last], CHILDREN)
