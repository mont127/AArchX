/*
 * The way back from native code into guest code, checked as far as it can be
 * checked without running any guest code.
 *
 * Four things are asserted.  The bank: ocerz_abi_callback_bank to
 * ocerz_abi_callback_bank_end is exactly 4096 slots of eight bytes, which is
 * what lets a slot find its own index from its own address.  The notation: an
 * argument of class c carries its callback's own signature in braces, stored
 * against that argument's index with every other entry left empty, and each way
 * of getting that wrong is refused - c as a result, c with no braces, a brace
 * never closed, a nested signature that itself names a callback, one that does
 * not parse, and one too long for OCERZ_ABI_CB_MAX.  Interning: one guest
 * function under one signature is one address, whichever string the signature
 * arrives in and whatever was interned in between; a different function or a
 * different signature is a different address; every address is a slot of the
 * bank on an eight-byte boundary; and a notation that is malformed or names a
 * callback gets NULL.  Refusal: a slot reached on a thread with no guest cpu
 * returns zero in both result registers, checked through the dispatcher with
 * its outputs poisoned beforehand and through the slot's own address called as
 * an ordinary native function pointer, on the test's main thread and on a thread
 * the test creates, which is what a native framework's private thread looks
 * like.
 *
 * Exhaustion runs last because it cannot be undone.  Distinct functions are
 * interned until the bank refuses, which must happen at exactly 4096 slots
 * counting the ones the earlier checks took, with no address handed out twice,
 * and a pair interned before the bank filled must still get its own address
 * back afterwards.  A refused notation that quietly consumed a slot shows up
 * here as the bank running out early.
 *
 * The too-long refusal cannot be isolated.  OCERZ_ABI_CB_MAX is 24 bytes and
 * the longest signature the parser accepts at all is nineteen characters, a
 * result, sixteen arguments and two parentheses, so a nested signature too long
 * to store also has too many arguments; the test asserts the refusal and not
 * which rule produced it, and separately that the nineteen-character one is
 * accepted and stored whole.
 *
 * What is not here is an argument reaching a guest function.  No unit test in
 * this tree enters guest code through ocerz_vm_call, and a harness built to do
 * it would be tested as much as the engine.  Argument placement, the result,
 * the stack alignment the guest is entered with, a bridged strcmp inside a
 * comparator, qsort re-entered from its own comparator, and a guest fault inside
 * a callback are covered end to end by the callback_* cases in
 * tests/run_native_tests.sh, where real guest comparators are called by the
 * host's real qsort and bsearch.
 *
 * The map is the identity one, as in test_abi.c and test_bridge.c, because
 * native mode is the only mode that hands a slot to native code.
 */
#include "ocerz/abi.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA       (4ull << 30)
#define BANK_SLOTS  4096u
#define BANK_STRIDE 8u
#define FN_A        0x0000000100004000ull
#define FN_B        0x0000000100004010ull
#define FN_C        0x0000000100004020ull
#define FN_EXHAUST  0x0000000180000000ull
#define OUT_POISON  0xfeedfacecafebeefull

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

static unsigned char g_used[BANK_SLOTS];
static unsigned g_nused;
static void *g_cmp_addr;
static void *g_dbl_addr;

static uintptr_t bank_lo(void)
{
    return (uintptr_t)ocerz_abi_callback_bank;
}

static uintptr_t bank_hi(void)
{
    return (uintptr_t)ocerz_abi_callback_bank_end;
}

static int slot_of(void *addr, unsigned *slot)
{
    uintptr_t a = (uintptr_t)addr;

    if (a < bank_lo() || a >= bank_hi() || (a - bank_lo()) % BANK_STRIDE != 0)
        return 0;
    *slot = (unsigned)((a - bank_lo()) / BANK_STRIDE);
    return *slot < BANK_SLOTS;
}

static int note_new(void *addr, const char *what)
{
    unsigned slot = 0;

    CHECK(slot_of(addr, &slot),
          "%s: %p is not a slot of the bank [%p, %p) on an 8-byte boundary", what,
          addr, (void *)bank_lo(), (void *)bank_hi());
    if (!slot_of(addr, &slot))
        return 0;
    CHECK(!g_used[slot], "%s: slot %u at %p was already handed out", what, slot,
          addr);
    if (g_used[slot])
        return 0;
    g_used[slot] = 1;
    g_nused++;
    return 1;
}

