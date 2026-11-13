/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include "live_bridge.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef PS5
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <ps5/kernel.h>
#include <pthread.h>
#include <stdatomic.h>
#include "engine.h"
#include "notify.h"

typedef struct
{
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[10];
    uint32_t apptype;
    char unknown2[0x36];
} AppInfo;
extern int sceKernelGetAppInfo(pid_t, AppInfo *);
#endif
#define VERSION "0.40"
#define WEB_HUD_EDITOR 0
#ifndef PORT
#define PORT 7210
#endif
static const char *root = "/data/gt7p";
static char token[33], message[256] = "";
static Config config;
#ifndef PS5
/* Host preview: the live bridge and its pad shortcut exist only on PS5. */
static void gt7_live_pad_configure(int enabled) { (void)enabled; }
static void gt7_live_pad_poll(void) {}
static int gt7_live_credit_auto_poll(void) { return -1; }
static int gt7_rev_limit_tick(void) { return -1; }
#endif

static int broken;
static volatile sig_atomic_t stopping;
#ifdef PS5
static atomic_int startup_pending;
static int update_notification_sent;
static pthread_t startup_thread;
static int startup_thread_created;
#endif
static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}
/* Rest mode can invalidate an existing listening socket while the payload
 * process itself survives.  Keep socket creation in one place so the main
 * loop can recover the web endpoint instead of exiting on the first EBADF or
 * ENOTSOCK returned by select/accept after wake. */
