/*
 * m32's specials and raw system calls, called directly on a 32-bit cpu over the 4 GB window.  These are the calls
 * the i386 programs in tests/m32 cannot pin on a machine without Xcode 26, whose ld-classic is the last linker
 * that links i386 (tests/m32/build.sh): keymgr, _dyld_func_lookup, the wcsrtombs family, NSLogv, the raw system
 * calls a self-loading program makes and the dyld m32 hands it (src/m32_selfload.c).
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#define STACK    0x00800000u
#define DATA     0x00900000u
#define RET_ADDR 0x00123456u

static OcerzVM g_vm;
static int g_checks, g_failures;

static void check(const char *what, int ok)
{
    g_checks++;
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        g_failures++;
    }
}

static OcerzCPU *cpu(void) { return &g_vm.cpu; }

/* a cdecl call of the special for symbol: the return address at the stack pointer, the words above it; eax */
static uint32_t call(const char *symbol, int n, const uint32_t *args)
{
    M32Special fn = m32_special(symbol);
    check(symbol, fn != NULL);
    if (!fn)
        return 0xdeadbeefu;
    m32_wr(STACK, RET_ADDR);
    for (int i = 0; i < n; i++)
        m32_wr(STACK + 4 + 4 * (uint32_t)i, args[i]);
    cpu()->gpr[OCERZ_RSP] = STACK;
    cpu()->rip = 0;
    fn(&g_vm, cpu());
    check("a special returns to its caller", cpu()->rip == RET_ADDR && cpu()->gpr[OCERZ_RSP] == STACK + 4);
    return (uint32_t)cpu()->gpr[OCERZ_RAX];
}

/* a raw system call by sysenter: number in eax, arguments above the return address at ecx, back to edx */
static uint32_t sysenter(uint32_t eax, int n, const uint32_t *args)
{
    m32_wr(STACK, 0x0badf00du);
    for (int i = 0; i < n; i++)
        m32_wr(STACK + 4 + 4 * (uint32_t)i, args[i]);
    cpu()->cc_op = OCERZ_CC_NONE;
    cpu()->gpr[OCERZ_RAX] = eax;
    cpu()->gpr[OCERZ_RCX] = STACK;
    cpu()->gpr[OCERZ_RDX] = RET_ADDR;
    cpu()->gpr[OCERZ_RSP] = 0x1234u;
    m32_raw_syscall(&g_vm, cpu(), 1);
    check("sysenter returns to edx with the stack at ecx", cpu()->rip == RET_ADDR && cpu()->gpr[OCERZ_RSP] == STACK);
    return (uint32_t)cpu()->gpr[OCERZ_RAX];
}

static int carry(void) { return (cpu()->rflags & OCERZ_CF) != 0; }

static void put_str(uint32_t g, const char *s) { memcpy(m32_h(g), s, strlen(s) + 1); }

static void test_keymgr(void)
{
    uint32_t a[2] = { 14, DATA };   /* KEYMGR_ATEXIT_LIST, as crt3.o asks */
    m32_wr(DATA, 0xcccccccc);
    check("keymgr: get_and_lock_2 succeeds", call("__keymgr_get_and_lock_processwide_ptr_2", 2, a) == 0);
    check("keymgr: a key starts NULL", m32_rd(DATA) == 0);
    uint32_t s[2] = { 14, 0x4321 };
    check("keymgr: set_and_unlock succeeds", call("__keymgr_set_and_unlock_processwide_ptr", 2, s) == 0);
    check("keymgr: get_and_lock_2 reads it back", call("__keymgr_get_and_lock_processwide_ptr_2", 2, a) == 0 &&
                                                   m32_rd(DATA) == 0x4321);
    uint32_t u[1] = { 14 };
    call("__keymgr_unlock_processwide_ptr", 1, u);
    check("keymgr: the older get_and_lock returns it", call("__keymgr_get_and_lock_processwide_ptr", 1, u) == 0x4321);
    call("__keymgr_unlock_processwide_ptr", 1, u);
    uint32_t other[2] = { 302, DATA };
    check("keymgr: keys are separate", call("__keymgr_get_and_lock_processwide_ptr_2", 2, other) == 0 && m32_rd(DATA) == 0);
    call("__keymgr_unlock_processwide_ptr", 1, other);
}

static void test_dyld_func_lookup(void)
{
    put_str(DATA + 0x100, "__dyld_make_delayed_module_initializer_calls");
    uint32_t a[2] = { DATA + 0x100, DATA + 0x200 };
    check("_dyld_func_lookup answers crt1.o's initializer call", call("__dyld_func_lookup", 2, a) == 1);
    uint32_t fn = m32_rd(DATA + 0x200);
    check("... with a trap address", fn >= OCERZ_DYLDAPI_LO && fn < OCERZ_DYLDAPI_HI);
    put_str(DATA + 0x100, "__dyld_image_count");
    check("_dyld_func_lookup answers a dyld API m32 has", call("__dyld_func_lookup", 2, a) == 1 &&
                                                          m32_rd(DATA + 0x200) != 0);
    put_str(DATA + 0x100, "__dyld_no_such_function");
    check("_dyld_func_lookup answers 0 for what m32 lacks", call("__dyld_func_lookup", 2, a) == 0 &&
                                                            m32_rd(DATA + 0x200) == 0);
}

