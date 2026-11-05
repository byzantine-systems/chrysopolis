"""Steered fault programs against `restart-image`, one boot per example.

`nix run .#testbed-fuzz -- -k system --hypothesis-show-statistics`, with
SYSTEM_FUZZ_EXAMPLES examples (default 5).
"""

from __future__ import annotations

import asyncio
import itertools
import json
import os
from dataclasses import dataclass
from pathlib import Path
from time import perf_counter

import pytest
import structlog
from hypothesis import HealthCheck, event, example, given, note, settings, target
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed import crashcheck, crashlog, inject, probes
from chryso_testbed.__main__ import load
from chryso_testbed.abi import Abi
from chryso_testbed.blkdebug import PLAN, Expected, Plan, shaped
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.crashcheck import Span
from chryso_testbed.faults import (
    FILES,
    PROGRAM,
    Arm,
    Corrupt,
    Fault,
    Get,
    Model,
    Ping,
    Program,
    Put,
    Step,
    Unplug,
)
from chryso_testbed.ldfi import load_corpus
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.outcomes import Outcome, render
from chryso_testbed.scenario import Settings, default_failures
from chryso_testbed.steer import SCENARIOS, programs

log_ = structlog.get_logger()
IMAGE = "restart-image"
EXAMPLES = itertools.count(1)
MANIFEST = Path(os.environ["CHRYSO_TESTBED_MANIFEST"])
# Steering simulates Root's budget, so it needs the ABI before any boot.
BUDGET = Abi.load(Path(json.loads(MANIFEST.read_text())["abi"])).driver_budget

PLANS = st.builds(
    shaped,
    from_schema(PLAN_SCHEMA).map(PLAN.validate_python),
    st.sampled_from(["clean", "single", "transient", "any"]),
)


def _program_for(plan: Plan, name: str) -> st.SearchStrategy[Run]:
    start = Model.start(BUDGET, Expected.of(plan))
    return programs(start, name).map(lambda program: Run(plan, name, program))


@dataclass(frozen=True, slots=True)
class Run:
    """One example: the boot's disk plan, the scenario and its program."""

    plan: Plan
    scenario: str
    program: Program


STEERED = st.tuples(PLANS, st.sampled_from(sorted(SCENARIOS))).flatmap(
    lambda chosen: _program_for(*chosen)
)
# Programs `chryso-testbed ldfi` found failing, replayed every campaign.
CORPUS = tuple(Run((), "corpus", p) for p in load_corpus(Path(__file__).with_name("corpus.json")))
RUNS = st.one_of(STEERED, st.sampled_from(CORPUS)) if CORPUS else STEERED


def abstract(model: Model) -> tuple[object, ...]:
    """The part of the state coverage counts: the armed fault, which drivers
    are gone, and which file states exist."""
    armed = model.storage.armed
    files = frozenset(model.storage.file(f).at for f in range(FILES))
    return (
        None if armed is None else (armed.op, armed.region),
        model.root.gone,
        files,
        model.storage.damaged,
    )


def reaction(step: Step, seen: Outcome, model: Model) -> str | None:
    """A row of the fault-reaction table, for steps a disk fault could touch."""
    if not isinstance(step, Put | Get):
        return None
    if (armed := model.storage.armed) is not None:
        fault = f"{armed.op}-error/{armed.region}" + ("/once" if armed.once else "")
    elif model.storage.file(step.file).at == "corrupt":
        fault = "corrupt/data"
    elif model.storage.damaged:
        fault = "damaged-volume"
    else:
        return None
    return f"{fault}: {step.kind} -> {render(seen)}"


CRASH_POINTS = 4


