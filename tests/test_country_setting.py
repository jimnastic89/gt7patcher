"""Player-country payload setting: validation, emission and persistence.

Runs the host build (never the console) and exercises the /api/config round trip.
"""
from pathlib import Path
import json
import re
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "build" / "gt7patcher-host"

FULL = {"mask": 1, "preset": 1, "units": 1, "language": 2, "country": 14,
        "speed": 0, "mass": 0, "power": 2, "torque": 2, "temperature": 1, "autostart": 0}


class Host:
    def __init__(self, state: Path):
        self.state = state
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.process = subprocess.Popen([str(HOST), str(state), str(self.port)],
                                        stdout=subprocess.DEVNULL)

    def __enter__(self):
        for _ in range(60):
            try:
                self.status()
                return self
            except OSError:
                time.sleep(.05)
        raise AssertionError("host server did not start")

    def __exit__(self, *exc):
        self.process.terminate()
        self.process.wait(timeout=8)

    def status(self) -> dict:
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/api/status", timeout=1) as r:
            return json.load(r)

    def post_config(self, config: dict) -> dict:
        token = self.status()["token"]
        data = urllib.parse.urlencode(config).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/api/config", data=data,
                                         headers={"X-GT7-Token": token})
        with urllib.request.urlopen(request, timeout=2) as r:
            return json.load(r)

    def xml(self) -> str:
        return (self.state / "xml_prospero" / "PPSA01317.xml").read_text()


class CountrySettingTests(unittest.TestCase):
    def run_host(self, fn):
        with tempfile.TemporaryDirectory() as temp:
            with Host(Path(temp)) as host:
                return fn(host)

    def test_saved_country_is_persisted_and_round_trips(self):
        def body(host):
            host.post_config(FULL)
            xml = host.xml()
            self.assertIn("<!-- GT7FEATURE country -->", xml)
            # Typed InGarageRank::nationalFlag_ copy redirect + zero-pad stub.
            self.assertIn('Address="0x0117c098"', xml)
            self.assertIn('Address="0x0430ea10"', xml)
            # GB == 0x47,0x42 -> mov word [rcx+0x80],0x4247 inside the stub.
            self.assertRegex(xml, r'Address="0x0430ea10" Value="[0-9a-f]*66c781800000004742[0-9a-f]*"')
            # GT7CONFIG carries country last
            self.assertRegex(xml, r"<!-- GT7CONFIG \d+ \d+ \d+ \d+ \d+ \d+ \d+ \d+ \d+ \d+ 14 -->")
            return host.status()["config"]["country"]
        self.assertEqual(self.run_host(body), 14)

    def test_default_is_off_and_emits_nothing(self):
        def body(host):
            cfg = dict(FULL)
            cfg["country"] = -1
            host.post_config(cfg)
            xml = host.xml()
            self.assertNotIn("GT7FEATURE country", xml)
            return host.status()["config"]["country"]
        self.assertEqual(self.run_host(body), -1)

    def test_out_of_range_country_is_rejected(self):
        def body(host):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                host.post_config({**FULL, "country": 999})
            self.assertEqual(caught.exception.code, 400)
        self.run_host(body)

    def test_legacy_snapshot_without_country_loads_as_off(self):
        def body(host):
            path = host.state / "xml_prospero" / "PPSA01317.xml"
            path.parent.mkdir(parents=True, exist_ok=True)
            # pre-existing build wrote 10 positional fields, no country
            path.write_text("<Patch>\n<!-- GT7CONFIG 1 1 1 2 0 0 2 2 1 0 -->\n</Patch>\n")
            return host.status()["config"]["country"]
        self.assertEqual(self.run_host(body), -1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
