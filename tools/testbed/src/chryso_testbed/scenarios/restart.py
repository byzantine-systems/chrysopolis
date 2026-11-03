"""Restart group: drivers restarted on request and through injected faults.

Each recovery scenario keeps one boot and runs its steps in order, so
restart counts carry from step to step as they do on a real system.
"""

from __future__ import annotations

import asyncio
import re

from chryso_testbed import probes
from chryso_testbed.console import Mark
from chryso_testbed.machine import Machine
from chryso_testbed.scenario import Context, registry

GROUP = "restart"
IMAGE = "restart-image"


async def ready(ctx: Context, m: Machine) -> None:
    async with asyncio.timeout(400):
        banner = await m.console.wait("Eshell")
        await probes.load_test_modules(m, ctx.settings.test_modules, banner)


def no_debug_restart(m: Machine, child: int, since: Mark, why: str) -> None:
    if m.console.search(rf"ROOT\|debug-restart\|child={child}\b", since) is not None:
        raise m.console.fail(why)


@registry.scenario("serial-recovery", group=GROUP, timeout=900)
async def serial_recovery(ctx: Context) -> None:
    serial, crasher = ctx.abi.child("serial"), ctx.abi.crasher
    async with ctx.boot(IMAGE) as m:
        async with ctx.step("restart-smoke"), asyncio.timeout(300):
            await m.console.wait("Eshell")
            ns = [int(h.groups[0] or 0) for h in m.console.findall(r"CRASHER\|init\|n=(\d+)")]
            if not ns or max(ns) < 2:
                raise m.console.fail(f"crasher never restarted: {ns}")
            counts = [
                int(h.groups[0] or 0)
                for h in m.console.findall(rf"ROOT\|restart\|child={crasher}\|count=(\d+)")
            ]
            if not counts or max(counts) < 2:
                raise m.console.fail(f"crasher restart count never climbed: {counts}")
            probes.require_present(
                m, f"ROOT|giveup|child={crasher}|reason=budget-exhausted", "no give-up"
            )
        ctx.record("restart-smoke", budget_exhausted=True)

        await ready(ctx, m)
        async with ctx.step("serial-restart-smoke"), asyncio.timeout(300):
            await probes.call(m, "chryso_test:serial_before().", r"SERIAL_BEFORE\|2")
            mark = m.console.mark()
            await m.console.send("chryso_test:restart_pd('serial').")
            await m.console.wait(r"PD_RESTART\|request\|class=serial", since=mark)
            await m.console.wait(rf"ROOT\|debug-restart\|child={serial}\|count=1\b", since=mark)
            await probes.call(m, "chryso_test:serial_after().", r"SERIAL_AFTER\|42")
            await probes.call(m, "chryso_test:serial_rx().", r"SERIAL_RX\|3")
        ctx.record("serial-restart-smoke", restart_count=1)

        async with ctx.step("serial-fault-smoke"), asyncio.timeout(300):
            mark = m.console.mark()
            await probes.call(m, "chryso_test:serial_before().", r"SERIAL_BEFORE\|2")
            await m.console.send("chryso_test:fault_pd('serial').")
            await m.console.wait(r"PD_RESTART\|request\|class=serial\|mode=fault", since=mark)
            await m.console.wait(rf"ROOT\|restart\|child={serial}\|count=2\b", since=mark)
            probes.assert_fault_event(m, serial, 2, mark)
            await probes.call(m, "chryso_test:serial_after().", r"SERIAL_AFTER\|42")
            await probes.call(m, "chryso_test:serial_rx().", r"SERIAL_RX\|3")
            no_debug_restart(m, serial, mark, "serial fault used the requested-restart path")
        ctx.record("serial-fault-smoke", restart_count=2)


async def monotonic(m: Machine, tag: str) -> int:
    hit = await probes.call(
        m, f"chryso_clock:now_tagged('{tag}').", rf"CLOCK_{tag.upper()}\|(-?\d+)"
    )
    return int(hit.groups[0] or 0)


async def sleep_works(m: Machine, tag: str) -> None:
    hit = await probes.call(m, f"chryso_clock:sleep_check('{tag}').", rf"{tag.upper()}\|(\d+)")
    if int(hit.groups[0] or 0) < 500:
        raise m.console.fail(f"{tag}: timer:sleep(500) returned too early")


