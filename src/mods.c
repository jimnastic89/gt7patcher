/* SPDX-License-Identifier: GPL-3.0-only */
/* Gameplay modifiers for GT7 01.710.000. Payout rates are verified data writes
 * into the game's loaded DMS tables, whose layout is checked first; a mismatch
 * leaves the game untouched for that session. World Circuits prizes need one
 * code hook (event_hook.h), installed only over the expected bytes.
 *
 * DMS (data modeling system) tables: the manager singleton sits at RVA
 * 0x677a208 {vtable, store}, with its "initialised" flag at 0x677a218. The
 * accessor at RVA 0x142360 finds a table id in the u32 list at RVA 0x4d045f0
 * (index i) and returns the shared_ptr at *(store + i * 24). A table object
 * starts with "dmsb", u32 version, u32 count and a row pointer at +0x10.
 *
 * Race payouts: table 0x208 (GameModeCoeff) holds 15 rows {u64 index,
 * char *name, double rate}. CaclulateQuickRacePrizeRatio and
 * CalculateExperienceAndPrizeTable multiply every computed prize table by
 * the rate of the race's game mode (custom races and arcade: 0.6).
 *
 * World Circuits prizes are fixed per event in the event data, not computed.
 * The event hook rewrites an event's prize table as the event loads, from a
 * table of {event id, new 1st prize} that this file keeps in the game.
 *
 * Balance (build/balance.h, from data/balance.json and gt7re/balance.py) is the
 * recommended set: custom race rates, World Circuits prizes and roulette odds.
 * Each area starts from Balance or from the game's values (Balance by default),
 * and the user's own values override it.
 *
 * Roulette tickets: table 0x265 (RouletteTicket) holds 13 rows of 28 int32:
 * ID, Level, Kind, an unknown field, then 12 {value, rate %} slots: Credit1-4,
 * Car1-3, Parts1-3, Invitation, Engine. A row's rates add up to 100. Car values
 * are car groups (1 to 3, 0 none), parts values part groups (0 to 2, -1 none),
 * Invitation and Engine 1 (0 none). Rows 1-6 are the 1 to 6 star tickets,
 * 7-10 the credit, car, parts and invitation tickets, 11-13 the 6 star engine,
 * parts and car tickets. */
#include "mods.h"
#include "event_hook.h"
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef PS5
#include "engine.h"
#include "event_hook_blob.h"
#endif
#include "balance.h"

#define MODE_COUNT 15
#define DMS_MANAGER_RVA 0x677A208u
#define DMS_READY_RVA 0x677A218u
#define DMS_ID_LIST_RVA 0x4D045F0u
#define DMS_ID_COUNT 304
#define GAME_MODE_COEFF_ID 0x208u
#define RATE_MIN 0.0
#define RATE_MAX 10.0
#define ROULETTE_ID 0x265u
#define ROULETTE_ROWS 13
#define ROULETTE_FIELDS 28
#define ROULETTE_SLOTS 12
#define ROULETTE_CREDIT_MAX 100000000
#define EVENT_OVERRIDES_MAX 96
_Static_assert(sizeof(balance_events) / sizeof(balance_events[0]) + EVENT_OVERRIDES_MAX <= EVENT_HOOK_TABLE_MAX,
               "Balance and the user's events fit the hook table");

static const char *const mode_names[MODE_COUNT] = {
    "ArcadeSingle", "TimeAttack", "DriftTrial", "FreeRun", "CustomRace",
    "VS", "OnlineRoom", "OnlineVS", "DailyRace", "DailyRacePractice",
    "SportChampionShip", "SportChampionShipPractice", "VRTour", "LV_RATE", "CE_RATE"};
/* The game's own rates in 01.710.000, read from a fresh session. They are the
 * reference for "use the game's rates", so a session that was already changed
 * (by an earlier payload run) is still restored correctly. */
