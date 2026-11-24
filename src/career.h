/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Gran Turismo 7 (PPSA01317, 01.710.000) keeps two kinds of career save:
 *
 * memory.dat, in the save "sce_sdmemory": the online game's local copy of
 * the career, a list of { u32 size; char name[8]; u8 body[size]; } ending at
 * a zero size.
 *
 * gt7save.bin, in the save "gt7offline": written by the Offline Mode patch
 * (gt7patcher), a list of { u64 index; u32 flag; u32 size; u64 stamp;
 * char name[32]; u8 body[size]; }. The patch's loader checks index < 60 and
 * size < 16 MiB, then hands each body to the game by its index. A flag of
 * 0xFFFFFFFF marks a record the game keeps in a save of its own (driver file,
 * liveries) and stores no body.
 *
 * Both hold the same record bodies (4-byte magic, big-endian u16 version),
 * so the online career moves over by putting each online body into the
 * offline save in place of the record with the same name.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GT7_NAME_MAX 32
#define GT7_EXTERNAL 0xFFFFFFFFu
#define GT7_MAX_INDEX 60u
#define GT7_MAX_BODY 0x1000000u
#define GT7_OFFLINE_HEADER 56u
#define GT7_MEMORY_HEADER 12u

typedef struct
{
    char name[GT7_NAME_MAX + 1];
    uint64_t index;
    uint32_t flag;
    uint32_t size;
    uint64_t stamp;
    size_t offset;
    const uint8_t *body;
} gt7_record;

typedef struct
{
    gt7_record *items;
    size_t count;
    size_t cap;
    size_t end;
    int salvaged;
} gt7_records;

typedef struct
{
    char name[GT7_NAME_MAX + 1];
    int from_online;
    uint32_t size;
    int online_version;
    int offline_version;
} gt7_merge_line;

typedef struct
{
    gt7_merge_line lines[128];
    size_t count;
    size_t carried;
    char dropped[512];
    char newer[512];
} gt7_merge_report;

void gt7_records_free(gt7_records *r);
const gt7_record *gt7_find(const gt7_records *r, const char *name);
int gt7_record_version(const gt7_record *r);
size_t gt7_records_bytes(const gt7_records *r);

/* Parse a gt7save.bin. Returns 0, or -1 with a reason in err. */
int gt7_parse_offline(const uint8_t *data, size_t len, gt7_records *out, char *err, size_t errlen);

/* Parse a memory.dat. When its record list holds no GARAGE and an offline
 * save is given as a pattern, the rest of the file is searched for records
 * the offline save also has (same name and magic): a game that rewrote the
 * start of the file leaves the older records behind it. Sets out->salvaged
 * when the records came from that search. Returns the record count. */
size_t gt7_parse_memory(const uint8_t *data, size_t len, const gt7_records *pattern, gt7_records *out);

/* Build a new gt7save.bin: the offline save's records in its order, each with
 * the online body when the online save has a record of that name. Returns 0,
 * or -1 with a reason in err. *out is malloc'd. */
int gt7_merge(const gt7_records *online, const gt7_records *offline, uint8_t **out, size_t *outlen,
              gt7_merge_report *rep, char *err, size_t errlen);
