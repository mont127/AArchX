/*
 * Calls into i386 guest code (include/ocerz/m32.h).
 *
 * m32_call runs a guest function with 32-bit arguments on the calling thread's guest cpu (loader work: initializers,
 * atexit handlers).  m32_callback turns a guest function pointer handed to a host function into something the host
 * can call: a slot of AArchX's callback bank reserved for m32, whose dispatcher reads the arm64 arguments by the
 * host notation, converts them to i386 cdecl words by the guest notation, runs the guest function and converts its
 * EAX / EDX:EAX / ST0 result back.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/decode.h"
#include "ocerz/m32.h"
#include "ocerz/m32_sig.h"
#include "ocerz/vm.h"

typedef struct M32Cb {
    uint32_t fn;
    M32Sig *g, *h;
    char *gnote, *hnote;
} M32Cb;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static M32Cb *g_cbs[OCERZ_ABI_CALLBACK_SLOTS];
static struct { uint32_t fn; char *gnote, *hnote; void *addr; } *g_known;
static int g_nknown, g_capknown;

/* the calling thread's guest cpu, attaching a 32-bit one to a thread the host started */
int m32_started;

static OcerzCPU *guest_cpu(struct OcerzVM *vm)
{
    OcerzCPU *cpu = ocerz_vm_current_cpu();
    if (!cpu && !m32_started)
        return &vm->cpu;   /* loader work on the main thread before the program starts */
    if (!cpu || !cpu->mode32)
        cpu = m32_thread_attach(vm);
    return cpu;
}

/* AArchX's attach gives the thread a cpu, a 2 MB guest stack from the window and a thread block; it becomes 32-bit */
OcerzCPU *m32_thread_attach(struct OcerzVM *vm)
{
    OcerzCPU *cpu = ocerz_thread_attach(vm);
    if (cpu && !cpu->mode32) {
        cpu->mode32 = 1;
        cpu->cs_sel = M32_CS;
        cpu->seg_sel[OCERZ_SREG_CS] = M32_CS;
    }
    return cpu;
}

uint32_t m32_call(struct OcerzVM *vm, uint32_t fn, const uint32_t *args, int nargs, uint32_t *edx, double *st0)
{
    OcerzCPU *cpu = guest_cpu(vm);
    if (!cpu || nargs > 16)
        return 0;
    OcerzGuestCall call;
    memset(&call, 0, sizeof call);
    for (int i = 0; i < nargs; i++)
        call.stack[i] = args[i];
    call.nstack = nargs;
    uint32_t top = ((uint32_t)cpu->gpr[OCERZ_RSP] - 256) & ~15u;
    struct OcerzBridgeFrame saved;
    ocerz_bridge_guest_enter(&saved);
    ocerz_vm_call32(vm, fn, &call, top, M32_TRAP(M32_ID_SENTINEL));
    ocerz_bridge_guest_leave(&saved);
    if (edx)
        *edx = (uint32_t)call.rdx;
    if (st0)
        memcpy(st0, &call.xmm0, 8);
    return (uint32_t)call.rax;
}

/* read the host arguments of a callback the way Apple's arm64 ABI placed them */
typedef struct HostArgs { const uint64_t *x, *v; const uint8_t *stack; int nx, nv; uint32_t sp; } HostArgs;

static uint64_t take_stack(HostArgs *a, uint32_t size, uint32_t align)
{
    a->sp = (a->sp + align - 1) & ~(align - 1);
    uint64_t v = 0;
    memcpy(&v, a->stack + a->sp, size > 8 ? 8 : size);
    a->sp += size;
    return v;
}

static void take_struct(HostArgs *a, const M32Type *t, uint8_t *buf)
{
    if (m32_is_hfa(t)) {
        if (a->nv + t->n <= 8) {
            for (int i = 0; i < t->n; i++)
                memcpy(buf + t->moff[i], &a->v[a->nv++], t->msize[i]);
        } else {
            a->nv = 8;
            a->sp = (a->sp + t->align - 1) & ~(t->align - 1u);
            memcpy(buf, a->stack + a->sp, t->size);
            a->sp += t->size;
        }
    } else if (t->size <= 16) {
        uint32_t words = (t->size + 7) / 8;
        if (a->nx + (int)words <= 8) {
            memcpy(buf, &a->x[a->nx], t->size);
            a->nx += (int)words;
        } else {
            a->nx = 8;
            a->sp = (a->sp + 7) & ~7u;
            memcpy(buf, a->stack + a->sp, t->size);
            a->sp += words * 8;
        }
    } else {
        const void *p = (const void *)(uintptr_t)(a->nx < 8 ? a->x[a->nx++] : take_stack(a, 8, 8));
        memcpy(buf, p, t->size);
    }
}