static const double game_rate[MODE_COUNT] = {0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 1.0, 1.0, 1.5, 1.0, 2.0, 1.0, 1.0, 2.0, 2.0};
/* The game's RouletteTicket rows in 01.710.000, read from a fresh session. */
static const int32_t roulette_game[ROULETTE_ROWS][ROULETTE_FIELDS] = {
    {1, 1, 0, 1, 2000, 40, 5000, 27, 10000, 19, 100000, 1, 1, 3, 0, 0, 0, 0, 0, 10, -1, 0, -1, 0, 0, 0, 0, 0},
    {2, 2, 0, 5, 5000, 40, 10000, 35, 30000, 9, 100000, 1, 1, 5, 0, 0, 0, 0, 0, 10, -1, 0, -1, 0, 0, 0, 0, 0},
    {3, 3, 0, 10, 5000, 40, 10000, 35, 100000, 4, 500000, 1, 1, 7, 2, 3, 0, 0, 0, 5, 1, 5, -1, 0, 0, 0, 0, 0},
    {4, 4, 0, 15, 10000, 28, 30000, 20, 500000, 4, 1000000, 1, 1, 8, 2, 5, 3, 1, -1, 0, 1, 10, 2, 10, 1, 10, 1, 3},
    {5, 5, 0, 20, 0, 0, 100000, 36, 500000, 13, 1000000, 1, 0, 0, 2, 20, 3, 7, -1, 0, -1, 0, 2, 10, 1, 8, 1, 5},
    {6, 6, 0, 20, 0, 0, 500000, 40, 1000000, 20, 0, 0, 0, 0, 0, 0, 3, 40, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0},
    {7, 5, 0, 1, 10000, 33, 20000, 33, 100000, 34, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0},
    {8, 5, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 33, 2, 33, 3, 34, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0},
    {9, 5, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 33, 1, 33, 2, 34, 0, 0, 0, 0},
    {10, 5, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, -1, 0, 1, 100, 0, 0},
    {11, 6, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, 1, 100},
    {12, 6, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 2, 100, 0, 0, 0, 0},
    {13, 6, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 100, 0, 0, -1, 0, -1, 0, 0, 0, 0, 0},
};

typedef struct
{
    /* Per area: start from Balance (1) or from the game's values (0). */
    int balance_rates, balance_events, balance_roulette;
    double rate[MODE_COUNT]; /* the user's own rate; 0: the area's starting value */
    uint32_t event_count;
    EventHookEntry events[EVENT_OVERRIDES_MAX];
    uint32_t roulette_set;   /* bit r: the user changed ticket row r */
    int32_t roulette_value[ROULETTE_ROWS][ROULETTE_SLOTS], roulette_rate[ROULETTE_ROWS][ROULETTE_SLOTS];
} ModsConfig;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ModsConfig config;
static char path_json[256], path_log[256];
static int session_pid, session_blocked, have_current, pending;
static double current[MODE_COUNT];
static char state[160] = "Waiting for GT7.";

/* Install and apply states of the event hook and the roulette odds. */
enum
{
    STATE_WAITING,
    STATE_READY,
    STATE_BLOCKED
};
static int hook_state, hook_dirty = 1;
static char hook_note[120] = "Waiting for GT7.";
static uint32_t hook_loads, hook_changed, hook_head;
static EventHookLog hook_log[EVENT_LOG_COUNT];

/* Roulette ticket state for the status. */
static int roulette_state, roulette_checked;
static char roulette_note[120] = "Waiting for GT7.";

