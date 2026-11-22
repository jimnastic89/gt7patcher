/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include "assets.h"
#include "live_bridge.h"

static pthread_mutex_t config_io_lock = PTHREAD_MUTEX_INITIALIZER;

/* The country patch needs a code cave in executable padding. That padding
 * is shared with the payload's live-settings bootstrap, so the cave must
 * sit beyond the reserved bootstrap region. */
enum
{
    COUNTRY_CAVE = 0x430ea10,
    ASPECT_CAVE = 0x430eaa0
};
_Static_assert(COUNTRY_CAVE >= LIVE_IMAGE_BASE + LIVE_BOOT_RVA + LIVE_BOOT_RESERVED,
               "country code cave overlaps the live-settings bootstrap region");
_Static_assert(ASPECT_CAVE >= COUNTRY_CAVE + 56 && ASPECT_CAVE >= LIVE_IMAGE_BASE + LIVE_BOOT_RVA + LIVE_BOOT_RESERVED,
               "aspect cave must clear the country cave and the live-settings bootstrap");

/* Player-country table. Order MUST match the <select id="country"> options in
 * web/index.html. GT7's profile/flag dependency uses ISO alpha-3 identifiers;
 * the UI continues to display the corresponding alpha-2 labels. */
static const char *const country_codes[COUNTRY_COUNT] = {
    "AUS", "AUT", "BEL", "BRA", "CAN", "CHE", "CHL", "CHN", "CZE", "DEU", "DNK", "ESP", "FIN", "FRA", "GBR",
    "GRC", "HKG", "HRV", "HUN", "IDN", "IRL", "ISR", "IND", "ITA", "JPN", "KOR", "MEX", "MYS", "NLD", "NOR",
    "NZL", "PHL", "POL", "PRT", "ROU", "RUS", "SWE", "SGP", "SVK", "THA", "TUR", "TWN", "UKR", "USA", "ZAF", "ARG"};
const char *country_code(int i)
{
    return (i >= 0 && i < COUNTRY_COUNT) ? country_codes[i] : "";
}

/* Video-output aspect-ratio overrides. GT7 keeps its design aspect as a
 * shared 16:9 float literal at RVA 0x04af7478 that feeds the camera
 * projection, the render-target sizing and many 2D scaling maths, so it
 * cannot be rewritten globally (that desyncs the output and blanks the
 * screen). The two symmetric camera-projection operands at
 * 0x921909/0x921911 are instead redirected to a private cave holding
 * +/-ratio, leaving the shared literal at 16:9. Order MUST match the
 * <select id="aspect"> options in web/index.html; index -1 leaves the
 * game's own value untouched. */
static const struct
{
    int w, h;
} aspect_ratios[] =
    {{16, 10}, {21, 9}, {32, 9}, {3, 2}, {4, 3}, {5, 4}, {2, 1}, {235, 100}, {239, 100}};
_Static_assert(sizeof(aspect_ratios) / sizeof(aspect_ratios[0]) == ASPECT_COUNT,
               "aspect ratio table size must match ASPECT_COUNT");
static float aspect_value(int i) { return (float)aspect_ratios[i].w / (float)aspect_ratios[i].h; }

