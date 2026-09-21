/*
 * Blocks crossing between guest x86 code and native arm64 code, checked from
 * both sides with real guest code and real native blocks.
 *
 * The guest code is 120 bytes assembled by clang -arch x86_64 and copied into
 * guest memory: an invoke that returns its captured int plus its argument, a
 * copy helper that copies that int and counts, a dispose helper that counts, a
 * __block keep helper that copies a word and counts, a destroy helper that
 * counts, and two callers that jump through a block's word at offset 16 with
 * the block in rdi, or in rsi behind a structure result's pointer.  The VM runs
 * it with the JIT off, on a thread the test creates and attaches itself, since
 * no VM is the process's in a harness.  The native blocks are block literals in
 * this file, compiled for arm64 by the same clang, so their flags, descriptors
 * and signatures are what a framework's are.
 *
 * The notation half runs first.  A block signature becomes its invoke's
 * notation with the block itself as a first argument of class k, a declared
 * notation gets that first argument inserted, including after a structure or
 * block result, and a signature that does not start with the block, one with a
 * type the engine refuses, a declared notation naming a callback and a block
 * with neither are refused.  The internal trampoline is checked as bytes: one
 * address however often it is asked for, a mov of the reserved id into r11 and
 * a jump through a slot holding the bridge's trap address, nothing for an index
 * past the table, and the reserved id named as ocerz's own with a special's xmm
 * contract.
 *
 * Guest to native.  Null stays null and a native block passes through.  A guest
 * global block gets one global wrapper, the same every time, that unwraps back
 * to it and that native code can call.  A guest heap block gets a heap wrapper
 * holding one reference, the same wrapper while it lives with each lookup a
 * reference of the caller's, and when the last of those is released the guest
 * block's count is back where it started.  A guest stack block with helpers is
 * copied guest-side: the copy is a malloc block whose helpers are callback slots
 * bound to the guest's, the guest's copy helper runs once and the captured word
 * arrives, two copies share one shadow descriptor, and the native _Block_release
 * that frees a copy runs the guest's dispose helper.  Wrapping the stack block
 * itself wraps a fresh copy, and releasing the wrapper frees it.  A block with
 * no signature is wrapped under a declared one when there is one, and when there
 * is none the wrapper is made and stops by name, exit 72, only when called.
 *
 * Native to guest.  A native heap block gets a guest view whose invoke is the
 * trampoline and whose captured word is the native block, which holds one
 * native reference, is the same view while it lives, unwraps back to the native
 * block, and when guest code calls it through its word at offset 16 the
 * trampoline traps and the native block runs with the guest's argument.  A
 * native global block gets a global view, and a native stack block a view of a
 * copy that dies with the view.  A result the caller adopts moves its reference
 * into the view; a wrapper of a guest block returned that way unwraps to the
 * guest block with a reference of the guest's own.
 *
 * The runtime specials run against a hand-built cpu: _Block_copy of a guest
 * stack block returns a heap copy in rax and returns through the return address,
 * _Block_object_assign of a block field stores a copy, and of a __block variable
 * still on the stack with helpers moves it to the heap guest-side, forwarding
 * both ways, keep run once, helpers bound to slots, a second assign sharing it
 * and three native disposes running destroy once.  A __block variable with no
 * helpers goes to the native function, which copies its bytes.
 */
#include "ocerz/blocks.h"
#include "ocerz/abi.h"
#include "ocerz/dyldapi.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"

#include <Block.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARENA        (4ull << 30)
#define GUEST_SPAN   0x10000ull
#define OFF_INVOKE   0x10ull
#define OFF_COPY     0x20ull
#define OFF_DISPOSE  0x30ull
#define OFF_KEEP     0x40ull
#define OFF_DESTROY  0x50ull
#define OFF_CALL     0x60ull
#define OFF_CALL_STR 0x70ull
#define OFF_COPIES   0x100ull
#define OFF_DISPOSES 0x108ull
#define OFF_KEEPS    0x110ull
#define OFF_DESTROYS 0x118ull
#define OFF_BLOCKS   0x1000ull
#define OFF_DESCS    0x2000ull
#define OFF_STRINGS  0x3000ull
#define OFF_STACK    0x8000ull
#define RET_ADDR     0x0000000044332200ull

