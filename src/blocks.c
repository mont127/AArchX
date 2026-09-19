/*
 * Blocks in both directions between x86 guest code and native arm64 code.
 *
 * ocerz/blocks.h states what each conversion builds and why; what follows is
 * what this file knows that the header does not.
 *
 * ---- one layout, two instruction sets ----
 * libclosure's Block_layout is isa, a 32-bit flags word, 32 bits reserved,
 * invoke and descriptor, then the captures; its descriptor is reserved and
 * size, then copy and dispose when BLOCK_HAS_COPY_DISPOSE is set, then the
 * signature and the extended layout when BLOCK_HAS_SIGNATURE is.  A __block
 * variable's Block_byref is isa, forwarding, flags and size, then keep and
 * destroy when BLOCK_BYREF_HAS_COPY_DISPOSE is set, then a layout when
 * BLOCK_BYREF_LAYOUT_EXTENDED is.  clang lays all of these down identically
 * for x86_64 and arm64, with identical flags: an ARC heap block with captures
 * is 0xc1000006 on both, a global block 0x50000000.  So every word here is read
 * the same whichever side made the block, and only the function words need
 * converting.  A block whose flags carry BLOCK_SMALL_DESCRIPTOR has a relative
 * descriptor no x86 compiler emits; its signature is asked of the native
 * _Block_signature, and it is never taken for one of this file's wrappers.
 *
 * ---- the wrappers' descriptors ----
 * A wrapper's descriptor is a libclosure descriptor followed by fields of this
 * file's own, which native code never reads: the notation the wrapped block's
 * invoke is called under, parsed, and for a native wrapper the callback slot
 * already bound for it.  A native wrapper's descriptor is kept per guest
 * descriptor, because every block of one literal shares its descriptor and its
 * invoke function, so the notation is converted and the slot interned once per
 * literal rather than once per crossing; a guest view's is kept per native
 * signature string, or per declared notation for a native block without one.
 * Descriptors are never freed.  A notation that does not convert still makes a
 * descriptor, marked dead with the reason: the wrapper is made and handed over,
 * and only calling it stops the process by name, because native code and guest
 * code both hand blocks around that nobody ever calls.
 *
 * ---- lifetime ----
 * Every wrapper is a heap or a global block with native helpers, so its
 * lifetime is libclosure's own: its count lives in its flags, native
 * _Block_copy raises it, native _Block_release lowers it and at zero calls the
 * dispose helper and frees it.  The dispose helper takes the wrapper out of
 * the table that maps what it wraps to it, but only if the table still names
 * this wrapper, and then releases what it wrapped, outside the table's lock:
 * that release can run a guest dispose helper, which can convert blocks of its
 * own.  A lookup retains a wrapper it finds with _Block_tryRetain, which
 * refuses one whose count has already reached zero, so a wrapper on its way out
 * is replaced in the table rather than revived.  Global blocks, on either side,
 * never die, and neither do their wrappers.
 *
 * ---- a guest stack block ----
 * A stack block cannot be wrapped as it is, because the wrapper may outlive the
 * guest frame the block lives in, and it cannot be handed to the native
 * _Block_copy, which would call its x86 copy helper as arm64 code.  So
 * ocerz_block_copy_guest does what _Block_copy does, in the same order: copy
 * the block's bytes to the heap, reset its count to one, and run the copy
 * helper from the old block to the new, then set the new block's isa.  The
 * helper runs through a callback slot, the same way native code runs a guest
 * callback, so it runs as guest code on the calling thread and its own calls
 * to _Block_object_assign come back here.  The copy's descriptor is a shadow of
 * the guest's, kept per guest descriptor, whose copy and dispose words are
 * slots for the guest's helpers: the guest's own descriptor is in the guest's
 * constant data and is never written, and with the shadow in place the native
 * _Block_release that frees the copy runs the guest's dispose helper as guest
 * code instead of as arm64 code.  The same is done for a __block variable
 * still on the stack whose keep and destroy helpers are guest code, the heap
 * copy holding slots for them, and the stack variable's forwarding pointer is
 * pointed at the copy as libclosure does, so the guest frame's own accesses,
 * which go through forwarding, see the variable the blocks see.
 *
 * ---- calling a guest view ----
 * A guest view's invoke is a trampoline ocerz wrote into a guest page, twelve
 * bytes of the same shape as a synthesized library's stubs, so the JIT's fast
 * call reaches ocerz_block_invoke_trap from inside translated code as it
 * reaches any bridged function.  The view is in rdi, or in rsi behind a
 * structure result's pointer, and it is found by its invoke word.  The call
 * goes to the native block's invoke under the notation in the view's
 * descriptor, whose first argument unwraps the view, as a crossing like any
 * other: a bridge frame names it while native code runs, a block result is
 * borrowed, and pending guest signals are delivered on the way out.
 */
