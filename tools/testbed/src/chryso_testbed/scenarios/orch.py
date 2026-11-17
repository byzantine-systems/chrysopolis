"""Orch group: Root's status page, spec bank, command PPC and fallback.

Probe results arrive as framed runs, so a driver line spliced into one
fails the scenario as damaged instead of a regex half-matching it.
"""

from __future__ import annotations

import asyncio
import re
import statistics
from collections.abc import Sequence

from chryso_testbed import frames, inject, probes, rootlog
from chryso_testbed.console import ScenarioFailure
from chryso_testbed.faults import (
    BootConfig,
    Driver,
    Fault,
    Halt,
    Look,
    Model,
    Program,
    Rewant,
    Step,
    Want,
    boot_profile,
)
from chryso_testbed.machine import Machine
from chryso_testbed.outcomes import Outcome, render
from chryso_testbed.scenario import BEAM_FAULTS_EXPECTED, Context, registry
from chryso_testbed.scenarios.restart import net_ready

GROUP = "orch"
STATUS_ERRORS = r"ROOT_STATUS\|(?:invalid|torn|unexpected)"


async def booted(ctx: Context, m: Machine, *, after: int = 1) -> None:
    """Wait for the `after`th shell banner and load the probes."""
    async with asyncio.timeout(400):
        banners = await m.console.wait_count("Eshell", after)
        await probes.load_test_modules(m, ctx.settings.test_modules, banners[after - 1])


def stat_fields(result: frames.Pass) -> dict[str, str]:
    (stat,) = probes.records(result, "stat")
    return dict(stat.fields)


@registry.scenario("orch-status-smoke", group=GROUP, timeout=900)
async def orch_status(ctx: Context) -> None:
    """The C probe and /dev/chrysostat agree at boot and after a BEAM restart."""
    beam = ctx.abi.beam
    async with ctx.boot("test-image", failures=BEAM_FAULTS_EXPECTED) as m:
        async with ctx.step("boot"), asyncio.timeout(400):
            await m.console.wait(r"ROOT_STATUS\|boot\|incarnation=1\b")
            await booted(ctx, m)
            fields = stat_fields(
                await probes.run(m, f"chryso_ctl:stat(boot, {beam}).", "CTL_STAT.BOOT")
            )
            if fields.get("incarnation") != "1" or fields.get("count") != "0":
                raise m.console.fail(f"boot status read {fields}")

        async with ctx.step("local errors"), asyncio.timeout(120):
            await probes.run_probe(
                m,
                "chryso_ctl:stat_errors(probe).",
                "CTL_STAT_ERR.PROBE",
                ["short_read", "write_readonly", "second_read", "open_write"],
            )

        async with ctx.step("restart"), asyncio.timeout(600):
            mark = m.console.mark()
            await m.console.send("chryso_beam:stop_vm().")
            await m.console.wait(rf"ROOT\|restart\|child={beam}\|count=1\b", since=mark)
            await m.console.wait(r"ROOT_STATUS\|restart\|incarnation=2\|count=1\b", since=mark)
            await booted(ctx, m, after=2)
            fields = stat_fields(
                await probes.run(m, f"chryso_ctl:stat(restarted, {beam}).", "CTL_STAT.RESTARTED")
            )
            if fields.get("incarnation") != "2" or fields.get("count") != "1":
                raise m.console.fail(f"restarted status read {fields}")
        probes.require_absent(m, STATUS_ERRORS, "the status probe reported a bad page")


