/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdint.h>

int gt7_live_wheel_clone(uintptr_t source, unsigned phase, uintptr_t *copy, unsigned *state);

/* Optional live-memory inspection and instrumentation API. Release builds do
 * not include this header or compile the corresponding implementation. */
int gt7_live_peek(uintptr_t address, void *buffer, uint32_t length);
int gt7_live_car_pointers(uintptr_t *implementation, uintptr_t *expanded,
                          uintptr_t *style, uint32_t *model_code,
                          uint32_t *sequence);
int gt7_live_snapshot(uintptr_t address, unsigned length);
void gt7_live_snapshot_clear(void);
int gt7_live_snapshot_diff(char *out, unsigned size, unsigned *changed_bytes,
                           unsigned *changed_runs);
int gt7_live_mapinfo(char *out, uint32_t size);
int gt7_live_find(uintptr_t address, uint32_t length, const char *needle,
                  uint32_t needle_length, uintptr_t *hits,
                  uint32_t max_hits, uint32_t *out_count);
int gt7_live_probe_install(uintptr_t target, uint32_t displaced, int mode,
                           uintptr_t watch);
int gt7_live_probe_remove(void);
int gt7_live_probe_read(uintptr_t *words, uint32_t count);
int gt7_live_probe_history(uintptr_t *words, uint32_t count, uint32_t *next);
int gt7_live_probe_install_slot(uintptr_t target, uint32_t displaced,
                                int mode, uintptr_t watch, uint32_t slot,
                                uint64_t cfg);
int gt7_live_probe_remove_slot(uint32_t slot);
int gt7_live_trace_init(void);
int gt7_live_trace_report(char *out, uint32_t size, uint64_t *since,
                          uint32_t max);
int gt7_live_hooks_auto(void);
int gt7_live_hooks_list(char *out, uint32_t size);
int gt7_live_menu_call(uintptr_t target, uintptr_t receiver, unsigned argc,
                       uintptr_t args, unsigned *result);

#ifdef GT7_LIVE_TEST
int gt7_live_test_attach(void);
#endif