#include "ocerz/blocks.h"
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/objcbridge.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"

#include <Block.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define BLK_DEALLOCATING 0x0001
#define BLK_REFCOUNT_MASK 0xfffe
#define BLK_SMALL_DESCRIPTOR (1 << 22)
#define BLK_NEEDS_FREE (1 << 24)
#define BLK_HAS_COPY_DISPOSE (1 << 25)
#define BLK_IS_GLOBAL (1 << 28)
#define BLK_USE_STRET (1 << 29)
#define BLK_HAS_SIGNATURE (1 << 30)
#define BLK_BYREF_NEEDS_FREE (1 << 24)
#define BLK_BYREF_HAS_COPY_DISPOSE (1 << 25)
#define BLK_BYREF_LAYOUT_EXTENDED (1 << 28)
#define BLK_FIELD_IS_BLOCK 7
#define BLK_FIELD_IS_BYREF 8
#define BLK_FIELD_IS_WEAK 16
#define BLK_ALL_COPY_DISPOSE_FLAGS 0x9f
#define BLK_BUCKETS 1024
#define BLK_MAX_SIZE (1u << 24)

extern void *_NSConcreteMallocBlock[32];
extern const char *_Block_signature(void *block);
extern bool _Block_tryRetain(const void *block);

typedef struct BlkLayout {
    void *isa;
    _Atomic int32_t flags;
    int32_t reserved;
    void *invoke;
    const void *desc;
    uint64_t inner;
} BlkLayout;

typedef struct BlkByref {
    void *isa;
    struct BlkByref *forwarding;
    _Atomic int32_t flags;
    uint32_t size;
    uint64_t keep;
    uint64_t destroy;
    uint64_t layout;
} BlkByref;

typedef struct BlkDesc {
    uint64_t reserved;
    uint64_t size;
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *block);
    const char *signature;
    const char *layout;
    const char *dead;
    void *slot;
    uint64_t slot_for;
    OcerzAbiSig sig;
    char notation[OCERZ_BLOCK_NOTATION_MAX];
} BlkDesc;

typedef struct BlkNode {
    struct BlkNode *next;
    uint64_t key;
    void *val;
    char name[];
} BlkNode;

typedef struct BlkMap {
    BlkNode *bucket[BLK_BUCKETS];
} BlkMap;

static pthread_mutex_t g_blk_lock = PTHREAD_MUTEX_INITIALIZER;
static BlkMap g_blk_live_native;
static BlkMap g_blk_live_view;
static BlkMap g_blk_global_native;
static BlkMap g_blk_global_view;
static BlkMap g_blk_native_desc;
static BlkMap g_blk_view_desc;
static BlkMap g_blk_shadow;
static BlkNode *g_blk_native_desc_named;
static BlkNode *g_blk_view_desc_named;
static _Atomic uint64_t g_blk_thunk;

static void blk_native_copy_helper(void *dst, const void *src);
static void blk_native_dispose_helper(const void *block);
static void blk_view_copy_helper(void *dst, const void *src);
static void blk_view_dispose_helper(const void *block);

static unsigned blk_hash(uint64_t key)
{
    uint64_t k = key * 0x9e3779b97f4a7c15ull;
    return (unsigned)((k ^ (k >> 29)) & (BLK_BUCKETS - 1));
}

static void *blk_map_get(const BlkMap *m, uint64_t key)
{
    for (const BlkNode *n = m->bucket[blk_hash(key)]; n; n = n->next)
        if (n->key == key)
            return n->val;
    return NULL;
}

static int blk_map_put(BlkMap *m, uint64_t key, void *val)
{
    unsigned b = blk_hash(key);
    for (BlkNode *n = m->bucket[b]; n; n = n->next) {
        if (n->key == key) {
            n->val = val;
            return 1;
        }
    }
    BlkNode *n = malloc(sizeof *n);
    if (!n)
        return 0;
    n->key = key;
    n->val = val;
    n->next = m->bucket[b];
    m->bucket[b] = n;
    return 1;
}

