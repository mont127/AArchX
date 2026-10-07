/*
 * 32-bit returns and indirect calls into code that changes between visits.
 * A translator that sends ret, call reg and call [mem] through a per-site cache
 * must not hand back a translation of bytes that have since been rewritten.
 * Nothing here calls FlushInstructionCache: x86 needs no flush, so Windows code
 * that skips it is correct and has to keep working.
 *
 *   i686-w64-mingw32-gcc -O2 -o smc_ret32.exe smc_ret32.c
 *   wine smc_ret32.exe [iterations, default 20000]
 *
 * ret target: a stub calls a C function that rewrites the instruction the call
 *   returns to, so the rewrite happens while the stub's frame is live and the
 *   ret lands on the new bytes.  The new bytes alternate between two
 *   instruction shapes of different lengths.
 * call target: C calls a stub through a pointer and rewrites its immediate
 *   before every call.
 * Each prints its sum and "ok" or "BAD"; exit 0 when all are ok.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *page;
static unsigned g_next;

/* Rewrites the bytes at page+5, the return address of the call in the stub. */
static void __cdecl rewrite_ret_target(void)
{
    unsigned char *l = page + 5;
    if (g_next & 1) {
        l[0] = 0xb8; memcpy(l + 1, &g_next, 4); l[5] = 0xc3;    /* mov eax, imm32; ret */
    } else {
        l[0] = 0x31; l[1] = 0xc0;                                /* xor eax, eax */
        l[2] = 0x05; memcpy(l + 3, &g_next, 4);                  /* add eax, imm32 */
        l[7] = 0xc3;                                             /* ret */
    }
}

int main(int argc, char **argv)
{
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    int bad = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    page = VirtualAlloc(NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) {
        printf("VirtualAlloc failed\n");
        return 1;
    }

    /* page+0: call rewrite_ret_target; page+5: rewritten each time, then ret. */
    int32_t rel = (int32_t)((uintptr_t)rewrite_ret_target - (uintptr_t)(page + 5));
    page[0] = 0xe8; memcpy(page + 1, &rel, 4);
    g_next = 1;
    rewrite_ret_target();
    unsigned (*stub)(void) = (unsigned (*)(void))page;
    unsigned long long sum = 0, want = 0;
    for (unsigned i = 0; i < iters; i++) {
        g_next = i * 2654435761u;
        want += g_next;
        sum += stub();
    }
    printf("ret target  %llu %s\n", sum, sum == want ? "ok" : "BAD");
    bad |= sum != want;

    /* page+0x1000: mov eax, imm32; ret, called through a pointer, imm rewritten each time. */
    unsigned char *q = page + 0x1000;
    q[0] = 0xb8; q[5] = 0xc3;
    unsigned (*volatile fp)(void) = (unsigned (*)(void))q;
    sum = want = 0;
    for (unsigned i = 0; i < iters; i++) {
        unsigned v = i ^ 0x5a5a5a5au;
        memcpy(q + 1, &v, 4);
        want += v;
        sum += fp();
    }
    printf("call target %llu %s\n", sum, sum == want ? "ok" : "BAD");
    bad |= sum != want;
    return bad;
}
