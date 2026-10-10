/*
 * m32's threads (include/ocerz/m32.h): pthread_create and friends, pthread objects, keys, and
 * thread-local variables.
 *
 * i386 pthread objects are smaller than arm64's (a mutex is 44 bytes, not 64), so the host object lives beside
 * them: the first time a guest object is used its signature word becomes M32_PTHREAD_MAGIC and the next 8 bytes hold
 * a pointer to a host object created from the guest's own initializer (PTHREAD_MUTEX_INITIALIZER, a recursive one,
 * pthread_mutex_init with its attributes).  Every later call finds the host object in O(1) and never takes a lock
 * of ours.  Attribute objects keep their settings in the guest structure itself.
 */
#include <errno.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"
#include "ocerz/vm.h"

#define M32_PTHREAD_MAGIC 0x4d333221u   /* "!23M": the guest object has a host twin at +4 */
#define MUTEX_SIG_INIT    0x32aaaba7u    /* _PTHREAD_MUTEX_SIG_init */
#define MUTEX_SIG_ERRCHK  0x32aaaba1u
#define MUTEX_SIG_RECUR   0x32aaaba2u
#define COND_SIG_INIT     0x3cb0b1bbu
#define RWLOCK_SIG_INIT   0x2da8b3b4u
#define ATTR_MAGIC        0x4d334154u    /* our pthread_attr_t / mutexattr_t / condattr_t contents */

#define RETI(v) do { int r_ = (v); m32_ret(cpu, (uint32_t)r_, 0, 0); return OCERZ_STEP_OK; } while (0)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *twin(uint32_t g)
{
    void *h;
    memcpy(&h, m32_h(g + 4), sizeof h);
    return h;
}

static void set_twin(uint32_t g, void *h)
{
    memcpy(m32_h(g + 4), &h, sizeof h);
    __atomic_store_n((uint32_t *)m32_h(g), M32_PTHREAD_MAGIC, __ATOMIC_RELEASE);
}

static int mutex_type_of_attr(uint32_t attr)
{
    return attr && m32_rd(attr) == ATTR_MAGIC ? (int)m32_rd(attr + 4) : PTHREAD_MUTEX_DEFAULT;
}

static pthread_mutex_t *mutex_of(uint32_t g)
{
    if (__atomic_load_n((uint32_t *)m32_h(g), __ATOMIC_ACQUIRE) == M32_PTHREAD_MAGIC)
        return twin(g);
    pthread_mutex_lock(&g_lock);
    pthread_mutex_t *m;
    if (m32_rd(g) == M32_PTHREAD_MAGIC)
        m = twin(g);
    else {
        uint32_t sig = m32_rd(g);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, sig == MUTEX_SIG_RECUR ? PTHREAD_MUTEX_RECURSIVE
                                      : sig == MUTEX_SIG_ERRCHK ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_DEFAULT);
        m = malloc(sizeof *m);
        pthread_mutex_init(m, &a);
        pthread_mutexattr_destroy(&a);
        set_twin(g, m);
    }
    pthread_mutex_unlock(&g_lock);
    return m;
}

static pthread_cond_t *cond_of(uint32_t g)
{
    if (__atomic_load_n((uint32_t *)m32_h(g), __ATOMIC_ACQUIRE) == M32_PTHREAD_MAGIC)
        return twin(g);
    pthread_mutex_lock(&g_lock);
    pthread_cond_t *c;
    if (m32_rd(g) == M32_PTHREAD_MAGIC)
        c = twin(g);
    else {
        c = malloc(sizeof *c);
        pthread_cond_init(c, NULL);
        set_twin(g, c);
    }
    pthread_mutex_unlock(&g_lock);
    return c;
}

static pthread_rwlock_t *rwlock_of(uint32_t g)
{
    if (__atomic_load_n((uint32_t *)m32_h(g), __ATOMIC_ACQUIRE) == M32_PTHREAD_MAGIC)
        return twin(g);
    pthread_mutex_lock(&g_lock);
    pthread_rwlock_t *l;
    if (m32_rd(g) == M32_PTHREAD_MAGIC)
        l = twin(g);
    else {
        l = malloc(sizeof *l);
        pthread_rwlock_init(l, NULL);
        set_twin(g, l);
    }
    pthread_mutex_unlock(&g_lock);
    return l;
}

