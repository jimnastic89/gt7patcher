/* SPDX-License-Identifier: GPL-3.0-only */
#include "career.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int push(gt7_records *r, const gt7_record *rec)
{
    if (r->count == r->cap)
    {
        size_t cap = r->cap ? r->cap * 2 : 64;
        gt7_record *items = realloc(r->items, cap * sizeof(*items));
        if (!items)
            return -1;
        r->items = items;
        r->cap = cap;
    }
    r->items[r->count++] = *rec;
    return 0;
}

void gt7_records_free(gt7_records *r)
{
    free(r->items);
    memset(r, 0, sizeof(*r));
}

const gt7_record *gt7_find(const gt7_records *r, const char *name)
{
    for (size_t i = 0; i < r->count; i++)
        if (!strcmp(r->items[i].name, name))
            return &r->items[i];
    return NULL;
}

int gt7_record_version(const gt7_record *r)
{
    if (!r->body || r->size < 6)
        return -1;
    return r->body[4] << 8 | r->body[5];
}

size_t gt7_records_bytes(const gt7_records *r)
{
    size_t n = 0;
    for (size_t i = 0; i < r->count; i++)
        n += r->items[i].size;
    return n;
}

/* A record name: upper-case letters, digits and '_', then only NUL bytes. */
static int read_name(const uint8_t *p, size_t n, char *out)
{
    size_t len = 0;
    while (len < n && p[len])
    {
        uint8_t c = p[len];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
        out[len] = (char)c;
        len++;
    }
    out[len] = 0;
    for (size_t i = len; i < n; i++)
        if (p[i])
            return 0;
    return len > 0;
}

int gt7_parse_offline(const uint8_t *data, size_t len, gt7_records *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    size_t off = 0;
    while (off + GT7_OFFLINE_HEADER <= len)
    {
        const uint8_t *h = data + off;
        gt7_record rec = {0};
        rec.index = le64(h);
        rec.flag = le32(h + 8);
        rec.size = le32(h + 12);
        rec.stamp = le64(h + 16);
        rec.offset = off;
        if (!rec.index && !rec.flag && !rec.size && !rec.stamp && !h[24])
            break;
        if (!read_name(h + 24, GT7_NAME_MAX, rec.name))
        {
            fail(err, errlen, "unreadable record name at offset %zu", off);
            gt7_records_free(out);
            return -1;
        }
        if (rec.size > len - off - GT7_OFFLINE_HEADER)
        {
            fail(err, errlen, "record %s runs past the end of the file", rec.name);
            gt7_records_free(out);
            return -1;
        }
        rec.body = h + GT7_OFFLINE_HEADER;
        if (push(out, &rec))
        {
            fail(err, errlen, "out of memory");
            gt7_records_free(out);
            return -1;
        }
        off += GT7_OFFLINE_HEADER + rec.size;
    }
    out->end = off;
    for (size_t i = off; i < len; i++)
        if (data[i])
        {
            fail(err, errlen, "unexpected data after the last record (offset %zu)", off);
            gt7_records_free(out);
            return -1;
        }
    return 0;
}

static int memory_header(const uint8_t *data, size_t len, size_t off, gt7_record *rec)
{
    if (off + GT7_MEMORY_HEADER > len)
        return 0;
    uint32_t size = le32(data + off);
    if (!size || size >= GT7_MAX_BODY || size > len - off - GT7_MEMORY_HEADER)
        return 0;
    memset(rec, 0, sizeof(*rec));
    if (!read_name(data + off + 4, 8, rec->name))
        return 0;
    rec->size = size;
    rec->offset = off;
    rec->body = data + off + GT7_MEMORY_HEADER;
    return 1;
}