Config defaults(void) { return (Config){1, 1, 1, 2, -1, 0, 0, 2, 2, 1, 0, 0, -1}; }
int valid_config(const Config *c)
{
    return c->mask <= (1U << PATCH_COUNT) - 1 &&
           c->preset >= 0 && c->preset <= 5 && c->units >= 0 && c->units <= 1 &&
           c->language >= -1 && c->language <= 26 && c->country >= -1 && c->country < COUNTRY_COUNT &&
           c->speed >= 0 && c->speed <= 1 &&
           c->mass >= 0 && c->mass <= 1 && c->power >= 0 && c->power <= 2 &&
           c->torque >= 0 && c->torque <= 2 && c->temperature >= 1 && c->temperature <= 2 && c->autostart >= 0 && c->autostart <= 1 && c->important_dismissed >= 0 && c->important_dismissed <= 1 && c->aspect >= -1 && c->aspect < ASPECT_COUNT;
}
int parse_config(const char *body, Config *c)
{
    unsigned seen = 0;
    char copy[512];
    if (strlen(body) >= sizeof(copy))
        return -1;
    strcpy(copy, body);
    memset(c, 0, sizeof(*c));
    c->country = -1;
    c->aspect = -1;
    for (char *item = strtok(copy, "&"); item; item = strtok(NULL, "&"))
    {
        char *eq = strchr(item, '=');
        if (!eq)
            return -1;
        *eq++ = 0;
        if (!*eq)
            return -1;
        char *end;
        long v = strtol(eq, &end, 10);
        if (*end || v < -1 || v > (long)((1U << PATCH_COUNT) - 1U))
            return -1;
        const char *keys[] = {"mask", "preset", "units", "language", "speed", "mass", "power", "torque", "temperature", "autostart", "country", "aspect", "important"};
        int k;
        for (k = 0; k < 13; k++)
            if (!strcmp(item, keys[k]))
                break;
        if (k == 13 || (seen & (1U << k)))
            return -1;
        seen |= 1U << k;
        switch (k)
        {
        case 0:
            c->mask = (unsigned)v;
            break;
        case 1:
            c->preset = (int)v;
            break;
        case 2:
            c->units = (int)v;
            break;
        case 3:
            c->language = (int)v;
            break;
        case 4:
            c->speed = (int)v;
            break;
        case 5:
            c->mass = (int)v;
            break;
        case 6:
            c->power = (int)v;
            break;
        case 7:
            c->torque = (int)v;
            break;
        case 8:
            c->temperature = (int)v;
            break;
        case 9:
            c->autostart = (int)v;
            break;
        case 10:
            c->country = (int)v;
            break;
        case 11:
            c->aspect = (int)v;
            break;
        case 12:
            c->important_dismissed = (int)v;
            break;
        }
    }
    /* Accept the full 12-key payload, plus the 9/10/11-key payloads written by
     * older builds (which had no country and/or aspect field). */
    unsigned full = (1U << 13) - 1;
    if (!(LIVE_SETTINGS_RELEASE_ENABLED))
        c->mask &= ~LIVE_SETTINGS_BIT;
    return (seen == full || seen == ((1U << 12) - 1) || seen == ((1U << 11) - 1) || seen == ((1U << 10) - 1) || seen == ((1U << 9) - 1)) && valid_config(c) ? 0 : -1;
}
/*static void line(FILE *f, uint32_t a, const unsigned char *b, size_t n)
{
    fprintf(f, "<Line Type=\"bytes\" Address=\"0x%08x\" Value=\"", a);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%02x", b[i]);
    fputs("\"/>\n", f);
}*/
static void store(unsigned char *b, uint32_t a, uint32_t target, uint32_t value)
{
    b[0] = 0xc7;
    b[1] = 5;
    int32_t displacement = (int32_t)(target - (a + 10));
    memcpy(b + 2, &displacement, 4);
    memcpy(b + 6, &value, 4);
}
static void put32(unsigned char *b, int32_t v) { memcpy(b, &v, 4); }
static int plan_add(PatchPlan *plan, PatchOperationType type, uintptr_t address,
                    const unsigned char *expected, const unsigned char *replacement,
                    size_t length, unsigned feature, const char *name)
{
    if (plan->count >= PATCH_PLAN_MAX_OPERATIONS)
        return -1;
    plan->operations[plan->count++] = (PatchOperation){type, address, expected,
                                                       replacement, length, feature, name};
    return 0;
}
static unsigned char *plan_bytes(PatchPlan *plan, size_t length)
{
    unsigned char *result;
    if (length > sizeof(plan->generated) - plan->generated_used)
        return NULL;
    result = plan->generated + plan->generated_used;
    plan->generated_used += length;
    return result;
}
static int add_generated(PatchPlan *plan, uintptr_t address, const unsigned char *bytes,
                         size_t length, unsigned feature, const char *name)
{
    unsigned char *copy = plan_bytes(plan, length);
    if (!copy)
        return -1;
    memcpy(copy, bytes, length);
    return plan_add(plan, PATCH_OPERATION_BYTES, address, NULL, copy, length, feature, name);
}
static int add_country(PatchPlan *plan, int country)
{
    static const char codes[][3] = {
        "AU", "AT", "BE", "BR", "CA", "CH", "CL", "CN", "CZ", "DE", "DK", "ES", "FI", "FR", "GB",
        "GR", "HK", "HR", "HU", "ID", "IE", "IL", "IN", "IT", "JP", "KR", "MX", "MY", "NL", "NO",
        "NZ", "PH", "PL", "PT", "RO", "RU", "SE", "SG", "SK", "TH", "TR", "TW", "UA", "US", "ZA", "AR"};
    _Static_assert(sizeof(codes) / sizeof(codes[0]) == COUNTRY_COUNT, "country table");
    const uint32_t site = 0x3b97047;
    static const unsigned char expected[] = {
        0xc4, 0xa1, 0x7c, 0x11, 0x84, 0x20, 0xd8, 0x03, 0x00, 0x00};
    unsigned char stub[] = {
        0xc4, 0xa1, 0x7c, 0x11, 0x84, 0x20, 0xd8, 0x03, 0x00, 0x00, /* displaced store */
        0x9c,                                                       /* pushfq */
        0x41, 0x83, 0xff, 0x00,                                     /* cmp r15d, 0 */
        0x75, 0x15,                                                 /* jne restore */
        0x48, 0x81, 0x7d, 0x08, 0x1e, 0x6c, 0xb9, 0x03,             /* cmp [rbp+8], 0x3b96c1e */
        0x75, 0x0b,                                                 /* jne restore */
        0xc7, 0x84, 0x20, 0xf8, 0x03, 0x00, 0x00, 0, 0, 0, 0,       /* store alpha-2 + NUL */
        0x9d,                                                       /* popfq */
        0xe9, 0, 0, 0, 0};
    unsigned char jump[] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    memcpy(stub + 34, codes[country], 2);
    put32(stub + sizeof(stub) - 4,
          (int32_t)(site + sizeof(jump) - (COUNTRY_CAVE + sizeof(stub))));
    put32(jump + 1, (int32_t)(COUNTRY_CAVE - (site + 5)));
    unsigned char *payload = plan_bytes(plan, sizeof(stub) + sizeof(jump));
    if (!payload)
        return -1;
    memcpy(payload, stub, sizeof(stub));
    memcpy(payload + sizeof(stub), jump, sizeof(jump));
    return plan_add(plan, PATCH_OPERATION_COUNTRY, site, expected, payload, sizeof(stub),
                    PATCH_COUNT + 2, "Player country");
}
int build_patch_plan(const Config *c, PatchPlan *plan)
{
    if (!c || !plan || !valid_config(c))
        return -1;
    memset(plan, 0, sizeof(*plan));
    for (unsigned i = 0; i < PATCH_COUNT; i++)
        if (c->mask & (1U << i))
        {
            if (i == 6)
                plan->live_bridge = 1;
            else if (i == 8)
            {
                if (plan_add(plan, PATCH_OPERATION_DEBUG_ENVIRONMENT, 0, NULL, NULL, 0,
                             i, feature_name(i)))
                    return -1;
            }
            else if (i == 9)
            {
                if (plan_add(plan, PATCH_OPERATION_CLOCK_FIX, 0, NULL, NULL, 0,
                             i, feature_name(i)))
                    return -1;
            }
            else
                for (unsigned j = patch_offsets[i]; j < patch_offsets[i] + patch_counts[i]; j++)
                    if (plan_add(plan, PATCH_OPERATION_BYTES, patch_definitions[j].address, NULL,
                                 patch_definitions[j].replacement, patch_definitions[j].length,
                                 i, feature_name(i)))
                        return -1;
        }
    plan->wheel_runtime = (c->mask & WHEEL_OFFSET_CUSTOM_BIT) != 0;
    if (c->units)
    {
        unsigned char bytes[71];
        uint32_t values[] = {c->speed, c->power, c->torque, 3, c->temperature, 0, c->mass};
        for (unsigned i = 0; i < 7; i++)
            store(bytes + i * 10, 0xc22060 + i * 10, 0x611aecc + i * 4, values[i]);
        bytes[70] = 0xc3;
        if (add_generated(plan, 0xc22060, bytes, sizeof(bytes), PATCH_COUNT, "Display units"))
            return -1;
        const uint32_t addresses[] = {0x1c4e7f0, 0x1c4e800, 0x1c4e810, 0x1c4e820};
        const uint32_t targets[] = {0x611aed0, 0x611aedc, 0x611aed4, 0x611aecc};
        const uint32_t values2[] = {c->power, c->temperature, c->torque, c->speed};
        for (unsigned i = 0; i < 4; i++)
        {
            unsigned char value[11];
            store(value, addresses[i], targets[i], values2[i]);
            value[10] = 0xc3;
            if (add_generated(plan, addresses[i], value, sizeof(value), PATCH_COUNT, "Display units"))
                return -1;
        }
    }
    if (c->language >= 0)
    {
        unsigned char bytes[] = {0xbf, 0, 0, 0, 0};
        uint32_t language = (uint32_t)c->language;
        memcpy(bytes + 1, &language, sizeof(language));
        if (add_generated(plan, 0xc21e93, bytes, sizeof(bytes), PATCH_COUNT + 1, "Game language"))
            return -1;
        bytes[0] = 0xeb;
        bytes[1] = 3;
        if (add_generated(plan, 0xc21e98, bytes, 2, PATCH_COUNT + 1, "Game language"))
            return -1;
    }
    if (c->country >= 0 && add_country(plan, c->country))
        return -1;
    if (c->aspect >= 0)
    {
        const uint32_t site_pos = 0x92190d, site_neg = 0x921915;
        unsigned char payload[16];
        float aspect = aspect_value(c->aspect), negative = -aspect;
        put32(payload, (int32_t)(ASPECT_CAVE - (site_pos + 4)));
        put32(payload + 4, (int32_t)((ASPECT_CAVE + 4) - (site_neg + 4)));
        memcpy(payload + 8, &aspect, 4);
        memcpy(payload + 12, &negative, 4);
        unsigned char *copy = plan_bytes(plan, sizeof(payload));
        if (!copy || (memcpy(copy, payload, sizeof(payload)), plan_add(plan, PATCH_OPERATION_ASPECT,
                                                                       site_pos, NULL, copy, sizeof(payload), PATCH_COUNT + 3, "Video aspect ratio")))
            return -1;
    }
    return 0;
}
/*int emit_xml(FILE *f, const Config *c)
{
    PatchPlan plan;
    if (build_patch_plan(c, &plan))
        return -1;
    fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Patch><TitleID><ID>PPSA01317</ID></TitleID>\n"
          "<Metadata Title=\"Gran Turismo 7\" Name=\"GT7 selected patches\" Author=\"AI-generated; based on High-Speed007 and PSVIETHOA; GT7Patcher\" PatchVer=\"1.0\" AppVer=\"01.710.000\" ImageBase=\"0x00400000\" AppElf=\"eboot.bin\"><PatchList>\n",
          f);
    unsigned last_feature = ~0U;
    for (size_t i = 0; i < plan.count; i++)
    {
        PatchOperation *op = &plan.operations[i];
        if (op->feature != last_feature)
        {
            if (op->feature < PATCH_COUNT)
                fprintf(f, "<!-- GT7FEATURE %u -->\n", op->feature);
            else
                fprintf(f, "<!-- GT7FEATURE %s -->\n", op->name);
            last_feature = op->feature;
        }
        if (op->type == PATCH_OPERATION_BYTES)
            line(f, (uint32_t)op->address, op->replacement, op->length);
        else if (op->type == PATCH_OPERATION_DEBUG_ENVIRONMENT)
            line(f, 0x060e83b0, (const unsigned char *)"debug\0\0", 7);
        else if (op->type == PATCH_OPERATION_CLOCK_FIX)
            line(f, 0x060e83c0, NULL, 0);
        else if (op->type == PATCH_OPERATION_COUNTRY)
        {
            line(f, COUNTRY_CAVE, op->replacement, op->length);
            line(f, (uint32_t)op->address, op->replacement + op->length, 10);
        }
        else if (op->type == PATCH_OPERATION_ASPECT)
        {
            line(f, (uint32_t)op->address, op->replacement, 4);
            line(f, 0x921915, op->replacement + 4, 4);
            line(f, ASPECT_CAVE, op->replacement + 8, 8);
        }
    }
    if (plan.live_bridge)
        fputs("<!-- GT7LIVEHOOK 1 -->\n", f);
    fputs("</PatchList></Metadata>\n", f);
    fprintf(f, "<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d %d %d %d -->\n", c->mask, c->preset, c->units, c->language, c->speed, c->mass, c->power, c->torque, c->temperature, c->autostart, c->country, c->aspect, c->important_dismissed);
    fputs("</Patch>\n", f);
    return ferror(f) ? -1 : 0;
}*/
static int save_config_unlocked(const char *root, const Config *c)
{
    char path[512], temp[520];
    char patches[512];
    size_t patch_length = 0;
    patches[0] = 0;
    for (unsigned i = 0; i < PATCH_COUNT; i++)
        if (c->mask & (1U << i))
        {
            int written = snprintf(patches + patch_length, sizeof(patches) - patch_length,
                                   "%s%s", patch_length ? "," : "", feature_source(i));
            if (written < 0 || (size_t)written >= sizeof(patches) - patch_length)
                return -1;
            patch_length += (size_t)written;
        }
    if (snprintf(path, sizeof(path), "%s/config.json", root) >= (int)sizeof(path))
        return -1;
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "wb");
    if (!f)
        return -1;
    int r = fprintf(f,
                    "{\n  \"patches\": \"%s\",\n  \"preset\": %d,\n  \"units\": %d,\n"
                    "  \"language\": %d,\n  \"speed\": %d,\n  \"mass\": %d,\n"
                    "  \"power\": %d,\n  \"torque\": %d,\n  \"temperature\": %d,\n"
                    "  \"autostart\": %d,\n  \"country\": %d,\n  \"aspect\": %d,\n  \"important\": %d\n}\n",
                    patches, c->preset, c->units, c->language, c->speed, c->mass, c->power,
                    c->torque, c->temperature, c->autostart, c->country, c->aspect,
                    c->important_dismissed) < 0;
    if (fflush(f) || fsync(fileno(f)))
        r = -1;
    if (fclose(f) || r || rename(temp, path))
    {
        unlink(temp);
        return -1;
    }
    return 0;
}
int save_config(const char *root, const Config *c)
{
    int r;
    pthread_mutex_lock(&config_io_lock);
    r = save_config_unlocked(root, c);
    pthread_mutex_unlock(&config_io_lock);
    return r;
}
static int load_legacy_config(const char *root, Config *c)
{
    char path[512], buf[512];
    if (snprintf(path, sizeof(path), "%s/patches/xml_prospero/PPSA01317.xml", root) >= (int)sizeof(path))
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? 1 : -1;
    int r = -1, has_race_info = 0, has_debug_environment = 0, has_live_hook = 0, has_opening_movie_skip = 0;
    Config loaded = {0};
    unsigned saved_mask = 0;
    while (fgets(buf, sizeof(buf), f))
    {
        if (strstr(buf, "<!-- GT7LIVEHOOK 1 -->"))
            has_live_hook = 1;
        if (strstr(buf, "<!-- GT7FEATURE 7 -->"))
            has_opening_movie_skip = 1;
        if (strstr(buf, "Name=\"Race Info fifth blank option (experimental)\""))
            has_race_info = 1;
        if (strstr(buf, "Address=\"0x060ec3b0\"") || strstr(buf, "Address=\"0x060e83b0\"") || strstr(buf, "Address=\"0x05ce83b0\""))
            has_debug_environment = 1;
        unsigned mask;
        Config x = {0};
        x.country = -1;
        x.aspect = -1;
        int fields = sscanf(buf, "<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d %d %d %d", &mask, &x.preset, &x.units, &x.language, &x.speed, &x.mass, &x.power, &x.torque, &x.temperature, &x.autostart, &x.country, &x.aspect, &x.important_dismissed);
        if (fields >= 9 && fields <= 13)
        {
            /* HUD builds may save bit 5 for their experimental Race Info
             * option and bit 6 for Wheel Offset. Preserve the supported
             * Wheel Offset choice, discard the unsupported experiment. */
            if (mask <= (1U << PATCH_COUNT) - 1U)
            {
                x.mask = 0;
                if (valid_config(&x))
                {
                    loaded = x;
                    saved_mask = mask;
                    r = 0;
                }
            }
        }
    }
    fclose(f);
    if (r)
        return r;
    unsigned normalized = (saved_mask & 0x1fU) | (saved_mask & DEBUG_ENVIRONMENT_BIT) | (saved_mask & CLOCK_FIX_BIT) | (saved_mask & MUSIC_JINGLE_FILTER_BIT) | (saved_mask & FREE_PHOTO_CAMERA_BIT);
    /* Bit 6 is Wheel Offset in HUD snapshots, but was Debug environment in
     * native snapshots. Preserve the independent wheel option; the Debug
     * environment selection round-trips on its own bit above. */
    if ((saved_mask & 0x40U) && has_live_hook)
        normalized |= LIVE_SETTINGS_BIT;
    else if ((saved_mask & 0x40U) && !has_live_hook && !has_debug_environment)
        normalized |= 0x20U;
    if ((saved_mask & 0x20U) && (!has_race_info || has_debug_environment))
        normalized |= 0x20U;
    if ((saved_mask & OPENING_MOVIE_SKIP_BIT) && has_opening_movie_skip)
        normalized |= OPENING_MOVIE_SKIP_BIT;
    loaded.mask = normalized;
#if !LIVE_SETTINGS_RELEASE_ENABLED
    loaded.mask &= ~LIVE_SETTINGS_BIT;
#endif
    if (!valid_config(&loaded))
        return -1;
    *c = loaded;
    return 0;
}
static int json_number(const char *json, const char *key, long *value)
{
    char needle[32];
    const char *p;
    char *end;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
        return -1;
    p = strstr(json, needle);
    if (!p || !(p = strchr(p + strlen(needle), ':')))
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    *value = strtol(p, &end, 10);
    return end == p ? -1 : 0;
}