@registry.scenario("orch-spec-smoke", group=GROUP, timeout=1200)
async def orch_spec(ctx: Context) -> None:
    """Root applies a committed spec mid-commit and rejects a conflicting bank."""
    beam = ctx.abi.beam
    generation = "0x0000000000000001"
    async with ctx.boot("spec-commit-image", failures=BEAM_FAULTS_EXPECTED) as m:
        async with ctx.step("apply"), asyncio.timeout(700):
            await m.console.wait(r"SPEC_FIXTURE\|open\|committed=1\|pending=2")
            await booted(ctx, m)
            mark = m.console.mark()
            await m.console.send("chryso_beam:stop_vm().")
            await m.console.wait(
                rf"ROOT\|spec\|applied\|generation={generation}\|bank=1\b", since=mark
            )
            await m.console.wait(rf"ROOT\|restart\|child={beam}\|count=1\b", since=mark)
            await m.console.wait(r"SPEC_FIXTURE\|conflict-staged", since=mark)
            await booted(ctx, m, after=2)

        async with ctx.step("reject"), asyncio.timeout(300):
            mark = m.console.mark()
            await m.console.send("chryso_beam:stop_vm().")
            await m.console.wait(
                rf"ROOT\|spec\|reject\|reason=conflict\|generation={generation}\|bank=0\b",
                since=mark,
            )
            await m.console.wait(
                rf"ROOT\|giveup\|child={beam}\|reason=budget-exhausted", since=mark
            )
        probes.require_present(m, "SPEC_FIXTURE|applied|generation=1", "spec never applied")
        probes.require_absent(
            m,
            r"SPEC_FIXTURE\|(?:unexpected|writer-refused|invalid|status-unreadable|later)",
            "the spec fixture reported a failure",
        )
        probes.require_absent(m, STATUS_ERRORS, "the status probe reported a bad page")
        probes.require_absent(
            m, rf"ROOT\|restart\|child={beam}\|count=2\b", "the rejected spec raised the budget"
        )


@registry.scenario("orch-command-smoke", group=GROUP, timeout=900)
async def orch_command(ctx: Context) -> None:
    """Root's PPC from a C fixture and from Erlang, audited against Root's rows."""
    timer = ctx.abi.child("timer")
    fixture_steps = (
        "hello", "hello-version", "short", "unknown-opcode", "undeclared-child",
        "beam-child", "slot", "stale", "restart", "retry", "published", "flood",
    )  # fmt: skip
    async with ctx.boot("command-image") as m:
        async with ctx.step("C fixture"), asyncio.timeout(400):
            await m.console.wait(r"CMD_FIXTURE\|done")
            for step in fixture_steps:
                if m.console.search(rf"CMD_FIXTURE\|{step}\|result=\d+\|ok") is None:
                    raise m.console.fail(f"fixture step {step} did not report ok")
            probes.require_absent(
                m, r"CMD_FIXTURE\|[^|\r\n]*\|result=\d+\|unexpected", "a fixture step failed"
            )
            probes.require_absent(
                m,
                r"CMD_FIXTURE\|(?:invalid-map|status-unreadable|later)",
                "the fixture could not run",
            )
            probes.require_present(m, "CMD_FIXTURE|syncs=", "the fixture flood never ran")
            await booted(ctx, m)

        async with ctx.step("/dev/chrysoctl"), asyncio.timeout(300):
            result = await probes.run_probe(
                m,
                f"chryso_ctl:ctl_script(cmd, {timer}).",
                "CTL.CMD",
                [
                    "second_open", "read_idle", "hello", "short_write", "bad_magic",
                    "bad_opcode", "stale", "restart", "retry", "dropped_reply",
                ],
            )  # fmt: skip
            probes.assert_audit(m, result)
            restarts = [
                hit.groups
                for hit in m.console.findall(r"ROOT\|ctl-restart\|child=(\d+)\|count=(\d+)")
            ]
            if restarts != [(str(timer), "1"), (str(timer), "2")]:
                raise m.console.fail(f"ctl restarts were {restarts}")
            probes.require_absent(
                m,
                rf"ROOT\|(?:fault|giveup|restart)\|child={timer}\b",
                "a commanded restart took the fault path",
            )
            hit = await probes.call(
                m, "chryso_clock:sleep_check('cmd_slept').", r"CMD_SLEPT\|(\d+)"
            )
            if int(hit.groups[0] or 0) < 500:
                raise m.console.fail("the restarted timer no longer serves sleeps")


def latency_rows(result: frames.Pass) -> list[dict[str, float]]:
    rows = []
    for sample in probes.records(result, "sample"):
        f = dict(sample.fields)
        t0, t1, t2, t3 = (int(f[k]) for k in ("inject", "fault", "decided", "published"))
        freq = int(f["cntfrq"])
        if not t0 <= t1 <= t2 <= t3:
            raise AssertionError(f"timestamps out of order: {f}")
        rows.append({
            "deliver_us": (t1 - t0) * 1e6 / freq,
            "decide_us": (t2 - t1) * 1e6 / freq,
            "total_us": (t3 - t0) * 1e6 / freq,
        })  # fmt: skip
    return rows