static int open_listener(unsigned port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
#ifdef PS5
    a.sin_addr.s_addr = htonl(INADDR_ANY);
#else
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
    if (bind(s, (void *)&a, sizeof(a)) || listen(s, 8))
    {
        close(s);
        return -1;
    }
    return s;
}
static int transfer(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n)
    {
        ssize_t r = send(fd, p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
static void response(int fd, int code, const char *type, const char *body)
{
    char h[512];
    size_t n = strlen(body);
    int z = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: frame-ancestors 'none'\r\nConnection: close\r\n\r\n", code, code == 200 ? "OK" : "Error", type, n);
    if (z > 0 && (size_t)z < sizeof(h) && !transfer(fd, h, (size_t)z))
        transfer(fd, body, n);
}
#ifdef PS5
/* Convert the JSON-like property maps embedded in resolved VM binding objects
 * into a small, read-only feed for the PowerShell inspector.  The objects are
 * not assumed to have a fixed C layout: only quoted keys followed by a colon
 * and scalar JSON values are emitted.  Binary metadata and unresolved values
 * are deliberately skipped. */
static int binding_json_escape(char *out, size_t cap, size_t *at, const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = s[i];
        if (c == '"' || c == '\\')
        {
            if (*at + 2 >= cap)
                return -1;
            out[(*at)++] = '\\';
            out[(*at)++] = (char)c;
        }
        else if (c >= 0x20 && c < 0x7f)
        {
            if (*at + 1 >= cap)
                return -1;
            out[(*at)++] = (char)c;
        }
    }
    return 0;
}
#define LIVE_BINDING_MAX_ROWS 1200u
static int binding_emit_rows(char *out, size_t cap, size_t *at, const unsigned char *raw, size_t len, uintptr_t source, unsigned *rows)
{
    for (size_t i = 1; i + 4 < len; i++)
    {
        if (*rows >= LIVE_BINDING_MAX_ROWS)
            return 0;
        /* Serialized property maps are frequently embedded after a binary
         * header, so the opening quote need not be immediately preceded by
         * '{' or ','. The key/colon/scalar grammar below is the validation. */
        if (raw[i] != '"')
            continue;
        size_t ks = i + 1;
        while (i + 1 < len && raw[i + 1] != '"' && raw[i + 1] >= 0x20 && raw[i + 1] < 0x7f)
            i++;
        if (i + 1 >= len || raw[i + 1] != '"' || i == ks || i + 1 - ks > 96 || i + 2 >= len || raw[i + 2] != ':')
            continue;
        size_t vs = i + 3;
        while (vs < len && (raw[vs] == ' ' || raw[vs] == '\t' || raw[vs] == '\r' || raw[vs] == '\n'))
            vs++;
        if (vs >= len)
            continue;
        if (raw[vs] == '{' || raw[vs] == '[')
            continue; /* nested object/array: not scalar */
        size_t ve = vs;
        if (raw[vs] == '"')
        {
            ve = vs + 1;
            int esc = 0;
            for (; ve < len; ve++)
            {
                if (!esc && raw[ve] == '"')
                    break;
                esc = !esc && raw[ve] == '\\';
                if (raw[ve] != '\\')
                    esc = 0;
            }
            if (ve >= len)
                continue;
            ve++;
        }
        else
        {
            if (raw[vs] != '-' && (raw[vs] < '0' || raw[vs] > '9') && raw[vs] != 't' && raw[vs] != 'f' && raw[vs] != 'n')
                continue;
            while (ve < len && raw[ve] != ',' && raw[ve] != '}' && raw[ve] != '\r' && raw[ve] != '\n' && raw[ve] >= 0x20 && raw[ve] < 0x7f)
                ve++;
            while (ve > vs && (raw[ve - 1] == ' ' || raw[ve - 1] == '\t'))
                ve--;
            if (ve == vs)
                continue;
        }
        if (*at + 256 + (i - ks) + (ve - vs) >= cap - 1)
            return -1;
        if (*rows)
            out[(*at)++] = ',';
        int n = snprintf(out + *at, cap - *at, "{\"name\":\"");
        if (n < 0)
            return -1;
        *at += (size_t)n;
        if (binding_json_escape(out, cap, at, raw + ks, i + 1 - ks))
            return -1;
        n = snprintf(out + *at, cap - *at, "\",\"value\":");
        if (n < 0)
            return -1;
        *at += (size_t)n;
        if (raw[vs] == '"')
        {
            out[(*at)++] = '"';
            if (binding_json_escape(out, cap, at, raw + vs + 1, (ve - vs) - 2))
                return -1;
            out[(*at)++] = '"';
        }
        else
        {
            memcpy(out + *at, raw + vs, ve - vs);
            *at += ve - vs;
        }
        n = snprintf(out + *at, cap - *at, ",\"source\":\"0x%llx\"}", (unsigned long long)source);
        if (n < 0)
            return -1;
        *at += (size_t)n;
        (*rows)++;
        i = ve;
    }
    return 0;
}
static int binding_emit_names(char *out, size_t cap, size_t *at, const unsigned char *raw, size_t len, uintptr_t source, unsigned *rows)
{
    for (size_t i = 0; i < len;)
    {
        if (*rows >= LIVE_BINDING_MAX_ROWS)
            return 0;
        while (i < len && (raw[i] < 0x20 || raw[i] >= 0x7f))
            i++;
        size_t start = i;
        while (i < len && raw[i] >= 0x20 && raw[i] < 0x7f)
            i++;
        size_t n = i - start;
        if (n < 5 || n > 96)
            continue;
        char name[97];
        memcpy(name, raw + start, n);
        name[n] = 0;
        if (!strchr(name, '_') || strchr(name, '/') || strchr(name, '.') || strchr(name, ':') || strchr(name, '?'))
            continue;
        if (!strncmp(name, "pdx", 3) && name[3])
        {
            memmove(name, name + 3, n - 2);
            n -= 3;
        }
        else if (!strncmp(name, "pd", 2) && name[2])
        {
            memmove(name, name + 2, n - 1);
            n -= 2;
        }
        if (n < 5 || !name[0])
            continue;
        if (*at + 256 + n >= cap - 1)
            return -1;
        if (*rows)
            out[(*at)++] = ',';
        int z = snprintf(out + *at, cap - *at, "{\"name\":\"");
        if (z < 0)
            return -1;
        *at += (size_t)z;
        if (binding_json_escape(out, cap, at, (const unsigned char *)name, n))
            return -1;
        z = snprintf(out + *at, cap - *at, "\",\"value\":\"<unresolved>\",\"source\":\"0x%llx\"}", (unsigned long long)source);
        if (z < 0)
            return -1;
        *at += (size_t)z;
        (*rows)++;
    }
    return 0;
}
static int binding_emit_flag_assets(char *out, size_t cap, size_t *at, const unsigned char *raw, size_t len, uintptr_t source, unsigned *rows)
{
    static const char prefix[] = "pd/image/piece/4k/flag_S/";
    for (size_t i = 0; i + sizeof(prefix) - 1 + 6 < len; i++)
    {
        if (memcmp(raw + i, prefix, sizeof(prefix) - 1))
            continue;
        size_t end = i + sizeof(prefix) - 1;
        while (end < len && raw[end] >= 'A' && raw[end] <= 'Z' && end - i < 64)
            end++;
        if (end == i + sizeof(prefix) - 1 || end + 4 > len || memcmp(raw + end, ".png", 4))
            continue;
        size_t n = end + 4 - i;
        if (*at + 256 + n >= cap - 1)
            return -1;
        if (*rows)
            out[(*at)++] = ',';
        int z = snprintf(out + *at, cap - *at, "{\"name\":\"asset_path\",\"value\":\"");
        if (z < 0)
            return -1;
        *at += (size_t)z;
        if (binding_json_escape(out, cap, at, raw + i, n))
            return -1;
        z = snprintf(out + *at, cap - *at, "\",\"source\":\"0x%llx\"}", (unsigned long long)source);
        if (z < 0)
            return -1;
        *at += (size_t)z;
        (*rows)++;
        i = end + 3;
    }
    return 0;
}
static int live_binding_feed(char *out, size_t cap, unsigned *rows, unsigned *objects)
{
#ifdef PS5
    uintptr_t words[64 * 32];
    uint32_t next = 0;
    unsigned char raw[8192];
    uintptr_t seen[32];
    unsigned seen_count = 0;
    if (gt7_live_probe_history(words, 64 * 32, &next))
        return -1;
    size_t at = 0;
    *rows = 0;
    *objects = 0;
    int n = snprintf(out, cap, "{\"available\":true,\"next\":%u,\"bindings\":[", next);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    at = (size_t)n;
    for (unsigned r = 0; r < 64; r++)
    {
        /* The image-binding dispatcher receives the useful binding object in
         * rdi, but its VM receiver/table/argument objects are in the other
         * argument registers. Inspect all six registers: race participant
         * country data can live in the receiver or argument table rather than
         * in the image object itself. */
        for (unsigned k = 0; k < 6; k++)
        {
            uintptr_t source = words[r * 32 + k];
            if (!source)
                continue;
            int duplicate = 0;
            for (unsigned j = 0; j < seen_count; j++)
                if (seen[j] == source)
                    duplicate = 1;
            if (duplicate)
                continue;
            if (seen_count < 32)
                seen[seen_count++] = source;
            else
                continue;
            if (gt7_live_peek(source, raw, sizeof(raw)))
                continue;
            (*objects)++;
            if (binding_emit_rows(out, cap, &at, raw, sizeof(raw), source, rows))
                return -1;
            if (binding_emit_names(out, cap, &at, raw, sizeof(raw), source, rows))
                return -1;
            if (binding_emit_flag_assets(out, cap, &at, raw, sizeof(raw), source, rows))
                return -1;
        }
    }
    if (at + 32 >= cap)
        return -1;
    snprintf(out + at, cap - at, "] ,\"objects\":%u,\"rows\":%u}", *objects, *rows);
    return 0;
#else
    (void)out;
    (void)cap;
    (void)rows;
    (void)objects;
    return -1;
#endif
}
/* Decode the text parameter used by the reusable notification endpoint. */
static size_t decode_query_text(const char *src, char *dst, size_t cap)
{
    size_t n = 0;
    while (*src && n + 1 < cap)
    {
        if (*src == '+')
        {
            dst[n++] = ' ';
            src++;
            continue;
        }
        if (*src == '%' && src[1] && src[2])
        {
            unsigned hi, lo;
            if (sscanf(src + 1, "%1x%1x", &hi, &lo) == 2)
            {
                dst[n++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        dst[n++] = *src++;
    }
    dst[n] = 0;
    return n;
}
#endif
static int running_gt7(void)
{
#ifdef PS5
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t n = 0;
    if (sysctl(mib, 4, NULL, &n, NULL, 0))
        return -1;
    unsigned char *b = malloc(n);
    if (!b)
        return -1;
    if (sysctl(mib, 4, b, &n, NULL, 0))
    {
        free(b);
        return -1;
    }
    int found = 0;
    for (size_t at = 0; at + sizeof(struct kinfo_proc) <= n;)
    {
        struct kinfo_proc *k = (void *)(b + at);
        if (k->ki_structsize <= 0 || (size_t)k->ki_structsize < sizeof(*k) || at + (size_t)k->ki_structsize > n)
        {
            found = -1;
            break;
        }
        if (!strcmp(k->ki_comm, "eboot.bin"))
        {
            AppInfo a = {0};
            if (sceKernelGetAppInfo(k->ki_pid, &a))
            {
                found = -1;
                break;
            }
            if (!memcmp(a.title_id, "PPSA01317", 9) || !memcmp(a.title_id, "PPSA01316", 9))
            {
                found = 1;
                break;
            }
        }
        at += (size_t)k->ki_structsize;
    }
    free(b);
    return found;
#else
    return 0;
#endif
}
/* Persisted in-race HUD hide list, one entry per line in <root>/hud.txt. An
 * entry containing '/' is a widget path (RaceDisplay/Container/.../TCS) and
 * matches exactly that widget; an entry without one is a bare widget name and
 * matches every widget with that name. It is re-pushed to the live bridge
 * whenever the game is running so a selection survives races and game restarts.
 * Paths are what let a HUD element be hidden without also hiding a pause-menu
 * row that merely shares its name. */
static char hud_path[512];
/* Saved HUD group layout (<root>/hud_layout.json). The page owns the schema and
 * the payload stores it verbatim, so every browser that connects sees the same
 * grouping and the same custom item names. */
static char hud_layout_path[512];
static char hud_hidden[LIVE_HUD_MAX][LIVE_HUD_PATH];
static unsigned hud_hidden_count;
#ifdef PS5
static Gt7Hud hud_state; /* ~6 KB: kept off the request thread's stack */
typedef struct
{
    const char *identifier;
    const char *label;
    const char *const *keys;
    unsigned count;
} RaceMenuGroup;
#include "race_menu_groups.h"
static const RaceMenuGroup *race_menu_selection[LIVE_MENU_MAX];
static char race_menu_labels[LIVE_MENU_MAX][LIVE_MENU_LABEL];
static uint8_t race_menu_dividers[LIVE_MENU_MAX];
static unsigned race_menu_count;
#define RACE_GROUP_COUNT (sizeof(race_menu_groups) / sizeof(race_menu_groups[0]))
static uint8_t race_group_off[RACE_GROUP_COUNT];
static char race_group_names[RACE_GROUP_COUNT][LIVE_MENU_LABEL];
static int race_apply_saved(void);
static int race_menu_save(void);

static void race_write_group(FILE *file, size_t index, const char *separator)
{
    fprintf(file, "%s      {\"identifier\": \"%s\", \"displayName\": \"",
            separator, race_menu_groups[index].identifier);
    for (const char *p = race_group_names[index]; *p; p++)
    {
        if (*p == '"' || *p == '\\')
            fputc('\\', file);
        fputc(*p, file);
    }
    fprintf(file, "\", \"enabled\": %s}", race_group_off[index] ? "false" : "true");
}

static int race_menu_save(void)
{
    char path[512], temp[sizeof(path) + sizeof(".tmp")];
    int length = snprintf(path, sizeof(path), "%s/race-info-display.json", root);
    if (length < 0 || (size_t)length >= sizeof(path))
        return -1;
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *file = fopen(temp, "wb");
    if (!file)
        return -1;

    uint8_t listed[RACE_GROUP_COUNT] = {0};
    fputs("{\n  \"raceInfoDisplay\": {\n    \"visible\": [\n", file);
    for (unsigned i = 0; i < race_menu_count; i++)
    {
        size_t index = (size_t)(race_menu_selection[i] - race_menu_groups);
        if (i && race_menu_dividers[i])
            fputs(",\n      {\"type\": \"divider\"}", file);
        race_write_group(file, index, i ? ",\n" : "");
        listed[index] = 1;
    }
    fputs("\n    ],\n    \"hidden\": [\n", file);
    unsigned written = 0;
    for (size_t i = 0; i < RACE_GROUP_COUNT; i++)
        if (!listed[i])
            race_write_group(file, i, written++ ? ",\n" : "");
    fputs("\n    ]\n  }\n}\n", file);
    int failed = ferror(file);
    if (fclose(file) || failed || rename(temp, path))
    {
        unlink(temp);
        return -1;
    }
    return 0;
}

static int race_json_take(const char **cursor, char token)
{
    *cursor += strspn(*cursor, " \t\r\n");
    if (**cursor != token)
        return 0;
    ++*cursor;
    return 1;
}

static int race_json_string(const char **cursor, char *value, size_t capacity)
{
    if (!race_json_take(cursor, '"'))
        return 0;
    size_t length = 0;
    while (**cursor && **cursor != '"')
    {
        unsigned char c = (unsigned char)*(*cursor)++;
        if (c == '\\')
        {
            c = (unsigned char)**cursor;
            if (c != '"' && c != '\\' && c != '/')
                return 0;
            ++*cursor;
        }
        if (c < 32 || length + 1 >= capacity)
            return 0;
        value[length++] = (char)c;
    }
    value[length] = 0;
    if (**cursor != '"')
        return 0;
    ++*cursor;
    return 1;
}

static int race_json_entries(const char **cursor, int visible, uint8_t *seen)
{
    if (!race_json_take(cursor, '['))
        return 0;
    if (race_json_take(cursor, ']'))
        return 1;
    int divider = visible;
    do
    {
        char key[16], identifier[96] = "", label[LIVE_MENU_LABEL] = "";
        unsigned fields = 0;
        if (!race_json_take(cursor, '{'))
            return 0;
        do
        {
            if (!race_json_string(cursor, key, sizeof(key)) || !race_json_take(cursor, ':'))
                return 0;
            unsigned field;
            if (!strcmp(key, "identifier"))
            {
                field = 1;
                if (!race_json_string(cursor, identifier, sizeof(identifier)))
                    return 0;
            }
            else if (!strcmp(key, "displayName"))
            {
                field = 2;
                if (!race_json_string(cursor, label, sizeof(label)))
                    return 0;
            }
            else if (!strcmp(key, "type"))
            {
                char type[16];
                field = 4;
                if (!race_json_string(cursor, type, sizeof(type)) || strcmp(type, "divider"))
                    return 0;
            }
            else if (!strcmp(key, "enabled"))
            {
                if (fields & 16u)
                    return 0;
                fields |= 16u;
                *cursor += strspn(*cursor, " \t\r\n");
                if (!strncmp(*cursor, "true", 4))
                    *cursor += 4;
                else if (!strncmp(*cursor, "false", 5))
                {
                    fields |= 8u;
                    *cursor += 5;
                }
                else
                    return 0;
                continue;
            }
            else
                return 0;
            if (fields & field)
                return 0;
            fields |= field;
        } while (race_json_take(cursor, ','));
        if (!race_json_take(cursor, '}'))
            return 0;
        if (fields == 4 && visible)
        {
            divider = 1;
            continue;
        }
        if ((fields & 7u) != 3u)
            return 0;
        size_t index = 0;
        size_t count = sizeof(race_menu_groups) / sizeof(race_menu_groups[0]);
        while (index < count && strcmp(identifier, race_menu_groups[index].identifier))
            index++;
        if (index == count || seen[index])
            return 0;
        seen[index] = 1;
        memcpy(race_group_names[index], label, sizeof(label));
        race_group_off[index] = (fields & 8u) != 0;
        if (visible)
        {
            if (race_menu_count == LIVE_MENU_MAX)
                return 0;
            const RaceMenuGroup *group = &race_menu_groups[index];
            race_menu_selection[race_menu_count] = group;
            strcpy(race_menu_labels[race_menu_count], *label ? label : group->label);
            race_menu_dividers[race_menu_count++] = divider ? LIVE_MENU_DIVIDER : 0;
            divider = 0;
        }
    } while (race_json_take(cursor, ','));
    return race_json_take(cursor, ']');
}

static int race_json_load(const char *cursor)
{
    char key[16];
    uint8_t seen[sizeof(race_menu_groups) / sizeof(race_menu_groups[0])] = {0};
    if (!race_json_take(&cursor, '{') ||
        !race_json_string(&cursor, key, sizeof(key)) || strcmp(key, "raceInfoDisplay") ||
        !race_json_take(&cursor, ':') || !race_json_take(&cursor, '{'))
        return 0;
    unsigned sections = 0;
    do
    {
        if (!race_json_string(&cursor, key, sizeof(key)) || !race_json_take(&cursor, ':'))
            return 0;
        unsigned section = !strcmp(key, "visible") ? 1u : !strcmp(key, "hidden") ? 2u : 0u;
        if (!section || (sections & section) || !race_json_entries(&cursor, section == 1, seen))
            return 0;
        sections |= section;
    } while (race_json_take(&cursor, ','));
    return sections == 3 && race_json_take(&cursor, '}') && race_json_take(&cursor, '}') &&
           !cursor[strspn(cursor, " \t\r\n")];
}

static int race_menu_load_legacy(void)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/race-info-display.txt", root);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    while (race_menu_count < LIVE_MENU_MAX && fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n')
        {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
            continue;
        }
        char *name = line + strspn(line, " \t");
        n = strlen(name);
        while (n && strchr(" \t\r\n", name[n - 1]))
            name[--n] = 0;
        if (!n || name[0] == '#' || n >= LIVE_MENU_LABEL)
            continue;
        for (size_t i = 0; i < sizeof(race_menu_groups) / sizeof(race_menu_groups[0]); i++)
        {
            const RaceMenuGroup *group = &race_menu_groups[i];
            if (strcmp(name, group->label))
                continue;
            unsigned j = 0;
            while (j < race_menu_count && race_menu_selection[j] != group)
                j++;
            if (j == race_menu_count)
            {
                race_menu_selection[j] = group;
                memcpy(race_menu_labels[j], name, n + 1);
                race_menu_dividers[j] = j == 0 ? LIVE_MENU_DIVIDER : 0;
                race_menu_count++;
            }
            break;
        }
    }
    fclose(f);
    return 1;
}

static int race_menu_read(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    const size_t capacity = 65536;
    char *json = malloc(capacity);
    int valid = 0;
    if (json)
    {
        size_t size = fread(json, 1, capacity - 1, file);
        json[size] = 0;
        valid = feof(file) && !ferror(file) && !memchr(json, 0, size) && race_json_load(json);
        free(json);
    }
    fclose(file);
    return valid;
}

static void race_menu_clear(void)
{
    race_menu_count = 0;
    memset(race_menu_labels, 0, sizeof(race_menu_labels));
    memset(race_group_off, 0, sizeof(race_group_off));
    memset(race_group_names, 0, sizeof(race_group_names));
}

static void race_menu_load(void)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/race-info-display.json", root);
    if (length < 0 || (size_t)length >= sizeof(path))
        return;
    if (access(path, F_OK) && errno == ENOENT)
    {
        int legacy = race_menu_load_legacy();
        if (!legacy)
            (void)race_json_load(race_menu_default_json);
        if (race_menu_save())
            notify("Race Info Display: could not create JSON.");
        return;
    }
    if (race_menu_read(path))
        return;
    race_menu_clear();
    (void)race_json_load(race_menu_default_json);
    notify("Race Info Display: could not read JSON. Using the default menu.");
}
#endif
static void hud_load(void)
{
    hud_hidden_count = 0;
    FILE *f = fopen(hud_path, "rb");
    if (!f)
        return;
    char line[LIVE_HUD_PATH + 8];
    while (hud_hidden_count < LIVE_HUD_MAX && fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n || n >= LIVE_HUD_PATH)
            continue;
        memcpy(hud_hidden[hud_hidden_count], line, n + 1);
        hud_hidden_count++;
    }
    fclose(f);
}
static int hud_save(const char entries[][LIVE_HUD_PATH], unsigned count)
{
    char temp[sizeof(hud_path) + sizeof(".tmp")];
    snprintf(temp, sizeof(temp), "%s.tmp", hud_path);
    FILE *f = fopen(temp, "wb");
    if (!f)
        return -1;
    int failed = 0;
    for (unsigned i = 0; i < count; i++)
        if (fprintf(f, "%s\n", entries[i]) < 0)
        {
            failed = 1;
            break;
        }
    if (fclose(f) || failed || rename(temp, hud_path))
    {
        unlink(temp);
        return -1;
    }
    return 0;
}
static void hud_push(void)
{
#ifdef PS5
    gt7_hud_set((const char (*)[LIVE_HUD_PATH])hud_hidden, hud_hidden_count);
#endif
}
#ifdef PS5
/* The web editor considers a key covered when it or a slash-delimited
 * ancestor is hidden. Group rows use the same rule. */
