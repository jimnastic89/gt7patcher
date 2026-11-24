/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host test for the merge.
 *
 *   test_career <memory.dat> <gt7save.bin> [expected gt7save.bin]
 *
 * Merges the two saves and, when an expected file is given, checks the result
 * against it byte for byte. Then repeats the merge on a copy of memory.dat
 * whose start was overwritten the way a fresh game would write it (one
 * DRIVERIV record and the end marker), which must recover the same career.
 */
#include "../src/career.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        fprintf(stderr, "%s: read failed\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int merge(const uint8_t *mem, size_t mem_len, const gt7_records *offline, uint8_t **out, size_t *out_len,
                 int expect_salvaged)
{
    gt7_records online;
    gt7_parse_memory(mem, mem_len, offline, &online);
    printf("online: %zu records, %zu bytes, salvaged=%d\n", online.count, gt7_records_bytes(&online), online.salvaged);
    if (online.salvaged != expect_salvaged)
    {
        printf("FAIL: salvaged=%d, expected %d\n", online.salvaged, expect_salvaged);
        return 1;
    }
    gt7_merge_report rep;
    char err[256] = "";
    if (gt7_merge(&online, offline, out, out_len, &rep, err, sizeof(err)))
    {
        printf("FAIL: %s\n", err);
        return 1;
    }
    for (size_t i = 0; i < rep.count; i++)
        printf("  %-12s %-7s %8u  v%d -> v%d\n", rep.lines[i].name, rep.lines[i].from_online ? "online" : "kept",
               rep.lines[i].size, rep.lines[i].online_version, rep.lines[i].offline_version);
    printf("carried %zu, dropped: %s, newer: %s, size %zu\n", rep.carried, rep.dropped, rep.newer[0] ? rep.newer : "-",
           *out_len);
    gt7_records_free(&online);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <memory.dat> <gt7save.bin> [expected]\n", argv[0]);
        return 2;
    }
    size_t mem_len, off_len, exp_len = 0;
    uint8_t *mem = load(argv[1], &mem_len);
    uint8_t *off = load(argv[2], &off_len);
    uint8_t *exp = argc > 3 ? load(argv[3], &exp_len) : NULL;

    gt7_records offline;
    char err[256] = "";
    if (gt7_parse_offline(off, off_len, &offline, err, sizeof(err)))
    {
        printf("FAIL: offline save: %s\n", err);
        return 1;
    }
    printf("offline: %zu records, %zu bytes\n", offline.count, gt7_records_bytes(&offline));

    uint8_t *out;
    size_t out_len;
    if (merge(mem, mem_len, &offline, &out, &out_len, 0))
        return 1;
    if (exp && (out_len != exp_len || memcmp(out, exp, out_len)))
    {
        printf("FAIL: result differs from %s (%zu vs %zu bytes)\n", argv[3], out_len, exp_len);
        return 1;
    }

    /* A fresh game's memory.dat: DRIVERIV at offset 0, then the end marker. */
    const gt7_record *driver = gt7_find(&offline, "DRIVERIV");
    if (driver && driver->size + 16 < mem_len)
    {
        uint8_t *damaged = malloc(mem_len);
        memcpy(damaged, mem, mem_len);
        damaged[0] = (uint8_t)driver->size;
        damaged[1] = (uint8_t)(driver->size >> 8);
        damaged[2] = damaged[3] = 0;
        memset(damaged + 4, 0, 8);
        memcpy(damaged + 4, "DRIVERIV", 8);
        memcpy(damaged + 12, driver->body, driver->size);
        memset(damaged + 12 + driver->size, 0, 4);
        uint8_t *out2;
        size_t out2_len;
        if (merge(damaged, mem_len, &offline, &out2, &out2_len, 1))
            return 1;
        if (out2_len != out_len || memcmp(out, out2, out_len))
        {
            printf("FAIL: recovered career differs from the intact one\n");
            return 1;
        }
        free(out2);
        free(damaged);
    }

    gt7_records empty;
    uint8_t zero[64] = {0};
    if (gt7_parse_memory(zero, sizeof(zero), &offline, &empty) != 0)
    {
        printf("FAIL: an empty memory.dat produced records\n");
        return 1;
    }
    gt7_records_free(&empty);

    printf("PASS\n");
    free(out);
    gt7_records_free(&offline);
    free(mem);
    free(off);
    free(exp);
    return 0;
}
