"""Capture the writer of GT7's transient race-participant country field.

The address is session-specific.  The watcher attaches through ps5debug-NG,
arms a four-byte write watchpoint, records the first hit, then immediately
continues and detaches so it does not leave the game paused.
"""
from __future__ import annotations

import json
import sys
import time
import traceback
from pathlib import Path

from ps5dbg import PS5Debug


HOST = "10.0.0.24"
PID = 260
FIELD_ADDRESS = 0xE41E121F38
OUTPUT = Path.home() / "AppData" / "Local" / "Temp" / "gt7-country-watch.json"


def save(result: dict[str, object]) -> None:
    OUTPUT.write_text(json.dumps(result), encoding="ascii")


def main() -> int:
    result: dict[str, object] = {"state": "starting", "address": hex(FIELD_ADDRESS)}
    try:
        with PS5Debug(HOST, timeout=10) as ps:
            session = ps.debug_session(interrupt_port=755)
            session.attach(PID)
            # DR7 len=3 means four bytes; breaktype=1 is write-only.
            session.set_watchpoint(0, 1, FIELD_ADDRESS, 3, 1)
            result.update(state="armed", armedAt=time.time())
            save(result)
            event = session.events().get(timeout=600)
            result.update(
                state="hit",
                lwpid=event.lwpid,
                thread=event.tdname,
                status=event.status,
                registers={name: hex(value) for name, value in event.regs.as_dict().items()},
            )
            save(result)
            session.continue_()
            session.detach()
    except Exception as exc:  # Record failures so the caller can safely retry.
        result.update(state="error", error=repr(exc), trace=traceback.format_exc())
        save(result)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
