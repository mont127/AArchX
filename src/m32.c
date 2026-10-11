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
#include <execinfo.h>
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

/* OCERZ_M32LOG=keys: each key event as AppKit routes it - the key window, its first responder and whether that
 * class has a keyDown: of its own (a guest class pair answers with the guest's) - from a host local monitor */
static void keys_diag(void *unused)
{
    void *(*send)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");   /* no varargs: arm64 */
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *(*getcls)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT, "object_getClass");
    const char *(*cname)(void *) = (const char *(*)(void *))dlsym(RTLD_DEFAULT, "class_getName");
    void *(*imp_of)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "class_getMethodImplementation");
    void *ev = cls("NSEvent"), *app = send(cls("NSApplication"), sel("sharedApplication"));
    void *responder_kd = imp_of(cls("NSResponder"), sel("keyDown:"));
    void *(^h)(void *) = ^void *(void *e) {
        unsigned long type = (unsigned long)send(e, sel("type"));
        unsigned short code = (unsigned short)(uintptr_t)send(e, sel("keyCode"));
        void *win = send(app, sel("keyWindow")), *fr = win ? send(win, sel("firstResponder")) : NULL;
        void *fc = fr ? getcls(fr) : NULL;
        fprintf(stderr, "ocerz: m32: keys type %lu code %u keyWindow %s firstResponder %s (own keyDown: %s) event window %s\n",
                type, code, win ? cname(getcls(win)) : "none", fc ? cname(fc) : "none",
                fc ? (imp_of(fc, sel("keyDown:")) != responder_kd ? "yes" : "no") : "-",
                send(e, sel("window")) ? cname(getcls(send(e, sel("window")))) : "none");
        return e;
    };
    void *(*add)(void *, void *, unsigned long long, void *) = (void *(*)(void *, void *, unsigned long long, void *))send;
    add(ev, sel("addLocalMonitorForEventsMatchingMask:handler:"), (1 << 10) | (1 << 11) | (1 << 12), h);
    fprintf(stderr, "ocerz: m32: keys: watching key events\n");
}

/* OCERZ_M32_KEYTEST=<secs>[:<keycode>[/<hold ms>],...] (default 13, W; hold 400 ms): after <secs>, post each key's
 * down and, <hold> later, its up event to the app's key window, the next key a second after - keyboard tests without
 * anyone at the Mac.  Pair with a JIT block map: the blocks first translated after the first keyDown: are the path
 * the key took. */
static int keytest_n;
static unsigned short keytest_codes[16];
static void keytest_post(void *ctx)
{
    uintptr_t k = (uintptr_t)ctx;
    int down = !(k & 0x10000);
    unsigned short code = (unsigned short)k;
    struct pt { double x, y; };
    void *(*send)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    double (*send_d)(void *, void *) = (double (*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *(*mkev)(void *, void *, unsigned long, struct pt, unsigned long, double, long, void *, void *, void *,
                  signed char, unsigned short) = (void *(*)(void *, void *, unsigned long, struct pt, unsigned long,
                                                            double, long, void *, void *, void *, signed char,
                                                            unsigned short))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void (*post)(void *, void *, void *, signed char) =
        (void (*)(void *, void *, void *, signed char))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *(*str)(void *, void *, const char *) = (void *(*)(void *, void *, const char *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *app = send(cls("NSApplication"), sel("sharedApplication"));
    void *win = send(app, sel("keyWindow"));
    const char *why = "key";
    if (!win) { win = send(app, sel("mainWindow")); why = "main"; }
    long num = win ? (long)send(win, sel("windowNumber")) : 0;
    static const char map[128] = { [0] = 'a', [1] = 's', [2] = 'd', [13] = 'w', [12] = 'q', [14] = 'e', [36] = '\r',
                                   [48] = '\t', [49] = ' ', [53] = 0x1b };
    char ch[2] = { code < 128 ? map[code] : 0, 0 };
    void *chars = str(cls("NSString"), sel("stringWithUTF8String:"), ch);
    double t = send_d(send(cls("NSProcessInfo"), sel("processInfo")), sel("systemUptime"));
    void *e = mkev(cls("NSEvent"), sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:"
                                        "characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
                   down ? 10 : 11, (struct pt){ 0, 0 }, 0, t, num, NULL, chars, chars, 0, code);
    post(app, sel("postEvent:atStart:"), e, 0);
    fprintf(stderr, "ocerz: m32: keytest %s %u to %s window %ld (%s)\n", down ? "down" : "up", code,
            why, num, win ? "found" : "none");
}

static void keytest_start(const char *spec)
{
    double at = strtod(spec, NULL);
    unsigned hold[16];
    const char *p = strchr(spec, ':');
    while (p && keytest_n < 16) {
        char *e;
        keytest_codes[keytest_n] = (unsigned short)strtoul(p + 1, &e, 0);
        hold[keytest_n++] = *e == '/' ? (unsigned)strtoul(e + 1, NULL, 0) : 400;
        p = strchr(p + 1, ',');
    }
    if (!keytest_n) {
        keytest_codes[0] = 13;
        hold[keytest_n++] = 400;
    }
    for (int i = 0; i < keytest_n; i++) {
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(),
                         (void *)(uintptr_t)keytest_codes[i], keytest_post);
        at += hold[i] / 1000.0;
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(),
                         (void *)(uintptr_t)(keytest_codes[i] | 0x10000), keytest_post);
        at += 1;
    }
}

/* OCERZ_M32LOG=monitors: every local event monitor AppKit adds (the game's and its own), removes and frees, with
 * the observer, its handler and who asked (a short backtrace) */
static void monitors_diag(void)
{
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL))
        return;
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *(*meta)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT, "object_getClass");
    void *(*get_m)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
    void *(*set_imp)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "method_setImplementation");
    void *(*imp_block)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT, "imp_implementationWithBlock");
    void *ev = meta(cls("NSEvent"));
    static void *(*add)(void *, void *, unsigned long long, long, void *);
    static void (*rem)(void *, void *, void *);
    static void (*dealloc)(void *, void *);
    void *m = get_m(ev, sel("addLocalMonitorForEventsMatchingMask:placement:handler:"));
    if (m && !add)
        add = (void *(*)(void *, void *, unsigned long long, long, void *))set_imp(m,
            imp_block(^void *(void *self, unsigned long long mask, long place, void *h) {
                void *o = add(self, sel("addLocalMonitorForEventsMatchingMask:placement:handler:"), mask, place, h);
                void *bt[5];
                int n = backtrace(bt, 5);
                Dl_info di;
                fprintf(stderr, "ocerz: m32: monitors: add mask %#llx handler %p -> observer %p from", mask, h, o);
                for (int k = 1; k < n; k++)
                    fprintf(stderr, " %s", dladdr(bt[k], &di) && di.dli_sname ? di.dli_sname : "?");
                fprintf(stderr, "\n");
                return o;
            }));
    m = get_m(ev, sel("removeMonitor:"));
    if (m && !rem)
        rem = (void (*)(void *, void *, void *))set_imp(m, imp_block(^(void *self, void *o) {
            fprintf(stderr, "ocerz: m32: monitors: remove observer %p\n", o);
            rem(self, sel("removeMonitor:"), o);
        }));
    m = get_m(cls("_NSLocalEventObserver"), sel("dealloc"));
    if (m && !dealloc)
        dealloc = (void (*)(void *, void *))set_imp(m, imp_block(^(void *self) {
            fprintf(stderr, "ocerz: m32: monitors: observer %p freed (handler %p)\n", self, ((void **)self)[1]);
            dealloc(self, sel("dealloc"));
        }));
}

