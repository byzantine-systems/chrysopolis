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
from hypothesis import HealthCheck, given, note, settings
from hypothesis_jsonschema import from_schema

from chryso_testbed import probes
from chryso_testbed.__main__ import load
from chryso_testbed.blkdebug import RULES, fails_writes
from chryso_testbed.machine import Machine, Profile
from chryso_testbed.scenario import Settings, default_failures

TERM = r"ok|\{error,\w+\}"
WRITE = rf"FS_WRITE_FUZZ\|(\{{error,\w+\}}|\{{(?:{TERM}),(?:{TERM}),(?:{TERM})\}})"
EXAMPLES = itertools.count(1)


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
@given(rules=from_schema(RULES))
async def test_a_write_reports_its_fault_and_reads_survive(
    testbed: Settings, rules: list[dict[str, object]]
) -> None:
    """Under any plan the system boots, a write reports an error exactly when
    the plan fails writes, and reads keep working. No PD may fault."""
    workdir = testbed.out / f"example-{next(EXAMPLES)}"
    note(f"rules={rules} console={workdir}")
    profile = Profile(testbed.images["test-image"], testbed.disk, blkdebug=tuple(rules))
    failures = default_failures(testbed.abi)
    async with Machine("fuzz", profile, workdir, testbed.tools, failures) as m:
        async with asyncio.timeout(300):
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, testbed.test_modules, banner)
            hit = await probes.call(m, "chryso_fs:write_status('fuzz').", WRITE)
            await probes.call(m, "chryso_fs:read_tagged('fuzz', lists).", r"FS_FUZZ\|\d+")
    # {error, R} from open, or {Write, Sync, Close}. Sync is enosys on this path.
    outcome = re.findall(TERM, hit.groups[0] or "")
    reported = len(outcome) == 1 or outcome[0] != "ok" or outcome[2] != "ok"
    assert reported == fails_writes(rules), f"write reported {hit.groups[0]}"