#define F_DEALLOC    0x0001
#define F_REFCOUNT   0xfffe
#define F_NEEDS_FREE (1 << 24)
#define F_COPY_DISP  (1 << 25)
#define F_GLOBAL     (1 << 28)
#define F_SIGNATURE  (1 << 30)

extern void *_NSConcreteMallocBlock[32];

static const uint8_t kGuest[] = {
    0x48, 0x89, 0xf8, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x8b, 0x47, 0x20, 0x01, 0xf0, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x8b, 0x46, 0x20, 0x89, 0x47, 0x20, 0x48, 0xff, 0x05, 0xd3, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc,
    0x48, 0xff, 0x05, 0xd1, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x48, 0x8b, 0x46, 0x28, 0x48, 0x89, 0x47, 0x28, 0x48, 0xff, 0x05, 0xc1, 0x00, 0x00, 0x00, 0xc3,
    0x48, 0xff, 0x05, 0xc1, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x48, 0x8b, 0x47, 0x10, 0xff, 0xe0, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x48, 0x8b, 0x46, 0x10, 0xff, 0xe0, 0xcc, 0xcc,
};

typedef struct Blk {
    void *isa;
    int32_t flags;
    int32_t reserved;
    uint64_t invoke;
    uint64_t desc;
    uint64_t word;
} Blk;

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

static OcerzVM g_vm;
static uint64_t g_guest;

static Blk *host_blk(uint64_t g)
{
    return (Blk *)ocerz_g2h(g);
}

static uint64_t counter(uint64_t off)
{
    return ocerz_ld(g_guest + off, 8);
}

static int refcount(const void *block)
{
    return (((const Blk *)block)->flags & F_REFCOUNT) / 2;
}

static uint64_t put_string(uint64_t off, const char *s)
{
    memcpy(ocerz_g2h(g_guest + OFF_STRINGS + off), s, strlen(s) + 1);
    return g_guest + OFF_STRINGS + off;
}

static uint64_t put_desc(uint64_t off, uint64_t size, int helpers, uint64_t sig)
{
    uint64_t d = g_guest + OFF_DESCS + off;
    uint64_t *w = ocerz_g2h(d);
    int k = 0;
    w[k++] = 0;
    w[k++] = size;
    if (helpers) {
        w[k++] = g_guest + OFF_COPY;
        w[k++] = g_guest + OFF_DISPOSE;
    }
    if (sig) {
        w[k++] = sig;
        w[k++] = 0;
    }
    return d;
}

static void fill_blk(Blk *b, void *isa, int32_t flags, uint64_t desc, uint64_t word)
{
    b->isa = isa;
    b->flags = flags;
    b->reserved = 0;
    b->invoke = g_guest + OFF_INVOKE;
    b->desc = desc;
    b->word = word;
}

static uint64_t guest_blk(uint64_t off, void *isa, int32_t flags, uint64_t desc, uint64_t word)
{
    uint64_t g = g_guest + OFF_BLOCKS + off;
    fill_blk(host_blk(g), isa, flags, desc, word);
    return g;
}

static void check_notation(const char *enc, const char *declared, int want_rc, const char *want)
{
    char out[OCERZ_BLOCK_NOTATION_MAX];
    int rc = ocerz_block_invoke_notation(enc, declared, out, sizeof out);
    CHECK(rc == want_rc && (want_rc != OCERZ_OK || strcmp(out, want) == 0),
          "invoke notation of %s declared %s: rc %d notation %s, want rc %d notation %s",
          enc ? enc : "(none)", declared ? declared : "(none)", rc, rc == OCERZ_OK ? out : "-", want_rc,
          want ? want : "-");
}

static void test_notation(void)
{
    check_notation("i20@?0i8d12", NULL, OCERZ_OK, "i(k{}id)");
    check_notation("v8@?0", "v(L)", OCERZ_OK, "v(k{})");
    check_notation("q24@?0@8@16", NULL, OCERZ_OK, "l(k{}pp)");
    check_notation("v16@?0@?8", NULL, OCERZ_OK, "v(k{}k{})");
    check_notation("c32@?0@8Q16^c24", NULL, OCERZ_OK, "b(k{}pLp)");
    check_notation("{CGPoint=dd}8@?0", NULL, OCERZ_OK, "{dd}(k{})");
    check_notation(NULL, "v(L)", OCERZ_OK, "v(k{}L)");
    check_notation(NULL, "v()", OCERZ_OK, "v(k{})");
    check_notation(NULL, "{dd}(p)", OCERZ_OK, "{dd}(k{}p)");
    check_notation(NULL, "k{v()}(p)", OCERZ_OK, "k{v()}(k{}p)");
    check_notation(NULL, NULL, OCERZ_EUNDEF, NULL);
    check_notation(NULL, "", OCERZ_EUNDEF, NULL);
    check_notation("v8i0", NULL, OCERZ_EFORMAT, NULL);
    check_notation("v8^v0", NULL, OCERZ_EFORMAT, NULL);
    check_notation("D8@?0", NULL, OCERZ_EUNSUP, NULL);
    check_notation(NULL, "v(c{v()})", OCERZ_EUNSUP, NULL);
    check_notation(NULL, "v(x)", OCERZ_EUNSUP, NULL);
}