def summary(rows: Sequence[dict[str, float]]) -> dict[str, dict[str, float]]:
    return {
        key: {
            "min": min(r[key] for r in rows),
            "median": statistics.median(r[key] for r in rows),
            "max": max(r[key] for r in rows),
        }
        for key in ("deliver_us", "decide_us", "total_us")
    }


async def latency(ctx: Context, m: Machine, tag: str, mode: str) -> frames.Pass:
    """21 driver faults timed from Root's own ticks, then the in-situ audit."""
    a = ctx.abi
    samples = ",".join(
        [
            f"{{serial,{a.child('serial')}}}",
            f"{{blk,{a.child('blk')}}}",
            f"{{eth,{a.child('eth')}}}",
        ]
        * 7
    )
    result = await probes.run(
        m, f"chryso_ctl:latency({tag}, {mode}, [{samples}]).", f"LAT.{tag.upper()}"
    )
    (done,) = probes.records(result, "done")
    if done.get("samples") != "21" or done.get("missing") != "0":
        raise m.console.fail(f"latency samples: {dict(done.fields)}")
    probes.assert_audit(m, result)
    return result


@registry.scenario("orch-fallback-smoke", group=GROUP, timeout=2400, exclusive=True)
async def orch_fallback(ctx: Context) -> None:
    """Root's fault safety without the BEAM, timed quiet and under a PPC flood."""
    beam, timer, eth = ctx.abi.beam, ctx.abi.child("timer"), ctx.abi.child("eth")
    bound = ctx.settings.fallback_bound_us

    async with ctx.step("baseline"), ctx.boot("fallback-image") as m, asyncio.timeout(900):
        await booted(ctx, m)
        rows = latency_rows(await latency(ctx, m, "fb_base", "baseline"))
        ctx.record("baseline", latency=summary(rows))

    async with ctx.step("flood"), ctx.boot("fallback-image", failures=BEAM_FAULTS_EXPECTED) as m:
        async with asyncio.timeout(900):
            await m.console.wait(r"SPEC_FIXTURE\|open\|committed=1\|pending=2")
            await booted(ctx, m)
            result = await latency(ctx, m, "fb_flood", "flood")
            rows = latency_rows(result)
            (flood,) = probes.records(result, "flood")
            calls, limited = int(flood.get("calls")), int(flood.get("rate_limited"))
            ctx.record(
                "flood", calls=calls, rate_limited=limited, bound_us=bound, latency=summary(rows)
            )
            if limited == 0:
                raise m.console.fail(f"flood never reached rate_limited ({calls} calls)")
            worst = max(r["total_us"] for r in rows)
            if worst > bound:
                raise m.console.fail(f"flood worst {worst:.1f} us exceeds the {bound} us bound")
            pre = stat_fields(
                await probes.run(m, f"chryso_ctl:stat(fb_pre, {eth}).", "CTL_STAT.FB_PRE")
            )

        async with asyncio.timeout(900):
            mark = m.console.mark()
            await m.console.send("chryso_beam:stop_vm().")
            # Root applies the committed spec at its first restart decision,
            # which during the flood is the first driver fault, not this halt.
            await m.console.wait(r"ROOT\|spec\|applied\|generation=0x0000000000000001")
            await m.console.wait(rf"ROOT\|restart\|child={beam}\|count=1\b", since=mark)
            await m.console.wait(r"SPEC_FIXTURE\|conflict-staged", since=mark)
            await booted(ctx, m, after=2)
            await probes.run_probe(
                m,
                f"chryso_ctl:reconcile(fb_post, {eth}, {pre['incarnation']}, {pre['seq']}, 1).",
                "CTL_RECONCILE.FB_POST",
                ["incarnation_advanced", "applied_spec", "old_seq", "fresh"],
            )
            await m.console.wait(r"ROOT\|spec\|reject\|reason=conflict", since=mark)

        async with asyncio.timeout(300):
            mark = m.console.mark()
            await m.console.send("chryso_beam:stop_vm().")
            await m.console.wait(
                rf"ROOT\|giveup\|child={beam}\|reason=budget-exhausted", since=mark
            )
            await m.console.wait(r"PEER\|done", since=mark)
            tail = m.console.text()[mark.offset :].decode("utf-8", "replace")
            expected = {
                "peer restart 1": "PEER|fallback|timer|count=1" in tail,
                "peer restart 2": "PEER|fallback|timer|count=2" in tail,
                "two injections": tail.count(f"ROOT|fault-inject|child={timer}") == 2,
                "one beam give-up": tail.count(f"ROOT|giveup|child={beam}|") == 1,
            }
            if failed := [name for name, ok in expected.items() if not ok]:
                raise m.console.fail(f"fallback peer stage: {failed}")
            for count in (1, 2):
                await m.console.wait(rf"ROOT\|restart\|child={timer}\|count={count}\b", since=mark)
            probes.require_absent(
                m, r"PEER\|(?:failed|beam-not-gone|unexpected|invalid)", "the peer failed"
            )
            probes.require_absent(m, r"ROOT\|reject", "Root rejected a request")

    async with ctx.step("nomap"), ctx.boot("fallback-nomap-image") as m, asyncio.timeout(900):
        await booted(ctx, m)
        fields = stat_fields(
            await probes.run(m, f"chryso_ctl:stat(fb_nomap, {eth}).", "CTL_STAT.FB_NOMAP")
        )
        if fields.get("error") != "enodev":
            raise m.console.fail(f"nomap status read {fields}")
        for count in range(1, ctx.abi.driver_budget + 1):
            await probes.call(
                m, "chryso_test:fault_pd(eth).", rf"ROOT\|restart\|child={eth}\|count={count}\b"
            )
        await probes.call(
            m, "chryso_test:fault_pd(eth).", rf"ROOT\|giveup\|child={eth}\|reason=budget-exhausted"
        )
        probes.require_absent(m, r"ROOT\|status\|", "publication ran without a status page")
        probes.require_absent(m, r"ROOT\|spec\|", "the spec consumer ran without a spec page")


