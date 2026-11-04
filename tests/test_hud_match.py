"""HUD key matching, on the host, against the real bridge source.

The game mounts the race HUD twice - RaceDisplay/Container/objective and
RaceDisplay/Container/subjective - and the camera picks which copy is drawn
(bumper cam draws the subjective one). A selection saved against one container
must hide the other, or the element stays on screen when the camera changes.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "live_bridge.c"
#include <assert.h>
#include <string.h>

static int match(const char *entry, const char *name, const char *path, const char *alias) {
    memset((void *)live_mailbox.hud_entries, 0, sizeof(live_mailbox.hud_entries));
    if (entry) memcpy((void *)live_mailbox.hud_entries[0], entry, strlen(entry));
    return hud_match(&live_mailbox, 1, name, path, alias);
}

int main(void) {
    /* the same path matches itself */
    assert(match("RaceDisplay/Container/objective/ABS", "ABS",
                 "RaceDisplay/Container/objective/ABS", ""));
    /* ... and the other camera's copy of the same widget */
    assert(match("RaceDisplay/Container/objective/ABS", "ABS",
                 "RaceDisplay/Container/subjective/ABS", ""));
    assert(match("RaceDisplay/Container/subjective/SpeedMeter/dial", "dial",
                 "RaceDisplay/Container/objective/SpeedMeter/dial", ""));
    assert(match("RaceDisplay/Container/subjective", "subjective",
                 "RaceDisplay/Container/objective", ""));
    /* the containers are only interchangeable as a segment: a widget nested one
     * level deeper in one copy than the other stays distinct */
    assert(!match("RaceDisplay/Container/objective/MFDNav/MfdNavDots", "MfdNavDots",
                  "RaceDisplay/Container/subjective/MfdNavDots", ""));
    /* a sibling, a child, a parent and a different case do not match */
    assert(!match("RaceDisplay/Container/objective/ABS", "TCS",
                  "RaceDisplay/Container/objective/TCS", ""));
    assert(!match("RaceDisplay/Container/objective/ABS", "ABS",
                  "RaceDisplay/Container/objective/ABS/Icon", ""));
    assert(!match("RaceDisplay/Container/objective/ABS", "Icon",
                  "RaceDisplay/Container/objective/ABS/Icon", ""));
    assert(!match("RaceDisplay/Container/Objective/ABS", "ABS",
                  "RaceDisplay/Container/objective/ABS", ""));
    assert(!match("RaceDisplay/Container/objectivex/ABS", "ABS",
                  "RaceDisplay/Container/subjective/ABS", ""));
    /* a bare name still matches every widget with that name, in either copy */
    assert(match("ABS", "ABS", "RaceDisplay/Container/subjective/ABS", ""));
    assert(!match("ABS", "TCS", "RaceDisplay/Container/subjective/TCS", ""));
    /* the compatibility path (the old reader saw inline names only) keeps
     * working, in both copies */
    assert(match("RaceDisplay/Container/objective/Icon", "Icon",
                 "RaceDisplay/Container/objective/BrakeAssist_icon/Icon",
                 "RaceDisplay/Container/objective/Icon"));
    assert(match("RaceDisplay/Container/objective/Icon", "Icon",
                 "RaceDisplay/Container/subjective/BrakeAssist_icon/Icon",
                 "RaceDisplay/Container/subjective/Icon"));
    /* an empty path matches nothing, and an empty entry matches nothing */
    assert(!match("RaceDisplay/Container/objective/ABS", "ABS", "", ""));
    assert(!match("", "ABS", "RaceDisplay/Container/objective/ABS", ""));
    /* the comparators themselves */
    assert(hud_same_view("RaceDisplay/Container/objective/ABS", "RaceDisplay/Container/subjective/ABS"));
    assert(hud_same_view("objective", "subjective"));
    assert(hud_same_view("", ""));
    assert(!hud_same_view("objective/X", "subjective"));
    assert(!hud_same_view("a/objectivex/b", "a/subjective/b"));
    assert(!hud_same_view("a/x/b", "a/y/b"));
    assert(!hud_same_view("a/b", "a/b/c"));
    return 0;
}
'''


class HudMatchTests(unittest.TestCase):
    def test_paths_match_across_the_two_camera_copies(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'hud_match.c').write_text(FIXTURE)
            binary = path / 'hud_match'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), str(path / 'hud_match.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
