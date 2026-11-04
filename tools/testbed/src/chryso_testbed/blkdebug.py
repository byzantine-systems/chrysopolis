"""blkdebug `inject-error` rules, as a JSON Schema.

A narrowed copy of QEMU's QAPI `BlkdebugInjectErrorOptions` (qapi/block-core.json).
A rule either delays I/O and then succeeds, or fails guest writes. Reads
never fail, so every plan still boots. Both fields are explicit because
blkdebug defaults `errno` to EIO and `iotype` to reads and writes alike.

Scenarios pass rules straight to QEMU, and properties draw them from this
schema with hypothesis-jsonschema.
"""

from __future__ import annotations

import errno
from collections.abc import Iterable, Mapping
from typing import Any, Final

DELAY: Final[dict[str, Any]] = {
    "type": "object",
    "properties": {
        "event": {"enum": ["read_aio", "write_aio"]},
        "iotype": {"enum": ["read", "write"]},
        "errno": {"const": 0},
        "delay-ns": {"type": "integer", "minimum": 0, "maximum": 1_000_000},
        "once": {"type": "boolean"},
    },
    "required": ["event", "iotype", "errno", "delay-ns"],
    "additionalProperties": False,
}
WRITE_ERROR: Final[dict[str, Any]] = {
    "type": "object",
    "properties": {
        "event": {"const": "write_aio"},
        "iotype": {"const": "write"},
        "errno": {"enum": [errno.EIO, errno.ENOSPC]},
        "once": {"type": "boolean"},
    },
    "required": ["event", "iotype", "errno"],
    "additionalProperties": False,
}
RULES: Final[dict[str, Any]] = {
    "type": "array",
    "items": {"oneOf": [DELAY, WRITE_ERROR]},
    "maxItems": 4,
}


def fails_writes(rules: Iterable[Mapping[str, object]]) -> bool:
    """Whether a plan drawn from RULES fails a guest write."""
    return any(rule["errno"] != 0 for rule in rules)
