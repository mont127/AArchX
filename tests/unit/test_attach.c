/*
 * Giving a host thread a guest personality, checked as far as it can be checked
 * without running any guest code.
 *
 * Everything that looks at a personality looks at it from the thread that owns
 * it and while that thread is still alive, since the personality is torn down
 * with the thread and its guest memory may be gone by the time a joiner looks.
 * A personality is a cpu whose vm is the VM attach was handed, whose RSP is
 * non-zero, on a 16-byte boundary and has at least 64 KB of readable and
 * writable guest stack below it, with a store and load round trip at both ends
 * of that span, and whose gs_base is non-zero, on an 8-byte boundary and has
 * eight readable thread-block slots behind it.  It is also registered like any
 * other guest thread, which is asserted through ocerz_vm_thread_regs: the port
 * of the host thread must find a cpu, and the RSP that cpu reports must be the
 * personality's.  That is the lookup JavaScriptCore's collector goes through
 * when it suspends a thread and reads its registers, so a personality missing
 * from it is a thread the collector cannot see.
 *
 * Asserted, in order.  Before any VM exists ocerz_vm_process is NULL, and
 * attaching to what it returns gives NULL on a thread the test creates, which
 * is the state in which the callback dispatcher still refuses.
 * ocerz_thread_attach(NULL) gives NULL on the main thread and on a created one,
 * and leaves both without a cpu.  ocerz_thread_detach on a thread that never
 * attached, twice in a row, leaves it without a cpu.  On a new thread
 * ocerz_vm_current_cpu is NULL before attach, attach gives a personality, a
 * second attach gives the same pointer, and ocerz_vm_current_cpu gives it too;
 * that first attach in the process also retires plain memory mode, jit_plain_mem
 * going from 1 to 0 and jit_ordered_required from 0 to 1, since a second thread
 * running guest code is a second observer of guest memory.  Eight threads
 * released together all attach at once, the way libdispatch's workers meet a
 * callback, and are held until every one has attached so no personality can be
 * torn down and its memory handed to the next: their cpus must differ, their
 * stack tops must be at least the 64 KB each was checked to own apart, and their
 * thread blocks at least their eight slots apart.  Detach on an attached thread
 * leaves it without a cpu and unregistered, a second detach changes nothing, and
 * attaching again builds a personality that passes every check above.  A thread
 * detached before it ever attached still gets a working personality when it
 * does attach.
 *
 * Exit runs last.  A thread that attaches and returns must not take the process
 * with it, and once it has been joined its port must no longer find a cpu.  Then
 * a thousand threads attach and exit one after another, each unregistered once
 * joined, and a thread attaching after them must still get a personality and
 * still be found by its port.  A thousand is more than the 512 cpus the registry
 * holds, so a teardown that leaked registry entries fails the last thread, and
 * more personalities of a megabyte than the 1 GB arena has room for once the
 * loader's quarter is set aside, so a teardown that never gave back its guest
 * memory fails attach partway through.  The arena is smaller than the other
 * harnesses' for that reason alone.
 *
 * What is not here.  Nothing enters guest code, so a personality actually
 * running a callback, the dispatcher attaching a libdispatch worker on first
 * use, bridged calls and nested callbacks on such a worker, dispatch_sync_f
 * reusing the calling thread's own cpu, and a guest fault on an attached thread
 * being reported as the guest's are the attach_* cases in
 * tests/run_native_tests.sh.  Detach leaving alone a thread ocerz started itself
 * cannot be driven here at all: the only ways a thread gets a cpu other than
 * attach are running a guest thread and calling into guest code, and both run
 * guest code.  Nor is ocerz_vm_process asserted non-NULL, because a VM becomes
 * the process's by starting to run, which no unit harness does; attach is handed
 * the VM directly instead.  The map is the identity one native mode runs in, as
 * in test_callback.c, and jit_plain_mem starts at 1 because that is what main
 * sets before any guest code runs.
 */
#include "ocerz/cpu.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA       (1ull << 30)
#define STACK_SPAN  0x10000ull
#define GUEST_PAGE  0x1000ull
#define GS_SLOTS    8u
#define RACERS      8
#define CHURN       1000
#define PATTERN     0x5a17ac4ed0c0ffeeull

