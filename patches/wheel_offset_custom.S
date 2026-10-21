/* SPDX-License-Identifier: GPL-3.0-only
 * Replacement for GT7 01.710.000 GetWheelOffsetPulldownInfo at 0x3896010.
 * Builds Standard and eleven car-relative pairs based on the native Wide value.
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
    push r14
    push r15
    sub rsp, 0xa8
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

    /* Ask GT7 for this car's signed front/rear Wide offsets. */
    mov word ptr [rbp - 0x4c], 0
    mov rdi, r12
    lea rsi, [rbp - 0x4c]
    mov eax, 0x304fef0
    call rax
    test al, al
    je .done
    movsx r14d, byte ptr [rbp - 0x4c]
    movsx r15d, byte ptr [rbp - 0x4b]

    /* Add Wide itself, then ten progressively wider pairs through Wide - 50. */
    xor r13d, r13d
.pair_loop:
    mov eax, r14d
    sub eax, r13d
    mov word ptr [rbp - 0x50], ax
    mov eax, r15d
    sub eax, r13d
    mov word ptr [rbp - 0x4e], ax

    /* Allocate the label exactly as GT7 does for strings longer than 15 bytes.
     * The vector inserter takes ownership of this buffer.
     */
    mov edi, 0x20
    mov eax, 0x430a300
    call rax
    mov qword ptr [rbp - 0xb8], rax
    mov rdi, rax
    mov dword ptr [rdi], 0x6e6f7246
    mov word ptr [rdi + 4], 0x2074
    add rdi, 6
    movsx eax, word ptr [rbp - 0x50]
    call .append_signed
    movabs rax, 0x2072616552202f20
    mov qword ptr [rdi], rax
    add rdi, 8
    movsx eax, word ptr [rbp - 0x4e]
    call .append_signed
    mov byte ptr [rdi], 0

    /* Transfer the allocated long string to the option vector. */
    lea rsi, [rbp - 0x70]
    mov rax, qword ptr [rbp - 0xb8]
    mov qword ptr [rsi], rax
    mov rcx, rdi
    sub rcx, rax
    mov qword ptr [rsi + 0x10], rcx
    mov qword ptr [rsi + 0x18], 31
    mov rdi, rbx
    lea rdx, [rbp - 0x50]
    mov eax, 0x3896350
    call rax

    add r13d, 5
    cmp r13d, 55
    jne .pair_loop

.done:
    add rsp, 0xa8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
/* Append signed EAX as decimal at RDI and return the advanced pointer in RDI. */
.append_signed:
    test eax, eax
    jns .magnitude
    mov byte ptr [rdi], '-'
    inc rdi
    neg eax
.magnitude:
    xor r8d, r8d
    mov ecx, 10
.digits:
    xor edx, edx
    div ecx
    push rdx
    inc r8d
    test eax, eax
    jne .digits
.emit:
    pop rdx
    add dl, '0'
    mov byte ptr [rdi], dl
    inc rdi
    dec r8d
    jne .emit
    ret
.size wheel_offset_options, .-wheel_offset_options
