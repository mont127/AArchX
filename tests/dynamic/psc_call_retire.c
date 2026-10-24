/*
 * An indirect call's per-site cache across rewrites of what it calls
 * (src/jit.c, psc_cols): one call site in this file's loop reaches eight
 * functions on a page of their own through a table of pointers, at offsets
 * whose bits 7..11 are not zero, so the column a call site files each under
 * (bits 2..6 of target ^ target >> 5) is not the one a jmp site would.  Each
 * round rewrites every function on the page, which retires their blocks;
 * the next calls must reach the new code, not a cached body of the old.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define HOST_PAGE 0x4000u

static void put_ret_imm(uint8_t *p, uint32_t imm)
{
    p[0] = 0xb8; memcpy(p + 1, &imm, 4);
    p[5] = 0xc3;
}

int main(void)
{
    uint8_t *raw = mmap(NULL, 3 * HOST_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1));
    static const uint32_t off[8] = { 0x180, 0x2a0, 0x3c0, 0x4e0, 0x5f0, 0x710, 0x830, 0x950 };
    uint32_t (*fn[8])(void);
    for (int k = 0; k < 8; k++)
        fn[k] = (uint32_t (*)(void))(void *)(p + off[k]);
    unsigned bad = 0;
    volatile uint32_t sink = 0;
    for (uint32_t round = 1; round <= 30; round++) {
        for (int k = 0; k < 8; k++)
            put_ret_imm(p + off[k], round * 16 + (uint32_t)k);
        for (int i = 0; i < 400; i++) {
            int k = i & 7;
            uint32_t v = fn[k]();
            if (v != round * 16 + (uint32_t)k) bad++;
            sink += v;
        }
    }
    if (!bad) printf("OK\n");
    else printf("FAIL %u stale calls\n", bad);
    return 0;
}