static void test_bank(void)
{
    uintptr_t span = bank_hi() - bank_lo();

    CHECK(bank_hi() > bank_lo(),
          "the bank ends at %p, which is not after its start %p",
          (void *)bank_hi(), (void *)bank_lo());
    CHECK(span == 4096u * 8u,
          "the bank spans %llu bytes, want 4096 slots of 8 bytes, %u",
          (unsigned long long)span, 4096u * 8u);
    CHECK(OCERZ_ABI_CALLBACK_SLOTS == 4096 && OCERZ_ABI_CALLBACK_STRIDE == 8,
          "abi.h declares %d slots of %d bytes, want 4096 of 8",
          (int)OCERZ_ABI_CALLBACK_SLOTS, (int)OCERZ_ABI_CALLBACK_STRIDE);
}

typedef struct CbAccept {
    const char *notation;
    char ret;
    const char *args;
    int at[2];
    const char *nested[2];
} CbAccept;

static const CbAccept kAccept[] = {
    { "v(pLLc{i(pp)})", 'v', "pLLc", { 3, -1 }, { "i(pp)", NULL } },
    { "p(ppLLc{i(pp)})", 'p', "ppLLc", { 4, -1 }, { "i(pp)", NULL } },
    { "v(c{v()})", 'v', "c", { 0, -1 }, { "v()", NULL } },
    { "i(c{d(dd)}pc{L(LLLLLLLLLLLLLLLL)})", 'i', "cpc", { 0, 2 },
      { "d(dd)", "L(LLLLLLLLLLLLLLLL)" } },
};
#define NACCEPT (sizeof kAccept / sizeof kAccept[0])

static void check_accept(const CbAccept *c)
{
    OcerzAbiSig sig;
    int n = (int)strlen(c->args);
    int r, i, k;

    memset(&sig, 0xa5, sizeof sig);
    r = ocerz_abi_parse(c->notation, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK",
          c->notation, r);
    if (r != OCERZ_OK)
        return;
    CHECK(sig.ret == c->ret, "%s: parsed result class '%c', want '%c'",
          c->notation, sig.ret ? sig.ret : '?', c->ret);
    CHECK(sig.nargs == n, "%s: parsed %d arguments, want %d", c->notation,
          sig.nargs, n);
    for (i = 0; i < n && i < sig.nargs; i++)
        CHECK(sig.arg[i] == c->args[i],
              "%s: parsed argument %d as class '%c', want '%c'", c->notation, i,
              sig.arg[i] ? sig.arg[i] : '?', c->args[i]);

    for (i = 0; i < OCERZ_ABI_MAX_ARGS; i++) {
        const char *want = NULL;

        for (k = 0; k < 2; k++)
            if (c->at[k] == i)
                want = c->nested[k];
        if (want) {
            CHECK(memchr(sig.cb[i], '\0', OCERZ_ABI_CB_MAX) != NULL &&
                      strcmp(sig.cb[i], want) == 0,
                  "%s: cb[%d] holds \"%.*s\", want the nested notation \"%s\"",
                  c->notation, i, OCERZ_ABI_CB_MAX, sig.cb[i], want);
        } else {
            CHECK(sig.cb[i][0] == '\0',
                  "%s: cb[%d] holds \"%.*s\", but argument %d names no callback",
                  c->notation, i, OCERZ_ABI_CB_MAX, sig.cb[i], i);
        }
    }
}

static const char *const kReject[] = {
    "c()", "c(pp)", "c{i(pp)}(pp)", "c{i(pp)}()",
    "v(c)", "v(pLLc)", "v(cp)", "p(ppLLc)", "v(c())", "v(ci(pp))",
    "v(c{", "v(c{i(pp)", "v(c{i(pp))", "v(pLLc{i(pp))", "v(c{i(pp)}",
    "v(c{v(c{i(pp)})})", "v(c{i(pc{i(pp)})})", "v(c{i(c)})", "v(c{c()})",
    "v(c{})", "v(c{i(pp})", "v(c{x(p)})", "v(c{i(v)})", "v(c{i(pp)x})",
    "v(c{i(s)})", "v(c{(pp)})", "v(c{i})",
    "v(p{i(pp)})", "v(pLLc{i(pp)}})", "v(c})", "v({i(pp)})",
};
#define NREJECT (sizeof kReject / sizeof kReject[0])

static void check_reject(const char *notation, const char *why)
{
    OcerzAbiSig sig;
    int r;

    memset(&sig, 0xa5, sizeof sig);
    r = ocerz_abi_parse(notation, &sig);
    CHECK(r != OCERZ_OK, "ocerz_abi_parse(\"%s\") accepted %s", notation, why);
}