static void test_trampoline(void)
{
    uint64_t t = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_BLOCK_INVOKE);
    CHECK(t != 0, "the block trampoline has no address");
    if (!t)
        return;
    CHECK(ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_BLOCK_INVOKE) == t,
          "asked twice, the block trampoline moved");
    uint64_t imp = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_IMP);
    uint64_t fn = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_FN);
    CHECK(imp != 0 && fn != 0 && imp != t && fn != t && imp != fn,
          "the three trampolines are not three addresses: %#llx %#llx %#llx", (unsigned long long)t,
          (unsigned long long)imp, (unsigned long long)fn);
    CHECK(ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_FN + 1) == 0,
          "a trampoline past the table has an address");
    const uint8_t *b = ocerz_g2h(t);
    uint32_t id, rel;
    memcpy(&id, b + 2, 4);
    memcpy(&rel, b + 8, 4);
    CHECK(b[0] == 0x41 && b[1] == 0xbb && b[6] == 0xff && b[7] == 0x25,
          "the trampoline is not mov r11d, imm32 then jmp [rip+disp32]: %02x %02x .. %02x %02x", b[0], b[1],
          b[6], b[7]);
    CHECK(id == 0xfff00000u, "the trampoline loads id %#x, want the reserved ordinal's entry 0", id);
    uint64_t slot = t + 12 + (uint64_t)(int64_t)(int32_t)rel;
    CHECK(ocerz_ld(slot, 8) == OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF,
          "the trampoline's slot holds %#llx, want the bridge's trap address",
          (unsigned long long)ocerz_ld(slot, 8));
    const char *lib = NULL, *sym = NULL;
    CHECK(ocerz_vdylib_export_name(id, &lib, &sym) && lib && strcmp(lib, "ocerz") == 0,
          "the reserved id is not named as ocerz's own: %s", lib ? lib : "(nothing)");
    uint16_t in = 0, out = 0;
    CHECK(ocerz_vdylib_xmm_contract(id, &in, &out) && in == 0xff && out == 0x3,
          "the reserved id's xmm contract is %#x/%#x, want a special's 0xff/0x3", in, out);
    CHECK(ocerz_vdylib_export_name(0xfff00000u + OCERZ_VDYLIB_TRAMP_NATIVE_FN, &lib, &sym) && lib &&
          strcmp(lib, "ocerz") == 0, "the last reserved id is not named as ocerz's own");
    CHECK(!ocerz_vdylib_export_name(0xfff00001u + OCERZ_VDYLIB_TRAMP_NATIVE_FN, NULL, NULL),
          "a reserved id past the table is named");
}

static int call_native(uint64_t block, int arg)
{
    const Blk *b = (const Blk *)(uintptr_t)block;
    return ((int (*)(const void *, int))(uintptr_t)b->invoke)(b, arg);
}

static int call_guest(uint64_t view, int arg)
{
    OcerzCPU *cpu = ocerz_vm_current_cpu();
    OcerzGuestCall call;
    memset(&call, 0, sizeof call);
    call.gpr[0] = view;
    call.gpr[1] = (uint64_t)(uint32_t)arg;
    uint64_t top = (cpu->gpr[OCERZ_RSP] - 128) & ~0xfull;
    int rc = ocerz_vm_call_abi(&g_vm, g_guest + OFF_CALL, &call, top);
    CHECK(rc == OCERZ_OK, "calling guest view %#llx from guest code returned %d", (unsigned long long)view, rc);
    return (int)call.rax;
}