/* OCERZ_M32_PEEKW=<addr>,<addr>...: 20 s (OCERZ_M32_PEEKW_SECS) in, the wchar_t (UTF-32) strings at those guest addresses - literals a game
 * builds at run time in __bss (UE3's TEXT() strings) */
static void peekw(void *unused)
{
    char list[512];
    snprintf(list, sizeof list, "%s", getenv("OCERZ_M32_PEEKW"));
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        uint32_t a = (uint32_t)strtoul(t + (*t == '#' || *t == '*' || *t == '$'), NULL, 16);
        if (*t == '$') {   /* $addr: the C string or CF object the word there points at */
            uint32_t w = ocerz_addr_readable(a) ? m32_rd(a) : 0;
            char out[200] = "";
            uint64_t isa = w && ocerz_addr_readable(w) && ocerz_addr_readable(w + 7) ? *(uint64_t *)m32_h(w) : 0;
            if (m32_is_handle(w) || (isa >> 32 && isa >> 32 < 0x10)) {   /* a host CF object, or one living in the window */
                void *(*desc)(const void *) = (void *(*)(const void *))dlsym(RTLD_DEFAULT, "CFCopyDescription");
                int (*cstr)(const void *, char *, long, uint32_t) =
                    (int (*)(const void *, char *, long, uint32_t))dlsym(RTLD_DEFAULT, "CFStringGetCString");
                void *d = desc ? desc(m32_host(w)) : NULL;
                if (d)
                    cstr(d, out, sizeof out, 0x08000100);
            } else
                for (int n = 0; w && ocerz_addr_readable(w + (uint32_t)n) && n < (int)sizeof out - 1 && *(uint8_t *)m32_h(w + (uint32_t)n); n++)
                    out[n] = *(char *)m32_h(w + (uint32_t)n), out[n + 1] = 0;
            fprintf(stderr, "ocerz: m32: peekw %#x -> %#x \"%s\"\n", a, w, out);
            continue;
        }
        if (*t == '#') {   /* #addr: the word there */
            fprintf(stderr, "ocerz: m32: peekw %#x word %#x\n", a, ocerz_addr_readable(a) ? m32_rd(a) : 0xdeadu);
            continue;
        }
        if (*t == '*' && ocerz_addr_readable(a = (uint32_t)strtoul(t + 1, NULL, 16)))   /* *addr: an FString's data */
            a = m32_rd(a);
        char out[200];
        int n = 0;
        for (uint32_t k = 0; ocerz_addr_readable(a + 4 * k) && n < (int)sizeof out - 1; k++) {
            uint32_t c = m32_rd(a + 4 * k);
            if (!c)
                break;
            out[n++] = c >= 32 && c < 127 ? (char)c : '?';
        }
        out[n] = 0;
        fprintf(stderr, "ocerz: m32: peekw %#x = \"%s\"\n", a, out);
    }
}

int m32_active;

int m32_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    m32_active = 1;
    if (getenv("OCERZ_M32_PEEKW"))
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (getenv("OCERZ_M32_PEEKW_SECS") ? atoi(getenv("OCERZ_M32_PEEKW_SECS")) : 20) *
                                                              NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, peekw);
    if (getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "keys"))
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, keys_diag);
    if (getenv("OCERZ_M32_KEYTEST"))
        keytest_start(getenv("OCERZ_M32_KEYTEST"));
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
    if (getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "monitors"))
        monitors_diag();   /* after the window and images: AppKit loaded at the very start stalls the game */
    return ocerz_vm_run(vm);
}
