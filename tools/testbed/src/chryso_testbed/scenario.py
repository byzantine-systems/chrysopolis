"""Scenarios: async functions that boot images and assert on what they see."""

from __future__ import annotations

import json
from collections.abc import AsyncIterator, Awaitable, Callable, Iterable
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from time import perf_counter
from typing import Any, Final

from chryso_testbed.abi import Abi
from chryso_testbed.blkdebug import Plan
from chryso_testbed.machine import Machine, Profile, Tools
from chryso_testbed.ports import Ports

type Body = Callable[[Context], Awaitable[None]]


@dataclass(frozen=True, slots=True)
class Scenario:
    name: str
    group: str
    body: Body
    timeout: float
    exclusive: bool = False
    tags: frozenset[str] = frozenset()


class Registry:
    def __init__(self) -> None:
        self._scenarios: dict[str, Scenario] = {}

    def scenario(
        self,
        name: str,
        *,
        group: str,
        timeout: float,
        exclusive: bool = False,
        tags: Iterable[str] = (),
    ) -> Callable[[Body], Body]:
        """Register an async scenario body under `name`."""

        def register(body: Body) -> Body:
            if name in self._scenarios:
                raise ValueError(f"scenario {name!r} registered twice")
            self._scenarios[name] = Scenario(name, group, body, timeout, exclusive, frozenset(tags))
            return body

        return register

    def select(self, *, group: str | None = None, names: Iterable[str] = ()) -> list[Scenario]:
        wanted = set(names)
        if unknown := wanted - self._scenarios.keys():
            raise KeyError(f"unknown scenarios: {sorted(unknown)}")
        return [
            s
            for s in self._scenarios.values()
            if (group is None or s.group == group) and (not wanted or s.name in wanted)
        ]


registry = Registry()


@dataclass(frozen=True, slots=True)
class Settings:
    images: dict[str, Path]
    disk: Path
    abi: Abi
    tools: Tools
    out: Path
    jobs: int
    test_modules: tuple[str, ...] = ()
    fallback_bound_us: int = 0


@dataclass(slots=True)
class Step:
    name: str
    seconds: float
    passed: bool


MON_FAULT: Final = r"MON\|ERROR"
# A probe that crashed prints this instead of its result, so waiting on is futile.
# The shell may print its next prompt on the same line first.
# A crashed probe (`** exception`), or a typed line the shell could not
# parse (`* 1:19: syntax error ...`), which would otherwise hang the wait.
SHELL_EXCEPTION: Final = r"^(?:\d+> )?(?:\*\* exception |\* \d+:\d+: )"


def default_failures(abi: Abi) -> list[str]:
    """A monitor-caught PD fault, a fault of the BEAM itself, or a crashed probe."""
    return [MON_FAULT, rf"ROOT\|fault\|child={abi.beam}(?:\||\r|$)", SHELL_EXCEPTION]


#: For scenarios whose subject is a BEAM fault.
BEAM_FAULTS_EXPECTED: Final = (MON_FAULT, SHELL_EXCEPTION)


@dataclass(slots=True)
class Context:
    scenario: Scenario
    settings: Settings
    ports: Ports
    steps: list[Step] = field(default_factory=list)
    facts: list[dict[str, Any]] = field(default_factory=list)
    machines: list[Machine] = field(default_factory=list)

    @property
    def abi(self) -> Abi:
        return self.settings.abi

    @property
    def workdir(self) -> Path:
        return self.settings.out / self.scenario.name

    @asynccontextmanager
    async def boot(
        self,
        image: str,
        *,
        hostfwd: tuple[tuple[int, int], ...] = (),
        failures: Iterable[str] | None = None,
        blkdebug: Plan = (),
        disk: Path | None = None,
        crashlog: bool = False,
        reboot: bool = False,
    ) -> AsyncIterator[Machine]:
        """Boot `image` and power it off on exit. `blkdebug` holds disk fault
        rules, and `disk` replaces the store FAT image, a crash image say."""
        name = f"{image}-{len(self.machines) + 1}"
        profile = Profile(
            self.settings.images[image],
            disk or self.settings.disk,
            hostfwd,
            blkdebug=blkdebug,
            crashlog=crashlog,
            reboot=reboot,
        )
        watched = default_failures(self.abi) if failures is None else failures
        async with Machine(
            name, profile, self.workdir / name, self.settings.tools, watched
        ) as machine:
            self.machines.append(machine)
            yield machine

    @asynccontextmanager
    async def step(self, name: str) -> AsyncIterator[None]:
        """A named, timed part of a scenario, shown in the report."""
        started = perf_counter()
        passed = False
        try:
            yield
            passed = True
        finally:
            self.steps.append(Step(name, perf_counter() - started, passed))

    def diagnosis(self) -> str:
        """What each machine was waiting for when cut short, and its tail."""
        return "\n".join(
            f"--- {m.name}: waiting for {m.console.interrupted or 'nothing'} ---\n"
            f"{m.console.tail()}"
            for m in self.machines
        )

    def record(self, scenario: str, **facts: Any) -> None:
        self.facts.append({"scenario": scenario, **facts})

    def save_facts(self) -> None:
        if self.facts:
            self.workdir.mkdir(parents=True, exist_ok=True)
            text = json.dumps(self.facts, indent=2, sort_keys=True) + "\n"
            (self.workdir / "observations.json").write_text(text)
