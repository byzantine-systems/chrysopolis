"""The interpreter for fault program steps: QMP calls and shell probes in,
typed outcomes out. `perform` never judges, `check` compares with the model.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import Callable, Sequence
from typing import Final

import structlog

from chryso_testbed import probes
from chryso_testbed.abi import Abi
from chryso_testbed.blkdebug import SectorRead, SectorWrite
from chryso_testbed.console import Mark
from chryso_testbed.fatlayout import Region, layout
from chryso_testbed.faults import (
    Arm,
    Corrupt,
    CrashPut,
    Disarm,
    Driver,
    Fault,
    Get,
    Model,
    Pause,
    Ping,
    Program,
    Put,
    Reset,
    Restart,
    Step,
    Unplug,
)
from chryso_testbed.machine import Machine
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted, Seed, Status, render
from chryso_testbed.ports import Ports

log = structlog.get_logger(__name__)
PING_TIMEOUT: Final = 20.0
# What a corrupted sector is filled with. Only one of a file's sixteen
# sectors changes, so it reads back as differs whatever its seed.
CORRUPTION: Final = 0xEE
RECLAIMED: Final = r"ETH\|restart\|reclaimed\|rx=\d+\|tx=\d+"
# What each driver prints once it is serving again, or has been stopped.
SETTLED: Final[dict[tuple[Driver, str], str]] = {
    ("blk", "restart"): r"driver restarted: failed \d+ client request",
    ("blk", "debug-restart"): r"driver restarted: failed \d+ client request",
    ("blk", "giveup"): r"driver stopped: \d+ client\(s\) marked not ready",
    ("eth", "restart"): RECLAIMED,
    ("eth", "debug-restart"): RECLAIMED,
}


def put_outcome(reported: str) -> Outcome:
    """`{Write,Sync,Close}` or `{error,R}` as OK, EIO or ERROR."""
    errors = set(re.findall(r"\{error,(\w+)\}", reported))
    if reported == "{ok,ok,ok}":
        return Status.OK
    return Status.EIO if errors == {"eio"} else Status.ERROR


def get_outcome(reported: str) -> Outcome:
    """`{ok,Seed}`, `differs` or `{error,R}` as a Seed, DIFFERS, ENOENT, EIO or ERROR."""
    if seed := re.fullmatch(r"\{ok,(\d+)\}", reported):
        return Seed(int(seed[1]))
    match reported:
        case "differs":
            return Status.DIFFERS
        case "{error,enoent}":
            return Status.ENOENT
        case "{error,eio}":
            return Status.EIO
        case _:
            return Status.ERROR


def decision(kind: str, count: str | None, reason: str | None) -> Outcome:
    """A `ROOT|<kind>|child=N|...` line as an outcome."""
    match kind, count, reason:
        case "restart" | "debug-restart" as path, str(n), _:
            return Restarted(path, int(n))
        case "giveup", _, str(why):
            return GaveUp(why)
        case "gone", _, _:
            return Status.GONE
        case _:
            raise ValueError(f"unreadable Root decision: {kind} {count} {reason}")


async def root_decision(
    m: Machine, driver: Driver, child: int, mark: Mark, *, settle: bool = True
) -> Outcome:
    """Root's answer to a request for `child`, and with `settle` the driver
    serving again after it."""
    hit = await m.console.wait(
        rf"ROOT\|(restart|debug-restart|giveup|gone)\|child={child}\|"
        r"(?:count=(\d+)|reason=([\w-]+)|ignored=[\w-]+)",
        since=mark,
    )
    kind, count, reason = hit.groups
    if settle and (settled := SETTLED.get((driver, kind or ""))) is not None:
        await m.console.wait(settled, since=m.console.at(hit.end))
    return decision(kind or "", count, reason)


SERVING_TRIES: Final = 20


async def serving(m: Machine, driver: Driver, tag: str, ports: Ports) -> bool:
    """Whether `driver` serves again, shown by a request that succeeds: a
    module read for blk, a ping for eth. Its own console lines cannot be
    waited for here, since the next command's echo splices into them."""
    for attempt in range(SERVING_TRIES):
        probe = f"{tag}r{attempt}"
        match driver:
            case "blk":
                hit = await probes.call(
                    m, f"chryso_fs:read_status({probe}, lists).",
                    rf"BLKGONE_{probe.upper()}\|(ok|error|EXIT)",
                )  # fmt: skip
                if hit.groups[0] == "ok":
                    return True
            case "eth":
                if await ping(m, probe, ports.take()) == Status.OK:
                    return True
            case _:
                return True
        await asyncio.sleep(0.5)
    return False


async def ping(m: Machine, tag: str, port: int) -> Outcome:
    """OK when the host got the tag and the guest said so, UNREACHABLE otherwise."""
    expected = tag.upper().encode()
    async with probes.Listener(port, len(expected)) as listener:
        mark = m.console.mark()
        await m.console.send(f"chryso_net:ping('{tag}', {port}).")
        said = await m.console.wait(rf"NET_(OK|ERR)\|{tag.upper()}\b", since=mark)
        if said.groups[0] != "OK":
            return Status.UNREACHABLE
        try:
            async with asyncio.timeout(PING_TIMEOUT):
                got = await listener.received()
        except TimeoutError:
            return Status.UNREACHABLE
    return Status.OK if got == expected else Status.UNREACHABLE


