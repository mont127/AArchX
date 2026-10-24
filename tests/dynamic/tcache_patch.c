/*
 * Records of the shared cache's code next to code a program patches.  Run
 * plain, the program calls atoi from the cache, whose blocks go into the store.
 * Run with "patch", it makes atoi's page writable before calling it, drops a
 * jmp to its own function there and calls it: atoi's records are found stale
 * and translated again, and the call must reach the patch rather than the
 * stored translation of atoi.  The patched run loads nothing ahead
 * (run_dynamic_tests.sh), so atoi's record is found at that first call; loaded
 * before the patch, its block would be retired by it and never reused, which
 * tests nothing here.  Run with "wait", it sleeps first, so the warm thread has
 * read every record and the next miss loads the rest ahead
 * (OCERZ_TCACHE_AHEAD, run_dynamic_tests.sh), the patched atoi's among them:
 * that record has a dep in this program where the patched run had it, not
 * mapped now, so its deps check faults inside the run of loads.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile int g_hit;

static int my_atoi(const char *s)
{
    (void)s;
    g_hit++;
    return 4242;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "wait"))
        usleep(300000);
    int (*p_atoi)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "atoi");
    if (!p_atoi) { printf("no atoi\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "patch")) {
        uintptr_t page = (uintptr_t)p_atoi & ~0xfffULL;
        if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { printf("mprotect\n"); return 1; }
        volatile uint64_t *slot = (volatile uint64_t *)(void *)p_atoi;
        slot[0] = 0xcccc0000000225ffULL;   /* jmp *0(%rip), then the target */
        slot[1] = (uint64_t)(uintptr_t)my_atoi;
        mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
        int r = p_atoi("41");
        if (r != 4242 || g_hit != 1) { printf("ran the stored atoi (ret=%d hit=%d)\n", r, g_hit); return 1; }
        printf("OK\n");
        return 0;
    }
    long acc = 0;
    for (int i = 0; i < 20000; i++)
        acc += p_atoi("41");
    if (acc != 20000L * 41) { printf("atoi wrong\n"); return 1; }
    printf("OK\n");
    return 0;
}
