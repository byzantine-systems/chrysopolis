"""Steered fault programs against `restart-image`, one boot per example.

`nix run .#testbed-fuzz -- -k system --hypothesis-show-statistics`. Knobs,
validated once at import (`FuzzKnobs`): SYSTEM_FUZZ_EXAMPLES (default 5),
SYSTEM_FUZZ_SCENARIOS (a comma list of steering scenarios, default all) and
SYSTEM_FUZZ_DISK (a disk from the manifest's `disks`, such as a negative
control, default the shipped disk).

After an example that used an orchestrator step, its decision log must
replay and Root's ring must agree with the console since the last reset.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import os
from dataclasses import dataclass, replace
from pathlib import Path
from time import perf_counter

import pytest
import structlog
from hypothesis import HealthCheck, event, example, given, note, settings, target
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema
from pydantic import BaseModel, ConfigDict, Field, ValidationInfo, field_validator

from chryso_testbed import crashcheck, crashlog, inject, probes, rootlog
from chryso_testbed.__main__ import load
from chryso_testbed.abi import Abi
from chryso_testbed.blkdebug import PLAN, Expected, Plan, shaped
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.crashcheck import Span
from chryso_testbed.faults import (
    DRIVERS,
    FILES,
    PROGRAM,
    Absorb,
    Arm,
    Corrupt,
    CrashPut,
    Fault,
    Get,
    Look,
    Model,
    Overwrite,
    Ping,
    Program,
    Put,
    Remove,
    Rename,
    Reset,
    Rewant,
    Step,
    Unplug,
    Want,
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
_RAW_MANIFEST = json.loads(MANIFEST.read_text())
# Steering simulates Root's budget, so it needs the ABI before any boot.
BUDGET = Abi.load(Path(_RAW_MANIFEST["abi"])).driver_budget


class FuzzKnobs(BaseModel):
    """The campaign's environment knobs, checked before any example runs, so a
    misspelt scenario or disk fails collection instead of drawing nothing."""

    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)

    examples: int = Field(ge=1)
    scenarios: tuple[str, ...]
    disk: str | None

    @field_validator("scenarios")
    @classmethod
    def known_scenarios(cls, value: tuple[str, ...]) -> tuple[str, ...]:
        if unknown := [name for name in value if name not in SCENARIOS]:
            raise ValueError(f"unknown scenarios {unknown}, known {sorted(SCENARIOS)}")
        if not value:
            raise ValueError("no scenario chosen")
        return value

    @field_validator("disk")
    @classmethod
    def known_disk(cls, value: str | None, info: ValidationInfo) -> str | None:
        disks = info.context["disks"] if info.context else {}
        if value is not None and value not in disks:
            raise ValueError(f"unknown disk {value!r}, known {sorted(disks)}")
        return value


def _knobs() -> FuzzKnobs:
    env = os.environ
    chosen = env.get("SYSTEM_FUZZ_SCENARIOS", "")
    return FuzzKnobs.model_validate(
        {
            "examples": int(env.get("SYSTEM_FUZZ_EXAMPLES", "5")),
            "scenarios": tuple(chosen.split(",")) if chosen else tuple(sorted(SCENARIOS)),
            "disk": env.get("SYSTEM_FUZZ_DISK") or None,
        },
        context={"disks": _RAW_MANIFEST.get("disks", {})},
    )


KNOBS = _knobs()

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
    crash_seed: int = 0  # which writes of each cut epoch survive, 0 keeps none


STEERED = st.tuples(PLANS, st.sampled_from(KNOBS.scenarios)).flatmap(
    lambda chosen: _program_for(*chosen)
)
# Programs `chryso-testbed ldfi` found failing, replayed every campaign.
CORPUS = tuple(Run((), "corpus", p) for p in load_corpus(Path(__file__).with_name("corpus.json")))
_BASE = st.one_of(STEERED, st.sampled_from(CORPUS)) if CORPUS else STEERED
RUNS = st.builds(lambda run, seed: replace(run, crash_seed=seed), _BASE, st.integers(0, 2**32 - 1))


def abstract(model: Model) -> tuple[object, ...]:
    """The part of the state coverage counts: the armed fault, each driver's
    chart, which file states exist, and what the orchestrator wants."""
    armed = model.storage.armed
    files = frozenset(model.storage.file(f).at for f in range(FILES))
    return (
        None if armed is None else (armed.op, armed.region),
        tuple(model.root.chart(d).at for d in DRIVERS),
        files,
        model.storage.damaged,
        tuple(sorted(model.orch.wanted)),
    )


def reaction(step: Step, seen: Outcome, model: Model) -> str | None:
    """A row of the fault-reaction table, for steps a disk fault could touch,
    and for orchestrator steps on a driver that is recovering or gone."""
    if isinstance(step, Want | Rewant | Look | Absorb):
        state = model.root.chart(step.driver).at
        return (
            None if state == "running" else f"{state}/{step.driver}: {step.kind} -> {render(seen)}"
        )
    if not isinstance(step, Put | Get | Overwrite | Remove | Rename):
        return None
    file = step.src if isinstance(step, Rename) else step.file
    if (armed := model.storage.armed) is not None:
        fault = f"{armed.op}-error/{armed.region}" + ("/once" if armed.once else "")
    elif model.storage.file(file).at == "corrupt":
        fault = "corrupt/data"
    elif model.storage.damaged:
        fault = "damaged-volume"
    else:
        return None
    return f"{fault}: {step.kind} -> {render(seen)}"


CRASH_POINTS = 4


def crash_checks(
    raw: bytes, spans: list[Span], base: Path, scratch: Path, seed: int
) -> tuple[list[str], list[str]]:
    """Judge crash images at up to CRASH_POINTS flushes, each with a seeded part
    of the next epoch. Returns problems and events for the test thread."""
    log = crashlog.parse(raw)
    flushes = len(log.flushes)
    if not flushes:
        return [], ["crash points checked: 0"]
    chosen = sorted({round(i * (flushes - 1) / (CRASH_POINTS - 1)) for i in range(CRASH_POINTS)})
    found, events = [], [f"crash points checked: {len(chosen)}"]
    for k in chosen:
        t0 = perf_counter()
        part = crashlog.persisted(log, k + 1, seed)
        size = len(log.epochs[k + 1]) if k + 1 < len(log.epochs) else 0
        share = "none" if not part else "all" if len(part) == size else "part"
        events.append(f"crash kept {share} of its epoch")
        kept = crashlog.state(log, k + 1, part)
        image = crashlog.replay(log, base, scratch / f"crash-{k}.img", kept)
        t1 = perf_counter()
        crash = crashlog.crashed_at(log, k + 1, part)
        stable = crashlog.stable(log, k + 1)
        promised = crashcheck.allowed(spans, stable, crash)
        verdict = crashcheck.judge(image, promised, cut=crashcheck.moves(spans, stable, crash))
        t2 = perf_counter()
        found += [f"crash after flush {k}: {p} (image {image})" for p in verdict.problems]
        events += [f"crash left a broken chain in {name}" for name in verdict.broken]
        if verdict.diverged:
            events.append("crash left the two FATs different")
        events += ["crash left a rename on both names"] * len(verdict.crosslinked)
        if k in (chosen[0], chosen[-1]):
            code = crashcheck.fsck(image, scratch)
            events.append(f"crash fsck exit {code}")
            if code > 1:  # 1 reports lost clusters, which a crash may leave
                found.append(f"crash after flush {k}: fsck.fat exited {code} (image {image})")
        if not verdict.problems:
            image.unlink()  # keep only an image that broke a promise
        log_.debug("crash.point", flush=k, replay_s=t1 - t0, judge_s=t2 - t1,
                   fsck_s=perf_counter() - t2)  # fmt: skip
    return found, events


async def orchestrator_audits(
    m: Machine, testbed: Settings, decisions: list[tuple[Step, Outcome]]
) -> None:
    """The decision log replays through the pure step, and Root's ring agrees
    with the console since the last reset, which starts the ring over."""
    result = await probes.run(m, "chryso_orch:replay(fuzz).", "ORCH_REPLAY.FUZZ")
    (replay,) = probes.records(result, "replay")
    assert replay.get("verdict") == "match", dict(replay.fields)
    last_reset = max(
        (i for i, (step, _) in enumerate(decisions) if isinstance(step, Reset | CrashPut)),
        default=-1,
    )
    events, _ = rootlog.read(await probes.run(m, "chryso_ctl:events(fuzz, 0).", "CTL_EVENTS.FUZZ"))
    since = decisions[last_reset + 1 :]
    children = {d: testbed.abi.child(d) for d in DRIVERS}
    problems = rootlog.disagreements(since, events, children)
    assert not problems, problems
    event("audited")


@pytest.fixture(scope="session")
def testbed(tmp_path_factory: pytest.TempPathFactory) -> Settings:
    manifest = Path(os.environ["CHRYSO_TESTBED_MANIFEST"])
    return load(manifest, tmp_path_factory.mktemp("system"), 1)


@pytest.mark.asyncio
@settings(
    max_examples=KNOBS.examples,
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
# Found by this test: a crash state holding part of a rewrite (crash_seed 1)
# was held to the old seed (an oracle mistake: started is judged at the crash).
@example(
    run=Run(
        (),
        "pinned",
        (
            Put(kind="put", file=0, seed=0),
            Corrupt(kind="corrupt", region="data", file=0),
            Get(kind="get", file=0),
            Put(kind="put", file=0, seed=0),
            Get(kind="get", file=0),
        ),
        crash_seed=1,
    )
)
# The orchestrator: a want settles its driver before the read after it.
@example(
    run=Run(
        (),
        "pinned",
        (Put(kind="put", file=1, seed=4), Want(kind="want", driver="blk"), Get(kind="get", file=1)),
    )
)
# A look and a want inside eth's recovery window.
@example(
    run=Run(
        (),
        "pinned",
        (
            Fault(kind="fault", driver="eth", settle=False),
            Look(kind="look", driver="eth"),
            Want(kind="want", driver="eth"),
        ),
    )
)
# A fault inside a want, then the want held (W13).
@example(
    run=Run(
        (), "pinned", (Absorb(kind="absorb", driver="blk"), Rewant(kind="rewant", driver="blk"))
    )
)
# A fault inside a want on eth's last restart gives it up, and a want asks nothing.
@example(
    run=Run(
        (),
        "pinned",
        (
            *[Fault(kind="fault", driver="eth")] * BUDGET,
            Absorb(kind="absorb", driver="eth"),
            Want(kind="want", driver="eth"),
        ),
    )
)
# A reset forgets the want, so the rewant after it is the identity.
@example(
    run=Run(
        (),
        "pinned",
        (
            Want(kind="want", driver="timer"),
            Reset(kind="reset"),
            Rewant(kind="rewant", driver="timer"),
        ),
    )
)
# Found by this test: with blk given up on, FatFs reported a missing file
# from its cached window. Now the FAT server fails every request instead.
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
    disk = testbed.disks[KNOBS.disk] if KNOBS.disk is not None else testbed.disk
    profile = Profile(testbed.images[IMAGE], disk, blkdebug=plan, crashlog=True, reboot=True)
    decisions: list[tuple[Step, Outcome]] = []
    async with Machine(
        "system", profile, workdir, testbed.tools, default_failures(testbed.abi)
    ) as m:

        def observe(step: Step, seen: Outcome, before: Model) -> None:
            start = spans[-1].end if spans else 0
            spans.append(Span(step, seen, start, m.logged()))
            note(f"{step!r}: {render(seen)}")
            covered.add((abstract(before), step.kind))
            decisions.append((step, seen))
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
        if any(isinstance(step, Want | Rewant | Look | Absorb) for step in program):
            async with asyncio.timeout(120):
                await orchestrator_audits(m, testbed, decisions)
        target(len(covered), label="state-step pairs covered")
        async with asyncio.timeout(60):
            await probes.call(m, "chryso_test:alive(fuzz).", r"FUZZ_ALIVE\|42")
        # The log lives in the machine's scratch, which leaving the context removes.
        raw = await asyncio.to_thread(m.write_log.read_bytes)
    broken, seen = await asyncio.to_thread(
        crash_checks, raw, spans, testbed.disk, workdir, run.crash_seed
    )
    for name in seen:
        event(name)
    assert not broken, broken