/* ---- mutexes ---- */
static int sp_mutex_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, mutex_type_of_attr(m32_arg(cpu, 1)));
    pthread_mutex_t *m = malloc(sizeof *m);
    int r = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    set_twin(g, m);
    RETI(r);
}
static int sp_mutex_destroy(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    if (m32_rd(g) == M32_PTHREAD_MAGIC) {
        pthread_mutex_t *m = twin(g);
        pthread_mutex_destroy(m);
        free(m);
        m32_wr(g, 0);
    }
    RETI(0);
}
static int sp_mutex_lock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_mutex_lock(mutex_of(m32_arg(cpu, 0)))); }
static int sp_mutex_trylock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_mutex_trylock(mutex_of(m32_arg(cpu, 0)))); }
static int sp_mutex_unlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_mutex_unlock(mutex_of(m32_arg(cpu, 0)))); }

static int sp_mutexattr_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    m32_wr(g, ATTR_MAGIC);
    m32_wr(g + 4, PTHREAD_MUTEX_DEFAULT);
    RETI(0);
}
static int sp_mutexattr_settype(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_wr(m32_arg(cpu, 0) + 4, m32_arg(cpu, 1));
    RETI(0);
}
static int sp_mutexattr_gettype(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + 4));
    RETI(0);
}
static int sp_ok(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(0); }

/* ---- condition variables ---- */
static int sp_cond_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_cond_t *c = malloc(sizeof *c);
    int r = pthread_cond_init(c, NULL);
    set_twin(m32_arg(cpu, 0), c);
    RETI(r);
}
static int sp_cond_destroy(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    if (m32_rd(g) == M32_PTHREAD_MAGIC) {
        pthread_cond_t *c = twin(g);
        pthread_cond_destroy(c);
        free(c);
        m32_wr(g, 0);
    }
    RETI(0);
}
/* OCERZ_M32LOG=cond: each wait and signal, by thread, with the guest's callers */
static void log_cond(const OcerzCPU *cpu, const char *what)
{
    static int log = -1;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "cond");
    if (!log)
        return;
    uint32_t bp = (uint32_t)cpu->gpr[OCERZ_RBP];
    fprintf(stderr, "ocerz: m32: %s [t%u] %#x from %#x", what, (unsigned)pthread_mach_thread_np(pthread_self()),
            m32_arg(cpu, 0), m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    for (int i = 0; i < 6 && bp && bp < M32_HANDLE_LO && !(bp & 3); i++, bp = m32_rd(bp))
        fprintf(stderr, " %#x", m32_rd(bp + 4));
    fprintf(stderr, "\n");
}
static int sp_cond_wait(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_cond(cpu, "cond_wait");
    RETI(pthread_cond_wait(cond_of(m32_arg(cpu, 0)), mutex_of(m32_arg(cpu, 1))));
}
static struct timespec ts_in(uint32_t g) { struct timespec t = { (int32_t)m32_rd(g), (int32_t)m32_rd(g + 4) }; return t; }
static int sp_cond_timedwait(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_cond(cpu, "cond_timedwait");
    struct timespec t = ts_in(m32_arg(cpu, 2));
    int r = pthread_cond_timedwait(cond_of(m32_arg(cpu, 0)), mutex_of(m32_arg(cpu, 1)), &t);
    if (r == ETIMEDOUT)
        log_cond(cpu, "cond_timeout");
    RETI(r);
}
static int sp_cond_timedwait_rel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_cond(cpu, "cond_timedwait_rel");
    struct timespec t = ts_in(m32_arg(cpu, 2));
    RETI(pthread_cond_timedwait_relative_np(cond_of(m32_arg(cpu, 0)), mutex_of(m32_arg(cpu, 1)), &t));
}
static int sp_cond_signal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_cond(cpu, "cond_signal");
    RETI(pthread_cond_signal(cond_of(m32_arg(cpu, 0))));
}
static int sp_cond_broadcast(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_cond(cpu, "cond_broadcast");
    RETI(pthread_cond_broadcast(cond_of(m32_arg(cpu, 0))));
}

/* ---- read-write locks ---- */
static int sp_rwlock_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_rwlock_t *l = malloc(sizeof *l);
    int r = pthread_rwlock_init(l, NULL);
    set_twin(m32_arg(cpu, 0), l);
    RETI(r);
}
static int sp_rwlock_destroy(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(0); }   /* ponytail: the twin is kept */
static int sp_rwlock_rdlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_rwlock_rdlock(rwlock_of(m32_arg(cpu, 0)))); }
static int sp_rwlock_wrlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_rwlock_wrlock(rwlock_of(m32_arg(cpu, 0)))); }
static int sp_rwlock_tryrdlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_rwlock_tryrdlock(rwlock_of(m32_arg(cpu, 0)))); }
static int sp_rwlock_trywrlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_rwlock_trywrlock(rwlock_of(m32_arg(cpu, 0)))); }
static int sp_rwlock_unlock(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_rwlock_unlock(rwlock_of(m32_arg(cpu, 0)))); }