async def locate(m: Machine, region: Region, file: int) -> int | None:
    """The disk sector holding `region` of fuzz file `file` right now."""
    image = await m.copy_disk()
    found = await asyncio.to_thread(layout, image)
    return found.sector(region, f"fuzz{file}.bin")


DHCP: Final = r"SOCKET_SMOKE\|DHCP:"


async def reboot(m: Machine, modules: Sequence[str]) -> None:
    """Reset the system and wait until the shell is ready again: the lease,
    if the first boot had one, then the prompt, then the probes reloaded."""
    await m.disarm()
    leased = m.console.search(DHCP) is not None
    mark = m.console.mark()
    await m.reset()
    if leased:
        await m.console.wait(DHCP, since=mark)
    banner = await m.console.wait("Eshell", since=mark)
    await probes.load_test_modules(m, modules, banner)


async def perform(
    m: Machine, step: Step, *, tag: str, abi: Abi, port: int, modules: Sequence[str]
) -> Outcome:
    """Do `step` and return its outcome. `tag` is a fresh lower-case atom, and
    `modules` are the probes a reboot reloads."""
    match step:
        case Fault(driver=d) | Restart(driver=d):
            mark = m.console.mark()
            verb = "fault" if isinstance(step, Fault) else "restart"
            await m.console.send(f"chryso_test:{verb}_pd({d}).")
            return await root_decision(m, d, abi.child(d), mark, settle=step.settle)
        case Unplug(ms=ms):
            async with m.unplugged():
                await asyncio.sleep(ms / 1000)
            return Status.OK
        case Pause(ms=ms):
            async with m.paused():
                await asyncio.sleep(ms / 1000)
            return Status.OK
        case Put(file=f, seed=s):
            hit = await probes.call(
                m, f"chryso_fs:put({tag}, {f}, {s}).", rf"FS_PUT_{tag.upper()}\|(\S+)"
            )
            return put_outcome(hit.groups[0] or "")
        case Get(file=f):
            hit = await probes.call(
                m, f"chryso_fs:get({tag}, {f}).", rf"FS_GET_{tag.upper()}\|(\S+)"
            )
            return get_outcome(hit.groups[0] or "")
        case Ping():
            return await ping(m, tag, port)
        case Arm(op=op, region=region, file=f, once=once):
            if (sector := await locate(m, region, f)) is None:
                await m.disarm()
                return Status.ABSENT
            rule = SectorRead if op == "read" else SectorWrite
            await m.arm((rule(sector=sector, once=once),))
            return Status.PRESENT
        case Disarm():
            await m.disarm()
            return Status.OK
        case Corrupt(region=region, file=f):
            if (sector := await locate(m, region, f)) is None:
                return Status.ABSENT
            await m.overwrite(sector, 1, CORRUPTION)
            return Status.PRESENT
        case Reset():
            await reboot(m, modules)
            return Status.OK
        case CrashPut(file=f, seed=s, after_ms=ms):
            await m.console.send(f"chryso_fs:put({tag}, {f}, {s}).")
            await asyncio.sleep(ms / 1000)
            await reboot(m, modules)
            return Status.OK


async def check(
    m: Machine,
    program: Program,
    model: Model,
    *,
    abi: Abi,
    modules: Sequence[str],
    observe: Callable[[Step, Outcome, Model], object] | None = None,
    step_timeout: float = 300,
) -> Model:
    """Run `program`, failing at the first outcome `model` does not allow.

    Each step logs a `step` event with its fields and outcome. `observe` sees
    each step, its outcome and the model before it. Returns
    the model after the last step, so the caller can see what the run went
    through (which drivers are gone, say)."""
    ports = Ports()
    # Drivers left recovering by an unsettled restart, confirmed serving
    # after the next step.
    pending: set[Driver] = set()
    for n, step in enumerate(program):
        allowed = model.expect(step)
        async with asyncio.timeout(step_timeout):
            seen = await perform(m, step, tag=f"s{n}", abi=abi, port=ports.take(), modules=modules)
            if isinstance(step, Reset | CrashPut):
                pending.clear()  # a reboot starts every driver afresh
            unsettled: Driver | None = None
            if isinstance(step, Fault | Restart) and not step.settle:
                unsettled = step.driver
            for driver in sorted(pending - {unsettled}):
                if not await serving(m, driver, f"s{n}{driver}", ports):
                    log.warning("driver.unconfirmed", driver=driver, n=n)
                pending.discard(driver)
            if unsettled is not None and isinstance(seen, Restarted):
                pending.add(unsettled)
        if observe is not None:
            observe(step, seen, model)
        fields = {"n": n, **step.model_dump(), "outcome": render(seen)}
        if seen not in allowed:
            wanted = "anything" if allowed.anything else sorted(map(render, allowed.outcomes))
            log.error("step.disallowed", **fields, allowed=wanted)
            raise m.console.fail(f"step {n} {step!r}: got {render(seen)}, allowed {wanted}")
        log.info("step", **fields)
        model = model.after(step, seen)
    return model