def crash_checks(
    raw: bytes, spans: list[Span], base: Path, scratch: Path
) -> tuple[list[str], list[str]]:
    """Crash images at up to CRASH_POINTS flushes, evenly spaced, each held to
    what the writes finished before it promised. Returns the problems, and
    events (fsck's verdict on the first and last, since FatFs promises no
    clean volume) for the caller to record: this runs off the test's thread."""
    log = crashlog.parse(raw)
    flushes = len(log.flushes)
    if not flushes:
        return [], ["crash points checked: 0"]
    chosen = sorted({round(i * (flushes - 1) / (CRASH_POINTS - 1)) for i in range(CRASH_POINTS)})
    found, events = [], [f"crash points checked: {len(chosen)}"]
    for k in chosen:
        t0 = perf_counter()
        image = crashlog.replay(log, base, scratch / f"crash-{k}.img", crashlog.state(log, k + 1))
        t1 = perf_counter()
        promised = crashcheck.durable(spans, crashlog.stable(log, k + 1))
        verdict = crashcheck.judge(image, promised)
        t2 = perf_counter()
        found += [f"crash after flush {k}: {p} (image {image})" for p in verdict.problems]
        events += [f"crash left a broken chain in {name}" for name in verdict.broken]
        if verdict.diverged:
            events.append("crash left the two FATs different")
        if k in (chosen[0], chosen[-1]):
            events.append(f"crash fsck exit {crashcheck.fsck(image, scratch)}")
        if not verdict.problems:
            image.unlink()  # 100 MB each: keep only an image that broke a promise
        log_.debug("crash.point", flush=k, replay_s=t1 - t0, judge_s=t2 - t1,
                   fsck_s=perf_counter() - t2)  # fmt: skip
    return found, events


@pytest.fixture(scope="session")
def testbed(tmp_path_factory: pytest.TempPathFactory) -> Settings:
    manifest = Path(os.environ["CHRYSO_TESTBED_MANIFEST"])
    return load(manifest, tmp_path_factory.mktemp("system"), 1)


@pytest.mark.asyncio
@settings(
    max_examples=int(os.environ.get("SYSTEM_FUZZ_EXAMPLES", "5")),
    deadline=None,
    print_blob=True,
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.data_too_large],
)
@given(run=RUNS)
# Found by this test: a link change left virtio-net's config interrupt unacknowledged.
@example(
    run=Run((), "pinned", (Ping(kind="ping"), Unplug(kind="unplug", ms=1500), Ping(kind="ping")))
)
# Found by this test: a rewrite may move a file's data off a faulted sector
# (a gap in the model, not the system).
@example(
    run=Run(
        (),
        "pinned",
        (
            Put(kind="put", file=0, seed=0),
            Arm(kind="arm", op="read", region="data", file=0, once=False),
            Get(kind="get", file=0),
            Put(kind="put", file=0, seed=0),
            Get(kind="get", file=0),
        ),
    )
)
# Found by this test: with blk given up on, FatFs still reports a missing
# file from its cached directory sector (a gap in the model, not the system).
@example(
    run=Run(
        (),
        "pinned",
        (*[Fault(kind="fault", driver="blk")] * (BUDGET + 1), Get(kind="get", file=3)),
    )
)
async def test_the_system_does_what_its_model_allows(testbed: Settings, run: Run) -> None:
    """Under any plan and steered program the system boots, no PD faults
    unasked, and every step's outcome is one the model allows from the state
    before it."""
    plan, program = run.plan, run.program
    model = Model.start(testbed.abi.driver_budget, Expected.of(plan))
    event(f"scenario {run.scenario}")
    covered: set[tuple[object, str]] = set()
    spans: list[Span] = []
    workdir = testbed.out / f"example-{next(EXAMPLES)}"
    note(f"plan={PLAN.dump_python(plan)} console={workdir}")
    note(f"program={PROGRAM.dump_python(program)}")
    profile = Profile(
        testbed.images[IMAGE], testbed.disk, blkdebug=plan, crashlog=True, reboot=True
    )
    async with Machine(
        "system", profile, workdir, testbed.tools, default_failures(testbed.abi)
    ) as m:

        def observe(step: Step, seen: Outcome, before: Model) -> None:
            start = spans[-1].end if spans else 0
            spans.append(Span(step, seen, start, m.logged()))
            note(f"{step!r}: {render(seen)}")
            covered.add((abstract(before), step.kind))
            if (row := reaction(step, seen, before)) is not None:
                event(row)
            if isinstance(step, Arm | Corrupt):
                event(f"aim {step.kind} -> {render(seen)}")

        async with asyncio.timeout(400):
            await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, testbed.test_modules, banner)
        model = await inject.check(
            m, program, model, abi=testbed.abi, modules=testbed.test_modules, observe=observe
        )
        for driver in model.root.gone:
            event(f"gave up {driver}")
        target(len(covered), label="state-step pairs covered")
        async with asyncio.timeout(60):
            await probes.call(m, "chryso_test:alive(fuzz).", r"FUZZ_ALIVE\|42")
        # The log lives in the machine's scratch, which leaving the context removes.
        raw = await asyncio.to_thread(m.write_log.read_bytes)
    broken, seen = await asyncio.to_thread(crash_checks, raw, spans, testbed.disk, workdir)
    for name in seen:
        event(name)
    assert not broken, broken
