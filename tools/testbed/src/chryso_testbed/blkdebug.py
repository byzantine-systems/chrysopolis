"""blkdebug `inject-error` rules, as a JSON Schema.

A narrowed copy of QEMU's QAPI `BlkdebugInjectErrorOptions` (qapi/block-core.json):
only the guest data events, a few errnos (0 means delay only) and bounded
delays. Scenarios pass rules straight to QEMU, and property tests generate
them from this schema with hypothesis-jsonschema.
"""

from __future__ import annotations

import errno
from typing import Any, Final

RULE: Final[dict[str, Any]] = {
    "type": "object",
    "properties": {
        "event": {"enum": ["read_aio", "write_aio", "flush_to_disk"]},
        "iotype": {"enum": ["read", "write", "flush"]},
        "errno": {"enum": [0, errno.EIO, errno.ENOSPC]},
        "delay-ns": {"type": "integer", "minimum": 0, "maximum": 5_000_000},
        "once": {"type": "boolean"},
        "state": {"type": "integer", "minimum": 1, "maximum": 3},
    },
    "required": ["event"],
    "additionalProperties": False,
}
RULES: Final[dict[str, Any]] = {"type": "array", "items": RULE, "maxItems": 4}
