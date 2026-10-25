/*
 * Records of the shared cache's code that skip the deps check (src/jit.c,
 * tc_rec_pristine) next to code a program patches.  Run plain, the program
 * calls atoi from the cache, whose blocks go into the store as pristine.  Run
 * with "patch", it makes atoi's page writable before calling it, drops a jmp
 * to its own function there and calls it: the change of protection goes
 * through ocerz_jit_invalidate_range, which marks the cache's code touched, so
 * atoi's records are checked, found stale and translated again, and the call
 * must reach the patch rather than the stored translation of atoi.  The patched
 * run loads nothing ahead (run_dynamic_tests.sh), so atoi's record is found at
 * that first call; loaded before the patch, its block would be retired by it
 * and never reused, which tests nothing here.  Run with "wait", it sleeps before
 * the calls, so the warm thread has read every record and the next miss loads
 * the rest ahead (OCERZ_TCACHE_AHEAD, run_dynamic_tests.sh), the patched atoi's
 * among them: that record has a dep in this program where the patched run had
 * it, not mapped now, so its deps check faults inside the run of loads.
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

/* labs, a block of the cache's code first run just before atoi: after the
   sleep its miss loads what the profile has next ahead, atoi's record */
__attribute__((noinline)) static int calls(long (*p_labs)(long), int (*p_atoi)(const char *))
{
    long acc = p_labs(-1) - 1;
    for (int i = 0; i < 20000; i++)
        acc += p_atoi("41");
    return acc == 20000L * 41;
}

int main(int argc, char **argv)
{
    int (*p_atoi)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "atoi");
    long (*p_labs)(long) = (long (*)(long))dlsym(RTLD_DEFAULT, "labs");
    if (!p_atoi || !p_labs) { printf("no atoi\n"); return 1; }
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
    if (argc > 1 && !strcmp(argv[1], "wait"))
        usleep(300000);
    if (!calls(p_labs, p_atoi)) { printf("atoi wrong\n"); return 1; }
    printf("OK\n");
    return 0;
}
