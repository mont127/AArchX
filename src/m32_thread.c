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
#include <sys/mman.h>

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

/* pthread_rwlockattr_t is 16 bytes on i386 and 24 on arm64, and rwlock_init above makes its lock without one: the
 * attribute lives in the guest's bytes, its process-shared setting in word 1 (2, PTHREAD_PROCESS_PRIVATE, unset). */
static int sp_rwlockattr_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t a = m32_arg(cpu, 0);
    if (a) {
        memset(m32_h(a), 0, 16);
        m32_wr(a, 0x52574c41u);   /* 'RWLA', i386's signature */
        m32_wr(a + 4, 2);
    }
    RETI(0);
}
static int sp_rwlockattr_setpshared(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t a = m32_arg(cpu, 0), v = m32_arg(cpu, 1);
    if (v != 1 && v != 2)
        RETI(EINVAL);
    if (a)
        m32_wr(a + 4, v);
    RETI(0);
}
static int sp_rwlockattr_getpshared(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t a = m32_arg(cpu, 0), out = m32_arg(cpu, 1);
    if (out)
        m32_wr(out, a && m32_rd(a + 4) == 1 ? 1 : 2);
    RETI(0);
}

/* OSAtomicEnqueue/Dequeue (a LIFO: head, generation) and OSAtomicFifoEnqueue/Dequeue (head, tail, spare) link the
 * guest's own elements through a 4-byte pointer at offset in each.  Crossed, the host copied the head in and out,
 * which is not atomic, and wrote 8-byte host addresses into the elements.  They run on the guest's words, under one
 * lock. */
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static int sp_OSAtomicEnqueue(struct OcerzVM *vm, OcerzCPU *cpu)   /* (OSQueueHead *, void *new, size_t offset) */
{
    uint32_t q = m32_arg(cpu, 0), item = m32_arg(cpu, 1), off = m32_arg(cpu, 2);
    pthread_mutex_lock(&g_queue_lock);
    m32_wr(item + off, m32_rd(q));
    m32_wr(q, item);
    m32_wr(q + 4, m32_rd(q + 4) + 1);
    pthread_mutex_unlock(&g_queue_lock);
    RETI(0);
}
static int sp_OSAtomicDequeue(struct OcerzVM *vm, OcerzCPU *cpu)   /* (OSQueueHead *, size_t offset) */
{
    uint32_t q = m32_arg(cpu, 0), off = m32_arg(cpu, 1);
    pthread_mutex_lock(&g_queue_lock);
    uint32_t item = m32_rd(q);
    if (item) {
        m32_wr(q, m32_rd(item + off));
        m32_wr(q + 4, m32_rd(q + 4) + 1);
    }
    pthread_mutex_unlock(&g_queue_lock);
    RETI(item);
}
static int sp_OSAtomicFifoEnqueue(struct OcerzVM *vm, OcerzCPU *cpu)   /* (OSFifoQueueHead *, void *new, offset) */
{
    uint32_t q = m32_arg(cpu, 0), item = m32_arg(cpu, 1), off = m32_arg(cpu, 2);
    pthread_mutex_lock(&g_queue_lock);
    m32_wr(item + off, 0);
    uint32_t tail = m32_rd(q + 4);
    m32_wr(tail ? tail + off : q, item);
    m32_wr(q + 4, item);
    pthread_mutex_unlock(&g_queue_lock);
    RETI(0);
}
static int sp_OSAtomicFifoDequeue(struct OcerzVM *vm, OcerzCPU *cpu)   /* (OSFifoQueueHead *, size_t offset) */
{
    uint32_t q = m32_arg(cpu, 0), off = m32_arg(cpu, 1);
    pthread_mutex_lock(&g_queue_lock);
    uint32_t item = m32_rd(q);
    if (item) {
        m32_wr(q, m32_rd(item + off));
        if (!m32_rd(q))
            m32_wr(q + 4, 0);
    }
    pthread_mutex_unlock(&g_queue_lock);
    RETI(item);
}

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

/* ---- thread attributes: magic, stack size, detach state, stack top, guard size ----
 * A thread's stack is the guest's when it names one: Wine runs each Windows thread on the stack it allocated for it
 * (pthread_attr_setstack), whose bounds its TEB records and its exception dispatch checks frames against.  A stack
 * address is the top of the stack, as Darwin keeps it (pthread_attr_setstackaddr takes the top; setstack the base and
 * a size), and the checks are Darwin's, with the 4 KB pages i386 programs are told.  A thread that names no stack and
 * asks for more than the attached cpu's own (about 2 MB) gets a guest mapping that large. */