static void test_wide_conversions(void)
{
    uint32_t wide = DATA + 0x300, cell = DATA + 0x400, out = DATA + 0x500;
    const uint32_t hi[3] = { 'h', 'i', 0 };
    memcpy(m32_h(wide), hi, sizeof hi);
    m32_wr(cell, wide);
    memset(m32_h(out), 'x', 16);
    uint32_t a[4] = { out, cell, 16, 0 };
    check("wcsrtombs converts", call("_wcsrtombs", 4, a) == 2 && !strcmp((const char *)m32_h(out), "hi"));
    check("wcsrtombs sets the guest's pointer to NULL at the end", m32_rd(cell) == 0);
    put_str(DATA + 0x600, "xy");
    m32_wr(cell, DATA + 0x600);
    uint32_t b[4] = { out, cell, 1, 0 };
    check("mbsrtowcs converts one", call("_mbsrtowcs", 4, b) == 1 && m32_rd(out) == 'x');
    check("mbsrtowcs advances the guest's pointer", m32_rd(cell) == DATA + 0x601);
    m32_wr(cell, DATA + 0x600);
    uint32_t c[4] = { 0, cell, 0, 0 };
    check("mbsrtowcs without a destination counts", call("_mbsrtowcs", 4, c) == 2 && m32_rd(cell) == DATA + 0x600);
}

/* NSLogv(format, va_list): the va_list is the address of the format's first argument on the guest's stack.
 * Foundation is opened here, as m32 finds it in a game's process; ocerz itself links no framework. */
static void test_nslogv(void)
{
    if (!dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY | RTLD_GLOBAL))
        return;
    void *(*make)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    void (*release)(void *) = (void (*)(void *))dlsym(RTLD_DEFAULT, "CFRelease");
    void *fmt = make ? make(NULL, "nslogv %d %s|", 0x08000100 /* kCFStringEncodingUTF8 */) : NULL;
    check("a CFString for the format", fmt != NULL);
    if (!fmt)
        return;
    put_str(DATA + 0x800, "ok");
    m32_wr(DATA + 0x900, 7);
    m32_wr(DATA + 0x904, DATA + 0x800);
    int pipefd[2], saved = dup(2);
    if (pipe(pipefd) != 0)
        return;
    dup2(pipefd[1], 2);
    uint32_t a[2] = { m32_handle(fmt), DATA + 0x900 };
    call("_NSLogv", 2, a);
    dup2(saved, 2);
    close(saved);
    close(pipefd[1]);
    char buf[512] = "";
    ssize_t n = read(pipefd[0], buf, sizeof buf - 1);
    close(pipefd[0]);
    buf[n > 0 ? n : 0] = 0;
    check("NSLogv logs the format's arguments from the va_list", strstr(buf, "nslogv 7 ok|") != NULL);
    release(fmt);
}

static void test_raw_syscalls(void)
{
    check("getpid", sysenter(20, 0, NULL) == (uint32_t)getpid() && !carry());
    uint32_t mp[3] = { DATA, 0x1000, PROT_READ };
    check("the argument size above the number is ignored", sysenter(74 | 12u << 16, 3, mp) == 0 && !carry());
    check("an unknown call fails with ENOSYS and the carry set", sysenter(9999, 0, NULL) == ENOSYS && carry());
    uint32_t mm[7] = { 0, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0xffffffffu, 0, 0 };
    uint32_t g = sysenter(197, 7, mm);
    check("mmap maps in the window", !carry() && g && ocerz_addr_readable(g) && ocerz_addr_readable(g + 0x3fff));
    put_str(DATA + 0x700, "/usr/lib/dyld");
    uint32_t op[3] = { DATA + 0x700, O_RDONLY, 0 };
    int fd = (int)sysenter(5, 3, op);
    check("opening the dyld gives m32's own", !carry() && fd >= 0);
    uint8_t h[0x200];
    check("... which reads", fd >= 0 && pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h);
    uint32_t w[8];
    memcpy(w, h, sizeof w);
    check("... an i386 dylinker", w[0] == MH_MAGIC && w[1] == CPU_TYPE_I386 && w[3] == MH_DYLINKER);
    uint32_t eip;
    memcpy(&eip, h + 28 + 56 + 16 + 4 * 10, 4);
    check("... whose entry is the dyld trap", eip == M32_TRAP(M32_ID_DYLD));
    if (fd >= 0)
        close(fd);
}

int main(void)
{
    if (ocerz_mem_init(0, M32_WINDOW) != OCERZ_OK ||
        ocerz_map_fixed(STACK - 0x10000, 0x10000 + 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(DATA, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(M32_HANDLE_LO, M32_HANDLE_HI - M32_HANDLE_LO, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "test_m32: cannot set up the window\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;
    ocerz_cpu_reset(cpu());
    cpu()->mode32 = 1;
    test_keymgr();
    test_dyld_func_lookup();
    test_wide_conversions();
    test_nslogv();
    test_raw_syscalls();
    printf("test_m32: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