static void test_passthrough(void)
{
    uint64_t out = 1, owned = 1;
    CHECK(ocerz_block_to_native(0, "", &out, &owned) == OCERZ_OK && out == 0 && owned == 0,
          "a null guest block became %#llx", (unsigned long long)out);
    out = owned = 1;
    CHECK(ocerz_block_to_guest(0, "", &out, &owned) == OCERZ_OK && out == 0 && owned == 0,
          "a null native block became %#llx", (unsigned long long)out);

    int (^native)(int) = ^(int x) { return x + 1; };
    uint64_t n = (uint64_t)(uintptr_t)native;
    CHECK(ocerz_block_to_native(n, "", &out, &owned) == OCERZ_OK && out == n && owned == 0,
          "a native block handed to native code became %#llx", (unsigned long long)out);
    CHECK(!ocerz_block_is_guest(n), "a native block is taken for a guest block");

    uint64_t g = guest_blk(0x000, _NSConcreteGlobalBlock, F_GLOBAL, put_desc(0, 40, 0, 0), 1);
    CHECK(ocerz_block_to_guest(g, "", &out, &owned) == OCERZ_OK && out == g && owned == 0,
          "a guest block handed to guest code became %#llx", (unsigned long long)out);
    CHECK(ocerz_block_is_guest(g), "a guest block is not taken for one");
}

static void test_guest_global(void)
{
    uint64_t sig = put_string(0x000, "i12@?0i8");
    uint64_t g = guest_blk(0x040, _NSConcreteGlobalBlock, F_GLOBAL | F_SIGNATURE, put_desc(0x040, 40, 0, sig), 37);
    uint64_t w = 0, w2 = 0, owned = 1, inner = 0;

    CHECK(ocerz_block_to_native(g, "", &w, &owned) == OCERZ_OK && w && w != g && owned == 0,
          "a guest global block gave wrapper %#llx owned %#llx", (unsigned long long)w, (unsigned long long)owned);
    if (!w)
        return;
    const Blk *b = (const Blk *)(uintptr_t)w;
    CHECK(b->isa == (void *)_NSConcreteGlobalBlock && (b->flags & (F_GLOBAL | F_COPY_DISP | F_SIGNATURE)) ==
          (F_GLOBAL | F_COPY_DISP | F_SIGNATURE) && !(b->flags & F_NEEDS_FREE),
          "the global wrapper has isa %p flags %#x", b->isa, b->flags);
    CHECK(ocerz_block_to_native(g, "", &w2, &owned) == OCERZ_OK && w2 == w,
          "the same guest global block gave a second wrapper %#llx after %#llx", (unsigned long long)w2,
          (unsigned long long)w);
    CHECK(ocerz_block_native_wrapper(w, &inner) && inner == g, "the wrapper does not name its guest block");
    CHECK(!ocerz_block_is_guest(w), "the wrapper's invoke is taken for guest code");
    uint64_t back = 0;
    CHECK(ocerz_block_to_guest(w, "", &back, &owned) == OCERZ_OK && back == g && owned == 0,
          "the wrapper handed back to guest code became %#llx, want the guest block", (unsigned long long)back);
    CHECK(((const uint64_t *)b->desc)[4] == sig, "the wrapper's descriptor does not carry the block's signature");
    CHECK(_Block_copy(b) == b, "native _Block_copy of a global wrapper moved it");
    _Block_release(b);
    CHECK(call_native(w, 5) == 42, "native code calling the wrapper got %d, want 37 + 5", call_native(w, 5));
}

