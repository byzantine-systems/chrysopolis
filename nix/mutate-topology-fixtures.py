#!/usr/bin/env python3
"""Build restart-topology fixtures with one planted failure each.

Usage: mutate-topology-fixtures.py <report.txt> <system.sdf> <out-dir>
         --ppc-slot N --ppc-badge 0x... --fault-badge 0x...

Writes <out-dir>/control with verbatim copies, and one <out-dir>/fail-<name>
directory per planted difference. Every edit asserts it changed exactly one
place, so a report or SDF layout drift breaks this generator loudly instead of
producing a vacuous fixture the checker would pass.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

PPC_BADGE_BIT = 0x8000000000000000


def read(path: str) -> list[str]:
    return pathlib.Path(path).read_text(encoding="utf-8").splitlines(keepends=True)


def write_case(out: pathlib.Path, name: str, report: list[str], sdf: list[str]) -> None:
    case = out / name
    (case).mkdir(parents=True, exist_ok=True)
    (case / "report.txt").write_text("".join(report), encoding="utf-8")
    (case / "system.sdf").write_text("".join(sdf), encoding="utf-8")


def cnode_block(lines: list[str], cnode: str) -> tuple[int, int]:
    """Line range (start inclusive, end exclusive) of one CNode report block."""
    header = f"\t- CNode: '{cnode}'"
    start = None
    for index, line in enumerate(lines):
        if line.rstrip("\n") == header:
            start = index
            continue
        if start is not None and line.startswith("\t- "):
            return start, index
    if start is None:
        raise SystemExit(f"fixture: no {header!r} block in report")
    return start, len(lines)


def slot_span(lines: list[str], start: int, end: int, slot: int) -> tuple[int, int]:
    """Line range of one '* Slot:' entry inside a CNode block."""
    marker = f"\t\t* Slot: {slot}\n"
    for index in range(start, end):
        if lines[index] == marker:
            for stop in range(index + 1, end + 1):
                if stop == end or lines[stop].startswith("\t\t* "):
                    return index, stop
    raise SystemExit(f"fixture: no slot {slot} in the CNode block")


def set_badge(lines: list[str], start: int, stop: int, new: str) -> None:
    for index in range(start, stop):
        if lines[index].startswith("\t\t\t-> Badge: "):
            lines[index] = f"\t\t\t-> Badge: {new}\n"
            return
    raise SystemExit("fixture: slot has no Badge line")


def set_object(lines: list[str], start: int, stop: int, new: str) -> None:
    for index in range(start, stop):
        if lines[index].startswith("\t\t\t-> Object: "):
            lines[index] = f"\t\t\t-> Object: {new}\n"
            return
    raise SystemExit("fixture: slot has no Object line")


def sdf_line(lines: list[str], *needles: str) -> int:
    hits = [i for i, line in enumerate(lines) if all(n in line for n in needles)]
    if len(hits) != 1:
        raise SystemExit(f"fixture: expected one SDF line matching {needles}, found {len(hits)}")
    return hits[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report")
    parser.add_argument("sdf")
    parser.add_argument("out")
    parser.add_argument("--ppc-slot", type=int, required=True)
    parser.add_argument("--ppc-badge", required=True)
    parser.add_argument("--fault-badge", required=True)
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    report = read(args.report)
    sdf = read(args.sdf)
    write_case(out, "control", report, sdf)

    beam_start, beam_end = cnode_block(report, "cnode_beam_server")
    root_start, root_end = cnode_block(report, "cnode_root")
    virt_start, virt_end = cnode_block(report, "cnode_serial_virt_tx")

    # The PPC badge low bits no longer name root's channel.
    case = read(args.report)
    start, stop = slot_span(case, beam_start, beam_end, args.ppc_slot)
    set_badge(case, start, stop, hex(int(args.ppc_badge, 16) ^ 1))
    write_case(out, "fail-ppc-badge", case, sdf)

    # The PPC endpoint cap is gone from the caller's CNode entirely.
    case = read(args.report)
    start, stop = slot_span(case, beam_start, beam_end, args.ppc_slot)
    del case[start:stop]
    write_case(out, "fail-ppc-cap-removed", case, sdf)

    # Root's CNode gains a signaling cap to beam_server.
    case = read(args.report)
    start, stop = slot_span(case, root_start, root_end, 3)
    set_object(case, start, stop, "'ntfn_beam_server'")
    write_case(out, "fail-root-signal", case, sdf)

    # Root's CNode carries a PPC-badged cap: control authority on the wrong
    # side of the transport. Slot 20 is the give-up notification cap; only its
    # badge changes, so the give-up object checks still hold.
    case = read(args.report)
    start, stop = slot_span(case, root_start, root_end, 20)
    set_badge(case, start, stop, hex(PPC_BADGE_BIT))
    write_case(out, "fail-ppc-in-root", case, sdf)

    # A virtualiser obtains a second PPC endpoint to Root, independently of
    # BEAM's declared cap. The checker must inspect all CNodes, not just BEAM.
    case = read(args.report)
    start, stop = slot_span(case, virt_start, virt_end, 1)
    set_object(case, start, stop, "'ep_root'")
    set_badge(case, start, stop, args.ppc_badge)
    write_case(out, "fail-second-ppc-caller", case, sdf)

    # beam_server's fault cap badge carries a different child id.
    case = read(args.report)
    start, stop = slot_span(case, beam_start, beam_end, 2)
    set_badge(case, start, stop, hex(int(args.fault_badge, 16) ^ 1))
    write_case(out, "fail-fault-badge", case, sdf)

    # One-writer rights drift: beam_server may write the status page.
    case = read(args.sdf)
    index = sdf_line(case, '<map mr="root_status"', 'setvar_vaddr="root_status_view"')
    case[index] = case[index].replace('perms="r"', 'perms="rw"')
    assert case[index] != sdf[index], "fixture: status map edit did not apply"
    write_case(out, "fail-status-write", report, case)

    # One-writer rights drift: root may write the spec page.
    case = read(args.sdf)
    index = sdf_line(case, '<map mr="orchestrator_spec"', 'setvar_vaddr="orchestrator_spec_view"')
    case[index] = case[index].replace('perms="r"', 'perms="rw"')
    assert case[index] != sdf[index], "fixture: spec map edit did not apply"
    write_case(out, "fail-spec-write", report, case)

    # One alias pins a non-default cache attribute the other alias does not.
    case = read(args.sdf)
    index = sdf_line(case, '<map mr="root_status"', 'setvar_vaddr="root_status_view"')
    case[index] = case[index].replace(" />", ' cached="false" />')
    assert case[index] != sdf[index], "fixture: cache-attribute edit did not apply"
    write_case(out, "fail-alias-attribute", report, case)

    # The SDF and the ABI disagree on where the status window sits.
    case = read(args.sdf)
    index = sdf_line(case, '<map mr="root_status"', 'setvar_vaddr="root_status_view"')
    old = case[index]
    vaddr = old.split('vaddr="')[1].split('"')[0]
    moved = hex(int(vaddr, 16) + 0x8000)
    case[index] = old.replace(f'vaddr="{vaddr}"', f'vaddr="{moved}"')
    assert case[index] != old, "fixture: vaddr edit did not apply"
    write_case(out, "fail-vaddr", report, case)

    names = sorted(p.name for p in out.iterdir() if p.is_dir())
    print(f"fixtures: {', '.join(names)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
