"""Regression tests for importing GT7Patcher-HUD's saved snapshot layout."""

import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request


HERE = Path(__file__).resolve().parents[1]
HOST = HERE / "build" / "gt7patcher-host"


def snapshot(mask: int, race_info: bool = False, debug_environment: bool = False) -> str:
    metadata = ('<Metadata Name="Race Info fifth blank option (experimental)" />\n'
                if race_info else '')
    line = ('<Line Type="bytes" Address="0x060e83b0" Value="64656275670000" />\n'
            if debug_environment else '')
    return ("<Patch>\n" + metadata +
            line +
            f"<!-- GT7CONFIG {mask} 1 1 2 0 0 2 2 1 0 -->\n</Patch>\n")


class HudConfigMigrationTests(unittest.TestCase):
    def status_for(self, text: str) -> dict:
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp)
            path = state / "xml_prospero" / "PPSA01317.xml"
            path.parent.mkdir()
            path.write_text(text)
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
            process = subprocess.Popen([str(HOST), str(state), str(port)], stdout=subprocess.DEVNULL)
            try:
                for _ in range(60):
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/status", timeout=1) as response:
                            return json.load(response)
                    except OSError:
                        time.sleep(.05)
                self.fail("host server did not start")
            finally:
                process.terminate()
                process.wait(timeout=8)

    def test_release_discards_live_hook_selection(self) -> None:
        text = snapshot(0x40).replace('<Patch>\n', '<Patch>\n<!-- GT7LIVEHOOK 1 -->\n')
        self.assertEqual(self.status_for(text)['config']['mask'], 0)

    def test_live_hook_and_wheel_remain_independent(self) -> None:
        text = snapshot(0x60).replace('<Patch>\n', '<Patch>\n<!-- GT7LIVEHOOK 1 -->\n')
        self.assertEqual(self.status_for(text)['config']['mask'], 0x20)

    def test_new_hud_wheel_bit_is_preserved_and_race_info_is_dropped(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x60, race_info=True))["config"]["mask"], 0x20)

    def test_new_hud_wheel_without_race_info_is_preserved(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x40))["config"]["mask"], 0x20)

    def test_legacy_wheel_bit_remains_supported(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x20))["config"]["mask"], 0x20)

    def test_race_info_only_is_not_enabled_in_base_payload(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x20, race_info=True))["config"]["mask"], 0)

    def test_native_debug_environment_bit_is_discarded(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x40, debug_environment=True))["config"]["mask"], 0)

    def test_faulty_first_build_debug_selection_is_discarded(self) -> None:
        old = snapshot(0x40).replace("<Patch>\n", '<Patch>\n<Line Type="bytes" Address="0x05ce83b0" Value="64656275670000" />\n')
        self.assertEqual(self.status_for(old)["config"]["mask"], 0)

    def test_native_wheel_is_preserved_while_debug_is_discarded(self) -> None:
        self.assertEqual(self.status_for(snapshot(0x60, debug_environment=True))["config"]["mask"], 0x20)


if __name__ == "__main__":
    unittest.main(verbosity=2)
