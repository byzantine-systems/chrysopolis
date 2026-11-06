"""Why a step succeeded, and the smallest fault sets that would undo it (LDFI).

A success is supported by alternatives: the driver instance that served it,
or its recovery completing while budget remains, plus the disk sectors it
used. Minimal hitting sets of those alternatives become program edits.
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from itertools import product
from typing import Final, Literal

from chryso_testbed.fatlayout import REGIONS, Region
from chryso_testbed.faults import (
    Arm,
    Disarm,
    Driver,
    Fault,
    Get,
    Model,
    Ping,
    Program,
    Put,
    Step,
)
from chryso_testbed.outcomes import Outcome, Seed, Status

# Serial and timer carry every probe, so the run never cuts them.
PATHS: Final[dict[type[Step], tuple[Driver, ...]]] = {Put: ("blk",), Get: ("blk",), Ping: ("eth",)}


@dataclass(frozen=True, slots=True, order=True)
class Instance:
    """The driver incarnation that served the step."""

    driver: Driver
    epoch: int


@dataclass(frozen=True, slots=True, order=True)
class Recovered:
    """Root restarting the driver into the next incarnation."""

    driver: Driver
    epoch: int


@dataclass(frozen=True, slots=True, order=True)
class Budget:
    """Root still having budget to restart the driver."""

    driver: Driver


@dataclass(frozen=True, slots=True, order=True)
class Sectors:
    """One region of the file the step touched being readable or writable."""

    region: Region
    file: int


type Atom = Instance | Recovered | Budget | Sectors
type Support = frozenset[Atom]


def succeeded(step: Step, outcome: Outcome) -> bool:
    return outcome == Status.OK or (isinstance(outcome, Seed) and isinstance(step, Get))


def lineage(step: Step, model: Model) -> tuple[Support, ...]:
    """The alternative supports of `step` succeeding from `model`. Each
    support is a set of facts that together suffice."""
    drivers = PATHS.get(type(step), ())
    per_driver = []
    for d in drivers:
        chart = model.root.chart(d)
        epoch = chart.model.spent
        alternatives: list[frozenset[Atom]] = [frozenset({Instance(d, epoch)})]
        if chart.has_budget():
            alternatives.append(frozenset({Recovered(d, epoch + 1), Budget(d)}))
        per_driver.append(alternatives)
    sectors: frozenset[Atom] = frozenset()
    if isinstance(step, Put | Get) and model.storage.file(step.file).at in {"held", "corrupt"}:
        sectors = frozenset(Sectors(r, step.file) for r in REGIONS)
    return tuple(frozenset().union(sectors, *combo) for combo in product(*per_driver))


def hitting_sets(supports: Sequence[Support], max_size: int = 2) -> list[frozenset[Atom]]:
    """Minimal sets of atoms meeting every support, at most `max_size` big
    (Berge's incremental algorithm)."""
    transversals: set[frozenset[Atom]] = {frozenset()}
    for support in supports:
        grown = {t if t & support else t | {atom} for t in transversals for atom in support}
        transversals = {t for t in grown if len(t) <= max_size}
        transversals = {t for t in transversals if not any(o < t for o in transversals)}
    return sorted(transversals, key=lambda t: (len(t), sorted(map(repr, t))))


def cut(hit: frozenset[Atom], step: Step, model: Model) -> tuple[list[Step], list[Step]]:
    """Steps to insert before and after `step` so the facts in `hit` fail."""
    before: list[Step] = []
    after: list[Step] = []
    drivers = sorted({a.driver for a in hit if isinstance(a, Instance | Recovered | Budget)})
    for d in drivers:
        kinds = {type(a) for a in hit if not isinstance(a, Sectors) and a.driver == d}
        if Budget in kinds:
            chart = model.root.chart(d)
            left = chart.model.budget - chart.model.spent + 1
            before += [Fault(kind="fault", driver=d)] * left
        elif Recovered in kinds:
            # The second fault lands in the first one's recovery window and
            # leaves the driver recovering when the step runs.
            unsettled = Fault(kind="fault", driver=d, settle=False)
            before += [unsettled, unsettled]
        else:
            before.append(Fault(kind="fault", driver=d))
    for a in sorted(x for x in hit if isinstance(x, Sectors)):
        op: Literal["read", "write"] = "read" if isinstance(step, Get) else "write"
        before.append(Arm(kind="arm", op=op, region=a.region, file=a.file, once=False))
        after.append(Disarm(kind="disarm"))
    return before, after


def edit(program: Program, index: int, hit: frozenset[Atom], model: Model) -> Program:
    """`program` with the faults that cut `hit` placed around step `index`."""
    before, after = cut(hit, program[index], model)
    return (*program[:index], *before, program[index], *after, *program[index + 1 :])


def candidates(
    program: Program, trace: Iterable[tuple[Step, Outcome, Model]], max_size: int = 2
) -> list[tuple[int, frozenset[Atom], Program]]:
    """For every success in a run, each minimal cut and the program it gives."""
    found = []
    for index, (step, seen, before) in enumerate(trace):
        if not succeeded(step, seen):
            continue
        for hit in hitting_sets(lineage(step, before), max_size):
            if hit:
                found.append((index, hit, edit(program, index, hit, before)))
    return found
