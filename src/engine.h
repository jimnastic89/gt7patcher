/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "live_bridge.h"
typedef struct {int pid;uint64_t object;uint32_t amount;} Gt7Credits;
typedef struct {int pid;uint32_t model_code,sequence;int8_t front,rear;} Gt7WheelWidth;
typedef struct {int pid;uint32_t model_code;int8_t inch_up;int16_t offset_front,offset_rear,hub_front,hub_rear;} Gt7WheelGeometry;
typedef struct {int pid;uint32_t model_code;uint8_t category;} Gt7EngineSwap;
typedef struct {int pid;uint32_t state,result,amount,chars,dbg_got,dbg_end,dbg_status,dbg_user;} Gt7CreditPrompt;
/* HUD visibility: the set of MENU widget keys (paths, or bare names) the
 * bridge is hiding. The field order mirrors the mailbox tail. */
typedef struct {int pid;uint32_t count,sequence,applied,reserved;char entries[LIVE_HUD_MAX][LIVE_HUD_PATH];} Gt7Hud;
/* One named widget in the live tree, as published to the web UI. `depth` is the
 * indentation level and `hidden` reports whether its layout rect is zeroed.
 * `alt` is the path an older reader would have built (empty when it matches
 * `path`): it keeps a group saved before heap-allocated widget names were
 * decoded from losing sight of its widgets. */
typedef struct {
    char path[LIVE_HUD_PATH];char alt[LIVE_HUD_PATH];unsigned depth;
    uintptr_t address,vtable,children;
    float rect[8];int hidden;
} Gt7HudNode;
int gt7_credits_read(Gt7Credits *out);
int gt7_credits_set(const Gt7Credits *expected,uint32_t amount);
int gt7_wheel_width_read(Gt7WheelWidth *out);
int gt7_wheel_width_set(const Gt7WheelWidth *expected,int8_t front,int8_t rear);
int gt7_wheel_geometry_read(Gt7WheelGeometry *out);
int gt7_wheel_geometry_set(const Gt7WheelGeometry *expected,int8_t inch_up,int16_t offset_front,int16_t offset_rear,int16_t hub_front,int16_t hub_rear);
int gt7_engine_swap_read(Gt7EngineSwap *out);
int gt7_hud_read(Gt7Hud *out);
int gt7_hud_set(const char (*entries)[LIVE_HUD_PATH],uint32_t count);
/* Walk the live MENU widget tree and hand every named widget to emit (return
 * non-zero from emit to stop early). Paths are built exactly as the bridge
 * builds them, so a path selected in the UI matches at runtime. */
int gt7_hud_tree(int (*emit)(void *context,const Gt7HudNode *node),void *context);
int gt7_engine_swap_set(const Gt7EngineSwap *expected,uint8_t category,int unlock);
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
/* Temporary read-only diagnostic used to map GT7 state from inside the game
 * process (no debugger attach). Remove before release. */
int gt7_live_peek(uintptr_t address,void *buffer,uint32_t length);
/* Read-only diagnostic: live object-graph pointers sampled for the riding car. */
int gt7_live_car_pointers(uintptr_t *implementation,uintptr_t *expanded,uintptr_t *style,uint32_t *model_code,uint32_t *sequence);
/* Read-only diagnostic: snapshot a memory region in the payload, then report
 * only the byte runs that changed. Remove before release. */
int gt7_live_snapshot(uintptr_t address,unsigned length);
void gt7_live_snapshot_clear(void);
int gt7_live_snapshot_diff(char *out,unsigned size,unsigned *changed_bytes,unsigned *changed_runs);
int gt7_live_mapinfo(char *out,uint32_t size);
int gt7_live_find(uintptr_t address,uint32_t length,const char *needle,uint32_t needle_length,uintptr_t *hits,uint32_t max_hits,uint32_t *out_count);
int gt7_live_probe_install(uintptr_t target,uint32_t displaced,int mode,uintptr_t watch);
int gt7_live_probe_remove(void);
int gt7_live_probe_read(uintptr_t *words,uint32_t count);
int gt7_live_probe_history(uintptr_t *words,uint32_t count,uint32_t *next);
int gt7_live_probe_install_slot(uintptr_t target,uint32_t displaced,int mode,uintptr_t watch,uint32_t slot,uint64_t cfg);
int gt7_live_probe_remove_slot(uint32_t slot);
int gt7_live_trace_init(void);
int gt7_live_trace_report(char *out,uint32_t size,uint64_t *since,uint32_t max);
int gt7_live_hooks_auto(void);
int gt7_live_hooks_list(char *out,uint32_t size);
int gt7_live_menu_call(uintptr_t target,uintptr_t receiver,unsigned argc,uintptr_t args,unsigned *result);
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