static void blk_map_drop(BlkMap *m, uint64_t key, const void *val)
{
    BlkNode **at = &m->bucket[blk_hash(key)];
    for (; *at; at = &(*at)->next) {
        if ((*at)->key == key) {
            BlkNode *n = *at;
            if (n->val == val) {
                *at = n->next;
                free(n);
            }
            return;
        }
    }
}

static BlkLayout *blk_host(uint64_t block)
{
    return (BlkLayout *)ocerz_g2h(block);
}

static uint64_t blk_guest(const void *host)
{
    return host ? ocerz_h2g(host) : 0;
}

static uint64_t blk_thunk(void)
{
    uint64_t t = atomic_load(&g_blk_thunk);
    if (!t) {
        t = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_BLOCK_INVOKE);
        if (t)
            atomic_store(&g_blk_thunk, t);
    }
    return t;
}

static int blk_is_view(const BlkLayout *b)
{
    uint64_t t = atomic_load(&g_blk_thunk);
    return t && (uint64_t)(uintptr_t)b->invoke == t;
}

static const char *blk_signature(const BlkLayout *b, int guest)
{
    int32_t flags = b->flags;
    if (!guest && (flags & BLK_SMALL_DESCRIPTOR))
        return _Block_signature((void *)b);
    if (!(flags & BLK_HAS_SIGNATURE) || !b->desc)
        return NULL;
    const uint64_t *d = guest ? ocerz_g2h((uint64_t)(uintptr_t)b->desc) : b->desc;
    uint64_t sig = d[(flags & BLK_HAS_COPY_DISPOSE) ? 4 : 2];
    if (!sig)
        return NULL;
    return guest ? (const char *)ocerz_g2h(sig) : (const char *)(uintptr_t)sig;
}

int ocerz_block_native_wrapper(uint64_t block, uint64_t *inner)
{
    if (inner)
        *inner = 0;
    if (!block)
        return 0;
    const BlkLayout *b = blk_host(block);
    int32_t flags = b->flags;
    if (!(flags & BLK_HAS_COPY_DISPOSE) || (flags & BLK_SMALL_DESCRIPTOR) || !b->desc)
        return 0;
    const BlkDesc *d = b->desc;
    if (d->dispose != blk_native_dispose_helper)
        return 0;
    if (inner)
        *inner = b->inner;
    return 1;
}

int ocerz_block_guest_view(uint64_t block, uint64_t *inner)
{
    if (inner)
        *inner = 0;
    if (!block)
        return 0;
    const BlkLayout *b = blk_host(block);
    if (!blk_is_view(b))
        return 0;
    if (inner)
        *inner = b->inner;
    return 1;
}

int ocerz_block_is_guest_object(uint64_t obj)
{
    if (!obj || (obj & 7))
        return 0;
    const BlkLayout *b = blk_host(obj);
    void *isa = b->isa;
    if (isa != (void *)_NSConcreteStackBlock && isa != (void *)_NSConcreteMallocBlock &&
        isa != (void *)_NSConcreteGlobalBlock)
        return 0;
    return !blk_is_view(b) && ocerz_abi_is_guest_code((uint64_t)(uintptr_t)b->invoke);
}

int ocerz_block_is_guest(uint64_t block)
{
    if (!block)
        return 0;
    return ocerz_abi_is_guest_code((uint64_t)(uintptr_t)blk_host(block)->invoke);
}

static int blk_insert_self(const char *declared, char *out, size_t outlen)
{
    size_t len = strlen(declared);
    const char *open = NULL;
    int depth = 0;
    for (const char *p = declared; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}')
            depth--;
        else if (*p == '(' && depth == 0) {
            open = p;
            break;
        }
    }
    if (!open || len + 4 > outlen)
        return OCERZ_EFORMAT;
    size_t head = (size_t)(open - declared) + 1;
    memcpy(out, declared, head);
    memcpy(out + head, "k{}", 3);
    memcpy(out + head + 3, open + 1, len - head + 1);
    return OCERZ_OK;
}