static void test_guest_heap(void)
{
    uint64_t sig = put_string(0x040, "i12@?0i8");
    Blk *g = malloc(sizeof *g);
    fill_blk(g, _NSConcreteMallocBlock, F_NEEDS_FREE | F_SIGNATURE | 2, put_desc(0x080, 40, 0, sig), 100);
    uint64_t gg = ocerz_h2g(g);
    uint64_t w = 0, w2 = 0, owned = 0, owned2 = 0;

    CHECK(ocerz_block_to_native(gg, "", &w, &owned) == OCERZ_OK && w && owned == w,
          "a guest heap block gave wrapper %#llx owned %#llx", (unsigned long long)w, (unsigned long long)owned);
    if (!w)
        return;
    const Blk *b = (const Blk *)(uintptr_t)w;
    CHECK(b->isa == (void *)_NSConcreteMallocBlock && (b->flags & F_NEEDS_FREE) && refcount(b) == 1,
          "the heap wrapper has isa %p flags %#x", b->isa, b->flags);
    CHECK(refcount(g) == 2, "the guest block's count is %d with a wrapper holding it, want 2", refcount(g));
    CHECK(ocerz_block_to_native(gg, "", &w2, &owned2) == OCERZ_OK && w2 == w && owned2 == w,
          "the same live heap block gave %#llx after %#llx", (unsigned long long)w2, (unsigned long long)w);
    CHECK(refcount(b) == 2, "a second lookup left the wrapper's count at %d, want 2", refcount(b));
    CHECK(call_native(w, 11) == 111, "native code calling the heap wrapper got %d", call_native(w, 11));
    ocerz_block_release(owned2);
    ocerz_block_release(owned);
    CHECK(refcount(g) == 1, "with its wrapper released the guest block's count is %d, want 1", refcount(g));

    uint64_t w3 = 0, owned3 = 0;
    CHECK(ocerz_block_to_native(gg, "", &w3, &owned3) == OCERZ_OK && w3 && owned3 == w3 && refcount(g) == 2,
          "a wrapper made after the first died holds count %d", refcount(g));
    CHECK(call_native(w3, 1) == 101, "the second heap wrapper answered %d", call_native(w3, 1));

    uint64_t adopted = 0;
    CHECK(ocerz_block_result_to_guest(w3, "", 0, &adopted) == OCERZ_OK && adopted == gg,
          "an adopted wrapper result became %#llx, want the guest block", (unsigned long long)adopted);
    CHECK(refcount(g) == 2, "adopting the wrapper's reference left the guest block at %d, want 2", refcount(g));
    _Block_release(g);
    _Block_release(g);
}

static void test_guest_stack(void)
{
    uint64_t sig = put_string(0x080, "i12@?0i8");
    uint64_t desc = put_desc(0x100, 40, 1, sig);
    uint64_t s = guest_blk(0x080, _NSConcreteStackBlock, F_COPY_DISP | F_SIGNATURE, desc, 30);
    uint64_t copies = counter(OFF_COPIES), disposes = counter(OFF_DISPOSES);

    uint64_t c = ocerz_block_copy_guest(s);
    CHECK(c && c != s, "copying a guest stack block gave %#llx", (unsigned long long)c);
    if (!c)
        return;
    const Blk *cb = host_blk(c);
    CHECK(cb->isa == (void *)_NSConcreteMallocBlock && (cb->flags & F_NEEDS_FREE) && refcount(cb) == 1 &&
          (cb->flags & (F_COPY_DISP | F_SIGNATURE)) == (F_COPY_DISP | F_SIGNATURE),
          "the copy has isa %p flags %#x", cb->isa, cb->flags);
    CHECK(cb->word == 30 && cb->invoke == g_guest + OFF_INVOKE, "the copy captured %llu", (unsigned long long)cb->word);
    CHECK(counter(OFF_COPIES) == copies + 1, "the guest copy helper ran %llu times, want once",
          (unsigned long long)(counter(OFF_COPIES) - copies));
    const uint64_t *sd = (const uint64_t *)(uintptr_t)cb->desc;
    uint64_t fn = 0;
    CHECK(cb->desc != desc && sd[1] == 40 && sd[4] == sig, "the copy's descriptor is not a shadow of the guest's");
    CHECK(ocerz_abi_callback_sig((const void *)(uintptr_t)sd[2], &fn) && fn == g_guest + OFF_COPY,
          "the shadow's copy word is not a slot bound to the guest's copy helper");
    CHECK(ocerz_abi_callback_sig((const void *)(uintptr_t)sd[3], &fn) && fn == g_guest + OFF_DISPOSE,
          "the shadow's dispose word is not a slot bound to the guest's dispose helper");
    uint64_t c2 = ocerz_block_copy_guest(s);
    CHECK(c2 && host_blk(c2)->desc == cb->desc, "a second copy got a second shadow descriptor");
    CHECK(ocerz_block_copy_guest(c) == c && refcount(cb) == 2, "copying the heap copy did not share it");

    _Block_release(host_blk(c));
    CHECK(counter(OFF_DISPOSES) == disposes, "a release that left a reference ran the dispose helper");
    _Block_release(host_blk(c));
    CHECK(counter(OFF_DISPOSES) == disposes + 1, "the last native release ran the guest dispose helper %llu times",
          (unsigned long long)(counter(OFF_DISPOSES) - disposes));
    _Block_release(host_blk(c2));

    uint64_t w = 0, owned = 0, inner = 0;
    copies = counter(OFF_COPIES);
    disposes = counter(OFF_DISPOSES);
    CHECK(ocerz_block_to_native(s, "", &w, &owned) == OCERZ_OK && w && owned == w,
          "a guest stack block gave wrapper %#llx", (unsigned long long)w);
    CHECK(ocerz_block_native_wrapper(w, &inner) && inner != s && host_blk(inner)->word == 30,
          "the stack block's wrapper does not hold a heap copy");
    CHECK(counter(OFF_COPIES) == copies + 1, "wrapping a stack block ran the copy helper %llu times",
          (unsigned long long)(counter(OFF_COPIES) - copies));
    CHECK(call_native(w, 12) == 42, "native code calling the stack block's wrapper got %d", call_native(w, 12));
    ocerz_block_release(owned);
    CHECK(counter(OFF_DISPOSES) == disposes + 1, "releasing the wrapper ran the dispose helper %llu times",
          (unsigned long long)(counter(OFF_DISPOSES) - disposes));
}