static int hud_key_covers(const char *ancestor, const char *key)
{
    size_t length = strlen(ancestor);
    return length && !strncmp(ancestor, key, length) &&
           (!key[length] || key[length] == '/');
}

typedef struct
{
    const char *keys[LIVE_HUD_MAX + RACE_MENU_CATALOG_KEYS];
    unsigned count;
} HudKeyList;

static void hud_add_key(HudKeyList *list, const char *key)
{
    for (unsigned i = 0; i < list->count; i++)
        if (hud_key_covers(list->keys[i], key))
            return;
    unsigned count = 0;
    for (unsigned i = 0; i < list->count; i++)
        if (!hud_key_covers(key, list->keys[i]))
            list->keys[count++] = list->keys[i];
    list->keys[count++] = key;
    list->count = count;
}

static int race_compile_hud(HudKeyList *list)
{
    list->count = 0;
    for (size_t i = 0; i < RACE_GROUP_COUNT; i++)
    {
        if (!race_group_off[i])
            continue;
        const RaceMenuGroup *group = &race_menu_groups[i];
        for (unsigned j = 0; j < group->count; j++)
            hud_add_key(list, group->keys[j]);
    }
    if (list->count > LIVE_HUD_MAX)
        return 0;
    for (unsigned i = 0; i < list->count; i++)
        if (strlen(list->keys[i]) >= LIVE_HUD_PATH)
            return 0;
    return 1;
}

static void race_publish_hud(const HudKeyList *list)
{
    memset(hud_hidden, 0, sizeof(hud_hidden));
    for (unsigned i = 0; i < list->count; i++)
        strcpy(hud_hidden[i], list->keys[i]);
    hud_hidden_count = list->count;
}

static int race_apply_saved(void)
{
    static HudKeyList list;
    if (!race_compile_hud(&list))
        return 0;
    race_publish_hud(&list);
    return 1;
}

static int race_menu_replace(const char *path, const char *json)
{
    static const RaceMenuGroup *selection[LIVE_MENU_MAX];
    static char labels[LIVE_MENU_MAX][LIVE_MENU_LABEL];
    static char names[RACE_GROUP_COUNT][LIVE_MENU_LABEL];
    static uint8_t dividers[LIVE_MENU_MAX], off[RACE_GROUP_COUNT];
    unsigned count = race_menu_count;
    memcpy(selection, race_menu_selection, sizeof(selection));
    memcpy(labels, race_menu_labels, sizeof(labels));
    memcpy(names, race_group_names, sizeof(names));
    memcpy(dividers, race_menu_dividers, sizeof(dividers));
    memcpy(off, race_group_off, sizeof(off));
    race_menu_clear();
    static HudKeyList list;
    int valid = (json ? race_json_load(json) : race_menu_read(path)) && race_compile_hud(&list);
    int result = -1;
    if (valid && json)
    {
        char temp[528];
        snprintf(temp, sizeof(temp), "%s.tmp", path);
        FILE *file = fopen(temp, "wb");
        result = -2;
        if (file)
        {
            int failed = fputs(json, file) == EOF || fputc('\n', file) == EOF;
            if (fclose(file) || failed || rename(temp, path))
                unlink(temp);
            else
                result = 0;
        }
    }
    else if (valid)
        result = 0;
    if (!result)
    {
        race_publish_hud(&list);
        return 0;
    }

    race_menu_count = count;
    memcpy(race_menu_selection, selection, sizeof(selection));
    memcpy(race_menu_labels, labels, sizeof(labels));
    memcpy(race_group_names, names, sizeof(names));
    memcpy(race_menu_dividers, dividers, sizeof(dividers));
    memcpy(race_group_off, off, sizeof(off));
    return result;
}

static void race_menu_reload(void)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/race-info-display.json", root);
    if (length < 0 || (size_t)length >= sizeof(path))
        return;
    static struct stat previous;
    struct stat current;
    if (stat(path, &current))
        return;
    if (previous.st_ino == current.st_ino && previous.st_size == current.st_size &&
        previous.st_mtim.tv_sec == current.st_mtim.tv_sec &&
        previous.st_mtim.tv_nsec == current.st_mtim.tv_nsec)
        return;
    previous = current;
    if (race_menu_replace(path, NULL))
        notify("Race Info Display: JSON reload failed. Keeping the previous settings.");
}

static const char *race_menu_toggle(unsigned index)
{
    if (index >= race_menu_count)
        return "the configured row is unavailable";
    size_t group = (size_t)(race_menu_selection[index] - race_menu_groups);
    static HudKeyList next;
    race_group_off[group] ^= 1;
    const char *error = NULL;
    if (!race_compile_hud(&next))
        error = "the HUD hide-list limits were exceeded";
    else if (race_menu_save())
        error = "the JSON settings could not be saved";
    if (error)
        race_group_off[group] ^= 1;
    else
        race_publish_hud(&next);
    return error;
}

static void hud_menu_tick(void)
{
    if (!race_menu_count)
        return;
    uint8_t hidden[LIVE_MENU_MAX], events[LIVE_MENU_EVENTS];
    for (unsigned i = 0; i < race_menu_count; i++)
        hidden[i] = race_menu_dividers[i] | race_group_off[race_menu_selection[i] - race_menu_groups];
    uint32_t acknowledged, sequence;
    if (gt7_live_menu_sync(hud_state.pid, race_menu_labels, hidden, race_menu_count,
                         &acknowledged, events, &sequence))
        return;

    static int pid;
    static uint32_t handled;
    if (pid != hud_state.pid)
    {
        pid = hud_state.pid;
        handled = acknowledged;
    }
    if (sequence - handled > LIVE_MENU_EVENTS)
        return;
    while (handled != sequence)
    {
        unsigned index = events[handled % LIVE_MENU_EVENTS];
        const char *error = race_menu_toggle(index);
        if (error)
            notify("Race Info Display: %s could not be toggled because %s.",
                   index < race_menu_count ? race_menu_selection[index]->label : "this group",
                   error);
        handled++;
    }
    if (handled != acknowledged)
    {
        for (unsigned i = 0; i < race_menu_count; i++)
            hidden[i] = race_menu_dividers[i] | race_group_off[race_menu_selection[i] - race_menu_groups];
        gt7_live_menu_acknowledge(pid, handled, hidden, race_menu_count);
    }
}
#endif
static void hud_tick(void)
{
#ifdef PS5
    if (!(config.mask & LIVE_SETTINGS_BIT) || running_gt7() <= 0)
        return;
    if (gt7_hud_read(&hud_state))
        return;
    hud_menu_tick();
    int same = hud_state.count == hud_hidden_count;
    if (same)
        for (unsigned i = 0; i < hud_hidden_count; i++)
            if (strncmp(hud_state.entries[i], hud_hidden[i], LIVE_HUD_PATH))
            {
                same = 0;
                break;
            }
    if (!same)
        gt7_hud_set((const char (*)[LIVE_HUD_PATH])hud_hidden, hud_hidden_count);
#endif
}
#ifdef PS5
/* Serialiser for GET /api/live/screen and /api/live/hud/tree. Paths hold only printable ASCII, but
 * a quote or backslash still needs escaping to keep the JSON well formed. */