static void test_parse(void)
{
    static const size_t kLong[] = {
        OCERZ_ABI_CB_MAX, OCERZ_ABI_CB_MAX + 1, 64,
    };
    char longer[128];
    size_t i, k, n;

    for (i = 0; i < NACCEPT; i++)
        check_accept(&kAccept[i]);

    for (i = 0; i < NREJECT; i++)
        check_reject(kReject[i], "a callback notation that is not well formed");

    for (i = 0; i < sizeof kLong / sizeof kLong[0]; i++) {
        n = 0;
        memcpy(longer + n, "v(c{L(", 6);
        n += 6;
        for (k = 0; k + 3 < kLong[i]; k++)
            longer[n++] = 'L';
        memcpy(longer + n, ")})", 4);
        check_reject(longer, "a nested notation longer than OCERZ_ABI_CB_MAX can hold");
    }
}

static void test_intern(void)
{
    static const char *const kMalformed[] = {
        "", "i(pp", "q(pp)", "i(pp)x", "i(s)", "(pp)", "i(v)",
    };
    static const char *const kNamesCallback[] = {
        "v(c{i(pp)})", "v(pLLc{i(pp)})", "i(pc{v()})",
    };
    char same[] = "i(pp)";
    void *a, *a2, *a3, *b, *c, *d;
    size_t i;

    a = ocerz_abi_callback_intern(FN_A, "i(pp)");
    CHECK(a != NULL, "interning %#llx as i(pp) returned NULL",
          (unsigned long long)FN_A);
    if (a)
        note_new(a, "FN_A i(pp)");

    a2 = ocerz_abi_callback_intern(FN_A, same);
    CHECK(a2 == a,
          "interning %#llx as i(pp) a second time, from a different string, "
          "gave %p, want the first address %p", (unsigned long long)FN_A, a2, a);

    b = ocerz_abi_callback_intern(FN_B, "i(pp)");
    CHECK(b != NULL && b != a,
          "interning a different function %#llx as i(pp) gave %p, want an "
          "address other than %p and not NULL", (unsigned long long)FN_B, b, a);
    if (b && b != a)
        note_new(b, "FN_B i(pp)");

    c = ocerz_abi_callback_intern(FN_A, "l(pp)");
    CHECK(c != NULL && c != a && c != b,
          "interning %#llx as l(pp) gave %p, want an address other than its "
          "i(pp) one %p and not NULL", (unsigned long long)FN_A, c, a);
    if (c && c != a && c != b)
        note_new(c, "FN_A l(pp)");

    d = ocerz_abi_callback_intern(FN_C, "d(dd)");
    CHECK(d != NULL && d != a && d != b && d != c,
          "interning %#llx as d(dd) gave %p, want an address no other binding "
          "has and not NULL", (unsigned long long)FN_C, d);
    if (d && d != a && d != b && d != c)
        note_new(d, "FN_C d(dd)");

    a3 = ocerz_abi_callback_intern(FN_A, "i(pp)");
    CHECK(a3 == a,
          "interning %#llx as i(pp) after three other bindings gave %p, want "
          "%p", (unsigned long long)FN_A, a3, a);
    CHECK(ocerz_abi_callback_intern(FN_B, "i(pp)") == b,
          "re-interning %#llx as i(pp) did not give back %p",
          (unsigned long long)FN_B, b);

    for (i = 0; i < sizeof kMalformed / sizeof kMalformed[0]; i++) {
        void *p = ocerz_abi_callback_intern(FN_A, kMalformed[i]);
        CHECK(p == NULL, "interning %#llx as the malformed notation \"%s\" "
              "gave %p, want NULL", (unsigned long long)FN_A, kMalformed[i], p);
    }
    for (i = 0; i < sizeof kNamesCallback / sizeof kNamesCallback[0]; i++) {
        void *p = ocerz_abi_callback_intern(FN_B, kNamesCallback[i]);
        CHECK(p == NULL, "interning %#llx as \"%s\", a callback signature "
              "that itself takes a callback, gave %p, want NULL",
              (unsigned long long)FN_B, kNamesCallback[i], p);
    }

    g_cmp_addr = a;
    g_dbl_addr = d;
}

