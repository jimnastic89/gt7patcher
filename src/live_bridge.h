/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#define LIVE_MAGIC UINT64_C(0x3145474449523747)
#define LIVE_MAX_CREDITS 100000000U
enum {LIVE_IDLE, LIVE_QUEUED, LIVE_RUNNING, LIVE_DONE, LIVE_STALE, LIVE_INVALID, LIVE_CANCELLED};
enum {CAR_IDLE, CAR_QUEUED, CAR_RUNNING, CAR_READY, CAR_UNAVAILABLE};
enum {WHEEL_IDLE, WHEEL_QUEUED, WHEEL_RUNNING, WHEEL_DONE, WHEEL_STALE, WHEEL_INVALID, WHEEL_UNAVAILABLE};
enum {GEOMETRY_IDLE, GEOMETRY_QUEUED, GEOMETRY_RUNNING, GEOMETRY_DONE, GEOMETRY_STALE, GEOMETRY_INVALID, GEOMETRY_UNAVAILABLE};
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
} LiveMailbox;
extern volatile LiveMailbox live_mailbox;
void live_frame(void *context,float delta_time);
