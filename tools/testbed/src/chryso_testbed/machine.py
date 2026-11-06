"""One QEMU guest: its process, serial console and QMP connection.

The flags match the Microkit image's needs: the image is loaded at
0x70000000, and the drivers expect the disk on virtio-mmio bus 1 and the NIC
on bus 0. Writes go to a qcow2 overlay over the read-only FAT image, so a
boot costs no copy. A profile's `blkdebug` rules go to a blkdebug node under
both the overlay and the FAT image, so they see every guest data read and
write (`blkdebug.Plan`, dumped in QEMU's `inject-error` field names). The
overlay and QMP socket are scratch, removed on exit.

`arm` swaps a blkdebug node in above the overlay with `blockdev-reopen`, so
runtime rules see guest sectors. `copy_disk` and `overwrite` go through an
NBD export, since HMP's qemu-io would print on the serial console.
Only the console logs and QEMU's stderr stay in the work directory.

The testbed listens on the QMP socket and QEMU connects to it, so nothing
polls for the socket. A SHUTDOWN, RESET or STOP event fails the console's
waits at once, except the STOP of `paused()`. With -no-reboot a guest reset
arrives as SHUTDOWN.

The guest runs in its own process group, and leaving the context always
SIGKILLs and reaps it, even on cancellation.
"""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import json
import os
import shutil
import signal
import tempfile
from collections import Counter
from collections.abc import AsyncIterator, Iterable
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import Final, Self

import structlog
from qemu.qmp import EventListener, QMPClient

from chryso_testbed.blkdebug import PLAN, RUNTIME, Plan, SectorError
from chryso_testbed.console import Console

log = structlog.get_logger(__name__)
LOAD_ADDRESS: Final = "0x70000000"
QMP_TIMEOUT: Final = 30.0
FATAL_EVENTS: Final = ("SHUTDOWN", "RESET", "STOP", "GUEST_PANICKED")
NETDEV: Final = "net0"
SECTOR: Final = 512


@dataclass(frozen=True, slots=True)
class Tools:
    qemu: str
    qemu_img: str

    @property
    def qemu_io(self) -> str:
        """qemu-io ships beside qemu-img."""
        return str(Path(self.qemu_img).with_name("qemu-io"))


@dataclass(frozen=True, slots=True)
class Profile:
    image: Path
    disk: Path
    hostfwd: tuple[tuple[int, int], ...] = ()
    extra: tuple[str, ...] = ()
    blkdebug: Plan = ()
    # Keep a blklogwrites log of every guest write (`crashlog.py` reads it).
    crashlog: bool = False
    # Let a guest reset reboot instead of ending the run (`Machine.reset`).
    reboot: bool = False

    @property
    def base(self) -> str:
        """The node under `top` that runtime faults wrap."""
        if self.blkdebug:
            return "planned"
        return "logged" if self.crashlog else "hd"


def drive(profile: Profile, overlay: Path, log: Path | None = None) -> list[str]:
    """The disk, bottom up: a qcow2 overlay over the read-only FAT image, the
    write log, the plan, and the raw `top` node the device reads, which
    `Machine.arm` re-points at a blkdebug node. The plan sits above qcow2 so
    it faults guest I/O only, never qcow2's metadata writeback, and above the
    log so a failed write never reaches it."""
    file = {"driver": "file", "filename": str(overlay)}
    image = {"driver": "file", "filename": str(profile.disk), "read-only": True}
    nodes: list[dict[str, object]] = [{
        "driver": "qcow2", "node-name": "hd", "file": file,
        "backing": {"driver": "raw", "file": image},
    }]  # fmt: skip
    below = "hd"
    if profile.crashlog:
        if log is None:
            raise ValueError("a crashlog profile needs a log file")
        nodes.append({
            "driver": "blklogwrites", "node-name": "logged", "file": below,
            "log": {"driver": "file", "filename": str(log)}, "log-sector-size": SECTOR,
        })  # fmt: skip
        below = "logged"
    if profile.blkdebug:
        nodes.append({
            "driver": "blkdebug", "node-name": "planned", "image": below,
            "inject-error": PLAN.dump_python(profile.blkdebug),
        })  # fmt: skip
    nodes.append({"driver": "raw", "node-name": "top", "file": profile.base})
    return [arg for n in nodes for arg in ("-blockdev", json.dumps(n))]


