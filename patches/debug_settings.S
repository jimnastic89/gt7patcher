/* SPDX-License-Identifier: GPL-3.0-only
 * Debug Settings patch regions for GT7 01.710.000.
 * Mechanically converted from data/PPSA01317-original.xml.
 */

.section .patch.0000.012a40e0,"a",@progbits
.byte 0xc6, 0x07, 0x01, 0xc3, 0x90

.section .patch.0001.012b4b10,"a",@progbits
.byte 0xc6, 0x07, 0x01, 0xc3, 0x90

.section .patch.0002.0282e180,"a",@progbits
.byte 0x31, 0xc0, 0xc3

.section .patch.0003.00c21e98,"a",@progbits
.byte 0xeb, 0x03
