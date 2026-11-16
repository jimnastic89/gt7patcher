/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "live_bridge.h"
#ifdef GT7_DIAGNOSTICS
#include "engine_diagnostics.h"
#endif
typedef struct {int pid;uint64_t object;uint32_t amount;} Gt7Credits;
typedef struct {int pid;uint32_t model_code,sequence;int8_t front,rear;} Gt7WheelWidth;
typedef struct {int pid;uint32_t model_code;int8_t inch_up;int16_t offset_front,offset_rear,hub_front,hub_rear;} Gt7WheelGeometry;
typedef struct {int pid;uint32_t model_code;uint8_t category;} Gt7EngineSwap;
typedef struct {
    int pid;
    uint32_t normal_multiplier, fast_multiplier, vertical_multiplier;
} Gt7FreeCamera;
/* Physics rev limiter: the u16 maximum-RPM field of the active car's engine
 * spec object. `increase` is the user's requested RPM bump on top of the
 * car's native limit (0 = none); `native_rpm` is that native limit and
 * `max_rpm` is what the spec field currently holds. `applied` is non-zero
 * while an increase is in force. */
typedef struct {int pid;uint16_t max_rpm,idle_rpm,red_rpm,native_rpm,increase;uint8_t applied;uint64_t dial_addr;uint16_t dial_scale,dial_red;} Gt7RevLimit;
typedef struct {int pid;uint32_t state,result,amount,chars,dbg_got,dbg_end,dbg_status,dbg_user;} Gt7CreditPrompt;
/* HUD visibility: the set of MENU widget keys (paths, or bare names) the
 * bridge is hiding. The field order mirrors the mailbox tail. */
typedef struct {int pid;uint32_t count,sequence,applied,reserved;char entries[LIVE_HUD_MAX][LIVE_HUD_PATH];} Gt7Hud;
int gt7_credits_read(Gt7Credits *out);
int gt7_credits_set(const Gt7Credits *expected,uint32_t amount);
int gt7_wheel_width_read(Gt7WheelWidth *out);
int gt7_wheel_width_set(const Gt7WheelWidth *expected,int8_t front,int8_t rear);
int gt7_wheel_geometry_read(Gt7WheelGeometry *out);
int gt7_wheel_geometry_set(const Gt7WheelGeometry *expected,int8_t inch_up,int16_t offset_front,int16_t offset_rear,int16_t hub_front,int16_t hub_rear);
int gt7_engine_swap_read(Gt7EngineSwap *out);
int gt7_hud_read(Gt7Hud *out);
int gt7_hud_set(const char (*entries)[LIVE_HUD_PATH],uint32_t count);
int gt7_engine_swap_set(const Gt7EngineSwap *expected,uint8_t category,int unlock);
/* Read the active car's rev limiter and set an RPM increase. Only the limiter
 * field (spec+0x298) is ever written; live engine RPM is never modified. */
int gt7_rev_limit_read(Gt7RevLimit *out);
int gt7_rev_limit_set(const Gt7RevLimit *expected,uint16_t increase);
/* Re-apply a stored increase after a car change; safe to call every tick. */
int gt7_rev_limit_tick(void);
int gt7_free_camera_read(Gt7FreeCamera *out);
int gt7_free_camera_set(const Gt7FreeCamera *expected, uint32_t normal_multiplier,
                       uint32_t fast_multiplier, uint32_t vertical_multiplier);
int gt7_live_ready(void);
int gt7_live_arm_credit_input(void);
int gt7_live_credit_prompt_begin(void);
int gt7_live_credit_auto_poll(void);
int gt7_live_credit_prompt_read(Gt7CreditPrompt *out);
/* Diagnostic pad reader. Historic notes wrongly claimed this opened the credit
 * number pad; it never worked on hardware and is not a credit trigger. It only
 * logs pad state while the live bridge is enabled. */
void gt7_live_pad_poll(void);
void gt7_live_pad_configure(int enabled);
int gt7_live_menu_sync(int expected_pid, const char labels[][LIVE_MENU_LABEL],
                       const uint8_t *hidden, uint32_t count, uint32_t *acknowledged,
                       uint8_t events[LIVE_MENU_EVENTS], uint32_t *sequence);
int gt7_live_menu_active(void);
int gt7_live_menu_acknowledge(int expected_pid, uint32_t sequence,
                              const uint8_t *hidden, uint32_t count);
void gt7_live_configure(int enabled);
int gt7_engine_start(void);
int gt7_engine_active(void);
int gt7_engine_ready(void);
/* Last request received through the ShellCore launch hook.  This is kept in
 * the patch engine rather than relying on a notification being visible. */
void gt7_engine_launch_status(unsigned *requests,int *pid,const char **stage);
int gt7_engine_stop(void);
int gt7_install_icon(void);
int gt7_launch_game(void);
