"""Fault programs drawn by simulating the model (quickcheck-dynamic style).

`enabled` offers steps that matter in the current state, `reach` steers to
a goal, and `SCENARIOS` chain them. Look-ahead assumes `nominal` outcomes,
while the real run is judged against everything the model allows.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Final, Literal

from hypothesis import strategies as st

from chryso_testbed.fatlayout import REGIONS
from chryso_testbed.faults import (
    DRIVERS,
    FILES,
    KEPT,
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
    trim,
)
from chryso_testbed.outcomes import GaveUp, Outcome, Restarted, Seed, Status

MAX_STEPS: Final = 16
# When several outcomes are allowed, look ahead as if things went well.
PREFERENCE: Final[tuple[Status, ...]] = (
    Status.OK, Status.PRESENT, Status.DIFFERS, Status.ENOENT, Status.GONE,
    Status.UNREACHABLE, Status.EIO, Status.ERROR, Status.ABSENT,
)  # fmt: skip


def nominal(model: Model, step: Step) -> Outcome:
    """An outcome `model` allows for `step`, the one generation assumes."""
    allowed = model.expect(step)
    if allowed.anything:
        return Status.PRESENT if isinstance(step, Arm | Corrupt) else Status.OK
    for outcome in allowed.outcomes:
        if isinstance(outcome, Restarted | GaveUp | Seed):
            return outcome
    for status in PREFERENCE:
        if status in allowed.outcomes:
            return status
    raise AssertionError(f"{step!r} allows nothing")


def advance(model: Model, step: Step) -> Model:
    return model.after(step, nominal(model, step))


def held(model: Model) -> list[int]:
    """Files whose data is on disk, which faults and reads can aim at."""
    return [f for f in range(FILES) if model.storage.file(f).at in {"held", "corrupt"}]


def requestable(model: Model) -> list[Driver]:
    """Drivers a request can go to: up, and serial and timer within budget."""
    root = model.root
    return [
        d
        for d in DRIVERS
        if root.up(d) and (d not in KEPT or root.chart(d).model.spent < root.budget)
    ]


def _file(model: Model) -> st.SearchStrategy[int]:
    """Mostly a file that exists, sometimes any."""
    present = held(model)
    any_file = st.integers(0, FILES - 1)
    return st.one_of(st.sampled_from(present), any_file) if present else any_file


def writes(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(Put, kind=st.just("put"), file=_file(model), seed=st.integers(0, 255))


def _get(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(Get, kind=st.just("get"), file=_file(model))


def _arm(model: Model, op: Literal["read", "write"] | None = None) -> st.SearchStrategy[Step]:
    ops = st.just(op) if op is not None else st.sampled_from(["read", "write"])
    return st.builds(
        Arm,
        kind=st.just("arm"),
        op=ops,
        region=st.sampled_from(REGIONS),
        file=_file(model),
        once=st.booleans(),
    )


def _corrupt(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(
        Corrupt, kind=st.just("corrupt"), region=st.sampled_from(REGIONS), file=_file(model)
    )


def _request(model: Model, drivers: list[Driver]) -> st.SearchStrategy[Step]:
    driver, settle = st.sampled_from(drivers), st.booleans()
    return st.one_of(
        st.builds(Fault, kind=st.just("fault"), driver=driver, settle=settle),
        st.builds(Restart, kind=st.just("restart"), driver=driver, settle=settle),
    )


def enabled(model: Model) -> st.SearchStrategy[Step]:
    """A step that can matter from `model`. Workload first, so shrinking
    tends toward plain reads and writes."""
    choices = [writes(model), _get(model), _arm(model), _corrupt(model)]
    if model.storage.armed is not None:
        choices.append(st.just(Disarm(kind="disarm")))
    if drivers := requestable(model):
        choices.append(_request(model, drivers))
    choices += [
        st.just(Ping(kind="ping")),
        st.builds(Unplug, kind=st.just("unplug"), ms=st.integers(0, 1500)),
        st.builds(Pause, kind=st.just("pause"), ms=st.integers(0, 1000)),
    ]
    if not model.storage.damaged:  # a damaged volume may not mount again
        choices += [st.just(Reset(kind="reset")), _crash_put(model)]
    return st.one_of(choices)


def _crash_put(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(
        CrashPut,
        kind=st.just("crash-put"),
        file=_file(model),
        seed=st.integers(0, 255),
        after_ms=st.integers(0, 200),
    )


# --- scenarios ----------------------------------------------------------------------

type Scenario = Callable[[st.DrawFn, Model], tuple[list[Step], Model]]
type Goal = Callable[[Model], bool]
type Toward = Callable[[Model], st.SearchStrategy[Step]]


def anything(at_most: int) -> Scenario:
    """Up to `at_most` enabled steps."""

    def run(draw: st.DrawFn, model: Model) -> tuple[list[Step], Model]:
        steps: list[Step] = []
        for _ in range(draw(st.integers(0, at_most))):
            step = draw(enabled(model))
            steps.append(step)
            model = advance(model, step)
        return steps, model

    return run


def then(choose: Toward) -> Scenario:
    """One step chosen from the current state."""

    def run(draw: st.DrawFn, model: Model) -> tuple[list[Step], Model]:
        step = draw(choose(model))
        return [step], advance(model, step)

    return run


def reach(goal: Goal, toward: Toward, bound: int) -> Scenario:
    """Steps toward `goal`: up to `bound` mixed steps, three in four from
    `toward` and the rest any enabled step, then up to `bound` more from
    `toward` alone, stopping as soon as the goal holds. Where `toward` makes
    progress under nominal outcomes, the goal is always reached."""

    def run(draw: st.DrawFn, model: Model) -> tuple[list[Step], Model]:
        steps: list[Step] = []
        for focused in (False, True):
            for _ in range(bound):
                if goal(model):
                    return steps, model
                mixed = not focused and not draw(st.integers(0, 3))
                step = draw(enabled(model) if mixed else toward(model))
                steps.append(step)
                model = advance(model, step)
        return steps, model

    return run


def chain(*parts: Scenario) -> Scenario:
    def run(draw: st.DrawFn, model: Model) -> tuple[list[Step], Model]:
        steps: list[Step] = []
        for part in parts:
            more, model = part(draw, model)
            steps += more
        return steps, model

    return run


def has_file(model: Model) -> bool:
    return bool(held(model))


def gone(driver: Driver) -> Goal:
    return lambda model: not model.root.up(driver)


def _aimed(model: Model, make: Callable[[int], st.SearchStrategy[Step]]) -> st.SearchStrategy[Step]:
    """A step aimed at a present file, or a write that makes one."""
    present = held(model)
    return st.sampled_from(present).flatmap(make) if present else writes(model)


def _read_fault(file: int) -> st.SearchStrategy[Step]:
    return st.builds(
        Arm,
        kind=st.just("arm"),
        op=st.just("read"),
        region=st.just("data"),
        file=st.just(file),
        once=st.booleans(),
    )


def _metadata_write_fault(file: int) -> st.SearchStrategy[Step]:
    return st.builds(
        Arm,
        kind=st.just("arm"),
        op=st.just("write"),
        region=st.sampled_from(["fat", "entry"]),
        file=st.just(file),
        once=st.booleans(),
    )


def _data_corruption(file: int) -> st.SearchStrategy[Step]:
    return st.just(Corrupt(kind="corrupt", region="data", file=file))


def _read_fault_on_data(model: Model) -> st.SearchStrategy[Step]:
    return _aimed(model, _read_fault)


def _write_fault_on_metadata(model: Model) -> st.SearchStrategy[Step]:
    return _aimed(model, _metadata_write_fault)


def _corrupt_data(model: Model) -> st.SearchStrategy[Step]:
    return _aimed(model, _data_corruption)


def _workload(model: Model) -> st.SearchStrategy[Step]:
    return st.one_of(_get(model), writes(model))


def faults_to(drivers: tuple[Driver, ...], *, settle: bool = True) -> Toward:
    def toward(model: Model) -> st.SearchStrategy[Step]:
        live = [d for d in drivers if model.root.up(d)] or list(drivers)
        return st.builds(
            Fault, kind=st.just("fault"), driver=st.sampled_from(live), settle=st.just(settle)
        )

    return toward


def _in_window(model: Model) -> st.SearchStrategy[Step]:
    """A fault on a driver still recovering, so it lands in the window."""
    recovering = [d for d in DRIVERS if model.root.recovering(d)]
    return faults_to(tuple(recovering) or ("blk", "eth"))(model)


def _repeat(scenario: Scenario, times: int) -> Scenario:
    return chain(*[scenario] * times)


SCENARIOS: Final[dict[str, Scenario]] = {
    "latent": chain(
        reach(has_file, writes, 4), then(_read_fault_on_data), _repeat(then(_workload), 3)
    ),
    "corrupt-heal": chain(
        reach(has_file, writes, 4),
        then(_corrupt_data),
        then(_get),
        then(writes),
        then(_get),
    ),
    "write-fault": chain(
        reach(has_file, writes, 4), then(_write_fault_on_metadata), _repeat(then(_workload), 4)
    ),
    "recover-then-fault": chain(
        then(faults_to(("blk", "eth"), settle=False)),
        then(_in_window),
        anything(2),
        then(faults_to(("blk", "eth"))),
        anything(2),
    ),
    "exhaust": chain(reach(gone("blk"), faults_to(("blk",)), 10), _repeat(then(_workload), 3)),
    "reboot": chain(
        reach(has_file, writes, 4),
        _repeat(then(faults_to(("blk",))), 2),
        then(lambda _: st.just(Reset(kind="reset"))),
        _repeat(then(_workload), 2),
        then(faults_to(("blk",))),
    ),
    "mixed": anything(12),
}


@st.composite
def simulate(draw: st.DrawFn, scenario: Scenario, start: Model) -> tuple[list[Step], Model]:
    """The steps `scenario` draws from `start`, and the model they lead to
    under nominal outcomes."""
    return scenario(draw, start)


@st.composite
def programs(draw: st.DrawFn, start: Model, name: str) -> Program:
    """A program from scenario `name`, simulated from `start`."""
    steps, _ = SCENARIOS[name](draw, start)
    return trim(tuple(steps[:MAX_STEPS]), start.root.budget)
