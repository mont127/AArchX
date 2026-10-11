/*
 * Code that reaches code the guest then rewrites (src/jit.c,
 * ocerz_jit_invalidate_range): on one page a direct call, a jcc and a jmp, each
 * to a routine on another page; once all three have run 20,000 times the
 * routines are rewritten, twice, and the next calls must reach the new code.  The
 * call's callee is spliced into its caller's block, so the caller's block covers
 * code 16 KB past its own last instruction, which the rewrite has to find.  Run
 * on a page anywhere and on one asked for at 11 GB, in the Wine layout's low
 * window when it gives that.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* mov eax, imm; ret */
static void put_ret(uint8_t *p, uint32_t imm)
{
    p[0] = 0xb8; memcpy(p + 1, &imm, 4); p[5] = 0xc3;
}
static void put_rel32(uint8_t *at, int len, uint8_t *to)
{
    int32_t rel = (int32_t)(to - (at + len));
    memcpy(at + len - 4, &rel, 4);
}

/* The scenario on the 64 KB at page; 1 when every call reached the code written last. */
static int run_case(uint8_t *page)
{
    uint8_t *x = page, *y1 = page + 0x4000, *y2 = page + 0x8000, *y3 = page + 0xc000;
    /* x+0x00: call y1; add eax, 1; ret */
    x[0] = 0xe8; put_rel32(x, 5, y1); x[5] = 0x83; x[6] = 0xc0; x[7] = 0x01; x[8] = 0xc3;
    /* x+0x40: test edi, edi; jne y2 (rel32); mov eax, -1; ret */
    uint8_t *c = x + 0x40;
    c[0] = 0x85; c[1] = 0xff; c[2] = 0x0f; c[3] = 0x85; put_rel32(c + 2, 6, y2);
    c[8] = 0xb8; memset(c + 9, 0xff, 4); c[13] = 0xc3;
    /* x+0x80: add edi, 2; jmp y3 */
    uint8_t *j = x + 0x80;
    j[0] = 0x83; j[1] = 0xc7; j[2] = 0x02; j[3] = 0xe9; put_rel32(j + 3, 5, y3);
    put_ret(y1, 10); put_ret(y2, 20); put_ret(y3, 30);
    int (*fx)(void) = (int (*)(void))x;
    int (*fc)(int) = (int (*)(int))c;
    int (*fj)(int) = (int (*)(int))j;
    long sum = 0;
    for (int i = 0; i < 20000; i++) sum += fx() + fc(1) + fj(i);
    put_ret(y1, 11); put_ret(y2, 21); put_ret(y3, 31);
    int a = fx(), b = fc(1), d = fj(0);
    for (int i = 0; i < 20000; i++) sum += fx() + fc(1) + fj(i);
    put_ret(y1, 12); put_ret(y2, 22); put_ret(y3, 32);
    int e = fx(), f = fc(1), g = fj(0);
    if (sum == 20000L * (11 + 20 + 30) + 20000L * (12 + 21 + 31) && a == 12 && b == 21 && d == 31 &&
        e == 13 && f == 22 && g == 32)
        return 1;
    printf("FAIL at %p sum=%ld a=%d b=%d d=%d e=%d f=%d g=%d\n", (void *)page, sum, a, b, d, e, f, g);
    return 0;
}

int main(void)
{
    /* Anywhere, then at 11 GB, inside the Wine layout's low window when it gives that. */
    uint8_t *p1 = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *p2 = mmap((void *)0x2c0000000ull, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p1 == MAP_FAILED || p2 == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    if (run_case(p1) && run_case(p2)) printf("OK\n");
    return 0;
}
