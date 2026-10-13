/*
 * Running an i386 Mach-O program (include/ocerz/m32.h).
 *
 * m32_run reserves the 4 GB window, loads the program, builds the initial stack the way the kernel and dyld leave
 * it, starts a 32-bit cpu and runs it.  An LC_MAIN program starts at main with dyld's frame (argc, argv, envp,
 * apple) and returns into the M32_ID_EXIT trap; an LC_UNIXTHREAD program starts at its own start routine with argc
 * at the stack pointer.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "ocerz/bridge.h"
#include "ocerz/m32.h"
#include "ocerz/mem.h"
#include "ocerz/m32_image.h"
#include "ocerz/decode.h"
#include "ocerz/syscall.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

void ocerz_dyld_set_main_path(const char *path);

void m32_ret(OcerzCPU *cpu, uint32_t eax, uint32_t edx, int pop_extra)
{
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    cpu->rip = m32_rd(esp);
    cpu->gpr[OCERZ_RSP] = esp + 4 + (uint32_t)pop_extra;
    cpu->gpr[OCERZ_RAX] = eax;
    cpu->gpr[OCERZ_RDX] = edx;
}

void m32_ret_st0(OcerzCPU *cpu, double v)
{
    m32_ret(cpu, (uint32_t)cpu->gpr[OCERZ_RAX], (uint32_t)cpu->gpr[OCERZ_RDX], 0);
    cpu->ftop = (cpu->ftop - 1) & 7;
    cpu->fpr[cpu->ftop] = v;
    cpu->ftw = 0xff;
}

static uint32_t push_str(uint32_t *p, const char *s)
{
    size_t n = strlen(s) + 1;
    *p -= (uint32_t)n;
    memcpy(m32_h(*p), s, n);
    return *p;
}

/* argc, argv[], 0, envp[], 0, apple[], 0 at the returned stack pointer; the strings above them */
static uint32_t build_frame(uint32_t top, const char *path, int argc, char **argv, char **envp)
{
    int envc = 0;
    while (envp && envp[envc])
        envc++;
    char exe[1100];
    char real[1024];
    snprintf(exe, sizeof exe, "executable_path=%s", realpath(path, real) ? real : path);
    uint32_t p = top, *av = calloc((size_t)argc + 1, 4), *ev = calloc((size_t)envc + 1, 4), apple;
    for (int i = 0; i < argc; i++)
        av[i] = push_str(&p, argv[i]);
    for (int i = 0; i < envc; i++)
        ev[i] = push_str(&p, envp[i]);
    apple = push_str(&p, exe);
    p &= ~15u;
    uint32_t words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 + 2;
    uint32_t sp = (p - 4 * words) & ~15u, w = sp;
    m32_wr(w, (uint32_t)argc); w += 4;
    for (int i = 0; i < argc; i++, w += 4) m32_wr(w, av[i]);
    m32_wr(w, 0); w += 4;
    for (int i = 0; i < envc; i++, w += 4) m32_wr(w, ev[i]);
    m32_wr(w, 0); w += 4;
    m32_wr(w, apple); w += 4;
    m32_wr(w, 0);
    free(av);
    free(ev);
    return sp;
}


/* OCERZ_M32_ACTIVATE=1 (measurement runs): bring the game's app to the front 5 s after launch, as a Finder launch
 * would.  Launched from a shell it stays behind the active app, and macOS then runs a background app on the
 * efficiency cores (always in Low Power Mode, after a while otherwise), which makes frame rates meaningless. */
static void activate_now(void *unused)
{
    void *(*send)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void (*send_b)(void *, void *, int) = (void (*)(void *, void *, int))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    if (!send || !cls || !sel || !cls("NSApplication"))
        return;
    void *app = send(cls("NSApplication"), sel("sharedApplication"));
    if (app)
        send_b(app, sel("activateIgnoringOtherApps:"), 1);
}

int m32_active;

