/*
 * The way back from native code into guest code, and the question of whether a
 * function pointer needs to take it at all.
 *
 * Five things are asserted before any guest code runs.  The bank:
 * ocerz_abi_callback_bank to ocerz_abi_callback_bank_end is exactly 4096 slots
 * of eight bytes, which is what lets a slot find its own index from its own
 * address.  The notation: an argument of class c carries its callback's own
 * signature in braces, stored against that argument's index with every other
 * entry left empty, a nested signature may use the narrow classes, and each way
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
 * Conversion: ocerz_abi_is_guest_code calls a static function of the test
 * binary, strcmp and qsort from the shared cache, and a bank slot host code,
 * and calls the guest mapping, both ends of the guest reservation, and
 * anonymous host memory that no image covers guest code.
 * ocerz_abi_callback_convert gives null back as null, a native function and a
 * bank slot back unchanged, and a guest function a slot, the same one on a
 * second call, while guest code under a malformed notation is refused with a
 * zero result.  A c argument read by ocerz_abi_read_guest reaches x3 as the
 * native comparator itself, as null, or as the guest function's slot.  A bridged
 * call to the host's real qsort given a native comparator sorts its array by
 * calling that comparator directly; one routed through the bank would have
 * been refused on the main thread, which has no guest cpu, and never run.
 * Slots interned just before and just after that crossing are adjacent, so the
 * crossing took none.
 *
 * Narrow callbacks are the one place this file runs guest code, because the
 * only evidence that an 8- or 16-bit value crossed correctly is what the
 * guest's registers held and what the native caller read back.  The guest
 * function is a 207-byte recorder assembled by clang -arch x86_64 and copied
 * into guest memory.  It stores rdi to r9, the low words of xmm0 to xmm7 and
 * the ten eightbytes above its return address into a block at offset 0x200,
 * counts the call, loads rax and xmm0 from offset 0x300 and returns, so it asks
 * nothing of the interpreter beyond mov, movq, inc and ret.  The VM runs it with
 * the JIT off, on a thread the test creates and attaches itself, since no VM
 * is the process's in a harness and the dispatcher could not attach one.
 *
 * The recorder is bound under several notations, and a slot is called two
 * ways.  Called as a native function pointer of the matching C prototype, the
 * arguments are packed by clang's arm64 caller under Apple's rule and extended
 * only to 32 bits, and the result is read back through w0 with no second
 * extension, so a guest returning 0xdeadbeefcafe8080 must reach a b() caller as
 * 0xffffff80 and a B() caller as 0x80.  Called through
 * ocerz_abi_callback_dispatch with hand-built words, the x registers hold
 * garbage above their low bits - 0xdeadbeefcafe12ff for a B that must reach the
 * guest as 0xff - the stack is packed by hand with junk in its padding, and x0
 * is compared over all 64 bits.  The notations cover every narrow result,
 * B(bBhH) in registers, l(bBhHbBhH) spilling from host registers onto the
 * guest stack, l(iiiiiiiibhBHi) and h(bhBHbhBHbhBHbhBH) spilling from Apple's
 * packed stack onto eightbytes, and d(bdBdhfHf) keeping the integer and
 * floating-point counters apart.
 *
 * Exhaustion runs last among the interning checks because it cannot be undone.
 * Distinct functions are interned until the bank refuses, which must happen at
 * exactly 4096 slots counting the ones the earlier checks took, with no address
 * handed out twice, and a pair interned before the bank filled must still get
 * its own address back afterwards.  A refused notation that quietly consumed a
 * slot shows up here as the bank running out early.  With the bank full,
 * converting new guest code is refused with a zero result, while a native
 * function still converts to itself and the qsort crossing still sorts, since
 * a native comparator never needed a slot.
 *
 * The too-long refusal cannot be isolated.  OCERZ_ABI_CB_MAX is 24 bytes and
 * the longest signature the parser accepts at all is nineteen characters, a
 * result, sixteen arguments and two parentheses, so a nested signature too long
 * to store also has too many arguments; the test asserts the refusal and not
 * which rule produced it, and separately that the nineteen-character one is
 * accepted and stored whole.
 *
 * What is not here is a guest callback doing real work.  The stack alignment
 * the guest is entered with, a bridged strcmp inside a comparator, qsort
 * re-entered from its own comparator, and a guest fault inside a callback are
 * covered end to end by the callback_* cases in tests/run_native_tests.sh,
 * where real guest comparators are called by the host's real qsort and bsearch.
 *
 * The map is the identity one, as in test_abi.c and test_bridge.c, because
 * native mode is the only mode that hands a slot to native code.
 */
