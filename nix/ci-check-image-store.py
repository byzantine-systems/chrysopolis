"""Verify the imported CI artifact contains each check's image and disk."""

import json
import subprocess
import sys

# The testbed groups guard their images with a dry run in the workflow, so
# only the topology check still needs this script.
IMAGES = {"restart-topology": ("default", "restart-image", "budget-decay-image")}

if len(sys.argv) != 2 or sys.argv[1] not in IMAGES:
    raise SystemExit(f"usage: ci-check-image-store.py {{{','.join(IMAGES)}}}")

names = list(IMAGES[sys.argv[1]])

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
