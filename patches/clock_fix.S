/* SPDX-License-Identifier: GPL-3.0-only
 * Sync Game Clock to System marker for GT7 01.710.000.
 * Not a byte patch: src/engine.c resolves libSceRtc in the live game at launch
 * and retargets the clock source of sceRtcGetCurrentTick (apply_clock_fix). The
 * engine matches only this address; the byte below is a placeholder.
 */

.section .patch.0000.060e83c0,"a",@progbits
.byte 0x00