static int json_string(const char *json, const char *key, char *value, size_t size)
{
    char needle[32];
    const char *p, *end;
    size_t length;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
        return -1;
    p = strstr(json, needle);
    if (!p || !(p = strchr(p + strlen(needle), ':')))
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != '"' || !(end = strchr(p, '"')))
        return -1;
    length = (size_t)(end - p);
    if (length >= size)
        return -1;
    memcpy(value, p, length);
    value[length] = 0;
    return 0;
}

static int patch_mask_from_sources(const char *sources, unsigned *mask)
{
    char copy[512];
    char *source;
    if (strlen(sources) >= sizeof(copy))
        return -1;
    strcpy(copy, sources);
    *mask = 0;
    for (source = strtok(copy, ","); source; source = strtok(NULL, ","))
    {
        unsigned i;
        for (i = 0; i < PATCH_COUNT && strcmp(source, feature_source(i)); i++)
            ;
        if (i == PATCH_COUNT)
            return -1;
        *mask |= 1U << i;
    }
    return 0;
}

static int load_config_unlocked(const char *root, Config *c)
{
    char path[512], json[4096], form[512], sources[512];
    const char *keys[] = {"preset", "units", "language", "speed", "mass", "power",
                          "torque", "temperature", "autostart", "country", "aspect", "important"};
    long values[12], old_mask;
    unsigned mask;
    if (snprintf(path, sizeof(path), "%s/config.json", root) >= (int)sizeof(path))
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file)
        return errno == ENOENT ? 1 : -1;
    size_t size = fread(json, 1, sizeof(json) - 1, file);
    int valid = feof(file) && !ferror(file) && !memchr(json, 0, size);
    fclose(file);
    if (!valid)
        return -1;
    json[size] = 0;
    if (!json_string(json, "patches", sources, sizeof(sources)))
    {
        if (patch_mask_from_sources(sources, &mask))
            return -1;
    }
    else
    {
        if (json_number(json, "mask", &old_mask) || old_mask < 0 || old_mask > (long)((1U << PATCH_COUNT) - 1U))
            return -1;
        mask = (unsigned)old_mask;
    }
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (json_number(json, keys[i], &values[i]))
            return -1;
    if (snprintf(form, sizeof(form),
                 "mask=%u&preset=%ld&units=%ld&language=%ld&speed=%ld&mass=%ld&power=%ld&torque=%ld&temperature=%ld&autostart=%ld&country=%ld&aspect=%ld&important=%ld",
                 mask, values[0], values[1], values[2], values[3], values[4], values[5],
                 values[6], values[7], values[8], values[9], values[10], values[11]) >= (int)sizeof(form))
        return -1;
    return parse_config(form, c);
}

int load_config(const char *root, Config *c)
{
    int r;
    pthread_mutex_lock(&config_io_lock);
    r = load_config_unlocked(root, c);
    pthread_mutex_unlock(&config_io_lock);
    return r;
}

int initialize_config(const char *root, Config *c)
{
    int result = load_config(root, c);
    if (result != 1)
        return result;
    result = load_legacy_config(root, c);
    if (result < 0)
        return -1;
    if (result == 1)
        *c = defaults();
    if (save_config(root, c))
        return -1;
    return load_config(root, c);
}
const char *feature_name(unsigned i) { return patch_names[i]; }
const char *feature_note(unsigned i) { return patch_notes[i]; }
const char *feature_source(unsigned i) { return patch_sources[i]; }
unsigned feature_count(unsigned i) { return patch_counts[i]; }
const char *page_html(void) { return web_page; }