int ocerz_block_invoke_notation(const char *encoding, const char *declared, char *out, size_t outlen)
{
    OcerzAbiSig sig;
    if (!out || outlen == 0)
        return OCERZ_EUNDEF;
    out[0] = '\0';
    if (encoding) {
        int nargs = 0;
        if (ocerz_objc_notation(encoding, out, outlen, &nargs, NULL, NULL) != OCERZ_OBJC_OK)
            return OCERZ_EUNSUP;
    } else if (declared && *declared) {
        if (blk_insert_self(declared, out, outlen) != OCERZ_OK)
            return OCERZ_EFORMAT;
    } else {
        return OCERZ_EUNDEF;
    }
    if (ocerz_abi_parse(out, &sig) != OCERZ_OK)
        return OCERZ_EUNSUP;
    if (sig.nargs < 1 || sig.arg[0] != 'k')
        return OCERZ_EFORMAT;
    for (int i = 0; i < sig.nargs; i++)
        if (sig.arg[i] == 'c')
            return OCERZ_EUNSUP;
    return OCERZ_OK;
}

static const char *blk_notation_refusal(int rc)
{
    switch (rc) {
    case OCERZ_EUNDEF: return "it has no signature and its declaration gives none";
    case OCERZ_EFORMAT: return "its signature does not take the block itself as its first argument";
    default: return "its signature has a type the ABI engine does not carry";
    }
}

static void blk_desc_fill(BlkDesc *d, const char *encoding, const char *declared)
{
    int rc = ocerz_block_invoke_notation(encoding, declared, d->notation, sizeof d->notation);
    if (rc == OCERZ_OK && ocerz_abi_parse(d->notation, &d->sig) != OCERZ_OK)
        rc = OCERZ_EUNSUP;
    if (rc != OCERZ_OK)
        d->dead = blk_notation_refusal(rc);
}

static BlkDesc *blk_desc_locked(BlkMap *map, BlkNode **named, uint64_t key, const char *encoding,
                                const char *declared, int view)
{
    BlkDesc *d = NULL;
    const char *name = declared ? declared : "";
    if (encoding) {
        d = blk_map_get(map, key);
    } else {
        for (BlkNode *n = *named; n && !d; n = n->next)
            if (n->key == key && strcmp(n->name, name) == 0)
                d = n->val;
    }
    if (d)
        return d;
    d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->size = sizeof(BlkLayout);
    d->copy = view ? blk_view_copy_helper : blk_native_copy_helper;
    d->dispose = view ? blk_view_dispose_helper : blk_native_dispose_helper;
    d->signature = encoding;
    blk_desc_fill(d, encoding, declared);
    if (encoding) {
        if (!blk_map_put(map, key, d)) {
            free(d);
            return NULL;
        }
        return d;
    }
    size_t len = strlen(name);
    BlkNode *n = malloc(sizeof *n + len + 1);
    if (!n) {
        free(d);
        return NULL;
    }
    memcpy(n->name, name, len + 1);
    n->key = key;
    n->val = d;
    n->next = *named;
    *named = n;
    return d;
}

static BlkDesc *blk_native_desc_locked(const BlkLayout *g, const char *encoding, const char *declared)
{
    return blk_desc_locked(&g_blk_native_desc, &g_blk_native_desc_named, (uint64_t)(uintptr_t)g->desc,
                           encoding, declared, 0);
}

static BlkDesc *blk_view_desc_locked(const char *encoding, const char *declared)
{
    return blk_desc_locked(&g_blk_view_desc, &g_blk_view_desc_named, (uint64_t)(uintptr_t)encoding,
                           encoding, declared, 1);
}