#define ATTR_STACKSIZE 4
#define ATTR_DETACH    8
#define ATTR_STACKTOP  12   /* 0: none named */
#define ATTR_GUARD     16
#define M32_STACK_MIN  8192u   /* i386's PTHREAD_STACK_MIN */
#define M32_OWN_STACK  0x1f0000u   /* what an attached cpu's own guest stack holds (src/vm.c, ocerz_thread_attach) */

static int sp_attr_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    memset(m32_h(g), 0, 40);
    m32_wr(g, ATTR_MAGIC);
    m32_wr(g + ATTR_STACKSIZE, 512u << 10);
    m32_wr(g + ATTR_DETACH, PTHREAD_CREATE_JOINABLE);
    m32_wr(g + ATTR_GUARD, 4096);
    RETI(0);
}
/* any size, rounded up to whole pages, as m32 always took it (a game that once got 0 does not get EINVAL now) */
static int sp_attr_setstacksize(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t size = m32_arg(cpu, 1);
    m32_wr(m32_arg(cpu, 0) + ATTR_STACKSIZE, size > 0xfffff000u ? 0xfffff000u : (size + 0xfffu) & ~0xfffu);
    RETI(0);
}
static int sp_attr_getstacksize(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + ATTR_STACKSIZE)); RETI(0); }
static int sp_attr_setdetachstate(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 0) + ATTR_DETACH, m32_arg(cpu, 1)); RETI(0); }
static int sp_attr_getdetachstate(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + ATTR_DETACH)); RETI(0); }
/* (attr, base, size): the stack is [base, base + size) */
static int sp_attr_setstack(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t attr = m32_arg(cpu, 0), base = m32_arg(cpu, 1), size = m32_arg(cpu, 2);
    if ((base & 0xfff) || (size & 0xfff) || size < M32_STACK_MIN || (uint64_t)base + size > 0x100000000ull)
        RETI(EINVAL);
    m32_wr(attr + ATTR_STACKTOP, base + size);
    m32_wr(attr + ATTR_STACKSIZE, size);
    RETI(0);
}
static int sp_attr_getstack(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t attr = m32_arg(cpu, 0), top = m32_rd(attr + ATTR_STACKTOP), size = m32_rd(attr + ATTR_STACKSIZE);
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), top ? top - size : 0);
    if (m32_arg(cpu, 2))
        m32_wr(m32_arg(cpu, 2), size);
    RETI(0);
}
/* (attr, top) */
static int sp_attr_setstackaddr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t top = m32_arg(cpu, 1);
    if (top & 0xfff)
        RETI(EINVAL);
    m32_wr(m32_arg(cpu, 0) + ATTR_STACKTOP, top);
    RETI(0);
}
static int sp_attr_getstackaddr(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + ATTR_STACKTOP)); RETI(0); }
static int sp_attr_setguardsize(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 0) + ATTR_GUARD, m32_arg(cpu, 1)); RETI(0); }
static int sp_attr_getguardsize(struct OcerzVM *vm, OcerzCPU *cpu) { m32_wr(m32_arg(cpu, 1), m32_rd(m32_arg(cpu, 0) + ATTR_GUARD)); RETI(0); }

/* pthread_setname_np(name): the host thread gets it too (sample, the crash reports) */
static int sp_setname(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *name = (const char *)m32_h(m32_arg(cpu, 0));
    char buf[64];
    snprintf(buf, sizeof buf, "%s", name ? name : "");
    RETI(pthread_setname_np(buf));
}

/* ---- threads ---- */
/* stack: the top of the guest stack the thread starts on (0: the attached cpu's own); own, own_size: a mapping m32 made
 * for it, given back when the thread function returns (ponytail: not when the thread ends in pthread_exit) */
typedef struct Start { struct OcerzVM *vm; uint32_t fn, arg, stack, own, own_size; } Start;

