"""Require one contract per flake check, each naming what it protects.

The inventory is a rule about the checks, not a fact about the product, so it
lives here rather than in the phase-zero fingerprint.
"""

import json
import sys
from pathlib import Path


def main(contracts_path: Path, names_path: Path) -> str | None:
    contracts = json.loads(contracts_path.read_text(encoding="utf-8"))
    names = json.loads(names_path.read_text(encoding="utf-8"))
    if not isinstance(contracts, dict) or not isinstance(names, list):
        return "check contracts must be an object and check names must be an array"
    actual, documented = set(map(str, names)), set(contracts)
    if actual != documented:
        return (
            "check contract inventory differs: "
            f"undocumented={sorted(actual - documented)}, stale={sorted(documented - actual)}"
        )
    if empty := sorted(
        name
        for name, contract in contracts.items()
        if not isinstance(contract, dict) or not contract.get("protects")
    ):
        return f"check contracts with nothing protected: {empty}"
    print(f"{len(actual)} checks, each with a contract")
    return None


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: check-contracts.py CONTRACTS CHECK_NAMES")
    raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2])))
