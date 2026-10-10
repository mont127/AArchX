/*
 * i386 guest blocks and host blocks (include/ocerz/m32_objc.h).
 *
 * An i386 block is {isa, flags, reserved, invoke, descriptor}; its descriptor {reserved, size, [copy, dispose],
 * [signature]}.  A guest block crossing to the host becomes a global host block (never copied or freed by the host)
 * whose invoke is a callback slot into the guest invoke, aliased to the guest block so the invoke's first argument
 * converts back to it.  A guest stack block is first copied to the guest heap, as Block_copy would, because the host
 * may run it after the frame that made it is gone (dispatch_async).  _Block_copy and friends run here on the i386
 * layout, calling the block's own copy/dispose helpers in the guest.
 *
 * ponytail: heap copies and host wrappers are never freed; a program that makes blocks per frame leaks them.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32_objcrt.h"
#include "ocerz/vm.h"

#define BLOCK_REFCOUNT_MASK 0xfffe
#define BLOCK_NEEDS_FREE (1 << 24)
#define BLOCK_HAS_COPY_DISPOSE (1 << 25)
#define BLOCK_IS_GLOBAL (1 << 28)
#define BLOCK_HAS_SIGNATURE (1 << 30)
#define BLOCK_FIELD_IS_OBJECT 3
#define BLOCK_FIELD_IS_BLOCK 7
#define BLOCK_FIELD_IS_BYREF 8
#define BLOCK_BYREF_CALLER 128

typedef struct HostDesc { unsigned long reserved, size; void *copy, *dispose; const char *signature; } HostDesc;
typedef struct HostBlock { void *isa; int flags, reserved; void *invoke; HostDesc *desc; } HostBlock;

static uint32_t isa_of(const char *name) { return m32_export("/usr/lib/libSystem.B.dylib", name); }
static uint32_t stack_isa(void) { static uint32_t a; if (!a) a = isa_of("__NSConcreteStackBlock"); return a; }
static uint32_t global_isa(void) { static uint32_t a; if (!a) a = isa_of("__NSConcreteGlobalBlock"); return a; }
static uint32_t malloc_isa(void) { static uint32_t a; if (!a) a = m32_static_alloc(16, 4); return a; }

static uint32_t desc_signature(uint32_t blk)
{
    uint32_t flags = m32_rd(blk + 4), d = m32_rd(blk + 16);
    if (!(flags & BLOCK_HAS_SIGNATURE) || !d)
        return 0;
    return m32_rd(d + ((flags & BLOCK_HAS_COPY_DISPOSE) ? 16 : 8));
}

/* the guest's Block_copy: a stack block moves to the guest heap, a heap block gains a reference */
static uint32_t block_copy(uint32_t blk)
{
    if (!blk)
        return 0;
    uint32_t isa = m32_rd(blk), flags = m32_rd(blk + 4);
    if (isa == global_isa() || (flags & BLOCK_IS_GLOBAL))
        return blk;
    if (flags & BLOCK_NEEDS_FREE) {
        m32_wr(blk + 4, (flags & ~BLOCK_REFCOUNT_MASK) | ((flags + 2) & BLOCK_REFCOUNT_MASK));
        return blk;
    }
    uint32_t d = m32_rd(blk + 16), size = d ? m32_rd(d + 4) : 20;
    uint32_t heap = m32_malloc(size < 20 ? 20 : size);
    memcpy(m32_h(heap), m32_h(blk), size);
    m32_wr(heap, malloc_isa());
    m32_wr(heap + 4, (flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 2);
    if ((flags & BLOCK_HAS_COPY_DISPOSE) && d && m32_rd(d + 8)) {
        uint32_t args[2] = { heap, blk };
        m32_call(ocerz_vm_process(), m32_rd(d + 8), args, 2, NULL, NULL);
    }
    return heap;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int m32_is_guest_block(uint32_t g)
{
    if (!g || m32_is_handle(g))
        return 0;
    uint32_t isa = m32_rd(g);
    return isa == stack_isa() || isa == global_isa() || isa == malloc_isa();
}

void *m32_block_to_host(uint32_t blk, const char *guest_sig, const char *host_sig)
{
    if (!blk)
        return NULL;
    if (m32_is_handle(blk) || m32_host(blk) != m32_h(blk)) {
        if (!m32_host(blk))
            m32_log_once("a guest block's alias is NULL:", "m32_block_to_host");
        return m32_host(blk);   /* a host block, or one already crossed */
    }
    uint32_t isa = m32_rd(blk);
    if (isa == stack_isa())
        blk = block_copy(blk);
    else if (m32_host(blk) != m32_h(blk))
        return m32_host(blk);
    char gn[512];
    uint32_t sig = desc_signature(blk);
    const char *gnote = NULL;
    if (sig && !m32_encoding_notation((const char *)m32_h(sig), 1, gn, sizeof gn))
        gnote = gn;
    else
        gnote = guest_sig ? guest_sig : "v(k)";
    if (!guest_sig && !sig)
        m32_log_once("a guest block with no signature crosses as void(^)(void):", "block");
    const char *hnote = host_sig && !sig ? host_sig : gnote;   /* the same letters: the widths follow the side */
    void *invoke = m32_callback(m32_rd(blk + 12), gnote, hnote);
    if (!invoke) {
        char why[200];
        snprintf(why, sizeof why, "invoke %#x isa %#x flags %#x %s / %s", m32_rd(blk + 12), m32_rd(blk), m32_rd(blk + 4),
                 gnote, hnote);
        m32_log_once("a guest block has no callback:", why);
        return NULL;
    }
    HostBlock *h = calloc(1, sizeof *h);
    HostDesc *d = calloc(1, sizeof *d);
    static void *global_host_isa;
    if (!global_host_isa)
        global_host_isa = m32rt_sym("_NSConcreteGlobalBlock");
    d->size = sizeof *h;
    d->signature = sig ? strdup((const char *)m32_h(sig)) : NULL;
    h->isa = global_host_isa;
    h->flags = BLOCK_IS_GLOBAL | (sig ? BLOCK_HAS_SIGNATURE : 0);
    h->invoke = invoke;
    h->desc = d;
    pthread_mutex_lock(&g_lock);
    m32_alias(blk, h);
    pthread_mutex_unlock(&g_lock);
    return h;
}

/* a host block handed to the guest: a window block whose invoke calls the host block's */
uint32_t m32_block_to_guest(void *hb)
{
    if (!hb)
        return 0;
    uint32_t g = m32_handle_twin_lookup(hb);
    if (g)
        return g;
    static const char *(*sigf)(void *);
    if (!sigf)
        sigf = (const char *(*)(void *))m32rt_sym("_Block_signature");
    const char *enc = sigf ? sigf(hb) : NULL;
    char gn[512], hn[512];
    if (!enc || m32_encoding_notation(enc, 0, hn, sizeof hn)) {
        m32_log_once("a host block with no usable signature reaches the guest as void(^)(void):", "block");
        snprintf(hn, sizeof hn, "v(k)");
    }
    snprintf(gn, sizeof gn, "%s", hn);
    uint32_t blk = m32_static_alloc(20, 4);
    m32_wr(blk, global_isa());
    m32_wr(blk + 4, BLOCK_IS_GLOBAL);
    m32_wr(blk + 12, m32_export_hostfn(((HostBlock *)hb)->invoke, gn, hn));
    m32_alias(blk, hb);
    return blk;
}

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

static int sp_Block_copy(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t b = m32_arg(cpu, 0);
    if (b && (m32_is_handle(b) || m32_host(b) != m32_h(b)))
        RET(b);   /* a host block */
    RET(block_copy(b));
}

static int sp_Block_release(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t b = m32_arg(cpu, 0);
    if (b && !m32_is_handle(b) && m32_host(b) == m32_h(b)) {
        uint32_t flags = m32_rd(b + 4);
        if ((flags & BLOCK_NEEDS_FREE) && (flags & BLOCK_REFCOUNT_MASK))
            m32_wr(b + 4, (flags & ~BLOCK_REFCOUNT_MASK) | ((flags - 2) & BLOCK_REFCOUNT_MASK));
    }
    RET(0);
}

/* _Block_object_assign(dst, src, flags): what a block's copy helper does per captured variable */
static int sp_Block_object_assign(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t dst = m32_arg(cpu, 0), src = m32_arg(cpu, 1), flags = m32_arg(cpu, 2);
    if (flags & BLOCK_BYREF_CALLER) {   /* a __block variable's own copy helper: the value is assigned as is (libclosure) */
        m32_wr(dst, src);
        RET(0);
    }
    switch (flags & 0x1f) {
    case BLOCK_FIELD_IS_OBJECT: {
        void *h = m32_objc_to_host(src);
        if (h)
            ((void *(*)(void *, SEL))M32_MSGSEND)(h, sel_registerName("retain"));
        m32_wr(dst, src);
        break;
    }
    case BLOCK_FIELD_IS_BLOCK:
        m32_wr(dst, block_copy(src));
        break;
    case BLOCK_FIELD_IS_BYREF: {   /* {isa, forwarding, flags, size, [keep, destroy], vars} */
        uint32_t fwd = m32_rd(src + 4);
        if (fwd != src) {          /* already on the heap */
            m32_wr(dst, fwd);
            break;
        }
        uint32_t size = m32_rd(src + 12), bflags = m32_rd(src + 8);
        uint32_t heap = m32_malloc(size < 16 ? 16 : size);
        memcpy(m32_h(heap), m32_h(src), size);
        m32_wr(heap + 4, heap);
        m32_wr(src + 4, heap);
        if ((bflags & BLOCK_HAS_COPY_DISPOSE) && m32_rd(src + 16)) {
            uint32_t args[2] = { heap, src };
            m32_call(vm, m32_rd(src + 16), args, 2, NULL, NULL);
        }
        m32_wr(dst, heap);
        break;
    }
    default:
        m32_wr(dst, src);
    }
    RET(0);
}

static int sp_Block_object_dispose(struct OcerzVM *vm, OcerzCPU *cpu) { RET(0); }   /* see the ponytail above */

const M32SpecialEntry m32_block_specials[] = {
    { "__Block_copy", sp_Block_copy }, { "__Block_release", sp_Block_release },
    { "__Block_object_assign", sp_Block_object_assign }, { "__Block_object_dispose", sp_Block_object_dispose },
    { NULL, NULL }
};