typedef struct
{
    char *body;
    size_t cap, at;
    int first;
} HudTreeJson;
static int hud_tree_emit(void *context, const Gt7HudNode *node)
{
    HudTreeJson *j = context;
    if (j->at + 4u * LIVE_HUD_PATH + 128u > j->cap)
        return 1; /* stop; the page shows what fits */
    size_t n = j->at;
    n += (size_t)snprintf(j->body + n, j->cap - n, "%s{\"p\":\"", j->first ? "" : ",");
    j->first = 0;
    for (const char *c = node->path; *c; c++)
    {
        if (*c == '"' || *c == '\\')
            j->body[n++] = '\\';
        j->body[n++] = *c;
    }
    n += (size_t)snprintf(j->body + n, j->cap - n, "\",\"d\":%u,\"h\":%d,\"a\":\"%#llx\",\"v\":\"%#llx\",\"c\":\"%#llx\",\"r\":[", node->depth, node->hidden, (unsigned long long)node->address, (unsigned long long)node->vtable, (unsigned long long)node->children);
    for (unsigned i = 0; i < 8; i++)
        n += (size_t)snprintf(j->body + n, j->cap - n, "%s%.6g", i ? "," : "", node->rect[i]);
    n += (size_t)snprintf(j->body + n, j->cap - n, "]");
    /* The pre-heap-name path, when it differs: the page treats it as an alias
     * so a group saved earlier still shows its widgets as present. */
    if (node->alt[0])
    {
        n += (size_t)snprintf(j->body + n, j->cap - n, ",\"q\":\"");
        for (const char *c = node->alt; *c; c++)
        {
            if (*c == '"' || *c == '\\')
                j->body[n++] = '\\';
            j->body[n++] = *c;
        }
        j->body[n++] = '"';
    }
    j->body[n++] = '}';
    j->body[n] = 0;
    j->at = n;
    return 0;
}
#endif
static int engine_ready(void)
{
#ifdef PS5
    if (atomic_load(&startup_pending))
        return 0;
    return gt7_engine_ready();
#else
    return 0;
#endif
}
static int engine_active(void)
{
#ifdef PS5
    return gt7_engine_active();
#else
    return 0;
#endif
}
static int port_in_use(unsigned port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    int r = connect(s, (void *)&a, sizeof(a));
    close(s);
    return r == 0;
}
#ifdef PS5
static int wait_for_gt7_launch(void)
{
    /* LaunchApp queues an asynchronous request and its return value is not a
     * boolean error status. Confirm the result by observing GT7 itself. */
    for (unsigned i = 0; i < 50; i++)
    {
        if (running_gt7() > 0)
            return 1;
        usleep(100000);
    }
    return 0;
}
#endif
static int start_engine(void)
{
    if (engine_ready())
        return 0;
    if (broken)
    {
        strcpy(message, "Engine is unavailable. Reboot before retrying.");
        return -1;
    }
#ifdef PS5
    if (port_in_use(23916) || port_in_use(23900) || port_in_use(7211))
    {
        strcpy(message, "Another patch engine is running. Reboot and load only GT7Patcher.");
        return -1;
    }
    if (gt7_engine_start())
    {
        broken = 1;
        strcpy(message, "Could not verify the GT7 launch hook. Reboot before retrying.");
        notify("GT7Patcher: engine startup failed. Reboot before retrying.");
        return -1;
    }
    strcpy(message, "GT7 launch hook is ready. You can launch GT7 now.");
    notify("GT7Patcher is ready. You can launch GT7 now.");
    return 0;
#else
    (void)port_in_use;
    strcpy(message, "Host preview only: the console patch engine is unavailable here.");
    return -1;
#endif
}
static int make_dirs(const char *path)
{
    char b[512];
    if (strlen(path) >= sizeof(b))
        return -1;
    strcpy(b, path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(b, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    return mkdir(b, 0755) && errno != EEXIST ? -1 : 0;
}
static int initialize(void)
{
    if (make_dirs(root) || initialize_config(root, &config))
        return -1;
#ifdef PS5
    /* Remove only the payload's obsolete files after validating config.json. */
    const char *legacy_dirs[] = {"/data/gt7p/patches", "/data/gt7p/patches/xml_prospero"};
    struct stat info;
    if (!lstat(legacy_dirs[0], &info) && S_ISDIR(info.st_mode))
    {
        if (!lstat(legacy_dirs[1], &info) && S_ISDIR(info.st_mode))
        {
            unlink("/data/gt7p/patches/xml_prospero/PPSA01317.xml");
            rmdir(legacy_dirs[1]);
        }
        unlink("/data/gt7p/patches/hud.txt");
        unlink("/data/gt7p/patches/hud_layout.json");
        if (rmdir(legacy_dirs[0]) && errno != ENOENT)
            notify("GT7Patcher: legacy patches directory could not be removed.");
    }
#endif
    snprintf(hud_path, sizeof(hud_path), "%s/hud.txt", root);
    snprintf(hud_layout_path, sizeof(hud_layout_path), "%s/hud_layout.json", root);
    if (WEB_HUD_EDITOR)
        hud_load();
#ifdef PS5
    race_menu_load();
    if (!race_apply_saved())
        notify("Race Info Display: JSON HUD settings exceed the hide-list limits.");
#endif
    unsigned char random[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t n = fread(random, 1, sizeof(random), f);
    fclose(f);
    if (n != sizeof(random))
        return -1;
    for (unsigned i = 0; i < 16; i++)
        snprintf(token + i * 2, 3, "%02x", random[i]);
    return 0;
}
#ifdef PS5
static void *startup(void *unused)
{
    (void)unused;
    int icon = gt7_install_icon();
    if (icon)
    {
        strcpy(message, "GT7Patcher is running on port 7210. Home-screen installation failed; details are in /data/gt7p/startup.log. You can still start the engine here.");
        notify("GT7Patcher: shortcut installation failed. Open console port 7210.");
    }
    if (config.autostart)
    {
        int game = running_gt7();
        if (game)
        {
            strcpy(message, game > 0 ? "Automatic start skipped: close GT7, then start the engine." : "Automatic start skipped: could not check whether GT7 is closed.");
            notify("GT7Patcher: automatic start skipped. Close GT7 and start the engine from the web interface.");
        }
        else
            start_engine();
    }
    else if (!icon)
        notify("GT7Patcher: open the home-screen shortcut or console port 7210.");
    atomic_store(&startup_pending, 0);
    return NULL;
}
#endif
static void json_status(int fd)
{
    char body[4096];
    size_t n = 0;
    const char *status_message = message;
#ifdef PS5
    /* A crashed/exited GT7 process can leave stale bridge addresses briefly
     * visible to the kernel. Do not probe live memory while the game is not
     * confirmed running; status must remain a safe recovery endpoint. */
    int game_running = running_gt7();
    if (atomic_load(&startup_pending))
        status_message = "GT7Patcher is available on port 7210. Preparing the home-screen shortcut and saved startup option.";
#endif
    n += (size_t)snprintf(body + n, sizeof(body) - n, "{\"version\":\"%s\",\"token\":\"%s\",\"engineRunning\":%s,\"engineActive\":%s,\"gameRunning\":%d,\"message\":\"%s\"", VERSION, token, engine_ready() ? "true" : "false", engine_active() ? "true" : "false",
#ifdef PS5
                          game_running,
#else
                          0,
#endif
                          status_message);
#ifdef PS5
    {
        unsigned requests = 0;
        int pid = 0;
        const char *stage = "";
        gt7_engine_launch_status(&requests, &pid, &stage);
        n += (size_t)snprintf(body + n, sizeof(body) - n, ",\"launch\":{\"requests\":%u,\"pid\":%d,\"stage\":\"%s\"}", requests, pid, stage);
    }
#endif
    n += (size_t)snprintf(body + n, sizeof(body) - n, ",\"config\":{\"mask\":%u,\"preset\":%d,\"units\":%d,\"language\":%d,\"country\":%d,\"speed\":%d,\"mass\":%d,\"power\":%d,\"torque\":%d,\"temperature\":%d,\"autostart\":%d,\"aspect\":%d,\"important\":%d},\"features\":[", config.mask, config.preset, config.units, config.language, config.country, config.speed, config.mass, config.power, config.torque, config.temperature, config.autostart, config.aspect, config.important_dismissed);
    /* Original feature labels and notes contain no JSON quoting characters; verify at generation. */
    for (unsigned i = 0; i < PATCH_COUNT; i++)
        n += (size_t)snprintf(body + n, sizeof(body) - n, "%s{\"name\":\"%s\",\"note\":\"%s\",\"lines\":%u}", i ? "," : "", feature_name(i), feature_note(i), feature_count(i));
    n += (size_t)snprintf(body + n, sizeof(body) - n, "],\"live\":{\"creditPrompt\":{");
#ifdef PS5
    Gt7CreditPrompt credit_prompt;
    if ((config.mask & LIVE_SETTINGS_BIT) && !atomic_load(&startup_pending) && game_running > 0 && !gt7_live_credit_prompt_read(&credit_prompt))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":true,\"state\":%u,\"result\":%u,\"amount\":%u,\"chars\":%u", credit_prompt.state, credit_prompt.result, credit_prompt.amount, credit_prompt.chars);
    else
#endif
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":false,\"state\":null,\"result\":null,\"amount\":null,\"chars\":null");
    n += (size_t)snprintf(body + n, sizeof(body) - n, "}");
    n += (size_t)snprintf(body + n, sizeof(body) - n, ",\"wheelWidth\":{");
#ifdef PS5
    Gt7WheelWidth width;
    if ((config.mask & LIVE_SETTINGS_BIT) && !atomic_load(&startup_pending) && game_running > 0 && !gt7_wheel_width_read(&width))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":true,\"front\":%d,\"rear\":%d,\"modelCode\":%u,\"sequence\":%u", width.front, width.rear, width.model_code, width.sequence);
    else
#endif
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":false,\"front\":null,\"rear\":null,\"modelCode\":null,\"sequence\":null");
    n += (size_t)snprintf(body + n, sizeof(body) - n, "},\"wheelGeometry\":{");
#ifdef PS5
    Gt7WheelGeometry geometry;
    if ((config.mask & LIVE_SETTINGS_BIT) && !atomic_load(&startup_pending) && game_running > 0 && !gt7_wheel_geometry_read(&geometry))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":true,\"inchUp\":%d,\"offsetFront\":%d,\"offsetRear\":%d,\"hubFront\":%d,\"hubRear\":%d,\"modelCode\":%u", geometry.inch_up, geometry.offset_front, geometry.offset_rear, geometry.hub_front, geometry.hub_rear, geometry.model_code);
    else
#endif
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":false,\"inchUp\":null,\"offsetFront\":null,\"offsetRear\":null,\"hubFront\":null,\"hubRear\":null,\"modelCode\":null");
    n += (size_t)snprintf(body + n, sizeof(body) - n, "},\"engineSwap\":{");
#ifdef PS5
    Gt7EngineSwap swap;
    if ((config.mask & LIVE_SETTINGS_BIT) && !atomic_load(&startup_pending) && game_running > 0 && !gt7_engine_swap_read(&swap))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":true,\"category\":%u,\"modelCode\":%u", swap.category, swap.model_code);
    else
#endif
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":false,\"category\":null,\"modelCode\":null");
    n += (size_t)snprintf(body + n, sizeof(body) - n, "},\"revLimit\":{");
#ifdef PS5
    Gt7RevLimit rev;
    if ((config.mask & LIVE_SETTINGS_BIT) && !atomic_load(&startup_pending) && game_running > 0 && !gt7_rev_limit_read(&rev))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":true,\"maxRpm\":%u,\"idleRpm\":%u,\"redRpm\":%u,\"nativeRpm\":%u,\"increase\":%u,\"applied\":%s,\"dialAddr\":%llu,\"dialScale\":%u,\"dialRed\":%u", (unsigned)rev.max_rpm, (unsigned)rev.idle_rpm, (unsigned)rev.red_rpm, (unsigned)rev.native_rpm, (unsigned)rev.increase, rev.applied ? "true" : "false", (unsigned long long)rev.dial_addr, (unsigned)rev.dial_scale, (unsigned)rev.dial_red);
    else
