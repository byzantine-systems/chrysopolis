"""Host ports for scenarios that share one network namespace.

The guest reaches the host at 10.0.2.2, which slirp maps to the host's
loopback. Parallel scenarios therefore need distinct host ports, so each one
asks for its own instead of using fixed numbers.
"""

from __future__ import annotations

import socket


class Ports:
    """Hands out loopback ports no other scenario in this run holds."""

    def __init__(self) -> None:
        self._taken: set[int] = set()

    def take(self) -> int:
        for _ in range(64):
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.bind(("127.0.0.1", 0))
                port = int(s.getsockname()[1])
            if port not in self._taken:
                self._taken.add(port)
                return port
        raise RuntimeError("no free loopback port after 64 tries")
