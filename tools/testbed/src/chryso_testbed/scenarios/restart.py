"""Restart group: drivers restarted on request and through injected faults.

Each recovery scenario keeps one boot and runs its steps in order, so
restart counts carry from step to step as they do on a real system. The
steps take the restart count they expect, so the isolated scenarios run the
same steps on a fresh boot.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import Awaitable, Callable
from typing import Final

from chryso_testbed import inject, probes
from chryso_testbed.blkdebug import EIO, Delay, WriteError
from chryso_testbed.console import Mark
from chryso_testbed.faults import Model, Pause, Ping, Unplug
from chryso_testbed.machine import Machine
from chryso_testbed.scenario import Context, registry

GROUP = "restart"
IMAGE = "restart-image"
RECLAIMED = r"ETH\|restart\|reclaimed\|rx=\d+\|tx=\d+"


async def ready(ctx: Context, m: Machine) -> None:
    async with asyncio.timeout(400):
        banner = await m.console.wait("Eshell")
        await probes.load_test_modules(m, ctx.settings.test_modules, banner)


def no_debug_restart(m: Machine, child: int, since: Mark, why: str) -> None:
    if m.console.search(rf"ROOT\|debug-restart\|child={child}\b", since) is not None:
        raise m.console.fail(why)


async def request(m: Machine, verb: str, driver: str) -> Mark:
    """Ask Root to restart or fault a driver. The mark precedes the request."""
    mark = m.console.mark()
    await m.console.send(f"chryso_test:{verb}_pd('{driver}').")
    mode = r"\|mode=fault" if verb == "fault" else ""
    await m.console.wait(rf"PD_RESTART\|request\|class={driver}{mode}", since=mark)
    return mark


async def faulted(m: Machine, child: int, count: int, mark: Mark) -> None:
    """A genuine fault reached Root and became restart `count`."""
    await m.console.wait(rf"ROOT\|restart\|child={child}\|count={count}\b", since=mark)
    probes.assert_fault_event(m, child, count, mark)


# --- crasher ---------------------------------------------------------------------


async def crasher_ladder(m: Machine, crasher: int) -> None:
    """At boot Root restarts the faulting crasher until its budget runs out."""
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
    probes.require_present(m, f"ROOT|giveup|child={crasher}|reason=budget-exhausted", "no give-up")


# --- serial ----------------------------------------------------------------------


async def serial_works(m: Machine) -> None:
    """Output after the restart, and a second round trip so input works too."""
    await probes.call(m, "chryso_test:serial_after().", r"SERIAL_AFTER\|42")
    await probes.call(m, "chryso_test:serial_rx().", r"SERIAL_RX\|3")


async def serial_restart(m: Machine, serial: int, count: int) -> None:
    await probes.call(m, "chryso_test:serial_before().", r"SERIAL_BEFORE\|2")
    mark = await request(m, "restart", "serial")
    await m.console.wait(rf"ROOT\|debug-restart\|child={serial}\|count={count}\b", since=mark)
    await serial_works(m)


async def serial_fault(m: Machine, serial: int, count: int) -> None:
    await probes.call(m, "chryso_test:serial_before().", r"SERIAL_BEFORE\|2")
    mark = await request(m, "fault", "serial")
    await faulted(m, serial, count, mark)
    await serial_works(m)
    no_debug_restart(m, serial, mark, "serial fault used the requested-restart path")


# --- timer -----------------------------------------------------------------------


async def monotonic(m: Machine, tag: str) -> int:
    hit = await probes.call(
        m, f"chryso_clock:now_tagged('{tag}').", rf"CLOCK_{tag.upper()}\|(-?\d+)"
    )
    return int(hit.groups[0] or 0)


async def sleep_works(m: Machine, tag: str) -> None:
    hit = await probes.call(m, f"chryso_clock:sleep_check('{tag}').", rf"{tag.upper()}\|(\d+)")
    if int(hit.groups[0] or 0) < 500:
        raise m.console.fail(f"{tag}: timer:sleep(500) returned too early")


async def timer_survives(m: Machine, prefix: str, act: Callable[[], Awaitable[None]]) -> None:
    """Timeouts work, `act` restarts the driver, the clock advanced, timeouts still fire."""
    await sleep_works(m, f"slept_{prefix}before")
    before = await monotonic(m, f"{prefix}before")
    await act()
    after = await monotonic(m, f"{prefix}after")
    if after <= before:
        raise m.console.fail(f"clock did not advance: {before} -> {after}")
    await sleep_works(m, f"slept_{prefix}after")


async def timer_restart(m: Machine, timer: int, count: int) -> None:
    async def act() -> None:
        mark = await request(m, "restart", "timer")
        await m.console.wait(rf"ROOT\|debug-restart\|child={timer}\|count={count}\b", since=mark)

    await timer_survives(m, "", act)


async def timer_fault(m: Machine, timer: int, count: int) -> None:
    mark = m.console.mark()

    async def act() -> None:
        await faulted(m, timer, count, await request(m, "fault", "timer"))

    await timer_survives(m, "fault_", act)
    no_debug_restart(m, timer, mark, "timer fault used the requested-restart path")


# --- blk -------------------------------------------------------------------------


async def read(m: Machine, tag: str, module: str = "lists") -> None:
    """A genuine fs round trip: a module not yet loaded, its size printed."""
    await probes.call(m, f"chryso_fs:read_tagged('{tag}', {module}).", rf"FS_{tag.upper()}\|\d+")


async def blk_reconciled(m: Machine, mark: Mark) -> None:
    await m.console.wait("driver restarted, reconciling", since=mark)
    await m.console.wait(r"driver restarted: failed \d+ client request", since=mark)


async def blk_restart_idle(m: Machine, blk: int, count: int) -> None:
    await read(m, "before")
    mark = await request(m, "restart", "blk")
    await m.console.wait(rf"ROOT\|debug-restart\|child={blk}\|count={count}\b", since=mark)
    await blk_reconciled(m, mark)
    await read(m, "after_idle")


async def blk_restart_in_flight(m: Machine, blk: int, count: int) -> None:
    """A restart lands while a read is outstanding, and the reader still answers."""
    mark = m.console.mark()
    await m.console.send("chryso_fs:read_inflight(dict).")
    await m.console.send("chryso_test:restart_pd('blk').")
    await m.console.wait(rf"ROOT\|debug-restart\|child={blk}\|count={count}\b", since=mark)
    await m.console.wait(r"FS_INFLIGHT\|(ok|error|EXIT)", since=mark)
    await read(m, "after_inflight")


async def blk_fault_idle(m: Machine, blk: int, count: int) -> None:
    await read(m, "fault_before")
    mark = await request(m, "fault", "blk")
    await faulted(m, blk, count, mark)
    await blk_reconciled(m, mark)
    await read(m, "fault_after_idle")


async def blk_fault_in_flight(m: Machine, blk: int, count: int) -> None:
    """The fault reaches Root before the in-flight reader gets its answer."""
    mark = m.console.mark()
    await m.console.send("chryso_fs:read_during_fault().")
    await faulted(m, blk, count, mark)
    await m.console.wait("driver restarted, reconciling", since=mark)
    outcome = await m.console.wait(r"FS_INFLIGHT\|(ok|error|EXIT)", since=mark)
    fault = m.console.search(rf"ROOT\|fault\|child={blk}(?:\||\r?$)", mark)
    if fault is None or fault.start > outcome.start:
        raise m.console.fail("in-flight block client answered before the fault")
    await read(m, "fault_after_inflight")
    no_debug_restart(m, blk, mark, "block fault used the requested-restart path")


# --- eth -------------------------------------------------------------------------


async def eth_restart(m: Machine, eth: int, count: int) -> None:
    mark = await request(m, "restart", "eth")
    await m.console.wait(rf"ROOT\|debug-restart\|child={eth}\|count={count}\b", since=mark)
    await m.console.wait(RECLAIMED, since=mark)


async def eth_fault(m: Machine, eth: int, count: int) -> None:
    mark = await request(m, "fault", "eth")
    await faulted(m, eth, count, mark)
    await m.console.wait(RECLAIMED, since=mark)
    no_debug_restart(m, eth, mark, "network fault used the requested-restart path")


async def net_ready(ctx: Context, m: Machine) -> None:
    """slirp routes for the guest only once lwIP holds its lease."""
    async with asyncio.timeout(400):
        await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
    await ready(ctx, m)


# --- group scenarios -------------------------------------------------------------


@registry.scenario("serial-recovery", group=GROUP, timeout=900)
async def serial_recovery(ctx: Context) -> None:
    serial = ctx.abi.child("serial")
    async with ctx.boot(IMAGE) as m:
        async with ctx.step("restart-smoke"), asyncio.timeout(300):
            await crasher_ladder(m, ctx.abi.crasher)
        ctx.record("restart-smoke", budget_exhausted=True)
        await ready(ctx, m)
        async with ctx.step("serial-restart-smoke"), asyncio.timeout(300):
            await serial_restart(m, serial, 1)
        ctx.record("serial-restart-smoke", restart_count=1)
        async with ctx.step("serial-fault-smoke"), asyncio.timeout(300):
            await serial_fault(m, serial, 2)
        ctx.record("serial-fault-smoke", restart_count=2)


@registry.scenario("timer-recovery", group=GROUP, timeout=900)
async def timer_recovery(ctx: Context) -> None:
    timer = ctx.abi.child("timer")
    async with ctx.boot(IMAGE) as m:
        await ready(ctx, m)
        async with ctx.step("timer-restart-smoke"), asyncio.timeout(400):
            await timer_restart(m, timer, 1)
        ctx.record("timer-restart-smoke", clock_advanced=True)
        async with ctx.step("timer-fault-smoke"), asyncio.timeout(400):
            await timer_fault(m, timer, 2)
        ctx.record("timer-fault-smoke", clock_advanced=True)


@registry.scenario("blk-recovery", group=GROUP, timeout=1200)
async def blk_recovery(ctx: Context) -> None:
    blk = ctx.abi.child("blk")
    async with ctx.boot(IMAGE) as m:
        await ready(ctx, m)
        async with ctx.step("blk-restart-smoke idle"), asyncio.timeout(400):
            await blk_restart_idle(m, blk, 1)
        ctx.record("blk-restart-smoke-idle", restart_count=1)
        async with ctx.step("blk-restart-smoke in flight"), asyncio.timeout(400):
            await blk_restart_in_flight(m, blk, 2)
        ctx.record("blk-restart-smoke-in-flight", restart_count=2)
        async with ctx.step("blk-fault-smoke idle"), asyncio.timeout(400):
            await blk_fault_idle(m, blk, 3)
        ctx.record("blk-fault-smoke-idle", restart_count=3)
        async with ctx.step("blk-fault-smoke in flight"), asyncio.timeout(400):
            await blk_fault_in_flight(m, blk, 4)
        ctx.record("blk-fault-smoke-in-flight", restart_count=4)


# The first guest write fails with EIO, then the disk recovers.
EIO_ONCE: Final = (WriteError(errno=EIO, once=True),)
# Every guest read waits 1 ms and then succeeds.
SLOW_READS: Final = (Delay(errno=0, event="read_aio", iotype="read", delay_ns=1_000_000),)


@registry.scenario("blk-write-smoke", group=GROUP, timeout=600)
async def blk_write(ctx: Context) -> None:
    """The control for blk-eio-smoke: a write, sync and close, then a read."""
    async with ctx.boot("test-image") as m, asyncio.timeout(400):
        banner = await m.console.wait("Eshell")
        await probes.load_test_modules(m, ctx.settings.test_modules, banner)
        await probes.call(m, "chryso_fs:write_status('plain').", r"FS_WRITE_PLAIN\|\{ok,ok,ok\}")
        await read(m, "after_write")


@registry.scenario("blk-eio-smoke", group=GROUP, timeout=600)
async def blk_eio(ctx: Context) -> None:
    """One write fails with EIO: the writer sees eio and the volume stays readable."""
    async with ctx.boot("test-image", blkdebug=EIO_ONCE) as m, asyncio.timeout(400):
        banner = await m.console.wait("Eshell")
        await probes.load_test_modules(m, ctx.settings.test_modules, banner)
        # The failed write leaves nothing dirty, so sync and close may succeed.
        await probes.call(
            m,
            "chryso_fs:write_status('eio').",
            r"FS_WRITE_EIO\|\{\{error,eio\},(?:ok|\{error,eio\}),(?:ok|\{error,eio\})\}",
        )
        await probes.call(
            m, "chryso_fs:read_outcome('after_eio', lists).", r"FS_READ_AFTER_EIO\|\{ok,\d+,\d+\}"
        )


@registry.scenario("blk-slow-smoke", group=GROUP, timeout=900)
async def blk_slow(ctx: Context) -> None:
    """Every disk read is 1 ms late: the system still boots and reads."""
    async with ctx.boot("test-image", blkdebug=SLOW_READS) as m, asyncio.timeout(700):
        banner = await m.console.wait("Eshell")
        await probes.load_test_modules(m, ctx.settings.test_modules, banner)
        await read(m, "slow")


@registry.scenario("net-recovery", group=GROUP, timeout=1200)
async def net_recovery(ctx: Context) -> None:
    eth = ctx.abi.child("eth")
    async with ctx.boot(IMAGE) as m:
        await net_ready(ctx, m)
        async with ctx.step("net-restart-smoke baseline"), asyncio.timeout(300):
            await probes.ping(m, "before", ctx.ports.take())
        ctx.record("net-restart-smoke-baseline", pinged=True)
        async with ctx.step("net-restart-smoke twice"), asyncio.timeout(600):
            await eth_restart(m, eth, 1)
            await probes.ping(m, "after1", ctx.ports.take())
            await eth_restart(m, eth, 2)
            await probes.ping(m, "after2", ctx.ports.take())
        ctx.record("net-restart-smoke", restart_count=2)
        async with ctx.step("net-fault-smoke twice"), asyncio.timeout(600):
            await probes.ping(m, "fault_before", ctx.ports.take())
            await eth_fault(m, eth, 3)
            await probes.ping(m, "fault_after1", ctx.ports.take())
            await eth_fault(m, eth, 4)
            await probes.ping(m, "fault_after2", ctx.ports.take())
        ctx.record("net-fault-smoke", restart_count=4)


@registry.scenario("net-unplug-smoke", group=GROUP, timeout=900)
async def net_unplug(ctx: Context) -> None:
    """A pulled cable, then a paused VM: the guest dials the host after each."""
    program = (
        Ping(kind="ping"),
        Unplug(kind="unplug", ms=3000),
        Ping(kind="ping"),
        Pause(kind="pause", ms=1000),
        Ping(kind="ping"),
    )
    outcomes: list[str] = []
    async with ctx.boot(IMAGE) as m:
        await net_ready(ctx, m)
        async with ctx.step("unplug and pause"), asyncio.timeout(300):
            model = Model(ctx.abi.driver_budget)
            await inject.check(m, program, model, abi=ctx.abi, log=outcomes.append)
        ctx.record("net-unplug-smoke", outcomes=outcomes)


@registry.scenario("blk-giveup-smoke", group=GROUP, timeout=1500)
async def blk_giveup(ctx: Context) -> None:
    """Spend the block driver's budget by faults, then the system must degrade."""
    blk, budget = ctx.abi.child("blk"), ctx.abi.driver_budget
    fault_line = rf"ROOT\|fault\|child={blk}(?:\||\r?$)"

    async def read_status(m: Machine, tag: str, module: str) -> str:
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
            if await read_status(m, "before", "lists") != "ok":
                raise m.console.fail("baseline fs read failed before any restart")
            for n in range(1, budget + 1):
                mark = m.console.mark()
                await m.console.send("chryso_test:fault_pd('blk').")
                await m.console.wait(rf"ROOT\|restart\|child={blk}\|count={n}\b", since=mark)
                probes.assert_fault_sequence(m, blk, n)
            if await read_status(m, "atbudget", "dict") != "ok":
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
                if await read_status(m, tag, module) not in ("error", "EXIT"):
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
            if await read_status(m, "gone", "orddict") not in ("error", "EXIT"):
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