def argv(
    tools: Tools, profile: Profile, disk: Path, qmp: Path, log: Path | None = None
) -> list[str]:
    netdev = f"user,id={NETDEV}" + "".join(f",hostfwd=tcp::{h}-:{g}" for h, g in profile.hostfwd)
    return [
        tools.qemu,
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53",
        "-m", "size=2G",
        "-device", f"loader,file={profile.image / 'sel4-beam.img'},addr={LOAD_ADDRESS},cpu-num=0",
        "-global", "virtio-mmio.force-legacy=false",
        *drive(profile, disk, log),
        "-device", "virtio-blk-device,drive=top,share-rw=on,bus=virtio-mmio-bus.1",
        "-device", f"virtio-net-device,netdev={NETDEV},bus=virtio-mmio-bus.0",
        "-netdev", netdev,
        "-chardev", f"socket,id=qmp,path={qmp}",
        "-object", "monitor-qmp,id=qmp-monitor,chardev=qmp",
        "-serial", "stdio",
        "-display", "none",
        "-monitor", "none",
        *(() if profile.reboot else ("-no-reboot",)),
        "-d", "guest_errors",
        *profile.extra,
    ]  # fmt: skip


async def run(*cmd: str) -> str:
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
    )
    out, _ = await proc.communicate()
    text = out.decode(errors="replace")
    if proc.returncode:
        raise RuntimeError(f"{cmd[0]} exited {proc.returncode}: {text}")
    return text