/* ---- once: word 1 of the 8-byte pthread_once_t is 1 when done ---- */
static pthread_mutex_t g_once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static int sp_once(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0), fn = m32_arg(cpu, 1);
    if (__atomic_load_n((uint32_t *)m32_h(g + 4), __ATOMIC_ACQUIRE) == 1)
        RETI(0);
    pthread_mutex_lock(&g_once_lock);
    if (m32_rd(g + 4) != 1) {
        m32_call(vm, fn, NULL, 0, NULL, NULL);
        __atomic_store_n((uint32_t *)m32_h(g + 4), 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_once_lock);
    RETI(0);
}

/* ---- thread attributes: magic, stack size, detach state ---- */
static int sp_attr_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    memset(m32_h(g), 0, 40);
    m32_wr(g, ATTR_MAGIC);
    m32_wr(g + 4, 512u << 10);
    m32_wr(g + 8, PTHREAD_CREATE_JOINABLE);
    RETI(0);
}
static int sp_attr_setstacksize(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 0) + 4, m32_arg(cpu, 1)); RETI(0); }
static int sp_attr_getstacksize(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + 4)); RETI(0); }
static int sp_attr_setdetachstate(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 0) + 8, m32_arg(cpu, 1)); RETI(0); }
static int sp_attr_getdetachstate(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + 8)); RETI(0); }

/* pthread_setname_np(name): the host thread gets it too (sample, the crash reports) */
static int sp_setname(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *name = (const char *)m32_h(m32_arg(cpu, 0));
    char buf[64];
    snprintf(buf, sizeof buf, "%s", name ? name : "");
    RETI(pthread_setname_np(buf));
}

/* ---- threads ---- */
typedef struct Start { struct OcerzVM *vm; uint32_t fn, arg, stack; } Start;

static void *thread_main(void *p)
{
    Start s = *(Start *)p;
    free(p);
    OcerzCPU *cpu = m32_thread_attach(s.vm);
    if (!cpu)
        return NULL;
    /* A game's threads are interactive work.  At the default QoS macOS sometimes parks Batman's render thread on the
     * efficiency cores for good (7x slower frames, glstat cpus=0-3); the game's own pthread_setschedparam is not
     * applied (sp_setschedparam), since a fixed priority would take the thread out of QoS again. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint32_t r = m32_call(s.vm, s.fn, &s.arg, 1, NULL, NULL);
    return (void *)(uintptr_t)r;
}

/* pthread_create, and pthread_create_suspended_np: Bink's decoders start suspended and run on thread_resume(port),
 * which reaches the host thread directly (pthread_mach_thread_np of our handle is the host thread's port) */
static int create(struct OcerzVM *vm, OcerzCPU *cpu, int suspended)
{
    uint32_t out = m32_arg(cpu, 0), attr = m32_arg(cpu, 1);
    Start *s = malloc(sizeof *s);
    s->vm = vm;
    s->fn = m32_arg(cpu, 2);
    s->arg = m32_arg(cpu, 3);
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (attr && m32_rd(attr) == ATTR_MAGIC) {
        if (m32_rd(attr + 8) == PTHREAD_CREATE_DETACHED)
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    }
    pthread_attr_setstacksize(&a, 1u << 20);   /* the host side: the guest stack is the attached cpu's own */
    pthread_t t;
    int r = suspended ? pthread_create_suspended_np(&t, &a, thread_main, s) : pthread_create(&t, &a, thread_main, s);
    pthread_attr_destroy(&a);
    if (r)
        free(s);
    else if (out)
        m32_wr(out, m32_handle(t));
    RETI(r);
}
static int sp_pthread_create(struct OcerzVM *vm, OcerzCPU *cpu) { return create(vm, cpu, 0); }
static int sp_pthread_create_suspended(struct OcerzVM *vm, OcerzCPU *cpu) { return create(vm, cpu, 1); }
/* pthread_setschedparam(thread, policy, param): not applied, the thread keeps the QoS thread_main gave it (Batman
 * asks SCHED_OTHER 31-33); OCERZ_M32LOG=sched prints what the game asks */
static int sp_setschedparam(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_t t = m32_host(m32_arg(cpu, 0));
    int policy = (int)m32_arg(cpu, 1);
    struct sched_param p = { .sched_priority = (int)m32_rd(m32_arg(cpu, 2)) };
    if (getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "sched"))
        fprintf(stderr, "ocerz: m32: pthread_setschedparam [t%u] policy=%d priority=%d from %#x\n",
                (unsigned)pthread_mach_thread_np(t), policy, p.sched_priority, m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    RETI(0);
}
static int sp_pthread_join(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *v = NULL;
    int r = pthread_join(m32_host(m32_arg(cpu, 0)), &v);
    uint32_t out = m32_arg(cpu, 1);
    if (!r && out)
        m32_wr(out, (uint32_t)(uintptr_t)v);
    RETI(r);
}
static int sp_pthread_self(struct OcerzVM *vm, OcerzCPU *cpu) { RETI((int)m32_handle(pthread_self())); }
static int sp_pthread_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    /* end the guest function this thread was started with: its m32_call returns the value */
    cpu->gpr[OCERZ_RAX] = m32_arg(cpu, 0);
    cpu->rip = M32_TRAP(M32_ID_SENTINEL);
    return OCERZ_STEP_OK;
}

