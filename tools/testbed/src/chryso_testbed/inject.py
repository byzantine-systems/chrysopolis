"""Run fault program steps against a guest and name what each one did.

`perform` is the interpreter for `faults.Step`. It types probes at the shell
or calls QMP, waits for the step's own evidence (probe lines carry the
step's tag, Root lines are read from the step's mark onwards) and returns
one of the outcome strings `faults.Model` predicts. It never judges: an
unexpected outcome is still returned. `check` runs a whole program and
compares each outcome with the model, so a failure names both.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import Callable
from typing import Final

from chryso_testbed import probes
from chryso_testbed.abi import Abi
from chryso_testbed.console import Mark
from chryso_testbed.faults import (
    Driver,
    Fault,
    Get,
    Model,
    Pause,
    Ping,
    Program,
    Put,
    Restart,
    Step,
    Unplug,
)
from chryso_testbed.machine import Machine
from chryso_testbed.ports import Ports

PING_TIMEOUT: Final = 20.0
RECLAIMED: Final = r"ETH\|restart\|reclaimed\|rx=\d+\|tx=\d+"
# What each driver prints once it is serving again, or has been stopped.
SETTLED: Final[dict[tuple[Driver, str], str]] = {
    ("blk", "restart"): r"driver restarted: failed \d+ client request",
    ("blk", "debug-restart"): r"driver restarted: failed \d+ client request",
    ("blk", "giveup"): r"driver stopped: \d+ client\(s\) marked not ready",
    ("eth", "restart"): RECLAIMED,
    ("eth", "debug-restart"): RECLAIMED,
}


def put_outcome(reported: str) -> str:
    """`{Write,Sync,Close}` or `{error,R}` as ok, eio or error."""
    errors = set(re.findall(r"\{error,(\w+)\}", reported))
    if reported == "{ok,ok,ok}":
        return "ok"
    return "eio" if errors == {"eio"} else "error"


def get_outcome(reported: str) -> str:
    """`{ok,Seed}`, `differs` or `{error,R}` as seed:N, differs, enoent, eio or error."""
    if seed := re.fullmatch(r"\{ok,(\d+)\}", reported):
        return f"seed:{seed[1]}"
    match reported:
        case "differs":
            return "differs"
        case "{error,enoent}":
            return "enoent"
        case "{error,eio}":
            return "eio"
        case _:
            return "error"


async def root_decision(m: Machine, driver: Driver, child: int, mark: Mark) -> str:
    """Root's answer to a request for `child`, and the driver settled after it."""
    hit = await m.console.wait(
        rf"ROOT\|(restart|debug-restart|giveup|gone)\|child={child}\|"
        r"(?:count=(\d+)|reason=([\w-]+)|ignored=[\w-]+)",
        since=mark,
    )
    kind, count, reason = hit.groups
    if (settled := SETTLED.get((driver, kind or ""))) is not None:
        await m.console.wait(settled, since=m.console.at(hit.end))
    return f"{kind}:{count or reason}" if count or reason else kind or ""


async def ping(m: Machine, tag: str, port: int) -> str:
    """ok when the host got the tag and the guest said so, err otherwise."""
    expected = tag.upper().encode()
    async with probes.Listener(port, len(expected)) as listener:
        mark = m.console.mark()
        await m.console.send(f"chryso_net:ping('{tag}', {port}).")
        said = await m.console.wait(rf"NET_(OK|ERR)\|{tag.upper()}\b", since=mark)
        if said.groups[0] != "OK":
            return "err"
        try:
            async with asyncio.timeout(PING_TIMEOUT):
                got = await listener.received()
        except TimeoutError:
            return "err"
    return "ok" if got == expected else "err"


async def perform(m: Machine, step: Step, *, tag: str, abi: Abi, port: int) -> str:
    """Do `step` and return its outcome. `tag` is a fresh lower-case atom."""
    match step:
        case Fault(driver=d) | Restart(driver=d):
            mark = m.console.mark()
            verb = "fault" if isinstance(step, Fault) else "restart"
            await m.console.send(f"chryso_test:{verb}_pd({d}).")
            return await root_decision(m, d, abi.child(d), mark)
        case Unplug(ms=ms):
            async with m.unplugged():
                await asyncio.sleep(ms / 1000)
            return "ok"
        case Pause(ms=ms):
            async with m.paused():
                await asyncio.sleep(ms / 1000)
            return "ok"
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


async def check(
    m: Machine,
    program: Program,
    model: Model,
    *,
    abi: Abi,
    log: Callable[[str], object] = print,
    step_timeout: float = 300,
) -> Model:
    """Run `program`, failing at the first outcome `model` does not allow.

    Returns the model after the last step, so the caller can see what the
    run went through (which drivers are gone, say)."""
    ports = Ports()
    for n, step in enumerate(program):
        allowed = model.expect(step)
        async with asyncio.timeout(step_timeout):
            seen = await perform(m, step, tag=f"s{n}", abi=abi, port=ports.take())
        log(f"step {n} {step!r}: {seen}")
        if seen not in allowed:
            raise m.console.fail(f"step {n} {step!r}: got {seen}, allowed {allowed}")
        model = model.after(step, seen)
    return model