#include "ocerz/abi.h"
#include "ocerz/cpu.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA       (4ull << 30)
#define BANK_SLOTS  4096u
#define BANK_STRIDE 8u
#define FN_A        0x0000000100004000ull
#define FN_B        0x0000000100004010ull
#define FN_C        0x0000000100004020ull
#define FN_EXHAUST  0x0000000180000000ull
#define OUT_POISON  0xfeedfacecafebeefull
#define FN_ADJ_1    0x0000000100005000ull
#define FN_ADJ_2    0x0000000100005010ull
#define GUEST_SPAN  0x10000ull
#define REC_OFF     0x200ull
#define RES_OFF     0x300ull
#define REC_XMM     6
#define REC_STACK   14
#define REC_COUNT   24
#define RET_ADDR    0x0000000044332200ull

static const uint8_t kRecorder[] = {
    0x48, 0x8d, 0x05, 0xf9, 0x01, 0x00, 0x00, 0x48, 0x89, 0x38, 0x48, 0x89,
    0x70, 0x08, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x18, 0x4c, 0x89,
    0x40, 0x20, 0x4c, 0x89, 0x48, 0x28, 0x66, 0x0f, 0xd6, 0x40, 0x30, 0x66,
    0x0f, 0xd6, 0x48, 0x38, 0x66, 0x0f, 0xd6, 0x50, 0x40, 0x66, 0x0f, 0xd6,
    0x58, 0x48, 0x66, 0x0f, 0xd6, 0x60, 0x50, 0x66, 0x0f, 0xd6, 0x68, 0x58,
    0x66, 0x0f, 0xd6, 0x70, 0x60, 0x66, 0x0f, 0xd6, 0x78, 0x68, 0x48, 0x8b,
    0x4c, 0x24, 0x08, 0x48, 0x89, 0x48, 0x70, 0x48, 0x8b, 0x4c, 0x24, 0x10,
    0x48, 0x89, 0x48, 0x78, 0x48, 0x8b, 0x4c, 0x24, 0x18, 0x48, 0x89, 0x88,
    0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x88,
    0x88, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x28, 0x48, 0x89, 0x88,
    0x90, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x30, 0x48, 0x89, 0x88,
    0x98, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x38, 0x48, 0x89, 0x88,
    0xa0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x40, 0x48, 0x89, 0x88,
    0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x48, 0x48, 0x89, 0x88,
    0xb0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x50, 0x48, 0x89, 0x88,
    0xb8, 0x00, 0x00, 0x00, 0x48, 0xff, 0x80, 0xc0, 0x00, 0x00, 0x00, 0x48,
    0x8b, 0x05, 0x3a, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x7e, 0x05, 0x3a, 0x02,
    0x00, 0x00, 0xc3,
};

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
static OcerzVM g_vm;
static OcerzCPU g_cpu;
static uint64_t g_guest;
static volatile uint32_t g_w0;
static int g_native_cmp_calls;

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
    { "B(Hc{b(BhH)}h)", 'B', "Hch", { 1, -1 }, { "b(BhH)", NULL } },
    { "h(bc{H()}Bc{B(bhBHi)})", 'h', "bcBc", { 1, 3 }, { "H()", "B(bhBHi)" } },
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
    "v(b{B(h)})", "v(c{b(c{H()})})", "v(c{B(hv)})", "v(c{h(H)b})",
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

static uint64_t rec_word(int i)
{
    return ocerz_ld(g_guest + REC_OFF + 8ull * (uint64_t)i, 8);
}