size_t gt7_parse_memory(const uint8_t *data, size_t len, const gt7_records *pattern, gt7_records *out)
{
    memset(out, 0, sizeof(*out));
    size_t off = 0;
    gt7_record rec;
    while (memory_header(data, len, off, &rec))
    {
        if (push(out, &rec))
            break;
        off += GT7_MEMORY_HEADER + rec.size;
    }
    out->end = off;
    if (gt7_find(out, "GARAGE") || !pattern)
        return out->count;

    gt7_records found = {0};
    for (size_t at = off; at + GT7_MEMORY_HEADER <= len;)
    {
        const gt7_record *like;
        if (memory_header(data, len, at, &rec) && rec.size >= 4 && (like = gt7_find(pattern, rec.name)) &&
            like->flag != GT7_EXTERNAL && like->size >= 4 && !memcmp(like->body, rec.body, 4))
        {
            if (push(&found, &rec))
                break;
            at += GT7_MEMORY_HEADER + rec.size;
            continue;
        }
        at++;
    }
    if (!gt7_find(&found, "GARAGE"))
    {
        gt7_records_free(&found);
        return out->count;
    }
    gt7_records_free(out);
    *out = found;
    out->end = len;
    out->salvaged = 1;
    return out->count;
}

static void append_name(char *list, size_t cap, const char *name)
{
    size_t n = strlen(list);
    snprintf(list + n, cap - n, "%s%s", n ? ", " : "", name);
}

int gt7_merge(const gt7_records *online, const gt7_records *offline, uint8_t **out, size_t *outlen,
              gt7_merge_report *rep, char *err, size_t errlen)
{
    memset(rep, 0, sizeof(*rep));
    *out = NULL;
    *outlen = 0;
    if (offline->count > sizeof(rep->lines) / sizeof(rep->lines[0]))
    {
        fail(err, errlen, "the offline save has %zu records, more than expected", offline->count);
        return -1;
    }
    size_t total = 0;
    for (size_t i = 0; i < offline->count; i++)
    {
        const gt7_record *r = &offline->items[i];
        const gt7_record *o = r->flag == GT7_EXTERNAL ? NULL : gt7_find(online, r->name);
        total += GT7_OFFLINE_HEADER + (o ? o->size : r->size);
    }
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf)
    {
        fail(err, errlen, "out of memory");
        return -1;
    }
    size_t at = 0;
    for (size_t i = 0; i < offline->count; i++)
    {
        const gt7_record *r = &offline->items[i];
        const gt7_record *o = r->flag == GT7_EXTERNAL ? NULL : gt7_find(online, r->name);
        const gt7_record *src = o ? o : r;
        if (r->flag != GT7_EXTERNAL && (r->index >= GT7_MAX_INDEX || src->size >= GT7_MAX_BODY))
        {
            fail(err, errlen, "record %s is outside the Offline Mode loader's limits", r->name);
            free(buf);
            return -1;
        }
        put64(buf + at, r->index);
        put32(buf + at + 8, r->flag);
        put32(buf + at + 12, src->size);
        put64(buf + at + 16, r->stamp);
        memset(buf + at + 24, 0, GT7_NAME_MAX);
        memcpy(buf + at + 24, r->name, strlen(r->name));
        memcpy(buf + at + GT7_OFFLINE_HEADER, src->body, src->size);
        at += GT7_OFFLINE_HEADER + src->size;

        gt7_merge_line *line = &rep->lines[rep->count++];
        snprintf(line->name, sizeof(line->name), "%s", r->name);
        line->from_online = o != NULL;
        line->size = src->size;
        line->offline_version = gt7_record_version(r);
        line->online_version = o ? gt7_record_version(o) : -1;
        if (o)
        {
            rep->carried++;
            if (line->online_version > line->offline_version && line->offline_version >= 0)
                append_name(rep->newer, sizeof(rep->newer), r->name);
        }
    }
    for (size_t i = 0; i < online->count; i++)
    {
        const char *name = online->items[i].name;
        const gt7_record *r = gt7_find(offline, name);
        int listed = 0;
        for (size_t j = 0; j < i; j++)
            listed |= !strcmp(online->items[j].name, name);
        if ((!r || r->flag == GT7_EXTERNAL) && !listed)
            append_name(rep->dropped, sizeof(rep->dropped), name);
    }

    gt7_records check;
    char why[128] = "";
    if (gt7_parse_offline(buf, at, &check, why, sizeof(why)) || check.count != offline->count || check.end != at)
    {
        if (!why[0])
            snprintf(why, sizeof(why), "record count changed");
        fail(err, errlen, "the new save does not read back: %s", why);
        gt7_records_free(&check);
        free(buf);
        return -1;
    }
    gt7_records_free(&check);
    *out = buf;
    *outlen = at;
    return 0;
}