int m32_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    m32_active = 1;
    if (getenv("OCERZ_M32_ACTIVATE"))
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
                         activate_now);
    m32_log_imports = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "imports");
    if (ocerz_mem_init(0, M32_WINDOW) != OCERZ_OK)
        return 70;
    /* never handed out: the trap window, the handle cells (readable: a handle's isa word) and the top */
    ocerz_map_fixed(OCERZ_DYLDAPI_LO, OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO, PROT_NONE);
    ocerz_map_fixed(M32_HANDLE_LO, M32_HANDLE_HI - M32_HANDLE_LO, PROT_READ | PROT_WRITE);
    ocerz_map_fixed(M32_TOP_LO, (uint32_t)(M32_WINDOW - M32_TOP_LO), PROT_NONE);
    char real[1024];
    ocerz_dyld_set_main_path(realpath(path, real) ? real : path);   /* CoreFoundation's main bundle is the game's */
    ocerz_bridge_set_process_args(argc, argv);
    m32_hud_start();
    M32Image *img = m32_load_program(path);
    if (!img)
        return 65;
    if (m32_heap_init() != 0)
        return 70;
    if (m32_fixup_all(0)) {
        fprintf(stderr, "ocerz: m32: %s: required imports are unresolved\n", path);
        if (!getenv("OCERZ_M32_KEEP_GOING"))
            return 71;
    }
    if (!img->entry_main && !img->entry_thread) {
        fprintf(stderr, "ocerz: m32: %s has no entry point\n", path);
        return 65;
    }
    ocerz_ldt_install(M32_CS, 0, 0xfffff, 0xfb, 1, 0, 1);
    const uint32_t stack_size = 8u << 20;
    uint32_t stack_lo = (uint32_t)ocerz_map_anywhere(stack_size, PROT_READ | PROT_WRITE);
    if (!stack_lo)
        return 70;
    ocerz_vm_set_main_stack(stack_lo, stack_lo + stack_size);
    uint32_t sp = build_frame(stack_lo + stack_size, path, argc, argv, envp);
    OcerzCPU *cpu = &vm->cpu;
    ocerz_cpu_reset(cpu);
    cpu->gs_base = m32_static_alloc(4 * M32_TSD_KEYS, 16);   /* the main thread's specific data (m32_tsd_set) */
    cpu->mode32 = 1;
    cpu->cs_sel = M32_CS;
    cpu->seg_sel[OCERZ_SREG_CS] = M32_CS;
    if (img->entry_main) {
        /* dyld's LC_MAIN contract: main(argc, argv, envp, apple), then exit(result) */
        uint32_t argv_g = sp + 4, envp_g = argv_g + 4 * ((uint32_t)argc + 1), apple_g = envp_g;
        while (m32_rd(apple_g))
            apple_g += 4;
        apple_g += 4;
        uint32_t esp = ((sp - 20) & ~15u) - 4;   /* (esp + 4) % 16 == 0 at main's first instruction */
        m32_wr(esp, M32_TRAP(M32_ID_EXIT));
        m32_wr(esp + 4, (uint32_t)argc);
        m32_wr(esp + 8, argv_g);
        m32_wr(esp + 12, envp_g);
        m32_wr(esp + 16, apple_g);
        cpu->gpr[OCERZ_RSP] = esp;
        cpu->rip = img->entry_main;
    } else {
        cpu->gpr[OCERZ_RSP] = sp;
        cpu->rip = img->entry_thread;
    }
    OCERZ_LOG("m32: %s entry %#x esp %#x\n", path, (unsigned)cpu->rip, (unsigned)cpu->gpr[OCERZ_RSP]);
    ocerz_vm_install_handlers(vm);
    /* initializers, dependencies before their dependents (m32_images is in load order, the main image first) */
    uint32_t argv_g = sp + 4, envp_g = argv_g + 4 * ((uint32_t)argc + 1), apple_g = envp_g;
    while (m32_rd(apple_g))
        apple_g += 4;
    apple_g += 4;
    uint32_t init_args[5] = { (uint32_t)argc, argv_g, envp_g, apple_g, 0 };
    m32_argc_var = m32_static_alloc(16, 4);   /* what _NSGetArgc and friends point at */
    m32_argv_var = m32_argc_var + 4;
    m32_environ_var = m32_argc_var + 8;
    m32_progname_var = m32_argc_var + 12;
    m32_wr(m32_argc_var, (uint32_t)argc);
    m32_wr(m32_argv_var, argv_g);
    m32_wr(m32_environ_var, envp_g);
    const char *base = strrchr(path, '/');
    m32_wr(m32_progname_var, m32_cstring(base ? base + 1 : path));
    for (int i = m32_nimages - 1; i >= 0 && !vm->exited; i--)
        m32_run_initializers(vm, m32_images[i], init_args);
    if (vm->exited)
        return vm->exit_code;
    m32_started = 1;
    return ocerz_vm_run(vm);
}
