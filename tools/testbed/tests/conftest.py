"""Shared fixtures: a fake QEMU and qemu-img wired as executables."""

from __future__ import annotations

import json
import sys
from collections.abc import Callable
from pathlib import Path

import pytest

from chryso_testbed.machine import Profile, Tools

FAKE = Path(__file__).with_name("fake_qemu.py")


def executable(path: Path, body: str) -> str:
    path.write_text(f"#!/bin/sh\n{body}\n")
    path.chmod(0o755)
    return str(path)


@pytest.fixture
def tools(tmp_path: Path) -> Tools:
    qemu = executable(tmp_path / "qemu", f'exec "{sys.executable}" "{FAKE}" "$@"')
    # The overlay path is qemu-img's last argument.
    qemu_img = executable(tmp_path / "qemu-img", 'for last; do :; done\n: > "$last"')
    return Tools(qemu, qemu_img)


@pytest.fixture
def profile(tmp_path: Path) -> Profile:
    image = tmp_path / "image"
    image.mkdir()
    (image / "sel4-beam.img").write_bytes(b"")
    disk = tmp_path / "fat.img"
    disk.write_bytes(b"")
    return Profile(image, disk)


@pytest.fixture
def script(monkeypatch: pytest.MonkeyPatch) -> Callable[..., None]:
    def set_script(*actions: list[object]) -> None:
        monkeypatch.setenv("FAKE_QEMU_SCRIPT", json.dumps(list(actions)))

    return set_script
