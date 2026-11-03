"""Steps shared by scenarios: the Erlang shell and host-side TCP peers.

Probe tags are typed as lower-case atoms and printed upper-case by the
probe, so a wait never matches the echo of the line typed to start it.
"""

from __future__ import annotations

import asyncio
from collections.abc import Sequence
from typing import Final

from chryso_testbed.console import Hit, Mark
from chryso_testbed.machine import Machine

PROMPT: Final = r"\d+> "
TEST_EBIN: Final = "/lib/chryso_test/ebin"
CORE_EBIN: Final = "/lib/chryso_core/ebin"


async def shell_ready(machine: Machine, banner: Hit) -> None:
    """Wait for the first prompt after `banner`, so typed input is not lost."""
    await machine.console.wait(PROMPT, since=machine.console.at(banner.end), partial=True)


async def load_test_modules(machine: Machine, modules: Sequence[str], banner: Hit) -> None:
    """Load the probe modules off the FAT disk and require every one to load.

    ERTS boots in embedded mode, so a module is loaded by name or not at all.
    chryso_test loads first by hand because it loads the rest.
    """
    await shell_ready(machine, banner)
    mark = machine.console.mark()
    await machine.console.send(
        f'code:add_patha("{TEST_EBIN}"), code:add_patha("{CORE_EBIN}"), '
        "{module,chryso_test}=code:load_file(chryso_test), "
        f"chryso_test:loaded([{','.join(modules)}])."
    )
    report = await machine.console.wait(r"TEST_MODULES\|[^\r\n]*", since=mark)
    if not report.text.startswith("TEST_MODULES|ok"):
        raise machine.console.fail(f"probes failed to load: {report.text}")


async def call(machine: Machine, expression: str, expect: str) -> Hit:
    """Type `expression` and wait for `expect` in what follows it."""
    mark: Mark = machine.console.mark()
    await machine.console.send(expression)
    return await machine.console.wait(expect, since=mark)


def require_absent(machine: Machine, pattern: str, why: str) -> None:
    if (hit := machine.console.search(pattern)) is not None:
        raise machine.console.fail(f"{why}: {hit.text}")


def require_present(machine: Machine, text: str, why: str) -> None:
    if text.encode() not in machine.console.text():
        raise machine.console.fail(f"{why}: {text!r} never appeared")


# --- host-side TCP peers ---------------------------------------------------------


async def echo(port: int, payload: bytes, *, attempts: int = 30) -> bytes:
    """Send `payload` to the guest's echo server through hostfwd and read it back.

    The guest's listener is up before this runs, but the first connects can
    still land before slirp and lwIP finish the handshake path, so retry.
    """
    last: Exception | None = None
    for _ in range(attempts):
        try:
            async with asyncio.timeout(3):
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                try:
                    writer.write(payload)
                    await writer.drain()
                    return await reader.readexactly(len(payload))
                finally:
                    writer.close()
        except (OSError, asyncio.IncompleteReadError) as err:
            last = err
        await asyncio.sleep(1)
    raise AssertionError(f"host to guest echo failed after {attempts} attempts: {last}")


class Listener:
    """A host listener the guest dials at 10.0.2.2, keeping the first payload."""

    def __init__(self, port: int, size: int) -> None:
        self.port = port
        self._size = size
        self._got: asyncio.Future[bytes] = asyncio.get_running_loop().create_future()
        self._server: asyncio.Server | None = None

    async def __aenter__(self) -> Listener:
        self._server = await asyncio.start_server(self._accept, "127.0.0.1", self.port)
        return self

    async def __aexit__(self, *_: object) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    async def _accept(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            data = await reader.readexactly(self._size)
            if not self._got.done():
                self._got.set_result(data)
        except asyncio.IncompleteReadError as err:
            if not self._got.done():
                self._got.set_exception(
                    AssertionError(f"guest sent {err.partial!r}, expected {self._size} bytes")
                )
        finally:
            writer.close()

    async def received(self) -> bytes:
        return await self._got
