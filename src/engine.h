/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
typedef struct {int pid;uint64_t object;uint32_t amount;} Gt7Credits;
typedef struct {int pid;uint32_t model_code,sequence;int8_t front,rear;} Gt7WheelWidth;
typedef struct {int pid;uint32_t model_code;int8_t inch_up;int16_t offset_front,offset_rear,hub_front,hub_rear;} Gt7WheelGeometry;
int gt7_credits_read(Gt7Credits *out);
int gt7_credits_set(const Gt7Credits *expected,uint32_t amount);
int gt7_wheel_width_read(Gt7WheelWidth *out);
int gt7_wheel_width_set(const Gt7WheelWidth *expected,int8_t front,int8_t rear);
int gt7_wheel_geometry_read(Gt7WheelGeometry *out);
int gt7_wheel_geometry_set(const Gt7WheelGeometry *expected,int8_t inch_up,int16_t offset_front,int16_t offset_rear,int16_t hub_front,int16_t hub_rear);
int gt7_live_ready(void);
void gt7_live_configure(int enabled);
int gt7_engine_start(void);
int gt7_engine_active(void);
int gt7_engine_ready(void);
int gt7_engine_stop(void);
int gt7_install_icon(void);
int gt7_launch_game(void);
