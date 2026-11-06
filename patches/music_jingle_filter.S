/* SPDX-License-Identifier: GPL-3.0-only
 * Silence the short music jingles that ignore the player's music volume in
 * GT7 01.710.000. The named-music player (0xb4e380) receives every music asset
 * by name; the looping tracks and the race intros are unaffected, but the 5-10
 * second one-shot stings (race start, race finish, car purchase) share the
 * "race_jin*"/"race_cli*"/"car_get*" name prefixes. Filtering on the name at the
 * single entry point removes only those one-shots while leaving every other
 * music cue and all sound effects untouched - unlike muting the audio backend.
 *
 * The entry jumps into a zero-filled code cave (the text segment is mapped RWX
 * so no permissions change is needed) which returns 0 for a blocked name and
 * otherwise replays the displaced prologue in a second cave. Both caves sit in
 * the zero-fill gaps between existing functions, which are not touched by any
 * other patch in this repository.
 */

/* Entry: the 6-byte prologue pushes rbp, sets it up and pushes r15. Replace it
 * with a near jump to cave A. */
.section .patch.0000.00b4e380,"a",@progbits
.byte 0xe9, 0x97, 0x11, 0x7c, 0x03, 0x90

/* Cave A (0x24 bytes): return 0 when rsi begins with "car_" or with "race"
 * followed by 'j' or 'c'; otherwise continue in cave B. */
.section .patch.0001.0430f51c,"a",@progbits
.byte 0x81, 0x3e, 0x63, 0x61, 0x72, 0x5f, 0x74, 0x19, 0x81, 0x3e, 0x72, 0x61
.byte 0x63, 0x65, 0x75, 0x0c, 0x80, 0x7e, 0x05, 0x6a, 0x74, 0x0b, 0x80, 0x7e
.byte 0x05, 0x63, 0x74, 0x05, 0xe9, 0xa5, 0xf9, 0xff, 0xff, 0x31, 0xc0, 0xc3

/* Cave B: replay the displaced prologue bytes and jump back to 0xb4e386. */
.section .patch.0002.0430eee2,"a",@progbits
.byte 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0xe9, 0x99, 0xf4, 0x83, 0xfc
