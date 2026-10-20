/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdio.h>
typedef struct {
    unsigned mask;
    int preset, units, language, speed, mass, power, torque, temperature, autostart;
} Config;
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
