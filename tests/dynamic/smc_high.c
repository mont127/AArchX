/*
 * Self-modifying code in a HIGH page.  Wine maps most of its memory above the
 * emulator's low window and V8 writes its JIT code into pages up there.  Once
 * code from such a page has been translated the page is write-trapped, and the
 * first write must be recognised as an armed-page write and retried, not
 * treated as a wild host fault - that path used to end the thread silently,
 * which under Steam killed a V8 background job holding a JitPage mutex and
 * left the renderer's GC safepoint waiting forever.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void put_code(uint8_t *p, uint32_t imm)
{
    p[0] = 0xb8; memcpy(p + 1, &imm, 4);
    p[5] = 0xc3;
}

int main(void)
{
    uint8_t *page = mmap((void *)0x6fffbb240000ull, 0x8000, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    if ((uintptr_t)page < 0x300000000ull) { printf("page not high: %p\n", (void *)page); return 1; }
    int (*fn)(void) = (int (*)(void))page;
    int (*fn2)(void) = (int (*)(void))(page + 0x4000);
    put_code(page, 17);
    int a = fn();
    put_code(page, 42);
    int b = fn();
    put_code(page + 0x4000, 5);
    int c = fn2();
    put_code(page + 0x4000, 6);
    int d = fn2();
    put_code(page + 0x1f0, 9);
    int e = ((int (*)(void))(page + 0x1f0))();
    if (a == 17 && b == 42 && c == 5 && d == 6 && e == 9) printf("OK\n");
    else printf("FAIL a=%d b=%d c=%d d=%d e=%d\n", a, b, c, d, e);
    return 0;
}