static int checks;
static int failures;
static pthread_mutex_t g_check_lock = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    pthread_mutex_lock(&g_check_lock); \
    checks++; \
    if (!ok_) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
    pthread_mutex_unlock(&g_check_lock); \
} while (0)

static OcerzVM g_vm;

typedef struct Seen {
    OcerzCPU *cpu;
    uint64_t rsp;
    uint64_t gs;
    uint32_t port;
    int attached;
} Seen;

static uint32_t own_port(void)
{
    return (uint32_t)pthread_mach_thread_np(pthread_self());
}

static int registered_with_rsp(uint32_t port, uint64_t rsp)
{
    uint64_t gpr[16], rip = 0, rflags = 0;

    memset(gpr, 0, sizeof gpr);
    if (ocerz_vm_thread_regs(port, gpr, &rip, &rflags) != 0)
        return -1;
    return gpr[OCERZ_RSP] == rsp ? 1 : 0;
}

static int span_usable(uint64_t hi, uint64_t span, int need_write)
{
    uint64_t off;
    int prot;

    if (hi < span)
        return 0;
    for (off = 8;; off += GUEST_PAGE) {
        if (off > span)
            off = span;
        prot = ocerz_addr_prot(hi - off);
        if (prot < 0 || !(prot & PROT_READ) || (need_write && !(prot & PROT_WRITE)))
            return 0;
        if (off == span)
            return 1;
    }
}

static void check_personality(const char *where, OcerzCPU *cpu, Seen *seen)
{
    uint64_t rsp, gs, i;
    int stack_ok, gs_ok, reg;

    if (!cpu)
        return;
    rsp = cpu->gpr[OCERZ_RSP];
    gs = cpu->gs_base;
    if (seen) {
        seen->cpu = cpu;
        seen->rsp = rsp;
        seen->gs = gs;
        seen->port = own_port();
    }

    CHECK(cpu->vm == &g_vm,
          "%s: the personality's vm is %p, want the VM attach was handed, %p",
          where, (void *)cpu->vm, (void *)&g_vm);

    CHECK(rsp != 0, "%s: the personality's RSP is zero, so it has no guest stack",
          where);
    CHECK((rsp & 15) == 0,
          "%s: the personality's RSP %#llx is not on a 16-byte boundary", where,
          (unsigned long long)rsp);
    stack_ok = rsp != 0 && span_usable(rsp, STACK_SPAN, 1);
    CHECK(stack_ok,
          "%s: the %llu bytes of guest stack below RSP %#llx are not all readable "
          "and writable guest memory", where, (unsigned long long)STACK_SPAN,
          (unsigned long long)rsp);
    if (stack_ok) {
        ocerz_st(rsp - 8, 8, PATTERN);
        ocerz_st(rsp - STACK_SPAN, 8, ~PATTERN);
        CHECK(ocerz_ld(rsp - 8, 8) == PATTERN &&
                  ocerz_ld(rsp - STACK_SPAN, 8) == ~PATTERN,
              "%s: a value stored on the personality's stack below %#llx did not "
              "read back", where, (unsigned long long)rsp);
    }

    CHECK(gs != 0, "%s: the personality's gs_base is zero, so it has no guest "
          "thread block", where);
    CHECK((gs & 7) == 0,
          "%s: the personality's gs_base %#llx is not on an 8-byte boundary", where,
          (unsigned long long)gs);
    gs_ok = gs != 0;
    for (i = 0; gs_ok && i < GS_SLOTS; i++)
        if (!ocerz_addr_readable(gs + 8 * i) || !ocerz_addr_readable(gs + 8 * i + 7))
            gs_ok = 0;
    CHECK(gs_ok,
          "%s: the first %u slots of the thread block at gs_base %#llx are not "
          "readable guest memory", where, GS_SLOTS, (unsigned long long)gs);
    if (gs_ok)
        (void)ocerz_ld(gs, 8);

    reg = registered_with_rsp(own_port(), rsp);
    CHECK(reg == 1,
          "%s: %s", where,
          reg < 0 ? "the host thread's port finds no registered guest cpu, so a "
                    "thread_suspend or thread_get_state aimed at it would be "
                    "handed to the host kernel as if it ran no guest code"
                  : "the cpu registered under the host thread's port reports an "
                    "RSP other than the personality's");
}

