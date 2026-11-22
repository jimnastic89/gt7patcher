/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#define PATCH_COUNT 12
#define WHEEL_OFFSET_CUSTOM_BIT (1U << 5)
#define LIVE_SETTINGS_BIT (1U << 6)
#define LIVE_SETTINGS_RELEASE_ENABLED 1
#define OPENING_MOVIE_SKIP_BIT (1U << 7)
/* Hidden "Debug environment" build-string patch. Locates the internal
 * "release" string and rewrites it to "debug"; see the debug-environment
 * special operation in the launch plan. */
#define DEBUG_ENVIRONMENT_BIT (1U << 8)
/* Sync Game Clock to System. Not a byte patch: the engine resolves libSceRtc in the
 * live game at launch and retargets the clock source of sceRtcGetCurrentTick
 * (see the clock-fix special operation in the launch plan). It is applied only
 * when the user selects it. */
#define CLOCK_FIX_BIT (1U << 9)
/* Silence Short Music Jingles. Byte patch; see patches/music_jingle_filter.S. */
#define MUSIC_JINGLE_FILTER_BIT (1U << 10)
/* Free Photo Mode Camera. Byte patch; see patches/free_photo_camera.S. */
#define FREE_PHOTO_CAMERA_BIT (1U << 11)
/* Number of selectable player countries exposed by the payload UI. The order
 * of the ISO alpha-2 table in config.c MUST match the <select> options in
 * web/index.html. Index -1 means "leave the game's own value alone". */
#define COUNTRY_COUNT 46
/* Number of selectable video-output aspect ratios. Order MUST match the
 * <select id="aspect"> options in web/index.html. Index -1 leaves the game's
 * own 16:9 projection constant untouched. */
#define ASPECT_COUNT 9
typedef struct
{
    unsigned mask;
    int preset, units, language, country, speed, mass, power, torque, temperature, autostart, important_dismissed;
    int aspect;
} Config;
typedef struct
{
    uintptr_t address;
    const unsigned char *replacement;
    size_t length;
} PatchDefinition;
typedef enum
{
    PATCH_OPERATION_BYTES,
    PATCH_OPERATION_DEBUG_ENVIRONMENT,
    PATCH_OPERATION_CLOCK_FIX,
    PATCH_OPERATION_COUNTRY,
    PATCH_OPERATION_ASPECT
} PatchOperationType;
typedef struct
{
    PatchOperationType type;
    uintptr_t address;
    const unsigned char *expected;
    const unsigned char *replacement;
    size_t length;
    unsigned feature;
    const char *name;
} PatchOperation;
#define PATCH_PLAN_MAX_OPERATIONS 1024
typedef struct
{
    PatchOperation operations[PATCH_PLAN_MAX_OPERATIONS];
    size_t count;
    int live_bridge, wheel_runtime;
    unsigned char generated[1024];
    size_t generated_used;
} PatchPlan;
/* Internal ISO alpha-3 code for a country index, or "" when i is out of range. */
const char *country_code(int i);
Config defaults(void);
int valid_config(const Config *c);
int parse_config(const char *body, Config *c);
int build_patch_plan(const Config *c, PatchPlan *plan);
int save_config(const char *root, const Config *c);
int load_config(const char *root, Config *c);
int initialize_config(const char *root, Config *c);
const char *feature_name(unsigned i);
const char *feature_note(unsigned i);
const char *feature_source(unsigned i);
unsigned feature_count(unsigned i);
const char *page_html(void);
