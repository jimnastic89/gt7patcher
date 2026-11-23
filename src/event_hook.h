/* SPDX-License-Identifier: GPL-3.0-only
 * Event prize hook for GT7 01.710.000 (see event_hook.S).
 *
 * FUN_0086a320 (RVA) loads one event's GameParameter by id from
 * /game_parameter/gp.gpb (or game_parameter/gp/<id>.json). Every World
 * Circuits event goes through it, and GT7 loads an event again each time it
 * is opened. The hook calls the loader, then, when the id is in its table,
 * rewrites the event's prize table (EventParameter = *(GameParameter + 0x10),
 * vector<int> at +0xd0) so that 1st place pays the listed amount and every
 * other place keeps its ratio to 1st, rounded up to 100 Cr like the game's own
 * prize maths. The rewrite is absolute, so applying it twice changes nothing.
 * Every load is logged with the event's track label, the std::string at the
 * start of *(GameParameter + 0x20).
 *
 * The code lives in the zero padding after the relocation-read-only segment
 * (RVA 0x55e7b40 to 0x55e8000, after Offline Mode's data); log and table live
 * in the zero padding after .bss (RVA 0x7156ec8 to 0x7158000). The payload
 * makes the first executable, writes both, and only then replaces the loader's
 * first 8 bytes with a jump to the hook. */
#pragma once

#define EVENT_HOOK_RVA 0x055e7b40
#define EVENT_HOOK_LIMIT 0x055e8000
#define EVENT_HOOK_SITE_RVA 0x0086a320
#define EVENT_HOOK_MAGIC "G7EVHK03"
#define EVENT_HOOK_ABI 3
#define EVENT_HOOK_SIZE 0x200 /* header and code */

/* Header fields (offsets into the hook). */
#define EVENT_HOOK_OFF_ABI 0x08
#define EVENT_HOOK_OFF_COUNT 0x0c   /* table entries in use, written by the payload */
#define EVENT_HOOK_OFF_HEAD 0x10    /* log write counter */
#define EVENT_HOOK_OFF_LOADS 0x14   /* events loaded since install */
#define EVENT_HOOK_OFF_CHANGED 0x18 /* loads whose prizes were rewritten */

/* Log of recent loads, then the table. The payload writes the magic and the
 * table; the hook writes log entries. */
#define EVENT_LOG_RVA 0x07156f00
#define EVENT_LOG_LIMIT 0x07158000
#define EVENT_LOG_MAGIC "G7EVLG03"
#define EVENT_LOG_OFF_ENTRIES 0x40
#define EVENT_LOG_ENTRY 48
#define EVENT_LOG_COUNT 32
#define EVENT_LOG_COURSE 32
#define EVENT_TABLE_OFF 0x640 /* entries + count * entry */
#define EVENT_HOOK_TABLE_MAX ((EVENT_LOG_LIMIT - EVENT_LOG_RVA - EVENT_TABLE_OFF) / 8)

/* Prizes are int; the largest multiple of 100 that fits. */
#define EVENT_HOOK_PRIZE_MAX 2147483600

#ifndef __ASSEMBLER__
#include <stdint.h>
typedef struct
{
    uint32_t id, original_first, new_first, places;
    char course[EVENT_LOG_COURSE];
} EventHookLog;
typedef struct
{
    uint32_t id, first;
} EventHookEntry;
_Static_assert(sizeof(EventHookLog) == EVENT_LOG_ENTRY, "log entry size");
_Static_assert(EVENT_LOG_OFF_ENTRIES + EVENT_LOG_COUNT * EVENT_LOG_ENTRY == EVENT_TABLE_OFF, "table follows the log");
_Static_assert(EVENT_HOOK_TABLE_MAX >= 300, "table fits every World Circuits event");
#endif