__attribute__((format(printf, 1, 2))) static void mods_log(const char *fmt, ...)
{
    FILE *f = fopen(path_log, "a");
    if (!f)
        return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void set_state(const char *text)
{
    if (strcmp(state, text))
    {
        snprintf(state, sizeof(state), "%s", text);
        mods_log("%s", text);
    }
}

/* Finds "key": <number> in a flat JSON text. Returns 0 when found. */
static int json_double(const char *json, const char *key, double *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *at = strstr(json, needle);
    if (!at)
        return -1;
    at += strlen(needle);
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
        at++;
    if (*at != ':')
        return -1;
    at++;
    char *end;
    double v = strtod(at, &end);
    if (end == at || v != v || v > 1e300 || v < -1e300)
        return -1;
    *out = v;
    return 0;
}

/* Finds "key": true in a flat JSON text. */
static int json_true(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *at = strstr(json, needle);
    if (!at)
        return 0;
    at += strlen(needle);
    while (*at == ' ' || *at == ':')
        at++;
    return !strncmp(at, "true", 4);
}

static const char *skip_space(const char *at)
{
    while (*at && isspace((unsigned char)*at))
        at++;
    return at;
}

/* Reads "events": {"<event id>": <new 1st prize>, ...}; a prize of 0 drops
 * the event. */
static int parse_events(const char *json, ModsConfig *c)
{
    const char *at = strstr(json, "\"events\"");
    if (!at)
        return 0;
    at = skip_space(at + 8);
    if (*at++ != ':')
        return -1;
    at = skip_space(at);
    if (*at++ != '{')
        return -1;
    for (at = skip_space(at); *at != '}'; at = skip_space(at))
    {
        char *end;
        if (*at++ != '"' || !isdigit((unsigned char)*at))
            return -1;
        unsigned long long id = strtoull(at, &end, 10);
        if (*end != '"' || !id || id > UINT32_MAX)
            return -1;
        at = skip_space(end + 1);
        if (*at++ != ':')
            return -1;
        at = skip_space(at);
        if (!isdigit((unsigned char)*at))
            return -1;
        unsigned long long prize = strtoull(at, &end, 10);
        if (prize > EVENT_HOOK_PRIZE_MAX)
            return -1;
        at = skip_space(end);
        if (*at == ',')
            at++;
        else if (*at != '}')
            return -1;
        if (!prize)
            continue;
        for (uint32_t i = 0; i < c->event_count; i++)
            if (c->events[i].id == id)
                return -1;
        if (c->event_count >= EVENT_OVERRIDES_MAX)
            return -1;
        c->events[c->event_count++] = (EventHookEntry){(uint32_t)id, (uint32_t)prize};
    }
    return 0;
}

/* Reads "key": [n, ...] with exactly count integers; returns the text after it. */
static const char *json_ints(const char *at, const char *key, int32_t *out, int count)
{
    size_t n = strlen(key);
    at = skip_space(at);
    if (*at++ != '"' || strncmp(at, key, n) || at[n] != '"')
        return NULL;
    at = skip_space(at + n + 1);
    if (*at++ != ':')
        return NULL;
    at = skip_space(at);
    if (*at++ != '[')
        return NULL;
    for (int i = 0; i < count; i++)
    {
        char *end;
        long v = strtol(skip_space(at), &end, 10);
        if (end == skip_space(at) || v < INT32_MIN || v > INT32_MAX)
            return NULL;
        out[i] = (int32_t)v;
        at = skip_space(end);
        if (*at++ != (i + 1 < count ? ',' : ']'))
            return NULL;
    }
    return at;
}

/* A slot may pay out only with a value the game can give: credits, a car
 * group (1 to 3), a part group (0 to 2), one invitation or one engine. */
static int roulette_slot_valid(int slot, int32_t value)
{
    if (slot < 4)
        return value > 0 && value <= ROULETTE_CREDIT_MAX;
    if (slot < 7)
        return value >= 1 && value <= 3;
    if (slot < 10)
        return value >= 0 && value <= 2;
    return value == 1;
}

/* Reads "roulette": {"<ticket id>": {"values": [12], "rates": [12]}, ...}. A
 * slot with rate 0 keeps the game's value; a ticket's rates must add up to 100. */
static int parse_roulette(const char *json, ModsConfig *c)
{
    const char *at = strstr(json, "\"roulette\"");
    if (!at)
        return 0;
    at = skip_space(at + 10);
    if (*at++ != ':')
        return -1;
    at = skip_space(at);
    if (*at++ != '{')
        return -1;
    for (at = skip_space(at); *at != '}'; at = skip_space(at))
    {
        char *end;
        if (*at++ != '"' || !isdigit((unsigned char)*at))
            return -1;
        unsigned long id = strtoul(at, &end, 10);
        if (*end != '"' || id < 1 || id > ROULETTE_ROWS || (c->roulette_set >> (id - 1) & 1))
            return -1;
        int r = (int)id - 1, sum = 0;
        int32_t *values = c->roulette_value[r], *rates = c->roulette_rate[r];
        at = skip_space(end + 1);
        if (*at++ != ':')
            return -1;
        at = skip_space(at);
        if (*at++ != '{' || !(at = json_ints(at, "values", values, ROULETTE_SLOTS)))
            return -1;
        at = skip_space(at);
        if (*at++ != ',' || !(at = json_ints(at, "rates", rates, ROULETTE_SLOTS)))
            return -1;
        at = skip_space(at);
        if (*at++ != '}')
            return -1;
        for (int s = 0; s < ROULETTE_SLOTS; s++)
        {
            if (rates[s] < 0 || rates[s] > 100 || (rates[s] && !roulette_slot_valid(s, values[s])))
                return -1;
            if (!rates[s])
                values[s] = roulette_game[r][4 + 2 * s];
            sum += rates[s];
        }
        if (sum != 100)
            return -1;
        c->roulette_set |= 1u << r;
        at = skip_space(at);
        if (*at == ',')
            at++;
        else if (*at != '}')
            return -1;
    }
    return 0;
}

/* The recommended settings: Balance everywhere, nothing of the user's own. */
static void defaults(ModsConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->balance_rates = c->balance_events = c->balance_roulette = 1;
}

/* Returns 0, -1 for invalid settings, or 1 for settings from before Balance
 * (they are replaced by the recommended ones). */
static int parse(const char *json, ModsConfig *c)
{
    double v;
    defaults(c);
    if (!strstr(json, "\"balance_rates\""))
        return 1;
    c->balance_rates = json_true(json, "balance_rates");
    c->balance_events = json_true(json, "balance_events");
    c->balance_roulette = json_true(json, "balance_roulette");
    if (parse_events(json, c) || parse_roulette(json, c))
        return -1;
    for (int i = 0; i < MODE_COUNT; i++)
    {
        if (json_double(json, mode_names[i], &v))
            continue;
        if (v < RATE_MIN || v > RATE_MAX)
            return -1;
        c->rate[i] = v;
    }
    return 0;
}

static int write_config(const ModsConfig *c)
{
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path_json);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "{\"balance_rates\":%s,\"balance_events\":%s,\"balance_roulette\":%s,\"rates\":{",
            c->balance_rates ? "true" : "false", c->balance_events ? "true" : "false", c->balance_roulette ? "true" : "false");
    int first = 1;
    for (int i = 0; i < MODE_COUNT; i++)
        if (c->rate[i] > 0)
        {
            fprintf(f, "%s\"%s\":%g", first ? "" : ",", mode_names[i], c->rate[i]);
            first = 0;
        }
    fputs("},\"events\":{", f);
    for (uint32_t i = 0; i < c->event_count; i++)
        fprintf(f, "%s\"%u\":%u", i ? "," : "", c->events[i].id, c->events[i].first);
    fputs("},\"roulette\":{", f);
    first = 1;
    for (int r = 0; r < ROULETTE_ROWS; r++)
        if (c->roulette_set >> r & 1)
        {
            fprintf(f, "%s\"%d\":{\"values\":[", first ? "" : ",", r + 1);
            for (int s = 0; s < ROULETTE_SLOTS; s++)
                fprintf(f, "%s%d", s ? "," : "", c->roulette_value[r][s]);
            fputs("],\"rates\":[", f);
            for (int s = 0; s < ROULETTE_SLOTS; s++)
                fprintf(f, "%s%d", s ? "," : "", c->roulette_rate[r][s]);
            fputs("]}", f);
            first = 0;
        }
    fputs("}}\n", f);
    int bad = ferror(f);
    if (fclose(f) || bad || rename(tmp, path_json))
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Balance's rate for mode i, or 0 when Balance leaves it alone. */
static double balance_rate(int i)
{
    for (size_t k = 0; k < sizeof(balance_rates) / sizeof(balance_rates[0]); k++)
        if (!strcmp(balance_rates[k].mode, mode_names[i]))
            return balance_rates[k].rate;
    return 0;
}