@registry.scenario("timer-recovery", group=GROUP, timeout=900)
async def timer_recovery(ctx: Context) -> None:
    timer = ctx.abi.child("timer")
    async with ctx.boot(IMAGE) as m:
        await ready(ctx, m)
        async with ctx.step("timer-restart-smoke"), asyncio.timeout(400):
            await sleep_works(m, "slept_before")
            before = await monotonic(m, "before")
            mark = m.console.mark()
            await m.console.send("chryso_test:restart_pd('timer').")
            await m.console.wait(r"PD_RESTART\|request\|class=timer", since=mark)
            await m.console.wait(rf"ROOT\|debug-restart\|child={timer}\|count=1\b", since=mark)
            after = await monotonic(m, "after")
            if after <= before:
                raise m.console.fail(f"clock did not advance: {before} -> {after}")
            await sleep_works(m, "slept_after")
        ctx.record("timer-restart-smoke", clock_advanced=True)

        async with ctx.step("timer-fault-smoke"), asyncio.timeout(400):
            mark = m.console.mark()
            await sleep_works(m, "slept_fault_before")
            before = await monotonic(m, "fault_before")
            await m.console.send("chryso_test:fault_pd('timer').")
            await m.console.wait(r"PD_RESTART\|request\|class=timer\|mode=fault", since=mark)
            await m.console.wait(rf"ROOT\|restart\|child={timer}\|count=2\b", since=mark)
            probes.assert_fault_event(m, timer, 2, mark)
            after = await monotonic(m, "fault_after")
            if after <= before:
                raise m.console.fail(f"clock did not advance: {before} -> {after}")
            await sleep_works(m, "slept_fault_after")
            no_debug_restart(m, timer, mark, "timer fault used the requested-restart path")
        ctx.record("timer-fault-smoke", clock_advanced=True)


@registry.scenario("blk-recovery", group=GROUP, timeout=1200)
async def blk_recovery(ctx: Context) -> None:
    blk = ctx.abi.child("blk")
    async with ctx.boot(IMAGE) as m:
        await ready(ctx, m)
        async with ctx.step("blk-restart-smoke idle"), asyncio.timeout(400):
            await probes.call(m, "chryso_fs:read_tagged('before', lists).", r"FS_BEFORE\|\d+")
            mark = m.console.mark()
            await m.console.send("chryso_test:restart_pd('blk').")
            await m.console.wait(r"PD_RESTART\|request\|class=blk", since=mark)
            await m.console.wait(rf"ROOT\|debug-restart\|child={blk}\|count=1\b", since=mark)
            await m.console.wait("driver restarted, reconciling", since=mark)
            await m.console.wait(r"driver restarted: failed \d+ client request", since=mark)
            await probes.call(
                m, "chryso_fs:read_tagged('after_idle', lists).", r"FS_AFTER_IDLE\|\d+"
            )
        ctx.record("blk-restart-smoke-idle", restart_count=1)

        async with ctx.step("blk-restart-smoke in flight"), asyncio.timeout(400):
            mark = m.console.mark()
            await m.console.send("chryso_fs:read_inflight(dict).")
            await m.console.send("chryso_test:restart_pd('blk').")
            await m.console.wait(rf"ROOT\|debug-restart\|child={blk}\|count=2\b", since=mark)
            await m.console.wait(r"FS_INFLIGHT\|(ok|error|EXIT)", since=mark)
            await probes.call(
                m, "chryso_fs:read_tagged('after_inflight', lists).", r"FS_AFTER_INFLIGHT\|\d+"
            )
        ctx.record("blk-restart-smoke-in-flight", restart_count=2)

        async with ctx.step("blk-fault-smoke idle"), asyncio.timeout(400):
            mark = m.console.mark()
            await probes.call(
                m, "chryso_fs:read_tagged('fault_before', lists).", r"FS_FAULT_BEFORE\|\d+"
            )
            await m.console.send("chryso_test:fault_pd('blk').")
            await m.console.wait(r"PD_RESTART\|request\|class=blk\|mode=fault", since=mark)
            await m.console.wait(rf"ROOT\|restart\|child={blk}\|count=3\b", since=mark)
            probes.assert_fault_event(m, blk, 3, mark)
            await m.console.wait("driver restarted, reconciling", since=mark)
            await probes.call(
                m,
                "chryso_fs:read_tagged('fault_after_idle', lists).",
                r"FS_FAULT_AFTER_IDLE\|\d+",
            )
        ctx.record("blk-fault-smoke-idle", restart_count=3)

        async with ctx.step("blk-fault-smoke in flight"), asyncio.timeout(400):
            mark = m.console.mark()
            await m.console.send("chryso_fs:read_during_fault().")
            await m.console.wait(rf"ROOT\|restart\|child={blk}\|count=4\b", since=mark)
            probes.assert_fault_event(m, blk, 4, mark)
            await m.console.wait("driver restarted, reconciling", since=mark)
            outcome = await m.console.wait(r"FS_INFLIGHT\|(ok|error|EXIT)", since=mark)
            fault = m.console.search(rf"ROOT\|fault\|child={blk}(?:\||\r?$)", mark)
            if fault is None or fault.start > outcome.start:
                raise m.console.fail("in-flight block client answered before the fault")
            await probes.call(
                m,
                "chryso_fs:read_tagged('fault_after_inflight', lists).",
                r"FS_FAULT_AFTER_INFLIGHT\|\d+",
            )
            no_debug_restart(m, blk, mark, "block fault used the requested-restart path")
        ctx.record("blk-fault-smoke-in-flight", restart_count=4)