static _Noreturn void blk_dead_call(const char *what, const BlkDesc *d)
{
    fprintf(stderr, "ocerz: blocks: %s was called, and it cannot be: %s (signature %s)\n", what,
            d && d->dead ? d->dead : "it was made without a descriptor",
            d && d->signature ? d->signature : "none");
    fflush(stderr);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

static void blk_dead_invoke(void *block)
{
    const BlkLayout *b = block;
    blk_dead_call("a guest block handed to native code", b ? b->desc : NULL);
}

static void *blk_native_invoke_locked(BlkDesc *d, const BlkLayout *g)
{
    uint64_t fn = (uint64_t)(uintptr_t)g->invoke;
    if (d->dead)
        return (void *)blk_dead_invoke;
    if (d->slot && d->slot_for == fn)
        return d->slot;
    void *slot = ocerz_abi_callback_intern(fn, d->notation);
    if (!slot) {
        fprintf(stderr, "ocerz: blocks: guest block invoke %#llx under %s got no callback slot\n",
                (unsigned long long)fn, d->notation);
        return NULL;
    }
    d->slot = slot;
    d->slot_for = fn;
    return slot;
}

static BlkLayout *blk_make(void *isa, int32_t flags, void *invoke, const BlkDesc *d, uint64_t inner)
{
    BlkLayout *w = malloc(sizeof *w);
    if (!w)
        return NULL;
    w->isa = isa;
    atomic_store(&w->flags, flags);
    w->reserved = 0;
    w->invoke = invoke;
    w->desc = d;
    w->inner = inner;
    return w;
}

static int32_t blk_wrapper_flags(const BlkLayout *inner, const BlkDesc *d, int global)
{
    int32_t flags = BLK_HAS_COPY_DISPOSE | (inner->flags & BLK_USE_STRET);
    if (d->signature)
        flags |= BLK_HAS_SIGNATURE;
    return flags | (global ? BLK_IS_GLOBAL : BLK_NEEDS_FREE | 2);
}

static int blk_wrap_guest(BlkLayout *g, const char *declared, uint64_t *out, uint64_t *owned)
{
    int global = (g->flags & BLK_IS_GLOBAL) != 0;
    BlkMap *map = global ? &g_blk_global_native : &g_blk_live_native;
    uint64_t key = (uint64_t)(uintptr_t)g;
    const char *encoding = blk_signature(g, 1);

    pthread_mutex_lock(&g_blk_lock);
    BlkLayout *w = blk_map_get(map, key);
    if (w && (global || _Block_tryRetain(w))) {
        pthread_mutex_unlock(&g_blk_lock);
        *out = (uint64_t)(uintptr_t)w;
        *owned = global ? 0 : (uint64_t)(uintptr_t)w;
        return OCERZ_OK;
    }
    BlkDesc *d = blk_native_desc_locked(g, encoding, declared);
    void *invoke = d ? blk_native_invoke_locked(d, g) : NULL;
    if (invoke) {
        w = blk_make(global ? (void *)_NSConcreteGlobalBlock : (void *)_NSConcreteMallocBlock,
                     blk_wrapper_flags(g, d, global), invoke, d, blk_guest(g));
        if (w && !blk_map_put(map, key, w)) {
            free(w);
            w = NULL;
        }
        if (w && !global)
            _Block_copy(g);
    }
    pthread_mutex_unlock(&g_blk_lock);
    if (!w) {
        fprintf(stderr, "ocerz: blocks: guest block %#llx could not be given a native wrapper\n",
                (unsigned long long)blk_guest(g));
        return OCERZ_ENOMEM;
    }
    *out = (uint64_t)(uintptr_t)w;
    *owned = global ? 0 : (uint64_t)(uintptr_t)w;
    return OCERZ_OK;
}

int ocerz_block_to_native(uint64_t gblock, const char *declared, uint64_t *out, uint64_t *owned)
{
    uint64_t scratch;
    if (!owned)
        owned = &scratch;
    *owned = 0;
    if (!out)
        return OCERZ_EUNDEF;
    *out = 0;
    if (!gblock)
        return OCERZ_OK;

    BlkLayout *g = blk_host(gblock);
    if (blk_is_view(g)) {
        *out = g->inner;
        return OCERZ_OK;
    }
    if (!ocerz_abi_is_guest_code((uint64_t)(uintptr_t)g->invoke)) {
        *out = (uint64_t)(uintptr_t)g;
        return OCERZ_OK;
    }
    if (g->flags & (BLK_IS_GLOBAL | BLK_NEEDS_FREE))
        return blk_wrap_guest(g, declared, out, owned);

    uint64_t copy = ocerz_block_copy_guest(gblock);
    if (!copy)
        return OCERZ_ENOMEM;
    int rc = blk_wrap_guest(blk_host(copy), declared, out, owned);
    _Block_release(blk_host(copy));
    return rc;
}

static int blk_view_native(BlkLayout *n, const char *declared, uint64_t *out, uint64_t *owned)
{
    int global = (n->flags & BLK_IS_GLOBAL) != 0;
    BlkMap *map = global ? &g_blk_global_view : &g_blk_live_view;
    uint64_t key = (uint64_t)(uintptr_t)n;
    const char *encoding = blk_signature(n, 0);
    uint64_t thunk = blk_thunk();

    if (!thunk) {
        fprintf(stderr, "ocerz: blocks: no guest page holds the trampoline a native block needs\n");
        return OCERZ_ENOMEM;
    }
    pthread_mutex_lock(&g_blk_lock);
    BlkLayout *v = blk_map_get(map, key);
    if (v && (global || _Block_tryRetain(v))) {
        pthread_mutex_unlock(&g_blk_lock);
        *out = blk_guest(v);
        *owned = global ? 0 : (uint64_t)(uintptr_t)v;
        return OCERZ_OK;
    }
    BlkDesc *d = blk_view_desc_locked(encoding, declared);
    v = NULL;
    if (d) {
        v = blk_make(global ? (void *)_NSConcreteGlobalBlock : (void *)_NSConcreteMallocBlock,
                     blk_wrapper_flags(n, d, global), (void *)(uintptr_t)thunk, d,
                     (uint64_t)(uintptr_t)n);
        if (v && !blk_map_put(map, key, v)) {
            free(v);
            v = NULL;
        }
        if (v && !global)
            _Block_copy(n);
    }
    pthread_mutex_unlock(&g_blk_lock);
    if (!v) {
        fprintf(stderr, "ocerz: blocks: native block %p could not be given a guest view\n", (void *)n);
        return OCERZ_ENOMEM;
    }
    *out = blk_guest(v);
    *owned = global ? 0 : (uint64_t)(uintptr_t)v;
    return OCERZ_OK;
}

int ocerz_block_to_guest(uint64_t nblock, const char *declared, uint64_t *out, uint64_t *owned)
{
    uint64_t scratch, inner;
    if (!owned)
        owned = &scratch;
    *owned = 0;
    if (!out)
        return OCERZ_EUNDEF;
    *out = 0;
    if (!nblock)
        return OCERZ_OK;

    BlkLayout *n = (BlkLayout *)(uintptr_t)nblock;
    if (ocerz_block_native_wrapper(nblock, &inner)) {
        *out = inner;
        return OCERZ_OK;
    }
    if (ocerz_abi_is_guest_code((uint64_t)(uintptr_t)n->invoke)) {
        *out = blk_guest(n);
        return OCERZ_OK;
    }
    if (n->flags & (BLK_IS_GLOBAL | BLK_NEEDS_FREE))
        return blk_view_native(n, declared, out, owned);

    void *copy = _Block_copy(n);
    if (!copy)
        return OCERZ_ENOMEM;
    int rc = blk_view_native(copy, declared, out, owned);
    _Block_release(copy);
    return rc;
}

void ocerz_block_release(uint64_t block)
{
    if (block)
        _Block_release((const void *)(uintptr_t)block);
}

static void blk_autorelease(uint64_t block)
{
    static void *(*autorelease)(void *);
    static _Atomic int looked;
    if (!atomic_load(&looked)) {
        autorelease = (void *(*)(void *))dlsym(RTLD_DEFAULT, "objc_autorelease");
        atomic_store(&looked, 1);
    }
    if (autorelease)
        autorelease((void *)(uintptr_t)block);
}

int ocerz_block_result_to_guest(uint64_t nblock, const char *declared, int borrowed, uint64_t *out)
{
    uint64_t owned = 0, inner = 0;
    if (!out)
        return OCERZ_EUNDEF;
    *out = 0;
    int unwrapped = ocerz_block_native_wrapper(nblock, &inner);
    int rc = ocerz_block_to_guest(nblock, declared, out, &owned);
    if (rc != OCERZ_OK)
        return rc;
    if (borrowed) {
        if (owned)
            blk_autorelease(owned);
    } else if (owned) {
        _Block_release((const void *)(uintptr_t)nblock);
    } else if (unwrapped) {
        _Block_copy(blk_host(inner));
        _Block_release((const void *)(uintptr_t)nblock);
    }
    return OCERZ_OK;
}

int ocerz_block_result_to_native(uint64_t gblock, const char *declared, uint64_t *out)
{
    uint64_t owned = 0;
    int rc = ocerz_block_to_native(gblock, declared, out, &owned);
    if (rc == OCERZ_OK && owned)
        blk_autorelease(owned);
    return rc;
}

static void blk_native_copy_helper(void *dst, const void *src)
{
    const BlkLayout *s = src;
    BlkLayout *d = dst;
    d->inner = s->inner;
    _Block_copy(blk_host(s->inner));
}

static void blk_native_dispose_helper(const void *block)
{
    const BlkLayout *w = block;
    pthread_mutex_lock(&g_blk_lock);
    blk_map_drop(&g_blk_live_native, (uint64_t)(uintptr_t)blk_host(w->inner), w);
    pthread_mutex_unlock(&g_blk_lock);
    _Block_release(blk_host(w->inner));
}

static void blk_view_copy_helper(void *dst, const void *src)
{
    const BlkLayout *s = src;
    BlkLayout *d = dst;
    d->inner = (uint64_t)(uintptr_t)_Block_copy((const void *)(uintptr_t)s->inner);
}

static void blk_view_dispose_helper(const void *block)
{
    const BlkLayout *v = block;
    pthread_mutex_lock(&g_blk_lock);
    blk_map_drop(&g_blk_live_view, v->inner, v);
    pthread_mutex_unlock(&g_blk_lock);
    _Block_release((const void *)(uintptr_t)v->inner);
}

static int blk_helper_slot(uint64_t fn, const char *notation, uint64_t *out)
{
    if (ocerz_abi_callback_convert(fn, notation, out) != OCERZ_OK) {
        fprintf(stderr, "ocerz: blocks: guest helper %#llx could not be bound to a callback slot\n",
                (unsigned long long)fn);
        return 0;
    }
    *out = *out ? (uint64_t)(uintptr_t)ocerz_g2h(*out) : 0;
    return 1;
}

static const uint64_t *blk_shadow(uint64_t gdesc, int32_t flags)
{
    pthread_mutex_lock(&g_blk_lock);
    uint64_t *s = blk_map_get(&g_blk_shadow, gdesc);
    pthread_mutex_unlock(&g_blk_lock);
    if (s)
        return s;

    s = calloc(6, sizeof *s);
    if (!s)
        return NULL;
    const uint64_t *d = ocerz_g2h(gdesc);
    s[0] = d[0];
    s[1] = d[1];
    if (!blk_helper_slot(d[2], "v(pp)", &s[2]) || !blk_helper_slot(d[3], "v(p)", &s[3])) {
        free(s);
        return NULL;
    }
    if (flags & BLK_HAS_SIGNATURE) {
        s[4] = d[4] ? (uint64_t)(uintptr_t)ocerz_g2h(d[4]) : 0;
        s[5] = d[5];
    }

    pthread_mutex_lock(&g_blk_lock);
    uint64_t *had = blk_map_get(&g_blk_shadow, gdesc);
    if (!had && !blk_map_put(&g_blk_shadow, gdesc, s))
        had = NULL;
    pthread_mutex_unlock(&g_blk_lock);
    if (had) {
        free(s);
        return had;
    }
    return s;
}

static _Noreturn void blk_stop(const char *fmt, unsigned long long a)
{
    fputs("ocerz: blocks: ", stderr);
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    fflush(stderr);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

uint64_t ocerz_block_copy_guest(uint64_t gblock)
{
    if (!gblock)
        return 0;
    BlkLayout *b = blk_host(gblock);
    int32_t flags = b->flags;
    if ((flags & (BLK_NEEDS_FREE | BLK_IS_GLOBAL)) || blk_is_view(b) ||
        !ocerz_abi_is_guest_code((uint64_t)(uintptr_t)b->invoke))
        return blk_guest(_Block_copy(b));

    uint64_t gdesc = (uint64_t)(uintptr_t)b->desc;
    uint64_t size = gdesc ? ocerz_ld(gdesc + 8, 8) : 0;
    if (size < 32 || size > BLK_MAX_SIZE)
        blk_stop("guest stack block %#llx has a descriptor ocerz cannot read, so it cannot be copied",
                 (unsigned long long)gblock);

    BlkLayout *r = malloc((size_t)size);
    if (!r)
        blk_stop("no memory to copy guest block %#llx", (unsigned long long)gblock);
    memmove(r, b, (size_t)size);
    atomic_store(&r->flags, (flags & ~(BLK_REFCOUNT_MASK | BLK_DEALLOCATING)) | BLK_NEEDS_FREE | 2);
    if (flags & BLK_HAS_COPY_DISPOSE) {
        const uint64_t *shadow = blk_shadow(gdesc, flags);
        if (!shadow)
            blk_stop("guest block %#llx has helpers that could not be bound, so it cannot be copied",
                     (unsigned long long)gblock);
        r->desc = shadow;
        ((void (*)(void *, const void *))(uintptr_t)shadow[2])(r, b);
    }
    r->isa = (void *)_NSConcreteMallocBlock;
    return blk_guest(r);
}

static uint64_t blk_byref_copy(uint64_t gsrc, int flags)
{
    BlkByref *src = ocerz_g2h(gsrc);
    BlkByref *fwd = src ? src->forwarding : NULL;
    int32_t sflags = src ? src->flags : 0;
    if (fwd && (fwd->flags & BLK_REFCOUNT_MASK) == 0 && (sflags & BLK_BYREF_HAS_COPY_DISPOSE) &&
        (ocerz_abi_is_guest_code(src->keep) || ocerz_abi_is_guest_code(src->destroy))) {
        if (src->size < offsetof(BlkByref, layout) || src->size > BLK_MAX_SIZE)
            blk_stop("guest __block variable %#llx has a size ocerz cannot copy", (unsigned long long)gsrc);
        BlkByref *copy = calloc(1, src->size);
        if (!copy)
            blk_stop("no memory to copy guest __block variable %#llx", (unsigned long long)gsrc);
        uint64_t keep, destroy;
        if (!blk_helper_slot(src->keep, "v(pp)", &keep) || !blk_helper_slot(src->destroy, "v(p)", &destroy))
            blk_stop("guest __block variable %#llx has helpers that could not be bound",
                     (unsigned long long)gsrc);
        copy->isa = NULL;
        atomic_store(&copy->flags, sflags | BLK_BYREF_NEEDS_FREE | 4);
        copy->forwarding = copy;
        src->forwarding = copy;
        copy->size = src->size;
        copy->keep = keep;
        copy->destroy = destroy;
        if ((sflags & BLK_BYREF_LAYOUT_EXTENDED) && src->size >= sizeof(BlkByref))
            copy->layout = src->layout;
        ((void (*)(void *, void *))(uintptr_t)keep)(copy, src);
        return blk_guest(copy);
    }
    void *dst = NULL;
    _Block_object_assign(&dst, src, flags);
    return blk_guest(dst);
}

static void blk_return(OcerzCPU *cpu, uint64_t rax)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = rax;
}

static int blk_settle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (ocerz_peek_pending_async_sig() || (cpu->sig_pending & ~cpu->sig_mask))
        ocerz_guest_deliver_pending(vm, cpu);
    return OCERZ_STEP_OK;
}

