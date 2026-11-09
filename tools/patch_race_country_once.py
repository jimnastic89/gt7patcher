"""One-session test for GT7's local race-participant country field.

The opponent roster and its allocations vary between races, so this locates the
local participant by its observed name and uses the fixed country slot 0x30
bytes after that small-string buffer.  It then changes precisely ``UN\\0`` to
``AU\\0`` through ps5debug-NG's documented two-phase process-write protocol.
The stock ps5dbg 0.1.1 client incorrectly places the trailing bytes in the
request body and therefore blocks forever.
"""
from __future__ import annotations

import json
import struct
import time
from pathlib import Path

from ps5dbg import PS5Debug
from ps5dbg.constants import Cmd
from ps5dbg.wire import expect_success

HOST = "10.0.0.24"
PID = 260
RACE_HEAP_START = 0xE41D000000
RACE_HEAP_LENGTH = 0x02000000
CHUNK = 0x00100000
LOCAL_NAME = b"Turd Ferguson\0"
OUTPUT = Path.home() / "AppData" / "Local" / "Temp" / "gt7-race-country-test.json"


def save(value: dict[str, object]) -> None:
    OUTPUT.write_text(json.dumps(value), encoding="ascii")


def write_exact(ps: PS5Debug, address: int, data: bytes) -> None:
    conn = ps.connection
    conn.send_request(Cmd.PROC_WRITE, struct.pack("<IQI", PID, address, len(data)))
    expect_success(conn, context="race-country write acknowledgement")
    conn._sendall(data)  # The debugger receives the bytes only after its ACK.
    expect_success(conn, context="race-country write completion")


def main() -> None:
    state: dict[str, object] = {"state": "armed", "name": LOCAL_NAME[:-1].decode("ascii")}
    save(state)
    deadline = time.monotonic() + 600
    with PS5Debug(HOST, timeout=5) as ps:
        while time.monotonic() < deadline:
            for offset in range(0, RACE_HEAP_LENGTH, CHUNK):
                base = RACE_HEAP_START + offset
                blob = ps.read(PID, base, CHUNK)
                at = blob.find(LOCAL_NAME)
                if at < 0:
                    continue
                address = base + at + 0x30
                if ps.read(PID, address, 3) != b"UN\0":
                    continue
                write_exact(ps, address, b"AU\0")
                after = ps.read(PID, address, 3)
                state.update(state="written", address=hex(address), value=after.decode("ascii", "replace"))
                save(state)
                return
            time.sleep(0.2)
    state.update(state="timeout")
    save(state)


if __name__ == "__main__":
    main()
