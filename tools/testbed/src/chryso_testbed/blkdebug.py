"""blkdebug `inject-error` rules as Pydantic models, and what a plan of them
lets the guest report.

The models narrow QEMU's QAPI `BlkdebugInjectErrorOptions`
(qapi/block-core.json) to two kinds of rule. A `Delay` waits and then lets
the I/O succeed. A `WriteError` fails guest writes. Reads never fail, so
every plan still boots. `errno` and `iotype` are always sent, because
blkdebug defaults to EIO on reads and writes alike. `errno` also tags the
union.

`PLAN` validates plans, dumps them in QEMU's field names, and gives the JSON
Schema that properties draw plans from with hypothesis-jsonschema.

`SectorRead`/`SectorWrite` are armed at runtime on one guest sector.
"""

from __future__ import annotations

import errno
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Annotated, Final, Literal

from pydantic import AliasChoices, BaseModel, ConfigDict, Field, TypeAdapter


class _Rule(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid", strict=True, serialize_by_alias=True)
    once: bool = False


class Delay(_Rule):
    """Delay matching I/O by up to 1 ms, then let it succeed."""

    errno: Literal[0]
    event: Literal["read_aio", "write_aio"]
    iotype: Literal["read", "write"]
    delay_ns: int = Field(
        ge=0,
        le=1_000_000,
        validation_alias=AliasChoices("delay-ns", "delay_ns"),
        serialization_alias="delay-ns",
    )


class WriteError(_Rule):
    """Fail guest writes with EIO, every time or once.

    Only EIO: QEMU pauses the VM on ENOSPC by default (werror=enospc), and
    the guest sees every write error as VIRTIO_BLK_S_IOERR anyway."""

    errno: Literal[5]  # EIO on Linux, where QEMU runs
    event: Literal["write_aio"] = "write_aio"
    iotype: Literal["write"] = "write"


EIO: Final[Literal[5]] = 5
if errno.EIO != EIO:
    raise ImportError("WriteError.errno must equal the host's EIO")

type Rule = Annotated[Delay | WriteError, Field(discriminator="errno")]
type Plan = Annotated[tuple[Rule, ...], Field(max_length=4)]
PLAN: Final = TypeAdapter[Plan](Plan)
SCHEMA: Final = PLAN.json_schema()


class SectorRead(_Rule):
    """Fail guest reads that touch `sector` with EIO."""

    errno: Literal[5] = 5
    event: Literal["read_aio"] = "read_aio"
    iotype: Literal["read"] = "read"
    sector: int = Field(ge=0)


class SectorWrite(_Rule):
    """Fail guest writes that touch `sector` with EIO."""

    errno: Literal[5] = 5
    event: Literal["write_aio"] = "write_aio"
    iotype: Literal["write"] = "write"
    sector: int = Field(ge=0)


type SectorError = Annotated[SectorRead | SectorWrite, Field(discriminator="iotype")]
RUNTIME: Final = TypeAdapter[tuple[SectorError, ...]](tuple[SectorError, ...])


def fails_writes(plan: Iterable[Delay | WriteError]) -> bool:
    """Whether a plan fails a guest write."""
    return any(isinstance(rule, WriteError) for rule in plan)


type Shape = Literal["clean", "single", "transient", "any"]
SHAPES: Final[tuple[Shape, ...]] = ("clean", "single", "transient", "any")


def shaped(plan: Plan, shape: Shape) -> Plan:
    """Fit a plan to a shape, so every shape is drawn about equally often.

    A clean plan keeps only delays, a single one adds exactly one one-shot
    write error to them, and a transient one fails each write once."""
    delays = tuple(rule for rule in plan if isinstance(rule, Delay))
    match shape:
        case "clean":
            return delays
        case "single":
            return (*delays[:3], WriteError(errno=EIO, once=True))
        case "transient":
            return tuple(
                rule.model_copy(update={"once": True}) if isinstance(rule, WriteError) else rule
                for rule in plan
            )
        case _:
            return plan


@dataclass(frozen=True, slots=True)
class Expected:
    """What the guest may report under a plan.

    A write reports eio exactly when the plan fails writes. FatFs retries its
    dirty sector on the next sector load, so after a permanent write error a
    read fails with eio, after one one-shot error it works, and after several
    it may do either."""

    write_fails: bool
    reads: frozenset[str]

    @classmethod
    def of(cls, plan: Iterable[Delay | WriteError]) -> Expected:
        failing = [rule for rule in plan if isinstance(rule, WriteError)]
        if any(not rule.once for rule in failing):
            reads = frozenset({"error"})
        else:
            reads = frozenset({"ok"} if len(failing) <= 1 else {"ok", "error"})
        return cls(bool(failing), reads)

    @property
    def label(self) -> str:
        return f"write {'fails' if self.write_fails else 'works'}, reads {sorted(self.reads)}"
