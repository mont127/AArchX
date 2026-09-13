#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern uint64_t rz_probe(uint64_t pid);
__asm__(".text\n.p2align 4\n.globl _rz_probe\n_rz_probe:\n"
        " movabsq $0x1122334455667788, %rax\n"
        " movq $-8, %rcx\n"
        "1: movq %rax, (%rsp,%rcx)\n"
        " subq $8, %rcx\n"
        " cmpq $-128, %rcx\n"
        " jge 1b\n"
        " movl $30, %esi\n"
        " movl $0x2000025, %eax\n"
        " syscall\n"
        " movl $0x2000014, %eax\n"
        " syscall\n"
        " movabsq $0x1122334455667788, %rdx\n"
        " xorl %eax, %eax\n"
        " movq $-8, %rcx\n"
        "2: cmpq %rdx, (%rsp,%rcx)\n"
        " je 3f\n"
        " incq %rax\n"
        "3: subq $8, %rcx\n"
        " cmpq $-128, %rcx\n"
        " jge 2b\n"
        " ret\n");

static volatile int seen;

static void on_usr1(int sig)
{
    char buf[512];
    (void)sig;
    memset(buf, 0x99, sizeof buf);
    seen += buf[7] != 0;
}

int main(void)
{
    signal(SIGUSR1, on_usr1);
    uint64_t bad = rz_probe((uint64_t)getpid());
    if (seen != 1 || bad != 0) {
        printf("BAD handler ran %d times, %llu of 16 red-zone slots changed\n", seen, (unsigned long long)bad);
        return 1;
    }
    printf("OK\n");
    return 0;
}
