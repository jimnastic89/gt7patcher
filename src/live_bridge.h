/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#define LIVE_MAGIC UINT64_C(0x3145474449523747)
#define LIVE_MAX_CREDITS 100000000U
/* Executable padding shared with patched code caves. The live-settings
 * bridge writes its bootstrap into [LIVE_BOOT_RVA, LIVE_BOOT_RVA +
 * LIVE_BOOT_RESERVED); patched code caves must lie outside that range. */
#define LIVE_IMAGE_BASE 0x00400000u
#define LIVE_BOOT_RVA 0x03f0e940u
#define LIVE_BOOT_RESERVED 0x000000c8u
/* The live-settings allocation is mapped read/write/execute by the bootstrap
 * (see src/live_boot.S, size 0x20000). The bridge code occupies its start; the
 * credit number-input scratch lives at the offsets below. Keep LIVE_ALLOCATION
 * in step with the mmap size in live_boot.S. */
#define LIVE_ALLOCATION 0x00020000u
#define LIVE_PROMPT_PARAM 0x00010000u
#define LIVE_PROMPT_TEXT  0x00010200u
#define LIVE_PROMPT_WORK  0x00011000u
#define LIVE_PROMPT_WORK_SIZE 0x5000u
#define LIVE_PROMPT_TEXT_CHARS 32u
enum {PROMPT_IDLE,PROMPT_QUEUED,PROMPT_RUNNING,PROMPT_OPEN,PROMPT_ENTERED,PROMPT_APPLIED,PROMPT_CANCELLED,PROMPT_FAILED};
enum {LIVE_IDLE, LIVE_QUEUED, LIVE_RUNNING, LIVE_DONE, LIVE_STALE, LIVE_INVALID, LIVE_CANCELLED};
enum {CAR_IDLE, CAR_QUEUED, CAR_RUNNING, CAR_READY, CAR_UNAVAILABLE};
enum {WHEEL_IDLE, WHEEL_QUEUED, WHEEL_RUNNING, WHEEL_DONE, WHEEL_STALE, WHEEL_INVALID, WHEEL_UNAVAILABLE};
enum {GEOMETRY_IDLE, GEOMETRY_QUEUED, GEOMETRY_RUNNING, GEOMETRY_DONE, GEOMETRY_STALE, GEOMETRY_INVALID, GEOMETRY_UNAVAILABLE};
enum {ENGINE_IDLE, ENGINE_QUEUED, ENGINE_RUNNING, ENGINE_DONE, ENGINE_STALE, ENGINE_INVALID, ENGINE_UNAVAILABLE};
typedef struct {
    uint64_t magic, base, original;
    uint64_t make_event, acquire, destroy_event, set_credits;
    uint64_t heartbeat, context, expected_object;
    uint32_t state, enabled, expected_amount, amount, observed, sequence;
    uint64_t car_ctor, get_riding_car, car_dtor;
    uint32_t car_state, car_sequence, car_model_code;
    int8_t wheel_width_front, wheel_width_rear;
    uint8_t car_reserved[2];
    /* All routines below are version-locked GT7 code.  A wheel request is
     * claimed and executed by the game's UI thread, never by the payload. */
    uint64_t garage_root, garage_lock, garage_unlock, garage_riding_id;
    uint64_t garage_get_car, garage_set_car, set_wheel_width;
    uint32_t wheel_state, wheel_expected_sequence, wheel_expected_model_code;
    int8_t wheel_expected_front, wheel_expected_rear;
    int8_t wheel_requested_front, wheel_requested_rear;
    uint64_t set_wheel_inch_up, add_wheel_offset, set_hub_offset;
    uint32_t geometry_state, geometry_expected_model_code;
    int8_t wheel_inch_up;
    uint8_t geometry_reserved[3];
    int16_t wheel_offset_front, wheel_offset_rear, hub_offset_front, hub_offset_rear;
    int8_t geometry_expected_inch_up, geometry_requested_inch_up;
    int16_t geometry_expected_offset_front, geometry_expected_offset_rear, geometry_expected_hub_front, geometry_expected_hub_rear;
    int16_t geometry_requested_offset_front, geometry_requested_offset_rear, geometry_requested_hub_front, geometry_requested_hub_rear;
    uint64_t get_engine_swap, add_engine_swap, set_engine_swap, reset_engine_swap;
    uint32_t engine_state, engine_expected_model_code;
    uint8_t engine_swap_category, engine_expected_category, engine_requested_category, engine_unlock;
    /* The IME hook is installed through GT7's imported sceImeOpen pointer.
     * A non-zero pending flag changes only the next opening to number mode. */
    uint64_t ime_original, ime_close, ime_update;
    uint32_t credit_input_pending, ime_hook_enabled, ime_user_id;
    /* Credit number-input prompt. The payload queues PROMPT_QUEUED; the game
     * UI thread opens GT7's own sceImeOpen in Number mode, the platform
     * returns the digits through the bridge handler, and the value is applied
     * through the same native credit path as the live settings. */
    uint32_t prompt_state, prompt_result, prompt_open, prompt_chars, prompt_amount, prompt_reserved;
    /* Runtime address of this allocation, written by the payload. The bridge
     * needs it to pass absolute pointers to sceImeOpen. */
    uint64_t allocation;
} LiveMailbox;
extern volatile LiveMailbox live_mailbox;
void live_frame(void *context,float delta_time);