class Machine:
    def __init__(
        self, name: str, profile: Profile, workdir: Path, tools: Tools, failures: Iterable[str]
    ) -> None:
        self.name = name
        self.profile = profile
        self.workdir = workdir
        self.tools = tools
        self.console = Console(failures)
        self.qmp = QMPClient(name)
        self._proc: asyncio.subprocess.Process | None = None
        self._tasks: list[asyncio.Task[None]] = []
        # Events the testbed caused itself, which must not fail the run.
        self._expected = Counter[str]()
        self._armed: str | None = None
        self._arms = itertools.count(1)
        self._nbd: str | None = None
        # Disk images and logs go under workdir, never /tmp, which may be RAM.
        # Only the sockets get a short private directory there, since a unix
        # socket path must fit in 108 bytes and a Nix output path can exceed it.
        self._scratch = workdir / "scratch"
        self._sockets = Path(tempfile.mkdtemp(prefix="tb-"))

    @property
    def qmp_socket(self) -> Path:
        return self._sockets / "qmp.sock"

    @property
    def write_log(self) -> Path:
        """The blklogwrites log of a `crashlog` profile."""
        return self._scratch / "writes.log"

    def logged(self) -> int:
        """How far the write log has grown, which marks a step's end in it."""
        return self.write_log.stat().st_size

    async def reset(self) -> None:
        """Reset the whole system, as a power cycle that keeps the disk."""
        if not self.profile.reboot:
            raise RuntimeError("a reset needs a reboot profile")
        self._expected["RESET"] += 1
        await self.qmp.execute("system_reset")
        log.info("machine.reset", machine=self.name)

    async def __aenter__(self) -> Self:
        try:
            await self.start()
        except BaseException:
            await self.power_off()
            raise
        return self

    async def __aexit__(
        self,
        kind: type[BaseException] | None,
        error: BaseException | None,
        trace: TracebackType | None,
    ) -> None:
        await self.power_off()

    async def start(self) -> None:
        self._scratch.mkdir(parents=True, exist_ok=True)
        disk = self._scratch / "disk.qcow2"
        # No backing in the header: -blockdev names it, with any blkdebug node.
        size = str(self.profile.disk.stat().st_size)
        await run(self.tools.qemu_img, "create", "-q", "-f", "qcow2", str(disk), size)
        writes = self.write_log if self.profile.crashlog else None
        if writes is not None:
            writes.touch()
        await self.qmp.start_server(str(self.qmp_socket))
        with (self.workdir / "qemu.stderr").open("wb") as stderr:
            self._proc = await asyncio.create_subprocess_exec(
                *argv(self.tools, self.profile, disk, self.qmp_socket, writes),
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=stderr,
                start_new_session=True,
            )
        stdin, stdout = self._proc.stdin, self._proc.stdout
        if stdin is None or stdout is None:
            raise RuntimeError("QEMU started without console pipes")
        log.info("machine.start", machine=self.name, pid=self._proc.pid, workdir=str(self.workdir))
        self.console.attach(stdin)
        self._tasks.append(asyncio.create_task(self.console.pump(stdout)))
        await self._connected(self._proc)
        self._tasks.append(asyncio.create_task(self._watch()))

    async def _connected(self, proc: asyncio.subprocess.Process) -> None:
        """Wait for QEMU's QMP connection, failing at once if QEMU exits first."""
        accept = asyncio.create_task(self.qmp.accept())
        exited = asyncio.create_task(proc.wait())
        try:
            done, _ = await asyncio.wait(
                {accept, exited}, timeout=QMP_TIMEOUT, return_when=asyncio.FIRST_COMPLETED
            )
        finally:
            for task in (accept, exited):
                task.cancel()
            await asyncio.gather(accept, exited, return_exceptions=True)
        if accept in done:
            accept.result()
            return
        stderr = (self.workdir / "qemu.stderr").read_text(errors="replace")[-2000:]
        if exited in done:
            raise self.console.fail(
                f"QEMU exited with status {exited.result()} before QMP connected\n{stderr}"
            )
        raise self.console.fail(f"QEMU did not connect to QMP within {QMP_TIMEOUT:g}s\n{stderr}")

    async def _watch(self) -> None:
        listener = EventListener(FATAL_EVENTS)
        with self.qmp.listen(listener):
            async for event in listener:
                if self._expected[name := str(event["event"])]:
                    self._expected[name] -= 1
                    log.debug("qmp.stop.expected", machine=self.name)
                else:
                    log.warning("qmp.fatal", machine=self.name, qmp_event=event["event"])
                    await self.console.report(f"QEMU {event['event']} {event.get('data', {})}")
                    return

    async def arm(self, rules: tuple[SectorError, ...]) -> None:
        """Put `rules` between the device and the disk, replacing any armed."""
        name = f"dbg{next(self._arms)}"
        await self.qmp.execute(
            "blockdev-add",
            {"driver": "blkdebug", "node-name": name, "image": self.profile.base,
             "inject-error": RUNTIME.dump_python(rules)},
        )  # fmt: skip
        await self._point_top_at(name)
        log.info("disk.armed", machine=self.name, rules=RUNTIME.dump_python(rules))

    async def disarm(self) -> None:
        """Take the armed rules out, if any."""
        if self._armed is None:
            return
        await self._point_top_at(self.profile.base)
        log.info("disk.disarmed", machine=self.name)

    async def _point_top_at(self, node: str) -> None:
        await self.qmp.execute(
            "blockdev-reopen", {"options": [{"driver": "raw", "node-name": "top", "file": node}]}
        )
        if self._armed is not None:
            await self.qmp.execute("blockdev-del", {"node-name": self._armed})
        self._armed = node if node != self.profile.base else None

    async def export(self) -> str:
        """An NBD URI for the overlay, writable, served on a scratch socket."""
        if self._nbd is None:
            socket = self._sockets / "nbd.sock"
            await self.qmp.execute(
                "nbd-server-start", {"addr": {"type": "unix", "data": {"path": str(socket)}}}
            )
            await self.qmp.execute(
                "block-export-add",
                {"type": "nbd", "id": "disk", "node-name": "hd", "name": "hd", "writable": True},
            )
            self._nbd = f"nbd+unix:///hd?socket={socket}"
        return self._nbd

    async def copy_disk(self) -> Path:
        """The disk as the guest would read it now, as a raw scratch file."""
        copy = self._scratch / "disk.raw"
        copy.unlink(missing_ok=True)
        await run(self.tools.qemu_img, "convert", "-f", "raw", "-O", "raw",
                  await self.export(), str(copy))  # fmt: skip
        return copy

    async def overwrite(self, sector: int, count: int, byte: int) -> None:
        """Fill `count` sectors from `sector` with `byte`, behind the guest's back."""
        await run(self.tools.qemu_io, "-f", "raw", await self.export(),
                  "-c", f"write -P {byte} {sector * 512} {count * 512}")  # fmt: skip
        log.info("disk.overwritten", machine=self.name, sector=sector, count=count, byte=byte)

    @contextlib.asynccontextmanager
    async def paused(self) -> AsyncIterator[None]:
        """Hold the VM stopped for the body. Guest virtual time stops with it."""
        self._expected["STOP"] += 1
        await self.qmp.execute("stop")
        try:
            yield
        finally:
            await self.qmp.execute("cont")

    @contextlib.asynccontextmanager
    async def unplugged(self) -> AsyncIterator[None]:
        """Hold the NIC's link down for the body, as a pulled cable would."""
        await self.qmp.execute("set_link", {"name": NETDEV, "up": False})
        try:
            yield
        finally:
            await self.qmp.execute("set_link", {"name": NETDEV, "up": True})

    async def power_off(self) -> None:
        """SIGKILL the guest and reap it. Safe to call more than once."""
        if self._proc is not None and self._proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self._proc.pid, signal.SIGKILL)
            await self._proc.wait()
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        with contextlib.suppress(Exception):
            await self.qmp.disconnect()
        if self.workdir.is_dir():
            (self.workdir / "console.log").write_bytes(self.console.text())
            (self.workdir / "console.raw").write_bytes(self.console.raw())
        shutil.rmtree(self._scratch, ignore_errors=True)
        shutil.rmtree(self._sockets, ignore_errors=True)
