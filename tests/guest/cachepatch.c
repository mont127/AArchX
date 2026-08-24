/* Reproducer for hot-patching a dyld-shared-cache TEXT page, the way engines
 * that divert a libsystem entry point do it: run the victim first (so it is
 * translated), mprotect its page writable, drop a 6-byte indirect jmp into it,
 * then call it again and check the patch took effect. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>

static volatile int g_hit;

static int my_atoi(const char *s)
{
    (void)s;
    g_hit++;
    return 4242;
}

int main(void)
{
    int fails = 0;
    int (*p_atoi)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "atoi");
    if (!p_atoi) { printf("a  dlsym(atoi) FAILED\n"); return 1; }
    printf("a  dlsym(atoi) = %p\n", (void *)p_atoi);

    /* make the victim hot so the JIT really has a translation of these bytes */
    long acc = 0;
    for (int i = 0; i < 200000; i++) acc += p_atoi("41");
    if (acc != 200000L * 41) { printf("b  pre-patch atoi wrong (acc=%ld) FAIL\n", acc); fails++; }
    else printf("b  pre-patch atoi(\"41\") x200000 OK\n");

    uintptr_t page = (uintptr_t)p_atoi & ~0xfffULL;
    if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        printf("c  mprotect(%p, 0x2000, RWX) = -1 errno=%d (%s) FAIL\n",
               (void *)page, errno, strerror(errno));
        fails++;
    } else {
        printf("c  mprotect(%p, 0x2000, RWX) OK\n", (void *)page);
    }

    /* ff 25 02 00 00 00 = jmp qword ptr [rip+2]; then the 8-byte target */
    uint64_t tramp = 0xcccc0000000225ffULL;
    uint64_t target = (uint64_t)(uintptr_t)my_atoi;
    volatile uint64_t *slot = (volatile uint64_t *)p_atoi;
    slot[0] = tramp;
    slot[1] = target;
    printf("d  store landed\n");

    if (slot[0] != tramp || slot[1] != target) {
        printf("e  readback FAIL (%016llx %016llx)\n",
               (unsigned long long)slot[0], (unsigned long long)slot[1]);
        fails++;
    } else {
        printf("e  readback OK\n");
    }

    int r = p_atoi("41");
    if (r != 4242 || g_hit != 1) {
        printf("f  post-patch call ran the ORIGINAL (ret=%d hit=%d) FAIL\n", r, g_hit);
        fails++;
    } else {
        printf("f  post-patch call diverted (ret=%d hit=%d) OK\n", r, g_hit);
    }

    /* write again, now that the page has been executed since the last patch:
     * the write-after-translate case a plain one-shot fix would miss */
    slot[1] = (uint64_t)(uintptr_t)my_atoi;
    r = p_atoi("41");
    if (r != 4242 || g_hit != 2) {
        printf("g  second patch FAIL (ret=%d hit=%d)\n", r, g_hit);
        fails++;
    } else {
        printf("g  second patch OK (ret=%d hit=%d)\n", r, g_hit);
    }

    printf("%s\n", fails ? "RESULT FAIL" : "RESULT PASS");
    return fails ? 1 : 0;
}