@registry.scenario("net-recovery", group=GROUP, timeout=1200)
async def net_recovery(ctx: Context) -> None:
    eth = ctx.abi.child("eth")
    reclaimed = r"ETH\|restart\|reclaimed\|rx=\d+\|tx=\d+"

    async def restart(m: Machine, count: int) -> None:
        mark = m.console.mark()
        await m.console.send("chryso_test:restart_pd('eth').")
        await m.console.wait(r"PD_RESTART\|request\|class=eth", since=mark)
        await m.console.wait(rf"ROOT\|debug-restart\|child={eth}\|count={count}\b", since=mark)
        await m.console.wait(reclaimed, since=mark)

    async def fault(m: Machine, count: int) -> None:
        mark = m.console.mark()
        await m.console.send("chryso_test:fault_pd('eth').")
        await m.console.wait(r"PD_RESTART\|request\|class=eth\|mode=fault", since=mark)
        await m.console.wait(rf"ROOT\|restart\|child={eth}\|count={count}\b", since=mark)
        probes.assert_fault_event(m, eth, count, mark)
        await m.console.wait(reclaimed, since=mark)
        no_debug_restart(m, eth, mark, "network fault used the requested-restart path")

    async with ctx.boot(IMAGE) as m:
        async with asyncio.timeout(400):
            await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
        await ready(ctx, m)
        async with ctx.step("net-restart-smoke baseline"), asyncio.timeout(300):
            await probes.ping(m, "before", ctx.ports.take())
        ctx.record("net-restart-smoke-baseline", pinged=True)
        async with ctx.step("net-restart-smoke twice"), asyncio.timeout(600):
            await restart(m, 1)
            await probes.ping(m, "after1", ctx.ports.take())
            await restart(m, 2)
            await probes.ping(m, "after2", ctx.ports.take())
        ctx.record("net-restart-smoke", restart_count=2)
        async with ctx.step("net-fault-smoke twice"), asyncio.timeout(600):
            await probes.ping(m, "fault_before", ctx.ports.take())
            await fault(m, 3)
            await probes.ping(m, "fault_after1", ctx.ports.take())
            await fault(m, 4)
            await probes.ping(m, "fault_after2", ctx.ports.take())
        ctx.record("net-fault-smoke", restart_count=4)


