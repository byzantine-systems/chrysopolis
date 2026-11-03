"""Isolated scenarios: one behaviour on a fresh boot, run on demand.

They are in no check. Each reuses its group's steps, so a regression the
groups catch can be narrowed to a single boot with
`nix run .#testbed -- run -k <name>`.
"""

from __future__ import annotations

import asyncio

from chryso_testbed import probes
from chryso_testbed.scenario import Context, registry
from chryso_testbed.scenarios import boot, restart

GROUP = "isolated"


@registry.scenario("boot-smoke", group=GROUP, timeout=400)
async def boot_smoke(ctx: Context) -> None:
    async with ctx.boot("test-image") as m, asyncio.timeout(300):
        await m.console.wait("Eshell")
        boot.milestones(m)


@registry.scenario("shell-smoke", group=GROUP, timeout=600)
async def shell_smoke(ctx: Context) -> None:
    async with ctx.boot("test-image") as m, asyncio.timeout(500):
        await boot.shell(ctx, m, await m.console.wait("Eshell"))


@registry.scenario("tcp-smoke", group=GROUP, timeout=900)
async def tcp_smoke(ctx: Context) -> None:
    forward = ctx.ports.take()
    async with ctx.boot("test-image", hostfwd=((forward, 5555),)) as m:
        await restart.net_ready(ctx, m)
        async with asyncio.timeout(480):
            await boot.tcp_host_to_guest(m, forward)
            await boot.tcp_guest_to_host(ctx, m)


@registry.scenario("restart-smoke", group=GROUP, timeout=400)
async def restart_smoke(ctx: Context) -> None:
    async with ctx.boot(restart.IMAGE) as m, asyncio.timeout(300):
        await restart.crasher_ladder(m, ctx.abi.crasher)


@registry.scenario("serial-restart-smoke", group=GROUP, timeout=900)
async def serial_restart_smoke(ctx: Context) -> None:
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(300):
            await restart.serial_restart(m, ctx.abi.child("serial"), 1)


@registry.scenario("serial-fault-smoke", group=GROUP, timeout=900)
async def serial_fault_smoke(ctx: Context) -> None:
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(300):
            await restart.serial_fault(m, ctx.abi.child("serial"), 1)


@registry.scenario("timer-restart-smoke", group=GROUP, timeout=900)
async def timer_restart_smoke(ctx: Context) -> None:
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(400):
            await restart.timer_restart(m, ctx.abi.child("timer"), 1)


@registry.scenario("timer-fault-smoke", group=GROUP, timeout=900)
async def timer_fault_smoke(ctx: Context) -> None:
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(400):
            await restart.timer_fault(m, ctx.abi.child("timer"), 1)


@registry.scenario("blk-restart-smoke", group=GROUP, timeout=1200)
async def blk_restart_smoke(ctx: Context) -> None:
    blk = ctx.abi.child("blk")
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(800):
            await restart.blk_restart_idle(m, blk, 1)
            await restart.blk_restart_in_flight(m, blk, 2)


@registry.scenario("blk-fault-smoke", group=GROUP, timeout=1200)
async def blk_fault_smoke(ctx: Context) -> None:
    blk = ctx.abi.child("blk")
    async with ctx.boot(restart.IMAGE) as m:
        await restart.ready(ctx, m)
        async with asyncio.timeout(800):
            await restart.blk_fault_idle(m, blk, 1)
            await restart.blk_fault_in_flight(m, blk, 2)


@registry.scenario("net-restart-smoke", group=GROUP, timeout=1200)
async def net_restart_smoke(ctx: Context) -> None:
    eth = ctx.abi.child("eth")
    async with ctx.boot(restart.IMAGE) as m:
        await restart.net_ready(ctx, m)
        async with asyncio.timeout(900):
            await probes.ping(m, "before", ctx.ports.take())
            await restart.eth_restart(m, eth, 1)
            await probes.ping(m, "after1", ctx.ports.take())
            await restart.eth_restart(m, eth, 2)
            await probes.ping(m, "after2", ctx.ports.take())


@registry.scenario("net-fault-smoke", group=GROUP, timeout=1200)
async def net_fault_smoke(ctx: Context) -> None:
    eth = ctx.abi.child("eth")
    async with ctx.boot(restart.IMAGE) as m:
        await restart.net_ready(ctx, m)
        async with asyncio.timeout(900):
            await probes.ping(m, "before", ctx.ports.take())
            await restart.eth_fault(m, eth, 1)
            await probes.ping(m, "after1", ctx.ports.take())
            await restart.eth_fault(m, eth, 2)
            await probes.ping(m, "after2", ctx.ports.take())
