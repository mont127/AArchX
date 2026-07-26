# x86_64 entry stub for the static guest tests: unpack the XNU exec stack into the SysV ABI, call main, exit.

.text
.globl _start
_start:
    movq    (%rsp), %rdi
    leaq    8(%rsp), %rsi
    leaq    8(%rsi,%rdi,8), %rdx
    andq    $-16, %rsp
    callq   _main
    movl    %eax, %edi
    movl    $0x2000001, %eax
    syscall
    hlt