static void guest_prime(uint64_t rax, uint64_t xmm0)
{
    memset(ocerz_g2h(g_guest + REC_OFF), 0, 0x100);
    ocerz_st(g_guest + RES_OFF, 8, rax);
    ocerz_st(g_guest + RES_OFF + 8, 8, xmm0);
}

static void *bind_recorder(const char *notation)
{
    void *addr = ocerz_abi_callback_intern(g_guest, notation);

    CHECK(addr != NULL, "interning the guest recorder %#llx as %s returned NULL",
          (unsigned long long)g_guest, notation);
    if (addr)
        note_new(addr, notation);
    return addr;
}

static void expect_word(const char *what, int word, uint64_t want)
{
    uint64_t got = rec_word(word);
    const char *kind = word < REC_XMM ? "integer register"
                     : word < REC_STACK ? "xmm" : "stack slot";
    int index = word < REC_XMM ? word : word < REC_STACK ? word - REC_XMM
                                                         : word - REC_STACK;

    CHECK(got == want, "%s: the guest's %s %d held %#llx, want %#llx", what,
          kind, index, (unsigned long long)got, (unsigned long long)want);
}

static void expect_ran(const char *what)
{
    uint64_t n = rec_word(REC_COUNT);

    CHECK(n == 1, "%s: the guest recorder ran %llu times, want once", what,
          (unsigned long long)n);
}

static int dispatch(void *addr, const uint64_t *x, const uint64_t *v,
                    const uint8_t *stack, uint64_t *ox, uint64_t *ov)
{
    unsigned slot = 0;

    if (!addr || !slot_of(addr, &slot))
        return 0;
    *ox = OUT_POISON;
    *ov = OUT_POISON;
    ocerz_abi_callback_dispatch(slot, x, v, stack, ox, ov);
    return 1;
}

static void narrow_results(void)
{
    static const struct {
        const char *notation;
        uint32_t w0;
        uint64_t x0;
    } kRet[4] = {
        { "b()", 0xffffff80u, 0xffffffffffffff80ull },
        { "B()", 0x00000080u, 0x0000000000000080ull },
        { "h()", 0xffff8080u, 0xffffffffffff8080ull },
        { "H()", 0x00008080u, 0x0000000000008080ull },
    };
    uint64_t x[8], v[8], ox, ov;
    uint8_t stack[16];
    void *addr[4];
    int i;

    for (i = 0; i < 4; i++)
        addr[i] = bind_recorder(kRet[i].notation);

    for (i = 0; i < 4; i++) {
        if (!addr[i])
            continue;
        guest_prime(0xdeadbeefcafe8080ull, 0);
        g_w0 = 0x5a5a5a5au;
        switch (i) {
        case 0: g_w0 = (uint32_t)(int32_t)((int8_t (*)(void))addr[i])(); break;
        case 1: g_w0 = (uint32_t)((uint8_t (*)(void))addr[i])(); break;
        case 2: g_w0 = (uint32_t)(int32_t)((int16_t (*)(void))addr[i])(); break;
        default: g_w0 = (uint32_t)((uint16_t (*)(void))addr[i])(); break;
        }
        expect_ran(kRet[i].notation);
        CHECK(g_w0 == kRet[i].w0,
              "%s: a native caller read w0 %#x from a guest returning "
              "0xdeadbeefcafe8080 in rax, want %#x", kRet[i].notation,
              (unsigned)g_w0, (unsigned)kRet[i].w0);

        memset(x, 0, sizeof x);
        memset(v, 0, sizeof v);
        memset(stack, 0, sizeof stack);
        guest_prime(0xdeadbeefcafe8080ull, 0x4010000000000000ull);
        if (dispatch(addr[i], x, v, stack, &ox, &ov)) {
            expect_ran(kRet[i].notation);
            CHECK(ox == kRet[i].x0 && ov == 0,
                  "%s: dispatching a guest returning 0xdeadbeefcafe8080 left "
                  "x0=%#llx v0=%#llx, want x0=%#llx and v0 zero",
                  kRet[i].notation, (unsigned long long)ox,
                  (unsigned long long)ov, (unsigned long long)kRet[i].x0);
        }
    }
}