/* ---- keys: values are guest words kept as host pointers ---- */
static int sp_key_create(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_key_t k;
    uint32_t dtor = m32_arg(cpu, 1);
    void (*hd)(void *) = dtor ? (void (*)(void *))m32_callback(dtor, "v(u)", "v(L)") : NULL;
    int r = pthread_key_create(&k, hd);
    m32_wr(m32_arg(cpu, 0), (uint32_t)k);
    RETI(r);
}
static int sp_key_delete(struct OcerzVM *vm, OcerzCPU *cpu) { RETI(pthread_key_delete(m32_arg(cpu, 0))); }
static int sp_getspecific(struct OcerzVM *vm, OcerzCPU *cpu) { RETI((int)(uintptr_t)pthread_getspecific(m32_arg(cpu, 0))); }
static int sp_setspecific(struct OcerzVM *vm, OcerzCPU *cpu)
{
    RETI(pthread_setspecific(m32_arg(cpu, 0), (void *)(uintptr_t)m32_arg(cpu, 1)));
}

static int sp_sched_yield(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int log = -1;
    static unsigned n;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "yield");
    if (log && !(__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) & 4095)) {   /* OCERZ_M32LOG=yield: who spins */
        uint32_t bp = (uint32_t)cpu->gpr[OCERZ_RBP];
        fprintf(stderr, "ocerz: m32: sched_yield [t%u] from %#x", (unsigned)pthread_mach_thread_np(pthread_self()),
                m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
        for (int i = 0; i < 12 && bp && bp < M32_HANDLE_LO && !(bp & 3); i++, bp = m32_rd(bp))
            fprintf(stderr, " %#x", m32_rd(bp + 4));
        fprintf(stderr, "\n");
    }
    RETI(sched_yield());
}

/* ---- thread-local variables ----
 * i386 descriptors in __thread_vars are {thunk, key, offset}.  The loader points every thunk at the M32_ID_TLV trap
 * and puts the image's TLV index in key; the trap takes the descriptor in EAX and answers the address in EAX,
 * changing no other register (the i386 TLV contract). */
typedef struct TlvImage { uint32_t start, size, init_size; } TlvImage;
static TlvImage g_tlv[M32_MAX_IMAGES];
static int g_ntlv;
static __thread uint32_t t_tlv[M32_MAX_IMAGES];

void m32_tlv_register(M32Image *img)
{
    const M32Sect *vars = m32_section(img, "__DATA", "__thread_vars");
    if (!vars)
        return;
    uint32_t lo = 0xffffffffu, hi = 0, init_hi = 0;
    for (int i = 0; i < img->nsect; i++) {
        uint32_t type = img->sect[i].flags & SECTION_TYPE;
        if (type != S_THREAD_LOCAL_REGULAR && type != S_THREAD_LOCAL_ZEROFILL)
            continue;
        uint32_t a = img->sect[i].addr + (uint32_t)img->slide;
        if (a < lo) lo = a;
        if (a + img->sect[i].size > hi) hi = a + img->sect[i].size;
        if (type == S_THREAD_LOCAL_REGULAR && a + img->sect[i].size > init_hi) init_hi = a + img->sect[i].size;
    }
    pthread_mutex_lock(&g_lock);
    int idx = g_ntlv < M32_MAX_IMAGES ? g_ntlv++ : -1;
    if (idx >= 0) {
        g_tlv[idx].start = lo < hi ? lo : 0;
        g_tlv[idx].size = lo < hi ? hi - lo : 0;
        g_tlv[idx].init_size = init_hi > lo ? init_hi - lo : 0;
    }
    pthread_mutex_unlock(&g_lock);
    uint32_t at = vars->addr + (uint32_t)img->slide;
    for (uint32_t off = 0; idx >= 0 && off + 12 <= vars->size; off += 12) {
        m32_wr(at + off, M32_TRAP(M32_ID_TLV));
        m32_wr(at + off + 4, (uint32_t)idx);
    }
}

