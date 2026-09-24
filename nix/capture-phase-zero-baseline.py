#!/usr/bin/env python3
"""Capture the behavior-preserving refactor baseline.

The output is a deterministic JSON description of the generated topology,
Microkit reports, runtime-relevant ELF layout, image sizes, host-test matrix,
and checked behavior contracts. It deliberately omits timestamps, Nix store
prefixes, DWARF, and raw serial logs.

Microkit's report.txt is documented as human-readable rather than stable. The
parser is therefore pinned to the headings emitted by Microkit 2.3.0 and fails
closed if a required section or field is absent.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


REQUIRED_REPORT_HEADINGS = (
    "# IRQ Details",
    "# TCB Details",
    "# CNode Details",
    "# Kernel Objects Details: ID, Type, Name, Physical Address",
)
REQUIRED_SYMBOLS = {
    "root.elf": ("_start",),
    "beam_server.elf": ("_start", "_reset", "__init_array_start", "_bss", "_bss_end"),
    "beam_test.elf": ("_start", "_reset", "__init_array_start", "_bss", "_bss_end"),
}
PINNED_INPUTS = (
    "libmicrokitco",
    "lionsos",
    "musllibc",
    "nixpkgs",
    "sddf",
    "sdfgen",
    "zig-overlay",
    "zig2nix",
)
STORE_PATH_RE = re.compile(r"/nix/store/[a-z0-9]{32}-[^/\s]+")


class BaselineError(RuntimeError):
    """An input did not satisfy the pinned baseline format."""


def fail(message: str) -> None:
    raise BaselineError(message)


def read_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read JSON {path}: {error}")


def normalize_text(value: str) -> str:
    return STORE_PATH_RE.sub("<nix-store-path>", value.strip())


def normalize_scalar(value: str) -> Any:
    value = normalize_text(value)
    if len(value) >= 2 and value[0] == value[-1] == "'":
        value = value[1:-1]
    if re.fullmatch(r"-?[0-9]+", value):
        return int(value)
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        return f"0x{int(value, 16):x}"
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        fail(f"cannot hash {path}: {error}")
    return digest.hexdigest()


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def json_shape(value: Any) -> dict[str, Any]:
    """Record reviewable top-level shape while the canonical hash covers values."""
    if not isinstance(value, dict):
        return {"kind": type(value).__name__}
    return {
        "keys": sorted(value),
        "scalar_fields": {
            key: field
            for key, field in sorted(value.items())
            if isinstance(field, (bool, int, float, str)) or field is None
        },
    }


def attrs(element: ET.Element) -> dict[str, Any]:
    return {key: normalize_scalar(value) for key, value in sorted(element.attrib.items())}


def parse_sdf(sdf_dir: Path, id_count: int) -> dict[str, Any]:
    sdf_path = sdf_dir / "system.sdf"
    try:
        root = ET.parse(sdf_path).getroot()
    except (OSError, ET.ParseError) as error:
        fail(f"cannot parse SDF {sdf_path}: {error}")
    if root.tag != "system":
        fail(f"{sdf_path}: expected <system>, found <{root.tag}>")

    pds: list[dict[str, Any]] = []

    def visit_pd(element: ET.Element, parent: str | None) -> None:
        name = element.attrib.get("name")
        if not name:
            fail(f"{sdf_path}: protection_domain without a name")
        images = [attrs(child) for child in element if child.tag == "program_image"]
        if len(images) != 1:
            fail(f"{sdf_path}: {name} has {len(images)} program images, expected one")
        pds.append(
            {
                "name": name,
                "parent": parent,
                "attributes": attrs(element),
                "program_image": images[0],
                "maps": [attrs(child) for child in element if child.tag == "map"],
                "irqs": [attrs(child) for child in element if child.tag == "irq"],
                "setvars": [attrs(child) for child in element if child.tag == "setvar"],
            }
        )
        for child in element:
            if child.tag == "protection_domain":
                visit_pd(child, name)

    for child in root:
        if child.tag == "protection_domain":
            visit_pd(child, None)

    memory_regions = [attrs(child) for child in root if child.tag == "memory_region"]
    channels = [
        {
            "attributes": attrs(child),
            "ends": [attrs(end) for end in child if end.tag == "end"],
        }
        for child in root
        if child.tag == "channel"
    ]
    for index, channel in enumerate(channels):
        if len(channel["ends"]) != 2:
            fail(f"{sdf_path}: channel {index} has {len(channel['ends'])} ends")

    ids_by_pd: dict[str, set[int]] = {pd["name"]: set() for pd in pds}
    for pd in pds:
        for irq in pd["irqs"]:
            if "id" in irq:
                ids_by_pd[pd["name"]].add(int(irq["id"]))
    for channel in channels:
        for end in channel["ends"]:
            ids_by_pd[str(end["pd"])].add(int(end["id"]))

    id_usage = {}
    for pd_name in sorted(ids_by_pd):
        used = sorted(ids_by_pd[pd_name])
        id_usage[pd_name] = {
            "used": used,
            "used_count": len(used),
            "free_count": id_count - len(used),
            "maximum_used": used[-1] if used else None,
        }

    declared_bytes = 0
    fixed_physical_bytes = 0
    for region in memory_regions:
        if "size" not in region:
            fail(f"{sdf_path}: baseline requires an explicit size for {region.get('name')}")
        size = int(str(region["size"]), 0)
        declared_bytes += size
        if "phys_addr" in region:
            fixed_physical_bytes += size

    blobs = []
    for data_path in sorted(sdf_dir.glob("*.data"), key=lambda path: path.name):
        json_path = data_path.with_suffix(".json")
        if not json_path.is_file():
            fail(f"{sdf_dir}: {data_path.name} has no JSON sidecar")
        json_value = read_json(json_path)
        blobs.append(
            {
                "name": data_path.name,
                "bytes": data_path.stat().st_size,
                "sha256": sha256_file(data_path),
                "json_sha256": hashlib.sha256(canonical_json_bytes(json_value)).hexdigest(),
                "json_shape": json_shape(json_value),
            }
        )

    root_children = sorted(
        int(pd["attributes"]["id"])
        for pd in pds
        if pd["parent"] == "root" and "id" in pd["attributes"]
    )
    capacity = {
        "protection_domains": len(pds),
        "memory_regions": len(memory_regions),
        "channels": len(channels),
        "declared_memory_bytes": declared_bytes,
        "fixed_physical_memory_bytes": fixed_physical_bytes,
        "config_blob_bytes": sum(blob["bytes"] for blob in blobs),
        "root_child_ids": root_children,
        "root_child_count": len(root_children),
        "id_count_per_pd": id_count,
        "id_usage": id_usage,
    }
    complete_system = {
        "attributes": attrs(root),
        "memory_regions": memory_regions,
        "protection_domains": pds,
        "channels": channels,
    }
    pd_summaries = []
    for pd in pds:
        pd_summaries.append(
            {
                "name": pd["name"],
                "parent": pd["parent"],
                "attributes": pd["attributes"],
                "program_image": pd["program_image"],
                "map_count": len(pd["maps"]),
                "maps_sha256": hashlib.sha256(canonical_json_bytes(pd["maps"])).hexdigest(),
                "irqs": pd["irqs"],
                "setvars": pd["setvars"],
            }
        )
    return {
        "system": {
            "canonical_sha256": hashlib.sha256(canonical_json_bytes(complete_system)).hexdigest(),
            "attributes": complete_system["attributes"],
            "memory_regions": memory_regions,
            "protection_domains": pd_summaries,
            "channels": channels,
        },
        "config_blobs": blobs,
        "capacity": capacity,
    }


def report_blocks(lines: list[str], kind: str) -> list[tuple[str, list[str]]]:
    header = re.compile(rf"^\t- {re.escape(kind)}: '([^']+)'$")
    any_block = re.compile(r"^\t- [A-Za-z ]+: ")
    blocks: list[tuple[str, list[str]]] = []
    index = 0
    while index < len(lines):
        match = header.match(lines[index])
        if not match:
            index += 1
            continue
        name = match.group(1)
        body: list[str] = []
        index += 1
        while index < len(lines) and not lines[index].startswith("# ") and not any_block.match(lines[index]):
            body.append(lines[index])
            index += 1
        blocks.append((name, body))
    return blocks


def parse_fields(body: list[str]) -> tuple[dict[str, Any], dict[str, Any]]:
    fields: dict[str, Any] = {}
    bound: dict[str, Any] = {}
    in_bound = False
    for line in body:
        stripped = line.strip()
        if stripped == "* Bound Objects:":
            in_bound = True
            continue
        match = re.match(r"^(?:\*|->) ([^:]+): (.+)$", stripped)
        if not match:
            continue
        key, value = match.groups()
        target = bound if in_bound and stripped.startswith("->") else fields
        target[key] = normalize_scalar(value)
    return fields, bound


def parse_report(path: Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        fail(f"cannot read report {path}: {error}")
    for heading in REQUIRED_REPORT_HEADINGS:
        if heading not in text.splitlines():
            fail(f"{path}: Microkit 2.3.0 report layout changed, missing {heading!r}")
    lines = text.splitlines()

    irqs = []
    for name, body in report_blocks(lines, "IRQ"):
        fields, _ = parse_fields(body)
        if not {"Number", "Trigger", "CPU"}.issubset(fields):
            fail(f"{path}: IRQ {name} lacks Number, Trigger, or CPU")
        irqs.append({"name": name, **fields})

    tcbs = []
    for name, body in report_blocks(lines, "TCB"):
        fields, bound = parse_fields(body)
        required = {"IP", "SP", "IPC Buffer", "Priority", "CPU Affinity"}
        if not required.issubset(fields):
            fail(f"{path}: TCB {name} lacks required Microkit 2.3.0 fields")
        tcbs.append({"name": name, "fields": fields, "bound_objects": bound})

    cnodes = []
    for name, body in report_blocks(lines, "CNode"):
        slots = []
        slot: dict[str, Any] | None = None
        for line in body:
            stripped = line.strip()
            match = re.match(r"^\* Slot: ([0-9]+)$", stripped)
            if match:
                if slot is not None:
                    slots.append(slot)
                slot = {"slot": int(match.group(1))}
                continue
            match = re.match(r"^-> ([^:]+): (.+)$", stripped)
            if match and slot is not None:
                key, value = match.groups()
                if key == "Rights":
                    slot["rights"] = sorted(part.strip() for part in value.split(","))
                else:
                    slot[key.lower()] = normalize_scalar(value)
        if slot is not None:
            slots.append(slot)
        if not slots:
            fail(f"{path}: CNode {name} has no occupied slots")
        cnodes.append({"name": name, "slots": slots})

    if not irqs or not tcbs or not cnodes:
        fail(f"{path}: report parser found an empty IRQ, TCB, or CNode section")
    complete = {
        "irqs": sorted(irqs, key=lambda value: value["name"]),
        "tcbs": sorted(tcbs, key=lambda value: value["name"]),
        "cnodes": sorted(cnodes, key=lambda value: value["name"]),
    }
    tcb_summaries = []
    for tcb in complete["tcbs"]:
        tcb_summaries.append(
            {
                "name": tcb["name"],
                "ip": tcb["fields"]["IP"],
                "priority": tcb["fields"]["Priority"],
                "cpu_affinity": tcb["fields"]["CPU Affinity"],
                "fault_endpoint": tcb["bound_objects"].get("Fault Endpoint"),
                "scheduling_context": tcb["bound_objects"].get("Scheduling Context"),
            }
        )
    critical_cnodes = {
        cnode["name"]: cnode["slots"]
        for cnode in complete["cnodes"]
        if cnode["name"] in {"cnode_root", "cnode_beam_server", "cnode_crasher"}
    }
    cnode_summaries = [
        {
            "name": cnode["name"],
            "slot_count": len(cnode["slots"]),
            "slots_sha256": hashlib.sha256(canonical_json_bytes(cnode["slots"])).hexdigest(),
        }
        for cnode in complete["cnodes"]
    ]
    return {
        "canonical_sha256": hashlib.sha256(canonical_json_bytes(complete)).hexdigest(),
        "irqs": complete["irqs"],
        "tcbs": tcb_summaries,
        "critical_cnodes": critical_cnodes,
        "cnodes": cnode_summaries,
    }


def run_readelf(readelf: Path, flag: str, elf: Path) -> str:
    try:
        result = subprocess.run(
            [str(readelf), flag, str(elf)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        fail(f"cannot inspect {elf} with {readelf}: {error}")
    return result.stdout


def parse_elf(readelf: Path, path: Path) -> dict[str, Any]:
    header_text = run_readelf(readelf, "-hW", path)
    header = {}
    for key in ("Class", "Data", "Type", "Machine", "Entry point address"):
        match = re.search(rf"^\s*{re.escape(key)}:\s+(.+)$", header_text, re.MULTILINE)
        if not match:
            fail(f"{path}: ELF header lacks {key}")
        output_key = "entry" if key == "Entry point address" else key.lower().replace(" ", "_")
        header[output_key] = normalize_scalar(match.group(1))

    segments = []
    raw = path.read_bytes()
    for line in run_readelf(readelf, "-lW", path).splitlines():
        parts = line.split()
        if not parts or parts[0] != "LOAD" or len(parts) < 8:
            continue
        offset = int(parts[1], 16)
        file_size = int(parts[4], 16)
        content = raw[offset : offset + file_size]
        if len(content) != file_size:
            fail(f"{path}: PT_LOAD extends beyond the ELF file")
        segments.append(
            {
                "offset": f"0x{offset:x}",
                "virtual_address": f"0x{int(parts[2], 16):x}",
                "physical_address": f"0x{int(parts[3], 16):x}",
                "file_size": file_size,
                "memory_size": int(parts[5], 16),
                "flags": "".join(parts[6:-1]),
                "alignment": f"0x{int(parts[-1], 16):x}",
            }
        )
    if not segments:
        fail(f"{path}: no PT_LOAD segments")

    sections = []
    section_re = re.compile(
        r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
        r"[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S*)\s+\d+\s+\d+\s+(\d+)$"
    )
    for line in run_readelf(readelf, "-SW", path).splitlines():
        match = section_re.match(line)
        if not match:
            continue
        name, section_type, address, size, flags, alignment = match.groups()
        important_section = (
            name in {".text", ".init_array", ".data.rel.ro", ".got", ".data", ".bss", "ERTS_LOW_WRITE"}
            or name.endswith("_config")
        )
        if "A" not in flags or not important_section:
            continue
        sections.append(
            {
                "name": name,
                "type": section_type,
                "address": f"0x{int(address, 16):x}",
                "size": int(size, 16),
                "flags": flags,
                "alignment": int(alignment),
            }
        )
    if not sections:
        fail(f"{path}: no allocated ELF sections")

    wanted = REQUIRED_SYMBOLS[path.name]
    symbols: dict[str, str] = {}
    for line in run_readelf(readelf, "-sW", path).splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        name = parts[7]
        if name in wanted and name not in symbols:
            symbols[name] = f"0x{int(parts[1], 16):x}"
    missing = sorted(set(wanted) - set(symbols))
    if missing:
        fail(f"{path}: required symbols missing: {', '.join(missing)}")
    return {"header": header, "load_segments": segments, "allocated_sections": sections, "symbols": symbols}


def parse_host_tests(path: Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        fail(f"cannot read host-test build {path}: {error}")
    # Suite entries may carry owner/source fields before units after a move.
    # Require both a named entry and its units declaration so the inventory
    # still fails closed if the build table's shape changes unexpectedly.
    suites = re.findall(
        r'^\s*\.\{\s*\.name\s*=\s*"([^"]+)",[^\n]*\.units\s*=',
        text,
        re.MULTILINE,
    )
    variants = [
        {"name": name, "optimize": optimize, "sanitize_c": sanitize}
        for name, optimize, sanitize in re.findall(
            r'\.suffix = "([^"]+)", \.optimize = \.([A-Za-z]+), \.sanitize_c = \.([a-z]+)',
            text,
        )
    ]
    if not suites or not variants:
        fail(f"{path}: could not find host suite and variant declarations")
    return {"suites": suites, "variants": variants, "executions": len(suites) * len(variants)}


def resolve_lock_inputs(path: Path) -> dict[str, Any]:
    lock = read_json(path)
    try:
        root_inputs = lock["nodes"][lock["root"]]["inputs"]
        nodes = lock["nodes"]
    except (KeyError, TypeError) as error:
        fail(f"{path}: unsupported flake.lock structure: {error}")
    result = {}
    for name in PINNED_INPUTS:
        node_name = root_inputs.get(name)
        if not isinstance(node_name, str) or node_name not in nodes:
            fail(f"{path}: root input {name} does not resolve to one node")
        locked = nodes[node_name].get("locked", {})
        result[name] = {
            key: locked[key]
            for key in ("type", "url", "owner", "repo", "ref", "rev", "narHash")
            if key in locked
        }
    return result


def parse_contracts(contracts_path: Path, names_path: Path) -> dict[str, Any]:
    contracts = read_json(contracts_path)
    names = read_json(names_path)
    if not isinstance(contracts, dict) or not isinstance(names, list):
        fail("check contracts must be an object and check names must be an array")
    actual = sorted(str(name) for name in names)
    documented = sorted(contracts)
    if actual != documented:
        missing = sorted(set(actual) - set(documented))
        stale = sorted(set(documented) - set(actual))
        fail(f"check contract inventory differs: undocumented={missing}, stale={stale}")
    for name, contract in contracts.items():
        if not isinstance(contract, dict) or not contract.get("protects"):
            fail(f"check contract {name} has no protected behavior or artifact")
    return {"count": len(actual), "contracts": {name: contracts[name] for name in actual}}


def file_fact(path: Path, *, include_hash: bool = True) -> dict[str, Any]:
    if not path.is_file():
        fail(f"artifact is not a file: {path}")
    fact: dict[str, Any] = {"bytes": path.stat().st_size}
    if include_hash:
        fact["sha256"] = sha256_file(path)
    return fact


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--abi", type=Path, required=True)
    parser.add_argument("--flake-lock", type=Path, required=True)
    parser.add_argument("--host-build", type=Path, required=True)
    parser.add_argument("--contracts", type=Path, required=True)
    parser.add_argument("--check-names", type=Path, required=True)
    parser.add_argument("--production-sdf", type=Path, required=True)
    parser.add_argument("--restart-sdf", type=Path, required=True)
    parser.add_argument("--beam-zig", type=Path, required=True)
    parser.add_argument("--production-image", type=Path, required=True)
    parser.add_argument("--test-image", type=Path, required=True)
    parser.add_argument("--restart-image", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--microkit-version", required=True)
    parser.add_argument("--microkit-board", required=True)
    parser.add_argument("--microkit-config", required=True)
    parser.add_argument("--zig-version", required=True)
    parser.add_argument("--llvm-version", required=True)
    parser.add_argument("--otp-version", required=True)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        abi = read_json(args.abi)
        id_count = int(abi["microkit"]["id_count"])
        generated_with_capacity = {
            "production": parse_sdf(args.production_sdf, id_count),
            "restart": parse_sdf(args.restart_sdf, id_count),
        }
        generated = {
            name: {
                "system": value["system"],
                "config_blobs": value["config_blobs"],
            }
            for name, value in generated_with_capacity.items()
        }
        production_report = parse_report(args.production_image / "report.txt")
        test_report = parse_report(args.test_image / "report.txt")
        restart_report = parse_report(args.restart_image / "report.txt")
        test_report_fact = (
            {
                "same_as": "production",
                "sha256": hashlib.sha256(canonical_json_bytes(test_report)).hexdigest(),
            }
            if test_report == production_report
            else test_report
        )
        elf_dir = args.beam_zig / "bin"
        manifest = {
            "schema_version": 1,
            "toolchain": {
                "microkit": {
                    "version": args.microkit_version,
                    "board": args.microkit_board,
                    "config": args.microkit_config,
                },
                "zig": args.zig_version,
                "llvm": args.llvm_version,
                "otp": args.otp_version,
                "flake_inputs": resolve_lock_inputs(args.flake_lock),
            },
            "abi": abi,
            "host_tests": parse_host_tests(args.host_build),
            "generated": generated,
            "capacity": {
                name: value["capacity"] for name, value in generated_with_capacity.items()
            },
            "reports": {
                "production": production_report,
                "test": test_report_fact,
                "restart": restart_report,
            },
            "elf": {
                name: parse_elf(args.readelf, elf_dir / name)
                for name in sorted(REQUIRED_SYMBOLS)
            },
            "images": {
                # Source paths can appear in allocated diagnostic strings. A
                # source-only move may therefore alter whole-image bytes while
                # preserving every runtime address and behavior. Record image
                # size here; topology, capability, and ELF layout are compared
                # structurally above. The FAT disk has no native source paths,
                # so its byte hash remains useful.
                "production": file_fact(
                    args.production_image / "sel4-beam.img", include_hash=False
                ),
                "test": file_fact(args.test_image / "sel4-beam.img", include_hash=False),
                "restart": file_fact(
                    args.restart_image / "sel4-beam.img", include_hash=False
                ),
                "disk": file_fact(args.disk),
            },
            "checks": parse_contracts(args.contracts, args.check_names),
            "normalization": {
                "excluded": [
                    "Nix store prefixes",
                    "timestamps",
                    "ELF non-allocated sections including DWARF",
                    "ELF segment and whole-image hashes that encode source paths",
                    "raw QEMU serial logs",
                    "report kernel-object physical addresses",
                ],
                "serial_evidence": "required and forbidden marker contracts plus QEMU checks",
            },
        }
    except (BaselineError, KeyError, TypeError, ValueError) as error:
        print(f"phase-zero-baseline: {error}", file=sys.stderr)
        return 1

    output = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        sys.stdout.write(output)
    else:
        try:
            args.output.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"phase-zero-baseline: cannot write {args.output}: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