int ocerz_block_special_copy(struct OcerzVM *vm, OcerzCPU *cpu)
{
    blk_return(cpu, ocerz_block_copy_guest(cpu->gpr[OCERZ_RDI]));
    return blk_settle(vm, cpu);
}

int ocerz_block_special_object_assign(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t dst = cpu->gpr[OCERZ_RDI];
    uint64_t src = cpu->gpr[OCERZ_RSI];
    int flags = (int)(uint32_t)cpu->gpr[OCERZ_RDX];
    switch (flags & BLK_ALL_COPY_DISPOSE_FLAGS) {
    case BLK_FIELD_IS_BLOCK:
        ocerz_st(dst, 8, ocerz_block_copy_guest(src));
        break;
    case BLK_FIELD_IS_BYREF:
    case BLK_FIELD_IS_BYREF | BLK_FIELD_IS_WEAK:
        ocerz_st(dst, 8, blk_byref_copy(src, flags));
        break;
    default:
        _Block_object_assign(ocerz_g2h(dst), src ? ocerz_g2h(src) : NULL, flags);
        break;
    }
    blk_return(cpu, 0);
    return blk_settle(vm, cpu);
}

int ocerz_block_invoke_trap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t view = cpu->gpr[OCERZ_RDI];
    if (!ocerz_block_guest_view(view, NULL)) {
        view = cpu->gpr[OCERZ_RSI];
        if (!ocerz_block_guest_view(view, NULL))
            blk_stop("the trampoline for native blocks was entered with no native block's guest view in"
                     " rdi or rsi (rdi %#llx)", (unsigned long long)cpu->gpr[OCERZ_RDI]);
    }
    const BlkLayout *v = blk_host(view);
    const BlkDesc *d = v->desc;
    const BlkLayout *n = (const BlkLayout *)(uintptr_t)v->inner;
    if (d->dead)
        blk_dead_call("a native block handed to guest code", d);

    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, "(native block)", d->notation, n->invoke);
    int rc = ocerz_abi_perform_borrowed(&d->sig, n->invoke, cpu);
    ocerz_bridge_lower(&outer);
    if (rc != OCERZ_STEP_OK)
        return rc;
    return blk_settle(vm, cpu);
}