# --- the driver orchestrator ------------------------------------------------------

type Part = tuple[str, Program]
ORCH_DRIVERS: tuple[Driver, ...] = ("serial", "timer", "blk", "eth")


def orch_driver(budget: int) -> tuple[Part, ...]:
    """The smoke's program in named parts, for a Root that gives each driver
    `budget` restarts. `tests/test_smokes.py` folds it through the model."""

    def look(d: Driver) -> Look:
        return Look(kind="look", driver=d)

    eth_fault = Fault(kind="fault", driver="eth")
    others: tuple[Driver, ...] = ("serial", "timer", "eth")
    return (
        ("fault", (Fault(kind="fault", driver="blk"), look("blk"))),
        ("ready", tuple(look(d) for d in others)),
        (
            "want",
            (
                Want(kind="want", driver="timer"),
                Rewant(kind="rewant", driver="timer"),
                look("timer"),
            ),
        ),
        (
            "gone",
            (
                *[eth_fault] * budget,
                look("eth"),
                eth_fault,
                look("eth"),
                Want(kind="want", driver="eth"),
                look("eth"),
            ),
        ),
        ("flood", (Want(kind="want", driver="blk", flood=True),)),
    )


@registry.scenario("orch-driver-smoke", group=GROUP, timeout=1800)
async def orch_driver_smoke(ctx: Context) -> None:
    """The orchestrator on the guest while drivers fault, restart, exhaust
    their budget and get rate limited: every step's outcome must be one the
    fault model allows, the decision log must replay after every part, Root's
    ring must record exactly the commands sent, and Root must count no command
    for a driver it gave up on."""
    async with ctx.boot("restart-image") as m:
        await net_ready(ctx, m)
        facts = await run_orch_driver(ctx, m)
        ctx.record("orch-driver-smoke", **facts)


