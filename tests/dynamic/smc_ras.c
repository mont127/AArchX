/*
 * Rewritten code that live translations still reach.  A write to a code page
 * retires the blocks on that 16 KB host page, and nothing that outlives them
 * may still enter one: not the way back into a return site, not an indirect
 * branch's per-site cache.  The direct call here ends exactly at a host page
 * boundary and its callee has a branch, so it is not spliced into the caller:
 * the return site is a block of its own on the next page, and rewriting it
 * retires that block alone while the calling block stays live.  A second
 * function on the rewritten page is reached through a function pointer from
 * this file's loop, whose indirect-branch cache still holds the old body until
 * the retire takes it away.  Many rounds, so no single retire that happens to
 * coincide with a wider flush can hide a miss.
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
    uint8_t *raw = mmap(NULL, 4 * HOST_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    uint8_t *p0 = (uint8_t *)(((uintptr_t)raw + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1));
    uint8_t *p1 = p0 + HOST_PAGE;

    /* callee at the start of page 0: xor eax, eax; jz +0; ret */
    static const uint8_t callee[] = { 0x31, 0xc0, 0x74, 0x00, 0xc3 };
    memcpy(p0, callee, sizeof callee);
    /* caller ends at the page boundary: call p0, return site at p1 */
    uint8_t *call = p1 - 5;
    int32_t rel = (int32_t)(p0 - p1);
    call[0] = 0xe8; memcpy(call + 1, &rel, 4);
    /* an indirect-call target on the same page as the return site */
    uint8_t *tgt = p1 + 0x40;

    uint32_t (*caller)(void) = (uint32_t (*)(void))(void *)call;
    uint32_t (*fn)(void) = (uint32_t (*)(void))(void *)tgt;
    unsigned bad_ret = 0, bad_ind = 0;
    for (uint32_t round = 1; round <= 40; round++) {
        put_ret_imm(p1, round);
        put_ret_imm(tgt, 1000 + round);
        for (int i = 0; i < 200; i++) {
            if (caller() != round) bad_ret++;
            if (fn() != 1000 + round) bad_ind++;
        }
    }
    if (!bad_ret && !bad_ind) printf("OK\n");
    else printf("FAIL stale return site %u, stale indirect target %u\n", bad_ret, bad_ind);
    return 0;
}