#endif
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"available\":false,\"maxRpm\":null,\"idleRpm\":null,\"redRpm\":null,\"nativeRpm\":null,\"increase\":null,\"applied\":false,\"dialAddr\":null,\"dialScale\":null,\"dialRed\":null");
    snprintf(body + n, sizeof(body) - n, "}}}");
    response(fd, 200, "application/json", body);
}
static void serve(int fd)
{
    /* Requests are capped at 256 KiB below. Keep the buffer bounded to that
     * limit plus room for the request line and headers. */
    static char req[262144 + 4096];
    char method[12], path[128], proto[16];
    size_t used = 0;
    char *body = NULL;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    while (used + 1 < sizeof(req))
    {
        ssize_t r = recv(fd, req + used, sizeof(req) - used - 1, 0);
        if (r <= 0)
            return;
        used += (size_t)r;
        req[used] = 0;
        if ((body = strstr(req, "\r\n\r\n")))
            break;
    }
    if (!body || sscanf(req, "%11s %127s %15s", method, path, proto) != 3)
    {
        response(fd, 400, "text/plain", "Invalid request");
        return;
    }
    *body = 0;
    body += 4;
    size_t length = 0;
    int has_length = 0, authorized = 0;
    for (char *h = strstr(req, "\r\n"); h && h[2];)
    {
        h += 2;
        char *e = strstr(h, "\r\n");
        if (e)
            *e = 0;
        if (!strncasecmp(h, "Content-Length:", 15))
        {
            char *end;
            unsigned long l = strtoul(h + 15, &end, 10);
            if (has_length || *end || l > 262144)
            {
                response(fd, 400, "text/plain", "Invalid Content-Length");
                return;
            }
            length = (size_t)l;
            has_length = 1;
        }
        if (!strncasecmp(h, "Transfer-Encoding:", 18))
        {
            response(fd, 400, "text/plain", "Unsupported transfer encoding");
            return;
        }
        if (!strncasecmp(h, "X-GT7-Token: ", 13) && !strcmp(h + 13, token))
            authorized = 1;
        h = e;
    }
    if (!strcmp(method, "GET"))
    {
        if (!strcmp(path, "/"))
            response(fd, 200, "text/html; charset=utf-8", page_html());
        else if (!strcmp(path, "/api/status"))
            json_status(fd);
        else if (!strncmp(path, "/api/update-notify", 18) && (!path[18] || path[18] == '?'))
        {
#ifdef PS5
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before sending a notification.");
                return;
            }
            if (update_notification_sent)
            {
                response(fd, 200, "application/json", "{\"sent\":false}");
                return;
            }
            char *query = strchr(path, '?');
            char text[192];
            if (!query || strncmp(query + 1, "text=", 5) || !decode_query_text(query + 6, text, sizeof(text)))
            {
                response(fd, 400, "text/plain", "Use /api/update-notify?text=MESSAGE");
                return;
            }
            notify("%s", text);
            update_notification_sent = 1;
            response(fd, 200, "application/json", "{\"sent\":true}");
            return;
#else
            response(fd, 409, "text/plain", "Notifications are only available on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/notify", 11) && (!path[11] || path[11] == '?'))
        {
#ifdef PS5
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before sending a notification.");
                return;
            }
            char *query = strchr(path, '?');
            char text[192];
            if (!query || strncmp(query + 1, "text=", 5) || !decode_query_text(query + 6, text, sizeof(text)))
            {
                response(fd, 400, "text/plain", "Use /api/notify?text=MESSAGE");
                return;
            }
            notify("%s", text);
            response(fd, 200, "application/json", "{\"sent\":true}");
            return;
#else
            response(fd, 409, "text/plain", "Notifications are only available on PS5.");
            return;
#endif
        }
        else if (!strcmp(path, "/api/live/credit-input"))
        {
#ifdef PS5
            Gt7CreditPrompt prompt;
            if (gt7_live_credit_prompt_read(&prompt))
            {
                response(fd, 409, "text/plain", "Live numeric input is unavailable while supported GT7 is not running.");
                return;
            }
            char body[160];
            snprintf(body, sizeof(body), "{\"state\":%u,\"result\":%u,\"amount\":%u,\"chars\":%u,\"dbgGot\":%u,\"dbgEnd\":%u,\"dbgStatus\":%u,\"dbgUser\":%u}", prompt.state, prompt.result, prompt.amount, prompt.chars, prompt.dbg_got, prompt.dbg_end, prompt.dbg_status, prompt.dbg_user);
            response(fd, 200, "application/json", body);
            return;
#else
            response(fd, 409, "text/plain", "Live numeric input is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strcmp(path, "/api/live/hud/config"))
        {
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading HUD settings.");
                return;
            }
            char file_path[512];
            int n = snprintf(file_path, sizeof(file_path), "%s/race-info-display.json", root);
            FILE *file = n >= 0 && (size_t)n < sizeof(file_path) ? fopen(file_path, "rb") : NULL;
            static char json[65536];
            size_t size = file ? fread(json, 1, sizeof(json) - 1, file) : 0;
            int valid = file && size && feof(file) && !ferror(file) && !memchr(json, 0, size);
            if (file)
                fclose(file);
            json[size] = 0;
            response(fd, valid ? 200 : 409, valid ? "application/json" : "text/plain",
                     valid ? json : "Could not read race-info-display.json.");
            return;
        }
        else if (!strcmp(path, "/api/live/hud"))
        {
            /* In-race HUD hide list. Read-only here; the persisted selection is
             * returned along with whether the live bridge is reachable. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading HUD state.");
                return;
            }
            static char body[LIVE_HUD_MAX * LIVE_HUD_PATH + 512];
            int at;
#ifdef PS5
            int live = running_gt7() > 0 && !gt7_hud_read(&hud_state);
            at = snprintf(body, sizeof(body), "{\"available\":%s,\"applied\":%u,\"sequence\":%u,\"hidden\":[", live ? "true" : "false", live ? hud_state.applied : 0, live ? hud_state.sequence : 0);
#else
            at = snprintf(body, sizeof(body), "{\"available\":false,\"applied\":0,\"sequence\":0,\"hidden\":[");
#endif
            for (unsigned i = 0; i < hud_hidden_count && at > 0 && at < (int)sizeof(body) - 128; i++)
                at += snprintf(body + at, sizeof(body) - at, "%s\"%s\"", i ? "," : "", hud_hidden[i]);
            if (at > 0 && at < (int)sizeof(body) - 8)
                snprintf(body + at, sizeof(body) - at, "]}");
            response(fd, 200, "application/json", body);
            return;
        }
        else if (!strcmp(path, "/api/live/hud/tree") || !strcmp(path, "/api/live/screen"))
        {
            /* Live MENU widget tree for the UI's expandable groups. Every named
             * widget is reported with the full path the bridge matches on, its
             * depth and whether it is currently hidden. The in-car and bumper
             * root windows carry the same widgets, so duplicates are expected;
             * the page merges them by path. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading screen state.");
                return;
            }
#ifdef PS5
            if (running_gt7() <= 0)
            {
                response(fd, 409, "text/plain", "The live widget tree is unavailable while supported GT7 is not running.");
                return;
            }
            size_t cap = 512u * 1024u;
            char *body = malloc(cap);
            if (!body)
            {
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            HudTreeJson j = {body, cap, 0, 1};
            j.at = (size_t)snprintf(body, cap, "{\"available\":true,\"nodes\":[");
            if (gt7_hud_tree(hud_tree_emit, &j))
            {
                free(body);
                response(fd, 409, "text/plain", "The live widget tree is unavailable while supported GT7 is not running.");
                return;
            }
            snprintf(body + j.at, cap - j.at, "]}");
            response(fd, 200, "application/json", body);
            free(body);
            return;
#else
            response(fd, 409, "text/plain", "The live widget tree is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (WEB_HUD_EDITOR && !strcmp(path, "/api/live/hud/layout"))
        {
            /* Saved HUD group layout. Returned verbatim so every browser shows
             * the same grouping; an empty object means nothing is saved yet and
             * the page should upload what it has. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading the HUD layout.");
                return;
            }
            const char *empty = "{\"groups\":[],\"names\":{}}";
            FILE *f = fopen(hud_layout_path, "rb");
            if (!f)
            {
                response(fd, 200, "application/json", empty);
                return;
            }
            long len = 0;
            if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || len > 262144)
            {
                fclose(f);
                response(fd, 200, "application/json", empty);
                return;
            }
            rewind(f);
            char *body = malloc((size_t)len + 1);
            if (!body)
            {
                fclose(f);
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            size_t n = fread(body, 1, (size_t)len, f);
            fclose(f);
            body[n] = 0;
            if (!n)
            {
                free(body);
                response(fd, 200, "application/json", empty);
                return;
            }
            response(fd, 200, "application/json", body);
            free(body);
            return;
        }
        else if (!strncmp(path, "/api/live/dump", 14))
        {
            /* Temporary read-only diagnostic: hex dump of a region (<=1 MiB). Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            char *query = strchr(path, '?');
            unsigned long long address = 0;
            unsigned length = 0;
            if (!query || sscanf(query, "?addr=%llx&len=%u", &address, &length) != 2)
            {
                response(fd, 400, "text/plain", "Use /api/live/dump?addr=0xSTART&len=N");
                return;
            }
            if (!length || length > 1048576U)
            {
                response(fd, 400, "text/plain", "Length must be from 1 to 1048576.");
                return;
            }
            unsigned char *raw = malloc(length);
            char *hex = malloc((size_t)length * 2 + 1);
            static const char digits[] = "0123456789abcdef";
            if (!raw || !hex)
            {
                free(raw);
                free(hex);
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            if (gt7_live_peek((uintptr_t)address, raw, length))
            {
                free(raw);
                free(hex);
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            for (unsigned i = 0; i < length; i++)
            {
                hex[i * 2] = digits[raw[i] >> 4];
                hex[i * 2 + 1] = digits[raw[i] & 15];
            }
            hex[length * 2] = 0;
            response(fd, 200, "text/plain", hex);
            free(raw);
            free(hex);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strcmp(path, "/api/live/bindings"))
        {
            /* Read-only normalized feed for the binding inspector. It is
             * populated from resolved objects captured by the live probe;
             * non-scalar binary fields remain absent rather than guessed. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live bindings.");
                return;
            }
#ifdef PS5
            char *body = malloc(512u * 1024u);
            unsigned rows = 0, objects = 0;
            if (!body)
            {
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            if (live_binding_feed(body, 512u * 1024u, &rows, &objects))
            {
                free(body);
                response(fd, 409, "text/plain", "Live bindings are unavailable while supported GT7 is not running.");
                return;
            }
            response(fd, 200, "application/json", body);
            free(body);
            return;
#else
            response(fd, 409, "text/plain", "Live bindings are only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/hooks", 15))
        {
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live trace state.");
                return;
            }
#ifdef PS5
            if (strstr(path, "?install=1") != NULL)
            {
                if (gt7_live_hooks_auto())
                {
                    response(fd, 409, "text/plain", "Could not install the live trace hooks.");
                    return;
                }
            }
            char body[4096];
            if (gt7_live_hooks_list(body, sizeof(body)))
            {
                response(fd, 409, "text/plain", "Live trace state is unavailable while supported GT7 is not running.");
                return;
            }
            response(fd, 200, "application/json", body);
            return;
#else
            response(fd, 409, "text/plain", "Live trace hooks are only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/trace", 15))
        {
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live trace data.");
                return;
            }
#ifdef PS5
            char *query = strchr(path, '?');
            unsigned long long since = 0;
            unsigned max = 256;
            if (query)
            {
                char *p = strstr(query, "since=");
                if (p)
                    since = strtoull(p + 6, NULL, 0);
                p = strstr(query, "max=");
                if (p)
                    max = (unsigned)strtoul(p + 4, NULL, 0);
            }
            if (max > LIVE_TRACE_MAX)
                max = LIVE_TRACE_MAX;
            size_t cap = 1024u * 1024u;
            char *body = malloc(cap);
            if (!body)
            {
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            uint64_t cursor = (uint64_t)since;
            if (gt7_live_trace_report(body, (uint32_t)cap, &cursor, max))
            {
                free(body);
                response(fd, 409, "text/plain", "Live trace data is unavailable while supported GT7 is not running.");
                return;
            }
            response(fd, 200, "application/json", body);
            free(body);
            return;
#else
            response(fd, 409, "text/plain", "Live trace data is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/probe", 15))
        {
            /* Temporary read-only diagnostic: last logged hook registers. Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            if (!strcmp(path, "/api/live/probe/history"))
            {
                uintptr_t words[64 * 32];
                uint32_t next = 0;
                char *history = malloc(65536);
                int at;
                if (!history)
                {
                    response(fd, 500, "text/plain", "Out of memory");
                    return;
                }
                if (gt7_live_probe_history(words, 64 * 32, &next))
                {
                    free(history);
                    response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                    return;
                }
                at = snprintf(history, 65536, "{\"next\":%u,\"records\":[", next);
                for (unsigned i = 0; i < 64 && at > 0 && at < 65536 - 512; i++)
                {
                    at += snprintf(history + at, 65536 - at, "%s[", i ? "," : "");
                    for (unsigned j = 0; j < 26; j++)
                        at += snprintf(history + at, 65536 - at, "%s\"0x%llx\"", j ? "," : "", (unsigned long long)words[i * 32 + j]);
                    at += snprintf(history + at, 65536 - at, "]");
                }
                if (at > 0 && at < 65536)
                    snprintf(history + at, 65536 - at, "]}");
                response(fd, 200, "application/json", history);
                free(history);
                return;
            }
            uintptr_t words[128];
            char body[4096];
            int at;
            if (gt7_live_probe_read(words, 128))
            {
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            at = snprintf(body, sizeof(body), "{\"rdi\":\"0x%llx\",\"rsi\":\"0x%llx\",\"rdx\":\"0x%llx\",\"rcx\":\"0x%llx\",\"r8\":\"0x%llx\",\"r9\":\"0x%llx\",\"rsp\":\"0x%llx\",\"count\":%llu,\"ret\":\"0x%llx\",\"rbp\":\"0x%llx\",\"result\":\"0x%llx\",\"resultCount\":%llu,\"bindingObject\":\"0x%llx\",\"getter\":\"0x%llx\",\"stack\":[",
                          (unsigned long long)words[0], (unsigned long long)words[1], (unsigned long long)words[2], (unsigned long long)words[3], (unsigned long long)words[4], (unsigned long long)words[5], (unsigned long long)words[6], (unsigned long long)words[7], (unsigned long long)words[8], (unsigned long long)words[9], (unsigned long long)words[10], (unsigned long long)words[11], (unsigned long long)words[12], (unsigned long long)words[13]);
            for (int i = 0; i < 48 && at > 0 && at < (int)sizeof(body) - 40; i++)
                at += snprintf(body + at, sizeof(body) - at, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)words[64 + i]);
            if (at > 0 && at < (int)sizeof(body) - 40)
                at += snprintf(body + at, sizeof(body) - at, "],\"snap\":[");
            for (int i = 0; i < 24 && at > 0 && at < (int)sizeof(body) - 40; i++)
                at += snprintf(body + at, sizeof(body) - at, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)words[32 + i]);
            if (at > 0 && at < (int)sizeof(body) - 8)
                snprintf(body + at, sizeof(body) - at, "]}");
            response(fd, 200, "application/json", body);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strcmp(path, "/api/live/maps"))
        {
            /* Temporary read-only diagnostic: GT7 memory map. Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            char *buffer = malloc(262144);
            if (!buffer)
            {
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            int n = gt7_live_mapinfo(buffer, 262144);
            if (n < 0)
            {
                free(buffer);
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            response(fd, 200, "text/plain", buffer);
            free(buffer);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/find", 14))
        {
            /* Temporary read-only diagnostic: search GT7 memory for text.
             * Use /api/live/find?addr=0xSTART&len=N&text=STR&max=M
             * Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            char *query = strchr(path, '?');
            unsigned long long address = 0;
            unsigned length = 0, maxhits = 16;
            char text[128] = "";
            if (!query || sscanf(query, "?addr=%llx&len=%u&text=%127[^&]&max=%u", &address, &length, text, &maxhits) != 4)
            {
                response(fd, 400, "text/plain", "Use /api/live/find?addr=0xSTART&len=N&text=STR&max=M");
                return;
            }
            if (!length || length > 67108864U || !text[0] || maxhits > 64)
            {
                response(fd, 400, "text/plain", "len must be 1..67108864, text must be non-empty, max 1..64.");
                return;
            }
            unsigned char pattern[64];
            uint32_t plen = 0;
            if (!strncmp(text, "hex:", 4))
            {
                const char *hp = text + 4;
                for (uint32_t i = 0; hp[i] && hp[i + 1] && plen < sizeof(pattern); i += 2)
                {
                    unsigned v;
                    if (sscanf(hp + i, "%2x", &v) != 1)
                        break;
                    pattern[plen++] = (unsigned char)v;
                }
            }
            else
            {
                plen = (uint32_t)strlen(text);
                memcpy(pattern, text, plen);
            }
            if (!plen)
            {
                response(fd, 400, "text/plain", "Pattern must be non-empty.");
                return;
            }
            uintptr_t hits[64];
            uint32_t count = 0;
            if (gt7_live_find((uintptr_t)address, length, (const char *)pattern, plen, hits, maxhits, &count))
            {
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            char reply[1200];
            int off = snprintf(reply, sizeof(reply), "{\"count\":%u,\"hits\":[", count);
            for (uint32_t i = 0; i < count && off < (int)sizeof(reply) - 32; i++)
                off += snprintf(reply + off, sizeof(reply) - off, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)hits[i]);
            snprintf(reply + off, sizeof(reply) - off, "]}");
            response(fd, 200, "application/json", reply);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strcmp(path, "/api/live/carptr"))
        {
            /* Temporary read-only diagnostic: riding car object pointers. Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            uintptr_t implementation = 0, expanded = 0, style = 0;
            uint32_t model = 0, sequence = 0;
            if (gt7_live_car_pointers(&implementation, &expanded, &style, &model, &sequence))
            {
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            char reply[256];
            snprintf(reply, sizeof(reply), "{\"implementation\":\"0x%llx\",\"expanded\":\"0x%llx\",\"style\":\"0x%llx\",\"modelCode\":%u,\"sequence\":%u}", (unsigned long long)implementation, (unsigned long long)expanded, (unsigned long long)style, model, sequence);
            response(fd, 200, "application/json", reply);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/diff", 14))
        {
            /* Temporary read-only diagnostic: byte runs changed since the last snapshot. Remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            char *buffer = malloc(4194304);
            if (!buffer)
            {
                response(fd, 500, "text/plain", "Out of memory");
                return;
            }
            unsigned bytes = 0, runs = 0;
            int n = gt7_live_snapshot_diff(buffer, 4194304, &bytes, &runs);
            if (n < 0)
            {
                free(buffer);
                response(fd, 409, "text/plain", "No snapshot is stored, or live memory is unavailable.");
                return;
            }
            response(fd, 200, "application/json", buffer);
            free(buffer);
            return;
#else
            response(fd, 409, "text/plain", "Live memory is only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else if (!strncmp(path, "/api/live/peek", 14))
        {
            /* Temporary read-only diagnostic for mapping GT7 state from inside
             * the running game. Token-gated; remove before release. */
            if (!authorized)
            {
                response(fd, 403, "text/plain", "Reload this page before reading live memory.");
                return;
            }
#ifdef PS5
            char *query = strchr(path, '?');
            unsigned long long address = 0;
            unsigned len = 8;
            if (!query || sscanf(query, "?addr=%llx&len=%u", &address, &len) != 2)
            {
                response(fd, 400, "text/plain", "Use /api/live/peek?addr=0xADDR&len=N");
                return;
            }
            if (!len || len > 256)
            {
                response(fd, 400, "text/plain", "Length must be from 1 to 256.");
                return;
            }
            unsigned char buffer[256];
            static const char digits[] = "0123456789abcdef";
            char hex[513];
            char reply[600];
            if (gt7_live_peek((uintptr_t)address, buffer, len))
            {
                response(fd, 409, "text/plain", "Live memory is unavailable while supported GT7 is not running.");
                return;
            }
            for (unsigned i = 0; i < len; i++)
            {
                hex[i * 2] = digits[buffer[i] >> 4];
                hex[i * 2 + 1] = digits[buffer[i] & 15];
            }
            hex[len * 2] = 0;
            snprintf(reply, sizeof(reply), "{\"address\":\"0x%llx\",\"length\":%u,\"hex\":\"%s\"}", address, len, hex);
            response(fd, 200, "application/json", reply);
            return;
#else
            response(fd, 409, "text/plain", "Live memory reads are only available while supported GT7 is running on PS5.");
            return;
#endif
        }
        else
            response(fd, 404, "text/plain", "Not found");
        return;
    }
    if (strcmp(method, "POST"))
    {
        response(fd, 405, "text/plain", "Use POST");
        return;
    }
    if (!authorized)
    {
        response(fd, 403, "text/plain", "Reload this page before saving.");
        return;
    }
    if (!has_length)
    {
        response(fd, 400, "text/plain", "Content-Length required");
        return;
    }
    if ((size_t)(body - req) + length >= sizeof(req))
    {
        response(fd, 400, "text/plain", "Request too large");
        return;
    }
    size_t received = used - (size_t)(body - req);
    if (received > length)
    {
        response(fd, 400, "text/plain", "Invalid body length");
        return;
    }
    while (received < length)
    {
        ssize_t r = recv(fd, body + received, length - received, 0);
        if (r <= 0)
            return;
        received += (size_t)r;
    }
    body[length] = 0;
    if (strncmp(path, "/api/live/probe", 15) && strncmp(path, "/api/live/menu-call", 19) && strcmp(path, "/api/live/snapshot") && strcmp(path, "/api/live/snapshot/clear") && strcmp(path, "/api/config") && strcmp(path, "/api/live/credit-input") && strcmp(path, "/api/live/wheel-width") && strcmp(path, "/api/live/wheel-geometry") && strcmp(path, "/api/live/engine-swap") && strcmp(path, "/api/live/rev-limit") && strncmp(path, "/api/live/hud", 13) && strcmp(path, "/api/start") && strcmp(path, "/api/game/start") && strcmp(path, "/api/shortcut/reinstall") && strcmp(path, "/api/engine/stop") && strcmp(path, "/api/payload/stop"))
    {
        response(fd, 404, "text/plain", "Not found");
        return;
    }
