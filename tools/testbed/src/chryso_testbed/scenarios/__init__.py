"""Scenario modules. Importing one registers its scenarios."""

from __future__ import annotations

import importlib
from typing import Final

MODULES: Final[tuple[str, ...]] = ()


def load() -> None:
    for module in MODULES:
        importlib.import_module(f"{__name__}.{module}")
