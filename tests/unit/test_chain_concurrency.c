/* Patching a chained block tail while another thread runs it. */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/jit.h"
#include "ocerz/interp.h"
#include "ocerz/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define CODE_BASE 0x200000000ull
#define A_RIP     (CODE_BASE + 0x00)
#define B_RIP     (CODE_BASE + 0x40)
#define DONE_RIP  (CODE_BASE + 0x80)
#define STACK_TOP (CODE_BASE + 0x8000)

static OcerzVM g_vm;

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void put_bytes(uint64_t gaddr, const uint8_t *b, size_t n)
{
    uint8_t *p = (uint8_t *)ocerz_g2h(gaddr);
    memcpy(p, b, n);
}

static atomic_int  g_spinner_ready;
static atomic_int  g_target_compiled;
static atomic_int  g_stop_spin;
static atomic_ullong g_saw_done;
static atomic_ullong g_saw_b;
static atomic_int  g_bad_rip;
static atomic_int  g_bad_rc;

static void *spinner_entry(void *arg)
{
    (void)arg;
    OcerzCPU cpu;
    unsigned long long iters = 0;
    atomic_store(&g_spinner_ready, 1);
    for (;;) {
        ocerz_cpu_reset(&cpu);
        cpu.rip = A_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        if (rc != OCERZ_STEP_OK)
            atomic_store(&g_bad_rc, rc ? rc : -999);
        if (cpu.rip == DONE_RIP)
            atomic_fetch_add(&g_saw_done, 1);
        else if (cpu.rip == B_RIP)
            atomic_fetch_add(&g_saw_b, 1);
        else
            atomic_store(&g_bad_rip, 1);
        iters++;

        if (atomic_load(&g_stop_spin) && iters > 200000)
            break;
        if (iters > 80000000ull)
            break;
    }
    return NULL;
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }

    {
        int32_t rel = (int32_t)(B_RIP - (A_RIP + 5));
        uint8_t s[5] = { 0xE8, (uint8_t)rel, (uint8_t)(rel >> 8),
                         (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
        put_bytes(A_RIP, s, 5);
    }

    {
        int32_t rel = (int32_t)(DONE_RIP - (B_RIP + 5));
        uint8_t t[5] = { 0xE9, (uint8_t)rel, (uint8_t)(rel >> 8),
                         (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
        put_bytes(B_RIP, t, 5);
    }

    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 1;
    ocerz_vm_install_handlers(&g_vm);
    if (!g_vm.jit) {
        fprintf(stderr, "jit not created\n");
        return 2;
    }

    {
        OcerzCPU cpu;
        ocerz_cpu_reset(&cpu);
        cpu.rip = A_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        CHECK(rc == OCERZ_STEP_OK, "warm S rc=%d", rc);
        CHECK(cpu.rip == B_RIP, "warm S unchained rip=%#llx want B", (unsigned long long)cpu.rip);
    }

    pthread_t spin;
    pthread_create(&spin, NULL, spinner_entry, NULL);
    while (!atomic_load(&g_spinner_ready))
        ;

    for (volatile int i = 0; i < 2000000; i++)
        ;

    {
        OcerzCPU cpu;
        ocerz_cpu_reset(&cpu);
        cpu.rip = B_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        CHECK(rc == OCERZ_STEP_OK, "compile T rc=%d", rc);
        CHECK(cpu.rip == DONE_RIP, "T rip=%#llx want DONE", (unsigned long long)cpu.rip);
        atomic_store(&g_target_compiled, 1);
    }

    for (volatile long i = 0; i < 20000000L; i++)
        ;
    atomic_store(&g_stop_spin, 1);
    pthread_join(spin, NULL);

    unsigned long long saw_done = atomic_load(&g_saw_done);
    unsigned long long saw_b    = atomic_load(&g_saw_b);
    CHECK(!atomic_load(&g_bad_rc), "spinner saw a non-STEP_OK step (rc=%d)", atomic_load(&g_bad_rc));
    CHECK(!atomic_load(&g_bad_rip), "spinner saw a rip outside {B,DONE} -- torn/garbage patch");

    CHECK(saw_done > 0, "chain never observed by the spinning thread "
          "(saw_done=%llu saw_b=%llu) -- icache publish missing?", saw_done, saw_b);

    fprintf(stderr, "test_chain_concurrency: saw_b=%llu saw_done=%llu bad_rip=%d bad_rc=%d\n",
            saw_b, saw_done, atomic_load(&g_bad_rip), atomic_load(&g_bad_rc));
    if (g_fail) {
        fprintf(stderr, "test_chain_concurrency: %d checks FAILED\n", g_fail);
        return 1;
    }
    fprintf(stderr, "test_chain_concurrency: OK\n");
    return 0;
}
