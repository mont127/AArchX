/*
 * The call into host code, checked as far as it can be checked without a VM.
 *
 * Two halves.  The first is the table: every export the bridge claims must
 * also be an export of the virtual library, or the guest can never reach it,
 * and every export the bridge deliberately does not claim must still be an
 * export of the virtual library, or it would take the 71 path (nothing bound)
 * instead of the 72 path (bound, ran, no bridge behind it) and the two failure
 * modes would stop being distinguishable.  So each name is put through BOTH
 * readers - ocerz_bridge_lookup and the export trie of the image
 * ocerz_vdylib_image builds - rather than through a list written for the test.
 * The unbridged names here are not an arbitrary selection: they are the
 * variadic exclusion bridge.h states (printf, open, fcntl, ioctl), so a bridge
 * that grows one of them has changed a documented rule rather than broken a
 * test.  The list once also held atof and strtod, until signatures could name a
 * double, and qsort and bsearch, until a callback argument had a trampoline
 * back into guest code; qsort and bsearch are bridged names now, and their
 * lookups succeeding is also what proves the table's callback notation parses.
 *
 * The second half drives ocerz_bridge_invoke against a hand-built CPU.  The
 * memory map is the identity one, ocerz_mem_init_identity, because that is the
 * map native mode runs in and the one the bridge's pointer conversions are
 * written against; under it a host pointer and a guest pointer are the same
 * number, so a function that returns a pointer can be checked for the right
 * VALUE rather than merely a non-zero one.  What the non-identity map would
 * additionally pin - that the argument conversion really is applied, and that
 * a null return stays null instead of becoming the arena base - cannot be
 * pinned here as well, because a process gets one map and malloc's return is
 * only expressible as a guest address in this one.
 *
 * The register contract asserted is the one the stub implies.  A guest calls
 * an export with a CALL, so the return address is already on the stack when
 * the stub's jump reaches the trap window; the bridge therefore has to consume
 * it exactly as a RET would, rip from [rsp] and rsp forward by eight, with the
 * result in rax.  rax is poisoned before every call so a bridge that returns
 * without writing it fails rather than inheriting whatever was there.
 */
#include "ocerz/bridge.h"
#include "ocerz/vdylib.h"
#include "ocerz/dyld.h"
#include "ocerz/dyldapi.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cpu.h"
#include "ocerz/interp.h"

#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD_BASE  0x0000000210000000ull
#define RET_ADDR   0x0000000044332200ull
#define RAX_POISON 0xfeedfacecafebeefull
#define ARENA      (4ull << 30)
#define SCRATCH    0x10000ull

static int checks;
static int failures;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static const char *const kLib = "/usr/lib/libSystem.B.dylib";

static const char *const kBridged[] = {
    "___bzero", "_memcpy", "_memmove", "_memset", "_memcmp", "_memchr",
    "_strlen", "_strnlen", "_strcmp", "_strncmp", "_strcpy", "_strncpy",
    "_strcat", "_strchr", "_strrchr", "_strstr", "_strdup",
    "_malloc", "_calloc", "_realloc", "_free",
    "_write", "_read", "_close", "_puts", "_putchar", "_getenv", "_getpid",
    "_isatty", "_abs", "_labs", "_atoi", "_atol", "_time", "_clock",
    "_qsort", "_bsearch",
    "___error", "_exit", "_abort", "___stack_chk_fail",
};
#define NBRIDGED (sizeof kBridged / sizeof kBridged[0])

static const char *const kUnbridged[] = {
    "_printf", "_fprintf", "_sprintf", "_snprintf",
    "_open", "_fcntl", "_ioctl",
};
#define NUNBRIDGED (sizeof kUnbridged / sizeof kUnbridged[0])

static OcerzVM vm;
static uint64_t scratch;
static uint64_t stack_top;

static uint64_t put_str(uint64_t gaddr, const char *s)
{
    memcpy(ocerz_g2h(gaddr), s, strlen(s) + 1);
    return gaddr;
}

static uint64_t arm_call(OcerzCPU *cpu, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint64_t sp = stack_top - 8;

    memset(cpu->gpr, 0, sizeof cpu->gpr);
    cpu->gpr[OCERZ_RDI] = a0;
    cpu->gpr[OCERZ_RSI] = a1;
    cpu->gpr[OCERZ_RDX] = a2;
    cpu->gpr[OCERZ_RAX] = RAX_POISON;
    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;
    ocerz_st(sp, 8, RET_ADDR);
    return sp;
}

static void check_return(const char *sym, const OcerzCPU *cpu, uint64_t sp)
{
    CHECK(cpu->rip == RET_ADDR,
          "%s: rip is %#llx after the bridge, want the return address %#llx",
          sym, (unsigned long long)cpu->rip, (unsigned long long)RET_ADDR);
    CHECK(cpu->gpr[OCERZ_RSP] == sp + 8,
          "%s: rsp is %#llx after the bridge, want %#llx (the call's return "
          "address popped)",
          sym, (unsigned long long)cpu->gpr[OCERZ_RSP],
          (unsigned long long)(sp + 8));
}

