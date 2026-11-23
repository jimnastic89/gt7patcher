/* SPDX-License-Identifier: GPL-3.0-only
 * Event prize hook for GT7 01.710.000. Layout and purpose: event_hook.h.
 * Linked at its RVA (event_hook.ld), so the jump back into the loader is a
 * fixed rel32 inside the image, and so are the log and the table (eh_log_entries
 * and eh_table, defined in event_hook.ld). Uses no stack beyond its own pushes and calls
 * nothing but the original loader. */
#include "event_hook.h"
.intel_syntax noprefix
.section .text
.global eh_header
eh_header:
    .ascii EVENT_HOOK_MAGIC
    .long EVENT_HOOK_ABI
    .long 0 /* table count */
    .long 0 /* log head */
    .long 0 /* loads */
    .long 0 /* changed */
    .org 0x40, 0

/* Entered by a jmp from the loader's first instruction.
 * rdi = GameParameter (out), rsi = store, edx = event id. Returns void. */
.global eh_entry
eh_entry:
    push rbx
    push r12
    push r13
    mov rbx, rdi
    mov r12d, edx
    call eh_original
    mov rdi, rbx
    mov esi, r12d
    call eh_apply
    pop r13
    pop r12
    pop rbx
    ret

/* The loader's displaced prologue (push rbp; mov rbp, rsp; push r15). */
eh_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp gt7_load_continue

/* rdi = GameParameter, esi = event id; rbx also holds the GameParameter (set
 * by eh_entry). Changes caller-saved registers only. */
eh_apply:
    lock inc dword ptr [rip + eh_header + EVENT_HOOK_OFF_LOADS]
    mov r8, qword ptr [rdi + 0x10]   /* EventParameter */
    test r8, r8
    jz .Ldone
    mov edi, esi                     /* id */
    mov r9, qword ptr [r8 + 0xd0]    /* prize table begin */
    mov r10, qword ptr [r8 + 0xd8]   /* end */
    xor r11d, r11d                   /* original 1st prize, 0 when empty */
    cmp r9, r10
    jae .Lfind
    mov r11d, dword ptr [r9]
.Lfind:
    xor esi, esi                     /* new 1st prize for the log, 0 = untouched */
    mov ecx, dword ptr [rip + eh_header + EVENT_HOOK_OFF_COUNT]
    cmp ecx, EVENT_HOOK_TABLE_MAX
    ja .Llog
    lea rax, [rip + eh_table]
.Lnext_entry:
    test ecx, ecx
    jz .Llog
    cmp dword ptr [rax], edi
    je .Lfound
    add rax, 8
    dec ecx
    jmp .Lnext_entry
.Lfound:
    mov esi, dword ptr [rax + 4]     /* target 1st prize */
    test r11d, r11d
    jle .Lskip                       /* nothing to scale against */
    test esi, esi
    jz .Lskip
    lock inc dword ptr [rip + eh_header + EVENT_HOOK_OFF_CHANGED]
.Lscale:
    cmp r9, r10
    jae .Llog
    mov eax, dword ptr [r9]
    test eax, eax
    jle .Lscaled                     /* leave zero and negative entries */
    imul rax, rsi                    /* prize * target, fits in 64 bits */
    xor edx, edx
    div r11                          /* / original 1st */
    add rax, 99
    mov ecx, 100
    xor edx, edx
    div rcx
    imul rax, rax, 100               /* rounded up to 100 Cr */
    mov ecx, EVENT_HOOK_PRIZE_MAX
    cmp rax, rcx
    cmova rax, rcx
    mov dword ptr [r9], eax
.Lscaled:
    add r9, 4
    jmp .Lscale
.Lskip:
    xor esi, esi
.Llog:
    mov eax, 1
    lock xadd dword ptr [rip + eh_header + EVENT_HOOK_OFF_HEAD], eax
    and eax, EVENT_LOG_COUNT - 1
    imul eax, eax, EVENT_LOG_ENTRY
    lea rcx, [rip + eh_log_entries]
    add rcx, rax
    mov dword ptr [rcx], edi
    mov dword ptr [rcx + 4], r11d
    mov dword ptr [rcx + 8], esi
    mov rax, r10
    sub rax, qword ptr [r8 + 0xd0]
    shr rax, 2
    mov dword ptr [rcx + 12], eax
    /* Track label: the std::string {data or inline chars, size, capacity} at
     * the start of *(GameParameter + 0x20); at most 31 characters kept. */
    lea r9, [rcx + 16]
    mov rax, qword ptr [rbx + 0x20]
    test rax, rax
    jz .Lterm
    mov rdx, qword ptr [rax + 0x10]
    cmp qword ptr [rax + 0x18], 15
    jbe .Lclamp
    mov rax, qword ptr [rax]
    test rax, rax
    jz .Lterm
.Lclamp:
    cmp rdx, EVENT_LOG_COURSE - 1
    jbe .Lcopy
    mov edx, EVENT_LOG_COURSE - 1
.Lcopy:
    test rdx, rdx
    jz .Lterm
    mov r10b, byte ptr [rax]
    mov byte ptr [r9], r10b
    inc rax
    inc r9
    dec rdx
    jmp .Lcopy
.Lterm:
    mov byte ptr [r9], 0
.Ldone:
    ret

    .org EVENT_HOOK_SIZE, 0
