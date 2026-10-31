/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdio.h>
#define PATCH_COUNT 8
#define LIVE_SETTINGS_BIT (1U << 6)
#define OPENING_MOVIE_SKIP_BIT (1U << 7)
/* Number of selectable player countries exposed by the payload UI. The order
 * of the ISO alpha-2 table in config.c MUST match the <select> options in
 * web/index.html. Index -1 means "leave the game's own value alone". */
#define COUNTRY_COUNT 46
typedef struct {
    unsigned mask;
    int preset, units, language, country, speed, mass, power, torque, temperature, autostart;
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
