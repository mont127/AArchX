/*
 * The way back from native code into guest code, and the question of whether a
 * function pointer needs to take it at all.
 *
 * Five things are asserted before any guest code runs.  The bank:
 * ocerz_abi_callback_bank to ocerz_abi_callback_bank_end is exactly 4096 slots
 * of eight bytes, which is what lets a slot find its own index from its own
 * address.  The notation: an argument of class c carries its callback's own
 * signature in braces, stored against that argument's index with every other
 * entry left empty, a nested signature may use the narrow classes and
 * structures, whose own braces the callback's are matched past, and each way of
 * getting that wrong is refused - c as a result, c with no braces, a brace
 * never closed, a nested signature that itself names a callback, one that does
 * not parse, a malformed structure inside one, and one too long for
 * OCERZ_ABI_CB_MAX.  Interning: one guest
 * function under one signature is one address, whichever string the signature
 * arrives in and whatever was interned in between; a different function or a
 * different signature is a different address; every address is a slot of the
 * bank on an eight-byte boundary; and a notation that is malformed or names a
 * callback gets NULL.  Refusal: a slot reached on a thread with no guest cpu
 * returns zero in every result word, checked through the dispatcher with
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
 * Narrow and structure callbacks are where this file runs guest code, because
 * the only evidence that a value crossed correctly is what the guest's
 * registers held and what the native caller read back.  The narrow guest
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
 * Structures take a second recorder, 156 bytes assembled the same way, because a
 * structure result needs rdx and xmm1 as well as rax and xmm0, and a MEMORY
 * result needs the guest to write through rdi.  It records the same registers
 * and sixteen eightbytes above its return address, then either loads rax, rdx,
 * xmm0 and xmm1 from its result block or, when the block holds a byte count,
 * copies that many bytes through rdi and returns rdi in rax, asking the
 * interpreter for nothing beyond mov, movq, movb, lea, xor, inc, cmp, test, two
 * conditional branches and ret.  Thirty-four signatures are bound to it and each
 * is called through a generated C function pointer of the matching prototype,
 * so clang's arm64 caller decides where every structure goes - x or v
 * registers, Apple's packed stack, a copy the caller owns, a buffer in x8 - and
 * the recorder's words are compared with where this file's own System V
 * classification says the guest should find each value, a structure's bytes
 * past its end being zero.  The result block is primed with a structure whose
 * members all differ, with garbage past its end in its last register word, and
 * what the native caller receives is compared member by member.  The
 * signatures cover every result class - two INTEGER or two SSE eightbytes, the
 * mixed {df}, {dL} and {Ld}, aggregates returned in d0 to d3 from x86 registers
 * and from x86 MEMORY, structures returned through x8 including CATransform3D's
 * sixteen doubles - and the spill mixes test_abi.c runs forwards.  Each
 * structure is a real C type, so its layout is clang's.
 *
 * Hand-built dispatches cover what a well-behaved caller never shows.
 * {B}({B}{h}{fi}) with 0xdeadbeefcafe12b1 in x0 must give the guest rdi 0xb1,
 * and a guest leaving 0xdeadbeefcafe12c3 in rax must give the native side x0
 * 0xc3 and x1 zero.  {fff}(dddddd{fff}f) and {dddd}(ddddddd{dddd}d) put junk in
 * the v registers left after an aggregate spills, which must be ignored because
 * the spill closed them, and {LL}(LLLLLLL{LL}L) does the same with x7.
 * {LLL}({LLL}) writes its result through an x8 buffer and not a byte past it,
 * gives the guest result space just below the dispatcher's stack top, and is
 * refused with nothing run and every result word zero when x8 is null or the
 * copy's address is null; a spilled structure with no stack passed is refused
 * the same way.  {pdi}({pdi}) carries a pointer member through a copy and back
 * through x8.
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
 * The too-long refusal is isolated by a structure notation.  OCERZ_ABI_CB_MAX
 * is 48 bytes, and a signature of scalars that long would also have too many
 * arguments, but L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f{ff}ff) is 48
 * characters with fourteen arguments and parses on its own, so its refusal as a
 * nested notation is the length rule and nothing else, while the same notation
 * one argument shorter is accepted and stored whole.  The scalar ones are still
 * asserted refused, without saying which rule refused them.
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
    { "v(c{v({dd})})", 'v', "c", { 0, -1 }, { "v({dd})", NULL } },
    { "{dd}(c{{{dd}{dd}}(pp{{dd}{dd}}p)}{dd})", '{', "c{", { 0, -1 },
      { "{{dd}{dd}}(pp{{dd}{dd}}p)", NULL } },
    { "v(pc{L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f{ff}f)}c{{LL}()})", 'v', "pcc",
      { 1, 2 }, { "L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f{ff}f)", "{LL}()" } },
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
    "v(c{v({dd)})", "v(c{v({})})", "v(c{v({c})})", "v(c{{v}()})", "v(c{v({dd}})})",
    "v(c{v({dd})}", "v({c{v()}})", "v(c{v({{dd})})", "v(c{{dd}(c{v()})})",
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
    static const char kStructLong[] = "L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f{ff}ff)";
    OcerzAbiSig sig;
    char longer[128];
    size_t i, k, n;

    for (i = 0; i < NACCEPT; i++)
        check_accept(&kAccept[i]);

    for (i = 0; i < NREJECT; i++)
        check_reject(kReject[i], "a callback notation that is not well formed");

    CHECK(ocerz_abi_parse(kStructLong, &sig) == OCERZ_OK,
          "%s does not parse on its own, so its refusal as a nested notation proves "
          "nothing about length", kStructLong);
    memcpy(longer, "v(c{", 4);
    memcpy(longer + 4, kStructLong, strlen(kStructLong));
    memcpy(longer + 4 + strlen(kStructLong), "})", 3);
    check_reject(longer, "a nested notation of 48 characters and 14 arguments");

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

static void set_words(uint64_t *w, int n, uint64_t val)
{
    int i;

    for (i = 0; i < n; i++)
        w[i] = val;
}

static int words_zero(const uint64_t *w, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (w[i])
            return 0;
    return 1;
}

static void refuse_here(const char *where)
{
    static const int ka = 1, kb = 2;
    uint64_t x[8], v[8], ox[2], ov[4], bits;
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
        set_words(ox, 2, OUT_POISON);
        set_words(ov, 4, OUT_POISON);
        ocerz_abi_callback_dispatch(slot, x, v, stack, NULL, ox, ov);
        CHECK(words_zero(ox, 2) && words_zero(ov, 4),
              "%s: dispatching the i(pp) slot %u with no guest cpu left "
              "x0=%#llx x1=%#llx d0=%#llx d3=%#llx, want every result word "
              "zero", where, slot, (unsigned long long)ox[0],
              (unsigned long long)ox[1], (unsigned long long)ov[0],
              (unsigned long long)ov[3]);

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
        set_words(ox, 2, OUT_POISON);
        set_words(ov, 4, OUT_POISON);
        ocerz_abi_callback_dispatch(slot, x, v, stack, NULL, ox, ov);
        CHECK(words_zero(ox, 2) && words_zero(ov, 4),
              "%s: dispatching the d(dd) slot %u with no guest cpu left "
              "x0=%#llx x1=%#llx d0=%#llx d3=%#llx, want every result word "
              "zero", where, slot, (unsigned long long)ox[0],
              (unsigned long long)ox[1], (unsigned long long)ov[0],
              (unsigned long long)ov[3]);

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
    set_words(ox, 2, OUT_POISON);
    set_words(ov, 4, OUT_POISON);
    ocerz_abi_callback_dispatch(slot, x, v, stack, NULL, ox, ov);
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
    uint64_t x[8], v[8], ox[2], ov[4];
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
        if (dispatch(addr[i], x, v, stack, ox, ov)) {
            expect_ran(kRet[i].notation);
            CHECK(ox[0] == kRet[i].x0 && ox[1] == 0 && words_zero(ov, 4),
                  "%s: dispatching a guest returning 0xdeadbeefcafe8080 left "
                  "x0=%#llx x1=%#llx d0=%#llx, want x0=%#llx and the rest "
                  "zero", kRet[i].notation, (unsigned long long)ox[0],
                  (unsigned long long)ox[1], (unsigned long long)ov[0],
                  (unsigned long long)kRet[i].x0);
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
    uint64_t x[8], v[8], ox[2], ov[4], bits;
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
        if (dispatch(a8, x, v, stack, ox, ov)) {
            expect_ran(what);
            for (i = 0; i < 6; i++)
                expect_word(what, i, kGuest[i]);
            expect_word(what, REC_STACK, kGuest[6]);
            expect_word(what, REC_STACK + 1, kGuest[7]);
            CHECK(ox[0] == 0xfedcba9876543210ull,
                  "%s: x0 is %#llx, want the guest's whole rax", what,
                  (unsigned long long)ox[0]);
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
    uint64_t x[8], v[8], ox[2], ov[4];
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
        if (dispatch(a13, x, v, stack, ox, ov)) {
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
            CHECK(ox[0] == 0xdeadbeefcafe1280ull,
                  "%s: x0 is %#llx, want the guest's whole rax", what,
                  (unsigned long long)ox[0]);
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

#define SREC_OFF    0x2000ull
#define SREC_REC    (SREC_OFF + 0x400ull)
#define SREC_RES    (SREC_OFF + 0x600ull)
#define SREC_XMM    6
#define SREC_STACK  14
#define SREC_SLOTS  16
#define SREC_COUNT  30
#define SREC_MEM    0x28ull
#define CB_RES_ARG  20

typedef struct StType {
    const char *notation;
    size_t size;
    size_t align;
    int n;
    const char *cls;
    uint8_t off[OCERZ_ABI_STRUCT_MEMBERS];
} StType;

typedef struct CbStCase {
    const char *sig;
    void (*call)(void *addr);
    char ret;
    int ret_type;
    int nargs;
    char arg[OCERZ_ABI_MAX_ARGS];
    int type[OCERZ_ABI_MAX_ARGS];
} CbStCase;

static const uint8_t kStructRecorder[] = {
    0x48, 0x8d, 0x05, 0xf9, 0x03, 0x00, 0x00, 0x48, 0x89, 0x38, 0x48, 0x89,
    0x70, 0x08, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x18, 0x4c, 0x89,
    0x40, 0x20, 0x4c, 0x89, 0x48, 0x28, 0x66, 0x0f, 0xd6, 0x40, 0x30, 0x66,
    0x0f, 0xd6, 0x48, 0x38, 0x66, 0x0f, 0xd6, 0x50, 0x40, 0x66, 0x0f, 0xd6,
    0x58, 0x48, 0x66, 0x0f, 0xd6, 0x60, 0x50, 0x66, 0x0f, 0xd6, 0x68, 0x58,
    0x66, 0x0f, 0xd6, 0x70, 0x60, 0x66, 0x0f, 0xd6, 0x78, 0x68, 0x31, 0xc9,
    0x4c, 0x8b, 0x54, 0xcc, 0x08, 0x4c, 0x89, 0x54, 0xc8, 0x70, 0x48, 0xff,
    0xc1, 0x48, 0x83, 0xf9, 0x10, 0x72, 0xed, 0x48, 0xff, 0x80, 0xf0, 0x00,
    0x00, 0x00, 0x48, 0x8d, 0x0d, 0x97, 0x05, 0x00, 0x00, 0x4c, 0x8b, 0x41,
    0x20, 0x4d, 0x85, 0xc0, 0x74, 0x18, 0x45, 0x31, 0xc9, 0x46, 0x8a, 0x54,
    0x09, 0x28, 0x46, 0x88, 0x14, 0x0f, 0x49, 0xff, 0xc1, 0x4d, 0x39, 0xc1,
    0x72, 0xef, 0x48, 0x89, 0xf8, 0xc3, 0x48, 0x8b, 0x01, 0x48, 0x8b, 0x51,
    0x08, 0xf3, 0x0f, 0x7e, 0x41, 0x10, 0xf3, 0x0f, 0x7e, 0x49, 0x18, 0xc3,
};

typedef struct { uint64_t m0; uint64_t m1; } S_oLLc;
typedef struct { int32_t m0; uint64_t m1; } S_oiLc;
typedef struct { uint64_t m0; int32_t m1; } S_oLic;
typedef struct { double m0; double m1; } S_oddc;
typedef struct { float m0; float m1; } S_offc;
typedef struct { float m0; float m1; float m2; } S_offfc;
typedef struct { float m0; float m1; float m2; float m3; } S_offffc;
typedef struct { float m0; float m1; float m2; float m3; float m4; } S_offfffc;
typedef struct { struct { double m0; double m1; } m0; struct { double m0; double m1; } m1; } S_ooddcoddcc;
typedef struct { uint64_t m0; uint64_t m1; uint64_t m2; } S_oLLLc;
typedef struct { double m0; float m1; } S_odfc;
typedef struct { double m0; uint64_t m1; } S_odLc;
typedef struct { uint64_t m0; double m1; } S_oLdc;
typedef struct { int8_t m0; uint8_t m1; int16_t m2; uint16_t m3; int32_t m4; uint32_t m5; } S_obBhHiuc;
typedef struct { int8_t m0; int16_t m1; int8_t m2; } S_obhbc;
typedef struct { void *m0; double m1; int32_t m2; } S_opdic;
typedef struct { uint8_t m0; } S_oBc;
typedef struct { int16_t m0; } S_ohc;
typedef struct { int32_t m0; } S_oic;
typedef struct { uint64_t m0; } S_oLc;
typedef struct { void *m0; } S_opc;
typedef struct { float m0; } S_ofc;
typedef struct { double m0; } S_odc;
typedef struct { float m0; int32_t m1; } S_ofic;
typedef struct { uint8_t m0; uint8_t m1; uint8_t m2; } S_oBBBc;
typedef struct { double m0; double m1; double m2; double m3; } S_oddddc;
typedef struct { double m0; double m1; double m2; } S_odddc;
typedef struct { struct { double m0; int8_t m1; } m0; int8_t m1; } S_oodbcbc;
typedef struct { double m0; double m1; double m2; double m3; double m4; double m5; } S_oddddddc;
typedef struct { int8_t m0; double m1; } S_obdc;
typedef struct { float m0; double m1; } S_ofdc;
typedef struct { int8_t m0; float m1; } S_obfc;
typedef struct { struct { float m0; float m1; } m0; float m1; } S_ooffcfc;
typedef struct { struct { float m0; } m0; struct { float m0; } m1; } S_oofcofcc;
typedef struct { struct { struct { double m0; } m0; } m0; } S_ooodccc;
typedef struct { int64_t m0; uint16_t m1; } S_olHc;
typedef struct { double m0; double m1; double m2; double m3; double m4; double m5; double m6; double m7; double m8; double m9; double m10; double m11; double m12; double m13; double m14; double m15; } S_oddddddddddddddddc;

enum {
    ST_oLLc,
    ST_oiLc,
    ST_oLic,
    ST_oddc,
    ST_offc,
    ST_offfc,
    ST_offffc,
    ST_offfffc,
    ST_ooddcoddcc,
    ST_oLLLc,
    ST_odfc,
    ST_odLc,
    ST_oLdc,
    ST_obBhHiuc,
    ST_obhbc,
    ST_opdic,
    ST_oBc,
    ST_ohc,
    ST_oic,
    ST_oLc,
    ST_opc,
    ST_ofc,
    ST_odc,
    ST_ofic,
    ST_oBBBc,
    ST_oddddc,
    ST_odddc,
    ST_oodbcbc,
    ST_oddddddc,
    ST_obdc,
    ST_ofdc,
    ST_obfc,
    ST_ooffcfc,
    ST_oofcofcc,
    ST_ooodccc,
    ST_olHc,
    ST_oddddddddddddddddc,
    ST_COUNT
};

static const StType kStTypes[ST_COUNT] = {
    { "{LL}", sizeof(S_oLLc), _Alignof(S_oLLc), 2, "LL",
      { offsetof(S_oLLc, m0), offsetof(S_oLLc, m1) } },
    { "{iL}", sizeof(S_oiLc), _Alignof(S_oiLc), 2, "iL",
      { offsetof(S_oiLc, m0), offsetof(S_oiLc, m1) } },
    { "{Li}", sizeof(S_oLic), _Alignof(S_oLic), 2, "Li",
      { offsetof(S_oLic, m0), offsetof(S_oLic, m1) } },
    { "{dd}", sizeof(S_oddc), _Alignof(S_oddc), 2, "dd",
      { offsetof(S_oddc, m0), offsetof(S_oddc, m1) } },
    { "{ff}", sizeof(S_offc), _Alignof(S_offc), 2, "ff",
      { offsetof(S_offc, m0), offsetof(S_offc, m1) } },
    { "{fff}", sizeof(S_offfc), _Alignof(S_offfc), 3, "fff",
      { offsetof(S_offfc, m0), offsetof(S_offfc, m1), offsetof(S_offfc, m2) } },
    { "{ffff}", sizeof(S_offffc), _Alignof(S_offffc), 4, "ffff",
      { offsetof(S_offffc, m0), offsetof(S_offffc, m1), offsetof(S_offffc, m2), offsetof(S_offffc, m3) } },
    { "{fffff}", sizeof(S_offfffc), _Alignof(S_offfffc), 5, "fffff",
      { offsetof(S_offfffc, m0), offsetof(S_offfffc, m1), offsetof(S_offfffc, m2), offsetof(S_offfffc, m3), offsetof(S_offfffc, m4) } },
    { "{{dd}{dd}}", sizeof(S_ooddcoddcc), _Alignof(S_ooddcoddcc), 4, "dddd",
      { offsetof(S_ooddcoddcc, m0.m0), offsetof(S_ooddcoddcc, m0.m1), offsetof(S_ooddcoddcc, m1.m0), offsetof(S_ooddcoddcc, m1.m1) } },
    { "{LLL}", sizeof(S_oLLLc), _Alignof(S_oLLLc), 3, "LLL",
      { offsetof(S_oLLLc, m0), offsetof(S_oLLLc, m1), offsetof(S_oLLLc, m2) } },
    { "{df}", sizeof(S_odfc), _Alignof(S_odfc), 2, "df",
      { offsetof(S_odfc, m0), offsetof(S_odfc, m1) } },
    { "{dL}", sizeof(S_odLc), _Alignof(S_odLc), 2, "dL",
      { offsetof(S_odLc, m0), offsetof(S_odLc, m1) } },
    { "{Ld}", sizeof(S_oLdc), _Alignof(S_oLdc), 2, "Ld",
      { offsetof(S_oLdc, m0), offsetof(S_oLdc, m1) } },
    { "{bBhHiu}", sizeof(S_obBhHiuc), _Alignof(S_obBhHiuc), 6, "bBhHiu",
      { offsetof(S_obBhHiuc, m0), offsetof(S_obBhHiuc, m1), offsetof(S_obBhHiuc, m2), offsetof(S_obBhHiuc, m3), offsetof(S_obBhHiuc, m4), offsetof(S_obBhHiuc, m5) } },
    { "{bhb}", sizeof(S_obhbc), _Alignof(S_obhbc), 3, "bhb",
      { offsetof(S_obhbc, m0), offsetof(S_obhbc, m1), offsetof(S_obhbc, m2) } },
    { "{pdi}", sizeof(S_opdic), _Alignof(S_opdic), 3, "pdi",
      { offsetof(S_opdic, m0), offsetof(S_opdic, m1), offsetof(S_opdic, m2) } },
    { "{B}", sizeof(S_oBc), _Alignof(S_oBc), 1, "B",
      { offsetof(S_oBc, m0) } },
    { "{h}", sizeof(S_ohc), _Alignof(S_ohc), 1, "h",
      { offsetof(S_ohc, m0) } },
    { "{i}", sizeof(S_oic), _Alignof(S_oic), 1, "i",
      { offsetof(S_oic, m0) } },
    { "{L}", sizeof(S_oLc), _Alignof(S_oLc), 1, "L",
      { offsetof(S_oLc, m0) } },
    { "{p}", sizeof(S_opc), _Alignof(S_opc), 1, "p",
      { offsetof(S_opc, m0) } },
    { "{f}", sizeof(S_ofc), _Alignof(S_ofc), 1, "f",
      { offsetof(S_ofc, m0) } },
    { "{d}", sizeof(S_odc), _Alignof(S_odc), 1, "d",
      { offsetof(S_odc, m0) } },
    { "{fi}", sizeof(S_ofic), _Alignof(S_ofic), 2, "fi",
      { offsetof(S_ofic, m0), offsetof(S_ofic, m1) } },
    { "{BBB}", sizeof(S_oBBBc), _Alignof(S_oBBBc), 3, "BBB",
      { offsetof(S_oBBBc, m0), offsetof(S_oBBBc, m1), offsetof(S_oBBBc, m2) } },
    { "{dddd}", sizeof(S_oddddc), _Alignof(S_oddddc), 4, "dddd",
      { offsetof(S_oddddc, m0), offsetof(S_oddddc, m1), offsetof(S_oddddc, m2), offsetof(S_oddddc, m3) } },
    { "{ddd}", sizeof(S_odddc), _Alignof(S_odddc), 3, "ddd",
      { offsetof(S_odddc, m0), offsetof(S_odddc, m1), offsetof(S_odddc, m2) } },
    { "{{db}b}", sizeof(S_oodbcbc), _Alignof(S_oodbcbc), 3, "dbb",
      { offsetof(S_oodbcbc, m0.m0), offsetof(S_oodbcbc, m0.m1), offsetof(S_oodbcbc, m1) } },
    { "{dddddd}", sizeof(S_oddddddc), _Alignof(S_oddddddc), 6, "dddddd",
      { offsetof(S_oddddddc, m0), offsetof(S_oddddddc, m1), offsetof(S_oddddddc, m2), offsetof(S_oddddddc, m3), offsetof(S_oddddddc, m4), offsetof(S_oddddddc, m5) } },
    { "{bd}", sizeof(S_obdc), _Alignof(S_obdc), 2, "bd",
      { offsetof(S_obdc, m0), offsetof(S_obdc, m1) } },
    { "{fd}", sizeof(S_ofdc), _Alignof(S_ofdc), 2, "fd",
      { offsetof(S_ofdc, m0), offsetof(S_ofdc, m1) } },
    { "{bf}", sizeof(S_obfc), _Alignof(S_obfc), 2, "bf",
      { offsetof(S_obfc, m0), offsetof(S_obfc, m1) } },
    { "{{ff}f}", sizeof(S_ooffcfc), _Alignof(S_ooffcfc), 3, "fff",
      { offsetof(S_ooffcfc, m0.m0), offsetof(S_ooffcfc, m0.m1), offsetof(S_ooffcfc, m1) } },
    { "{{f}{f}}", sizeof(S_oofcofcc), _Alignof(S_oofcofcc), 2, "ff",
      { offsetof(S_oofcofcc, m0.m0), offsetof(S_oofcofcc, m1.m0) } },
    { "{{{d}}}", sizeof(S_ooodccc), _Alignof(S_ooodccc), 1, "d",
      { offsetof(S_ooodccc, m0.m0.m0) } },
    { "{lH}", sizeof(S_olHc), _Alignof(S_olHc), 2, "lH",
      { offsetof(S_olHc, m0), offsetof(S_olHc, m1) } },
    { "{dddddddddddddddd}", sizeof(S_oddddddddddddddddc), _Alignof(S_oddddddddddddddddc), 16, "dddddddddddddddd",
      { offsetof(S_oddddddddddddddddc, m0), offsetof(S_oddddddddddddddddc, m1), offsetof(S_oddddddddddddddddc, m2), offsetof(S_oddddddddddddddddc, m3), offsetof(S_oddddddddddddddddc, m4), offsetof(S_oddddddddddddddddc, m5), offsetof(S_oddddddddddddddddc, m6), offsetof(S_oddddddddddddddddc, m7), offsetof(S_oddddddddddddddddc, m8), offsetof(S_oddddddddddddddddc, m9), offsetof(S_oddddddddddddddddc, m10), offsetof(S_oddddddddddddddddc, m11), offsetof(S_oddddddddddddddddc, m12), offsetof(S_oddddddddddddddddc, m13), offsetof(S_oddddddddddddddddc, m14), offsetof(S_oddddddddddddddddc, m15) } },
};

static uint8_t g_cb_out[OCERZ_ABI_STRUCT_BYTES];

static size_t cls_size(char c)
{
    switch (c) {
    case 'b':
    case 'B': return 1;
    case 'h':
    case 'H': return 2;
    case 'i':
    case 'u':
    case 'f': return 4;
    default:  return 8;
    }
}

static int is_fp(char c)
{
    return c == 'f' || c == 'd';
}

static uint64_t cb_member_bits(char c, int arg, int m)
{
    int k = arg * 16 + m;

    switch (c) {
    case 'f': return 0x3fa00000u + (uint32_t)k * 0x1001u;
    case 'd': return 0x3ff4000000000000ull + (uint64_t)k * 0x0000010000000001ull;
    case 'b':
    case 'B': return (uint8_t)(0x81 + 7 * k);
    case 'h':
    case 'H': return (uint16_t)(0x8123 + 0x301 * k);
    case 'i':
    case 'u': return (uint32_t)(0x80001234u + 0x10101u * (uint32_t)k);
    case 'p': return g_guest + 0x3000ull + 8ull * (uint64_t)k;
    default:  return 0xc0de000000000000ull + (uint64_t)k * 0x0000000100010001ull;
    }
}

static void cb_build(void *p, int type, int arg)
{
    const StType *t = &kStTypes[type];
    int m;

    memset(p, 0, t->size);
    for (m = 0; m < t->n; m++) {
        uint64_t bits = cb_member_bits(t->cls[m], arg, m);
        memcpy((uint8_t *)p + t->off[m], &bits, cls_size(t->cls[m]));
    }
}

static uint64_t cb_scalar(char c, int i)
{
    return cb_member_bits(c, i, 15);
}

static float cb_float(int i)
{
    uint32_t b = (uint32_t)cb_scalar('f', i);
    float f;

    memcpy(&f, &b, sizeof f);
    return f;
}

static double cb_double(int i)
{
    uint64_t b = cb_scalar('d', i);
    double d;

    memcpy(&d, &b, sizeof d);
    return d;
}

static uint64_t cb_guest_scalar(char c, uint64_t bits)
{
    switch (c) {
    case 'b': return (uint64_t)(int64_t)(int8_t)bits;
    case 'B': return (uint8_t)bits;
    case 'h': return (uint64_t)(int64_t)(int16_t)bits;
    case 'H': return (uint16_t)bits;
    case 'i': return (uint64_t)(int64_t)(int32_t)bits;
    case 'u':
    case 'f': return (uint32_t)bits;
    default:  return bits;
    }
}

static int st_sysv_class(const StType *t, char cls[2])
{
    int n, m;

    if (t->size > 16)
        return 0;
    n = (int)((t->size + 7) / 8);
    cls[0] = 'S';
    cls[1] = 'S';
    for (m = 0; m < t->n; m++)
        if (!is_fp(t->cls[m]))
            cls[t->off[m] / 8] = 'I';
    return n;
}

static void scall_0(void *addr)
{
    S_oLLc (*fn)(S_oLLc) = (S_oLLc (*)(S_oLLc))addr;
    S_oLLc a0;
    S_oLLc r;

    cb_build(&a0, ST_oLLc, 0);
    r = fn(a0);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_1(void *addr)
{
    S_oiLc (*fn)(int8_t, S_oiLc, int16_t) = (S_oiLc (*)(int8_t, S_oiLc, int16_t))addr;
    S_oiLc a1;
    S_oiLc r;

    cb_build(&a1, ST_oiLc, 1);
    r = fn((int8_t)cb_scalar('b', 0), a1, (int16_t)cb_scalar('h', 2));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_2(void *addr)
{
    S_oLic (*fn)(S_oLic, uint64_t) = (S_oLic (*)(S_oLic, uint64_t))addr;
    S_oLic a0;
    S_oLic r;

    cb_build(&a0, ST_oLic, 0);
    r = fn(a0, (uint64_t)cb_scalar('L', 1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_3(void *addr)
{
    S_oddc (*fn)(S_oddc, double) = (S_oddc (*)(S_oddc, double))addr;
    S_oddc a0;
    S_oddc r;

    cb_build(&a0, ST_oddc, 0);
    r = fn(a0, cb_double(1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_4(void *addr)
{
    S_offc (*fn)(S_offc, S_offfc) = (S_offc (*)(S_offc, S_offfc))addr;
    S_offc a0;
    S_offfc a1;
    S_offc r;

    cb_build(&a0, ST_offc, 0);
    cb_build(&a1, ST_offfc, 1);
    r = fn(a0, a1);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_5(void *addr)
{
    S_offffc (*fn)(S_offffc) = (S_offffc (*)(S_offffc))addr;
    S_offffc a0;
    S_offffc r;

    cb_build(&a0, ST_offffc, 0);
    r = fn(a0);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_6(void *addr)
{
    S_offfffc (*fn)(S_offfffc, uint64_t) = (S_offfffc (*)(S_offfffc, uint64_t))addr;
    S_offfffc a0;
    S_offfffc r;

    cb_build(&a0, ST_offfffc, 0);
    r = fn(a0, (uint64_t)cb_scalar('L', 1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_7(void *addr)
{
    S_ooddcoddcc (*fn)(void *, void *, S_ooddcoddcc, void *) = (S_ooddcoddcc (*)(void *, void *, S_ooddcoddcc, void *))addr;
    S_ooddcoddcc a2;
    S_ooddcoddcc r;

    cb_build(&a2, ST_ooddcoddcc, 2);
    r = fn((void *)(uintptr_t)cb_scalar('p', 0), (void *)(uintptr_t)cb_scalar('p', 1), a2, (void *)(uintptr_t)cb_scalar('p', 3));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_8(void *addr)
{
    S_oLLLc (*fn)(S_oLLLc, S_oLLLc) = (S_oLLLc (*)(S_oLLLc, S_oLLLc))addr;
    S_oLLLc a0;
    S_oLLLc a1;
    S_oLLLc r;

    cb_build(&a0, ST_oLLLc, 0);
    cb_build(&a1, ST_oLLLc, 1);
    r = fn(a0, a1);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_9(void *addr)
{
    S_odfc (*fn)(S_odfc, S_odLc, S_oLdc) = (S_odfc (*)(S_odfc, S_odLc, S_oLdc))addr;
    S_odfc a0;
    S_odLc a1;
    S_oLdc a2;
    S_odfc r;

    cb_build(&a0, ST_odfc, 0);
    cb_build(&a1, ST_odLc, 1);
    cb_build(&a2, ST_oLdc, 2);
    r = fn(a0, a1, a2);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_10(void *addr)
{
    S_odLc (*fn)(S_odLc, double) = (S_odLc (*)(S_odLc, double))addr;
    S_odLc a0;
    S_odLc r;

    cb_build(&a0, ST_odLc, 0);
    r = fn(a0, cb_double(1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_11(void *addr)
{
    S_oLdc (*fn)(uint64_t, S_oLdc) = (S_oLdc (*)(uint64_t, S_oLdc))addr;
    S_oLdc a1;
    S_oLdc r;

    cb_build(&a1, ST_oLdc, 1);
    r = fn((uint64_t)cb_scalar('L', 0), a1);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_12(void *addr)
{
    S_obBhHiuc (*fn)(S_obBhHiuc, S_obhbc) = (S_obBhHiuc (*)(S_obBhHiuc, S_obhbc))addr;
    S_obBhHiuc a0;
    S_obhbc a1;
    S_obBhHiuc r;

    cb_build(&a0, ST_obBhHiuc, 0);
    cb_build(&a1, ST_obhbc, 1);
    r = fn(a0, a1);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_13(void *addr)
{
    S_opdic (*fn)(S_opdic, void *) = (S_opdic (*)(S_opdic, void *))addr;
    S_opdic a0;
    S_opdic r;

    cb_build(&a0, ST_opdic, 0);
    r = fn(a0, (void *)(uintptr_t)cb_scalar('p', 1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_14(void *addr)
{
    S_oBc (*fn)(S_oBc, S_ohc, S_oic, S_oLc, S_opc) = (S_oBc (*)(S_oBc, S_ohc, S_oic, S_oLc, S_opc))addr;
    S_oBc a0;
    S_ohc a1;
    S_oic a2;
    S_oLc a3;
    S_opc a4;
    S_oBc r;

    cb_build(&a0, ST_oBc, 0);
    cb_build(&a1, ST_ohc, 1);
    cb_build(&a2, ST_oic, 2);
    cb_build(&a3, ST_oLc, 3);
    cb_build(&a4, ST_opc, 4);
    r = fn(a0, a1, a2, a3, a4);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_15(void *addr)
{
    S_ohc (*fn)(S_ofc, S_odc) = (S_ohc (*)(S_ofc, S_odc))addr;
    S_ofc a0;
    S_odc a1;
    S_ohc r;

    cb_build(&a0, ST_ofc, 0);
    cb_build(&a1, ST_odc, 1);
    r = fn(a0, a1);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_16(void *addr)
{
    uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oLLc, uint64_t) = (uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oLLc, uint64_t))addr;
    S_oLLc a7;
    uint64_t r;

    cb_build(&a7, ST_oLLc, 7);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), (uint64_t)cb_scalar('L', 6), a7, (uint64_t)cb_scalar('L', 8));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_17(void *addr)
{
    double (*fn)(double, double, double, double, double, double, double, S_oddc, double) = (double (*)(double, double, double, double, double, double, double, S_oddc, double))addr;
    S_oddc a7;
    double r;

    cb_build(&a7, ST_oddc, 7);
    r = fn(cb_double(0), cb_double(1), cb_double(2), cb_double(3), cb_double(4), cb_double(5), cb_double(6), a7, cb_double(8));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_18(void *addr)
{
    double (*fn)(double, double, double, double, double, double, S_offfc, float) = (double (*)(double, double, double, double, double, double, S_offfc, float))addr;
    S_offfc a6;
    double r;

    cb_build(&a6, ST_offfc, 6);
    r = fn(cb_double(0), cb_double(1), cb_double(2), cb_double(3), cb_double(4), cb_double(5), a6, cb_float(7));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_19(void *addr)
{
    uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oBc, S_ohc, S_oic, S_oBc, int8_t, S_obhbc, S_ofic, S_oBBBc) = (uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oBc, S_ohc, S_oic, S_oBc, int8_t, S_obhbc, S_ofic, S_oBBBc))addr;
    S_oBc a8;
    S_ohc a9;
    S_oic a10;
    S_oBc a11;
    S_obhbc a13;
    S_ofic a14;
    S_oBBBc a15;
    uint64_t r;

    cb_build(&a8, ST_oBc, 8);
    cb_build(&a9, ST_ohc, 9);
    cb_build(&a10, ST_oic, 10);
    cb_build(&a11, ST_oBc, 11);
    cb_build(&a13, ST_obhbc, 13);
    cb_build(&a14, ST_ofic, 14);
    cb_build(&a15, ST_oBBBc, 15);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), (uint64_t)cb_scalar('L', 6), (uint64_t)cb_scalar('L', 7), a8, a9, a10, a11, (int8_t)cb_scalar('b', 12), a13, a14, a15);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_20(void *addr)
{
    uint64_t (*fn)(double, double, double, double, double, double, double, double, S_offc, S_ofc, S_oddc, float, S_offfc, int8_t, S_odc) = (uint64_t (*)(double, double, double, double, double, double, double, double, S_offc, S_ofc, S_oddc, float, S_offfc, int8_t, S_odc))addr;
    S_offc a8;
    S_ofc a9;
    S_oddc a10;
    S_offfc a12;
    S_odc a14;
    uint64_t r;

    cb_build(&a8, ST_offc, 8);
    cb_build(&a9, ST_ofc, 9);
    cb_build(&a10, ST_oddc, 10);
    cb_build(&a12, ST_offfc, 12);
    cb_build(&a14, ST_odc, 14);
    r = fn(cb_double(0), cb_double(1), cb_double(2), cb_double(3), cb_double(4), cb_double(5), cb_double(6), cb_double(7), a8, a9, a10, cb_float(11), a12, (int8_t)cb_scalar('b', 13), a14);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_21(void *addr)
{
    uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int32_t, S_oLic, int8_t, S_offfc, int8_t, S_odfc) = (uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int32_t, S_oLic, int8_t, S_offfc, int8_t, S_odfc))addr;
    S_oLic a9;
    S_offfc a11;
    S_odfc a13;
    uint64_t r;

    cb_build(&a9, ST_oLic, 9);
    cb_build(&a11, ST_offfc, 11);
    cb_build(&a13, ST_odfc, 13);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), (uint64_t)cb_scalar('L', 6), (uint64_t)cb_scalar('L', 7), (int32_t)cb_scalar('i', 8), a9, (int8_t)cb_scalar('b', 10), a11, (int8_t)cb_scalar('b', 12), a13);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_22(void *addr)
{
    uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int8_t, S_oLLLc, int8_t) = (uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int8_t, S_oLLLc, int8_t))addr;
    S_oLLLc a9;
    uint64_t r;

    cb_build(&a9, ST_oLLLc, 9);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), (uint64_t)cb_scalar('L', 6), (uint64_t)cb_scalar('L', 7), (int8_t)cb_scalar('b', 8), a9, (int8_t)cb_scalar('b', 10));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_23(void *addr)
{
    S_oLLLc (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oLLLc) = (S_oLLLc (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_oLLLc))addr;
    S_oLLLc a8;
    S_oLLLc r;

    cb_build(&a8, ST_oLLLc, 8);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), (uint64_t)cb_scalar('L', 6), (uint64_t)cb_scalar('L', 7), a8);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_24(void *addr)
{
    S_oddddc (*fn)(double, double, double, double, double, double, double, double, S_oddddc) = (S_oddddc (*)(double, double, double, double, double, double, double, double, S_oddddc))addr;
    S_oddddc a8;
    S_oddddc r;

    cb_build(&a8, ST_oddddc, 8);
    r = fn(cb_double(0), cb_double(1), cb_double(2), cb_double(3), cb_double(4), cb_double(5), cb_double(6), cb_double(7), a8);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_25(void *addr)
{
    S_odddc (*fn)(uint64_t, S_odddc, double) = (S_odddc (*)(uint64_t, S_odddc, double))addr;
    S_odddc a1;
    S_odddc r;

    cb_build(&a1, ST_odddc, 1);
    r = fn((uint64_t)cb_scalar('L', 0), a1, cb_double(2));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_26(void *addr)
{
    S_oodbcbc (*fn)(S_oodbcbc, int8_t) = (S_oodbcbc (*)(S_oodbcbc, int8_t))addr;
    S_oodbcbc a0;
    S_oodbcbc r;

    cb_build(&a0, ST_oodbcbc, 0);
    r = fn(a0, (int8_t)cb_scalar('b', 1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_27(void *addr)
{
    S_oddddddc (*fn)(void *, void *) = (S_oddddddc (*)(void *, void *))addr;
    S_oddddddc r;

    r = fn((void *)(uintptr_t)cb_scalar('p', 0), (void *)(uintptr_t)cb_scalar('p', 1));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_28(void *addr)
{
    void (*fn)(void *, void *, S_ooddcoddcc) = (void (*)(void *, void *, S_ooddcoddcc))addr;
    S_ooddcoddcc a2;

    cb_build(&a2, ST_ooddcoddcc, 2);
    fn((void *)(uintptr_t)cb_scalar('p', 0), (void *)(uintptr_t)cb_scalar('p', 1), a2);
}

static void scall_29(void *addr)
{
    S_ofic (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_ofic, S_ofic) = (S_ofic (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, S_ofic, S_ofic))addr;
    S_ofic a6;
    S_ofic a7;
    S_ofic r;

    cb_build(&a6, ST_ofic, 6);
    cb_build(&a7, ST_ofic, 7);
    r = fn((uint64_t)cb_scalar('L', 0), (uint64_t)cb_scalar('L', 1), (uint64_t)cb_scalar('L', 2), (uint64_t)cb_scalar('L', 3), (uint64_t)cb_scalar('L', 4), (uint64_t)cb_scalar('L', 5), a6, a7);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_30(void *addr)
{
    S_obdc (*fn)(S_obdc, S_ofdc, S_obfc) = (S_obdc (*)(S_obdc, S_ofdc, S_obfc))addr;
    S_obdc a0;
    S_ofdc a1;
    S_obfc a2;
    S_obdc r;

    cb_build(&a0, ST_obdc, 0);
    cb_build(&a1, ST_ofdc, 1);
    cb_build(&a2, ST_obfc, 2);
    r = fn(a0, a1, a2);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_31(void *addr)
{
    uint64_t (*fn)(S_oLLc, S_oLLc, S_oLLc, S_oLLc, S_oddddc, S_oddddc, int8_t, S_offc, int8_t, S_oddc, float) = (uint64_t (*)(S_oLLc, S_oLLc, S_oLLc, S_oLLc, S_oddddc, S_oddddc, int8_t, S_offc, int8_t, S_oddc, float))addr;
    S_oLLc a0;
    S_oLLc a1;
    S_oLLc a2;
    S_oLLc a3;
    S_oddddc a4;
    S_oddddc a5;
    S_offc a7;
    S_oddc a9;
    uint64_t r;

    cb_build(&a0, ST_oLLc, 0);
    cb_build(&a1, ST_oLLc, 1);
    cb_build(&a2, ST_oLLc, 2);
    cb_build(&a3, ST_oLLc, 3);
    cb_build(&a4, ST_oddddc, 4);
    cb_build(&a5, ST_oddddc, 5);
    cb_build(&a7, ST_offc, 7);
    cb_build(&a9, ST_oddc, 9);
    r = fn(a0, a1, a2, a3, a4, a5, (int8_t)cb_scalar('b', 6), a7, (int8_t)cb_scalar('b', 8), a9, cb_float(10));
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_32(void *addr)
{
    S_ooffcfc (*fn)(S_oofcofcc, S_ooodccc, S_olHc) = (S_ooffcfc (*)(S_oofcofcc, S_ooodccc, S_olHc))addr;
    S_oofcofcc a0;
    S_ooodccc a1;
    S_olHc a2;
    S_ooffcfc r;

    cb_build(&a0, ST_oofcofcc, 0);
    cb_build(&a1, ST_ooodccc, 1);
    cb_build(&a2, ST_olHc, 2);
    r = fn(a0, a1, a2);
    memcpy(g_cb_out, &r, sizeof r);
}

static void scall_33(void *addr)
{
    S_oddddddddddddddddc (*fn)(S_oddddddddddddddddc) = (S_oddddddddddddddddc (*)(S_oddddddddddddddddc))addr;
    S_oddddddddddddddddc a0;
    S_oddddddddddddddddc r;

    cb_build(&a0, ST_oddddddddddddddddc, 0);
    r = fn(a0);
    memcpy(g_cb_out, &r, sizeof r);
}

static const CbStCase kCbStCases[] = {
    { "{LL}({LL})", scall_0, '{', ST_oLLc, 1, { '{' }, { ST_oLLc } },
    { "{iL}(b{iL}h)", scall_1, '{', ST_oiLc, 3, { 'b', '{', 'h' }, { -1, ST_oiLc, -1 } },
    { "{Li}({Li}L)", scall_2, '{', ST_oLic, 2, { '{', 'L' }, { ST_oLic, -1 } },
    { "{dd}({dd}d)", scall_3, '{', ST_oddc, 2, { '{', 'd' }, { ST_oddc, -1 } },
    { "{ff}({ff}{fff})", scall_4, '{', ST_offc, 2, { '{', '{' }, { ST_offc, ST_offfc } },
    { "{ffff}({ffff})", scall_5, '{', ST_offffc, 1, { '{' }, { ST_offffc } },
    { "{fffff}({fffff}L)", scall_6, '{', ST_offfffc, 2, { '{', 'L' }, { ST_offfffc, -1 } },
    { "{{dd}{dd}}(pp{{dd}{dd}}p)", scall_7, '{', ST_ooddcoddcc, 4, { 'p', 'p', '{', 'p' }, { -1, -1, ST_ooddcoddcc, -1 } },
    { "{LLL}({LLL}{LLL})", scall_8, '{', ST_oLLLc, 2, { '{', '{' }, { ST_oLLLc, ST_oLLLc } },
    { "{df}({df}{dL}{Ld})", scall_9, '{', ST_odfc, 3, { '{', '{', '{' }, { ST_odfc, ST_odLc, ST_oLdc } },
    { "{dL}({dL}d)", scall_10, '{', ST_odLc, 2, { '{', 'd' }, { ST_odLc, -1 } },
    { "{Ld}(L{Ld})", scall_11, '{', ST_oLdc, 2, { 'L', '{' }, { -1, ST_oLdc } },
    { "{bBhHiu}({bBhHiu}{bhb})", scall_12, '{', ST_obBhHiuc, 2, { '{', '{' }, { ST_obBhHiuc, ST_obhbc } },
    { "{pdi}({pdi}p)", scall_13, '{', ST_opdic, 2, { '{', 'p' }, { ST_opdic, -1 } },
    { "{B}({B}{h}{i}{L}{p})", scall_14, '{', ST_oBc, 5, { '{', '{', '{', '{', '{' }, { ST_oBc, ST_ohc, ST_oic, ST_oLc, ST_opc } },
    { "{h}({f}{d})", scall_15, '{', ST_ohc, 2, { '{', '{' }, { ST_ofc, ST_odc } },
    { "L(LLLLLLL{LL}L)", scall_16, 'L', -1, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, -1, -1, ST_oLLc, -1 } },
    { "d(ddddddd{dd}d)", scall_17, 'd', -1, 9, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', 'd' }, { -1, -1, -1, -1, -1, -1, -1, ST_oddc, -1 } },
    { "d(dddddd{fff}f)", scall_18, 'd', -1, 8, { 'd', 'd', 'd', 'd', 'd', 'd', '{', 'f' }, { -1, -1, -1, -1, -1, -1, ST_offfc, -1 } },
    { "L(LLLLLLLL{B}{h}{i}{B}b{bhb}{fi}{BBB})", scall_19, 'L', -1, 16, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', '{', '{', '{', 'b', '{', '{', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oBc, ST_ohc, ST_oic, ST_oBc, -1, ST_obhbc, ST_ofic, ST_oBBBc } },
    { "L(dddddddd{ff}{f}{dd}f{fff}b{d})", scall_20, 'L', -1, 15, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', '{', '{', 'f', '{', 'b', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_offc, ST_ofc, ST_oddc, -1, ST_offfc, -1, ST_odc } },
    { "L(LLLLLLLLi{Li}b{fff}b{df})", scall_21, 'L', -1, 14, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'i', '{', 'b', '{', 'b', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, ST_oLic, -1, ST_offfc, -1, ST_odfc } },
    { "L(LLLLLLLLb{LLL}b)", scall_22, 'L', -1, 11, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'b', '{', 'b' }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, ST_oLLLc, -1 } },
    { "{LLL}(LLLLLLLL{LLL})", scall_23, '{', ST_oLLLc, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oLLLc } },
    { "{dddd}(dddddddd{dddd})", scall_24, '{', ST_oddddc, 9, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oddddc } },
    { "{ddd}(L{ddd}d)", scall_25, '{', ST_odddc, 3, { 'L', '{', 'd' }, { -1, ST_odddc, -1 } },
    { "{{db}b}({{db}b}b)", scall_26, '{', ST_oodbcbc, 2, { '{', 'b' }, { ST_oodbcbc, -1 } },
    { "{dddddd}(pp)", scall_27, '{', ST_oddddddc, 2, { 'p', 'p' }, { -1, -1 } },
    { "v(pp{{dd}{dd}})", scall_28, 'v', -1, 3, { 'p', 'p', '{' }, { -1, -1, ST_ooddcoddcc } },
    { "{fi}(LLLLLL{fi}{fi})", scall_29, '{', ST_ofic, 8, { 'L', 'L', 'L', 'L', 'L', 'L', '{', '{' }, { -1, -1, -1, -1, -1, -1, ST_ofic, ST_ofic } },
    { "{bd}({bd}{fd}{bf})", scall_30, '{', ST_obdc, 3, { '{', '{', '{' }, { ST_obdc, ST_ofdc, ST_obfc } },
    { "L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f)", scall_31, 'L', -1, 11, { '{', '{', '{', '{', '{', '{', 'b', '{', 'b', '{', 'f' }, { ST_oLLc, ST_oLLc, ST_oLLc, ST_oLLc, ST_oddddc, ST_oddddc, -1, ST_offc, -1, ST_oddc, -1 } },
    { "{{ff}f}({{f}{f}}{{{d}}}{lH})", scall_32, '{', ST_ooffcfc, 3, { '{', '{', '{' }, { ST_oofcofcc, ST_ooodccc, ST_olHc } },
    { "{dddddddddddddddd}({dddddddddddddddd})", scall_33, '{', ST_oddddddddddddddddc, 1, { '{' }, { ST_oddddddddddddddddc } },
};
#define NCBSTCASES (sizeof kCbStCases / sizeof kCbStCases[0])

static uint64_t srec_word(int i)
{
    return ocerz_ld(g_guest + SREC_REC + 8ull * (uint64_t)i, 8);
}

static void srec_clear(void)
{
    memset(ocerz_g2h(g_guest + SREC_REC), 0, 0x100);
    memset(ocerz_g2h(g_guest + SREC_RES), 0, 0x140);
}

static void srec_memory_result(const uint8_t *bytes, size_t len)
{
    ocerz_st(g_guest + SREC_RES + 32, 8, len);
    memcpy(ocerz_g2h(g_guest + SREC_RES + SREC_MEM), bytes, len);
}

static void *bind_struct(const char *notation)
{
    void *addr = ocerz_abi_callback_intern(g_guest + SREC_OFF, notation);

    CHECK(addr != NULL, "interning the structure recorder %#llx as %s returned NULL",
          (unsigned long long)(g_guest + SREC_OFF), notation);
    if (addr)
        note_new(addr, notation);
    return addr;
}

typedef struct CbWant {
    uint64_t gpr[6];
    uint64_t xmm[8];
    uint64_t stack[SREC_SLOTS];
    int ngpr;
    int nxmm;
    int nstack;
    int hidden;
} CbWant;

static void cb_expect(const CbStCase *c, CbWant *w)
{
    int i, k;

    memset(w, 0, sizeof *w);
    if (c->ret == '{' && kStTypes[c->ret_type].size > 16) {
        w->hidden = 1;
        w->ngpr = 1;
    }

    for (i = 0; i < c->nargs; i++) {
        char a = c->arg[i];

        if (a != '{') {
            uint64_t v = cb_guest_scalar(a, cb_scalar(a, i));

            if (is_fp(a) && w->nxmm < 8)
                w->xmm[w->nxmm++] = v;
            else if (!is_fp(a) && w->ngpr < 6)
                w->gpr[w->ngpr++] = v;
            else if (w->nstack < SREC_SLOTS)
                w->stack[w->nstack++] = v;
            continue;
        }

        const StType *t = &kStTypes[c->type[i]];
        uint8_t bytes[OCERZ_ABI_STRUCT_BYTES];
        char cls[2];
        int n = st_sysv_class(t, cls), ni = 0, ns = 0;
        int words = (int)((t->size + 7) / 8);

        memset(bytes, 0, sizeof bytes);
        cb_build(bytes, c->type[i], i);
        for (k = 0; k < n; k++) {
            if (cls[k] == 'I')
                ni++;
            else
                ns++;
        }
        if (n && w->ngpr + ni <= 6 && w->nxmm + ns <= 8) {
            for (k = 0; k < n; k++) {
                uint64_t v;

                memcpy(&v, bytes + 8 * k, 8);
                if (cls[k] == 'I')
                    w->gpr[w->ngpr++] = v;
                else
                    w->xmm[w->nxmm++] = v;
            }
        } else {
            for (k = 0; k < words && w->nstack < SREC_SLOTS; k++) {
                uint64_t v;

                memcpy(&v, bytes + 8 * k, 8);
                w->stack[w->nstack++] = v;
            }
        }
    }
}

static void cb_prime(const CbStCase *c, uint8_t *want)
{
    srec_clear();
    memset(want, 0, OCERZ_ABI_STRUCT_BYTES);

    if (c->ret == 'L') {
        uint64_t v = 0x0123456789abcdefull;
        ocerz_st(g_guest + SREC_RES, 8, v);
        memcpy(want, &v, 8);
    } else if (c->ret == 'd') {
        uint64_t v = 0x400921fb54442d18ull;
        ocerz_st(g_guest + SREC_RES + 16, 8, v);
        memcpy(want, &v, 8);
    } else if (c->ret == '{') {
        const StType *t = &kStTypes[c->ret_type];
        uint8_t words[16];
        char cls[2];
        int n = st_sysv_class(t, cls), k, ri = 0, si = 0;

        cb_build(want, c->ret_type, CB_RES_ARG);
        if (!n) {
            srec_memory_result(want, t->size);
            return;
        }
        memcpy(words, want, sizeof words);
        for (k = (int)t->size; k < 16; k++)
            words[k] = (uint8_t)(0xc0 | k);
        for (k = 0; k < n; k++) {
            uint64_t v;

            memcpy(&v, words + 8 * k, 8);
            if (cls[k] == 'I')
                ocerz_st(g_guest + SREC_RES + 8ull * (uint64_t)ri++, 8, v);
            else
                ocerz_st(g_guest + SREC_RES + 16 + 8ull * (uint64_t)si++, 8, v);
        }
    }
}

static void cb_run_native(const CbStCase *c)
{
    CbWant w;
    uint8_t want[OCERZ_ABI_STRUCT_BYTES];
    void *addr = bind_struct(c->sig);
    uint64_t ran;
    int k, m;

    if (!addr)
        return;
    cb_expect(c, &w);
    cb_prime(c, want);
    memset(g_cb_out, 0x5a, sizeof g_cb_out);

    c->call(addr);

    ran = srec_word(SREC_COUNT);
    CHECK(ran == 1, "%s called natively: the guest recorder ran %llu times, want once",
          c->sig, (unsigned long long)ran);
    if (ran != 1)
        return;

    if (w.hidden)
        CHECK(srec_word(0) != 0,
              "%s called natively: rdi is null, want the address of space for the MEMORY "
              "result", c->sig);
    for (k = w.hidden; k < w.ngpr; k++)
        CHECK(srec_word(k) == w.gpr[k],
              "%s called natively: guest integer register %d held %#llx, want %#llx",
              c->sig, k, (unsigned long long)srec_word(k), (unsigned long long)w.gpr[k]);
    for (k = 0; k < w.nxmm; k++)
        CHECK(srec_word(SREC_XMM + k) == w.xmm[k],
              "%s called natively: guest xmm%d held %#llx, want %#llx", c->sig, k,
              (unsigned long long)srec_word(SREC_XMM + k), (unsigned long long)w.xmm[k]);
    for (k = 0; k < w.nstack; k++)
        CHECK(srec_word(SREC_STACK + k) == w.stack[k],
              "%s called natively: guest stack eightbyte %d held %#llx, want %#llx",
              c->sig, k, (unsigned long long)srec_word(SREC_STACK + k),
              (unsigned long long)w.stack[k]);

    if (c->ret == '{') {
        const StType *t = &kStTypes[c->ret_type];

        for (m = 0; m < t->n; m++) {
            uint64_t g = 0, e = 0;
            size_t size = cls_size(t->cls[m]);

            memcpy(&g, g_cb_out + t->off[m], size);
            memcpy(&e, want + t->off[m], size);
            CHECK(g == e,
                  "%s called natively: result member %d ('%c' at offset %u) reached the "
                  "native caller as %#llx, want %#llx", c->sig, m, t->cls[m],
                  (unsigned)t->off[m], (unsigned long long)g, (unsigned long long)e);
        }
    } else if (c->ret != 'v') {
        uint64_t g, e;

        memcpy(&g, g_cb_out, 8);
        memcpy(&e, want, 8);
        CHECK(g == e, "%s called natively: the result reached the native caller as %#llx, "
              "want %#llx", c->sig, (unsigned long long)g, (unsigned long long)e);
    }
}

static int dispatch8(void *addr, const uint64_t *x, const uint64_t *v, const uint8_t *stack,
                     void *x8, uint64_t *ox, uint64_t *ov)
{
    unsigned slot = 0;

    if (!addr || !slot_of(addr, &slot))
        return 0;
    set_words(ox, 2, OUT_POISON);
    set_words(ov, 4, OUT_POISON);
    ocerz_abi_callback_dispatch(slot, x, v, stack, x8, ox, ov);
    return 1;
}

static void put_bytes(uint8_t *at, uint64_t v, size_t len)
{
    memcpy(at, &v, len);
}

static void expect_srec(const char *what, int word, uint64_t want)
{
    uint64_t got = srec_word(word);
    const char *kind = word < SREC_XMM ? "integer register"
                     : word < SREC_STACK ? "xmm" : "stack eightbyte";
    int index = word < SREC_XMM ? word : word < SREC_STACK ? word - SREC_XMM
                                                           : word - SREC_STACK;

    CHECK(got == want, "%s: the guest's %s %d held %#llx, want %#llx", what, kind, index,
          (unsigned long long)got, (unsigned long long)want);
}

static void expect_srec_ran(const char *what, uint64_t times)
{
    uint64_t n = srec_word(SREC_COUNT);

    CHECK(n == times, "%s: the guest recorder ran %llu times, want %llu", what,
          (unsigned long long)n, (unsigned long long)times);
}

static void struct_dispatch_small(void)
{
    uint64_t x[8], v[8], ox[2], ov[4];
    uint8_t stack[64];
    const char *what;
    void *a;
    int k;

    what = "{B}({B}{h}{fi}) dispatched with garbage above each structure";
    a = bind_struct("{B}({B}{h}{fi})");
    set_words(x, 8, 0xdeadbeefcafef00dull);
    x[0] = 0xdeadbeefcafe12b1ull;
    x[1] = 0xdeadbeefcafe8002ull;
    x[2] = 0x0000000940490fdbull;
    set_words(v, 8, 0x7ff8dead0000beefull);
    memset(stack, 0xa5, sizeof stack);
    srec_clear();
    ocerz_st(g_guest + SREC_RES, 8, 0xdeadbeefcafe12c3ull);
    ocerz_st(g_guest + SREC_RES + 8, 8, 0x1122334455667788ull);
    if (dispatch8(a, x, v, stack, NULL, ox, ov)) {
        expect_srec_ran(what, 1);
        expect_srec(what, 0, 0xb1);
        expect_srec(what, 1, 0x8002);
        expect_srec(what, 2, 0x0000000940490fdbull);
        CHECK(ox[0] == 0xc3 && ox[1] == 0 && words_zero(ov, 4),
              "%s: x0=%#llx x1=%#llx d0=%#llx, want x0 0xc3 alone from a guest leaving "
              "0xdeadbeefcafe12c3 in rax", what, (unsigned long long)ox[0],
              (unsigned long long)ox[1], (unsigned long long)ov[0]);
    }

    what = "{fff}(dddddd{fff}f) dispatched with the aggregate spilled";
    a = bind_struct("{fff}(dddddd{fff}f)");
    set_words(x, 8, 0xdeadbeefcafef00dull);
    for (k = 0; k < 8; k++)
        v[k] = 0x4000000000000000ull + (uint64_t)k;
    memset(stack, 0xa5, sizeof stack);
    put_bytes(stack, 0x3fa00001u, 4);
    put_bytes(stack + 4, 0x3fa00002u, 4);
    put_bytes(stack + 8, 0x3fa00003u, 4);
    put_bytes(stack + 12, 0x3fa00004u, 4);
    srec_clear();
    ocerz_st(g_guest + SREC_RES + 16, 8, 0x40a0000140a00002ull);
    ocerz_st(g_guest + SREC_RES + 24, 8, 0xdeadbeef40a00003ull);
    if (dispatch8(a, x, v, stack, NULL, ox, ov)) {
        expect_srec_ran(what, 1);
        for (k = 0; k < 6; k++)
            expect_srec(what, SREC_XMM + k, 0x4000000000000000ull + (uint64_t)k);
        expect_srec(what, SREC_XMM + 6, 0x3fa000023fa00001ull);
        expect_srec(what, SREC_XMM + 7, 0x3fa00003ull);
        expect_srec(what, SREC_STACK, 0x3fa00004ull);
        CHECK(ov[0] == 0x40a00002u && ov[1] == 0x40a00001u && ov[2] == 0x40a00003u &&
                  ov[3] == 0 && words_zero(ox, 2),
              "%s: d0..d3 are %#llx %#llx %#llx %#llx, want the three floats and zero, "
              "with nothing from xmm1's upper half", what, (unsigned long long)ov[0],
              (unsigned long long)ov[1], (unsigned long long)ov[2],
              (unsigned long long)ov[3]);
    }

    what = "{dddd}(ddddddd{dddd}d) dispatched with the aggregate and the double after it stacked";
    a = bind_struct("{dddd}(ddddddd{dddd}d)");
    set_words(x, 8, 0xdeadbeefcafef00dull);
    for (k = 0; k < 8; k++)
        v[k] = 0x4010000000000000ull + (uint64_t)k;
    memset(stack, 0xa5, sizeof stack);
    for (k = 0; k < 4; k++)
        put_bytes(stack + 8 * k, 0x4020000000000000ull + (uint64_t)k, 8);
    put_bytes(stack + 32, 0x4030000000000009ull, 8);
    srec_clear();
    {
        uint8_t res[32];

        for (k = 0; k < 4; k++)
            put_bytes(res + 8 * k, 0x4040000000000000ull + (uint64_t)k, 8);
        srec_memory_result(res, sizeof res);
    }
    if (dispatch8(a, x, v, stack, NULL, ox, ov)) {
        expect_srec_ran(what, 1);
        CHECK(srec_word(0) != 0, "%s: rdi is null, want space for the MEMORY result", what);
        for (k = 0; k < 7; k++)
            expect_srec(what, SREC_XMM + k, 0x4010000000000000ull + (uint64_t)k);
        expect_srec(what, SREC_XMM + 7, 0x4030000000000009ull);
        for (k = 0; k < 4; k++)
            expect_srec(what, SREC_STACK + k, 0x4020000000000000ull + (uint64_t)k);
        for (k = 0; k < 4; k++)
            CHECK(ov[k] == 0x4040000000000000ull + (uint64_t)k,
                  "%s: d%d is %#llx, want %#llx from the guest's result space", what, k,
                  (unsigned long long)ov[k], 0x4040000000000000ull + (uint64_t)k);
        CHECK(words_zero(ox, 2), "%s: x0 or x1 is not zero after an aggregate result", what);
    }

    what = "{LL}(LLLLLLL{LL}L) dispatched with the structure and the long after it stacked";
    a = bind_struct("{LL}(LLLLLLL{LL}L)");
    for (k = 0; k < 8; k++)
        x[k] = 0x1000ull + (uint64_t)k;
    set_words(v, 8, 0x7ff8dead0000beefull);
    memset(stack, 0xa5, sizeof stack);
    put_bytes(stack, 0xaaaaaaaaaaaaaaa1ull, 8);
    put_bytes(stack + 8, 0xbbbbbbbbbbbbbbb2ull, 8);
    put_bytes(stack + 16, 0xccccccccccccccc3ull, 8);
    srec_clear();
    ocerz_st(g_guest + SREC_RES, 8, 0x0102030405060708ull);
    ocerz_st(g_guest + SREC_RES + 8, 8, 0x1112131415161718ull);
    if (dispatch8(a, x, v, stack, NULL, ox, ov)) {
        expect_srec_ran(what, 1);
        for (k = 0; k < 6; k++)
            expect_srec(what, k, 0x1000ull + (uint64_t)k);
        expect_srec(what, SREC_STACK, 0x1006ull);
        expect_srec(what, SREC_STACK + 1, 0xaaaaaaaaaaaaaaa1ull);
        expect_srec(what, SREC_STACK + 2, 0xbbbbbbbbbbbbbbb2ull);
        expect_srec(what, SREC_STACK + 3, 0xccccccccccccccc3ull);
        CHECK(ox[0] == 0x0102030405060708ull && ox[1] == 0x1112131415161718ull &&
                  words_zero(ov, 4),
              "%s: x0=%#llx x1=%#llx, want rax and rdx", what, (unsigned long long)ox[0],
              (unsigned long long)ox[1]);
    }

    what = "{LL}(LLLLLLL{LL}L) dispatched with no stack";
    srec_clear();
    if (dispatch8(a, x, v, NULL, NULL, ox, ov)) {
        expect_srec_ran(what, 0);
        CHECK(words_zero(ox, 2) && words_zero(ov, 4),
              "%s: a refused callback left x0=%#llx d0=%#llx, want zero", what,
              (unsigned long long)ox[0], (unsigned long long)ov[0]);
    }
}

static void struct_dispatch_memory(void)
{
    uint64_t x[8], v[8], ox[2], ov[4], src[3];
    uint8_t stack[64], buf[64], pdi[24], res[24];
    const OcerzCPU *cpu = ocerz_vm_current_cpu();
    const char *what;
    uint64_t rdi, top;
    void *a;
    int k;

    what = "{LLL}({LLL}) dispatched with a buffer in x8";
    a = bind_struct("{LLL}({LLL})");
    src[0] = 0x1111111111111111ull;
    src[1] = 0x2222222222222222ull;
    src[2] = 0x3333333333333333ull;
    set_words(x, 8, 0xdeadbeefcafef00dull);
    x[0] = (uint64_t)(uintptr_t)src;
    set_words(v, 8, 0x7ff8dead0000beefull);
    memset(stack, 0xa5, sizeof stack);
    memset(buf, 0x77, sizeof buf);
    for (k = 0; k < 3; k++)
        put_bytes(res + 8 * k, 0xa0a0a0a0a0a0a0a1ull + (uint64_t)k, 8);
    srec_clear();
    srec_memory_result(res, sizeof res);
    if (dispatch8(a, x, v, stack, buf, ox, ov)) {
        expect_srec_ran(what, 1);
        rdi = srec_word(0);
        top = cpu ? cpu->gpr[OCERZ_RSP] - 128 : 0;
        CHECK(cpu && rdi + 24 <= top && rdi + 64 > top,
              "%s: the guest's result pointer is %#llx, want space just below the "
              "dispatcher's stack top %#llx", what, (unsigned long long)rdi,
              (unsigned long long)top);
        for (k = 0; k < 3; k++)
            expect_srec(what, SREC_STACK + k, src[k]);
        CHECK(memcmp(buf, res, sizeof res) == 0,
              "%s: the x8 buffer does not hold the structure the guest wrote", what);
        for (k = (int)sizeof res; k < (int)sizeof buf; k++)
            if (buf[k] != 0x77)
                break;
        CHECK(k == (int)sizeof buf, "%s: byte %d of the x8 buffer, past the structure, "
              "was written", what, k);
        CHECK(words_zero(ox, 2) && words_zero(ov, 4),
              "%s: x0=%#llx d0=%#llx, want zero beside an x8 result", what,
              (unsigned long long)ox[0], (unsigned long long)ov[0]);
    }

    what = "{LLL}({LLL}) dispatched with x8 null";
    srec_clear();
    srec_memory_result(res, sizeof res);
    if (dispatch8(a, x, v, stack, NULL, ox, ov)) {
        expect_srec_ran(what, 0);
        CHECK(words_zero(ox, 2) && words_zero(ov, 4),
              "%s: a refused callback left x0=%#llx d0=%#llx, want zero", what,
              (unsigned long long)ox[0], (unsigned long long)ov[0]);
    }

    what = "{LLL}({LLL}) dispatched with a null copy address";
    x[0] = 0;
    memset(buf, 0x77, sizeof buf);
    srec_clear();
    srec_memory_result(res, sizeof res);
    if (dispatch8(a, x, v, stack, buf, ox, ov)) {
        expect_srec_ran(what, 0);
        for (k = 0; k < (int)sizeof buf; k++)
            if (buf[k] != 0x77)
                break;
        CHECK(k == (int)sizeof buf && words_zero(ox, 2) && words_zero(ov, 4),
              "%s: a refused callback wrote the x8 buffer or left a result", what);
    }

    what = "{pdi}({pdi}) dispatched through a copy and an x8 buffer";
    a = bind_struct("{pdi}({pdi})");
    memset(pdi, 0, sizeof pdi);
    put_bytes(pdi, g_guest + 0x40, 8);
    put_bytes(pdi + 8, 0x3ff8000000000000ull, 8);
    put_bytes(pdi + 16, 0x7fffffffu, 4);
    set_words(x, 8, 0xdeadbeefcafef00dull);
    x[0] = (uint64_t)(uintptr_t)pdi;
    memset(res, 0, sizeof res);
    put_bytes(res, g_guest + 0x48, 8);
    put_bytes(res + 8, 0x4000000000000000ull, 8);
    put_bytes(res + 16, 0x80000000u, 4);
    memset(buf, 0x77, sizeof buf);
    srec_clear();
    srec_memory_result(res, sizeof res);
    if (dispatch8(a, x, v, stack, buf, ox, ov)) {
        expect_srec_ran(what, 1);
        expect_srec(what, SREC_STACK, g_guest + 0x40);
        expect_srec(what, SREC_STACK + 1, 0x3ff8000000000000ull);
        expect_srec(what, SREC_STACK + 2, 0x7fffffffull);
        CHECK(memcmp(buf, res, sizeof res) == 0,
              "%s: the x8 buffer does not hold the pointer, double and int the guest "
              "returned", what);
    }
}

static void *struct_thread(void *arg)
{
    OcerzCPU *cpu = ocerz_thread_attach(&g_vm);
    size_t i;

    CHECK(cpu != NULL, "attaching a created thread to the test's VM returned NULL, so "
          "no structure callback reaches guest code");
    if (!cpu)
        return arg;

    for (i = 0; i < NCBSTCASES; i++)
        cb_run_native(&kCbStCases[i]);
    struct_dispatch_small();
    struct_dispatch_memory();

    ocerz_thread_detach();
    return arg;
}

static void test_struct_callbacks(void)
{
    pthread_t th;
    int rc;

    rc = pthread_create(&th, NULL, struct_thread, NULL);
    CHECK(rc == 0, "pthread_create failed with %d, so structure callbacks are untested",
          rc);
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
    memcpy(ocerz_g2h(g_guest + SREC_OFF), kStructRecorder, sizeof kStructRecorder);
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
    test_struct_callbacks();
    test_exhaustion();
    test_convert_full();

    return report();
}
