/* SPDX-License-Identifier: GPL-3.0-only
 * Runs once in MENU::mUpdateContextPS4's frame callback, then always forwards
 * to the original callback. No debugger attach or instruction detour. */
.intel_syntax noprefix
.section .text
.global live_boot_entry
live_boot_entry:
    push rdi
    sub rsp, 16
    movss dword ptr [rsp], xmm0
    lock inc qword ptr [rip + live_boot_control + 32]
    cmp dword ptr [rip + live_boot_control + 40], 3
    jae .bridge
    xor eax, eax
    mov ecx, 1
    lock cmpxchg dword ptr [rip + live_boot_control + 40], ecx
    jne .forward
    xor edi, edi
    mov esi, 0x20000
    mov edx, 7
    mov ecx, 0x1002
    mov r8, -1
    xor r9d, r9d
    call qword ptr [rip + live_boot_control + 16]
    mov qword ptr [rip + live_boot_control + 24], rax
    mov dword ptr [rip + live_boot_control + 40], 2
.forward:
    movss xmm0, dword ptr [rsp]
    add rsp, 16
    pop rdi
    jmp qword ptr [rip + live_boot_control + 8]
.bridge:
    mov rax, qword ptr [rip + live_boot_control + 48]
    mov dword ptr [rip + live_boot_control + 40], 4
    movss xmm0, dword ptr [rsp]
    add rsp, 16
    pop rdi
    jmp rax
.section .data
.balign 8
.global live_boot_control
live_boot_control:
    .quad 0x31544f4f424c3747 /* G7LBOOT1 */
    .quad 0 /* original frame callback */
    .quad 0 /* mmap from the game's libkernel */
    .quad 0 /* allocated mapping */
    .quad 0 /* heartbeat */
    .long 0 /* 0=new, 1=allocating, 2=mapped, 3=published, 4=entered */
    .long 0
    .quad 0 /* published bridge entry; read only after state 3 */