int m32_tlv_trap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t desc = (uint32_t)cpu->gpr[OCERZ_RAX], idx = m32_rd(desc + 4), off = m32_rd(desc + 8);
    if (idx >= (uint32_t)g_ntlv) {
        fprintf(stderr, "ocerz: m32: bad thread-local descriptor %#x\n", desc);
        ocerz_vm_request_exit(vm, 134);
        return OCERZ_STEP_EXIT;
    }
    uint32_t block = t_tlv[idx];
    if (!block) {
        const TlvImage *t = &g_tlv[idx];
        block = m32_malloc(t->size ? t->size : 4);
        memset(m32_h(block), 0, t->size);
        if (t->init_size)
            memcpy(m32_h(block), m32_h(t->start), t->init_size);
        t_tlv[idx] = block;
    }
    m32_ret(cpu, block + off, (uint32_t)cpu->gpr[OCERZ_RDX], 0);
    return OCERZ_STEP_OK;
}

const M32SpecialEntry m32_thread_specials[] = {
    { "_pthread_mutex_init", sp_mutex_init }, { "_pthread_mutex_destroy", sp_mutex_destroy },
    { "_pthread_mutex_lock", sp_mutex_lock }, { "_pthread_mutex_trylock", sp_mutex_trylock },
    { "_pthread_mutex_unlock", sp_mutex_unlock },
    { "_pthread_mutexattr_init", sp_mutexattr_init }, { "_pthread_mutexattr_destroy", sp_ok },
    { "_pthread_mutexattr_settype", sp_mutexattr_settype }, { "_pthread_mutexattr_gettype", sp_mutexattr_gettype },
    { "_pthread_mutexattr_setpshared", sp_ok }, { "_pthread_mutexattr_setprotocol", sp_ok },
    { "_pthread_cond_init", sp_cond_init }, { "_pthread_cond_destroy", sp_cond_destroy },
    { "_pthread_cond_wait", sp_cond_wait }, { "_pthread_cond_timedwait", sp_cond_timedwait },
    { "_pthread_cond_timedwait_relative_np", sp_cond_timedwait_rel },
    { "_pthread_cond_signal", sp_cond_signal }, { "_pthread_cond_broadcast", sp_cond_broadcast },
    { "_pthread_condattr_init", sp_ok }, { "_pthread_condattr_destroy", sp_ok },
    { "_pthread_rwlock_init", sp_rwlock_init }, { "_pthread_rwlock_destroy", sp_rwlock_destroy },
    { "_pthread_rwlock_rdlock", sp_rwlock_rdlock }, { "_pthread_rwlock_wrlock", sp_rwlock_wrlock },
    { "_pthread_rwlock_tryrdlock", sp_rwlock_tryrdlock }, { "_pthread_rwlock_trywrlock", sp_rwlock_trywrlock },
    { "_pthread_rwlock_unlock", sp_rwlock_unlock },
    { "_pthread_once", sp_once },
    { "_pthread_attr_init", sp_attr_init }, { "_pthread_attr_destroy", sp_ok },
    { "_pthread_attr_setstacksize", sp_attr_setstacksize }, { "_pthread_attr_getstacksize", sp_attr_getstacksize },
    { "_pthread_attr_setdetachstate", sp_attr_setdetachstate }, { "_pthread_attr_getdetachstate", sp_attr_getdetachstate },
    { "_pthread_attr_setschedparam", sp_ok }, { "_pthread_attr_setschedpolicy", sp_ok },
    { "_pthread_attr_setinheritsched", sp_ok }, { "_pthread_attr_setscope", sp_ok },
    { "_pthread_create", sp_pthread_create }, { "_pthread_create_suspended_np", sp_pthread_create_suspended },
    { "_pthread_join", sp_pthread_join }, { "_pthread_setschedparam", sp_setschedparam },
    { "_pthread_setname_np", sp_setname },
    { "_pthread_self", sp_pthread_self }, { "_pthread_exit", sp_pthread_exit },
    { "_pthread_key_create", sp_key_create }, { "_pthread_key_delete", sp_key_delete },
    { "_pthread_getspecific", sp_getspecific }, { "_pthread_setspecific", sp_setspecific },
    { "_sched_yield", sp_sched_yield },
    { NULL, NULL }
};
