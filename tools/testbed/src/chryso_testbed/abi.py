"""Child ids and restart limits from interfaces/generated/system-abi.json."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class Abi:
    drivers: dict[str, int]
    beam: int
    crasher: int
    driver_budget: int
    exit_fault_base: int

    def child(self, driver: str) -> int:
        return self.drivers[driver]

    @classmethod
    def load(cls, path: Path) -> Abi:
        raw = json.loads(path.read_text())
        return cls(
            drivers={d["name"]: int(d["child"]) for d in raw["drivers"]},
            beam=int(raw["children"]["beam"]),
            crasher=int(raw["children"]["crasher"]),
            driver_budget=int(raw["restart"]["driver_budget"]),
            exit_fault_base=int(raw["restart"]["exit_fault_base"]),
        )
