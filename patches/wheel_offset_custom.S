/* SPDX-License-Identifier: GPL-3.0-only
 * Replacement for GT7 01.710.000 GetWheelOffsetPulldownInfo at 0x3896010.
 * Builds Standard, Wide and absolute -5 through -50 mm presets.
 */
.intel_syntax noprefix
.section .text
.global wheel_offset_options
.type wheel_offset_options,@function
wheel_offset_options:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    sub rsp, 0x68
    mov r12, rsi
    lea rbx, [rdi + 8]
    xor eax, eax
    mov dword ptr [rdi], eax
    pxor xmm0, xmm0
    movups xmmword ptr [rbx], xmm0
    mov qword ptr [rbx + 0x10], rax

    /* std::string("Standard"), value {0, 0}. */
    lea rsi, [rbp - 0x70]
    movups xmmword ptr [rsi], xmm0
    movups xmmword ptr [rsi + 0x10], xmm0
    movabs rax, 0x647261646e617453
    mov qword ptr [rsi], rax
    mov qword ptr [rsi + 0x10], 8
    mov qword ptr [rsi + 0x18], 15
    mov dword ptr [rbp - 0x50], 0
    mov rdi, rbx
    lea rdx, [rbp - 0x50]
    mov eax, 0x3896350
    call rax

    /* Ask GT7 for this car's front/rear Wide offsets. */
    mov word ptr [rbp - 0x4c], 0
    mov rdi, r12
    lea rsi, [rbp - 0x4c]
    mov eax, 0x304fef0
    call rax
    test al, al
    je .presets
    movsx eax, byte ptr [rbp - 0x4c]
    mov word ptr [rbp - 0x50], ax
    movsx eax, byte ptr [rbp - 0x4b]
    mov word ptr [rbp - 0x4e], ax

    /* std::string("Wide"), the stock generated pair. */
    lea rsi, [rbp - 0x70]
    pxor xmm0, xmm0
    movups xmmword ptr [rsi], xmm0
    movups xmmword ptr [rsi + 0x10], xmm0
    mov dword ptr [rsi], 0x65646957
    mov qword ptr [rsi + 0x10], 4
    mov qword ptr [rsi + 0x18], 15
    mov rdi, rbx
    lea rdx, [rbp - 0x50]
    mov eax, 0x3896350
    call rax

.presets:
    mov r13d, 5
.preset_loop:
    mov eax, r13d
    neg eax
    mov word ptr [rbp - 0x50], ax
    mov word ptr [rbp - 0x4e], ax

    /* Build "Offset -5mm" through "Offset -50mm" in a small string. */
    lea rsi, [rbp - 0x70]
    pxor xmm0, xmm0
    movups xmmword ptr [rsi], xmm0
    movups xmmword ptr [rsi + 0x10], xmm0
    movabs rax, 0x2d2074657366664f
    mov qword ptr [rsi], rax
    cmp r13d, 10
    jl .one_digit
    mov eax, r13d
    xor edx, edx
    mov ecx, 10
    div ecx
    add al, '0'
    add dl, '0'
    mov byte ptr [rsi + 8], al
    mov byte ptr [rsi + 9], dl
    mov word ptr [rsi + 10], 0x6d6d
    mov qword ptr [rsi + 0x10], 12
    jmp .append_preset
.one_digit:
    mov eax, r13d
    add al, '0'
    mov byte ptr [rsi + 8], al
    mov word ptr [rsi + 9], 0x6d6d
    mov qword ptr [rsi + 0x10], 11
.append_preset:
    mov qword ptr [rsi + 0x18], 15
    mov rdi, rbx
    lea rdx, [rbp - 0x50]
    mov eax, 0x3896350
    call rax
    add r13d, 5
    cmp r13d, 55
    jne .preset_loop
.done:
    add rsp, 0x68
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.size wheel_offset_options, .-wheel_offset_options
