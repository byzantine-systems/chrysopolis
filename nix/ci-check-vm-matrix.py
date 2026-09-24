"""Ensure the CI VM matrix covers every documented QEMU flake check."""

import json
import re
from pathlib import Path

contracts = json.loads(Path("baselines/phase-zero/contracts.json").read_text())
expected = {name for name, contract in contracts.items() if contract["kind"] == "qemu"}
workflow = Path(".github/workflows/build.yml").read_text()
matrix = workflow.split("      matrix:\n", 1)[1].split("    steps:\n", 1)[0]
actual = set(re.findall(r"^          - ([a-z0-9-]+)$", matrix, re.MULTILINE))
if actual != expected:
    raise SystemExit(
        f"CI VM matrix mismatch: missing={sorted(expected - actual)}, "
        f"extra={sorted(actual - expected)}"
    )
print(f"CI VM matrix covers all {len(actual)} QEMU checks")