/* The rate mode i gets without a value of the user's own. */
static double rate_baseline(int i)
{
    double b = config.balance_rates ? balance_rate(i) : 0;
    return b > 0 ? b : game_rate[i];
}

#ifdef PS5
/* Resolves a DMS table's row array and checks its row count. Returns 0 while
 * the tables are not ready, or when the table does not look as expected (then
 * *bad is set). */
static uintptr_t dms_rows(int pid, uintptr_t base, uint32_t id, uint32_t rows_expected, int *bad)
{
    uint8_t ready = 0;
    uint64_t store = 0, begin = 0, table = 0, rows = 0;
    unsigned char header[0x18];
    uint32_t ids[DMS_ID_COUNT];
    *bad = 0;
    if (gt7_game_read(pid, base + DMS_READY_RVA, &ready, 1) || ready != 1)
        return 0;
    if (gt7_game_read(pid, base + DMS_MANAGER_RVA + 8, &store, 8) || !store)
        return 0;
    if (gt7_game_read(pid, base + DMS_ID_LIST_RVA, ids, sizeof(ids)))
        return 0;
    int index = -1;
    for (int i = 0; i < DMS_ID_COUNT; i++)
        if (ids[i] == id)
            index = i;
    if (index < 0)
    {
        *bad = 1;
        return 0;
    }
    if (gt7_game_read(pid, store + (uint64_t)index * 24, &begin, 8) || !begin ||
        gt7_game_read(pid, begin, &table, 8) || !table ||
        gt7_game_read(pid, table, header, sizeof(header)))
        return 0;
    uint32_t count;
    memcpy(&count, header + 8, 4);
    memcpy(&rows, header + 0x10, 8);
    if (memcmp(header, "dmsb", 4) || count != rows_expected || !rows)
    {
        *bad = 1;
        return 0;
    }
    return (uintptr_t)rows;
}

/* The GameModeCoeff rows, with every row name checked. */
static uintptr_t game_mode_rows(int pid, uintptr_t base, int *bad)
{
    uintptr_t rows = dms_rows(pid, base, GAME_MODE_COEFF_ID, MODE_COUNT, bad);
    if (!rows)
        return 0;
    for (int i = 0; i < MODE_COUNT; i++)
    {
        uint64_t name_ptr = 0;
        char name[32] = {0};
        if (gt7_game_read(pid, rows + (uint64_t)i * 0x18 + 8, &name_ptr, 8) || !name_ptr ||
            gt7_game_read(pid, name_ptr, name, sizeof(name) - 1) || strcmp(name, mode_names[i]))
        {
            *bad = 1;
            return 0;
        }
    }
    return (uintptr_t)rows;
}

