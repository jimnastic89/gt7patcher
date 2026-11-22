/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#define WHEEL_RUNTIME_OFFSET 0x44000u
#define WHEEL_RUNTIME_RESERVED 0x8000u
#define WHEEL_RUNTIME_MAGIC UINT64_C(0x31544c5053573747)
typedef struct
{
    uint64_t magic, base, original, context, heartbeat;
    uint32_t enabled, wheel_clone_state, wheel_clone_reserved, reserved;
    uint64_t wheel_clone_source, wheel_clone_result, wheel_event_result;
} WheelRuntime;
_Static_assert(sizeof(WheelRuntime) == 80, "wheel runtime ABI");
