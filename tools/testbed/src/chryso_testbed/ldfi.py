"""Lineage-driven fault injection: run, find why each step succeeded, inject
the smallest fault sets that would undo it, and repeat on what survives.

Every run is judged by the model and by the ring oracle. Failures go to the
corpus, which the fuzz replays. `ldfi.jsonl` records every run either way.
"""

from __future__ import annotations

import asyncio
import json
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Final

import structlog

from chryso_testbed import inject, lineage, probes, rootlog
from chryso_testbed.console import ScenarioFailure
from chryso_testbed.faults import PROGRAM, Driver, Fault, Get, Model, Ping, Program, Put, Step
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.outcomes import Outcome, render
from chryso_testbed.scenario import Settings, default_failures

log = structlog.get_logger(__name__)
IMAGE: Final = "restart-image"
MAX_STEPS: Final = 32
RUN_TIMEOUT: Final = 600.0
RECOVERABLE: Final[tuple[Driver, ...]] = ("blk", "eth")
# Known-good workloads to start from: each success in them has a lineage.
SEEDS: Final[tuple[Program, ...]] = (
    (Put(kind="put", file=0, seed=7), Get(kind="get", file=0), Ping(kind="ping")),
    (
        Put(kind="put", file=1, seed=3),
        Fault(kind="fault", driver="blk"),
        Get(kind="get", file=1),
        Ping(kind="ping"),
    ),
)


@dataclass(frozen=True, slots=True)
class Run:
    """One program's run: what each step saw, and why it failed, if it did."""

    trace: list[tuple[Step, Outcome, Model]]
    failure: str | None


async def execute(settings: Settings, program: Program, workdir: Path) -> Run:
    """Boot, run `program` against the model, then check Root's ring."""
    trace: list[tuple[Step, Outcome, Model]] = []
    profile = Profile(settings.images[IMAGE], settings.disk)
    failures = default_failures(settings.abi)
    try:
        async with Machine("ldfi", profile, workdir, settings.tools, failures) as m:
            async with asyncio.timeout(RUN_TIMEOUT):
                await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
                banner = await m.console.wait("Eshell")
                await probes.load_test_modules(m, settings.test_modules, banner)
                _, ring = rootlog.read(
                    await probes.run(m, "chryso_ctl:events(start, 0).", "CTL_EVENTS.START")
                )
                await inject.check(
                    m,
                    program,
                    Model.start(settings.abi.driver_budget),
                    abi=settings.abi,
                    observe=lambda step, seen, before: trace.append((step, seen, before)),
                )
                events, _ = rootlog.read(
                    await probes.run(
                        m, f"chryso_ctl:events(finish, {ring.head}).", "CTL_EVENTS.FINISH"
                    )
                )
        children = {d: settings.abi.child(d) for d in RECOVERABLE}
        decisions = [(step, seen) for step, seen, _ in trace]
        if problems := rootlog.disagreements(decisions, events, children):
            return Run(trace, f"Root's ring disagrees with its console: {problems}")
        return Run(trace, None)
    except (ScenarioFailure, TimeoutError) as failure:
        return Run(trace, str(failure) or "a deadline expired")


def describe(hit: frozenset[lineage.Atom]) -> str:
    return ", ".join(sorted(map(repr, hit)))


def _append(path: Path, record: dict[str, object]) -> None:
    with path.open("a") as report:
        report.write(json.dumps(record) + "\n")


async def explore(
    settings: Settings, out: Path, *, boots: int, per_run: int, seeds: tuple[Program, ...]
) -> list[dict[str, object]]:
    """Breadth-first over cuts, until `boots` runs are spent. Returns the
    corpus: every program that failed, with the cut that led to it."""
    report = out / "ldfi.jsonl"
    await asyncio.to_thread(out.mkdir, parents=True, exist_ok=True)
    await asyncio.to_thread(report.write_text, "")
    queue: deque[tuple[Program, str, int | None]] = deque((s, "seed", None) for s in seeds)
    tried: set[str] = set()
    corpus: list[dict[str, object]] = []
    for number in range(boots):
        while queue and json.dumps(PROGRAM.dump_python(queue[0][0])) in tried:
            queue.popleft()
        if not queue:
            break
        program, cut, parent = queue.popleft()
        dumped = PROGRAM.dump_python(program)
        tried.add(json.dumps(dumped))
        with structlog.contextvars.bound_contextvars(run=number, cut=cut):
            log.info("ldfi.run", steps=len(program), parent=parent)
            run = await execute(settings, program, out / f"run-{number}")
        record: dict[str, object] = {
            "run": number,
            "parent": parent,
            "cut": cut,
            "program": dumped,
            "outcomes": [f"{s.kind}: {render(o)}" for s, o, _ in run.trace],
            "failure": run.failure,
        }
        await asyncio.to_thread(_append, report, record)
        if run.failure is not None:
            log.warning("ldfi.failure", run=number, cut=cut, failure=run.failure[:200])
            corpus.append({"cut": cut, "program": dumped})
            continue
        for _, hit, edited in lineage.candidates(program, run.trace)[:per_run]:
            if len(edited) <= MAX_STEPS:
                queue.append((edited, describe(hit), number))
    await asyncio.to_thread((out / "corpus.json").write_text, json.dumps(corpus, indent=2) + "\n")
    return corpus


def load_corpus(path: Path) -> tuple[Program, ...]:
    """Programs from a corpus file, or none if it does not exist."""
    if not path.exists():
        return ()
    return tuple(
        PROGRAM.validate_python(entry["program"]) for entry in json.loads(path.read_text())
    )