async def run_orch_driver(ctx: Context, m: Machine) -> dict[str, object]:
    """Run the program part by part with the in-situ audits, failing at the
    first broken claim. Returns the facts a report records."""
    budget = ctx.abi.driver_budget
    model = Model.start(budget)
    decisions: list[tuple[Step, Outcome]] = []
    outcomes: list[str] = []
    replays: list[dict[str, str]] = []

    def observe(step: Step, seen: Outcome, before: Model) -> None:
        decisions.append((step, seen))
        outcomes.append(f"{step.kind} {step.model_dump(exclude={'kind'})}: {render(seen)}")

    _, ring = rootlog.read(await probes.run(m, "chryso_ctl:events(start, 0).", "CTL_EVENTS.START"))
    for n, (name, program) in enumerate(orch_driver(budget)):
        async with ctx.step(name), asyncio.timeout(600):
            model = await inject.check(
                m, program, model, abi=ctx.abi, modules=ctx.settings.test_modules, observe=observe
            )
            result = await probes.run(m, f"chryso_orch:replay(p{n}).", f"ORCH_REPLAY.P{n}")
            (replay,) = probes.records(result, "replay")
            fields = dict(replay.fields)
            replays.append({"part": name, **fields})
            if fields.get("verdict") != "match":
                raise m.console.fail(f"part {name}: the decision log did not replay: {fields}")
    events, _ = rootlog.read(
        await probes.run(m, f"chryso_ctl:events(finish, {ring.head}).", "CTL_EVENTS.FINISH")
    )
    children = {d: ctx.abi.child(d) for d in ORCH_DRIVERS}
    if problems := rootlog.disagreements(decisions, events, children):
        raise m.console.fail(f"Root's ring disagrees with its console: {problems}")
    flooded = [
        int(h.groups[0] or 0) for h in m.console.findall(r"k=want\|[^@]*flood_limited=(\d+)")
    ]
    if not flooded or max(flooded) == 0:
        raise m.console.fail(f"the flood was never rate limited: {flooded}")
    if not any(int(r.get("dropped", "0")) > 0 for r in replays):
        raise m.console.fail("the decision log never wrapped, so the bound went untested")
    return {
        "outcomes": outcomes,
        "replays": replays,
        "orchestrator_limited": max(int(r.get("limited", "0")) for r in replays),
        "flood_limited": max(flooded),
    }


# --- negative controls ------------------------------------------------------------

# Each variant disk carries one mutated rule (modules/images.nix), and the
# smoke must fail with exactly the failure its detection chain predicts.
NEGATIVE_CONTROLS: tuple[tuple[str, str], ...] = (
    # Reach: part gone's want of eth. Diverge: a command to a gone driver.
    # Propagate: Root prints ignored=ctl-restart. Fire: the model allows only
    # `unasked`.
    ("nc-gone", r"got gone, allowed \['unasked'\]"),
    # Reach: part want's want of timer. Diverge: a second identical command.
    # Propagate: Root answers already_done, so the counts stay right, and the
    # entry is still in the decision ring when the part ends. Fire: replay.
    ("nc-twice", r"part want: the decision log did not replay"),
)


@registry.scenario("orch-driver-negative", group="isolated", timeout=1800)
async def orch_driver_negative(ctx: Context) -> None:
    """orch-driver-smoke against each negative-control disk: each must fail,
    and for the reason predicted, or this scenario fails."""
    rows: list[dict[str, str]] = []
    for variant, required in NEGATIVE_CONTROLS:
        got = "passed"
        async with (
            ctx.step(variant),
            ctx.boot("restart-image", disk=ctx.settings.disks[variant]) as m,
        ):
            await net_ready(ctx, m)
            try:
                await run_orch_driver(ctx, m)
            except ScenarioFailure as failure:
                got = str(failure).split("\n", 1)[0]
        rows.append({"variant": variant, "predicted": required, "got": got})
        if re.search(required, got) is None:
            raise ScenarioFailure(f"{variant} did not fail as predicted: {got}")
    ctx.record("orch-driver-negative", controls=rows)


# --- the orchestrator across BEAM restarts ------------------------------------------

# orch-image's patched request (modules/images.nix).
ORCH_BOOT = BootConfig(profile="orch", safe_below=62)


async def boot_info(m: Machine, tag: str) -> dict[str, str]:
    (boot,) = probes.records(
        await probes.run(m, f"chryso_beam:boot_info({tag}).", f"BEAM_BOOT.{tag.upper()}"), "boot"
    )
    return dict(boot.fields)


def ctl_restarts(m: Machine, child: int) -> int:
    return len(m.console.findall(rf"ROOT\|ctl-restart\|child={child}\|count=\d+"))


@registry.scenario("orch-restart-smoke", group=GROUP, timeout=1800)
async def orch_restart_smoke(ctx: Context) -> None:
    """The orchestrator release boots with no shell command, a BEAM halt in the
    middle of a want leaves Root untouched, a halt after a want never repeats
    it, and near the BEAM's budget the selector boots start.boot while Root
    still serves raw commands."""
    async with ctx.boot("orch-image", failures=BEAM_FAULTS_EXPECTED) as m:
        await net_ready(ctx, m)
        facts = await run_orch_restart(ctx, m)
        ctx.record("orch-restart-smoke", **facts)


