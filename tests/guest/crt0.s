# tests/guest/crt0.s
#
# x86_64 process entry stub for the Ocerz guest test programs (AT&T syntax).
#
# These tests are static LC_UNIXTHREAD Mach-O binaries built with
# clang -nostdlib -static -Wl,-e,_start, so there is no dyld and no libc
# crt: _start is the first guest instruction the kernel (real XNU under
# Rosetta, or Ocerz's loader) jumps to. The ABI contract at _start is the
# XNU exec stack layout: rsp points at argc, immediately followed by the
# argv pointer vector (argc entries, NULL-terminated), then the envp vector
# (NULL-terminated), then apple[]. rsp is 16-byte aligned at entry.
#
# This stub reproduces the standard SysV-ABI C runtime prologue:
#   rdi = argc        = *(rsp)
#   rsi = argv        = rsp + 8
#   rdx = envp        = argv + 8*(argc+1)   (skip argc slots + the NULL)
# then forces rsp to a 16-byte boundary with andq $-16 (it already is at
# entry, but the AND makes the alignment explicit and survives any future
# pre-call pushes) so that after the call instruction pushes the 8-byte
# return address the callee sees rsp%16==8, exactly as the ABI requires at
# a function's first instruction. main's return value (in eax) is forwarded
# to the BSD exit syscall 0x2000001; the trailing hlt is unreachable but
# guarantees control never falls off the end into garbage.

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
