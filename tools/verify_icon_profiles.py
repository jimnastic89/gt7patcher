"""Verify GT7Patcher's 12.00-13.60 shortcut profiles against ShadowMountPlus."""

import re
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "engine.c"
EXPECTED = {
    0x1200: (0x2E0050, 0x2DD100, 0x1886360),
    0x1202: (0x2E0050, 0x2DD100, 0x1886360),
    0x1220: (0x2E0050, 0x2DD100, 0x1887200),
    0x1240: (0x2E0050, 0x2DD100, 0x1887200),
    0x1260: (0x2E0320, 0x2DD3D0, 0x188DFA0),
    0x1270: (0x2E0320, 0x2DD3D0, 0x188DFA0),
    0x1300: (0x31F6E0, 0x31C790, 0x1958090),
    0x1320: (0x31FF20, 0x31CFD0, 0x19619F0),
    0x1340: (0x3200A0, 0x31D150, 0x1961B70),
    0x1342: (0x3200A0, 0x31D150, 0x1961B70),
    0x1360: (0x320C40, 0x31DCF0, 0x1962700),
}


def main():
    profiles = {
        int(firmware, 16): tuple(int(value, 16) for value in values)
        for firmware, *values in re.findall(
            r"\{(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+)\}",
            SOURCE.read_text(),
        )
    }
    for firmware, expected in EXPECTED.items():
        actual = profiles.get(firmware)
        if actual != expected:
            raise SystemExit(
                f"firmware {firmware >> 8:x}.{firmware & 0xff:02x}: "
                f"expected {expected!r}, got {actual!r}"
            )
    print(f"verified {len(EXPECTED)} ShadowMountPlus shortcut profiles through 13.60")


if __name__ == "__main__":
    main()