static void hook_note_set(const char *text)
{
    if (strcmp(hook_note, text))
    {
        snprintf(hook_note, sizeof(hook_note), "%s", text);
        mods_log("World Circuits prizes: %s", text);
    }
}

/* The loader's first 8 bytes: push rbp; mov rbp, rsp; push r15; push r14. */
static const unsigned char hook_original[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};

/* jmp rel32 to the hook, a nop, and the loader's own push r14: one aligned
 * 8-byte write that leaves the rest of the loader as it was. */
static void hook_jump(unsigned char out[8])
{
    int32_t rel = (int32_t)(EVENT_HOOK_RVA + EVENT_HOOK_ENTRY - (EVENT_HOOK_SITE_RVA + 5));
    out[0] = 0xe9;
    memcpy(out + 1, &rel, 4);
    out[5] = 0x90;
    out[6] = 0x41;
    out[7] = 0x56;
}

/* Balance's events, then the user's, which replace Balance's for the same id. */
static uint32_t hook_events(EventHookEntry *out)
{
    uint32_t n = 0;
    if (config.balance_events)
        for (size_t i = 0; i < sizeof(balance_events) / sizeof(balance_events[0]); i++)
            out[n++] = (EventHookEntry){balance_events[i][0], balance_events[i][1]};
    for (uint32_t i = 0; i < config.event_count; i++)
    {
        uint32_t k = 0;
        while (k < n && out[k].id != config.events[i].id)
            k++;
        out[k] = config.events[i];
        n += k == n;
    }
    return n;
}

/* Count first, so the hook never reads a half-written table. */
static int hook_write_table(int pid, uintptr_t base)
{
    static EventHookEntry table[EVENT_HOOK_TABLE_MAX];
    uintptr_t cave = base + EVENT_HOOK_RVA;
    uint32_t count = hook_events(table), zero = 0;
    if (gt7_game_write(pid, cave + EVENT_HOOK_OFF_COUNT, &zero, 4) ||
        (count && gt7_game_write(pid, base + EVENT_LOG_RVA + EVENT_TABLE_OFF, table, count * sizeof(EventHookEntry))) ||
        (count && gt7_game_write(pid, cave + EVENT_HOOK_OFF_COUNT, &count, 4)))
        return -1;
    mods_log("World Circuits prizes: %u events in the game's table", count);
    return 0;
}

static int hook_block(const char *why)
{
    hook_state = STATE_BLOCKED;
    hook_note_set(why);
    return -1;
}

/* Writes the hook and its table, then the loader jump; refuses unless both the
 * loader and the hook area hold the bytes this build expects. */
static int hook_install(int pid, uintptr_t base)
{
    uintptr_t cave = base + EVENT_HOOK_RVA, site = base + EVENT_HOOK_SITE_RVA;
    unsigned char jump[8], code[8], blob[sizeof(event_hook_blob)];
    hook_jump(jump);
    if (gt7_game_read(pid, site, code, sizeof(code)) || gt7_game_read(pid, cave, blob, sizeof(blob)))
        return -1;
    int empty = 1;
    for (size_t i = 0; i < sizeof(blob); i++)
        empty &= !blob[i];
    /* The header's counters change at run time: compare magic, ABI and code. */
    int ours = !memcmp(blob, event_hook_blob, EVENT_HOOK_OFF_COUNT) &&
               !memcmp(blob + 0x40, event_hook_blob + 0x40, sizeof(blob) - 0x40);
    int adopted = !memcmp(code, jump, sizeof(code)); /* hooked earlier this session */
    uintptr_t log = base + EVENT_LOG_RVA;
    unsigned char logged[EVENT_LOG_LIMIT - EVENT_LOG_RVA];
    if (gt7_game_read(pid, log, logged, sizeof(logged)))
        return -1;
    int log_empty = 1;
    for (size_t i = 0; i < sizeof(logged); i++)
        log_empty &= !logged[i];
    if (adopted)
    {
        if (!ours)
            return hook_block("The event loader already jumps to another hook build; nothing was changed.");
    }
    else if (memcmp(code, hook_original, sizeof(code)))
        return hook_block("The event loader is not the expected 01.710.000 code; nothing was changed.");
    else if (!empty && memcmp(blob, EVENT_HOOK_MAGIC, 8))
        return hook_block("The hook area is in use by something else; nothing was changed.");
    if (!log_empty && memcmp(logged, EVENT_LOG_MAGIC, 8))
        return hook_block("The hook log area is in use by something else; nothing was changed.");
    if (log_empty && gt7_game_write(pid, log, EVENT_LOG_MAGIC, 8))
        return -1;
    if (gt7_game_make_executable(pid, cave, EVENT_HOOK_LIMIT - EVENT_HOOK_RVA))
        return hook_block("Could not make the hook area executable; nothing was changed.");
    if (!ours && gt7_game_write(pid, cave, event_hook_blob, sizeof(event_hook_blob)))
        return -1;
    if (hook_write_table(pid, base))
        return -1;
    hook_dirty = 0;
    if (gt7_game_patch_code(pid, site, hook_original, jump, sizeof(jump)))
        return hook_block("Could not write the event loader jump; nothing was changed.");
    mods_log("event hook %s at %#lx (entry +%#x), log at %#lx", adopted ? "kept" : "installed",
             (unsigned long)cave, EVENT_HOOK_ENTRY, (unsigned long)log);
    hook_state = STATE_READY;
    return 0;
}

