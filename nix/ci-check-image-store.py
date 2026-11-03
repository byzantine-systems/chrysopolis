"""Verify the imported CI artifact contains each check's image and disk."""

import json
import subprocess
import sys

IMAGES = {
    "testbed-boot": (
        "test-image",
        "default",
        "cothread-probe-image",
        "lifecycle-failure-image",
    ),
    "testbed-restart": ("restart-image", "budget-decay-image"),
    "testbed-orch": (
        "test-image",
        "spec-commit-image",
        "command-image",
        "fallback-image",
        "fallback-nomap-image",
    ),
}

if len(sys.argv) != 2:
    raise SystemExit("usage: ci-check-image-store.py CHECK")

check = sys.argv[1]
if check == "restart-topology":
    names = ["default", "restart-image", "budget-decay-image"]
else:
    if check not in IMAGES:
        raise SystemExit(f"unknown QEMU check: {check}")
    names = ["disk", *IMAGES[check]]

name_list = "[ " + " ".join(json.dumps(name) for name in names) + " ]"
expression = (
    "p: builtins.map (name: { inherit name; "
    "path = (builtins.getAttr name p).outPath; }) " + name_list
)
evaluated = subprocess.run(
    [
        "nix",
        "eval",
        "--json",
        ".#packages.x86_64-linux",
        "--apply",
        expression,
    ],
    check=True,
    # stderr stays on the job log, so a failed eval shows its cause.
    stdout=subprocess.PIPE,
    text=True,
)
for item in json.loads(evaluated.stdout):
    path = item["path"]
    validity = subprocess.run(["nix-store", "--check-validity", path], check=False)
    if validity.returncode:
        raise SystemExit(f"missing imported {item['name']}: {path}")
    print(f"imported {item['name']}: {path}")
