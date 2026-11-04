"""chryso-testbed: run scenario groups, or boot one image by hand.

The manifest is JSON written by Nix: image directories by name, the FAT disk,
system-abi.json, and the qemu and qemu-img binaries.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import sys
from pathlib import Path
from typing import Annotated

import structlog
import typer
from rich.console import Console as Terminal
from rich.table import Table

from chryso_testbed import log as logging_setup
from chryso_testbed import scenarios
from chryso_testbed.abi import Abi
from chryso_testbed.machine import Machine, Profile, Tools
from chryso_testbed.runner import Result, run, write_report
from chryso_testbed.scenario import Settings, registry

app = typer.Typer(no_args_is_help=True, help="QEMU scenarios for the Nix-built images.")
terminal = Terminal(stderr=True)
log = structlog.get_logger(__name__)


@app.callback()
def main(
    log_format: Annotated[
        logging_setup.Format, typer.Option(help="Console on a terminal, JSON lines otherwise.")
    ] = logging_setup.Format.AUTO,
) -> None:
    """Configure logging before any command runs."""
    logging_setup.configure(log_format)


Manifest = Annotated[
    Path,
    typer.Option(
        envvar="CHRYSO_TESTBED_MANIFEST", exists=True, dir_okay=False, help="Nix-built manifest."
    ),
]


def load(manifest: Path, out: Path, jobs: int) -> Settings:
    raw = json.loads(manifest.read_text())
    return Settings(
        images={name: Path(path) for name, path in raw["images"].items()},
        disk=Path(raw["disk"]),
        abi=Abi.load(Path(raw["abi"])),
        tools=Tools(raw["qemu"], raw["qemu_img"]),
        out=out,
        jobs=jobs,
        test_modules=tuple(raw["test_modules"]),
        fallback_bound_us=int(raw["fallback_bound_us"]),
    )


def show(results: list[Result]) -> None:
    table = Table("status", "scenario", "group", "seconds")
    for r in results:
        style = "green" if r.status == "pass" else "red"
        table.add_row(f"[{style}]{r.status}[/]", r.name, r.group, f"{r.seconds:.1f}")
    terminal.print(table)
    for r in results:
        if r.status != "pass":
            terminal.rule(f"[red]{r.name}")
            terminal.print(r.message, markup=False, highlight=False)


@app.command("run")
def run_command(
    manifest: Manifest,
    group: Annotated[str | None, typer.Option(help="Run every scenario in this group.")] = None,
    name: Annotated[
        list[str] | None, typer.Option("--name", "-k", help="Run this scenario.")
    ] = None,
    out: Annotated[Path, typer.Option(help="Reports and consoles go here.")] = Path("testbed-out"),
    jobs: Annotated[int, typer.Option(min=1, help="Scenarios run at once.")] = os.cpu_count() or 1,
) -> None:
    """Run a group or named scenarios and write report.json and junit.xml."""
    scenarios.load()
    chosen = registry.select(group=group, names=name or ())
    if not chosen:
        raise typer.BadParameter("no scenarios selected")
    results = asyncio.run(run(chosen, load(manifest, out, jobs)))
    write_report(results, out)
    show(results)
    raise typer.Exit(0 if all(r.status == "pass" for r in results) else 1)


@app.command("list")
def list_command() -> None:
    """List every scenario with its group."""
    scenarios.load()
    table = Table("group", "scenario", "exclusive")
    for s in registry.select():
        table.add_row(s.group, s.name, "yes" if s.exclusive else "")
    Terminal().print(table)


@app.command()
def shell(
    manifest: Manifest,
    image: Annotated[str, typer.Argument(help="Image name from the manifest.")],
) -> None:
    """Boot one image, mirror its console and forward typed lines until EOF."""
    asyncio.run(interact(load(manifest, Path("testbed-shell"), 1), image))


async def interact(settings: Settings, image: str) -> None:
    profile = Profile(settings.images[image], settings.disk)
    async with Machine("shell", profile, settings.out, settings.tools, ()) as machine:
        log.info("shell.ready", qmp_socket=str(machine.qmp_socket))
        reader = asyncio.StreamReader()
        await asyncio.get_running_loop().connect_read_pipe(
            lambda: asyncio.StreamReaderProtocol(reader), sys.stdin
        )

        async def mirror() -> None:
            async for chunk in machine.console.follow():
                sys.stdout.buffer.write(chunk)
                sys.stdout.flush()

        task = asyncio.create_task(mirror())
        try:
            while line := await reader.readline():
                await machine.console.send(line.decode().rstrip("\n"))
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task


if __name__ == "__main__":
    app()
