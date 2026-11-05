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
/* In-race HUD visibility control. The payload publishes a list of MENU widget
 * keys to hide; the bridge walks the live widget tree every frame and zeroes
 * the layout rect (node+0x238, 32 bytes) of any widget whose path or name
 * matches, restoring a cached rect when a key leaves the list. Paths are used
 * rather than addresses because the tree is rebuilt at every race start and
 * process restart. */
#define LIVE_HUD_MAX 64
#define LIVE_HUD_NAME 32
#define LIVE_HUD_PATH 96
/* One-shot UI diagnostic: the payload calls a single engine routine on the
 * game's UI thread as a VM native, following the engine's binding convention:
 * rsi = receiver object, edx = argument count, rcx = argument vector pointer
 * (arg[i] = *(uint64_t *)(rcx + 8*i)); rdi is unused. argc must equal the
 * routine's own expected count or the routine rejects the call. */
#define LIVE_MENU_CALL_MAX 6
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
    /* sceImeOpen's on-screen panel draws only the keyboard (the game
     * renders the text itself), so it looks like a bare number pad. The
     * system dialog opened through sceImeDialogInit carries its own text
     * entry field; the payload resolves those imports from the game's
     * jump table. */
    uint64_t ime_dialog_init, ime_dialog_status, ime_dialog_result, ime_dialog_term;
    uint32_t credit_input_pending, ime_hook_enabled, ime_user_id;
    /* Credit number-input prompt. The payload queues PROMPT_QUEUED; the game
     * UI thread opens the system number dialog (with a text entry field),
     * reads the accepted digits from inputTextBuffer, and applies them
     * through the same native credit path as the live settings. */
    uint32_t prompt_state, prompt_result, prompt_open, prompt_chars, prompt_amount, prompt_reserved;
    /* Read-only diagnostic for the credit prompt: the sceImeDialogGetResult
     * return value (prompt_reserved) and the dialog end status. */
    uint32_t prompt_dbg_end;
    /* Read-only diagnostic: last sceImeDialogGetStatus value observed while the
     * prompt was open (to confirm the status enum on the console). */
    uint32_t prompt_dbg_status;
    /* Runtime address of this allocation, written by the payload. The bridge
     * needs it to pass absolute pointers to the dialog parameter. */
    uint64_t allocation;
    /* Read-only diagnostic: live object-graph pointers sampled for the riding
     * car so the payload can dump the car's memory for analysis. Zeroed when
     * no car is available. */
    uint64_t car_object, car_implementation, car_expanded, car_style;
    /* HUD visibility: the payload writes hud_entries/hud_count; the bridge
     * zeroes the layout rect of matching widgets and reports hud_applied (the
     * number of distinct keys it is currently hiding) and hud_sequence (the
     * generation of the list it last applied). An entry containing '/' is a
     * widget path (the chain of named ancestors); an entry without one is a
     * bare widget name and matches every widget with that name. */
    uint32_t hud_count, hud_sequence, hud_applied;
    /* Credits popup auto-trigger (bridge -> payload). The bridge sets this to 1
     * the frame it first sees the game's "GTCredit" widget appear (the user
     * pressed the status-bar Credits button). The payload's idle tick consumes
     * it and arms the credit number pad via gt7_live_credit_prompt_begin(). */
    uint32_t credit_request;
    char hud_entries[LIVE_HUD_MAX][LIVE_HUD_PATH];
    /* One-shot UI diagnostic (engine -> payload). */
    uint32_t menu_call_state, menu_call_result, menu_call_argc, menu_call_reserved;
    /* menu_call_result holds MENU_CALL_ENTERED while the routine runs, so a
     * fault inside it can be told apart from a call never dispatched. */
    uint64_t menu_call_target, menu_call_receiver;
    uint64_t menu_call_argv[LIVE_MENU_CALL_MAX];
} LiveMailbox;
/* The mailbox is defined file-local in live_bridge.c. It must NOT be a global
 * symbol: the PS5 clang toolchain marks every global GLOBAL DEFAULT (ignoring
 * -fvisibility=hidden), which forces a GOT reference; GNU ld's non-PIE
 * relaxation then rewrites `add sym@GOTPCREL(%rip),%reg` into an *absolute*
 * address (the link-time VMA), which is garbage once the blob is loaded at a
 * high address. Static objects get direct PC-relative references instead. The
 * payload locates the mailbox by its offset within the blob (LIVE_MAILBOX). */
void live_frame(void *context,float delta_time);