static void dispatch(unsigned slot, const uint64_t *x, const uint64_t *v, const uint8_t *stack, void *x8,
                     uint64_t *out_x, uint64_t *out_v)
{
    memset(out_x, 0, 2 * sizeof *out_x);
    memset(out_v, 0, 4 * sizeof *out_v);
    M32Cb *cb = slot < OCERZ_ABI_CALLBACK_SLOTS ? g_cbs[slot] : NULL;
    OcerzVM *vm = ocerz_vm_process();
    if (!cb || !vm || vm->exited)
        return;
    const M32Sig *g = cb->g, *h = cb->h;
    HostArgs a = { x, v, stack, 0, 0, 0 };
    uint32_t words[16];
    int nw = 0;
    for (int i = 0; i < h->nargs && nw < 16; i++) {
        const M32Type *ha = &h->arg[i], *ga = &g->arg[i];
        if (ha->cls == '{') {
            uint8_t hbuf[256] = { 0 }, gbuf[256] = { 0 };
            take_struct(&a, ha, hbuf);
            for (int k = 0; k < ga->n && k < ha->n; k++) {
                uint64_t raw = 0;
                memcpy(&raw, hbuf + ha->moff[k], ha->msize[k]);
                uint64_t gv = m32_scalar_out(ga->mcls[k], ha->mcls[k], raw);
                memcpy(gbuf + ga->moff[k], &gv, ga->msize[k]);
            }
            for (uint32_t off = 0; off < ga->size && nw < 16; off += 4)
                memcpy(&words[nw++], gbuf + off, 4);
            continue;
        }
        int fp = ha->cls == 'f' || ha->cls == 'd';
        uint64_t raw = fp ? (a.nv < 8 ? a.v[a.nv++] : take_stack(&a, ha->size, ha->size))
                          : (a.nx < 8 ? a.x[a.nx++] : take_stack(&a, ha->size, ha->size));
        uint64_t gv = m32_scalar_out(ga->cls, ha->cls, raw);
        words[nw++] = (uint32_t)gv;
        if (ga->size == 8 && nw < 16)
            words[nw++] = (uint32_t)(gv >> 32);
    }
    static int log_cb = -1;
    if (log_cb < 0)
        log_cb = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "callbacks");
    if (log_cb)
        fprintf(stderr, "ocerz: m32: callback slot %u -> guest %#x %s (%d words)\n", slot, cb->fn, cb->gnote, nw);
    uint32_t edx = 0;
    double st0 = 0;
    uint64_t fpcr = ocerz_abi_round_swap(OCERZ_ABI_ROUND_NEAREST);
    uint32_t eax = m32_call(vm, cb->fn, words, nw, &edx, &st0);
    ocerz_abi_round_swap(fpcr & OCERZ_ABI_ROUND_MASK);
    const M32Type *gr = &g->ret, *hr = &h->ret;
    switch (gr->cls) {
    case 'v':
        break;
    case 'f': case 'd': {
        float f = (float)st0;
        uint32_t fb;
        memcpy(&fb, &f, 4);
        uint64_t db;
        memcpy(&db, &st0, 8);
        out_v[0] = hr->cls == 'f' ? fb : db;
        break;
    }
    case 'l': case 'L':
        out_x[0] = eax | (uint64_t)edx << 32;
        break;
    case '{':
        m32_log_once("structure results from guest callbacks are not supported:", cb->gnote);
        break;
    default:
        out_x[0] = m32_scalar_in(gr->cls, hr->cls, eax, "callback result", -1);
        break;
    }
    (void)x8;
}

void *m32_callback(uint32_t guest_fn, const char *guest_sig, const char *host_sig)
{
    /* a system function passed back as a callback is its own host function */
    if (guest_fn - OCERZ_DYLDAPI_LO < OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO) {
        M32Export *e = m32_export_by_id(guest_fn - (uint32_t)OCERZ_DYLDAPI_LO);
        return e && e->kind == M32_EX_FN ? e->host_fn : NULL;
    }
    if (!guest_sig || !host_sig)
        return NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nknown; i++)
        if (g_known[i].fn == guest_fn && !strcmp(g_known[i].gnote, guest_sig) && !strcmp(g_known[i].hnote, host_sig)) {
            void *addr = g_known[i].addr;
            pthread_mutex_unlock(&g_lock);
            return addr;
        }
    M32Cb *cb = calloc(1, sizeof *cb);
    cb->fn = guest_fn;
    cb->gnote = strdup(guest_sig);
    cb->hnote = strdup(host_sig);
    cb->g = m32_sig_parse(guest_sig, 1);
    cb->h = m32_sig_parse(host_sig, 0);
    unsigned slot = 0;
    void *addr = cb->g && cb->h && cb->g->nargs == cb->h->nargs ? ocerz_abi_callback_reserve(dispatch, &slot) : NULL;
    if (addr) {
        g_cbs[slot] = cb;
        if (g_nknown == g_capknown) {
            g_capknown = g_capknown ? g_capknown * 2 : 64;
            g_known = realloc(g_known, (size_t)g_capknown * sizeof *g_known);
        }
        g_known[g_nknown].fn = guest_fn;
        g_known[g_nknown].gnote = cb->gnote;
        g_known[g_nknown].hnote = cb->hnote;
        g_known[g_nknown].addr = addr;
        g_nknown++;
    } else {
        char why[300];
        snprintf(why, sizeof why, "%s / %s (%s)", guest_sig, host_sig, !cb->g ? "guest does not parse" : !cb->h ? "host does not parse"
                 : cb->g->nargs != cb->h->nargs ? "argument counts differ" : "no callback slot");
        m32_log_once("a callback could not be made:", why);
    }
    pthread_mutex_unlock(&g_lock);
    return addr;
}