static void narrow_registers(void)
{
    static const uint64_t kGarbage[8] = {
        0xdeadbeefcafe12ffull, 0xdeadbeefcafe12ffull, 0xdeadbeefcafe80ffull,
        0xdeadbeefcafe80ffull, 0x0123456789abcd80ull, 0x0123456789abcd80ull,
        0x0123456789ab8f80ull, 0x0123456789ab8f80ull,
    };
    static const uint64_t kGuest[8] = {
        0xffffffffffffffffull, 0xffull, 0xffffffffffff80ffull, 0x80ffull,
        0xffffffffffffff80ull, 0x80ull, 0xffffffffffff8f80ull, 0x8f80ull,
    };
    const char *what;
    uint8_t (*f4)(int8_t, uint8_t, int16_t, uint16_t);
    double (*fd)(int8_t, double, uint8_t, double, int16_t, float, uint16_t,
                 float);
    uint64_t x[8], v[8], ox, ov, bits;
    uint8_t stack[16];
    void *a4, *a8, *afp;
    double dr;
    int i;

    a4 = bind_recorder("B(bBhH)");
    a8 = bind_recorder("l(bBhHbBhH)");
    afp = bind_recorder("d(bdBdhfHf)");

    if (a4) {
        what = "B(bBhH) called natively";
        f4 = (uint8_t (*)(int8_t, uint8_t, int16_t, uint16_t))a4;
        guest_prime(0xdeadbeefcafe1280ull, 0);
        g_w0 = (uint32_t)f4((int8_t)-128, (uint8_t)0x80, (int16_t)-32768,
                            (uint16_t)0x8000);
        expect_ran(what);
        expect_word(what, 0, 0xffffffffffffff80ull);
        expect_word(what, 1, 0x80ull);
        expect_word(what, 2, 0xffffffffffff8000ull);
        expect_word(what, 3, 0x8000ull);
        CHECK(g_w0 == 0x80u,
              "%s: w0 is %#x, want 0x80 from a guest returning "
              "0xdeadbeefcafe1280", what, (unsigned)g_w0);
    }

    if (a8) {
        what = "l(bBhHbBhH) dispatched with garbage above the low bits";
        memcpy(x, kGarbage, sizeof x);
        memset(v, 0, sizeof v);
        memset(stack, 0xa5, sizeof stack);
        guest_prime(0xfedcba9876543210ull, 0);
        if (dispatch(a8, x, v, stack, &ox, &ov)) {
            expect_ran(what);
            for (i = 0; i < 6; i++)
                expect_word(what, i, kGuest[i]);
            expect_word(what, REC_STACK, kGuest[6]);
            expect_word(what, REC_STACK + 1, kGuest[7]);
            CHECK(ox == 0xfedcba9876543210ull,
                  "%s: x0 is %#llx, want the guest's whole rax", what,
                  (unsigned long long)ox);
        }
    }

    if (afp) {
        what = "d(bdBdhfHf) called natively";
        fd = (double (*)(int8_t, double, uint8_t, double, int16_t, float,
                         uint16_t, float))afp;
        guest_prime(0xdeadbeefcafe8080ull, 0x4008800000000000ull);
        dr = fd((int8_t)-1, 1.5, (uint8_t)0xff, -2.25, (int16_t)-1, 0.5f,
                (uint16_t)0xffff, -0.75f);
        memcpy(&bits, &dr, sizeof bits);
        expect_ran(what);
        expect_word(what, 0, 0xffffffffffffffffull);
        expect_word(what, 1, 0xffull);
        expect_word(what, 2, 0xffffffffffffffffull);
        expect_word(what, 3, 0xffffull);
        expect_word(what, REC_XMM + 0, 0x3ff8000000000000ull);
        expect_word(what, REC_XMM + 1, 0xc002000000000000ull);
        expect_word(what, REC_XMM + 2, 0x3f000000ull);
        expect_word(what, REC_XMM + 3, 0xbf400000ull);
        CHECK(bits == 0x4008800000000000ull,
              "%s: returned the bits %#llx, want the guest's xmm0 %#llx", what,
              (unsigned long long)bits, 0x4008800000000000ull);
    }
}