static void *thread_main(void *p)
{
    Start s = *(Start *)p;
    free(p);
    OcerzCPU *cpu = m32_thread_attach(s.vm);
    if (!cpu)
        return NULL;
    if (s.stack)
        cpu->gpr[OCERZ_RSP] = s.stack;
    /* A game's threads are interactive work.  At the default QoS macOS sometimes parks Batman's render thread on the
     * efficiency cores for good (7x slower frames, glstat cpus=0-3); the game's own pthread_setschedparam is not
     * applied (sp_setschedparam), since a fixed priority would take the thread out of QoS again. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint32_t r = m32_call(s.vm, s.fn, &s.arg, 1, NULL, NULL);
    if (s.own)
        ocerz_unmap(s.own, s.own_size);
    return (void *)(uintptr_t)r;
}

/* pthread_create, and pthread_create_suspended_np: Bink's decoders start suspended and run on thread_resume(port),
 * which reaches the host thread directly (pthread_mach_thread_np of our handle is the host thread's port) */
static int create(struct OcerzVM *vm, OcerzCPU *cpu, int suspended)
{
    uint32_t out = m32_arg(cpu, 0), attr = m32_arg(cpu, 1);
    Start *s = calloc(1, sizeof *s);
    if (!s)
        RETI(EAGAIN);
    s->vm = vm;
    s->fn = m32_arg(cpu, 2);
    s->arg = m32_arg(cpu, 3);
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (attr && m32_rd(attr) == ATTR_MAGIC) {
        if (m32_rd(attr + ATTR_DETACH) == PTHREAD_CREATE_DETACHED)
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        uint32_t size = m32_rd(attr + ATTR_STACKSIZE);
        s->stack = m32_rd(attr + ATTR_STACKTOP);
        if (!s->stack && size > M32_OWN_STACK) {
            uint32_t len = (size + 0x3fffu) & ~0x3fffu;   /* whole host pages, so the unmap takes it all */
            uint32_t g = m32_mmap(0, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (g == 0xffffffffu) {
                pthread_attr_destroy(&a);
                free(s);
                RETI(EAGAIN);
            }
            s->own = g;
            s->own_size = len;
            s->stack = g + len;
        }
    }
    pthread_attr_setstacksize(&a, 1u << 20);   /* the host side: the guest stack is the attached cpu's own */
    pthread_t t;
    int r = suspended ? pthread_create_suspended_np(&t, &a, thread_main, s) : pthread_create(&t, &a, thread_main, s);
    pthread_attr_destroy(&a);
    if (r) {
        if (s->own)
            ocerz_unmap(s->own, s->own_size);
        free(s);
    } else if (out)
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
/* i386 Darwin's gs points at the thread's specific data, and code reads a key's value straight from gs:[key * 4] (V8,
 * in Left 4 Dead 2's Chromium, checks that it can): what pthread_setspecific stores is mirrored there.  A thread's gs
 * is its AArchX thread block's, the main thread's a block of its own (m32_run). */
void m32_tsd_set(uint32_t key, uint32_t value)
{
    OcerzCPU *c = ocerz_vm_current_cpu();
    if (c && c->gs_base && key < M32_TSD_KEYS)
        m32_wr((uint32_t)c->gs_base + 4 * key, value);
}
static int sp_setspecific(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_tsd_set(m32_arg(cpu, 0), m32_arg(cpu, 1));
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
    { "_pthread_rwlockattr_init", sp_rwlockattr_init }, { "_pthread_rwlockattr_destroy", sp_ok },
    { "_pthread_rwlockattr_setpshared", sp_rwlockattr_setpshared },
    { "_pthread_rwlockattr_getpshared", sp_rwlockattr_getpshared },
    { "_OSAtomicEnqueue", sp_OSAtomicEnqueue }, { "_OSAtomicDequeue", sp_OSAtomicDequeue },
    { "_OSAtomicFifoEnqueue", sp_OSAtomicFifoEnqueue }, { "_OSAtomicFifoDequeue", sp_OSAtomicFifoDequeue },
    { "_pthread_once", sp_once },
    { "_pthread_attr_init", sp_attr_init }, { "_pthread_attr_destroy", sp_ok },
    { "_pthread_attr_setstacksize", sp_attr_setstacksize }, { "_pthread_attr_getstacksize", sp_attr_getstacksize },
    { "_pthread_attr_setdetachstate", sp_attr_setdetachstate }, { "_pthread_attr_getdetachstate", sp_attr_getdetachstate },
    { "_pthread_attr_setschedparam", sp_ok }, { "_pthread_attr_setschedpolicy", sp_ok },
    { "_pthread_attr_setinheritsched", sp_ok }, { "_pthread_attr_setscope", sp_ok },
    { "_pthread_attr_setstack", sp_attr_setstack }, { "_pthread_attr_getstack", sp_attr_getstack },
    { "_pthread_attr_setstackaddr", sp_attr_setstackaddr }, { "_pthread_attr_getstackaddr", sp_attr_getstackaddr },
    { "_pthread_attr_setguardsize", sp_attr_setguardsize }, { "_pthread_attr_getguardsize", sp_attr_getguardsize },
    { "_pthread_create", sp_pthread_create }, { "_pthread_create_suspended_np", sp_pthread_create_suspended },
    { "_pthread_join", sp_pthread_join }, { "_pthread_setschedparam", sp_setschedparam },
    { "_pthread_setname_np", sp_setname },
    { "_pthread_self", sp_pthread_self }, { "_pthread_exit", sp_pthread_exit },
    { "_pthread_key_create", sp_key_create }, { "_pthread_key_delete", sp_key_delete },
    { "_pthread_getspecific", sp_getspecific }, { "_pthread_setspecific", sp_setspecific },
    { "_sched_yield", sp_sched_yield },
    { NULL, NULL }
};
