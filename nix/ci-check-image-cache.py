"""Fail before a CI check can rebuild an image missing from its restored store."""

import json
import subprocess
import sys

IMAGES = {
    "boot-shell-tcp": "test-image",
    "beam-restart-smoke": "test-image",
    "rng-smoke": "test-image",
    "socket-smoke": "default",
    "serial-recovery": "restart-image",
    "timer-recovery": "restart-image",
    "blk-recovery": "restart-image",
    "net-recovery": "restart-image",
    "blk-giveup-smoke": "restart-image",
    "budget-decay-smoke": "budget-decay-image",
    "cothread-smoke": "cothread-probe-image",
    "lifecycle-config-failure-smoke": "lifecycle-failure-image",
}

if len(sys.argv) != 2:
    raise SystemExit("usage: ci-check-image-cache.py CHECK")

check = sys.argv[1]
if check == "restart-topology":
    names = ["default", "restart-image", "budget-decay-image"]
else:
    if check not in IMAGES:
        raise SystemExit(f"unknown QEMU check: {check}")
    names = ["disk", IMAGES[check]]

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
    capture_output=True,
    text=True,
)
for item in json.loads(evaluated.stdout):
    path = item["path"]
    validity = subprocess.run(["nix-store", "--check-validity", path], check=False)
    if validity.returncode:
        raise SystemExit(f"missing cached {item['name']}: {path}")
    print(f"cached {item['name']}: {path}")