static void test_unsigned(void)
{
    uint64_t desc = put_desc(0x180, 40, 0, 0);
    uint64_t g = guest_blk(0x100, _NSConcreteGlobalBlock, F_GLOBAL, desc, 8);
    uint64_t w = 0, owned = 0;

    CHECK(ocerz_block_to_native(g, "i(i)", &w, &owned) == OCERZ_OK && w,
          "a guest block with no signature and a declared one was not wrapped");
    if (w)
        CHECK(call_native(w, 7) == 15, "the declared signature's wrapper answered %d", call_native(w, 7));

    uint64_t d = guest_blk(0x140, _NSConcreteGlobalBlock, F_GLOBAL, desc, 8);
    uint64_t dw = 0;
    CHECK(ocerz_block_to_native(d, "", &dw, &owned) == OCERZ_OK && dw,
          "a guest block with no signature at all was not wrapped, which refuses it before anyone calls it");
    if (!dw)
        return;
    int fds[2];
    if (pipe(fds) != 0)
        return;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        call_native(dw, 1);
        _exit(0);
    }
    close(fds[1]);
    char err[1024];
    ssize_t n = read(fds[0], err, sizeof err - 1);
    err[n > 0 ? n : 0] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 72 && strstr(err, "it has no signature"),
          "calling a wrapper of a block with no signature: status %#x, stderr %s", status, err);
}

static void test_native_views(void)
{
    uint64_t trampoline = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_BLOCK_INVOKE);
    int mul = 7;
    int (^heap)(int) = Block_copy(^(int x) { return x * mul; });
    uint64_t n = (uint64_t)(uintptr_t)heap;
    uint64_t v = 0, v2 = 0, owned = 0, owned2 = 0, inner = 0;

    CHECK(ocerz_block_to_guest(n, "", &v, &owned) == OCERZ_OK && v && owned == v,
          "a native heap block gave view %#llx owned %#llx", (unsigned long long)v, (unsigned long long)owned);
    if (!v)
        return;
    const Blk *vb = host_blk(v);
    CHECK(vb->invoke == trampoline && vb->word == n && (vb->flags & F_NEEDS_FREE) && refcount(vb) == 1,
          "the view has invoke %#llx word %#llx flags %#x", (unsigned long long)vb->invoke,
          (unsigned long long)vb->word, vb->flags);
    CHECK(refcount(heap) == 2, "the native block's count is %d with a view holding it", refcount(heap));
    CHECK(ocerz_block_to_guest(n, "", &v2, &owned2) == OCERZ_OK && v2 == v && owned2 == v,
          "the same live native block gave view %#llx after %#llx", (unsigned long long)v2, (unsigned long long)v);
    CHECK(ocerz_block_guest_view(v, &inner) && inner == n, "the view does not name its native block");
    uint64_t back = 0, bo = 1;
    CHECK(ocerz_block_to_native(v, "", &back, &bo) == OCERZ_OK && back == n && bo == 0,
          "the view handed back to native code became %#llx", (unsigned long long)back);
    CHECK(call_guest(v, 6) == 42, "guest code calling the view got %d, want 6 * 7", call_guest(v, 6));
    ocerz_block_release(owned2);
    ocerz_block_release(owned);
    CHECK(refcount(heap) == 1, "with its view released the native block's count is %d", refcount(heap));

    uint64_t adopted = 0;
    Block_copy(heap);
    CHECK(ocerz_block_result_to_guest(n, "", 0, &adopted) == OCERZ_OK && adopted,
          "an adopted native result was not given a view");
    CHECK(refcount(heap) == 2, "adopting moved the native block's count to %d, want the result's own 2",
          refcount(heap));
    ocerz_block_release(adopted);
    CHECK(refcount(heap) == 1, "releasing the adopted view left the native block at %d", refcount(heap));
    Block_release(heap);

    int (^global)(int) = ^(int x) { return x + 1; };
    uint64_t gv = 0, gv2 = 0;
    CHECK(ocerz_block_to_guest((uint64_t)(uintptr_t)global, "", &gv, &owned) == OCERZ_OK && gv && owned == 0,
          "a native global block gave view %#llx owned %#llx", (unsigned long long)gv, (unsigned long long)owned);
    CHECK(ocerz_block_to_guest((uint64_t)(uintptr_t)global, "", &gv2, &owned) == OCERZ_OK && gv2 == gv,
          "a native global block gave a second view");
    if (gv)
        CHECK((host_blk(gv)->flags & F_GLOBAL) && call_guest(gv, 41) == 42, "the global view is not global or"
              " answered %d", call_guest(gv, 41));

    int add = 3;
    int (^stack)(int) = ^(int x) { return x + add; };
    uint64_t sv = 0;
    CHECK(ocerz_block_to_guest((uint64_t)(uintptr_t)stack, "", &sv, &owned) == OCERZ_OK && sv && owned == sv,
          "a native stack block gave view %#llx", (unsigned long long)sv);
    if (sv) {
        CHECK(host_blk(sv)->word != (uint64_t)(uintptr_t)stack, "a native stack block's view holds the stack block");
        CHECK(call_guest(sv, 4) == 7, "guest code calling the stack block's view got %d", call_guest(sv, 4));
        ocerz_block_release(owned);
    }
}

