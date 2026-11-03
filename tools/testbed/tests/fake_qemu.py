"""A stand-in for qemu-system-aarch64 in unit tests.

It connects to the QMP socket named on its command line, answers every
command, and plays the script in FAKE_QEMU_SCRIPT (JSON) on the console:

    ["out", text]     print text
    ["expect", text]  read stdin until a line contains text, then echo it
    ["event", name]   send a QMP event
    ["sleep", secs]   pause
    ["hang"]          wait until killed
"""

from __future__ import annotations

import json
import os
import socket
import sys
import threading
import time
from typing import BinaryIO


def qmp_path(argv: list[str]) -> str:
    for spec in argv:
        if spec.startswith("socket,id=qmp,path="):
            return spec.removeprefix("socket,id=qmp,path=")
    raise SystemExit("no QMP chardev")


GREETING = {
    "QMP": {
        "version": {"qemu": {"major": 11, "minor": 1, "micro": 1}, "package": ""},
        "capabilities": [],
    }
}


def send(conn: socket.socket, message: object) -> None:
    conn.sendall(json.dumps(message).encode() + b"\r\n")


def serve(conn: socket.socket) -> None:
    """Answer each JSON request. QMP sends no newline, so decode a stream."""
    send(conn, GREETING)
    decoder = json.JSONDecoder()
    pending = ""
    while data := conn.recv(4096):
        pending += data.decode()
        while pending.strip():
            try:
                request, end = decoder.raw_decode(pending.lstrip())
            except json.JSONDecodeError:
                break
            pending = pending.lstrip()[end:]
            send(conn, {"return": {}, **({"id": request["id"]} if "id" in request else {})})


def read_line(stream: BinaryIO) -> bytes:
    """Bytes up to and including CR, or b"" at EOF."""
    line = bytearray()
    while (byte := stream.read(1)) and byte != b"\r":
        line += byte
    return bytes(line + byte)


def main() -> None:
    conn = socket.socket(socket.AF_UNIX)
    conn.connect(qmp_path(sys.argv[1:]))
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
    out = sys.stdout.buffer
    for action, *args in json.loads(os.environ.get("FAKE_QEMU_SCRIPT", "[]")):
        match action:
            case "out":
                out.write(args[0].encode())
                out.flush()
            case "expect":
                # Typed lines end in CR, as on a serial console. Echo each one.
                for line in iter(lambda: read_line(sys.stdin.buffer), b""):
                    out.write(line + b"\n")
                    out.flush()
                    if args[0].encode() in line:
                        break
            case "event":
                stamp = {"seconds": 0, "microseconds": 0}
                send(conn, {"event": args[0], "data": {}, "timestamp": stamp})
            case "sleep":
                time.sleep(args[0])
            case "hang":
                threading.Event().wait()


if __name__ == "__main__":
    main()
