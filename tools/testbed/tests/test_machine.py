"""Machine lifecycle against the fake QEMU: console, QMP events, cleanup."""

from __future__ import annotations

import asyncio
import os
from collections.abc import Callable
from pathlib import Path

import pytest

from chryso_testbed.console import ScenarioFailure
from chryso_testbed.machine import Machine, Profile, Tools


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
