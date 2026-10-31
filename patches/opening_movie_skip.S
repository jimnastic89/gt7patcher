/* SPDX-License-Identifier: GPL-3.0-only
 * Allow skipping the opening movie on a new game in GT7 01.710.000.
 * The native IsFirstTimeOpeningMovie getter returns the bit which gates the
 * existing Circle-button skip path; forcing it false leaves the movie player
 * and all other first-time flags unchanged.
 */

.section .patch.0000.029126f0,"a",@progbits
.byte 0x31, 0xc0, 0xc3
