"""One QEMU guest: its process, serial console and QMP connection.

The flags match the Microkit image's needs: the image is loaded at
0x70000000, and the drivers expect the disk on virtio-mmio bus 1 and the NIC
on bus 0. Writes go to a qcow2 overlay over the read-only FAT image, so a
boot costs no copy. The overlay and QMP socket are scratch, removed on exit.
Only the console logs and QEMU's stderr stay in the work directory.

The testbed listens on the QMP socket and QEMU connects to it, so nothing
polls for the socket. A SHUTDOWN, RESET or STOP event fails the console's
waits at once. With -no-reboot a guest reset arrives as SHUTDOWN.

The guest runs in its own process group, and leaving the context always
SIGKILLs and reaps it, even on cancellation.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import shutil
import signal
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import Final, Self

from qemu.qmp import EventListener, QMPClient

from chryso_testbed.console import Console

LOAD_ADDRESS: Final = "0x70000000"
QMP_TIMEOUT: Final = 30.0
FATAL_EVENTS: Final = ("SHUTDOWN", "RESET", "STOP", "GUEST_PANICKED")


@dataclass(frozen=True, slots=True)
class Tools:
    qemu: str
    qemu_img: str


@dataclass(frozen=True, slots=True)
class Profile:
    image: Path
    disk: Path
    hostfwd: tuple[tuple[int, int], ...] = ()
    extra: tuple[str, ...] = ()


def argv(tools: Tools, profile: Profile, disk: Path, qmp: Path) -> list[str]:
    netdev = "user,id=net0" + "".join(f",hostfwd=tcp::{h}-:{g}" for h, g in profile.hostfwd)
    return [
        tools.qemu,
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53",
        "-m", "size=2G",
        "-device", f"loader,file={profile.image / 'sel4-beam.img'},addr={LOAD_ADDRESS},cpu-num=0",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={disk},if=none,format=qcow2,id=hd",
        "-device", "virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1",
        "-device", "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0",
        "-netdev", netdev,
        "-chardev", f"socket,id=qmp,path={qmp}",
        "-object", "monitor-qmp,id=qmp-monitor,chardev=qmp",
        "-serial", "stdio",
        "-display", "none",
        "-monitor", "none",
        "-no-reboot",
        "-d", "guest_errors",
        *profile.extra,
    ]  # fmt: skip


async def run(*cmd: str) -> None:
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
    )
    out, _ = await proc.communicate()
    if proc.returncode:
        raise RuntimeError(f"{cmd[0]} exited {proc.returncode}: {out.decode(errors='replace')}")


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
        # Scratch lives in a short private directory: a unix socket path must
        # fit in 108 bytes, which a Nix output path can exceed.
        self._scratch = Path(tempfile.mkdtemp(prefix="tb-"))

    @property
    def qmp_socket(self) -> Path:
        return self._scratch / "qmp.sock"

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
        self.workdir.mkdir(parents=True, exist_ok=True)
        disk = self._scratch / "disk.qcow2"
        await run(
            self.tools.qemu_img, "create", "-q", "-f", "qcow2",
            "-b", str(self.profile.disk), "-F", "raw", str(disk),
        )  # fmt: skip
        await self.qmp.start_server(str(self.qmp_socket))
        with (self.workdir / "qemu.stderr").open("wb") as stderr:
            self._proc = await asyncio.create_subprocess_exec(
                *argv(self.tools, self.profile, disk, self.qmp_socket),
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=stderr,
                start_new_session=True,
            )
        stdin, stdout = self._proc.stdin, self._proc.stdout
        if stdin is None or stdout is None:
            raise RuntimeError("QEMU started without console pipes")
        self.console.attach(stdin)
        self._tasks.append(asyncio.create_task(self.console.pump(stdout)))
        async with asyncio.timeout(QMP_TIMEOUT):
            await self.qmp.accept()
        self._tasks.append(asyncio.create_task(self._watch()))

    async def _watch(self) -> None:
        listener = EventListener(FATAL_EVENTS)
        with self.qmp.listen(listener):
            event = await listener.get()
        await self.console.report(f"QEMU {event['event']} {event.get('data', {})}")

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