static void *before_vm_thread(void *arg)
{
    OcerzCPU *cpu = ocerz_thread_attach(ocerz_vm_process());

    CHECK(cpu == NULL,
          "with no VM running, ocerz_thread_attach(ocerz_vm_process()) on a new "
          "thread gave %p, want NULL", (void *)cpu);
    CHECK(ocerz_vm_current_cpu() == NULL,
          "a refused attach left the thread with the cpu %p",
          (void *)ocerz_vm_current_cpu());
    return arg;
}

static int run_thread(void *(*fn)(void *), void *arg, const char *what)
{
    pthread_t th;
    int rc = pthread_create(&th, NULL, fn, arg);

    CHECK(rc == 0, "pthread_create failed with %d, so %s is untested", rc, what);
    if (rc != 0)
        return 0;
    pthread_join(th, NULL);
    return 1;
}

static void test_before_vm(void)
{
    CHECK(ocerz_vm_process() == NULL,
          "ocerz_vm_process is %p before any VM was built, want NULL",
          (void *)ocerz_vm_process());
    run_thread(before_vm_thread, NULL, "attaching with no VM");
}

static void null_here(const char *where)
{
    OcerzCPU *cpu;

    CHECK(ocerz_vm_current_cpu() == NULL,
          "%s: ocerz_vm_current_cpu is %p before anything attached", where,
          (void *)ocerz_vm_current_cpu());
    cpu = ocerz_thread_attach(NULL);
    CHECK(cpu == NULL, "%s: ocerz_thread_attach(NULL) gave %p, want NULL", where,
          (void *)cpu);
    CHECK(ocerz_vm_current_cpu() == NULL,
          "%s: ocerz_thread_attach(NULL) left the thread with the cpu %p", where,
          (void *)ocerz_vm_current_cpu());
}

static void *null_thread(void *arg)
{
    null_here("a thread the test created");
    return arg;
}

static void test_null(void)
{
    null_here("the test's main thread");
    run_thread(null_thread, NULL, "attach(NULL) on a created thread");
}

static void stray_detach_here(const char *where)
{
    ocerz_thread_detach();
    CHECK(ocerz_vm_current_cpu() == NULL,
          "%s: detaching a thread that never attached gave it the cpu %p", where,
          (void *)ocerz_vm_current_cpu());
    ocerz_thread_detach();
    CHECK(ocerz_vm_current_cpu() == NULL,
          "%s: a second detach on a thread that never attached gave it the cpu %p",
          where, (void *)ocerz_vm_current_cpu());
}

static void *stray_detach_thread(void *arg)
{
    stray_detach_here("a created thread that never attached");
    return arg;
}

static void *stray_then_attach_thread(void *arg)
{
    OcerzCPU *cpu;

    stray_detach_here("a created thread detached before it attached");
    cpu = ocerz_thread_attach(&g_vm);
    CHECK(cpu != NULL,
          "attach on a thread that had been detached before it ever attached gave "
          "NULL");
    CHECK(cpu == NULL || ocerz_vm_current_cpu() == cpu,
          "after attach following a stray detach, ocerz_vm_current_cpu is %p, want "
          "%p", (void *)ocerz_vm_current_cpu(), (void *)cpu);
    check_personality("attach after a stray detach", cpu, NULL);
    return arg;
}

static void test_stray_detach(void)
{
    stray_detach_here("the test's main thread, which never attached");
    run_thread(stray_detach_thread, NULL, "detach on a thread that never attached");
}

static void *single_thread(void *arg)
{
    Seen *seen = arg;
    OcerzCPU *a, *b;

    CHECK(ocerz_vm_current_cpu() == NULL,
          "a new thread already has the cpu %p before attaching",
          (void *)ocerz_vm_current_cpu());
    a = ocerz_thread_attach(&g_vm);
    CHECK(a != NULL, "the first attach on a new thread gave NULL");
    if (!a)
        return arg;
    seen->attached = 1;
    b = ocerz_thread_attach(&g_vm);
    CHECK(b == a,
          "a second attach on the same thread gave %p, want the first personality "
          "%p", (void *)b, (void *)a);
    CHECK(ocerz_vm_current_cpu() == a,
          "after attach ocerz_vm_current_cpu is %p, want the personality %p",
          (void *)ocerz_vm_current_cpu(), (void *)a);
    check_personality("a new thread's first attach", a, seen);
    return arg;
}