static void test_table(void)
{
    size_t i;
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);

    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);

    for (i = 0; i < NBRIDGED; i++) {
        const char *sym = kBridged[i];
        const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);

        CHECK(fn != NULL, "%s has no bridge descriptor", sym);
        CHECK(fn == ocerz_bridge_lookup(kLib, sym),
              "%s resolves to a different descriptor on a second lookup", sym);
        if (img) {
            int found = 0;
            uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
            CHECK(found,
                  "%s is bridged but %s does not export it, so no guest can "
                  "reach the bridge", sym, kLib);
            CHECK(!found || a != 0, "%s resolved to address zero", sym);
        }
    }

    for (i = 0; i < NUNBRIDGED; i++) {
        const char *sym = kUnbridged[i];
        const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);

        CHECK(fn == NULL,
              "%s has a bridge descriptor, but bridge.h excludes it", sym);
        if (img) {
            int found = 0;
            ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
            CHECK(found,
                  "%s is neither bridged nor exported by %s, so it would take "
                  "the 71 path instead of the 72 one", sym, kLib);
        }
    }

    CHECK(ocerz_bridge_lookup("/usr/lib/libNoSuchLibrary.dylib", "_memcpy") == NULL,
          "a library nobody provides has a bridge for _memcpy");
    CHECK(ocerz_bridge_lookup("", "_memcpy") == NULL,
          "the empty install name has a bridge for _memcpy");
    CHECK(ocerz_bridge_lookup(kLib, "_ocerz_no_such_export_xyzzy") == NULL,
          "a symbol nobody exports has a bridge descriptor");

    free(img);
}

static void test_invoke_int(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, "_strlen");
    uint64_t gs = put_str(scratch, "ocerz-bridge");
    uint64_t sp;
    int r;

    CHECK(fn != NULL, "_strlen has no bridge descriptor");
    if (!fn)
        return;

    sp = arm_call(cpu, gs, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strlen: invoke returned %d, want OCERZ_STEP_OK", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 12,
          "_strlen(\"ocerz-bridge\") put %#llx in rax, want 12",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strlen", cpu, sp);

    sp = arm_call(cpu, put_str(scratch + 64, ""), 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strlen: invoke returned %d on an empty string", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 0,
          "_strlen(\"\") put %#llx in rax, want 0",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strlen", cpu, sp);
}

static void test_invoke_ptr(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, "_strchr");
    uint64_t gs = put_str(scratch + 128, "ocerz-bridge");
    uint64_t sp;
    int r;

    CHECK(fn != NULL, "_strchr has no bridge descriptor");
    if (!fn)
        return;

    sp = arm_call(cpu, gs, '-', 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strchr: invoke returned %d, want OCERZ_STEP_OK", r);
    CHECK(cpu->gpr[OCERZ_RAX] == gs + 5,
          "_strchr(\"ocerz-bridge\", '-') put %#llx in rax, want %#llx",
          (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)(gs + 5));
    check_return("_strchr", cpu, sp);

    sp = arm_call(cpu, gs, 'q', 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strchr: invoke returned %d on a miss", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 0,
          "_strchr(\"ocerz-bridge\", 'q') put %#llx in rax, want 0",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strchr", cpu, sp);
}

static void test_invoke_heap(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *mal = ocerz_bridge_lookup(kLib, "_malloc");
    const struct OcerzBridgeFn *fre = ocerz_bridge_lookup(kLib, "_free");
    unsigned char want[64];
    uint64_t block, sp;
    int r;

    CHECK(mal != NULL, "_malloc has no bridge descriptor");
    CHECK(fre != NULL, "_free has no bridge descriptor");
    if (!mal || !fre)
        return;

    sp = arm_call(cpu, 64, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, mal);
    CHECK(r == OCERZ_STEP_OK, "_malloc: invoke returned %d, want OCERZ_STEP_OK", r);
    block = cpu->gpr[OCERZ_RAX];
    CHECK(block != 0 && block != RAX_POISON,
          "_malloc(64) put %#llx in rax", (unsigned long long)block);
    check_return("_malloc", cpu, sp);
    if (block == 0 || block == RAX_POISON)
        return;

    memset(want, 0x5a, sizeof want);
    memcpy(ocerz_g2h(block), want, sizeof want);
    CHECK(memcmp(ocerz_g2h(block), want, sizeof want) == 0,
          "the 64 bytes written through the guest address %#llx that _malloc "
          "returned did not read back",
          (unsigned long long)block);

    sp = arm_call(cpu, block, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fre);
    CHECK(r == OCERZ_STEP_OK, "_free: invoke returned %d, want OCERZ_STEP_OK", r);
    check_return("_free", cpu, sp);
}

static int report(void)
{
    printf("test_bridge: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }
    if (ocerz_vm_init(&vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    scratch = ocerz_map_anywhere(SCRATCH, PROT_READ | PROT_WRITE);
    if (scratch == 0) {
        fprintf(stderr, "scratch alloc failed\n");
        return 2;
    }
    stack_top = scratch + SCRATCH / 2;

    test_table();
    test_invoke_int();
    test_invoke_ptr();
    test_invoke_heap();
    ocerz_bridge_report();

    return report();
}
