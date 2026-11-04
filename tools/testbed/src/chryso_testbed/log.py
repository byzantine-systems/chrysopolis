"""Structured logging through structlog.

Console rendering on a terminal, JSON lines otherwise, to stderr. Context
bound with `structlog.contextvars` follows each asyncio task.
"""

from __future__ import annotations

import logging
import sys
from enum import StrEnum

import structlog


class Format(StrEnum):
    AUTO = "auto"  # console on a terminal, JSON lines otherwise
    CONSOLE = "console"
    JSON = "json"


def configure(fmt: Format = Format.AUTO, level: int = logging.INFO) -> None:
    """Set structlog up for the whole process."""
    json = fmt is Format.JSON or (fmt is Format.AUTO and not sys.stderr.isatty())
    renderer: structlog.typing.Processor = (
        structlog.processors.JSONRenderer()
        if json
        else structlog.dev.ConsoleRenderer(colors=sys.stderr.isatty())
    )
    structlog.configure(
        processors=[
            structlog.contextvars.merge_contextvars,
            structlog.processors.add_log_level,
            structlog.processors.TimeStamper(fmt="iso", utc=True),
            structlog.processors.dict_tracebacks if json else structlog.processors.format_exc_info,
            renderer,
        ],
        wrapper_class=structlog.make_filtering_bound_logger(level),
        logger_factory=structlog.PrintLoggerFactory(sys.stderr),
        cache_logger_on_first_use=True,
    )