async def run_orch_restart(ctx: Context, m: Machine) -> dict[str, object]:
    """The smoke's parts, each judged against the fault model's `Halt`."""
    timer = ctx.abi.child("timer")
    modules = ctx.settings.test_modules
    model = Model.start(ctx.abi.driver_budget, boot=ORCH_BOOT)
    decisions: list[tuple[Step, Outcome]] = []
    outcomes: list[str] = []

    def observe(step: Step, seen: Outcome, before: Model) -> None:
        decisions.append((step, seen))
        outcomes.append(f"{step.kind} {step.model_dump(exclude={'kind'})}: {render(seen)}")

    async def run(program: Program) -> Model:
        return await inject.check(m, program, model, abi=ctx.abi, modules=modules, observe=observe)

    async with ctx.step("boot"), asyncio.timeout(120):
        info = await boot_info(m, "b1")
        want = boot_profile(model.orch)
        got = inject.boot_outcome(info.get("boot", ""))
        if render(got) != f"boot:{want}" or info.get("orchestrator") != "true":
            raise m.console.fail(f"boot profile {render(got)}, wanted {want}, running: {info}")
        _, ring = rootlog.read(
            await probes.run(m, "chryso_ctl:events(start, 0).", "CTL_EVENTS.START")
        )

    async with ctx.step("halt before Root replies"), asyncio.timeout(600):
        (held,) = probes.records(
            await probes.run(m, f"chryso_orch:held_want(h1, {timer}).", "ORCH_HELD.H1"), "held"
        )
        if held.get("requested") != "ok":
            raise m.console.fail(f"the held want was refused: {dict(held.fields)}")
        model = await run((Halt(kind="halt"),))
        if ctl_restarts(m, timer):
            raise m.console.fail("a want halted before its command still reached Root")

    async with ctx.step("halt after Root replies"), asyncio.timeout(600):
        model = await run((Want(kind="want", driver="timer"),))
        (replay,) = probes.records(
            await probes.run(m, "chryso_orch:replay(r2).", "ORCH_REPLAY.R2"), "replay"
        )
        if replay.get("verdict") != "match":
            raise m.console.fail(f"the decision log did not replay: {dict(replay.fields)}")
        pre = stat_fields(await probes.run(m, f"chryso_ctl:stat(s2, {timer}).", "CTL_STAT.S2"))
        model = await run((Halt(kind="halt"),))
        if (n := ctl_restarts(m, timer)) != 1:
            raise m.console.fail(f"{n} ctl-restarts of the timer across the halts, wanted 1")

    async with ctx.step("safe mode"), asyncio.timeout(600):
        info = await boot_info(m, "b3")
        if info.get("orchestrator") != "false":
            raise m.console.fail(f"the orchestrator runs in safe mode: {info}")
        events, _ = rootlog.read(
            await probes.run(m, f"chryso_ctl:events(finish, {ring.head}).", "CTL_EVENTS.FINISH")
        )
        children = {d: ctx.abi.child(d) for d in ORCH_DRIVERS}
        if problems := rootlog.disagreements(decisions, events, children):
            raise m.console.fail(f"Root's ring disagrees with its console: {problems}")
        await probes.run_probe(
            m,
            f"chryso_ctl:reconcile(r3, {timer}, {pre['incarnation']}, {pre['seq']}, 0).",
            "CTL_RECONCILE.R3",
            ["incarnation_advanced", "applied_spec", "old_seq", "fresh"],
        )
    return {"outcomes": outcomes, "beam_spent": model.orch.beam_spent}


@registry.scenario("orch-restart-negative", group="isolated", timeout=900)
async def orch_restart_negative(ctx: Context) -> None:
    """orch-restart-smoke on orch-image-nc, whose boot config has a version the
    C refuses: the BEAM must boot start.boot, so the smoke fails at its first
    claim, for exactly that reason."""
    required = r"boot profile boot:start, wanted orch"
    got = "passed"
    async with ctx.boot("orch-image-nc", failures=BEAM_FAULTS_EXPECTED) as m:
        await net_ready(ctx, m)
        try:
            await run_orch_restart(ctx, m)
        except ScenarioFailure as failure:
            got = str(failure).split("\n", 1)[0]
    if re.search(required, got) is None:
        raise ScenarioFailure(f"orch-image-nc did not fail as predicted: {got}")
    ctx.record("orch-restart-negative", predicted=required, got=got)
