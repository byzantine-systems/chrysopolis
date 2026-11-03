"""Steps shared by scenarios: the Erlang shell and host-side TCP peers.

Probe tags are typed as lower-case atoms and printed upper-case by the
probe, so a wait never matches the echo of the line typed to start it.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Sequence
from typing import Final

from chryso_testbed import frames
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


# --- Root's fault path -----------------------------------------------------------


def _first(machine: Machine, pattern: str, since: Mark | None) -> int:
    hit = machine.console.search(pattern, since)
    if hit is None:
        raise machine.console.fail(f"missing {pattern!r}")
    return hit.start


def assert_fault_event(machine: Machine, child: int, count: int, since: Mark) -> None:
    """An injected fault reached Root and caused restart `count`, in that order."""
    inject = _first(machine, rf"ROOT\|fault-inject\|child={child}\b", since)
    fault = _first(machine, rf"ROOT\|fault\|child={child}(?:\||\r?$)", since)
    restart = _first(machine, rf"ROOT\|restart\|child={child}\|count={count}\b", since)
    if not inject < fault < restart:
        raise machine.console.fail(f"child {child}: injection, fault and restart out of order")


def assert_fault_sequence(machine: Machine, child: int, count: int) -> None:
    """The nth injection and the nth genuine fault both precede restart n."""
    injections = machine.console.findall(rf"ROOT\|fault-inject\|child={child}\b")
    faults = machine.console.findall(rf"ROOT\|fault\|child={child}(?:\||\r?$)")
    restart = _first(machine, rf"ROOT\|restart\|child={child}\|count={count}\b", None)
    if len(injections) < count or len(faults) < count:
        raise machine.console.fail(
            f"child {child}: {len(injections)} injections, {len(faults)} faults, want {count}"
        )
    if not injections[count - 1].start < faults[count - 1].start < restart:
        raise machine.console.fail(f"child {child}: fault {count} out of order")


# --- guest to host TCP -----------------------------------------------------------


async def ping(machine: Machine, tag: str, port: int) -> None:
    """The guest dials the host at `port` and both ends must see the tag."""
    expected = tag.upper().encode()
    try:
        async with Listener(port, len(expected)) as listener:
            mark = machine.console.mark()
            await machine.console.send(f"chryso_net:ping('{tag}', {port}).")
            got = await listener.received()
        if got != expected:
            raise machine.console.fail(f"{tag}: host received {got!r}")
        await machine.console.wait(rf"NET_OK\|{tag.upper()}\b", since=mark)
    except AssertionError:
        await postmortem(machine, tag, port)
        raise


async def postmortem(machine: Machine, tag: str, port: int) -> None:
    """Ask for a second, unrelated connection to see whether transmit still works.

    A prompt refusal means a SYN went out and a RST came back, so only the
    stuck connection was lost. Silence means the path itself is wedged. This
    is a diagnostic and never raises.
    """
    again = f"{tag}_again"
    with contextlib.suppress(AssertionError, TimeoutError, OSError):
        async with asyncio.timeout(30):
            mark = machine.console.mark()
            await machine.console.send(f"chryso_net:ping('{again}', {port + 100}).")
            await machine.console.wait(rf"NET_(?:OK|ERR)\|{again.upper()}", since=mark)


# --- framed probe runs -----------------------------------------------------------


async def run(machine: Machine, expression: str, run_name: str) -> frames.Pass:
    """Type a probe call and read back its framed run."""
    mark = machine.console.mark()
    await machine.console.send(expression)
    return await machine.console.await_run(run_name, since=mark)


def records(result: frames.Pass, kind: str) -> list[frames.Frame]:
    return [f for f in result.frames if f.kind == kind]


async def run_probe(
    machine: Machine, expression: str, run_name: str, steps: Sequence[str]
) -> frames.Pass:
    """A step probe: every named step must report ok and none unexpected."""
    result = await run(machine, expression, run_name)
    seen = {f.get("name"): f.get("verdict") for f in records(result, "step")}
    bad = {name: verdict for name, verdict in seen.items() if verdict != "ok"}
    missing = [step for step in steps if step not in seen]
    if bad or missing or result.unexpected:
        raise machine.console.fail(f"{run_name}: unexpected={bad} missing={missing}")
    return result


def assert_audit(machine: Machine, result: frames.Pass) -> frames.Frame:
    """The in-situ audit agreed with Root: rows and tallies both match."""
    audits = records(result, "audit")
    if len(audits) != 1:
        raise machine.console.fail(f"expected one audit record, got {audits!r}")
    audit = audits[0]
    fields = dict(audit.fields)
    if fields.get("rows") != "match" or fields.get("tallies") != "match":
        rows = [dict(r.fields) for r in records(result, "row")]
        raise machine.console.fail(f"audit disagreed with Root: {fields} rows={rows}")
    return audit
