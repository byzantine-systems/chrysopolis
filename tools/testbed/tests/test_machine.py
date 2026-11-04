"""Machine lifecycle against the fake QEMU: console, QMP events, cleanup."""

from __future__ import annotations

import asyncio
import json
import os
from collections.abc import Callable, Coroutine
from pathlib import Path

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st
from hypothesis_jsonschema import from_schema

from chryso_testbed.blkdebug import RULES
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


@given(rules=from_schema(RULES), overlay=paths, disk=paths)
@settings(max_examples=200, deadline=None)
def test_blockdev_puts_the_rules_under_both_layers(
    rules: list[dict[str, object]], overlay: Path, disk: Path
) -> None:
    """The overlay holds the guest's writes over a read-only image, and the
    rules, verbatim, wrap both or neither."""
    flag, value = drive(Profile(Path("/img"), disk, blkdebug=tuple(rules)), overlay)
    node = json.loads(value)
    assert (flag, node["driver"], node["node-name"]) == ("-blockdev", "qcow2", "hd")
    backing = node["backing"]
    assert backing["driver"] == "raw"
    for layer, path, readonly in ((node["file"], overlay, False), (backing["file"], disk, True)):
        image = layer["image"] if rules else layer
        assert layer.get("inject-error", []) == rules
        assert (layer["driver"] == "blkdebug") == bool(rules)
        assert image == {"driver": "file", "filename": str(path)} | (
            {"read-only": True} if readonly else {}
        )
