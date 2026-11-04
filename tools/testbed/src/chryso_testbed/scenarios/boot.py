"""Boot group: the production image, the bring-up image and two probes."""

from __future__ import annotations

import asyncio
import re

from chryso_testbed import probes
from chryso_testbed.console import Hit
from chryso_testbed.machine import Machine
from chryso_testbed.scenario import BEAM_FAULTS_EXPECTED, Context, registry

GROUP = "boot"
MILESTONES = (
    "beam_server up on the LionsOS reference stack.",
    "monotonic clock via sDDF timer:",
    "Handing off to ERTS core loop...",
    "MBR partitioning detected",
)


# --- steps, shared with the isolated scenarios ------------------------------------


def milestones(m: Machine) -> None:
    for milestone in MILESTONES:
        probes.require_present(m, milestone, "missing boot milestone")


async def shell(ctx: Context, m: Machine, banner: Hit) -> None:
    """The probes load and the shell evaluates, printing a bare 2 as well."""
    await probes.load_test_modules(m, ctx.settings.test_modules, banner)
    await probes.call(m, "chryso_test:eval_check().", r"SHELL_EVAL\|2")
    await m.console.wait(r"^2\r?$")


async def frame_check(m: Machine) -> None:
    """A control run is accepted and the same run with a changed CRC is refused."""
    mark = m.console.mark()
    await m.console.send("chryso_test:frame_check('fcheck').")
    control = await m.console.await_run("FCHECK.CONTROL", since=mark)
    if control.frames[0].get("text") != "a|b@c":
        raise m.console.fail(f"control run read back as {control.frames!r}")
    try:
        await m.console.await_run("FCHECK.PLANTED", since=mark)
    except AssertionError as refused:
        if "damaged FCHECK.PLANTED crc" not in str(refused):
            raise
    else:
        raise m.console.fail("a planted CRC change was accepted")


async def tcp_host_to_guest(m: Machine, forward: int) -> bytes:
    await m.console.wait(r"SOCKET_SMOKE\|DHCP:")
    await probes.call(m, "chryso_net:echo_server(5555).", "LISTENER_UP")
    echoed = await probes.echo(forward, b"CHRYSO_ECHO")
    if echoed != b"CHRYSO_ECHO":
        raise m.console.fail(f"echo returned {echoed!r}")
    await m.console.wait("ECHOED")
    return echoed


async def tcp_guest_to_host(ctx: Context, m: Machine) -> bytes:
    async with probes.Listener(ctx.ports.take(), len(b"CHRYSO_PING")) as listener:
        mark = m.console.mark()
        await m.console.send(f"chryso_net:ping('chryso_ping', {listener.port}).")
        got = await listener.received()
    if got != b"CHRYSO_PING":
        raise m.console.fail(f"host listener received {got!r}")
    await m.console.wait(r"NET_OK\|CHRYSO_PING", since=mark)
    return got


# --- scenarios ---------------------------------------------------------------------


@registry.scenario("boot-shell-tcp", group=GROUP, timeout=900)
async def boot_shell_tcp(ctx: Context) -> None:
    """Boot milestones, a working shell, frames, and TCP in both directions."""
    forward = ctx.ports.take()
    async with ctx.boot("test-image", hostfwd=((forward, 5555),)) as m:
        async with ctx.step("boot"), asyncio.timeout(300):
            banner = await m.console.wait("Eshell")
        milestones(m)
        ctx.record("boot-smoke", milestone_count=len(MILESTONES))
        async with ctx.step("shell"), asyncio.timeout(240):
            await shell(ctx, m, banner)
        ctx.record("shell-smoke", result=2)
        async with ctx.step("frames"), asyncio.timeout(60):
            await frame_check(m)
        async with ctx.step("tcp host to guest"), asyncio.timeout(300):
            echoed = await tcp_host_to_guest(m, forward)
        ctx.record("tcp-smoke-host-to-guest", payload=echoed.decode())
        async with ctx.step("tcp guest to host"), asyncio.timeout(180):
            got = await tcp_guest_to_host(ctx, m)
        ctx.record("tcp-smoke-guest-to-host", payload=got.decode())


@registry.scenario("socket-smoke", group=GROUP, timeout=300)
async def socket_smoke(ctx: Context) -> None:
    """Bring-up image, no ERTS: DHCP and the socket calls from C."""
    async with ctx.boot("default") as m, asyncio.timeout(180):
        await m.console.wait(r"SOCKET_SMOKE\|PASS")
        probes.require_present(m, "SOCKET_SMOKE|DHCP:", "no DHCP lease before the socket test")


