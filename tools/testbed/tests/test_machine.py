"""Machine lifecycle against the fake QEMU: console, QMP events, cleanup."""

from __future__ import annotations

import asyncio
import itertools
import json
import os
from collections.abc import Callable, Coroutine
from pathlib import Path

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import PLAN, SCHEMA, Plan
from chryso_testbed.console import ScenarioFailure
from chryso_testbed.machine import Machine, Profile, Tools, drive


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_console_round_trip_and_cleanup(
    tools: Tools, profile: Profile, tmp_path: Path, script: Callable[..., None]
) -> None:
    script(["out", "Eshell V16\r\n"], ["expect", "ping"], ["out", "PONG|1\r\n"], ["hang"])

    async def scenario() -> int:
        async with Machine("m", profile, tmp_path / "m", tools, ()) as machine:
            await machine.console.wait("Eshell")
            await machine.console.send("ping")
            await machine.console.wait(r"PONG\|1")
            assert machine._proc is not None
            return machine._proc.pid

    pid = asyncio.run(asyncio.wait_for(scenario(), 15))
    assert not alive(pid)
    assert b"PONG|1" in (tmp_path / "m" / "console.log").read_bytes()


def test_qmp_shutdown_fails_waits_at_once(
    tools: Tools, profile: Profile, tmp_path: Path, script: Callable[..., None]
) -> None:
    script(["out", "booting\r\n"], ["sleep", 0.2], ["event", "SHUTDOWN"], ["hang"])

    async def scenario() -> None:
        async with Machine("m", profile, tmp_path / "m", tools, ()) as machine:
            await machine.console.wait("never")

    with pytest.raises(ScenarioFailure, match="QEMU SHUTDOWN"):
        asyncio.run(asyncio.wait_for(scenario(), 15))


def test_cancellation_kills_the_guest(
    tools: Tools, profile: Profile, tmp_path: Path, script: Callable[..., None]
) -> None:
    script(["out", "up\r\n"], ["hang"])
    pids: list[int] = []

    async def scenario() -> None:
        ready = asyncio.Event()

        async def hold() -> None:
            async with Machine("m", profile, tmp_path / "m", tools, ()) as machine:
                await machine.console.wait("up")
                assert machine._proc is not None
                pids.append(machine._proc.pid)
                ready.set()
                await asyncio.Event().wait()

        task = asyncio.create_task(hold())
        await ready.wait()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(asyncio.wait_for(scenario(), 15))
    assert pids
    assert not alive(pids[0])


def test_a_dead_qemu_fails_at_once(profile: Profile, tmp_path: Path) -> None:
    """QEMU exiting before QMP connects is reported with its status, not a timeout."""
    dead = tmp_path / "dead-qemu"
    dead.write_text("#!/bin/sh\necho 'qemu: bad option' >&2\nexit 3\n")
    dead.chmod(0o755)
    qemu_img = tmp_path / "img"
    qemu_img.write_text('#!/bin/sh\nfor last; do :; done\n: > "$last"\n')
    qemu_img.chmod(0o755)
    tools = Tools(str(dead), str(qemu_img))

    async def scenario() -> None:
        async with Machine("m", profile, tmp_path / "m", tools, ()):
            pass

    started = asyncio.run(asyncio.wait_for(_timed(scenario()), 10))
    assert started < 5


async def _timed(coro: Coroutine[object, object, None]) -> float:
    loop = asyncio.get_running_loop()
    began = loop.time()
    with pytest.raises(ScenarioFailure, match=r"(?s)status 3 before QMP.*bad option"):
        await coro
    return loop.time() - began


paths = st.builds(
    Path, st.from_regex(r"/[a-z0-9_.-]{1,20}(/[a-z0-9_.-]{1,20}){0,3}", fullmatch=True)
)


@given(
    plan=from_schema(SCHEMA).map(PLAN.validate_python),
    overlay=paths,
    disk=paths,
    crashlog=st.booleans(),
)
@settings(max_examples=200, deadline=None)
def test_blockdev_puts_the_plan_above_qcow2_and_the_log(
    plan: Plan, overlay: Path, disk: Path, crashlog: bool
) -> None:
    """Bottom up: the overlay over a read-only image, the write log, the plan
    in QEMU's form, then `top`. Each node reads the one below it, so the plan
    faults guest I/O only and a write it fails never reaches the log."""
    profile = Profile(Path("/img"), disk, blkdebug=plan, crashlog=crashlog)
    args = drive(profile, overlay, Path("/log") if crashlog else None)
    assert args[::2] == ["-blockdev"] * (len(args) // 2)
    nodes = [json.loads(value) for value in args[1::2]]
    hd, *middle, top = nodes
    assert hd == {
        "driver": "qcow2", "node-name": "hd",
        "file": {"driver": "file", "filename": str(overlay)},
        "backing": {"driver": "raw",
                    "file": {"driver": "file", "filename": str(disk), "read-only": True}},
    }  # fmt: skip
    assert [n["driver"] for n in middle] == ["blklogwrites"] * crashlog + ["blkdebug"] * bool(plan)
    for below, node in itertools.pairwise(nodes):
        assert node.get("file", node.get("image")) == below["node-name"]
    if plan:
        assert PLAN.validate_python(middle[-1]["inject-error"]) == plan
    assert top == {"driver": "raw", "node-name": "top", "file": profile.base}
    assert profile.base == nodes[-2]["node-name"]
