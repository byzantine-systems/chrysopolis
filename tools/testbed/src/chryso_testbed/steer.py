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
    Absorb,
    Arm,
    Corrupt,
    CrashPut,
    Disarm,
    Driver,
    Fault,
    Get,
    Look,
    Model,
    Overwrite,
    Pause,
    Ping,
    Program,
    Put,
    Remove,
    Rename,
    Reset,
    Restart,
    Rewant,
    Step,
    Unplug,
    Want,
    trim,
)
from chryso_testbed.outcomes import Booted, Condition, GaveUp, Outcome, Restarted, Seed, Status

MAX_STEPS: Final = 16
# When several outcomes are allowed, look ahead as if things went well. Every
# Status is listed, or a step allowed only an unlisted one has no nominal.
PREFERENCE: Final[tuple[Status, ...]] = (
    Status.OK, Status.PRESENT, Status.DIFFERS, Status.ENOENT, Status.EEXIST, Status.GONE,
    Status.UNASKED, Status.UNREACHABLE, Status.EIO, Status.ERROR, Status.ABSENT,
)  # fmt: skip


def nominal(model: Model, step: Step) -> Outcome:
    """An outcome `model` allows for `step`, the one generation assumes."""
    allowed = model.expect(step)
    if allowed.anything:
        return Status.PRESENT if isinstance(step, Arm | Corrupt) else Status.OK
    for outcome in allowed.values:
        if isinstance(outcome, Restarted | GaveUp | Seed | Condition | Booted):
            return outcome
    for status in PREFERENCE:
        if status in allowed.values:
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


