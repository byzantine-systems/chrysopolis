"""Fault programs against the restart image, one boot per example.

Run with `nix run .#testbed-fuzz -- -k system`. SYSTEM_FUZZ_EXAMPLES
sets the example count (default 5). Each example boots with a blkdebug plan,
then runs a program of workload and fault steps, checking every outcome
against `faults.Model` as it goes.
"""

from __future__ import annotations

import asyncio
import itertools
import os
from pathlib import Path

import pytest
from hypothesis import HealthCheck, event, example, given, note, settings
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed import inject, probes
from chryso_testbed.__main__ import load
from chryso_testbed.blkdebug import PLAN, Expected, Plan, shaped
from chryso_testbed.blkdebug import SCHEMA as PLAN_SCHEMA
from chryso_testbed.faults import PROGRAM, STEP, Model, Ping, Program, Unplug, trim
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.scenario import Settings, default_failures

IMAGE = "restart-image"
EXAMPLES = itertools.count(1)

PLANS = st.builds(
    shaped,
    from_schema(PLAN_SCHEMA).map(PLAN.validate_python),
    st.sampled_from(["clean", "single", "transient", "any"]),
)
# Steps are drawn one by one: drawing whole programs from the schema makes most
# of them empty, and each example costs a boot.
PROGRAMS = st.lists(
    from_schema(STEP.json_schema()).map(STEP.validate_python), min_size=4, max_size=12
).map(tuple)


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
@given(plan=PLANS, program=PROGRAMS)
# Found by this test: a link change left virtio-net's config interrupt unacknowledged.
@example(plan=(), program=(Ping(kind="ping"), Unplug(kind="unplug", ms=3000), Ping(kind="ping")))
async def test_the_system_does_what_its_model_allows(
    testbed: Settings, plan: Plan, program: Program
) -> None:
    """Under any plan and program the system boots, no PD faults unasked, and
    every step's outcome is one the model allows from the state before it."""
    program = trim(program, testbed.abi.driver_budget)
    model = Model(testbed.abi.driver_budget, Expected.of(plan))
    event(f"steps={len(program)}")
    for kind in sorted({step.kind for step in program}):
        event(f"has {kind}")
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
        model = await inject.check(m, program, model, abi=testbed.abi, log=note)
        for driver in model.gone:
            event(f"gave up {driver}")
        async with asyncio.timeout(60):
            await probes.call(m, "chryso_test:alive(fuzz).", r"FUZZ_ALIVE\|42")