@registry.scenario("cothread-smoke", group=GROUP, timeout=300)
async def cothread_smoke(ctx: Context) -> None:
    """Native pthread ABI probes inside beam_server, before the payload."""
    async with ctx.boot("cothread-probe-image") as m, asyncio.timeout(240):
        await m.console.wait(r"COTHREAD_PROBE\|PASS")
        probes.require_absent(m, r"COTHREAD_PROBE\|FAIL", "native pthread probe failed")


@registry.scenario("lifecycle-config-failure-smoke", group=GROUP, timeout=180)
async def lifecycle_config_failure(ctx: Context) -> None:
    """A malformed config stops beam_server before any boot state appears."""
    async with ctx.boot("lifecycle-failure-image") as m, asyncio.timeout(90):
        await m.console.wait(r"BEAM\|lifecycle\|FATAL\|stage=config\|status=config-serial\|code=1")
        probes.require_absent(m, r"BEAM\|boot\|", "boot state appeared after config failed")
        probes.require_absent(m, "Microkit event loop live", "event loop ran with partial state")
        probes.require_absent(
            m, rf"ROOT\|fault\|child={ctx.abi.beam}", "a config failure requested a restart"
        )


@registry.scenario("beam-restart-smoke", group=GROUP, timeout=1500)
async def beam_restart(ctx: Context) -> None:
    """An ERTS exit restarts beam_server clean, and the exit code reaches Root."""
    beam = ctx.abi.beam
    # BEAM faults are the point here, so only a monitor-caught fault fails waits.
    async with ctx.boot("test-image", failures=BEAM_FAULTS_EXPECTED) as m:
        modules = ctx.settings.test_modules
        async with ctx.step("mark"), asyncio.timeout(400):
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, modules, banner)
            await probes.call(m, "chryso_beam:mark().", r"BEAM_MARK\|set")
            await probes.call(m, "chryso_beam:check('before').", r"BEAM_MARK_BEFORE\|present")

        async with ctx.step("stop"), asyncio.timeout(600):
            await probes.call(
                m, "chryso_beam:stop_vm().", r"BEAM\|exit\|code=0\|requesting-restart"
            )
            await m.console.wait(rf"ROOT\|fault\|child={beam}")
            await m.console.wait(rf"ROOT\|restart\|child={beam}\|count=1")
            banners = await m.console.wait_count("Eshell", 2)
            await m.console.wait(r"BEAM\|boot\|generation=2\|bss-counter=1")
        if m.console.text().count(b"FAT filesystem mounted via fs_server.") < 2:
            raise m.console.fail("the restarted beam_server did not re-mount the FAT volume")

        async with ctx.step("clean"), asyncio.timeout(300):
            await probes.load_test_modules(m, modules, banners[-1])
            await probes.call(m, "chryso_beam:check('after').", r"BEAM_MARK_AFTER\|absent")
            await probes.call(m, "chryso_test:eval_check().", r"SHELL_EVAL\|2")

        address = f"{ctx.abi.exit_fault_base + 3:x}"
        async with ctx.step("halt 3"), asyncio.timeout(600):
            mark = m.console.mark()
            await m.console.send("chryso_beam:halt_vm(3).")
            await m.console.wait(r"BEAM\|exit\|code=3\|requesting-restart", since=mark)
            await m.console.wait(rf"ROOT\|fault\|child={beam}\|.*mr1=0x0*{address}", since=mark)
            await m.console.wait(rf"ROOT\|restart\|child={beam}\|count=2", since=mark)
            await m.console.wait_count("Eshell", 3)
            await m.console.wait(r"BEAM\|boot\|generation=3\|bss-counter=1")
        probes.require_absent(m, r"ROOT\|giveup", "Root gave up on a child")


@registry.scenario("rng-smoke", group=GROUP, timeout=1200, exclusive=True)
async def rng_smoke(ctx: Context) -> None:
    """Two boots: the RNG fingerprint and every sampled value must differ."""
    patterns = {
        "fingerprint": r"RNG\|source=\S+\|fp=([0-9a-f]+)",
        "rand:bytes": r"RNG_BYTES\|(\S+)",
        "make_ref": r"RNG_REF\|(\S+)",
        "urandom": r"RNG_URANDOM\|(\S+)",
    }
    samples: list[dict[str, str]] = []
    for boot in (1, 2):
        async with ctx.step(f"boot {boot}"), ctx.boot("test-image") as m, asyncio.timeout(450):
            await m.console.wait(r"RNG\|source=")
            banner = await m.console.wait("Eshell")
            await probes.load_test_modules(m, ctx.settings.test_modules, banner)
            await probes.call(m, "chryso_rng:sample().", r"RNG_URANDOM\|")
            text = m.console.text().decode("utf-8", "replace")
            samples.append({k: re.findall(p, text)[-1] for k, p in patterns.items()})
    first, second = samples
    if same := [k for k in patterns if first[k] == second[k]]:
        raise AssertionError(f"identical across boots: {same} ({first})")
