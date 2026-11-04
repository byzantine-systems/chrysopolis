"""Run scenarios concurrently and write the report.

Shared scenarios run under a semaphore of `jobs`. Exclusive ones run alone
afterwards, because their bounds were measured on a quiet host. A failing
scenario becomes a result and never cancels its siblings.
"""

from __future__ import annotations

import asyncio
import json
import traceback
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path
from time import perf_counter
from typing import Literal

from chryso_testbed.ports import Ports
from chryso_testbed.scenario import Context, Scenario, Settings, Step

type Status = Literal["pass", "fail", "error"]


@dataclass(frozen=True, slots=True)
class Result:
    name: str
    group: str
    status: Status
    seconds: float
    message: str = ""
    steps: list[Step] = field(default_factory=list)


async def execute(scenario: Scenario, settings: Settings, ports: Ports) -> Result:
    ctx = Context(scenario, settings, ports)
    started = perf_counter()
    status: Status = "pass"
    message = ""
    try:
        async with asyncio.timeout(scenario.timeout):
            await scenario.body(ctx)
    except TimeoutError:
        status, message = "fail", f"a deadline expired\n{ctx.diagnosis()}"
    except AssertionError as failure:
        status, message = "fail", str(failure)
    except Exception:
        status, message = "error", traceback.format_exc()
    finally:
        ctx.save_facts()
    return Result(
        scenario.name, scenario.group, status, perf_counter() - started, message, ctx.steps
    )


async def run(scenarios: Sequence[Scenario], settings: Settings) -> list[Result]:
    ports = Ports()
    gate = asyncio.Semaphore(settings.jobs)

    async def gated(scenario: Scenario) -> Result:
        async with gate:
            return await execute(scenario, settings, ports)

    async with asyncio.TaskGroup() as group:
        shared = [group.create_task(gated(s)) for s in scenarios if not s.exclusive]
    results = [task.result() for task in shared]
    for scenario in scenarios:
        if scenario.exclusive:
            results.append(await execute(scenario, settings, ports))
    return results


def write_report(results: Sequence[Result], out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps([asdict(r) for r in results], indent=2) + "\n")
    suite = ET.Element(
        "testsuite",
        name="chryso-testbed",
        tests=str(len(results)),
        failures=str(sum(r.status == "fail" for r in results)),
        errors=str(sum(r.status == "error" for r in results)),
        time=f"{sum(r.seconds for r in results):.3f}",
    )
    for r in results:
        case = ET.SubElement(
            suite, "testcase", classname=r.group, name=r.name, time=f"{r.seconds:.3f}"
        )
        if r.status != "pass":
            ET.SubElement(case, "failure" if r.status == "fail" else "error").text = r.message
    ET.ElementTree(suite).write(out / "junit.xml", encoding="utf-8", xml_declaration=True)
