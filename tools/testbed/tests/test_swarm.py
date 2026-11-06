"""Laws of the fuzz swarm. Planted defects: matching the first mount instead of
the longest fails test_the_filesystem_is_the_innermost_mount, and so does
skipping the octal unescape, and a swarm that waits for every worker fails
test_a_failing_worker_stops_the_others."""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path
from time import perf_counter

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from chryso_testbed.swarm import filesystem, swarm

segments = st.sampled_from(["a", "b", "nix", "tmp", "my dir"])
points = st.lists(segments, min_size=1, max_size=3).map(lambda s: "/" + "/".join(s))
kinds = st.sampled_from(["ext4", "tmpfs", "btrfs", "nfs"])


def line(n: int, point: str, kind: str) -> str:
    escaped = point.replace(" ", "\\040")
    return f"{n} 1 0:{n} / {escaped} rw,relatime shared:{n} - {kind} src{n} rw"


@given(
    table=st.dictionaries(points, kinds, max_size=6),
    root=kinds,
    path=st.lists(segments, max_size=5).map(lambda s: Path("/", *s)),
)
def test_the_filesystem_is_the_innermost_mount(
    table: dict[str, str], root: str, path: Path
) -> None:
    mounts = {"/": root, **table}
    text = "\n".join(line(n, p, k) for n, (p, k) in enumerate(mounts.items()))
    innermost = next(str(p) for p in (path, *path.parents) if str(p) in mounts)
    assert filesystem(path, text) == mounts[innermost]


FAKE = """
import sys, time
args = dict(a.split("=", 1) for a in sys.argv[1:] if a.startswith("--"))
open(args["--basetemp"] + ".seed", "w").write(args["--hypothesis-seed"])
if args["--hypothesis-seed"] == sys.argv[1]:
    sys.exit(3)
if sys.argv[2] == "slow":
    time.sleep(60)
"""


@settings(max_examples=8, deadline=None)
@given(workers=st.integers(1, 4), data=st.data())
def test_a_failing_worker_stops_the_others(
    tmp_path_factory: pytest.TempPathFactory, workers: int, data: st.DataObject
) -> None:
    """Each worker gets its own seed and directory. With one worker failing,
    exactly that failure comes back, long before the others would finish."""
    fails = data.draw(st.none() | st.integers(0, workers - 1))
    out = tmp_path_factory.mktemp("swarm")
    script = out / "fake.py"
    script.write_text(FAKE)
    seed = 1000
    marker = str(seed + fails) if fails is not None else "none"
    began = perf_counter()
    failed = asyncio.run(
        swarm([marker, "slow" if fails is not None else "fast"], workers, out, seed,
              command=(sys.executable, str(script)))
    )  # fmt: skip
    assert perf_counter() - began < 60  # the others sleep 60 s, so a wait shows
    assert [(f.worker, f.code) for f in failed] == ([] if fails is None else [(fails, 3)])
    # A worker stopped early may not have written its seed yet.
    seeds = {n: int(p.read_text()) for n in range(workers) if (p := out / f"w{n}.seed").exists()}
    assert all(s == seed + n for n, s in seeds.items())
    assert fails is not None or len(seeds) == workers