static void narrow_stack(void)
{
    static const int32_t kInt[8] = {
        (int32_t)0x80000001, 2, -3, 0x7ffffffc, (int32_t)0x80000005, 6, -7,
        (int32_t)0xfffffff8,
    };
    static const uint8_t kPacked[16] = {
        0x80, 0xa5, 0x01, 0x80, 0xff, 0xa5, 0xfe, 0xff,
        0x09, 0x00, 0x00, 0x80, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    static const uint64_t kWant16[16] = {
        0xffffffffffffff81ull, 0xffffffffffff8002ull, 0x83ull, 0x8004ull,
        0x05ull, 0x06ull, 0xf7ull, 0xfff8ull,
        0xffffffffffffff89ull, 0x700aull, 0x8bull, 0x800cull,
        0x7full, 0x7fffull, 0xffull, 0xffffull,
    };
    const char *what;
    int64_t (*f13)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                   int32_t, int32_t, int8_t, int16_t, uint8_t, uint16_t,
                   int32_t);
    int16_t (*f16)(int8_t, int16_t, uint8_t, uint16_t, int8_t, int16_t,
                   uint8_t, uint16_t, int8_t, int16_t, uint8_t, uint16_t,
                   int8_t, int16_t, uint8_t, uint16_t);
    uint64_t x[8], v[8], ox, ov;
    uint8_t stack[16];
    void *a13, *a16;
    int64_t r;
    int i;

    a13 = bind_recorder("l(iiiiiiiibhBHi)");
    a16 = bind_recorder("h(bhBHbhBHbhBHbhBH)");

    if (a13) {
        what = "l(iiiiiiiibhBHi) called natively";
        f13 = (int64_t (*)(int32_t, int32_t, int32_t, int32_t, int32_t,
                           int32_t, int32_t, int32_t, int8_t, int16_t,
                           uint8_t, uint16_t, int32_t))a13;
        guest_prime(0x0123456789abcdefull, 0);
        r = f13(kInt[0], kInt[1], kInt[2], kInt[3], kInt[4], kInt[5], kInt[6],
                kInt[7], (int8_t)-2, (int16_t)0x8001, (uint8_t)0xfe,
                (uint16_t)0x8001, (int32_t)0x80000009);
        expect_ran(what);
        for (i = 0; i < 6; i++)
            expect_word(what, i, (uint64_t)(int64_t)kInt[i]);
        expect_word(what, REC_STACK + 0, (uint64_t)(int64_t)kInt[6]);
        expect_word(what, REC_STACK + 1, (uint64_t)(int64_t)kInt[7]);
        expect_word(what, REC_STACK + 2, 0xfffffffffffffffeull);
        expect_word(what, REC_STACK + 3, 0xffffffffffff8001ull);
        expect_word(what, REC_STACK + 4, 0xfeull);
        expect_word(what, REC_STACK + 5, 0x8001ull);
        expect_word(what, REC_STACK + 6, 0xffffffff80000009ull);
        CHECK(r == 0x0123456789abcdefll,
              "%s: returned %#llx, want the guest's whole rax", what,
              (unsigned long long)r);

        what = "l(iiiiiiiibhBHi) dispatched with a hand-packed stack";
        for (i = 0; i < 8; i++)
            x[i] = 0xdeadbeef00000000ull | (uint32_t)kInt[i];
        memset(v, 0, sizeof v);
        memcpy(stack, kPacked, sizeof stack);
        guest_prime(0xdeadbeefcafe1280ull, 0);
        if (dispatch(a13, x, v, stack, &ox, &ov)) {
            expect_ran(what);
            for (i = 0; i < 6; i++)
                expect_word(what, i, (uint64_t)(int64_t)kInt[i]);
            expect_word(what, REC_STACK + 0, (uint64_t)(int64_t)kInt[6]);
            expect_word(what, REC_STACK + 1, (uint64_t)(int64_t)kInt[7]);
            expect_word(what, REC_STACK + 2, 0xffffffffffffff80ull);
            expect_word(what, REC_STACK + 3, 0xffffffffffff8001ull);
            expect_word(what, REC_STACK + 4, 0xffull);
            expect_word(what, REC_STACK + 5, 0xfffeull);
            expect_word(what, REC_STACK + 6, 0xffffffff80000009ull);
            CHECK(ox == 0xdeadbeefcafe1280ull,
                  "%s: x0 is %#llx, want the guest's whole rax", what,
                  (unsigned long long)ox);
        }
    }

    if (a16) {
        what = "h(bhBHbhBHbhBHbhBH) called natively";
        f16 = (int16_t (*)(int8_t, int16_t, uint8_t, uint16_t, int8_t,
                           int16_t, uint8_t, uint16_t, int8_t, int16_t,
                           uint8_t, uint16_t, int8_t, int16_t, uint8_t,
                           uint16_t))a16;
        guest_prime(0xdeadbeefcafe7ffeull, 0);
        g_w0 = (uint32_t)(int32_t)f16((int8_t)0x81, (int16_t)0x8002, 0x83,
                                      0x8004, 5, 6, 0xf7, 0xfff8, (int8_t)0x89,
                                      0x700a, 0x8b, 0x800c, 0x7f, 0x7fff, 0xff,
                                      0xffff);
        expect_ran(what);
        for (i = 0; i < 6; i++)
            expect_word(what, i, kWant16[i]);
        for (i = 6; i < 16; i++)
            expect_word(what, REC_STACK + i - 6, kWant16[i]);
        CHECK(g_w0 == 0x7ffeu,
              "%s: w0 is %#x, want 0x7ffe from a guest returning "
              "0xdeadbeefcafe7ffe", what, (unsigned)g_w0);
    }
}

