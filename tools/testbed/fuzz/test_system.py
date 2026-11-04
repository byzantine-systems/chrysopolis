"""Fault programs against the restart image, one boot per example.

Run with `nix run .#testbed-fuzz -- -k system`. SYSTEM_FUZZ_EXAMPLES
sets the example count (default 5). Each example boots with a blkdebug plan,
then runs a program drawn from one of `steer.SCENARIOS` by simulating the
model, checking every outcome against `faults.Model` as it goes.

`--hypothesis-show-statistics` prints the scenarios drawn, whether each arm
or corruption found its target, and the fault-reaction table: for every
read or write a disk fault could touch, the fault and what the guest saw.
`target()` steers generation toward runs covering more distinct pairs of
abstract state and step kind.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import os
from dataclasses import dataclass
from pathlib import Path

import pytest
from hypothesis import HealthCheck, event, example, given, note, settings, target
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed import inject, probes
from chryso_testbed.__main__ import load
from chryso_testbed.abi import Abi
from chryso_testbed.blkdebug import PLAN, Expected, Plan, shaped
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
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
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.outcomes import Outcome, render
from chryso_testbed.scenario import Settings, default_failures
from chryso_testbed.steer import SCENARIOS, programs

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


RUNS = st.tuples(PLANS, st.sampled_from(sorted(SCENARIOS))).flatmap(
    lambda chosen: _program_for(*chosen)
)


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

    def observe(step: Step, seen: Outcome, before: Model) -> None:
        covered.add((abstract(before), step.kind))
        if (row := reaction(step, seen, before)) is not None:
            event(row)
        if isinstance(step, Arm | Corrupt):
            event(f"aim {step.kind} -> {render(seen)}")

    workdir = testbed.out / f"example-{next(EXAMPLES)}"
    note(f"plan={PLAN.dump_python(plan)} console={workdir}")
    note(f"program={PROGRAM.dump_python(program)}")
    profile = Profile(testbed.images[IMAGE], testbed.disk, blkdebug=plan)
    async with Machine(
        "system", profile, workdir, testbed.tools, default_failures(testbed.abi)
    ) as m:
        async with asyncio.timeout(400):
            await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, testbed.test_modules, banner)
        model = await inject.check(m, program, model, abi=testbed.abi, log=note, observe=observe)
        for driver in model.root.gone:
            event(f"gave up {driver}")
        target(len(covered), label="state-step pairs covered")
        async with asyncio.timeout(60):
            await probes.call(m, "chryso_test:alive(fuzz).", r"FUZZ_ALIVE\|42")
