"""Disk fault plans against the real image, one boot per example.

Run with `nix run .#testbed-fuzz`, optionally `-- --hypothesis-seed=N`.
BLK_FUZZ_EXAMPLES sets the example count (default 10). Failures replay from
the example database in .hypothesis/ and print a reproduce blob.
"""

from __future__ import annotations

import asyncio
import itertools
import os
import re
from pathlib import Path

import pytest
from hypothesis import HealthCheck, event, given, note, settings
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed import probes
from chryso_testbed.__main__ import load
from chryso_testbed.blkdebug import PLAN, SCHEMA, Expected, Plan, shaped
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.scenario import Settings, default_failures

TERM = r"ok|\{error,\w+\}"
WRITE = rf"FS_WRITE_FUZZ\|(\{{error,\w+\}}|\{{(?:{TERM}),(?:{TERM}),(?:{TERM})\}})"
READ = r"FS_READ_{tag}\|(\{{ok,\d+,\d+\}}|\{{error,\w+\}})"
BACK = r"FS_BACK_FUZZ\|(same|differs|\{error,\w+\})"
EXAMPLES = itertools.count(1)

PLANS = st.builds(
    shaped,
    from_schema(SCHEMA).map(PLAN.validate_python),
    st.sampled_from(["clean", "transient", "any"]),
)


@pytest.fixture(scope="session")
def testbed(tmp_path_factory: pytest.TempPathFactory) -> Settings:
    manifest = Path(os.environ["CHRYSO_TESTBED_MANIFEST"])
    return load(manifest, tmp_path_factory.mktemp("fuzz"), 1)


@pytest.mark.asyncio
@settings(
    max_examples=int(os.environ.get("BLK_FUZZ_EXAMPLES", "10")),
    deadline=None,
    print_blob=True,
    suppress_health_check=[HealthCheck.too_slow],
)
@given(plan=PLANS)
async def test_disk_faults_are_reported_and_contained(testbed: Settings, plan: Plan) -> None:
    """Under any plan the system boots and no PD faults, and:

    - a write reports eio exactly when the plan fails writes;
    - a later read reports what Expected allows, with eio its only error;
    - a read that works returns the bytes it returned before the fault;
    - a write reported as ok reads back exactly as written."""
    expected = Expected.of(plan)
    event(expected.label)
    workdir = testbed.out / f"example-{next(EXAMPLES)}"
    note(f"plan={PLAN.dump_python(plan)} expected={expected} console={workdir}")
    profile = Profile(testbed.images["test-image"], testbed.disk, blkdebug=plan)
    async with Machine("fuzz", profile, workdir, testbed.tools, default_failures(testbed.abi)) as m:
        async with asyncio.timeout(300):
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, testbed.test_modules, banner)
            # Boot does not write, so this read happens before any fault.
            before = await probes.call(
                m, "chryso_fs:read_outcome('before', lists).", READ.format(tag="BEFORE")
            )
            write = await probes.call(m, "chryso_fs:write_status('fuzz').", WRITE)
            after = await probes.call(
                m, "chryso_fs:read_outcome('after', lists).", READ.format(tag="AFTER")
            )
            back = await probes.call(m, "chryso_fs:read_back('fuzz').", BACK)
    reported = write.groups[0] or ""
    # {error, R} from open, or {Write, Sync, Close}. Sync is enosys on this path.
    steps = re.findall(TERM, reported)
    write_errors = {e for e in (steps[0], steps[-1]) if e != "ok"}
    assert write_errors == ({"{error,eio}"} if expected.write_fails else set()), reported

    read = after.groups[0] or ""
    assert ("ok" if read.startswith("{ok,") else "error") in expected.reads, read
    assert read in {before.groups[0], "{error,eio}"}, f"before {before.groups[0]}, after {read}"
    if not write_errors:
        assert back.groups[0] == "same", back.groups[0]
