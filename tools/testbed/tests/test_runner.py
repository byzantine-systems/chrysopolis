"""Runner outcomes, isolation between scenarios, exclusive ordering, reports."""

from __future__ import annotations

import asyncio
import xml.etree.ElementTree as ET
from pathlib import Path

from chryso_testbed.abi import Abi
from chryso_testbed.machine import Tools
from chryso_testbed.runner import run, write_report
from chryso_testbed.scenario import Context, Registry, Settings


def settings(out: Path, jobs: int = 4) -> Settings:
    abi = Abi(drivers={"serial": 0}, beam=5, crasher=4, driver_budget=8, exit_fault_base=0)
    return Settings({}, out / "fat.img", abi, Tools("qemu", "qemu-img"), out, jobs)


def test_outcomes_isolation_and_exclusive_order(tmp_path: Path) -> None:
    registry = Registry()
    order: list[str] = []

    @registry.scenario("passes", group="g", timeout=5)
    async def passes(ctx: Context) -> None:
        async with ctx.step("work"):
            await asyncio.sleep(0.05)
        order.append("passes")

    @registry.scenario("fails", group="g", timeout=5)
    async def fails(ctx: Context) -> None:
        raise AssertionError("expected 2, got 3")

    @registry.scenario("errors", group="g", timeout=5)
    async def errors(ctx: Context) -> None:
        raise KeyError("missing image")

    @registry.scenario("hangs", group="g", timeout=0.1)
    async def hangs(ctx: Context) -> None:
        await asyncio.Event().wait()

    @registry.scenario("quiet", group="g", timeout=5, exclusive=True)
    async def quiet(ctx: Context) -> None:
        order.append("quiet")
        ctx.record("quiet", ran=True)

    results = {r.name: r for r in asyncio.run(run(registry.select(group="g"), settings(tmp_path)))}
    assert {n: r.status for n, r in results.items()} == {
        "passes": "pass",
        "fails": "fail",
        "errors": "error",
        "hangs": "fail",
        "quiet": "pass",
    }
    assert "expected 2, got 3" in results["fails"].message
    assert "deadline expired" in results["hangs"].message
    assert results["passes"].steps[0].name == "work"
    assert order == ["passes", "quiet"]
    assert (tmp_path / "quiet" / "observations.json").exists()

    write_report(list(results.values()), tmp_path)
    suite = ET.parse(tmp_path / "junit.xml").getroot()
    assert (suite.get("tests"), suite.get("failures"), suite.get("errors")) == ("5", "2", "1")


def test_semaphore_bounds_concurrency(tmp_path: Path) -> None:
    registry = Registry()
    running = 0
    peak = 0

    async def body(ctx: Context) -> None:
        nonlocal running, peak
        running += 1
        peak = max(peak, running)
        await asyncio.sleep(0.02)
        running -= 1

    for i in range(8):
        registry.scenario(f"s{i}", group="g", timeout=5)(body)
    asyncio.run(run(registry.select(), settings(tmp_path, jobs=3)))
    assert peak == 3