static void hook_tick(int pid, uintptr_t base)
{
    int wanted = config.balance_events || config.event_count;
    uintptr_t cave = base + EVENT_HOOK_RVA;
    if (hook_state == STATE_BLOCKED)
        return;
    if (hook_state == STATE_WAITING)
    {
        uint8_t ready = 0;
        if (!wanted)
        {
            hook_note_set("Off.");
            return;
        }
        if (gt7_game_read(pid, base + DMS_READY_RVA, &ready, 1) || ready != 1)
        {
            hook_note_set("Waiting for GT7 to load its data.");
            return;
        }
        if (hook_install(pid, base))
            return;
    }
    if (gt7_game_make_executable(pid, cave, EVENT_HOOK_LIMIT - EVENT_HOOK_RVA))
    {
        /* Without execute permission the next event load would crash: unhook. */
        unsigned char jump[8];
        hook_jump(jump);
        int r = gt7_game_patch_code(pid, base + EVENT_HOOK_SITE_RVA, jump, hook_original, sizeof(jump));
        hook_block(r ? "The hook area lost execute permission and the hook could not be removed."
                     : "The hook area lost execute permission; the hook was removed.");
        return;
    }
    if (hook_dirty && !hook_write_table(pid, base))
        hook_dirty = 0;
    unsigned char header[0x20];
    if (!gt7_game_read(pid, cave, header, sizeof(header)) && !memcmp(header, EVENT_HOOK_MAGIC, 8))
    {
        memcpy(&hook_head, header + EVENT_HOOK_OFF_HEAD, 4);
        memcpy(&hook_loads, header + EVENT_HOOK_OFF_LOADS, 4);
        memcpy(&hook_changed, header + EVENT_HOOK_OFF_CHANGED, 4);
        (void)gt7_game_read(pid, base + EVENT_LOG_RVA + EVENT_LOG_OFF_ENTRIES, hook_log, sizeof(hook_log));
    }
    hook_note_set(wanted ? "Active." : "Installed; prizes are the game's own (off).");
}

static void roulette_note_set(const char *text)
{
    if (strcmp(roulette_note, text))
    {
        snprintf(roulette_note, sizeof(roulette_note), "%s", text);
        mods_log("Roulette: %s", text);
    }
}

/* The user's ticket, else Balance's, else the game's. A slot that never pays
 * keeps the game's value. */
static void roulette_wanted(int r, int32_t out[ROULETTE_FIELDS])
{
    const int32_t *values = NULL, *rates = NULL;
    memcpy(out, roulette_game[r], sizeof(roulette_game[r]));
    if (config.roulette_set >> r & 1)
    {
        values = config.roulette_value[r];
        rates = config.roulette_rate[r];
    }
    for (size_t i = 0; !values && config.balance_roulette && i < sizeof(balance_tickets) / sizeof(balance_tickets[0]); i++)
        if (balance_tickets[i][0] == r + 1)
        {
            values = &balance_tickets[i][1];
            rates = &balance_tickets[i][1 + ROULETTE_SLOTS];
        }
    for (int s = 0; values && s < ROULETTE_SLOTS; s++)
    {
        if (rates[s])
            out[4 + 2 * s] = values[s];
        out[5 + 2 * s] = rates[s];
    }
}

/* Keeps table 0x265 at the configured odds, or at the game's. Refuses a
 * session whose table holds neither when first seen. */