def _overwrite(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(
        Overwrite, kind=st.just("overwrite"), file=_file(model), seed=st.integers(0, 255)
    )


def _remove(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(Remove, kind=st.just("remove"), file=_file(model))


def _rename(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(
        Rename, kind=st.just("rename"), src=_file(model), dst=st.integers(0, FILES - 1)
    )


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
    choices = [writes(model), _get(model), _overwrite(model), _remove(model), _rename(model)]
    choices += [_arm(model), _corrupt(model)]
    if model.storage.armed is not None:
        choices.append(st.just(Disarm(kind="disarm")))
    if drivers := requestable(model):
        choices.append(_request(model, drivers))
    choices += _orchestration(model)
    choices += [
        st.just(Ping(kind="ping")),
        st.builds(Unplug, kind=st.just("unplug"), ms=st.integers(0, 1500)),
        st.builds(Pause, kind=st.just("pause"), ms=st.integers(0, 1000)),
    ]
    if not model.storage.damaged:  # a damaged volume may not mount again
        choices += [st.just(Reset(kind="reset")), _crash_put(model)]
    return st.one_of(choices)


RECOVERABLE: Final[tuple[Literal["blk", "eth"], ...]] = ("blk", "eth")


def _look(model: Model) -> st.SearchStrategy[Step]:
    return st.builds(Look, kind=st.just("look"), driver=st.sampled_from(DRIVERS))


def _want(model: Model) -> st.SearchStrategy[Step]:
    """A want of a driver a request can go to, flooded one time in four, or
    now and then of a gone recoverable driver, which asks nothing."""
    gone = [d for d in RECOVERABLE if not model.root.up(d)]
    drivers = requestable(model) + gone
    return st.builds(
        Want,
        kind=st.just("want"),
        driver=st.sampled_from(drivers or list(RECOVERABLE)),
        flood=st.sampled_from([False, False, False, True]),
    )


def _want_gone(model: Model) -> st.SearchStrategy[Step]:
    """A want of a recoverable driver Root gave up on, which asks nothing."""
    gone = [d for d in RECOVERABLE if not model.root.up(d)] or list(RECOVERABLE)
    return st.builds(Want, kind=st.just("want"), driver=st.sampled_from(gone), flood=st.just(False))


def _absorb(model: Model) -> st.SearchStrategy[Step]:
    up = [d for d in RECOVERABLE if model.root.up(d)]
    return st.builds(
        Absorb, kind=st.just("absorb"), driver=st.sampled_from(up or list(RECOVERABLE))
    )


def _rewant(model: Model) -> st.SearchStrategy[Step]:
    wanted = sorted(model.orch.wanted) or list(DRIVERS)
    return st.builds(Rewant, kind=st.just("rewant"), driver=st.sampled_from(wanted))


def _orchestration(model: Model) -> list[st.SearchStrategy[Step]]:
    """The orchestrator's steps that can matter from `model`."""
    choices = [_look(model)]
    if requestable(model) or not all(model.root.up(d) for d in RECOVERABLE):
        choices.append(_want(model))
    if any(model.root.up(d) for d in RECOVERABLE):
        choices.append(_absorb(model))
    if model.orch.wanted:
        choices.append(_rewant(model))
    return choices


def _in_window_look(model: Model) -> st.SearchStrategy[Step]:
    """A look or want at a driver still recovering, so it lands in the window."""
    recovering = [d for d in DRIVERS if model.root.recovering(d)]
    if not recovering:
        return _look(model)
    return st.one_of(
        st.builds(Look, kind=st.just("look"), driver=st.sampled_from(recovering)),
        st.builds(
            Want, kind=st.just("want"), driver=st.sampled_from(recovering), flood=st.just(False)
        ),
    )


def _reset_or_look(model: Model) -> st.SearchStrategy[Step]:
    """A reset, unless the volume is damaged and might not mount again."""
    return _look(model) if model.storage.damaged else st.just(Reset(kind="reset"))


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


def until(goal: Goal, toward: Toward) -> Scenario:
    """Steps from `toward` alone until `goal` holds, at most the budget plus
    one, which is enough for faults to drive any driver past its budget."""

    def run(draw: st.DrawFn, model: Model) -> tuple[list[Step], Model]:
        steps: list[Step] = []
        for _ in range(model.root.budget + 1):
            if goal(model):
                break
            step = draw(toward(model))
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


def free(model: Model) -> list[int]:
    """Files a rename can take as its target."""
    return [f for f in range(FILES) if model.storage.file(f).at == "absent"]


def _moved(model: Model) -> st.SearchStrategy[Step]:
    """A rename of a present file onto a free name, or a write that makes one."""
    targets = free(model)

    def rename(src: int) -> st.SearchStrategy[Step]:
        dst = st.sampled_from([f for f in targets if f != src] or [src])
        return st.builds(Rename, kind=st.just("rename"), src=st.just(src), dst=dst)

    return _aimed(model, rename)


def _removed(model: Model) -> st.SearchStrategy[Step]:
    return _aimed(model, lambda f: st.just(Remove(kind="remove", file=f)))


def _overwritten(model: Model) -> st.SearchStrategy[Step]:
    return _aimed(
        model,
        lambda f: st.builds(
            Overwrite, kind=st.just("overwrite"), file=st.just(f), seed=st.integers(0, 255)
        ),
    )


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
    # A latent read error that whole-sector overwrites must step around.
    "latent-overwrite": chain(
        reach(has_file, writes, 4),
        then(_read_fault_on_data),
        then(_overwritten),
        _repeat(then(_workload), 2),
    ),
    # A latent error carried by a rename, then a remove and a reset.
    "rename-remove": chain(
        reach(has_file, writes, 4),
        then(_read_fault_on_data),
        then(_moved),
        _repeat(then(_workload), 2),
        then(_removed),
        then(lambda _: st.just(Reset(kind="reset"))),
        _repeat(then(_workload), 2),
    ),
    "mixed": anything(12),
    # The orchestrator's parts, sized to fit MAX_STEPS at the shipped budget
    # of 8: a want, a look or want inside a recovery window, a fault inside a
    # want, eth driven past its budget by faults alone and then wanted, and a
    # reset with the rewant after it.
    "orchestrate": chain(
        then(_want),
        then(faults_to(RECOVERABLE, settle=False)),
        then(_in_window_look),
        then(_absorb),
        until(gone("eth"), faults_to(("eth",))),
        then(_want_gone),
        then(_reset_or_look),
        then(_rewant),
    ),
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