static void test_single(void)
{
    Seen seen;

    memset(&seen, 0, sizeof seen);
    CHECK(g_vm.jit_plain_mem == 1 && g_vm.jit_ordered_required == 0,
          "before the first attach jit_plain_mem is %d and jit_ordered_required "
          "%d, want 1 and 0, so the retire below proves nothing",
          (int)g_vm.jit_plain_mem, (int)g_vm.jit_ordered_required);
    if (!run_thread(single_thread, &seen, "a first attach"))
        return;
    CHECK(!seen.attached ||
              (g_vm.jit_plain_mem == 0 && g_vm.jit_ordered_required == 1),
          "after the first attach jit_plain_mem is %d and jit_ordered_required %d, "
          "want 0 and 1: two threads may now run guest code, and plain memory "
          "mode assumes one", (int)g_vm.jit_plain_mem,
          (int)g_vm.jit_ordered_required);
}

static pthread_mutex_t g_race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_race_cv = PTHREAD_COND_INITIALIZER;
static int g_race_go;
static int g_race_done;
static int g_race_release;

static void *racer_thread(void *arg)
{
    Seen *seen = arg;
    OcerzCPU *cpu;

    pthread_mutex_lock(&g_race_lock);
    while (!g_race_go)
        pthread_cond_wait(&g_race_cv, &g_race_lock);
    pthread_mutex_unlock(&g_race_lock);

    cpu = ocerz_thread_attach(&g_vm);
    CHECK(cpu != NULL, "a thread attaching alongside %d others got NULL",
          RACERS - 1);
    if (cpu) {
        seen->attached = 1;
        CHECK(ocerz_thread_attach(&g_vm) == cpu && ocerz_vm_current_cpu() == cpu,
              "a thread that attached alongside others does not get its own "
              "personality %p back", (void *)cpu);
        check_personality("a thread attaching alongside others", cpu, seen);
    }

    pthread_mutex_lock(&g_race_lock);
    g_race_done++;
    pthread_cond_broadcast(&g_race_cv);
    while (!g_race_release)
        pthread_cond_wait(&g_race_cv, &g_race_lock);
    pthread_mutex_unlock(&g_race_lock);
    return arg;
}