static void *narrow_thread(void *arg)
{
    OcerzCPU *cpu = ocerz_thread_attach(&g_vm);

    CHECK(cpu != NULL, "attaching a created thread to the test's VM returned "
          "NULL, so no narrow callback reaches guest code");
    if (!cpu)
        return arg;

    narrow_results();
    narrow_registers();
    narrow_stack();

    ocerz_thread_detach();
    return arg;
}

static void test_narrow(void)
{
    pthread_t th;
    int rc;

    rc = pthread_create(&th, NULL, narrow_thread, NULL);
    CHECK(rc == 0, "pthread_create failed with %d, so narrow callbacks are "
          "untested", rc);
    if (rc == 0)
        pthread_join(th, NULL);
}

static int native_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    g_native_cmp_calls++;
    return (x > y) - (x < y);
}

static uint64_t guest_sp(void)
{
    return ((g_guest + GUEST_SPAN - 0x100) & ~0xfull) - 8;
}

static void qsort_crossing(uint64_t comparator, const char *where)
{
    static const int kUnsorted[8] = { 5, -3, 9, 0, 7, -8, 2, 2 };
    static const int kSorted[8] = { -8, -3, 0, 2, 2, 5, 7, 9 };
    OcerzAbiSig sig;
    uint64_t array = g_guest + 0x1000;
    uint64_t sp = guest_sp();
    int r, sorted;

    r = ocerz_abi_parse("v(pLLc{i(pp)})", &sig);
    CHECK(r == OCERZ_OK, "%s: v(pLLc{i(pp)}) does not parse (%d)", where, r);
    if (r != OCERZ_OK)
        return;

    memcpy(ocerz_g2h(array), kUnsorted, sizeof kUnsorted);
    ocerz_cpu_reset(&g_cpu);
    g_cpu.mxcsr = 0x1f80;
    g_cpu.gpr[OCERZ_RDI] = array;
    g_cpu.gpr[OCERZ_RSI] = 8;
    g_cpu.gpr[OCERZ_RDX] = sizeof(int);
    g_cpu.gpr[OCERZ_RCX] = comparator;
    ocerz_st(sp, 8, RET_ADDR);
    g_cpu.gpr[OCERZ_RSP] = sp;

    g_native_cmp_calls = 0;
    r = ocerz_abi_perform(&sig, (const void *)(uintptr_t)qsort, &g_cpu);
    sorted = memcmp(ocerz_g2h(array), kSorted, sizeof kSorted) == 0;
    CHECK(r == OCERZ_OK && g_cpu.rip == RET_ADDR && g_cpu.gpr[OCERZ_RSP] == sp + 8,
          "%s: a bridged qsort given a native comparator returned %d with rip "
          "%#llx rsp %#llx, want OCERZ_OK, %#llx and %#llx", where, r,
          (unsigned long long)g_cpu.rip, (unsigned long long)g_cpu.gpr[OCERZ_RSP],
          (unsigned long long)RET_ADDR, (unsigned long long)(sp + 8));
    CHECK(sorted && g_native_cmp_calls > 0,
          "%s: after a bridged qsort the array is %ssorted and the native "
          "comparator ran %d times; one routed through the bank would have "
          "been refused on this thread and never run", where,
          sorted ? "" : "not ", g_native_cmp_calls);
}