@registry.scenario("blk-giveup-smoke", group=GROUP, timeout=1500)
async def blk_giveup(ctx: Context) -> None:
    """Spend the block driver's budget by faults, then the system must degrade."""
    blk, budget = ctx.abi.child("blk"), ctx.abi.driver_budget
    fault_line = rf"ROOT\|fault\|child={blk}(?:\||\r?$)"

    async def read(m: Machine, tag: str, module: str) -> str:
        hit = await probes.call(
            m,
            f"chryso_fs:read_status('{tag}', {module}).",
            rf"BLKGONE_{tag.upper()}\|(ok|error|EXIT)",
        )
        return hit.groups[0] or ""

    async with ctx.boot(IMAGE) as m:
        # Load the probes first: the disk is gone by the end.
        await ready(ctx, m)
        async with ctx.step("spend the budget"), asyncio.timeout(900):
            if await read(m, "before", "lists") != "ok":
                raise m.console.fail("baseline fs read failed before any restart")
            for n in range(1, budget + 1):
                mark = m.console.mark()
                await m.console.send("chryso_test:fault_pd('blk').")
                await m.console.wait(rf"ROOT\|restart\|child={blk}\|count={n}\b", since=mark)
                probes.assert_fault_sequence(m, blk, n)
            if await read(m, "atbudget", "dict") != "ok":
                raise m.console.fail("fs reads stopped working before the budget was spent")

        async with ctx.step("give up"), asyncio.timeout(400):
            await m.console.send("chryso_test:fault_pd('blk').")
            await m.console.wait_count(fault_line, budget + 1)
            await m.console.wait(rf"ROOT\|giveup\|child={blk}\|reason=budget-exhausted")
            injections = m.console.findall(rf"ROOT\|fault-inject\|child={blk}\b")
            faults = m.console.findall(fault_line)
            giveup = m.console.search(rf"ROOT\|giveup\|child={blk}\|reason=budget-exhausted")
            if len(injections) != budget + 1 or len(faults) != budget + 1 or giveup is None:
                raise m.console.fail(f"{len(injections)} injections, {len(faults)} faults")
            if not injections[-1].start < faults[-1].start < giveup.start:
                raise m.console.fail("final block fault and give-up were out of order")
            await m.console.wait("driver stopped, reconciling outstanding requests")
            await m.console.wait(r"driver stopped: \d+ client\(s\) marked not ready")
            for tag, module in (("after", "queue"), ("again", "sets")):
                if await read(m, tag, module) not in ("error", "EXIT"):
                    raise m.console.fail(f"fs read {tag} succeeded after the driver stopped")

        async with ctx.step("gone is terminal"), asyncio.timeout(400):
            await probes.call(
                m, "chryso_test:fault_pd('blk').", rf"ROOT\|gone\|child={blk}\|ignored=fault-inject"
            )
            await probes.call(
                m,
                "chryso_test:restart_pd('blk').",
                rf"ROOT\|gone\|child={blk}\|ignored=debug-restart",
            )
            text = m.console.text().decode("utf-8", "replace")
            checks = {
                "another fault": len(re.findall(fault_line, text, re.M)) == budget + 1,
                "another injection": len(m.console.findall(rf"ROOT\|fault-inject\|child={blk}\b"))
                == budget + 1,
                "a second give-up": text.count(f"ROOT|giveup|child={blk}|") == 1,
                "a second stop notice": text.count(
                    "driver stopped, reconciling outstanding requests"
                )
                == 1,
                "a debug restart": f"ROOT|debug-restart|child={blk}" not in text,
            }
            if failed := [name for name, ok in checks.items() if not ok]:
                raise m.console.fail(f"requests for the stopped driver caused {failed}")
            if await read(m, "gone", "orddict") not in ("error", "EXIT"):
                raise m.console.fail("fs read reached a stopped driver")
            await probes.call(m, "chryso_test:alive('blkgone').", r"BLKGONE_ALIVE\|42")


@registry.scenario("budget-decay-smoke", group=GROUP, timeout=900, exclusive=True)
async def budget_decay(ctx: Context) -> None:
    """A one-token window refills over time, but the lifetime ceiling holds."""
    eth = ctx.abi.child("eth")
    async with ctx.boot("budget-decay-image") as m:
        async with asyncio.timeout(400):
            await m.console.wait(r"ROOT\|clock\|freq=")
        await ready(ctx, m)
        async with ctx.step("decay"), asyncio.timeout(400):
            for count in range(1, 4):
                if count != 1:
                    # Real time must pass for the window to refill: the test
                    # Root leaks one charge every two seconds.
                    await asyncio.sleep(3)
                await probes.call(
                    m,
                    "chryso_test:fault_pd('eth').",
                    rf"ROOT\|restart\|child={eth}\|count={count}\|window=1\b",
                )
            await asyncio.sleep(3)
            await probes.call(
                m,
                "chryso_test:fault_pd('eth').",
                rf"ROOT\|giveup\|child={eth}\|reason=lifetime-exhausted",
            )
        text = m.console.text().decode("utf-8", "replace")
        if text.count(f"ROOT|giveup|child={eth}|") != 1:
            raise m.console.fail("Root gave up more than once")
        probes.require_absent(
            m, rf"ROOT\|giveup\|child={eth}\|reason=window-exhausted", "window exhausted instead"
        )
        probes.require_absent(m, r"ROOT\|clock\|unavailable", "Root's clock was unavailable")