static void roulette_tick(int pid, uintptr_t base)
{
    static int32_t current[ROULETTE_ROWS][ROULETTE_FIELDS];
    int32_t want[ROULETTE_FIELDS];
    if (roulette_state == STATE_BLOCKED)
        return;
    int bad;
    uintptr_t rows = dms_rows(pid, base, ROULETTE_ID, ROULETTE_ROWS, &bad);
    if (!rows)
    {
        if (bad)
        {
            roulette_state = STATE_BLOCKED;
            roulette_note_set("GT7's roulette table does not look as expected; nothing was changed this session.");
        }
        else
            roulette_note_set("Waiting for GT7 to load its data.");
        return;
    }
    if (gt7_game_read(pid, rows, current, sizeof(current)))
        return;
    if (!roulette_checked)
    {
        for (int r = 0; r < ROULETTE_ROWS; r++)
        {
            roulette_wanted(r, want);
            if (memcmp(current[r], roulette_game[r], sizeof(want)) && memcmp(current[r], want, sizeof(want)))
            {
                roulette_state = STATE_BLOCKED;
                roulette_note_set("GT7's roulette odds are not the game's or the saved ones; nothing was changed this session.");
                return;
            }
        }
        roulette_checked = 1;
        mods_log("Roulette: table at %#lx", (unsigned long)rows);
    }
    int failed = 0;
    for (int r = 0; r < ROULETTE_ROWS; r++)
    {
        roulette_wanted(r, want);
        if (!memcmp(current[r], want, sizeof(want)))
            continue;
        if (gt7_game_write(pid, rows + (uintptr_t)r * sizeof(want), want, sizeof(want)))
            failed++;
        else
            mods_log("Roulette: ticket %d %s", r + 1, memcmp(want, roulette_game[r], sizeof(want)) ? "changed" : "restored");
    }
    roulette_state = STATE_READY;
    if (failed)
        roulette_note_set("Could not write some ticket odds; will retry.");
    else
        roulette_note_set(config.balance_roulette || config.roulette_set ? "Changed odds are in the running game." : "The game's own odds.");
}

static void tick(void)
{
    int pid;
    uintptr_t base;
    if (gt7_game_pid(&pid, &base))
    {
        session_pid = 0;
        hook_state = STATE_WAITING;
        hook_note_set("Waiting for GT7.");
        roulette_state = STATE_WAITING;
        roulette_checked = 0;
        roulette_note_set("Waiting for GT7.");
        set_state("Waiting for GT7.");
        return;
    }
    if (pid != session_pid)
    {
        session_pid = pid;
        session_blocked = have_current = 0;
        pending = 1;
        hook_state = STATE_WAITING;
        hook_dirty = 1;
        hook_loads = hook_changed = hook_head = 0;
        memset(hook_log, 0, sizeof(hook_log));
        roulette_state = STATE_WAITING;
        roulette_checked = 0;
    }
    hook_tick(pid, base);
    roulette_tick(pid, base);
    if (session_blocked)
        return;
    int bad;
    uintptr_t rows = game_mode_rows(pid, base, &bad);
    if (!rows)
    {
        if (bad)
        {
            session_blocked = 1;
            set_state("GT7's payout table does not look as expected; nothing was changed this session.");
        }
        else
            set_state("Waiting for GT7 to load its data.");
        return;
    }
    for (int i = 0; i < MODE_COUNT; i++)
        if (gt7_game_read(pid, rows + (uintptr_t)i * 0x18 + 0x10, &current[i], 8))
            return;
    if (!have_current)
    {
        have_current = 1;
        mods_log("session pid %d: GameModeCoeff found at %#lx", pid, (unsigned long)rows);
        for (int i = 0; i < MODE_COUNT; i++)
            if (current[i] != game_rate[i])
                mods_log("%s was %g, not the game's %g, when found", mode_names[i], current[i], game_rate[i]);
    }
    int changed = 0, failed = 0;
    for (int i = 0; i < MODE_COUNT; i++)
    {
        double want = config.rate[i] > 0 ? config.rate[i] : rate_baseline(i);
        if (current[i] == want)
            continue;
        if (gt7_game_write(pid, rows + (uintptr_t)i * 0x18 + 0x10, &want, 8))
        {
            failed++;
            continue;
        }
        mods_log("%s: %g -> %g", mode_names[i], current[i], want);
        current[i] = want;
        changed++;
    }
    if (failed)
        set_state("Could not write some payout rates; will retry.");
    else if (changed || pending)
    {
        pending = 0;
        set_state("Payout rates are in the running game.");
    }
}

static void *thread_main(void *unused)
{
    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&lock);
        tick();
        pthread_mutex_unlock(&lock);
        sleep(3);
    }
    return NULL;
}
#endif

void mods_start(const char *root)
{
    snprintf(path_json, sizeof(path_json), "%s/mods.json", root);
    snprintf(path_log, sizeof(path_log), "%s/mods.log", root);
    FILE *f = fopen(path_json, "r");
    if (f)
    {
        static char buf[16384];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        int r = parse(buf, &config);
        if (r < 0)
            defaults(&config);
        if (r)
            mods_log("%s; starting from the recommended settings", r < 0 ? "mods.json is invalid" : "mods.json predates Balance");
        if (r && write_config(&config))
            mods_log("could not save the recommended settings");
    }
    else
    {
        defaults(&config);
        if (write_config(&config))
            mods_log("could not save the recommended settings");
    }
#ifdef PS5
    pthread_t thread;
    if (pthread_create(&thread, NULL, thread_main, NULL) == 0)
        pthread_detach(thread);
    else
        mods_log("could not start the modifier thread");
#endif
}