#ifdef PS5
    if (atomic_load(&startup_pending))
    {
        response(fd, 409, "text/plain", "Startup is still running. Please retry shortly; the browser page remains available.");
        return;
    }
#endif
    if (!strncmp(path, "/api/live/probe", 15))
    {
        /* Temporary diagnostic: install/remove a logging hook. Remove before release. */
#ifdef PS5
        if (!strcmp(path, "/api/live/probe/remove"))
        {
            if (length)
            {
                response(fd, 400, "text/plain", "Unexpected request body");
                return;
            }
            if (gt7_live_probe_remove())
            {
                response(fd, 409, "text/plain", "Could not remove the probe; the bridge may be unavailable.");
                return;
            }
            response(fd, 200, "application/json", "{\"removed\":true}");
            return;
        }
        char *query = strchr(path, '?');
        unsigned long long address = 0;
        unsigned displaced = 0;
        unsigned mode = 0;
        unsigned long long watch = 0;
        if (!query || sscanf(query, "?addr=%llx&len=%u", &address, &displaced) != 2)
        {
            response(fd, 400, "text/plain", "Use /api/live/probe?addr=0xADDR&len=N");
            return;
        }
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
        {
            const char *mp = strstr(query, "&mode=");
            if (mp)
                mode = (unsigned)strtoul(mp + 6, NULL, 10);
        }
        {
            const char *wp = strstr(query, "&watch=");
            if (wp)
                watch = strtoull(wp + 7, NULL, 0);
        }
        if (gt7_live_probe_install((uintptr_t)address, displaced, mode, (uintptr_t)watch))
        {
            response(fd, 409, "text/plain", "Could not install the probe at that address.");
            return;
        }
        response(fd, 200, "application/json", "{\"installed\":true}");
        return;
#else
        response(fd, 409, "text/plain", "Probes are only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strncmp(path, "/api/live/menu-call", 19) && (!path[19] || path[19] == '?'))
    {
#ifdef PS5
        unsigned long long target = 0, receiver = 0, args = 0;
        unsigned argc = 0, result = 0;
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
        char *query = strchr(path, '?');
        if (!query || sscanf(query, "?target=%llx&receiver=%llx&argc=%u&args=%llx", &target, &receiver, &argc, &args) != 4)
        {
            response(fd, 400, "text/plain", "Use /api/live/menu-call?target=0xc96f50&receiver=0xRACE_INFO_OPTION&argc=0&args=0 for a detached clone or args=1 to add the configured rows");
            return;
        }
        int rc = gt7_live_menu_call((uintptr_t)target, (uintptr_t)receiver, argc, (uintptr_t)args, &result);
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"rc\":%d,\"result\":%u}", rc, result);
        response(fd, rc ? 409 : 200, "application/json", reply);
        return;
#else
        response(fd, 409, "text/plain", "Live menu calls are only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/snapshot/clear"))
    {
        /* Temporary diagnostic: drop the stored snapshot. Remove before release. */
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
#ifdef PS5
        gt7_live_snapshot_clear();
        response(fd, 200, "application/json", "{\"cleared\":true}");
        return;
#else
        response(fd, 409, "text/plain", "Snapshots are only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/snapshot"))
    {
        /* Temporary diagnostic: store a memory snapshot in the payload. Remove before release. */
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
#ifdef PS5
        char *query = strchr(path, '?');
        unsigned long long address = 0;
        unsigned len = 0;
        if (!query || sscanf(query, "?addr=%llx&len=%u", &address, &len) != 2)
        {
            response(fd, 400, "text/plain", "Use /api/live/snapshot?addr=0xSTART&len=N");
            return;
        }
        int result = gt7_live_snapshot((uintptr_t)address, len);
        if (result == -1)
        {
            response(fd, 500, "text/plain", "Could not allocate the snapshot buffer.");
            return;
        }
        if (result)
        {
            response(fd, 409, "text/plain", "Live memory is unavailable, or the length is out of range (max 536870912).");
            return;
        }
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"stored\":true,\"addr\":\"0x%llx\",\"len\":%u}", address, len);
        response(fd, 200, "application/json", reply);
        return;
#else
        response(fd, 409, "text/plain", "Snapshots are only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/credit-input"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
        if (!(config.mask & LIVE_SETTINGS_BIT))
        {
            response(fd, 409, "text/plain", "Select and save the Live Settings patch first.");
            return;
        }
#ifdef PS5
        int result = gt7_live_credit_prompt_begin();
        if (result)
        {
            response(fd, 409, "text/plain", "Live numeric input is unavailable while supported GT7 is not running.");
            return;
        }
        response(fd, 200, "application/json", "{\"opening\":true}");
        return;
#else
        response(fd, 409, "text/plain", "Live numeric input is only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/wheel-width"))
    {
        int consumed = 0, front, rear, expected_front, expected_rear;
        unsigned model_code, sequence;
        char canonical[160];
        if (length >= sizeof(canonical) || sscanf(body, "front=%d&rear=%d&expectedFront=%d&expectedRear=%d&modelCode=%u&sequence=%u%n", &front, &rear, &expected_front, &expected_rear, &model_code, &sequence, &consumed) != 6 || consumed != (int)length ||
            front < INT8_MIN || front > INT8_MAX || rear < INT8_MIN || rear > INT8_MAX || expected_front < INT8_MIN || expected_front > INT8_MAX || expected_rear < INT8_MIN || expected_rear > INT8_MAX || !model_code || !sequence)
        {
            response(fd, 400, "text/plain", "Use signed wheel-width values from -128 to 127 and the current car sample.");
            return;
        }
        snprintf(canonical, sizeof(canonical), "front=%d&rear=%d&expectedFront=%d&expectedRear=%d&modelCode=%u&sequence=%u", front, rear, expected_front, expected_rear, model_code, sequence);
        if (strcmp(body, canonical))
        {
            response(fd, 400, "text/plain", "Invalid wheel-width request.");
            return;
        }
        if (!(config.mask & LIVE_SETTINGS_BIT))
        {
            response(fd, 409, "text/plain", "Select and save the Live Settings patch first.");
            return;
        }
#ifdef PS5
        Gt7WheelWidth before = {0, model_code, sequence, (int8_t)expected_front, (int8_t)expected_rear};
        int result = gt7_wheel_width_set(&before, (int8_t)front, (int8_t)rear);
        if (result)
        {
            response(fd, 409, "text/plain", result == -3 ? "The game has not acknowledged the wheel-width request yet. Do not resubmit; wait for the values to refresh." : result == -2 ? "The selected car or its widths changed since the last refresh. Refresh and retry."
                                                                                                                                                                                        : "Live bridge is unavailable or the native garage update could not be verified. Start GT7 with the Live Settings patch selected.");
            return;
        }
        strcpy(message, "Wheel widths applied through GT7 garage data.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "Wheel-width editing is only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/wheel-geometry"))
    {
        int inch, of, orr, hf, hr, einch, eof, eor, ehf, ehr, consumed = 0;
        unsigned model;
        char canonical[256];
        if (length >= sizeof(canonical) || sscanf(body, "inch=%d&offsetFront=%d&offsetRear=%d&hubFront=%d&hubRear=%d&expectedInch=%d&expectedOffsetFront=%d&expectedOffsetRear=%d&expectedHubFront=%d&expectedHubRear=%d&modelCode=%u%n", &inch, &of, &orr, &hf, &hr, &einch, &eof, &eor, &ehf, &ehr, &model, &consumed) != 11 || consumed != (int)length || inch < INT8_MIN || inch > INT8_MAX || einch < INT8_MIN || einch > INT8_MAX || of < INT16_MIN || of > INT16_MAX || orr < INT16_MIN || orr > INT16_MAX || hf < INT16_MIN || hf > INT16_MAX || hr < INT16_MIN || hr > INT16_MAX || eof < INT16_MIN || eof > INT16_MAX || eor < INT16_MIN || eor > INT16_MAX || ehf < INT16_MIN || ehf > INT16_MAX || ehr < INT16_MIN || ehr > INT16_MAX || !model)
        {
            response(fd, 400, "text/plain", "Use signed wheel geometry values within the displayed ranges.");
            return;
        }
        snprintf(canonical, sizeof(canonical), "inch=%d&offsetFront=%d&offsetRear=%d&hubFront=%d&hubRear=%d&expectedInch=%d&expectedOffsetFront=%d&expectedOffsetRear=%d&expectedHubFront=%d&expectedHubRear=%d&modelCode=%u", inch, of, orr, hf, hr, einch, eof, eor, ehf, ehr, model);
        if (strcmp(body, canonical))
        {
            response(fd, 400, "text/plain", "Invalid wheel-geometry request.");
            return;
        }
        if (!(config.mask & LIVE_SETTINGS_BIT))
        {
            response(fd, 409, "text/plain", "Select and save the Live Settings patch first.");
            return;
        }
#ifdef PS5
        Gt7WheelGeometry before = {0, model, (int8_t)einch, (int16_t)eof, (int16_t)eor, (int16_t)ehf, (int16_t)ehr};
        int result = gt7_wheel_geometry_set(&before, (int8_t)inch, (int16_t)of, (int16_t)orr, (int16_t)hf, (int16_t)hr);
        if (result)
        {
            response(fd, 409, "text/plain", result == -3 ? "The game has not acknowledged the wheel-geometry request yet. Do not resubmit; wait for the values to refresh." : result == -2 ? "The selected car or its wheel geometry changed since the last refresh. Refresh and retry."
                                                                                                                                                                                           : "The native garage update could not be verified. Wheel-offset changes are limited to 127 steps per apply.");
            return;
        }
        strcpy(message, "Wheel geometry applied through GT7 garage data.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "Wheel geometry editing is only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/hud/config"))
    {
#ifdef PS5
        if (running_gt7() != 0)
        {
            response(fd, 409, "text/plain", "Close GT7 before saving Race Info Display settings.");
            return;
        }
        char file_path[512];
        int n = snprintf(file_path, sizeof(file_path), "%s/race-info-display.json", root);
        int result = n < 0 || (size_t)n >= sizeof(file_path) ? -2 :
                     length >= 65535 || memchr(body, 0, length) ? -1 :
                     race_menu_replace(file_path, body);
        response(fd, result == -2 ? 500 : result ? 400 : 200, "text/plain",
                 result == -2 ? "Could not save race-info-display.json." :
                 result ? "Invalid Race Info Display settings or HUD hide-list limit exceeded." :
                 "Race Info Display settings saved.");
#else
        response(fd, 409, "text/plain", "Saving Race Info Display settings requires the PS5 payload.");
#endif
        return;
    }
    if (WEB_HUD_EDITOR && !strcmp(path, "/api/live/hud"))
    {
        /* Body: newline-separated MENU widget paths or names to hide. An empty
         * body clears the list. The selection is persisted and pushed live. */
        static char names[LIVE_HUD_MAX][LIVE_HUD_PATH];
        unsigned count = 0;
        int too_many = 0;
        for (char *p = body; *p;)
        {
            char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            while (n && (p[n - 1] == '\r' || p[n - 1] == ' '))
                n--;
            if (n && n < LIVE_HUD_PATH)
            {
                int ok = 1;
                for (size_t i = 0; i < n; i++)
                {
                    unsigned char c = (unsigned char)p[i];
                    if (c < 0x20 || c > 0x7e)
                        ok = 0;
                }
                if (ok)
                {
                    if (count < LIVE_HUD_MAX)
                    {
                        memcpy(names[count], p, n);
                        names[count][n] = 0;
                        count++;
                    }
                    else
                        too_many = 1;
                }
            }
            if (!e)
                break;
            p = e + 1;
        }
        if (too_many)
        {
            char m[80];
            snprintf(m, sizeof(m), "Too many HUD entries; at most %u can be hidden.", (unsigned)LIVE_HUD_MAX);
            response(fd, 400, "text/plain", m);
            return;
        }
        if (hud_save(names, count))
        {
            response(fd, 500, "text/plain", "Could not save the HUD selection.");
            return;
        }
        memcpy(hud_hidden, names, sizeof(hud_hidden));
        hud_hidden_count = count;
        hud_push();
        response(fd, 200, "text/plain", "HUD selection saved.");
        return;
    }
    if (WEB_HUD_EDITOR && !strcmp(path, "/api/live/hud/layout"))
    {
        /* Body: the JSON group layout the page keeps ({groups,names}). Stored
         * verbatim; independent of whether GT7 is running. */
        if (!length)
        {
            response(fd, 400, "text/plain", "Expected a layout body");
            return;
        }
        if (body[0] != '{')
        {
            response(fd, 400, "text/plain", "Expected a JSON object");
            return;
        }
        FILE *f = fopen(hud_layout_path, "wb");
        if (!f)
        {
            response(fd, 500, "text/plain", "Could not save the HUD layout.");
            return;
        }
        size_t written = fwrite(body, 1, length, f);
        int failed = fclose(f) != 0 || written != length;
        if (failed)
        {
            response(fd, 500, "text/plain", "Could not save the HUD layout.");
            return;
        }
        response(fd, 200, "application/json", "{\"saved\":true}");
        return;
    }
    if (!strcmp(path, "/api/live/engine-swap"))
    {
        unsigned category, expected, unlock, model;
        int consumed = 0;
        char canonical[96];
        if (length >= sizeof(canonical) || sscanf(body, "category=%u&expected=%u&unlock=%u&modelCode=%u%n", &category, &expected, &unlock, &model, &consumed) != 4 || consumed != (int)length || category > 2 || expected > 2 || unlock > 1 || !model)
        {
            response(fd, 400, "text/plain", "Use an engine swap slot from 0 to 2.");
            return;
        }
        snprintf(canonical, sizeof(canonical), "category=%u&expected=%u&unlock=%u&modelCode=%u", category, expected, unlock, model);
        if (strcmp(body, canonical))
        {
            response(fd, 400, "text/plain", "Invalid engine-swap request.");
            return;
        }
        if (!(config.mask & LIVE_SETTINGS_BIT))
        {
            response(fd, 409, "text/plain", "Select and save the Live Settings patch first.");
            return;
        }
#ifdef PS5
        Gt7EngineSwap before = {0, model, (uint8_t)expected};
        int result = gt7_engine_swap_set(&before, (uint8_t)category, (int)unlock);
        if (result)
        {
            response(fd, 409, "text/plain", result == -3 ? "The game has not acknowledged the engine request yet. Do not resubmit; wait for the value to refresh." : result == -2 ? "The selected car or engine swap changed since the last refresh. Refresh and retry."
                                                                                                                                                                                  : "GT7 rejected that swap slot. The car may not support it.");
            return;
        }
        strcpy(message, "Engine swap applied through GT7 garage data.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "Engine swapping is only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/live/rev-limit"))
    {
        unsigned increase, expected_max, expected_idle;
        int consumed = 0;
        char canonical[96];
        if (length >= sizeof(canonical) || sscanf(body, "increase=%u&expectedMax=%u&expectedIdle=%u%n", &increase, &expected_max, &expected_idle, &consumed) != 3 || consumed != (int)length ||
            increase > 8000 || expected_max < 1500 || expected_max > 20000 || expected_idle < 300 || expected_idle > 2000)
        {
            response(fd, 400, "text/plain", "Use an RPM increase from 0 to 8000 and the current limiter sample.");
            return;
        }
        snprintf(canonical, sizeof(canonical), "increase=%u&expectedMax=%u&expectedIdle=%u", increase, expected_max, expected_idle);
        if (strcmp(body, canonical))
        {
            response(fd, 400, "text/plain", "Invalid rev-limit request.");
            return;
        }
        if (!(config.mask & LIVE_SETTINGS_BIT))
        {
            response(fd, 409, "text/plain", "Select and save the Live Settings patch first.");
            return;
        }
#ifdef PS5
        Gt7RevLimit before = {0, (uint16_t)expected_max, (uint16_t)expected_idle, 0, 0, 0, 0, 0, 0, 0};
        int result = gt7_rev_limit_set(&before, (uint16_t)increase);
        if (result)
        {
            response(fd, 409, "text/plain", result == -2 ? "The car or its rev limiter changed since the last refresh. Refresh and retry." : "The rev limiter could not be updated. Sit in a car with the Live Settings patch selected.");
            return;
        }
        strcpy(message, increase ? "Rev limit raised by the requested amount." : "Rev limit restored to its native value.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "Rev-limit editing is only available while supported GT7 is running on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/payload/stop"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
#ifdef PS5
        if (gt7_engine_stop())
        {
            broken = 1;
            strcpy(message, "Could not safely stop the engine. Reboot before retrying.");
            response(fd, 409, "text/plain", message);
            return;
        }
#endif
        strcpy(message, "GT7Patcher has stopped. Run the payload again to reconnect.");
        json_status(fd);
        stopping = 1;
        return;
    }
    if (!strcmp(path, "/api/engine/stop"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
#ifdef PS5
        if (gt7_engine_stop())
        {
            broken = 1;
            strcpy(message, "Could not safely stop the engine. Reboot before retrying.");
            response(fd, 409, "text/plain", message);
            return;
        }
        notify("GT7Patcher: Patch engine stopped.");
#endif
        strcpy(message, "Patch engine stopped. You can change settings or start it again.");
        json_status(fd);
        return;
    }
    if (!strcmp(path, "/api/shortcut/reinstall"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
#ifdef PS5
        unlink("/data/gt7p/gt7patcher-icon-installed");
        if (gt7_install_icon())
        {
            strcpy(message, "Could not reinstall the home-screen shortcut. See /data/gt7p/startup.log.");
            response(fd, 409, "text/plain", message);
            return;
        }
        strcpy(message, "Home-screen shortcut reinstalled.");
        notify("GT7Patcher: Home-screen shortcut reinstalled.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "Shortcut installation is only available on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/game/start"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
        int game = running_gt7();
        if (game)
        {
            response(fd, 409, "text/plain", game > 0 ? "GT7 is already running." : "Cannot establish that GT7 is closed; launch was not requested.");
            return;
        }
#ifdef PS5
        if (!engine_ready())
        {
            response(fd, 409, "text/plain", "Start the patch engine before launching GT7.");
            return;
        }
        int launch_result = gt7_launch_game();
        if (!wait_for_gt7_launch())
        {
            snprintf(message, sizeof(message), "GT7 did not appear after the launch request (result 0x%08x).", (unsigned)launch_result);
            response(fd, 409, "text/plain", message);
            return;
        }
        strcpy(message, "GT7 launch requested. The patch engine will apply saved patches as the game starts.");
        json_status(fd);
        return;
#else
        response(fd, 409, "text/plain", "GT7 launch is only available on PS5.");
        return;
#endif
    }
    if (!strcmp(path, "/api/start"))
    {
        if (length)
        {
            response(fd, 400, "text/plain", "Unexpected request body");
            return;
        }
        int game = running_gt7();
        if (game)
        {
            response(fd, 409, "text/plain", game > 0 ? "Fully close GT7 before starting the engine." : "Cannot establish that GT7 is closed; engine was not started.");
            return;
        }
        if (start_engine())
        {
            response(fd, 409, "text/plain", message);
            return;
        }
    }
    else
    {
        Config next;
        if (parse_config(body, &next))
        {
            response(fd, 400, "text/plain", "Invalid settings.");
            return;
        }
        if (save_config(root, &next))
        {
            response(fd, 500, "text/plain", "Could not save the patch snapshot; previous settings retained.");
            return;
        }
        config = next;
        message[0] = 0;
    }
    json_status(fd);
}
int main(int argc, char **argv)
{
    unsigned port = PORT;
#ifndef PS5
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s LOCAL_STATE_DIRECTORY PORT\n", argv[0]);
        return 1;
    }
    root = argv[1];
    port = (unsigned)strtoul(argv[2], NULL, 10);
#else
    (void)argc;
    (void)argv;
    /* elfldr uses this syscall to assign payload.elf. Use the same mechanism
     * so process enumerators report the payload's real name. */
    syscall(SYS_thr_set_name, -1, "gt7patcher.elf");
    notify("GT7Patcher %s: starting on port 7210.", VERSION);
#endif
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    int s = open_listener(port);
    if (s < 0)
    {
        perror("GT7Patcher socket");
        return 1;
    }
    if (initialize())
    {
        perror("GT7Patcher initialization");
        close(s);
        return 1;
    }
#ifdef PS5
#ifdef GT7_LIVE_TEST
    /* Diagnostic build only: use a separate port and leave the running
     * payload's ShellCore launch hook and persisted choices untouched. */
    config.mask |= LIVE_SETTINGS_BIT;
    extern int gt7_live_test_attach(void);
    strcpy(message, gt7_live_test_attach() ? "Live bridge test installation failed." : "Live bridge test installed. Waiting for the game UI callback.");
#else
    FILE *log = fopen("/data/gt7p/startup.log", "w");
    if (log)
    {
        fprintf(log, "GT7Patcher %s main reached; firmware=%08x; web socket listening on 7210\n", VERSION, kernel_get_fw_version());
        fclose(log);
    }
    if (port_in_use(23916) || port_in_use(23900) || port_in_use(7211))
    {
        strcpy(message, "Engine startup requires no other patch system. Reboot and load only GT7Patcher.");
        notify("GT7Patcher: engine unavailable. Open console port 7210 for details.");
    }
    else
    {
        atomic_store(&startup_pending, 1);
        if (pthread_create(&startup_thread, NULL, startup, NULL))
        {
            atomic_store(&startup_pending, 0);
            strcpy(message, "Automatic setup could not start. GT7Patcher is available on port 7210.");
        }
        else
            startup_thread_created = 1;
    }
#endif
#endif
    printf("GT7Patcher %s on port %u\n", VERSION, port);
    fflush(stdout);
    while (!stopping)
    {
        gt7_live_pad_configure((config.mask & LIVE_SETTINGS_BIT) != 0);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        struct timeval timeout = {1, 0};
        int r = select(s + 1, &set, NULL, NULL, &timeout);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            /* A suspend/resume can leave the descriptor unusable. Reopen the
             * endpoint in-process; only an explicit stop ends the payload. */
            close(s);
            s = -1;
            while (!stopping && (s = open_listener(port)) < 0)
                usleep(100000);
            continue;
        }
        if (!r)
        {
#ifdef PS5
            /* Never touch the game bridge after GT7 has exited. In
             * particular, a crash can leave an eboot entry present long
             * enough for stale mdbg reads to stall the payload's main loop. */
            int game = running_gt7();
            if (game > 0)
            {
                gt7_live_pad_poll();
                hud_tick();
                gt7_live_credit_auto_poll();
                gt7_rev_limit_tick();
            }
            else if (game == 0)
                race_menu_reload();
#else
            gt7_live_pad_poll();
            hud_tick();
            gt7_live_credit_auto_poll();
            gt7_rev_limit_tick();
#endif
            continue;
        }
        int c = accept(s, NULL, NULL);
        if (c < 0)
        {
            if (errno == EINTR)
                continue;
            close(s);
            s = -1;
            while (!stopping && (s = open_listener(port)) < 0)
                usleep(100000);
            continue;
        }
        serve(c);
        close(c);
    }
    if (s >= 0)
        close(s);
#ifdef PS5
    if (startup_thread_created)
        pthread_join(startup_thread, NULL);
    (void)gt7_engine_stop();
#endif
    return 0;
}