static uint64_t gap(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

static void test_racers(void)
{
    pthread_t th[RACERS];
    Seen seen[RACERS];
    int started[RACERS];
    int i, j, n = 0;

    memset(seen, 0, sizeof seen);
    for (i = 0; i < RACERS; i++) {
        started[i] = pthread_create(&th[i], NULL, racer_thread, &seen[i]) == 0;
        CHECK(started[i], "could not start racing thread %d", i);
        n += started[i];
    }

    pthread_mutex_lock(&g_race_lock);
    g_race_go = 1;
    pthread_cond_broadcast(&g_race_cv);
    while (g_race_done < n)
        pthread_cond_wait(&g_race_cv, &g_race_lock);
    pthread_mutex_unlock(&g_race_lock);

    for (i = 0; i < RACERS; i++) {
        for (j = i + 1; j < RACERS; j++) {
            if (!seen[i].attached || !seen[j].attached)
                continue;
            CHECK(seen[i].cpu != seen[j].cpu,
                  "threads %d and %d, both still alive, share the cpu %p", i, j,
                  (void *)seen[i].cpu);
            CHECK(gap(seen[i].rsp, seen[j].rsp) >= STACK_SPAN,
                  "threads %d and %d have stack tops %#llx and %#llx, closer than "
                  "the %llu bytes each was checked to own", i, j,
                  (unsigned long long)seen[i].rsp, (unsigned long long)seen[j].rsp,
                  (unsigned long long)STACK_SPAN);
            CHECK(gap(seen[i].gs, seen[j].gs) >= 8ull * GS_SLOTS,
                  "threads %d and %d have gs_base %#llx and %#llx, so their thread "
                  "blocks overlap", i, j, (unsigned long long)seen[i].gs,
                  (unsigned long long)seen[j].gs);
        }
    }

    pthread_mutex_lock(&g_race_lock);
    g_race_release = 1;
    pthread_cond_broadcast(&g_race_cv);
    pthread_mutex_unlock(&g_race_lock);
    for (i = 0; i < RACERS; i++)
        if (started[i])
            pthread_join(th[i], NULL);
}

static void *detach_thread(void *arg)
{
    OcerzCPU *a, *b;
    uint32_t port = own_port();

    a = ocerz_thread_attach(&g_vm);
    CHECK(a != NULL, "attach before detach gave NULL");
    if (!a)
        return arg;
    check_personality("before detach", a, NULL);

    ocerz_thread_detach();
    CHECK(ocerz_vm_current_cpu() == NULL,
          "after detach ocerz_vm_current_cpu is still %p",
          (void *)ocerz_vm_current_cpu());
    CHECK(registered_with_rsp(port, 0) < 0,
          "after detach the thread's port %#x still finds a registered cpu", port);
    ocerz_thread_detach();
    CHECK(ocerz_vm_current_cpu() == NULL,
          "a second detach gave the thread the cpu %p",
          (void *)ocerz_vm_current_cpu());

    b = ocerz_thread_attach(&g_vm);
    CHECK(b != NULL, "attaching again after detach gave NULL");
    if (!b)
        return arg;
    CHECK(ocerz_vm_current_cpu() == b,
          "after attaching again ocerz_vm_current_cpu is %p, want %p",
          (void *)ocerz_vm_current_cpu(), (void *)b);
    CHECK(ocerz_thread_attach(&g_vm) == b,
          "a second attach after re-attaching did not give back %p", (void *)b);
    check_personality("attached again after detach", b, NULL);

    ocerz_thread_detach();
    CHECK(ocerz_vm_current_cpu() == NULL,
          "detaching the second personality left the cpu %p",
          (void *)ocerz_vm_current_cpu());
    return arg;
}

static void test_detach(void)
{
    run_thread(detach_thread, NULL, "detach and attach again");
    run_thread(stray_then_attach_thread, NULL, "attach after a stray detach");
}

typedef struct Churn {
    uint32_t port;
    int attached;
} Churn;

static void *exit_thread(void *arg)
{
    Churn *c = arg;

    c->port = own_port();
    c->attached = ocerz_thread_attach(&g_vm) != NULL;
    return arg;
}

static void *last_thread(void *arg)
{
    Seen *seen = arg;
    OcerzCPU *cpu = ocerz_thread_attach(&g_vm);

    CHECK(cpu != NULL,
          "after %d threads attached and exited, attach on a new thread gave NULL, "
          "so their teardown did not give back what attach takes", CHURN);
    if (cpu) {
        seen->attached = 1;
        check_personality("a thread attaching after a thousand exited", cpu, seen);
    }
    return arg;
}

static void test_exit(void)
{
    Churn one, c;
    Seen last;
    int i, refused = 0, first_refused = -1, lingering = 0, created = 0;

    memset(&one, 0, sizeof one);
    if (run_thread(exit_thread, &one, "a thread exiting while attached")) {
        CHECK(one.attached, "attach on a thread about to exit gave NULL");
        CHECK(!one.attached || registered_with_rsp(one.port, 0) < 0,
              "a thread that exited while attached has been joined, but its port "
              "%#x still finds a registered cpu", one.port);
    }

    for (i = 0; i < CHURN; i++) {
        pthread_t th;
        memset(&c, 0, sizeof c);
        if (pthread_create(&th, NULL, exit_thread, &c) != 0)
            continue;
        pthread_join(th, NULL);
        created++;
        if (!c.attached) {
            if (first_refused < 0)
                first_refused = i;
            refused++;
        } else if (registered_with_rsp(c.port, 0) >= 0) {
            lingering++;
        }
    }
    CHECK(created == CHURN, "only %d of %d exiting threads could be created",
          created, CHURN);
    CHECK(refused == 0,
          "%d of %d threads that attach and exit got NULL, the first at number "
          "%d, so exiting threads do not give back what attach takes", refused,
          created, first_refused);
    CHECK(lingering == 0,
          "%d of %d threads that attached and exited were still registered after "
          "being joined", lingering, created);

    memset(&last, 0, sizeof last);
    run_thread(last_thread, &last, "attach after many exits");
}

static int report(void)
{
    printf("test_attach: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    test_before_vm();

    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }
    if (ocerz_vm_init(&g_vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    g_vm.jit_plain_mem = 1;

    test_null();
    test_stray_detach();
    test_single();
    test_racers();
    test_detach();
    test_exit();

    return report();
}