/* Appends to out at n; returns the new length, or -1 once anything overflowed. */
__attribute__((format(printf, 4, 5))) static int append(char *out, size_t size, int n, const char *fmt, ...)
{
    if (n < 0 || (size_t)n >= size)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out + n, size - (size_t)n, fmt, ap);
    va_end(ap);
    return (r < 0 || (size_t)r >= size - (size_t)n) ? -1 : n + r;
}

int mods_status_json(char *out, size_t size)
{
    pthread_mutex_lock(&lock);
    int n = append(out, size, 0, "{\"balance\":{\"rates\":%s,\"events\":%s,\"roulette\":%s},\"state\":\"%s\",\"pid\":%d,\"rates\":[",
                   config.balance_rates ? "true" : "false", config.balance_events ? "true" : "false",
                   config.balance_roulette ? "true" : "false", state, session_pid);
    for (int i = 0; i < MODE_COUNT; i++)
    {
        char cur[32] = "null";
        if (have_current && session_pid)
            snprintf(cur, sizeof(cur), "%g", current[i]);
        n = append(out, size, n, "%s{\"name\":\"%s\",\"yours\":%g,\"game\":%g,\"balance\":%g,\"current\":%s}",
                   i ? "," : "", mode_names[i], config.rate[i] > 0 ? config.rate[i] : 0.0, game_rate[i], balance_rate(i), cur);
    }
    n = append(out, size, n, "],\"events\":{\"hook\":\"%s\",\"active\":%s,"
               "\"loads\":%u,\"changed\":%u,\"max\":%u,\"yours\":{",
               hook_note, hook_state == STATE_READY ? "true" : "false",
               hook_loads, hook_changed, (unsigned)EVENT_OVERRIDES_MAX);
    for (uint32_t i = 0; i < config.event_count; i++)
        n = append(out, size, n, "%s\"%u\":%u", i ? "," : "", config.events[i].id, config.events[i].first);
    n = append(out, size, n, "},\"recent\":[");
    for (uint32_t i = 0; i < EVENT_LOG_COUNT && i < hook_head; i++)
    {
        const EventHookLog *e = &hook_log[(hook_head - 1 - i) % EVENT_LOG_COUNT];
        char course[EVENT_LOG_COURSE];
        size_t k = 0;
        for (; k < sizeof(course) - 1 && e->course[k]; k++)
            course[k] = isalnum((unsigned char)e->course[k]) || e->course[k] == '_' ? e->course[k] : '?';
        course[k] = 0;
        n = append(out, size, n, "%s{\"id\":%u,\"original\":%u,\"new\":%u,\"places\":%u,\"course\":\"%s\"}",
                   i ? "," : "", e->id, e->original_first, e->new_first, e->places, course);
    }
    n = append(out, size, n, "]},\"roulette\":{\"state\":\"%s\",\"tickets\":[", roulette_note);
    for (int r = 0; r < ROULETTE_ROWS; r++)
    {
        n = append(out, size, n, "%s{\"id\":%d,\"level\":%d,\"kind\":%d,\"game\":[", r ? "," : "", r + 1,
                   roulette_game[r][1], roulette_game[r][2]);
        for (int f = 4; f < ROULETTE_FIELDS; f++)
            n = append(out, size, n, "%s%d", f > 4 ? "," : "", roulette_game[r][f]);
        if (config.roulette_set >> r & 1)
        {
            n = append(out, size, n, "],\"yours\":[");
            for (int s = 0; s < ROULETTE_SLOTS; s++)
                n = append(out, size, n, "%s%d,%d", s ? "," : "", config.roulette_value[r][s], config.roulette_rate[r][s]);
        }
        n = append(out, size, n, "]}");
    }
    n = append(out, size, n, "]}}");
    pthread_mutex_unlock(&lock);
    return n > 0 ? n : -1;
}

int mods_save(const char *body)
{
    ModsConfig c;
    if (!body || parse(body, &c))
        return -1;
    pthread_mutex_lock(&lock);
    int r = write_config(&c);
    if (!r)
    {
        config = c;
        pending = 1;
        hook_dirty = 1;
        mods_log("settings saved (Balance: rates %d, events %d, roulette %d)", c.balance_rates, c.balance_events, c.balance_roulette);
#ifdef PS5
        tick();
#endif
    }
    pthread_mutex_unlock(&lock);
    return r ? -2 : 0;
}
