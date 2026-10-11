/*
 * Objective-C 1 exceptions of i386 guests (include/ocerz/m32_objc.h).
 *
 * The fragile runtime's @try is setjmp-based: objc_exception_try_enter(data) registers the frame, the compiler
 * calls _setjmp(data->buf), and objc_exception_throw longjmps to the innermost frame with the exception in
 * data->pointers[0] (struct _objc_exception_data { int buf[18]; void *pointers[4]; }).  The frames are a
 * per-thread list here; the guest's _setjmp/_longjmp are m32's own (src/m32_libsystem.c), so a throw restores
 * guest registers only.  A host NSException stopped at a crossing (src/m32_catch.m) is thrown the same way.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ocerz/interp.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32_objcrt.h"
#include "ocerz/vm.h"

#define MAX_FRAMES 256
static __thread uint32_t t_frames[MAX_FRAMES];
static __thread int t_nframes;
static uint32_t g_uncaught;   /* NSSetUncaughtExceptionHandler's guest function */

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

static int sp_try_enter(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (t_nframes < MAX_FRAMES)
        t_frames[t_nframes++] = m32_arg(cpu, 0);
    RET(0);
}

static int sp_try_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0);
    while (t_nframes && t_frames[t_nframes - 1] != d)
        t_nframes--;   /* frames a longjmp skipped */
    if (t_nframes)
        t_nframes--;
    RET(0);
}

static int sp_extract(struct OcerzVM *vm, OcerzCPU *cpu) { RET(m32_rd(m32_arg(cpu, 0) + 72)); }

static int kind_of(uint32_t obj, uint32_t cls)
{
    void *h = m32_objc_to_host(obj), *hc = m32_class_host(cls);
    BOOL (*isKind)(id, SEL, Class) = (BOOL (*)(id, SEL, Class))M32_MSGSEND;
    return h && hc && isKind(h, sel_registerName("isKindOfClass:"), hc);
}

static int sp_match(struct OcerzVM *vm, OcerzCPU *cpu) { RET(kind_of(m32_arg(cpu, 1), m32_arg(cpu, 0))); }

int m32_objc_guest_throw(OcerzCPU *cpu, uint32_t exc)
{
    if (!t_nframes) {
        void *h = m32_objc_to_host(exc);
        const char *(*str)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
        void *reason = h ? ((void *(*)(id, SEL))M32_MSGSEND)(h, sel_registerName("description")) : NULL;
        fprintf(stderr, "ocerz: m32: uncaught Objective-C exception: %s\n",
                reason ? str(reason, sel_registerName("UTF8String")) : "(nil)");
        if (g_uncaught) {
            uint32_t args[1] = { exc };
            m32_call(ocerz_vm_process(), g_uncaught, args, 1, NULL, NULL);
        }
        fflush(NULL);
        abort();
    }
    uint32_t d = t_frames[--t_nframes];
    m32_wr(d + 72, exc);
    /* the guest's own longjmp layout (m32_libsystem.c): ebx, esi, edi, ebp, esp, eip */
    cpu->gpr[OCERZ_RBX] = m32_rd(d);
    cpu->gpr[OCERZ_RSI] = m32_rd(d + 4);
    cpu->gpr[OCERZ_RDI] = m32_rd(d + 8);
    cpu->gpr[OCERZ_RBP] = m32_rd(d + 12);
    cpu->gpr[OCERZ_RSP] = m32_rd(d + 16);
    cpu->rip = m32_rd(d + 20);
    cpu->gpr[OCERZ_RAX] = 1;
    return OCERZ_STEP_OK;
}

static int sp_throw(struct OcerzVM *vm, OcerzCPU *cpu) { return m32_objc_guest_throw(cpu, m32_arg(cpu, 0)); }

static int sp_uncaught_handler(struct OcerzVM *vm, OcerzCPU *cpu)
{
    g_uncaught = m32_arg(cpu, 0);
    RET(0);
}

static int sp_get_uncaught_handler(struct OcerzVM *vm, OcerzCPU *cpu) { RET(g_uncaught); }

const M32SpecialEntry m32_exc_specials[] = {
    { "_objc_exception_try_enter", sp_try_enter }, { "_objc_exception_try_exit", sp_try_exit },
    { "_objc_exception_extract", sp_extract }, { "_objc_exception_match", sp_match },
    { "_objc_exception_throw", sp_throw },
    { "_NSSetUncaughtExceptionHandler", sp_uncaught_handler },
    { "_NSGetUncaughtExceptionHandler", sp_get_uncaught_handler },
    { NULL, NULL }
};