static void prime_cpu(OcerzCPU *cpu, uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    uint64_t rsp = g_guest + OFF_STACK;
    ocerz_st(rsp, 8, RET_ADDR);
    cpu->gpr[OCERZ_RSP] = rsp;
    cpu->gpr[OCERZ_RDI] = rdi;
    cpu->gpr[OCERZ_RSI] = rsi;
    cpu->gpr[OCERZ_RDX] = rdx;
}

static void test_specials(void)
{
    OcerzCPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.vm = &g_vm;
    uint64_t sig = put_string(0x0c0, "i12@?0i8");
    uint64_t s = guest_blk(0x180, _NSConcreteStackBlock, F_COPY_DISP | F_SIGNATURE, put_desc(0x200, 40, 1, sig), 9);
    uint64_t copies = counter(OFF_COPIES);

    prime_cpu(&cpu, s, 0, 0);
    int rc = ocerz_block_special_copy(&g_vm, &cpu);
    uint64_t c = cpu.gpr[OCERZ_RAX];
    CHECK(rc == OCERZ_STEP_OK && c && c != s && cpu.rip == RET_ADDR && cpu.gpr[OCERZ_RSP] == g_guest + OFF_STACK + 8,
          "_Block_copy of a guest stack block: rc %d rax %#llx rip %#llx", rc, (unsigned long long)c,
          (unsigned long long)cpu.rip);
    CHECK(counter(OFF_COPIES) == copies + 1 && c && host_blk(c)->word == 9, "_Block_copy did not run the copy helper");
    if (c)
        _Block_release(host_blk(c));

    uint64_t dst = g_guest + OFF_BLOCKS + 0x800;
    ocerz_st(dst, 8, 0);
    prime_cpu(&cpu, dst, s, 7);
    rc = ocerz_block_special_object_assign(&g_vm, &cpu);
    uint64_t f = ocerz_ld(dst, 8);
    CHECK(rc == OCERZ_STEP_OK && f && f != s && host_blk(f)->isa == (void *)_NSConcreteMallocBlock &&
          cpu.rip == RET_ADDR, "_Block_object_assign of a block field stored %#llx", (unsigned long long)f);
    if (f)
        _Block_release(host_blk(f));

    uint64_t byref = g_guest + OFF_BLOCKS + 0x900;
    uint64_t *br = ocerz_g2h(byref);
    br[0] = 0;
    br[1] = byref;
    br[2] = (uint64_t)F_COPY_DISP | (48ull << 32);
    br[3] = g_guest + OFF_KEEP;
    br[4] = g_guest + OFF_DESTROY;
    br[5] = 0x1234;
    uint64_t keeps = counter(OFF_KEEPS), destroys = counter(OFF_DESTROYS);
    prime_cpu(&cpu, dst, byref, 8);
    ocerz_block_special_object_assign(&g_vm, &cpu);
    uint64_t h = ocerz_ld(dst, 8);
    CHECK(h && h != byref && br[1] == h && ocerz_ld(h + 8, 8) == h,
          "moving a __block variable left forwarding %#llx and the copy's %#llx", (unsigned long long)br[1],
          (unsigned long long)(h ? ocerz_ld(h + 8, 8) : 0));
    if (!h)
        return;
    const uint64_t *hw = ocerz_g2h(h);
    CHECK(((int32_t)hw[2] & F_REFCOUNT) == 4 && ((int32_t)hw[2] & F_NEEDS_FREE),
          "the heap __block variable has flags %#x, want NEEDS_FREE and a count of 2", (int32_t)hw[2]);
    CHECK(hw[5] == 0x1234 && counter(OFF_KEEPS) == keeps + 1, "keep ran %llu times and the value is %#llx",
          (unsigned long long)(counter(OFF_KEEPS) - keeps), (unsigned long long)hw[5]);
    uint64_t fn = 0;
    CHECK(ocerz_abi_callback_sig((const void *)(uintptr_t)hw[3], &fn) && fn == g_guest + OFF_KEEP &&
          ocerz_abi_callback_sig((const void *)(uintptr_t)hw[4], &fn) && fn == g_guest + OFF_DESTROY,
          "the heap __block variable's helpers are not slots bound to the guest's");
    ocerz_st(dst, 8, 0);
    prime_cpu(&cpu, dst, byref, 8);
    ocerz_block_special_object_assign(&g_vm, &cpu);
    CHECK(ocerz_ld(dst, 8) == h && ((int32_t)hw[2] & F_REFCOUNT) == 6,
          "a second assign gave %#llx with flags %#x", (unsigned long long)ocerz_ld(dst, 8), (int32_t)hw[2]);
    _Block_object_dispose(ocerz_g2h(byref), 8);
    _Block_object_dispose(ocerz_g2h(byref), 8);
    CHECK(counter(OFF_DESTROYS) == destroys, "destroy ran before the last dispose");
    _Block_object_dispose(ocerz_g2h(byref), 8);
    CHECK(counter(OFF_DESTROYS) == destroys + 1, "the last native dispose ran destroy %llu times",
          (unsigned long long)(counter(OFF_DESTROYS) - destroys));

    uint64_t plain = g_guest + OFF_BLOCKS + 0xa00;
    uint64_t *pw = ocerz_g2h(plain);
    pw[0] = 0;
    pw[1] = plain;
    pw[2] = 32ull << 32;
    pw[3] = 0x5678;
    prime_cpu(&cpu, dst, plain, 8);
    ocerz_block_special_object_assign(&g_vm, &cpu);
    uint64_t ph = ocerz_ld(dst, 8);
    CHECK(ph && ph != plain && pw[1] == ph && ocerz_ld(ph + 24, 8) == 0x5678,
          "a __block variable with no helpers was not copied by the native function");
    _Block_object_dispose(ocerz_g2h(plain), 8);
    _Block_object_dispose(ocerz_g2h(plain), 8);
}

static void *guest_thread(void *arg)
{
    OcerzCPU *cpu = ocerz_thread_attach(&g_vm);
    CHECK(cpu != NULL, "attaching a created thread to the test's VM returned NULL");
    if (!cpu)
        return arg;
    test_passthrough();
    test_guest_global();
    test_guest_heap();
    test_guest_stack();
    test_unsigned();
    test_native_views();
    test_specials();
    ocerz_thread_detach();
    return arg;
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
    memcpy(ocerz_g2h(g_guest), kGuest, sizeof kGuest);
    if (ocerz_vm_init(&g_vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    g_vm.jit_enabled = 0;
    g_vm.jit_plain_mem = 1;

    test_notation();
    test_trampoline();

    pthread_t th;
    int rc = pthread_create(&th, NULL, guest_thread, NULL);
    CHECK(rc == 0, "pthread_create failed with %d", rc);
    if (rc == 0)
        pthread_join(th, NULL);

    printf("test_blocks: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
