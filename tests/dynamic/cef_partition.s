    .text
    .globl _cef_partition
    .p2align 4
_cef_partition:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %rax
    leaq    0x10(%rdi), %rax
    cmpq    %rsi, %rax
    je      L_abort
    movq    (%rdi), %rcx
    movl    0x8(%rdi), %r8d
    movl    $0x10, %r9d
    jmp     L_1039
L_1022:
    jbe     L_1049
L_1024:
    leaq    (%rdi,%r9), %rax
    addq    $0x10, %rax
    addq    $0x10, %r9
    cmpq    %rsi, %rax
    je      L_abort
L_1039:
    movq    (%rdi,%r9), %rax
    cmpq    %rax, %rcx
    jne     L_1022
    cmpl    %r8d, 0x8(%rdi,%r9)
    jl      L_1024
L_1049:
    leaq    (%rdi,%r9), %rdx
    cmpq    $0x10, %r9
    je      L_1062
    cmpq    %rdi, %rsi
    je      L_abort
    leaq    -0x10(%rsi), %r9
    jmp     L_109d
L_1062:
    movq    %rsi, %r9
    cmpq    %rsi, %rdx
    jae     L_10ab
    leaq    -0x10(%rsi), %r9
    jmp     L_107b
L_1070:
    ja      L_10ab
L_1072:
    cmpq    %r9, %rdx
    jae     L_10ab
    addq    $-0x10, %r9
L_107b:
    movq    (%r9), %r10
    cmpq    %r10, %rcx
    jne     L_1070
    cmpl    %r8d, 0x8(%r9)
    jge     L_1072
    jmp     L_10ab
L_108b:
    ja      L_10ab
L_108d:
    leaq    -0x10(%r9), %r10
    cmpq    %rdi, %r9
    movq    %r10, %r9
    je      L_abort
L_109d:
    movq    (%r9), %r10
    cmpq    %r10, %rcx
    jne     L_108b
    cmpl    %r8d, 0x8(%r9)
    jge     L_108d
L_10ab:
    movq    %rdx, %r10
    cmpq    %r9, %rdx
    jae     L_10be
    movq    (%r9), %rbx
    movq    %rdx, %r10
    movq    %r9, %r11
    jmp     L_110b
L_10be:
    leaq    -0x10(%r10), %rax
    cmpq    %rdi, %rax
    je      L_10d5
    movq    -0x10(%r10), %rsi
    movq    %rsi, (%rdi)
    movl    -0x8(%r10), %esi
    movl    %esi, 0x8(%rdi)
L_10d5:
    cmpq    %r9, %rdx
    setae   %dl
    movq    %rcx, -0x10(%r10)
    movl    %r8d, -0x8(%r10)
    addq    $0x8, %rsp
    popq    %rbx
    popq    %rbp
    retq
L_10ea:
    ja      L_1106
L_10ec:
    leaq    -0x10(%r11), %rbx
    cmpq    %rdi, %r11
    movq    %rbx, %r11
    je      L_abort
L_10f8:
    movq    (%r11), %rbx
    cmpq    %rbx, %rcx
    jne     L_10ea
    cmpl    %r8d, 0x8(%r11)
    jge     L_10ec
L_1106:
    cmpq    %r11, %r10
    jae     L_10be
L_110b:
    movq    %rbx, (%r10)
    movq    %rax, (%r11)
    movl    0x8(%r10), %eax
    movl    0x8(%r11), %ebx
    movl    %ebx, 0x8(%r10)
    movl    %eax, 0x8(%r11)
    jmp     L_1125
L_1123:
    jbe     L_113c
L_1125:
    addq    $0x10, %r10
    cmpq    %rsi, %r10
    je      L_abort
    movq    (%r10), %rax
    cmpq    %rax, %rcx
    jne     L_1123
    cmpl    %r8d, 0x8(%r10)
    jl      L_1125
L_113c:
    cmpq    %rdi, %r11
    je      L_abort
    addq    $-0x10, %r11
    jmp     L_10f8
L_abort:
    xorl    %eax, %eax
    call    _cef_partition_abort
