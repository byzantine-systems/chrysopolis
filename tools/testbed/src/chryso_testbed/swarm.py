"""The fuzz as parallel pytest workers, each with its own Hypothesis seed and
work directory, sharing the example database. An example is mostly a boot,
so throughput grows with the worker count until cores or memory run out.
"""

from __future__ import annotations

import asyncio
import os
import random
import re
import signal
import sys
from collections.abc import Sequence
from pathlib import Path
from time import perf_counter
from typing import Final

import structlog

from chryso_testbed import log as logging_

log = structlog.get_logger()

PYTEST: Final = (sys.executable, "-m", "pytest")
RAM_BACKED: Final = frozenset({"tmpfs", "ramfs"})
GRACE: Final = 30.0  # seconds a stopped worker gets to power its machine off


class WorkerFailed(Exception):
    def __init__(self, worker: int, code: int, output: Path) -> None:
        super().__init__(f"worker {worker} exited {code}, output in {output}")
        self.worker, self.code, self.output = worker, code, output


def filesystem(path: Path, mountinfo: str) -> str:
    """The type of the filesystem holding `path`, an absolute path, from the
    text of /proc/self/mountinfo: the mount whose point is its longest prefix."""
    best, kind = -1, "unknown"
    for line in mountinfo.splitlines():
        fields, _, rest = line.partition(" - ")
        point = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), fields.split()[4])
        if path.is_relative_to(point) and len(point) > best:
            best, kind = len(point), rest.split()[0]
    return kind


async def worker(n: int, command: Sequence[str], args: Sequence[str], out: Path, seed: int) -> None:
    """Run worker `n` to completion, raising WorkerFailed on a nonzero exit.
    Cancelled, it gets SIGINT so pytest unwinds and powers QEMU off, then its
    whole session is killed after GRACE seconds."""
    output = out / f"w{n}.log"
    started = perf_counter()
    with output.open("wb") as sink:
        proc = await asyncio.create_subprocess_exec(
            *command, *args, f"--basetemp={out / f'w{n}'}", f"--hypothesis-seed={seed + n}",
            stdout=sink, stderr=asyncio.subprocess.STDOUT, start_new_session=True,
        )  # fmt: skip
        try:
            code = await proc.wait()
        except asyncio.CancelledError:
            proc.send_signal(signal.SIGINT)
            try:
                async with asyncio.timeout(GRACE):
                    await asyncio.shield(proc.wait())
            except TimeoutError:
                os.killpg(proc.pid, signal.SIGKILL)
                await proc.wait()
            raise
    log.info("swarm.worker", worker=n, code=code, seconds=round(perf_counter() - started, 1))
    if code:
        raise WorkerFailed(n, code, output)


async def swarm(
    args: Sequence[str], workers: int, out: Path, seed: int, command: Sequence[str] = PYTEST
) -> list[WorkerFailed]:
    """Run `workers` workers with their output in `out`, which must exist.
    The first to fail stops the rest, and the failures come back."""
    log.info("swarm.start", workers=workers, seed=seed, out=str(out))
    failures: list[WorkerFailed] = []
    try:
        async with asyncio.TaskGroup() as group:
            for n in range(workers):
                group.create_task(worker(n, command, args, out, seed))
    except* WorkerFailed as failed:
        failures += [e for e in failed.exceptions if isinstance(e, WorkerFailed)]
    return failures


def main() -> None:
    """Pytest's own arguments pass through. CHRYSO_FUZZ_WORKERS (default 1,
    which execs pytest as is), CHRYSO_FUZZ_OUT (default fuzz-out) and
    CHRYSO_FUZZ_SEED (default random) shape the swarm."""
    args = sys.argv[1:]
    workers = int(os.environ.get("CHRYSO_FUZZ_WORKERS", "1"))
    if workers <= 1:
        os.execv(PYTEST[0], [*PYTEST, *args])
    if any(a.startswith(("--basetemp", "--hypothesis-seed")) for a in args):
        sys.exit("swarm: each worker gets its own --basetemp and --hypothesis-seed")
    out = Path(os.environ.get("CHRYSO_FUZZ_OUT", "fuzz-out")).resolve()
    if (kind := filesystem(out, Path("/proc/self/mountinfo").read_text())) in RAM_BACKED:
        sys.exit(f"swarm: {out} is on {kind}, which is RAM; set CHRYSO_FUZZ_OUT to a disk")
    seed = int(os.environ.get("CHRYSO_FUZZ_SEED") or random.SystemRandom().randrange(2**32))
    out.mkdir(parents=True, exist_ok=True)
    logging_.configure()
    failures = asyncio.run(swarm(args, workers, out, seed))
    for failure in failures:
        sys.stderr.write(f"\n===== {failure} =====\n")
        sys.stderr.write(failure.output.read_text(errors="replace")[-20_000:])
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