static void test_convert(void)
{
    const uint64_t native = (uint64_t)(uintptr_t)native_cmp;
    const uint64_t cached = (uint64_t)(uintptr_t)strcmp;
    const uint64_t bank = (uint64_t)(uintptr_t)g_cmp_addr;
    const uint64_t guest_fn = g_guest + 0x40;
    OcerzAbiSig sig;
    OcerzAbiCall call;
    uint64_t out, again, cmp[3], want[3];
    void *before, *after, *host_map;
    int r, k;

    CHECK(ocerz_abi_is_guest_code(native) == 0,
          "a static function of the test binary, %#llx, is classed as guest code",
          (unsigned long long)native);
    CHECK(ocerz_abi_is_guest_code(cached) == 0 &&
              ocerz_abi_is_guest_code((uint64_t)(uintptr_t)qsort) == 0,
          "strcmp or qsort, both in the host shared cache, is classed as guest "
          "code");
    CHECK(bank != 0 && ocerz_abi_is_guest_code(bank) == 0,
          "the bank slot %#llx, arm64 code inside the host image, is classed as "
          "guest code", (unsigned long long)bank);
    CHECK(ocerz_abi_is_guest_code(g_guest) == 1 &&
              ocerz_abi_is_guest_code(g_guest + GUEST_SPAN - 1) == 1,
          "the guest mapping [%#llx, %#llx) is not classed as guest code",
          (unsigned long long)g_guest, (unsigned long long)(g_guest + GUEST_SPAN));
    CHECK(ocerz_abi_is_guest_code(ocerz_arena_lo) == 1 &&
              ocerz_abi_is_guest_code(ocerz_arena_hi - 1) == 1,
          "the ends of the guest reservation [%#llx, %#llx) are not classed as "
          "guest code", (unsigned long long)ocerz_arena_lo,
          (unsigned long long)ocerz_arena_hi);
    host_map = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                    -1, 0);
    if (host_map != MAP_FAILED) {
        CHECK(ocerz_abi_is_guest_code((uint64_t)(uintptr_t)host_map) == 1,
              "anonymous memory %p outside the reservation and every host image "
              "is classed as host code, but only the shared cache and dyld's "
              "images are", host_map);
        munmap(host_map, 0x4000);
    }

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(0, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out == 0,
          "converting a null callback returned %d and %#llx, want OCERZ_OK and 0",
          r, (unsigned long long)out);

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(native, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out == native,
          "converting the native function %#llx returned %d and %#llx, want it "
          "back unchanged", (unsigned long long)native, r, (unsigned long long)out);

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(cached, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out == cached,
          "converting strcmp %#llx returned %d and %#llx, want it back unchanged",
          (unsigned long long)cached, r, (unsigned long long)out);

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(bank, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out == bank,
          "converting the bank slot %#llx returned %d and %#llx, want it back "
          "unchanged rather than a trampoline to a trampoline",
          (unsigned long long)bank, r, (unsigned long long)out);

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(guest_fn, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out != 0 && out != guest_fn,
          "converting the guest function %#llx returned %d and %#llx, want "
          "OCERZ_OK and a trampoline", (unsigned long long)guest_fn, r,
          (unsigned long long)out);
    if (r == OCERZ_OK && out && out != guest_fn) {
        note_new(ocerz_g2h(out), "converted guest function");
        again = OUT_POISON;
        r = ocerz_abi_callback_convert(guest_fn, "i(pp)", &again);
        CHECK(r == OCERZ_OK && again == out &&
                  out == ocerz_h2g(ocerz_abi_callback_intern(guest_fn, "i(pp)")),
              "converting %#llx again gave %#llx, want the address it was "
              "interned at, %#llx", (unsigned long long)guest_fn,
              (unsigned long long)again, (unsigned long long)out);
    }

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(guest_fn + 8, "i(pp", &out);
    CHECK(r != OCERZ_OK && out == 0,
          "converting guest code under a malformed notation returned %d and "
          "%#llx, want a refusal and 0", r, (unsigned long long)out);
    CHECK(ocerz_abi_callback_convert(native, "i(pp)", NULL) != OCERZ_OK,
          "converting with nowhere to put the result succeeded");

    before = ocerz_abi_callback_intern(FN_ADJ_1, "i(pp)");
    if (before)
        note_new(before, "slot before the native qsort");

    r = ocerz_abi_parse("v(pLLc{i(pp)})", &sig);
    CHECK(r == OCERZ_OK, "v(pLLc{i(pp)}) does not parse (%d)", r);
    if (r == OCERZ_OK) {
        cmp[0] = native;
        cmp[1] = 0;
        cmp[2] = guest_fn;
        want[0] = native;
        want[1] = 0;
        want[2] = (uint64_t)(uintptr_t)ocerz_abi_callback_intern(guest_fn, "i(pp)");
        for (k = 0; k < 3; k++) {
            ocerz_cpu_reset(&g_cpu);
            g_cpu.gpr[OCERZ_RDI] = g_guest + 0x1000;
            g_cpu.gpr[OCERZ_RSI] = 8;
            g_cpu.gpr[OCERZ_RDX] = sizeof(int);
            g_cpu.gpr[OCERZ_RCX] = cmp[k];
            g_cpu.gpr[OCERZ_RSP] = guest_sp();
            memset(&call, 0x5a, sizeof call);
            r = ocerz_abi_read_guest(&sig, &g_cpu, &call);
            CHECK(r == OCERZ_OK && call.x[3] == want[k],
                  "v(pLLc{i(pp)}) with %#llx in rcx gave x3 %#llx (%d), want "
                  "%#llx", (unsigned long long)cmp[k],
                  (unsigned long long)call.x[3], r, (unsigned long long)want[k]);
        }
    }

    qsort_crossing(native, "with a free bank");

    after = ocerz_abi_callback_intern(FN_ADJ_2, "i(pp)");
    if (after)
        note_new(after, "slot after the native qsort");
    CHECK(before && after &&
              (uintptr_t)after == (uintptr_t)before + BANK_STRIDE,
          "the slots interned either side of a bridged qsort with a native "
          "comparator are %p and %p, want adjacent; the crossing consumed a slot",
          before, after);
}

