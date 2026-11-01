"""Guard the ShellCore shortcut profile table against accidental 12.x regressions."""
import re
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "engine.c").read_text()


class IconProfileTests(unittest.TestCase):
    def test_known_firmware_profiles_match_the_validated_shellcore_table(self):
        found = {
            int(firmware, 16): tuple(int(value, 16) for value in values)
            for firmware, *values in re.findall(
                r"\{(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+)\}",
                SOURCE,
            )
        }
        self.assertEqual(found[0x1200], (0x2E0050, 0x2DD100, 0x1886360))
        self.assertEqual(found[0x1202], (0x2E0050, 0x2DD100, 0x1886360))
        self.assertEqual(found[0x1220], (0x2E0050, 0x2DD100, 0x1887200))
        self.assertEqual(found[0x1240], (0x2E0050, 0x2DD100, 0x1887200))
        self.assertEqual(found[0x1260], (0x2E0320, 0x2DD3D0, 0x188DFA0))
        self.assertEqual(found[0x1270], (0x2E0320, 0x2DD3D0, 0x188DFA0))

    def test_every_12x_shortcut_address_is_selected_from_the_profile(self):
        self.assertIn("module.mapbase+profile->smp_cave", SOURCE)
        self.assertIn("module.mapbase+profile->install_all", SOURCE)
        self.assertIn("module.mapbase+profile->title_dir", SOURCE)


if __name__ == "__main__":
    unittest.main(verbosity=2)
