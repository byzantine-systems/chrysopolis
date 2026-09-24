#!/usr/bin/env python3
"""Negative fixtures for each phase-zero structure rule."""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parent / "check-project-structure.py"
SPEC = importlib.util.spec_from_file_location("project_structure", SOURCE)
assert SPEC is not None and SPEC.loader is not None
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)
REPOSITORY = SOURCE.parent.parent


class StructureFixtures(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for directory in ("src", "build", "modules", "include", "interfaces"):
            shutil.copytree(REPOSITORY / directory, self.root / directory)
        for name in ("build.zig", "build.zig.zon"):
            shutil.copyfile(REPOSITORY / name, self.root / name)

    def append(self, path: str, content: str) -> None:
        target = self.root / path
        target.write_text(target.read_text() + content)

    def reject(self, rule: str) -> None:
        with self.assertRaisesRegex(checker.StructureError, rule):
            checker.check(self.root, "clang")

    def test_forbidden_framework_include(self) -> None:
        self.append("src/pd/root/policy/root_policy.h", "\n#include <microkit.h>\n")
        self.reject("forbidden-include")

    def test_cross_pd_private_include(self) -> None:
        self.append("src/pd/beam/restart/beam_restart_layout.h", '\n#include "root_policy.h"\n')
        self.reject("cross-pd-include")

    def test_public_header_isolation(self) -> None:
        (self.root / "include/chrysopolis/broken.h").write_text('#include "missing.h"\n')
        self.reject("public-header")

    def test_public_header_positive(self) -> None:
        (self.root / "include/chrysopolis/working.h").write_text(
            "#ifndef CHRYSOPOLIS_WORKING_H\n#define CHRYSOPOLIS_WORKING_H\n"
            "#include <stdint.h>\n"
            "typedef uint32_t chryso_working_id;\n#endif\n"
        )
        checker.check(self.root, "clang")

    def test_unlisted_source(self) -> None:
        (self.root / "src/pd/beam/unlisted.c").write_text("int unlisted(void) { return 0; }\n")
        self.reject("explicit-source")

    def test_unused_path_does_not_count_as_source(self) -> None:
        (self.root / "src/pd/beam/unlisted.c").write_text("int unlisted(void) { return 0; }\n")
        self.append("build.zig", '\nconst unused = b.path("src/pd/beam/unlisted.c");\n')
        self.reject("explicit-source")

    def test_globbed_source(self) -> None:
        path = self.root / "build.zig"
        path.write_text(path.read_text().replace('"main.c",', '"*.c",', 1))
        self.reject("explicit-source")

    def test_stale_directory(self) -> None:
        (self.root / "src/runtime").mkdir()
        self.reject("stale-path")

    def test_stale_operational_path(self) -> None:
        self.append("build.zig", '\nconst stale = "src/runtime/main.c";\n')
        self.reject("stale-path")

    def test_generated_header(self) -> None:
        (self.root / "include/chrysopolis/runtime_abi.h").write_text("#pragma once\n")
        self.reject("generated-header")

    def test_tcp_formatting_exclusion(self) -> None:
        path = self.root / "modules/devshell.nix"
        path.write_text(path.read_text().replace('"src/pd/beam/io/tcp.c"', '"src/pd/beam/io/not-tcp.c"'))
        self.reject("tcp-exclusion")


if __name__ == "__main__":
    unittest.main()
