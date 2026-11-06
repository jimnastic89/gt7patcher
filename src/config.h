/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdio.h>
#define PATCH_COUNT 10
#define LIVE_SETTINGS_BIT (1U << 6)
#define LIVE_SETTINGS_RELEASE_ENABLED 1
#define OPENING_MOVIE_SKIP_BIT (1U << 7)
/* Hidden "Debug environment" build-string patch. Locates the internal
 * "release" string and rewrites it to "debug"; see apply_debug_environment. */
#define DEBUG_ENVIRONMENT_BIT (1U << 8)
/* Sync Game Clock to System. Not a byte patch: the engine resolves libSceRtc in the
 * live game at launch and retargets the clock source of sceRtcGetCurrentTick
 * (see apply_clock_fix). It is applied only when the user selects it. */
#define CLOCK_FIX_BIT (1U << 9)
/* Number of selectable player countries exposed by the payload UI. The order
 * of the ISO alpha-2 table in config.c MUST match the <select> options in
 * web/index.html. Index -1 means "leave the game's own value alone". */
#define COUNTRY_COUNT 46
/* Number of selectable video-output aspect ratios. Order MUST match the
 * <select id="aspect"> options in web/index.html. Index -1 leaves the game's
 * own 16:9 projection constant untouched. */
#define ASPECT_COUNT 9
typedef struct {
    unsigned mask;
    int preset, units, language, country, speed, mass, power, torque, temperature, autostart, important_dismissed;
    int aspect;
} Config;
/* ISO alpha-2 code for a country index, or "" when i is out of range. */
const char *country_code(int i);
Config defaults(void);
int valid_config(const Config *c);
int parse_config(const char *body, Config *c);
int emit_xml(FILE *f, const Config *c);
int save_config(const char *root, const Config *c);
int load_config(const char *root, Config *c);
const char *feature_name(unsigned i);
const char *feature_note(unsigned i);
unsigned feature_count(unsigned i);
const char *page_html(void);