static void refuse_here(const char *where)
{
    static const int ka = 1, kb = 2;
    uint64_t x[8], v[8], ox, ov, bits;
    uint8_t stack[64];
    unsigned slot = 0;
    int (*cmp)(const void *, const void *);
    double (*dbl)(double, double);
    double dr;
    int r;

    CHECK(ocerz_vm_current_cpu() == NULL,
          "%s: ocerz_vm_current_cpu is %p, so this thread has a guest cpu and "
          "nothing below tests a refusal", where, (void *)ocerz_vm_current_cpu());

    if (g_cmp_addr && slot_of(g_cmp_addr, &slot)) {
        memset(x, 0, sizeof x);
        memset(v, 0, sizeof v);
        memset(stack, 0, sizeof stack);
        x[0] = (uint64_t)(uintptr_t)&ka;
        x[1] = (uint64_t)(uintptr_t)&kb;
        ox = OUT_POISON;
        ov = OUT_POISON;
        ocerz_abi_callback_dispatch(slot, x, v, stack, &ox, &ov);
        CHECK(ox == 0 && ov == 0,
              "%s: dispatching the i(pp) slot %u with no guest cpu left "
              "x0=%#llx v0=%#llx, want both zero", where, slot,
              (unsigned long long)ox, (unsigned long long)ov);

        cmp = (int (*)(const void *, const void *))(uintptr_t)g_cmp_addr;
        r = cmp(&ka, &kb);
        CHECK(r == 0,
              "%s: calling the i(pp) slot at %p as a native function returned "
              "%d, want the refusal's 0", where, g_cmp_addr, r);
    } else {
        CHECK(0, "%s: no i(pp) slot was interned, so its refusal is untested",
              where);
    }

    if (g_dbl_addr && slot_of(g_dbl_addr, &slot)) {
        memset(x, 0, sizeof x);
        memset(v, 0, sizeof v);
        memset(stack, 0, sizeof stack);
        v[0] = 0x3ff8000000000000ull;
        v[1] = 0xc002000000000000ull;
        ox = OUT_POISON;
        ov = OUT_POISON;
        ocerz_abi_callback_dispatch(slot, x, v, stack, &ox, &ov);
        CHECK(ox == 0 && ov == 0,
              "%s: dispatching the d(dd) slot %u with no guest cpu left "
              "x0=%#llx v0=%#llx, want both zero", where, slot,
              (unsigned long long)ox, (unsigned long long)ov);

        dbl = (double (*)(double, double))(uintptr_t)g_dbl_addr;
        dr = dbl(1.5, -2.25);
        memcpy(&bits, &dr, sizeof bits);
        CHECK(bits == 0,
              "%s: calling the d(dd) slot at %p as a native function returned "
              "the bits %#llx, want the refusal's +0.0", where, g_dbl_addr,
              (unsigned long long)bits);
    } else {
        CHECK(0, "%s: no d(dd) slot was interned, so its refusal is untested",
              where);
    }
}

static void *refuse_thread(void *arg)
{
    refuse_here("a thread the test created");
    return arg;
}

static void test_refusal(void)
{
    pthread_t th;
    int rc;

    refuse_here("the test's main thread");

    rc = pthread_create(&th, NULL, refuse_thread, NULL);
    CHECK(rc == 0, "pthread_create failed with %d, so the refusal on a thread "
          "nobody attached is untested", rc);
    if (rc == 0)
        pthread_join(th, NULL);
}

static void test_exhaustion(void)
{
    const unsigned limit = BANK_SLOTS + 64u;
    const unsigned before = g_nused;
    void *addr = NULL, *first = NULL;
    unsigned handed = 0, i;

    for (i = 0; i < limit; i++) {
        addr = ocerz_abi_callback_intern(FN_EXHAUST + 16ull * i, "i(pp)");
        if (!addr)
            break;
        if (!first)
            first = addr;
        handed++;
        note_new(addr, "exhaustion");
    }

    CHECK(addr == NULL,
          "interning %u distinct functions never returned NULL from a bank of "
          "%u slots", limit, BANK_SLOTS);
    CHECK(g_nused == BANK_SLOTS && before + handed == BANK_SLOTS,
          "the bank refused after %u distinct slot(s), %u taken earlier in this "
          "test and %u here, want exactly %u; running out early means a refused "
          "notation or a repeated binding consumed a slot", g_nused, before,
          handed, BANK_SLOTS);

    addr = ocerz_abi_callback_intern(FN_EXHAUST + 16ull * (limit + 1), "i(pp)");
    CHECK(addr == NULL,
          "a new function interned after the bank refused got %p, want NULL "
          "again", addr);

    addr = ocerz_abi_callback_intern(FN_A, "i(pp)");
    CHECK(addr != NULL && addr == g_cmp_addr,
          "re-interning %#llx as i(pp) with the bank full gave %p, want its "
          "existing %p", (unsigned long long)FN_A, addr, g_cmp_addr);
    if (first) {
        addr = ocerz_abi_callback_intern(FN_EXHAUST, "i(pp)");
        CHECK(addr == first,
              "re-interning the first exhaustion function with the bank full "
              "gave %p, want its existing %p", addr, first);
    }
}

static int report(void)
{
    printf("test_callback: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }

    test_bank();
    test_parse();
    test_intern();
    test_refusal();
    test_exhaustion();

    return report();
}
