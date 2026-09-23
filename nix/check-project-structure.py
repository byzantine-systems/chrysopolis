#!/usr/bin/env python3
"""Enforce the phase-zero source and dependency boundaries without a target build.

The inventory parser intentionally accepts the current explicit Zig source syntax only.
An unfamiliar source declaration must be added deliberately, not silently missed.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
ZIG_STRING_C = re.compile(r'"([^"\n]*\.c)"')
DIRECT_SOURCE = re.compile(r'\.file\s*=\s*b\.path\("(src/[^"\n]*\.c)"\)')
PROBE_SOURCE = re.compile(
    r'diagnostics\.addContractProbes\(\s*b,\s*glue\.root_module,\s*b\.path\("(src/[^"\n]*\.c)"\)'
)
BEAM_LIST = re.compile(
    r'\.root\s*=\s*b\.path\("src/pd/beam"\).*?\.files\s*=\s*&\.\{(.*?)\n\s*\},',
    re.DOTALL,
)
GENERATED_HEADER_NAMES = {"runtime_abi.h", "system_abi.h"}
TCP = "src/pd/beam/io/tcp.c"
BEAM = "src/pd/beam/"
ROOT = "src/pd/root/"


class StructureError(RuntimeError):
    """A checked ownership or build rule was violated."""


def fail(rule: str, path: str, detail: str) -> None:
    raise StructureError(f"{rule}: {path}: {detail}")


def without_comments(source: str) -> str:
    """Strip C/Zig comments while keeping quoted path literals intact."""
    return re.sub(
        r'"(?:\\.|[^"\\])*"|/\*[\s\S]*?\*/|//[^\n]*',
        lambda match: "" if match.group().startswith(("/*", "//")) else match.group(),
        source,
    )


def owner(path: str) -> str | None:
    if path.startswith(BEAM):
        return "beam"
    if path.startswith(ROOT):
        return "root"
    if path.startswith("src/pd/smp/"):
        return "smp"
    if path.startswith("src/pd/test_support/"):
        return "test_support"
    return None


def framework(include: str) -> str | None:
    if include in ("microkit.h", "libmicrokitco.h") or include.startswith("sel4/"):
        return "kernel"
    if include.startswith(("sddf/", "lions/")):
        return "lionsos"
    return None


def permitted_framework(path: str, kind: str) -> bool:
    if path in ("src/pd/root/main.c", "src/pd/test_support/crasher.c"):
        return kind == "kernel"
    if path == "src/pd/smp/smp.c":
        return kind == "kernel"
    if not path.startswith(BEAM):
        return False
    relative = path[len(BEAM) :]
    if kind == "kernel":
        return relative.startswith(("config/", "compat/", "io/", "restart/", "payload/"))
    return relative.startswith(("config/", "compat/", "io/", "security/", "payload/"))


def check_include(path: str, include: str, private_headers: dict[str, set[str]]) -> None:
    kind = framework(include)
    if kind and not permitted_framework(path, kind):
        fail("forbidden-include", path, include)

    current_owner = owner(path)
    if path.startswith(("src/lib/", "include/chrysopolis/", "interfaces/")) and kind:
        fail("forbidden-include", path, include)

    if path.startswith("src/platform/") and include.startswith("src/pd/"):
        fail("platform-entry", path, include)

    candidates = private_headers.get(Path(include).name, set())
    if include.startswith("src/pd/"):
        candidates = candidates | {include}
    for candidate in candidates:
        other_owner = owner(candidate)
        if other_owner and other_owner != current_owner:
            if current_owner is not None or path.startswith(("src/lib/", "src/platform/", "include/", "interfaces/")):
                fail("cross-pd-include", path, f"{include} resolves to {candidate}")


def check_includes(root: Path) -> None:
    headers = {
        path.relative_to(root).as_posix()
        for base in ("src", "include", "interfaces")
        for path in (root / base).rglob("*.h")
    }
    private_headers: dict[str, set[str]] = {}
    for header in headers:
        if owner(header):
            private_headers.setdefault(Path(header).name, set()).add(header)
    for base in ("src", "include", "interfaces"):
        for path in sorted((root / base).rglob("*")):
            if path.suffix not in (".c", ".h"):
                continue
            relative = path.relative_to(root).as_posix()
            for include in INCLUDE.findall(without_comments(path.read_text())):
                check_include(relative, include, private_headers)


def check_sources(root: Path) -> None:
    build_file = root / "build.zig"
    source = without_comments(build_file.read_text())
    helpers = "\n".join(
        without_comments(path.read_text()) for path in sorted((root / "build").glob("*.zig"))
    )
    for text in (source, helpers):
        if re.search(r'\b(?:glob|walk|iterate)\s*\(', text):
            fail("explicit-source", "build/", "source discovery is not allowed")
        if any("*" in name or "?" in name for name in ZIG_STRING_C.findall(text)):
            fail("explicit-source", "build/", "wildcard C source")
    groups = BEAM_LIST.findall(source)
    if len(groups) != 1:
        fail("explicit-source", "build.zig", "expected one literal BEAM glue source list")
    entries = [BEAM + name for name in ZIG_STRING_C.findall(groups[0])]
    entries += DIRECT_SOURCE.findall(source + helpers)
    entries += PROBE_SOURCE.findall(source + helpers)
    if len(entries) != len(set(entries)):
        fail("explicit-source", "build.zig", "duplicate first-party C source")
    actual = {path.relative_to(root).as_posix() for path in (root / "src").rglob("*.c")}
    dormant = {"src/pd/smp/smp.c"}
    missing = actual - set(entries) - dormant
    unknown = set(entries) - actual
    if missing or unknown or dormant & set(entries):
        fail("explicit-source", "src", f"unlisted={sorted(missing)} unknown={sorted(unknown)} dormant-linked={sorted(dormant & set(entries))}")


def check_stale_paths(root: Path) -> None:
    if (root / "src/runtime").exists():
        fail("stale-path", "src/runtime", "old ownership directory exists")
    paths = [root / "build.zig", root / "build.zig.zon"]
    for base, pattern in (("build", "*.zig"), ("modules", "*.nix"), ("src", "*.c"), ("src", "*.h"), ("tests/host", "*.zig"), ("tools", "*.zig")):
        paths.extend((root / base).rglob(pattern))
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if "src/runtime" in without_comments(path.read_text()):
            fail("stale-path", relative, "operational reference to src/runtime")


def check_generated_headers(root: Path) -> None:
    for base in ("src", "include", "interfaces"):
        for path in (root / base).rglob("*.h"):
            relative = path.relative_to(root).as_posix()
            if path.name in GENERATED_HEADER_NAMES or relative.startswith("interfaces/generated/"):
                fail("generated-header", relative, "generated C headers belong in build outputs")


def check_tcp_exclusion(root: Path) -> None:
    config = without_comments((root / "modules/devshell.nix").read_text())
    match = re.search(r'settings\.global\.excludes\s*=\s*\[([^]]*)\]', config)
    if not match or re.findall(r'"([^"]+)"', match.group(1)) != [TCP]:
        fail("tcp-exclusion", "modules/devshell.nix", f"treefmt must exclude exactly {TCP}")


def check_public_headers(root: Path, compiler: str) -> None:
    public = root / "include/chrysopolis"
    for header in sorted(public.rglob("*.h")):
        name = header.relative_to(root / "include").as_posix()
        # The Nix clang wrapper adds linker arguments even for syntax-only
        # invocations. Ignore that driver warning, not C diagnostics.
        command = [compiler, "-std=c23", "-Wall", "-Wextra", "-Werror", "-Wno-unused-command-line-argument", "-fsyntax-only", "-x", "c", "-I", str(root / "include"), "-include", name, "-include", name, "/dev/null"]
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        if result.returncode:
            fail("public-header", name, result.stderr.strip())


def check(root: Path, compiler: str) -> None:
    check_sources(root)
    check_stale_paths(root)
    check_generated_headers(root)
    check_tcp_exclusion(root)
    check_includes(root)
    check_public_headers(root, compiler)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--cc", default="clang")
    arguments = parser.parse_args()
    try:
        check(arguments.root.resolve(), arguments.cc)
    except (OSError, StructureError) as error:
        print(f"project-structure: {error}", file=sys.stderr)
        sys.exit(1)