static void test_convert_full(void)
{
    const uint64_t native = (uint64_t)(uintptr_t)native_cmp;
    uint64_t out;
    int r;

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(g_guest + 0x80, "i(pp)", &out);
    CHECK(r != OCERZ_OK && out == 0,
          "converting new guest code with the bank full returned %d and %#llx, "
          "want a refusal and 0", r, (unsigned long long)out);

    out = OUT_POISON;
    r = ocerz_abi_callback_convert(native, "i(pp)", &out);
    CHECK(r == OCERZ_OK && out == native,
          "converting a native function with the bank full returned %d and "
          "%#llx, want it back unchanged", r, (unsigned long long)out);

    qsort_crossing(native, "with the bank full");
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
    g_guest = ocerz_map_anywhere(GUEST_SPAN, PROT_READ | PROT_WRITE);
    if (g_guest == 0) {
        fprintf(stderr, "guest page alloc failed\n");
        return 2;
    }
    memcpy(ocerz_g2h(g_guest), kRecorder, sizeof kRecorder);
    if (ocerz_vm_init(&g_vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    g_vm.jit_enabled = 0;
    g_vm.jit_plain_mem = 1;

    test_bank();
    test_parse();
    test_intern();
    test_refusal();
    test_convert();
    test_narrow();
    test_exhaustion();
    test_convert_full();

    return report();
}
