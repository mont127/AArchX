/*
 * m32's specials and raw system calls, called directly on a 32-bit cpu over the 4 GB window.  These are the calls
 * the i386 programs in tests/m32 cannot pin on a machine without Xcode 26, whose ld-classic is the last linker
 * that links i386 (tests/m32/build.sh): keymgr, _dyld_func_lookup, the wcsrtombs family, NSLogv, the raw system
 * calls a self-loading program makes and the dyld m32 hands it (src/m32_selfload.c).
 *
 * QuickDraw's GWorlds and drawing (src/m32_qd.c) are checked pixel by pixel in their guest memory.
 *
 * `test_m32 carbon`, which make check-m32 runs once runtime/apis32 is generated, crosses Carbon calls through those
 * databases as translated code does: the window, menu and title functions 32-bit code alone is declared to have and
 * arm64 HIToolbox still exports (tools/sdkgen, declare_hidden), and CreateNewWindow without the compositing
 * attribute, which 64-bit HIToolbox refuses and m32 adds (src/m32_carbon.c); then a window's QuickDraw port, the
 * graphics devices, cursors and the region calls QuickDraw still exports, through tools/sdkgen/headers/QuickDrawRetained.h.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32_sig.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
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

/* a cdecl call of an import through its trap, as translated code makes it: m32_export's address, crossed by the
 * record runtime/apis32 has for it */
static uint32_t cross(const char *lib, const char *symbol, int n, const uint32_t *args)
{
    uint32_t trap = m32_export(lib, symbol);
    check(symbol, trap != 0);
    m32_wr(STACK, RET_ADDR);
    for (int i = 0; i < n; i++)
        m32_wr(STACK + 4 + 4 * (uint32_t)i, args[i]);
    cpu()->gpr[OCERZ_RSP] = STACK;
    cpu()->rip = trap;
    m32_trap(&g_vm, cpu());
    check("a crossing returns to its caller", cpu()->rip == RET_ADDR && cpu()->gpr[OCERZ_RSP] == STACK + 4);
    return (uint32_t)cpu()->gpr[OCERZ_RAX];
}

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

#define CARBON "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon"
#define APPSERVICES "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices"

/* the 32-bit pixel at (h, v) of the port whose PixMap handle is pmh, as 0xRRGGBB (QuickDraw's xRGB, big-endian) */
static uint32_t qd_pixel(uint32_t pmh, int h, int v)
{
    uint32_t pm[1] = { pmh };
    uint32_t base = call("_GetPixBaseAddr", 1, pm), rb = call("_GetPixRowBytes", 1, pm);
    const uint8_t *px = (const uint8_t *)m32_h(base + (uint32_t)v * rb + 4 * (uint32_t)h);
    return (uint32_t)px[1] << 16 | (uint32_t)px[2] << 8 | px[3];
}

static uint32_t qd_rect(uint32_t g, int16_t top, int16_t left, int16_t bottom, int16_t right)
{
    const int16_t r[4] = { top, left, bottom, right };
    memcpy(m32_h(g), r, sizeof r);
    return g;
}

static void qd_rgb(uint32_t g, uint16_t r, uint16_t gr, uint16_t b)
{
    const uint16_t c[3] = { r, gr, b };
    memcpy(m32_h(g), c, sizeof c);
}

/* red, after the display's color matching (0xff0000 shows as 0xff2500 or so) */
static int reddish(uint32_t c) { return (c >> 16 & 0xff) > 0xe0 && (c >> 8 & 0xff) < 0x60 && (c & 0xff) < 0x60; }

/* the pixel at (h, v) of a window's content view as HIToolbox draws it, through every draw handler, as 0xRRGGBB:
 * whatever a window's QuickDraw port shows */
static uint32_t view_pixel(void *window, int h, int v)
{
    typedef struct { uint32_t signature; int32_t id; } ViewID;
    void *hi = dlopen(CARBON, RTLD_LAZY);
    void *(*root)(void *) = (void *(*)(void *))dlsym(hi, "HIViewGetRoot");
    int32_t (*find)(void *, ViewID, void **) = (int32_t (*)(void *, ViewID, void **))dlsym(hi, "HIViewFindByID");
    const ViewID *content_id = (const ViewID *)dlsym(hi, "kHIViewWindowContentID");
    int32_t (*offscreen)(void *, uint32_t, CGRect *, CGImageRef *) =
        (int32_t (*)(void *, uint32_t, CGRect *, CGImageRef *))dlsym(hi, "HIViewCreateOffscreenImage");
    size_t (*width)(CGImageRef) = (size_t (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageGetWidth");
    size_t (*height)(CGImageRef) = (size_t (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageGetHeight");
    CGColorSpaceRef (*rgb)(void) = (CGColorSpaceRef (*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB");
    CGContextRef (*bitmap)(void *, size_t, size_t, size_t, size_t, CGColorSpaceRef, uint32_t) =
        (CGContextRef (*)(void *, size_t, size_t, size_t, size_t, CGColorSpaceRef, uint32_t))dlsym(RTLD_DEFAULT, "CGBitmapContextCreate");
    void (*draw)(CGContextRef, CGRect, CGImageRef) = (void (*)(CGContextRef, CGRect, CGImageRef))dlsym(RTLD_DEFAULT, "CGContextDrawImage");
    void (*release)(const void *) = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
    void *content = NULL;
    CGRect frame;
    CGImageRef img = NULL;
    if (!root || !find || !content_id || !offscreen || find(root(window), *content_id, &content) || !content ||
        offscreen(content, 0, &frame, &img) || !img)
        return 0xdeadbeef;
    size_t w = width(img), ht = height(img), scale = frame.size.width > 0 ? w / (size_t)frame.size.width : 1;
    uint8_t *px = calloc(w * ht, 4);
    CGColorSpaceRef cs = rgb();
    CGContextRef c = bitmap(px, w, ht, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    draw(c, CGRectMake(0, 0, (CGFloat)w, (CGFloat)ht), img);
    const uint8_t *at = px + ((size_t)v * scale + scale / 2) * w * 4 + ((size_t)h * scale + scale / 2) * 4;
    uint32_t value = (uint32_t)at[0] << 16 | (uint32_t)at[1] << 8 | at[2];
    release(c);
    release(cs);
    release(img);
    free(px);
    return value;
}

/* tsearch's trees live in guest memory, compared and walked by guest functions (src/m32_tsearch.c) */
#define CODE 0x00a00000u
static void test_tsearch(void)
{
    /* int compare(const int *a, const int *b) { return *a - *b; } */
    static const uint8_t cmp[] = { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00, 0x8b, 0x4c, 0x24, 0x08, 0x2b, 0x01, 0xc3 };
    /* void action(const void *node, VISIT v, int level) { if (v == postorder || v == leaf) visits++; } */
    uint8_t act[] = { 0x8b, 0x44, 0x24, 0x08, 0x83, 0xf8, 0x01, 0x74, 0x05, 0x83, 0xf8, 0x03, 0x75, 0x06,
                      0xff, 0x05, 0, 0, 0, 0, 0xc3 };
    uint32_t visits = DATA + 0x1100;
    memcpy(act + 16, &visits, 4);
    memcpy(m32_h(CODE), cmp, sizeof cmp);
    memcpy(m32_h(CODE + 0x40), act, sizeof act);
    uint32_t root = DATA + 0x1104, keys = DATA + 0x1120;
    m32_wr(root, 0);
    const int32_t vals[7] = { 50, 20, 80, 10, 30, 70, 90 };
    int inserted = 1;
    for (int i = 0; i < 7; i++) {
        m32_wr(keys + 4 * (uint32_t)i, (uint32_t)vals[i]);
        uint32_t node = call("_tsearch", 3, (uint32_t[]){ keys + 4 * (uint32_t)i, root, CODE });
        inserted &= node != 0 && m32_rd(node) == keys + 4 * (uint32_t)i;   /* a node's first word is its key */
    }
    check("tsearch inserts, its node's first word the key", inserted);
    m32_wr(DATA + 0x1140, 30);
    uint32_t found = call("_tfind", 3, (uint32_t[]){ DATA + 0x1140, root, CODE });
    check("tfind finds by the guest comparison", found && m32_rd(found) == keys + 16);
    uint32_t again = call("_tsearch", 3, (uint32_t[]){ DATA + 0x1140, root, CODE });
    check("tsearch of a key already there hands back its node", again == found);
    m32_wr(DATA + 0x1144, 31);
    check("tfind of a missing key", call("_tfind", 3, (uint32_t[]){ DATA + 0x1144, root, CODE }) == 0);
    m32_wr(visits, 0);
    call("_twalk", 2, (uint32_t[]){ m32_rd(root), CODE + 0x40 });
    check("twalk visits every node once in order", m32_rd(visits) == 7);
    check("tdelete", call("_tdelete", 3, (uint32_t[]){ DATA + 0x1140, root, CODE }) != 0 &&
                     call("_tfind", 3, (uint32_t[]){ DATA + 0x1140, root, CODE }) == 0);
    m32_wr(visits, 0);
    call("_twalk", 2, (uint32_t[]){ m32_rd(root), CODE + 0x40 });
    check("... and six are left", m32_rd(visits) == 6);
}

/* positional printf arguments, placed in position order (src/m32_stdio.c) */
static void test_positional(void)
{
    put_str(DATA + 0x1200, "two");
    m32_wr(DATA + 0x1210, 7);                       /* %1$d */
    m32_wr(DATA + 0x1214, DATA + 0x1200);           /* %2$s */
    const double pi = 3.25;
    memcpy(m32_h(DATA + 0x1218), &pi, 8);           /* %3$.*1$f */
    uint64_t slots[8] = { 0 };
    int n = m32_printf_slots("%2$s %1$d %3$.*1$f", DATA + 0x1210, slots, 8, "test");
    double d;
    memcpy(&d, &slots[2], 8);
    check("positional printf arguments in position order", n == 3 && slots[0] == 7 &&
                                                           !strcmp((const char *)(uintptr_t)slots[1], "two") && d == 3.25);
    check("positional and sequential arguments mixed are refused", m32_printf_slots("%1$d %d", DATA + 0x1210, slots, 8, "test") == -1);
    check("a position no conversion takes is refused", m32_printf_slots("%2$d", DATA + 0x1210, slots, 8, "test") == -1);
}

/* NSCreateMapTable with Foundation's own callbacks, as a guest that copied NSNonOwnedPointerMapKeyCallBacks has
 * them: handles of host functions (src/m32_objc.c) */
static void test_map_table(void)
{
    if (!dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY | RTLD_GLOBAL))
        return;
    const uint64_t *keycb = (const uint64_t *)dlsym(RTLD_DEFAULT, "NSNonOwnedPointerMapKeyCallBacks");
    const uint64_t *valcb = (const uint64_t *)dlsym(RTLD_DEFAULT, "NSNonOwnedPointerMapValueCallBacks");
    void (*insert)(void *, const void *, const void *) = (void (*)(void *, const void *, const void *))dlsym(RTLD_DEFAULT, "NSMapInsert");
    void *(*get)(void *, const void *) = (void *(*)(void *, const void *))dlsym(RTLD_DEFAULT, "NSMapGet");
    if (!keycb || !valcb || !insert || !get)
        return;
    uint32_t a[10];
    for (int i = 0; i < 5; i++)
        a[i] = keycb[i] ? m32_handle((void *)(uintptr_t)keycb[i]) : 0;
    a[5] = 0;
    for (int i = 0; i < 3; i++)
        a[6 + i] = valcb[i] ? m32_handle((void *)(uintptr_t)valcb[i]) : 0;
    a[9] = 4;
    uint32_t t = call("_NSCreateMapTable", 10, a);
    check("NSCreateMapTable with callbacks by value", t != 0);
    if (!t)
        return;
    insert(m32_host(t), m32_h(DATA + 0x1300), m32_h(DATA + 0x1304));
    check("... makes a table that keeps what goes in", get(m32_host(t), m32_h(DATA + 0x1300)) == m32_h(DATA + 0x1304));
}

/* GWorlds: their pixels and PixMap in guest memory, pens, fills, clipping, the origin and CopyBits */
static void test_quickdraw(void)
{
    uint32_t out = DATA + 0xb00, r = DATA + 0xb10, rgb = DATA + 0xb20;
    uint32_t a[6] = { out, 32, qd_rect(r, 0, 0, 32, 64), 0, 0, 0 };
    check("NewGWorld 32 bits", call("_NewGWorld", 6, a) == 0 && m32_rd(out) != 0);
    uint32_t gw = m32_rd(out);
    uint32_t s[2] = { gw, 0 };
    call("_SetGWorld", 2, s);
    uint32_t g[2] = { DATA + 0xb30, DATA + 0xb34 };
    call("_GetGWorld", 2, g);
    check("GetGWorld answers the GWorld SetGWorld set", m32_rd(DATA + 0xb30) == gw);
    uint32_t w[1] = { gw };
    uint32_t pmh = call("_GetGWorldPixMap", 1, w), pm[1] = { pmh };
    check("LockPixels", (call("_LockPixels", 1, pm) & 0xff) == 1);
    check("GetPixBaseAddr, GetPixRowBytes and GetPixDepth", call("_GetPixBaseAddr", 1, pm) != 0 &&
                                                              call("_GetPixRowBytes", 1, pm) >= 256 &&
                                                              (call("_GetPixDepth", 1, pm) & 0xffff) == 32);
    uint32_t pmp = m32_rd(pmh);
    int16_t rowbytes, bounds[4];
    memcpy(&rowbytes, m32_h(pmp + 4), 2);
    memcpy(bounds, m32_h(pmp + 6), 8);
    check("the PixMap is marked one and bounds the GWorld", (rowbytes & 0x8000) && bounds[0] == 0 && bounds[1] == 0 &&
                                                            bounds[2] == 32 && bounds[3] == 64);
    uint32_t whole[1] = { qd_rect(r, 0, 0, 32, 64) };
    call("_EraseRect", 1, whole);
    check("EraseRect paints the background, white", qd_pixel(pmh, 5, 5) == 0xffffff);
    qd_rgb(rgb, 0xffff, 0, 0);
    uint32_t c[1] = { rgb };
    call("_RGBForeColor", 1, c);
    uint32_t m[2] = { 0, 10 }, l[2] = { 63, 10 };
    call("_MoveTo", 2, m);
    call("_LineTo", 2, l);
    check("LineTo draws a one-pixel line below and right of the pen", qd_pixel(pmh, 20, 10) == 0xff0000 &&
                                                                     qd_pixel(pmh, 20, 11) == 0xffffff &&
                                                                     qd_pixel(pmh, 20, 9) == 0xffffff);
    uint32_t pen[1] = { DATA + 0xb40 };
    call("_GetPen", 1, pen);
    int16_t pv[2];
    memcpy(pv, m32_h(DATA + 0xb40), 4);
    check("the pen ends where the line does", pv[0] == 10 && pv[1] == 63);
    qd_rgb(rgb, 0, 0, 0xffff);
    call("_RGBForeColor", 1, c);
    uint32_t pr[1] = { qd_rect(r, 20, 40, 30, 50) };
    call("_PaintRect", 1, pr);
    check("PaintRect fills its rectangle and not its right or bottom edge",
          qd_pixel(pmh, 45, 25) == 0x0000ff && qd_pixel(pmh, 40, 20) == 0x0000ff && qd_pixel(pmh, 39, 25) == 0xffffff &&
          qd_pixel(pmh, 50, 25) == 0xffffff && qd_pixel(pmh, 45, 30) == 0xffffff);
    uint32_t o[2] = { 100, 200 };
    call("_SetOrigin", 2, o);
    uint32_t b[2] = { gw, DATA + 0xb50 };
    call("_GetPortBounds", 2, b);
    int16_t pb[4];
    memcpy(pb, m32_h(DATA + 0xb50), 8);
    check("SetOrigin moves the port's bounds", pb[0] == 200 && pb[1] == 100 && pb[2] == 232 && pb[3] == 164);
    qd_rgb(rgb, 0, 0xffff, 0);
    call("_RGBForeColor", 1, c);
    uint32_t po[1] = { qd_rect(r, 201, 101, 203, 103) };
    call("_PaintRect", 1, po);
    check("drawing after SetOrigin lands at the pixels the local coordinates name", qd_pixel(pmh, 1, 1) == 0x00ff00 &&
                                                                                   qd_pixel(pmh, 3, 3) == 0xffffff);
    uint32_t z[2] = { 0, 0 };
    call("_SetOrigin", 2, z);
    uint32_t left[1] = { qd_rect(r, 0, 0, 32, 32) };
    call("_ClipRect", 1, left);
    qd_rgb(rgb, 0, 0, 0);
    call("_RGBForeColor", 1, c);
    uint32_t low[1] = { qd_rect(r, 0, 0, 8, 64) };
    call("_PaintRect", 1, low);
    check("the clip region keeps drawing inside it", qd_pixel(pmh, 10, 5) == 0x000000 && qd_pixel(pmh, 40, 5) == 0xffffff);
    uint32_t wide[1] = { qd_rect(r, -32767, -32767, 32767, 32767) };
    call("_ClipRect", 1, wide);
    uint32_t a2[6] = { out, 32, qd_rect(r, 0, 0, 32, 64), 0, 0, 0 };
    call("_NewGWorld", 6, a2);
    uint32_t gw2 = m32_rd(out), w2[1] = { gw2 };
    uint32_t pmh2 = call("_GetGWorldPixMap", 1, w2);
    uint32_t pw[1] = { gw }, pw2[1] = { gw2 };
    uint32_t cb[6] = { call("_GetPortBitMapForCopyBits", 1, pw), call("_GetPortBitMapForCopyBits", 1, pw2),
                       qd_rect(DATA + 0xb60, 20, 40, 30, 50), qd_rect(DATA + 0xb68, 0, 0, 10, 10), 0, 0 };
    call("_CopyBits", 6, cb);
    check("CopyBits copies one GWorld's rectangle into another's", qd_pixel(pmh2, 5, 5) == 0x0000ff &&
                                                                   qd_pixel(pmh2, 9, 9) == 0x0000ff);
    uint32_t q[2] = { gw, DATA + 0xb70 };
    check("QDBeginCGContext hands out the port's context", call("_QDBeginCGContext", 2, q) == 0 && m32_rd(DATA + 0xb70) != 0);
    check("QDEndCGContext takes it back", call("_QDEndCGContext", 2, q) == 0);
    call("_DisposeGWorld", 1, w2);
    check("a disposed GWorld is no port", call("_IsValidPort", 1, w2) == 0 && call("_IsValidPort", 1, w) == 1);
    uint32_t a16[6] = { out, 16, qd_rect(r, 0, 0, 8, 8), 0, 0, 0 };
    check("NewGWorld 16 bits", call("_NewGWorld", 6, a16) == 0);
    uint32_t g16[1] = { m32_rd(out) };
    call("_SetGWorld", 2, (uint32_t[]){ g16[0], 0 });
    qd_rgb(rgb, 0xffff, 0, 0);
    call("_RGBForeColor", 1, c);
    call("_PaintRect", 1, (uint32_t[]){ qd_rect(r, 0, 0, 8, 8) });
    uint32_t pm16[1] = { call("_GetGWorldPixMap", 1, g16) };
    const uint8_t *px16 = (const uint8_t *)m32_h(call("_GetPixBaseAddr", 1, pm16));
    check("16-bit pixels are big-endian x555", (call("_GetPixDepth", 1, pm16) & 0xffff) == 16 && px16[0] == 0x7c &&
                                               px16[1] == 0x00);
    uint32_t a8[6] = { out, 8, qd_rect(r, 0, 0, 8, 8), 0, 0, 0 };
    check("an 8-bit GWorld is refused with cDepthErr", (int16_t)call("_NewGWorld", 6, a8) == -157 && m32_rd(out) == 0);
    call("_SetGWorld", 2, s);
}

/* AGL on a window's QuickDraw port: the context draws into the window (src/m32_agl.c) */
static void test_agl(uint32_t port)
{
    const uint32_t attrs[] = { 4 /* AGL_RGBA */, 5 /* AGL_DOUBLEBUFFER */, 0 };
    memcpy(m32_h(DATA + 0xc00), attrs, sizeof attrs);
    uint32_t pf = call("_aglChoosePixelFormat", 3, (uint32_t[]){ 0, 0, DATA + 0xc00 });
    check("aglChoosePixelFormat", pf != 0);
    uint32_t ctx = call("_aglCreateContext", 2, (uint32_t[]){ pf, 0 });
    check("aglCreateContext", ctx != 0);
    check("aglSetDrawable on a window's port", (call("_aglSetDrawable", 2, (uint32_t[]){ ctx, port }) & 0xff) == 1);
    check("aglGetDrawable answers the port", call("_aglGetDrawable", 1, (uint32_t[]){ ctx }) == port);
    check("aglSetCurrentContext", (call("_aglSetCurrentContext", 1, (uint32_t[]){ ctx }) & 0xff) == 1 &&
                                  call("_aglGetCurrentContext", 0, NULL) == ctx);
    void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))dlsym(RTLD_DEFAULT, "glClearColor");
    void (*clear)(uint32_t) = (void (*)(uint32_t))dlsym(RTLD_DEFAULT, "glClear");
    void (*read)(int, int, int, int, uint32_t, uint32_t, void *) =
        (void (*)(int, int, int, int, uint32_t, uint32_t, void *))dlsym(RTLD_DEFAULT, "glReadPixels");
    void (*viewport)(uint32_t, int *) = (void (*)(uint32_t, int *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
    int vp[4] = { 0, 0, 0, 0 };
    uint8_t px[4] = { 0, 0, 0, 0 };
    if (clear_color && clear && read && viewport) {
        viewport(0x0BA2 /* GL_VIEWPORT */, vp);
        clear_color(0, 1, 0, 1);
        clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
        read(5, 5, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    }
    check("the context draws into the window's content", vp[2] == 400 && vp[3] == 200 && px[0] == 0 && px[1] == 0xff);
    const uint32_t rect[4] = { 0, 0, 100, 50 };
    memcpy(m32_h(DATA + 0xc20), rect, sizeof rect);
    check("aglSetInteger(AGL_BUFFER_RECT)", (call("_aglSetInteger", 3, (uint32_t[]){ ctx, 202, DATA + 0xc20 }) & 0xff) == 1);
    m32_wr(DATA + 0xc30, 0xffffffff);
    check("aglGetInteger(AGL_SWAP_INTERVAL)", (call("_aglGetInteger", 3, (uint32_t[]){ ctx, 222, DATA + 0xc30 }) & 0xff) == 1 &&
                                              m32_rd(DATA + 0xc30) != 0xffffffff);
    check("aglGetCGLContext", (call("_aglGetCGLContext", 2, (uint32_t[]){ ctx, DATA + 0xc40 }) & 0xff) == 1 &&
                              m32_rd(DATA + 0xc40) != 0);
    call("_aglSwapBuffers", 1, (uint32_t[]){ ctx });
    check("aglSetDrawable(NULL) detaches", (call("_aglSetDrawable", 2, (uint32_t[]){ ctx, 0 }) & 0xff) == 1 &&
                                           call("_aglGetDrawable", 1, (uint32_t[]){ ctx }) == 0);
    call("_aglSetCurrentContext", 1, (uint32_t[]){ 0 });
    check("aglDestroyContext", (call("_aglDestroyContext", 1, (uint32_t[]){ ctx }) & 0xff) == 1);
    call("_aglDestroyPixelFormat", 1, (uint32_t[]){ pf });
    check("aglGetError", call("_aglGetError", 0, NULL) == 0);
}

#define FOUR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* Carbon event parameters cross in their i386 layouts; UPPs are their functions; EventRecords are 16 bytes */
static void test_event_params(uint32_t w, uint32_t port)
{
    check("New...UPP hands back its function", call("_NewEventHandlerUPP", 1, (uint32_t[]){ 0x1234 }) == 0x1234 &&
                                               call("_NewControlUserPaneDrawUPP", 1, (uint32_t[]){ 0x5678 }) == 0x5678);
    check("Dispose...UPP does nothing", call("_DisposeNavEventUPP", 1, (uint32_t[]){ 0x1234 }) == 0 &&
                                        m32_special("_NewWindowsBitmap") == NULL);
    void *hi = dlopen(CARBON, RTLD_LAZY);
    int32_t (*create)(void *, uint32_t, uint32_t, double, uint32_t, void **) =
        (int32_t (*)(void *, uint32_t, uint32_t, double, uint32_t, void **))dlsym(hi, "CreateEvent");
    void *(*root)(void *) = (void *(*)(void *))dlsym(hi, "HIViewGetRoot");
    void *ev = NULL;
    create(NULL, FOUR('c','n','t','l'), 4 /* kEventControlDraw */, 0, 0, &ev);
    uint32_t gev = m32_handle(ev);
    /* a ControlRef: a 4-byte handle in, the same out */
    void *view = root(m32_host(w));
    m32_wr(DATA + 0xd00, m32_handle(view));
    check("SetEventParameter(typeControlRef)", call("_SetEventParameter", 5, (uint32_t[]){ gev, FOUR('-','-','-','-'),
                                                FOUR('c','t','r','l'), 4, DATA + 0xd00 }) == 0);
    m32_wr(DATA + 0xd10, 0);
    uint32_t get[7] = { gev, FOUR('-','-','-','-'), FOUR('c','t','r','l'), DATA + 0xd20, 4, DATA + 0xd24, DATA + 0xd10 };
    check("GetEventParameter(typeControlRef) gives the handle back", call("_GetEventParameter", 7, get) == 0 &&
                                                                      m32_rd(DATA + 0xd10) == m32_handle(view) &&
                                                                      m32_rd(DATA + 0xd24) == 4);
    uint32_t wild[7] = { gev, FOUR('-','-','-','-'), FOUR('*','*','*','*'), DATA + 0xd20, 4, DATA + 0xd24, DATA + 0xd10 };
    check("... and with typeWildCard", call("_GetEventParameter", 7, wild) == 0 && m32_rd(DATA + 0xd10) == m32_handle(view) &&
                                       m32_rd(DATA + 0xd20) == FOUR('c','t','r','l'));
    /* the draw event's GrafPtr, which 64-bit Carbon has none of: the control's window's port */
    uint32_t gp[7] = { gev, FOUR('g','r','f','p'), FOUR('g','r','f','p'), 0, 4, 0, DATA + 0xd30 };
    check("GetEventParameter(typeGrafPtr) is the window's QuickDraw port", call("_GetEventParameter", 7, gp) == 0 &&
                                                                         m32_rd(DATA + 0xd30) == port);
    /* an HIPoint: two floats on i386 */
    const float pt[2] = { 12.5f, -3.25f };
    memcpy(m32_h(DATA + 0xd40), pt, sizeof pt);
    call("_SetEventParameter", 5, (uint32_t[]){ gev, FOUR('m','l','o','c'), FOUR('h','i','p','t'), 8, DATA + 0xd40 });
    float back[2] = { 0, 0 };
    int32_t (*host_get)(void *, uint32_t, uint32_t, uint32_t *, unsigned long, unsigned long *, void *) =
        (int32_t (*)(void *, uint32_t, uint32_t, uint32_t *, unsigned long, unsigned long *, void *))dlsym(hi, "GetEventParameter");
    double hd[2] = { 0, 0 };
    host_get(ev, FOUR('m','l','o','c'), FOUR('h','i','p','t'), NULL, sizeof hd, NULL, hd);
    uint32_t gpt[7] = { gev, FOUR('m','l','o','c'), FOUR('h','i','p','t'), 0, 8, DATA + 0xd24, DATA + 0xd50 };
    call("_GetEventParameter", 7, gpt);
    memcpy(back, m32_h(DATA + 0xd50), sizeof back);
    check("an HIPoint crosses as doubles and comes back as floats", hd[0] == 12.5 && hd[1] == -3.25 && back[0] == 12.5f &&
                                                                    back[1] == -3.25f && m32_rd(DATA + 0xd24) == 8);
    /* an HICommand: its MenuRef at 8, its index at 12 */
    const uint32_t cmd[4] = { 1, FOUR('q','u','i','t'), m32_handle(view), 7 };
    memcpy(m32_h(DATA + 0xd60), cmd, sizeof cmd);
    call("_SetEventParameter", 5, (uint32_t[]){ gev, FOUR('-','-','-','-'), FOUR('h','c','m','d'), 16, DATA + 0xd60 });
    memset(m32_h(DATA + 0xd70), 0, 16);
    call("_GetEventParameter", 7, (uint32_t[]){ gev, FOUR('-','-','-','-'), FOUR('h','c','m','d'), 0, 16, 0, DATA + 0xd70 });
    check("an HICommand crosses", !memcmp(m32_h(DATA + 0xd60), m32_h(DATA + 0xd70), 14));
    /* an EventRecord from a key event: what, then a 4-byte message at 2 */
    void *down = NULL;
    create(NULL, FOUR('k','e','y','b'), 1 /* kEventRawKeyDown */, 0, 0, &down);
    int32_t (*host_set)(void *, uint32_t, uint32_t, unsigned long, const void *) =
        (int32_t (*)(void *, uint32_t, uint32_t, unsigned long, const void *))dlsym(hi, "SetEventParameter");
    const char ch = 'a';
    const uint32_t zero = 0;
    host_set(down, FOUR('k','c','h','r'), FOUR('T','E','X','T'), 1, &ch);
    host_set(down, FOUR('k','c','o','d'), FOUR('m','a','g','n'), 4, &zero);
    host_set(down, FOUR('k','m','o','d'), FOUR('m','a','g','n'), 4, &zero);
    memset(m32_h(DATA + 0xd80), 0xee, 16);
    check("ConvertEventRefToEventRecord", (call("_ConvertEventRefToEventRecord", 2, (uint32_t[]){ m32_handle(down), DATA + 0xd80 }) & 0xff) == 1);
    uint16_t what;
    uint32_t message;
    memcpy(&what, m32_h(DATA + 0xd80), 2);
    memcpy(&message, m32_h(DATA + 0xd82), 4);
    check("... in i386's 16 bytes: keyDown, 'a'", what == 3 /* keyDown */ && message == 'a');
    void (*release)(const void *) = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
    release(ev);
    release(down);

    /* GetAvailableWindowPositioningBounds: what HIToolbox gives for the main display */
    int32_t (*avail)(uint32_t, int, CGRect *) = (int32_t (*)(uint32_t, int, CGRect *))dlsym(hi, "HIWindowGetAvailablePositioningBounds");
    uint32_t (*main_id)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "CGMainDisplayID");
    CGRect ab;
    avail(main_id(), 1, &ab);
    int16_t ar[4] = { 0, 0, 0, 0 };
    check("GetAvailableWindowPositioningBounds", call("_GetAvailableWindowPositioningBounds", 2,
                                                      (uint32_t[]){ call("_GetMainDevice", 0, NULL), DATA + 0xd90 }) == 0);
    memcpy(ar, m32_h(DATA + 0xd90), 8);
    check("... the display less its menu bar and Dock", ar[0] == (int16_t)ab.origin.y && ar[1] == (int16_t)ab.origin.x &&
                                                        ar[2] == (int16_t)(ab.origin.y + ab.size.height) &&
                                                        ar[3] == (int16_t)(ab.origin.x + ab.size.width));
}

/* CGFloat arrays: floats on i386, doubles on the host (src/m32_cross.c, g_float_arrays) */
static void test_float_arrays(void)
{
    const char *CG = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    void *(*rgb)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB");
    const double *(*components)(void *) = (const double *(*)(void *))dlsym(RTLD_DEFAULT, "CGColorGetComponents");
    uint32_t space = m32_handle(rgb());
    const float c[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    memcpy(m32_h(DATA + 0xe00), c, sizeof c);
    uint32_t color = cross(CG, "_CGColorCreate", 2, (uint32_t[]){ space, DATA + 0xe00 });
    const double *hc = color ? components(m32_host(color)) : NULL;
    check("CGColorCreate takes its components as floats", hc && hc[0] == 0.25 && hc[1] == 0.5 && hc[2] == 0.75 && hc[3] == 1.0);
    /* two rectangles filled into a bitmap: both, not the first alone */
    uint32_t a[6] = { DATA + 0xe10, 32, qd_rect(DATA + 0xe20, 0, 0, 16, 64), 0, 0, 0 };
    call("_NewGWorld", 6, a);
    uint32_t gw = m32_rd(DATA + 0xe10), q[2] = { gw, DATA + 0xe30 };
    call("_SetGWorld", 2, (uint32_t[]){ gw, 0 });
    call("_EraseRect", 1, (uint32_t[]){ qd_rect(DATA + 0xe20, 0, 0, 16, 64) });
    call("_QDBeginCGContext", 2, q);
    uint32_t ctx = m32_rd(DATA + 0xe30);
    const float rects[8] = { 0, 0, 4, 4, 40, 0, 4, 4 };   /* CoreGraphics' coordinates: from the bottom left */
    memcpy(m32_h(DATA + 0xe40), rects, sizeof rects);
    cross(CG, "_CGContextSetRGBFillColor", 5, (uint32_t[]){ ctx, 0x3f800000 /* 1.0f */, 0, 0, 0x3f800000 });
    cross(CG, "_CGContextFillRects", 3, (uint32_t[]){ ctx, DATA + 0xe40, 2 });
    call("_QDEndCGContext", 2, q);
    uint32_t pmh = call("_GetGWorldPixMap", 1, (uint32_t[]){ gw });
    check("CGContextFillRects fills every rectangle", qd_pixel(pmh, 1, 14) == 0xff0000 && qd_pixel(pmh, 41, 14) == 0xff0000 &&
                                                      qd_pixel(pmh, 20, 14) == 0xffffff);
    call("_DisposeGWorld", 1, (uint32_t[]){ gw });
}

static uint32_t pstr(uint32_t g, const char *s)
{
    size_t n = strlen(s);
    *(uint8_t *)m32_h(g) = (uint8_t)n;
    memcpy(m32_h(g + 1), s, n);
    return g;
}

/* QuickDraw's and the Font Manager's font families, on CoreText (src/m32_atsu.c) */
static void test_fonts(void)
{
    uint32_t id = DATA + 0xf00;
    call("_GetFNum", 2, (uint32_t[]){ pstr(DATA + 0xf10, "Helvetica"), id });
    check("GetFNum: Helvetica is classic 21", *(int16_t *)m32_h(id) == 21);
    call("_GetFNum", 2, (uint32_t[]){ pstr(DATA + 0xf10, "geneva"), id });
    check("... Geneva 3, whatever its case", *(int16_t *)m32_h(id) == 3);
    *(int16_t *)m32_h(id) = 99;
    call("_GetFNum", 2, (uint32_t[]){ pstr(DATA + 0xf10, "No Such Family Q9"), id });
    check("... a family the system lacks, 0", *(int16_t *)m32_h(id) == 0);
    int16_t menlo = (int16_t)call("_FMGetFontFamilyFromName", 1, (uint32_t[]){ pstr(DATA + 0xf10, "Menlo") });
    check("FMGetFontFamilyFromName gives another family an ID from 1024", menlo >= 1024 &&
          (int16_t)call("_FMGetFontFamilyFromName", 1, (uint32_t[]){ pstr(DATA + 0xf10, "Menlo") }) == menlo);
    check("FMGetFontFamilyFromName: kInvalidFontFamily", (int16_t)call("_FMGetFontFamilyFromName", 1,
                                                                       (uint32_t[]){ pstr(DATA + 0xf10, "No Such Family Q9") }) == -1);
    call("_FMGetFontFamilyName", 2, (uint32_t[]){ (uint32_t)(uint16_t)menlo, DATA + 0xf20 });
    check("FMGetFontFamilyName", !memcmp(m32_h(DATA + 0xf20), "\x05Menlo", 6));
    check("FMGetFontFamilyName of no family", (int16_t)call("_FMGetFontFamilyName", 2, (uint32_t[]){ 777, DATA + 0xf20 }) == -985);
    uint32_t inst[4] = { 21, 1 /* bold */, DATA + 0xf30, DATA + 0xf34 };
    check("FMGetFontFromFontFamilyInstance: Helvetica Bold", call("_FMGetFontFromFontFamilyInstance", 4, inst) == 0 &&
                                                            m32_rd(DATA + 0xf30) >= 0x10000 && (*(int16_t *)m32_h(DATA + 0xf34) & 1));
    uint32_t back[3] = { m32_rd(DATA + 0xf30), DATA + 0xf40, DATA + 0xf44 };
    check("FMGetFontFamilyInstanceFromFont goes back", call("_FMGetFontFamilyInstanceFromFont", 3, back) == 0 &&
                                                       *(int16_t *)m32_h(DATA + 0xf40) == 21);
    check("GetAppFont and GetSysFont", call("_GetAppFont", 0, NULL) == 1 && call("_GetSysFont", 0, NULL) == 0);
    uint32_t ats = call("_FMGetATSFontFamilyRefFromFontFamily", 1, (uint32_t[]){ (uint32_t)(uint16_t)menlo });
    check("an ATS family and back", ats != 0 && (int16_t)call("_FMGetFontFamilyFromATSFontFamilyRef", 1, (uint32_t[]){ ats }) == menlo);
    uint32_t theme[5] = { 1 /* kThemeSmallSystemFont */, 0, DATA + 0xf50, DATA + 0xf60, DATA + 0xf64 };
    check("GetThemeFont: the small system font", call("_GetThemeFont", 5, theme) == 0 && *(uint8_t *)m32_h(DATA + 0xf50) > 0 &&
                                                 *(int16_t *)m32_h(DATA + 0xf60) == 11);
    call("_GetFNum", 2, (uint32_t[]){ DATA + 0xf50, id });
    check("... whose name GetFNum knows as the system font", *(int16_t *)m32_h(id) == 0);
    /* ATSUI: a copied style, and the unjustified bounds of a line laid out in it */
    uint32_t st = DATA + 0xf70, cp = DATA + 0xf74;
    call("_ATSUCreateStyle", 1, (uint32_t[]){ st });
    check("ATSUCreateAndCopyStyle", call("_ATSUCreateAndCopyStyle", 2, (uint32_t[]){ m32_rd(st), cp }) == 0 && m32_rd(cp) != 0 &&
                                    m32_rd(cp) != m32_rd(st));
    const uint16_t hello[5] = { 'H', 'e', 'l', 'l', 'o' };
    memcpy(m32_h(DATA + 0xf80), hello, sizeof hello);
    m32_wr(DATA + 0xf90, 5);
    m32_wr(DATA + 0xf94, m32_rd(cp));
    uint32_t mk[8] = { DATA + 0xf80, 0, 5, 5, 1, DATA + 0xf90, DATA + 0xf94, DATA + 0xf98 };
    call("_ATSUCreateTextLayoutWithTextPtr", 8, mk);
    uint32_t ub[7] = { m32_rd(DATA + 0xf98), 0, 5, DATA + 0xfa0, DATA + 0xfa4, DATA + 0xfa8, DATA + 0xfac };
    check("ATSUGetUnjustifiedBounds", call("_ATSUGetUnjustifiedBounds", 7, ub) == 0 && (int32_t)m32_rd(DATA + 0xfa4) > 10 << 16 &&
                                      (int32_t)m32_rd(DATA + 0xfa8) > 4 << 16);
    /* the Text Encoding Converter, its longs narrowed: Unicode to MacRoman */
    void *create = dlsym(RTLD_DEFAULT, "CreateUnicodeToTextInfoByEncoding");
    void *info = NULL;
    if (create)
        ((int32_t (*)(uint32_t, void **))create)(0 /* kTextEncodingMacRoman */, &info);
    const uint16_t cafe[4] = { 'c', 'a', 'f', 0xe9 };
    memcpy(m32_h(DATA + 0xfb0), cafe, sizeof cafe);
    uint32_t tec[12] = { m32_handle(info), 8, DATA + 0xfb0, 0, 0, 0, 0, 0, 16, DATA + 0xfc0, DATA + 0xfc4, DATA + 0xfd0 };
    check("ConvertFromUnicodeToText", info && call("_ConvertFromUnicodeToText", 12, tec) == 0 && m32_rd(DATA + 0xfc0) == 8 &&
                                      m32_rd(DATA + 0xfc4) == 4 && !memcmp(m32_h(DATA + 0xfd0), "caf\x8e", 4));
}

/* what Firefox 3.6 needed of fonts: tables, platform fonts, glyph info and bounds, and Lucida Grande as the system font */
static void test_font_tables(void)
{
    uint32_t lucida = DATA + 0x1400;
    void *(*cfs)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    uint32_t font = call("_ATSFontFindFromPostScriptName", 2, (uint32_t[]){ m32_handle(cfs(NULL, "LucidaGrande", 0x08000100)), 0 });
    check("ATSFontFindFromPostScriptName(LucidaGrande)", font >= 0x10000);
    uint32_t t[6] = { font, FOUR('h','e','a','d'), 0, 0, 0, DATA + 0x1410 };
    check("ATSFontGetTable sizes a table", call("_ATSFontGetTable", 6, t) == 0 && m32_rd(DATA + 0x1410) >= 54);
    uint32_t r[6] = { font, FOUR('h','e','a','d'), 12, 4, DATA + 0x1420, DATA + 0x1410 };
    uint8_t magic[4];
    check("... and reads it from an offset", call("_ATSFontGetTable", 6, r) == 0 && m32_rd(DATA + 0x1410) == 4);
    memcpy(magic, m32_h(DATA + 0x1420), 4);
    check("... 'head''s magic number", magic[0] == 0x5f && magic[1] == 0x0f && magic[2] == 0x3c && magic[3] == 0xf5);
    m32_wr(lucida, font);
    uint32_t cg = call("_CGFontCreateWithPlatformFont", 1, (uint32_t[]){ lucida });
    void *(*psname)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT, "CGFontCopyPostScriptName");
    int (*get)(void *, char *, long, uint32_t) = (int (*)(void *, char *, long, uint32_t))dlsym(RTLD_DEFAULT, "CFStringGetCString");
    char name[64] = "";
    if (cg)
        get(psname(m32_host(cg)), name, sizeof name, 0x08000100);
    check("CGFontCreateWithPlatformFont makes the font without ATS", cg && !strcmp(name, "LucidaGrande"));
    /* glyph info and bounds of "Hi" in a style of that font */
    uint32_t st = DATA + 0x1430;
    call("_ATSUCreateStyle", 1, (uint32_t[]){ st });
    check("ATSUSetFontFeatures is accepted", call("_ATSUSetFontFeatures", 4, (uint32_t[]){ m32_rd(st), 0, 0, 0 }) == 0);
    const uint16_t hi[2] = { 'H', 'i' };
    memcpy(m32_h(DATA + 0x1440), hi, sizeof hi);
    m32_wr(DATA + 0x1450, 2);
    m32_wr(DATA + 0x1454, m32_rd(st));
    call("_ATSUCreateTextLayoutWithTextPtr", 8, (uint32_t[]){ DATA + 0x1440, 0, 2, 2, 1, DATA + 0x1450, DATA + 0x1454, DATA + 0x1458 });
    uint32_t layout = m32_rd(DATA + 0x1458);
    m32_wr(DATA + 0x1460, 0);
    call("_ATSUGetGlyphInfo", 5, (uint32_t[]){ layout, 0, 2, DATA + 0x1460, 0 });
    uint32_t need = m32_rd(DATA + 0x1460);
    check("ATSUGetGlyphInfo sizes its array", need == 8 + 2 * 28);
    uint32_t arr = DATA + 0x1500;
    m32_wr(DATA + 0x1460, need);
    check("ATSUGetGlyphInfo", call("_ATSUGetGlyphInfo", 5, (uint32_t[]){ layout, 0, 2, DATA + 0x1460, arr }) == 0);
    float x0, x1;
    memcpy(&x0, m32_h(arr + 8 + 20), 4);
    memcpy(&x1, m32_h(arr + 8 + 28 + 20), 4);
    check("... two glyphs, their characters, style and advancing positions",
          m32_rd(arr) == layout && m32_rd(arr + 4) == 2 && m32_rd(arr + 8 + 8) == 0 && m32_rd(arr + 8 + 28 + 8) == 1 &&
          m32_rd(arr + 8 + 12) == m32_rd(st) && x0 == 0 && x1 > 3);
    uint32_t gb[9] = { layout, 10u << 16, 50u << 16, 0, 2, 0, 1, DATA + 0x1600, DATA + 0x1640 };
    check("ATSUGetGlyphBounds", call("_ATSUGetGlyphBounds", 9, gb) == 0 && m32_rd(DATA + 0x1640) == 1);
    int32_t trap[8];
    memcpy(trap, m32_h(DATA + 0x1600), sizeof trap);
    check("... a box about the baseline at (10, 50)", trap[0] == 10 << 16 && trap[1] < 50 << 16 && trap[5] > 50 << 16 &&
                                                     trap[2] > trap[0]);
    /* +[NSFont systemFontOfSize:] from i386 code is Lucida Grande */
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL))
        return;
    uint32_t cls = m32_class_guest(objc_getClass("NSFont")), sel = m32_sel_guest(sel_registerName("systemFontOfSize:"));
    const float size = 12;
    uint32_t sizew;
    memcpy(&sizew, &size, 4);
    uint32_t f = call("_objc_msgSend", 3, (uint32_t[]){ cls, sel, sizew });
    void *(*send)(void *, void *) = (void *(*)(void *, void *))objc_msgSend;
    double (*send_d)(void *, void *) = (double (*)(void *, void *))objc_msgSend;
    void *hf = f ? m32_objc_to_host(f) : NULL;
    name[0] = 0;
    if (hf)
        get(send(hf, sel_registerName("fontName")), name, sizeof name, 0x08000100);
    check("systemFontOfSize: is Lucida Grande for i386 code", !strcmp(name, "LucidaGrande") &&
                                                              send_d(hf, sel_registerName("pointSize")) == 12);
}

/* code built against the 10.4 SDK writes stdout as &__sF[1] */
static void test_sF(void)
{
    uint32_t sf = m32_special_data("___sF", NULL);
    extern FILE *__stdinp, *__stdoutp, *__stderrp;
    check("__sF is three FILEs standing for the host's streams", sf && m32_host(sf) == (void *)__stdinp &&
                                                                 m32_host(sf + 88) == (void *)__stdoutp &&
                                                                 m32_host(sf + 176) == (void *)__stderrp &&
                                                                 *(int16_t *)m32_h(sf + 88 + 14) == 1);
}

/* menus and controls arm64 HIToolbox dropped (src/m32_controls.c) */
/* i386 functions the CoreGraphics callback tests hand over, assembled with clang -target i386 (at CODE + 0x200):
 *   0x00 applier(info, element): info[0]++, info[1] += element->type, info[2 + n] = element->points[0].x
 *   0x1d evaluate(info, in, out): out[0] = in[0] * 0.5f
 *   0x3b counter(info, ...): (*info)++
 *   0x42 size(void): {1.5f, 2.5f} in EAX:EDX
 *   0x4d rect(sret): {1, 2, 3, 4} through the hidden pointer, popped with ret $4 */
static const uint8_t k_cg_code[] = {
    0x8b, 0x44, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08, 0x8b, 0x10, 0xff, 0x00, 0x53, 0x8b, 0x19, 0x01, 0x58, 0x04, 0x8b,
    0x49, 0x04, 0x8b, 0x19, 0x89, 0x5c, 0x90, 0x08, 0x5b, 0xc3, 0x8b, 0x44, 0x24, 0x08, 0x8b, 0x4c, 0x24, 0x0c, 0xf3,
    0x0f, 0x10, 0x00, 0xba, 0x00, 0x00, 0x00, 0x3f, 0x66, 0x0f, 0x6e, 0xca, 0xf3, 0x0f, 0x59, 0xc1, 0xf3, 0x0f, 0x11,
    0x01, 0xc3, 0x8b, 0x44, 0x24, 0x04, 0xff, 0x00, 0xc3, 0xb8, 0x00, 0x00, 0xc0, 0x3f, 0xba, 0x00, 0x00, 0x20, 0x40,
    0xc3, 0x8b, 0x44, 0x24, 0x04, 0xc7, 0x00, 0x00, 0x00, 0x80, 0x3f, 0xc7, 0x40, 0x04, 0x00, 0x00, 0x00, 0x40, 0xc7,
    0x40, 0x08, 0x00, 0x00, 0x40, 0x40, 0xc7, 0x40, 0x0c, 0x00, 0x00, 0x80, 0x40, 0xc2, 0x04, 0x00 };
enum { CG_CODE = CODE + 0x200, APPLIER = CG_CODE, EVALUATE = CG_CODE + 0x1d, COUNTER = CG_CODE + 0x3b,
       SIZE_FN = CG_CODE + 0x42, RECT_FN = CG_CODE + 0x4d };

static uint32_t float_word(float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}

static void *host_fn(const char *name)
{
    void *f = dlsym(RTLD_DEFAULT, name);
    check(name, f != NULL);
    return f;
}

/* CoreGraphics' callbacks, path elements and the private calls cairo uses (src/m32_cg.c, src/m32_atsu.c), and
 * structures a guest callback returns (src/m32_callback.c) */
static void test_cg(void)
{
    memcpy(m32_h(CG_CODE), k_cg_code, sizeof k_cg_code);
    void *(*cfstr)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))host_fn("CFStringCreateWithCString");
    void *(*font_named)(void *) = (void *(*)(void *))host_fn("CGFontCreateWithFontName");
    int (*ascent)(void *) = (int (*)(void *))host_fn("CGFontGetAscent");
    CGRect (*path_box)(void *) = (CGRect (*)(void *))host_fn("CGPathGetBoundingBox");
    void *font = font_named(cfstr(NULL, "Helvetica", 0x08000100));
    check("Helvetica as a CGFont", font != NULL);
    uint32_t f = m32_handle(font);
    /* CGFontGetGlyphsForUnichars: two characters, two glyphs, and glyph 0 for none */
    const uint16_t chars[3] = { 'H', 'i', 0xFFFF };
    memcpy(m32_h(DATA + 0x2000), chars, sizeof chars);
    memset(m32_h(DATA + 0x2010), 0xff, 6);
    call("_CGFontGetGlyphsForUnichars", 4, (uint32_t[]){ f, DATA + 0x2000, DATA + 0x2010, 3 });
    const uint16_t *glyphs = m32_h(DATA + 0x2010);
    check("CGFontGetGlyphsForUnichars", glyphs[0] && glyphs[1] && glyphs[0] != glyphs[1] && glyphs[2] == 0);
    /* CGFontGetHMetrics: a guest pointer to {ascent, descent, leading} in font units */
    uint32_t m = call("_CGFontGetHMetrics", 1, (uint32_t[]){ f });
    check("CGFontGetHMetrics", m && (int32_t)m32_rd(m) == ascent(font) && (int32_t)m32_rd(m + 4) < 0);
    /* CGFontGetGlyphPath: the outline at an em of 1, scaled by the transform's six floats */
    const float twice[6] = { 2, 0, 0, 2, 0, 0 };
    memcpy(m32_h(DATA + 0x2020), twice, sizeof twice);
    uint32_t p1 = call("_CGFontGetGlyphPath", 4, (uint32_t[]){ f, 0, 0, glyphs[0] });
    uint32_t p2 = call("_CGFontGetGlyphPath", 4, (uint32_t[]){ f, DATA + 0x2020, 0, glyphs[0] });
    CGRect b1 = p1 ? path_box(m32_host(p1)) : (CGRect){ { 0, 0 }, { 0, 0 } }, b2 = p2 ? path_box(m32_host(p2)) : (CGRect){ { 0, 0 }, { 0, 0 } };
    check("CGFontGetGlyphPath at an em of 1", b1.size.width > 0.3 && b1.size.width < 1.5 && b1.size.height > 0.3);
    check("CGFontGetGlyphPath through the transform", b2.size.width > 1.9 * b1.size.width && b2.size.width < 2.1 * b1.size.width);
    /* CGPathApply: the guest applier sees each element as {type, points}, its points as floats */
    void *(*mk_path)(void) = (void *(*)(void))host_fn("CGPathCreateMutable");
    void (*move)(void *, const void *, double, double) = (void (*)(void *, const void *, double, double))host_fn("CGPathMoveToPoint");
    void (*line)(void *, const void *, double, double) = (void (*)(void *, const void *, double, double))host_fn("CGPathAddLineToPoint");
    void (*curve)(void *, const void *, double, double, double, double, double, double) =
        (void (*)(void *, const void *, double, double, double, double, double, double))host_fn("CGPathAddCurveToPoint");
    void (*close_path)(void *) = (void (*)(void *))host_fn("CGPathCloseSubpath");
    void *path = mk_path();
    move(path, NULL, 1, 2);
    line(path, NULL, 3, 4);
    curve(path, NULL, 5, 6, 7, 8, 9, 10);
    close_path(path);
    memset(m32_h(DATA + 0x2040), 0, 32);
    call("_CGPathApply", 3, (uint32_t[]){ m32_handle(path), DATA + 0x2040, APPLIER });
    const float *xs = m32_h(DATA + 0x2048);
    check("CGPathApply hands the guest each element", m32_rd(DATA + 0x2040) == 4 && m32_rd(DATA + 0x2044) == 0 + 1 + 3 + 4 &&
                                                       xs[0] == 1 && xs[1] == 3 && xs[2] == 5);
    /* CGFunctionCreate: a gray axial shading 0 to 0.5 across 16 pixels, evaluated by guest code */
    void *(*gray)(void) = (void *(*)(void))host_fn("CGColorSpaceCreateDeviceGray");
    void *(*axial)(void *, CGPoint, CGPoint, void *, bool, bool) = (void *(*)(void *, CGPoint, CGPoint, void *, bool, bool))host_fn("CGShadingCreateAxial");
    void *(*bitmap)(void *, size_t, size_t, size_t, size_t, void *, uint32_t) =
        (void *(*)(void *, size_t, size_t, size_t, size_t, void *, uint32_t))host_fn("CGBitmapContextCreate");
    void (*draw_shading)(void *, void *) = (void (*)(void *, void *))host_fn("CGContextDrawShading");
    const float unit[2] = { 0, 1 };
    memcpy(m32_h(DATA + 0x2080), unit, sizeof unit);
    memcpy(m32_h(DATA + 0x2088), unit, sizeof unit);
    m32_wr(DATA + 0x2090, 0);   /* CGFunctionCallbacks: version, evaluate, releaseInfo */
    m32_wr(DATA + 0x2094, EVALUATE);
    m32_wr(DATA + 0x2098, 0);
    uint32_t fn = call("_CGFunctionCreate", 6, (uint32_t[]){ 0, 1, DATA + 0x2080, 1, DATA + 0x2088, DATA + 0x2090 });
    check("CGFunctionCreate", fn != 0);
    uint8_t px[16] = { 0 };
    void *ctx = bitmap(px, 16, 1, 8, 16, gray(), 0);
    void *sh = fn ? axial(gray(), CGPointMake(0, 0), CGPointMake(16, 0), m32_host(fn), false, false) : NULL;
    if (ctx && sh)
        draw_shading(ctx, sh);
    check("a CGFunction evaluated by guest code", px[0] < 16 && px[15] > 100 && px[15] < 140 && px[8] > px[4]);
    /* CGPatternCreate: the guest's draw callback runs when the pattern fills, its release when the pattern goes */
    void *(*pattern_space)(void *) = (void *(*)(void *))host_fn("CGColorSpaceCreatePattern");
    void (*fill_space)(void *, void *) = (void (*)(void *, void *))host_fn("CGContextSetFillColorSpace");
    void (*fill_pattern)(void *, void *, const double *) = (void (*)(void *, void *, const double *))host_fn("CGContextSetFillPattern");
    void (*fill_rect)(void *, CGRect) = (void (*)(void *, CGRect))host_fn("CGContextFillRect");
    void (*release)(const void *) = (void (*)(const void *))host_fn("CFRelease");
    m32_wr(DATA + 0x20a0, 0);   /* draws */
    m32_wr(DATA + 0x20b0, 0);   /* CGPatternCallbacks */
    m32_wr(DATA + 0x20b4, COUNTER);
    m32_wr(DATA + 0x20b8, COUNTER);
    uint32_t pa[16] = { DATA + 0x20a0, 0, 0, float_word(4), float_word(4), float_word(1), 0, 0, float_word(1), 0, 0,
                        float_word(4), float_word(4), 0, 1, DATA + 0x20b0 };
    uint32_t pat = call("_CGPatternCreate", 16, pa);
    check("CGPatternCreate", pat != 0);
    if (pat && ctx) {
        const double alpha = 1;
        fill_space(ctx, pattern_space(NULL));
        fill_pattern(ctx, m32_host(pat), &alpha);
        fill_rect(ctx, CGRectMake(0, 0, 16, 1));
    }
    uint32_t draws = m32_rd(DATA + 0x20a0);
    check("a pattern's guest draw callback", draws > 0);
    if (pat)
        release(m32_host(pat));   /* the context's fill keeps it until the fill changes */
    /* CGContextSetCompositeOperation and CGContextSetCTM, cairo's private calls */
    void (*set_gray)(void *, double, double) = (void (*)(void *, double, double))host_fn("CGContextSetGrayFillColor");
    CGAffineTransform (*get_ctm)(void *) = (CGAffineTransform (*)(void *))host_fn("CGContextGetCTM");
    if (ctx) {
        fill_space(ctx, gray());
        set_gray(ctx, 1, 1);
        check("a pattern's guest release callback", !pat || m32_rd(DATA + 0x20a0) == draws + 1);
        fill_rect(ctx, CGRectMake(0, 0, 16, 1));
        call("_CGContextSetCompositeOperation", 2, (uint32_t[]){ m32_handle(ctx), 0 });   /* clear */
        fill_rect(ctx, CGRectMake(0, 0, 8, 1));
        check("CGContextSetCompositeOperation: clear", px[2] == 0 && px[12] == 255);
        call("_CGContextSetCTM", 7, (uint32_t[]){ m32_handle(ctx), float_word(2), 0, 0, float_word(3), float_word(5), float_word(7) });
        CGAffineTransform t = get_ctm(ctx);
        check("CGContextSetCTM", t.a == 2 && t.d == 3 && t.tx == 5 && t.ty == 7 && t.b == 0 && t.c == 0);
    }
    /* structures returned by guest callbacks: in EAX:EDX, and through a hidden pointer (a callback finds the guest
     * through the process's VM, as in a real run) */
    ocerz_vm_install_handlers(&g_vm);
    CGSize (*size_fn)(void) = (CGSize (*)(void))m32_callback(SIZE_FN, "{ff}()", "{dd}()");
    CGSize sz = size_fn ? size_fn() : (CGSize){ 0, 0 };
    check("a structure a guest callback returns in EAX:EDX", sz.width == 1.5 && sz.height == 2.5);
    CGRect (*rect_fn)(void) = (CGRect (*)(void))m32_callback(RECT_FN, "{{ff}{ff}}()", "{{dd}{dd}}()");
    CGRect r = rect_fn ? rect_fn() : (CGRect){ { 0, 0 }, { 0, 0 } };
    check("a structure a guest callback returns through memory", r.origin.x == 1 && r.origin.y == 2 && r.size.width == 3 && r.size.height == 4);
}

/* libcrypto's MD5 on CommonCrypto, LaunchServices' private scheme handler (src/m32_libsystem.c, src/m32_carbon.c) */
static void test_removed_calls(void)
{
    uint32_t c = DATA + 0x2100, digest = DATA + 0x2180;
    put_str(DATA + 0x21a0, "abc");
    call("_MD5_Init", 1, (uint32_t[]){ c });
    call("_MD5_Update", 3, (uint32_t[]){ c, DATA + 0x21a0, 3 });
    call("_MD5_Final", 2, (uint32_t[]){ digest, c });
    static const uint8_t abc[16] = { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 };
    check("MD5 of abc", !memcmp(m32_h(digest), abc, 16));
    void *(*cfstr)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))host_fn("CFStringCreateWithCString");
    m32_wr(DATA + 0x21b0, 0xdeadbeef);
    int32_t st = (int32_t)call("__LSCopyDefaultSchemeHandlerURL", 2, (uint32_t[]){ m32_handle(cfstr(NULL, "http", 0x08000100)), DATA + 0x21b0 });
    uint32_t app = m32_rd(DATA + 0x21b0);
    check("_LSCopyDefaultSchemeHandlerURL answers a URL or none", (st == 0 && app != 0) || (st == -10814 && app == 0));
}

static void test_menus_controls(uint32_t w)
{
    uint32_t mo = DATA + 0x1000;
    cross(CARBON, "_CreateNewMenu", 3, (uint32_t[]){ 300, 0, mo });
    uint32_t menu = m32_rd(mo);
    void *(*cfs)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    uint32_t text = m32_handle(cfs(NULL, "Load ROM", 0x08000100));
    cross(CARBON, "_AppendMenuItemTextWithCFString", 5, (uint32_t[]){ menu, text, 0, FOUR('l','o','a','d'), 0 });
    cross(CARBON, "_DisableMenuCommand", 2, (uint32_t[]){ menu, FOUR('l','o','a','d') });
    check("a disabled command", (call("_IsMenuCommandEnabled", 2, (uint32_t[]){ menu, FOUR('l','o','a','d') }) & 0xff) == 0);
    call("_EnableMenuCommand", 2, (uint32_t[]){ menu, FOUR('l','o','a','d') });
    check("EnableMenuCommand", (call("_IsMenuCommandEnabled", 2, (uint32_t[]){ menu, FOUR('l','o','a','d') }) & 0xff) == 1);
    call("_SetItemCmd", 3, (uint32_t[]){ menu, 1, 'O' });
    call("_GetItemCmd", 3, (uint32_t[]){ menu, 1, DATA + 0x1010 });
    check("SetItemCmd and GetItemCmd", *(int16_t *)m32_h(DATA + 0x1010) == 'O');
    cross(CARBON, "_CreateNewMenu", 3, (uint32_t[]){ 301, 0, mo });
    uint32_t sub = m32_rd(mo);
    cross(CARBON, "_InsertMenu", 2, (uint32_t[]){ sub, (uint32_t)-1 /* kInsertHierarchicalMenu */ });
    check("SetMenuItemHierarchicalID", call("_SetMenuItemHierarchicalID", 3, (uint32_t[]){ menu, 1, 301 }) == 0);
    call("_GetMenuItemHierarchicalID", 3, (uint32_t[]){ menu, 1, DATA + 0x1014 });
    check("GetMenuItemHierarchicalID", *(int16_t *)m32_h(DATA + 0x1014) == 301);
    uint32_t mine = m32_handle(calloc(1, 64));   /* a menu bar the program made itself, as Kega Fusion does */
    call("_SetMenuBar", 1, (uint32_t[]){ mine });
    call("_DisposeMenuBar", 1, (uint32_t[]){ mine });
    uint32_t bar = call("_GetMenuBar", 0, NULL);
    check("GetMenuBar", bar != 0);
    call("_SetMenuBar", 1, (uint32_t[]){ bar });
    call("_DisposeMenuBar", 1, (uint32_t[]){ bar });

    /* a push button: its reference value, 32-bit value, region and hit part */
    uint32_t r = qd_rect(DATA + 0x1020, 20, 30, 40, 130);
    uint32_t title = m32_handle(cfs(NULL, "Push", 0x08000100));
    check("CreatePushButtonControl", cross(CARBON, "_CreatePushButtonControl", 4, (uint32_t[]){ w, r, title, DATA + 0x1030 }) == 0);
    uint32_t button = m32_rd(DATA + 0x1030);
    call("_SetControlReference", 2, (uint32_t[]){ button, 0xcafe1234 });
    check("SetControlReference and GetControlReference", call("_GetControlReference", 1, (uint32_t[]){ button }) == 0xcafe1234);
    call("_SetControl32BitMaximum", 2, (uint32_t[]){ button, 1000 });
    call("_SetControl32BitValue", 2, (uint32_t[]){ button, 1 });
    check("the 32-bit value and maximum", call("_GetControl32BitMaximum", 1, (uint32_t[]){ button }) == 1000 &&
                                          call("_GetControl32BitValue", 1, (uint32_t[]){ button }) == 1);
    void *(*newrgn)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "NewRgn");
    void (*bounds)(void *, int16_t *) = (void (*)(void *, int16_t *))dlsym(RTLD_DEFAULT, "GetRegionBounds");
    void *rgn = newrgn();
    int16_t rb[4] = { 0, 0, 0, 0 };
    check("GetControlRegion", call("_GetControlRegion", 3, (uint32_t[]){ button, (uint32_t)-1, m32_handle(rgn) }) == 0);
    bounds(rgn, rb);
    check("... is the control's frame in the window", rb[0] == 20 && rb[1] == 30 && rb[2] == 40 && rb[3] == 130);
    check("TestControl inside the button", (int16_t)call("_TestControl", 2, (uint32_t[]){ button, 30u | 80u << 16 }) != 0 &&
                                           (int16_t)call("_TestControl", 2, (uint32_t[]){ button, 5u | 5u << 16 }) == 0);
    check("FindControl finds it", (int16_t)call("_FindControl", 3, (uint32_t[]){ 30u | 80u << 16, w, DATA + 0x1034 }) != 0 &&
                                  m32_rd(DATA + 0x1034) == button);
    check("IsControlEnabled", (call("_IsControlEnabled", 1, (uint32_t[]){ button }) & 0xff) == 1);
    /* a user pane keeps the procedures the program hands it */
    check("CreateUserPaneControl", call("_CreateUserPaneControl", 4, (uint32_t[]){ w, qd_rect(DATA + 0x1040, 50, 10, 90, 60), 0,
                                                                                  DATA + 0x1050 }) == 0 && m32_rd(DATA + 0x1050));
    uint32_t pane = m32_rd(DATA + 0x1050);
    m32_wr(DATA + 0x1060, 0x00123450);
    check("SetControlData(kControlUserPaneDrawProcTag)", (int16_t)cross(CARBON, "_SetControlData", 5,
                                                         (uint32_t[]){ pane, 0, FOUR('d','r','a','w'), 4, DATA + 0x1060 }) == 0);
    m32_wr(DATA + 0x1064, 0);
    cross(CARBON, "_GetControlData", 6, (uint32_t[]){ pane, 0, FOUR('d','r','a','w'), 4, DATA + 0x1064, 0 });
    check("GetControlData gives the procedure back", m32_rd(DATA + 0x1064) == 0x00123450);
    check("a pane with no hit-test procedure is hit inside", (int16_t)call("_TestControl", 2, (uint32_t[]){ pane, 70u | 20u << 16 }) == 1 &&
                                                             (int16_t)call("_TestControl", 2, (uint32_t[]){ pane, 20u | 70u << 16 }) == 0);
    m32_wr(DATA + 0x1060, 0);
    cross(CARBON, "_SetControlData", 5, (uint32_t[]){ pane, 0, FOUR('d','r','a','w'), 4, DATA + 0x1060 });
    uint32_t root = DATA + 0x1070;
    cross(CARBON, "_GetRootControl", 2, (uint32_t[]){ w, root });
    call("_CountSubControls", 2, (uint32_t[]){ m32_rd(root), DATA + 0x1074 });
    check("CountSubControls", *(uint16_t *)m32_h(DATA + 0x1074) >= 2);
}

static void test_carbon(void)
{
    void *(*session)(void) = (void *(*)(void))dlsym(dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                                                           RTLD_LAZY), "CGSessionCopyCurrentDictionary");
    void *(*make)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    int (*get)(void *, char *, long, uint32_t) = (int (*)(void *, char *, long, uint32_t))dlsym(RTLD_DEFAULT, "CFStringGetCString");
    if (!session || !session()) {
        printf("test_m32: SKIP carbon (no window server session)\n");
        return;
    }
    check("Carbon opens", dlopen(CARBON, RTLD_LAZY | RTLD_GLOBAL) != NULL);   /* as m32 opens a program's libraries */
    const int16_t rect[4] = { 100, 120, 300, 520 };   /* top, left, bottom, right */
    memcpy(m32_h(DATA + 0xa00), rect, sizeof rect);
    m32_wr(DATA + 0xa10, 0);
    uint32_t a[4] = { 6 /* kDocumentWindowClass */, 0, DATA + 0xa00, DATA + 0xa10 };
    check("CreateNewWindow without the compositing attribute makes a window", cross(CARBON, "_CreateNewWindow", 4, a) == 0);
    uint32_t w = m32_rd(DATA + 0xa10);
    check("... and hands the guest a window", w != 0);
    if (!w)
        return;
    uint32_t b[3] = { w, 33 /* kWindowContentRgn */, DATA + 0xa20 };
    check("GetWindowBounds answers the bounds it was made with", cross(CARBON, "_GetWindowBounds", 3, b) == 0 &&
                                                                 !memcmp(m32_h(DATA + 0xa20), rect, sizeof rect));
    uint32_t title = m32_handle(make(NULL, "m32 carbon", 0x08000100));
    uint32_t t[2] = { w, title };
    check("SetWindowTitleWithCFString", cross(CARBON, "_SetWindowTitleWithCFString", 2, t) == 0);
    uint32_t c[2] = { w, DATA + 0xa30 };
    char back[64] = "";
    check("CopyWindowTitleAsCFString gives the title back", cross(CARBON, "_CopyWindowTitleAsCFString", 2, c) == 0 &&
                                                            get(m32_host(m32_rd(DATA + 0xa30)), back, sizeof back, 0x08000100) &&
                                                            !strcmp(back, "m32 carbon"));
    uint32_t v[1] = { w };
    check("IsWindowVisible before ShowWindow", (cross(CARBON, "_IsWindowVisible", 1, v) & 0xff) == 0);
    uint32_t m[3] = { 128, 0, DATA + 0xa40 };
    check("CreateNewMenu", cross(CARBON, "_CreateNewMenu", 3, m) == 0 && m32_rd(DATA + 0xa40) != 0);
    uint32_t item = m32_handle(make(NULL, "Open ROM", 0x08000100));
    uint32_t i[5] = { m32_rd(DATA + 0xa40), item, 0, 0, 0x6f70656e /* 'open' */ };
    check("InsertMenuItemTextWithCFString", cross(CARBON, "_InsertMenuItemTextWithCFString", 5, i) == 0);
    uint32_t n[1] = { m32_rd(DATA + 0xa40) };
    check("CountMenuItems counts it", (cross(CARBON, "_CountMenuItems", 1, n) & 0xffff) == 1);

    /* the window's QuickDraw port: its content's size, drawn into in guest memory */
    uint32_t port = call("_GetWindowPort", 1, v), pt[1] = { port };
    check("GetWindowPort", port != 0 && call("_GetWindowPort", 1, v) == port && call("_GetWindowFromPort", 1, pt) == w);
    call("_SetPortWindowPort", 1, v);
    uint32_t gp[1] = { DATA + 0xa50 };
    call("_GetPort", 1, gp);
    check("SetPortWindowPort makes it the current port", m32_rd(DATA + 0xa50) == port);
    uint32_t pb[2] = { port, DATA + 0xa60 };
    call("_GetPortBounds", 2, pb);
    int16_t pr[4];
    memcpy(pr, m32_h(DATA + 0xa60), 8);
    check("a window's port bounds its content", pr[0] == 0 && pr[1] == 0 && pr[2] == 200 && pr[3] == 400);
    qd_rgb(DATA + 0xa70, 0xffff, 0, 0);
    call("_RGBForeColor", 1, (uint32_t[]){ DATA + 0xa70 });
    call("_PaintRect", 1, (uint32_t[]){ qd_rect(DATA + 0xa80, 10, 10, 20, 20) });
    uint32_t pmh = call("_GetPortPixMap", 1, pt);
    check("drawing into a window's port fills its pixels", qd_pixel(pmh, 15, 15) == 0xff0000 && qd_pixel(pmh, 25, 15) != 0xff0000);
    check("... and the window's content view shows them, on white", reddish(view_pixel(m32_host(w), 15, 15)) &&
                                                                    view_pixel(m32_host(w), 25, 15) == 0xffffff &&
                                                                    view_pixel(m32_host(w), 15, 185) == 0xffffff);
    {
        int16_t p0[2] = { 5, 7 };   /* v, h */
        memcpy(m32_h(DATA + 0xa90), p0, 4);
        call("_LocalToGlobal", 1, (uint32_t[]){ DATA + 0xa90 });
        memcpy(p0, m32_h(DATA + 0xa90), 4);
        check("LocalToGlobal adds the content's top left", p0[0] == 105 && p0[1] == 127);
        call("_GlobalToLocal", 1, (uint32_t[]){ DATA + 0xa90 });
        memcpy(p0, m32_h(DATA + 0xa90), 4);
        check("GlobalToLocal takes it away", p0[0] == 5 && p0[1] == 7);
    }
    uint32_t q[2] = { port, DATA + 0xaa0 };
    check("QDBeginCGContext on a window's port", call("_QDBeginCGContext", 2, q) == 0 && m32_rd(DATA + 0xaa0) != 0 &&
                                                 call("_QDEndCGContext", 2, q) == 0);

    /* devices: the main display first, in global coordinates */
    CGRect (*bounds)(uint32_t) = (CGRect (*)(uint32_t))dlsym(RTLD_DEFAULT, "CGDisplayBounds");
    uint32_t (*main_id)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "CGMainDisplayID");
    uint32_t gd = call("_GetMainDevice", 0, NULL), gdh[1] = { gd };
    int16_t gr[4];
    memcpy(gr, m32_h(m32_rd(gd) + 34), 8);
    CGRect mb = bounds(main_id());
    check("GetMainDevice bounds the main display", gd && gr[1] == (int16_t)mb.origin.x && gr[0] == (int16_t)mb.origin.y &&
                                                   gr[3] == (int16_t)(mb.origin.x + mb.size.width) &&
                                                   gr[2] == (int16_t)(mb.origin.y + mb.size.height));
    check("QDGetCGDirectDisplayID names it", call("_QDGetCGDirectDisplayID", 1, gdh) == main_id());
    check("TestDeviceAttribute: mainScreen", (call("_TestDeviceAttribute", 2, (uint32_t[]){ gd, 11 }) & 0xff) == 1);

    /* cursors: the arrow and a system cursor become NSCursor's */
    call("_GetQDGlobalsArrow", 1, (uint32_t[]){ DATA + 0xab0 });
    call("_SetCursor", 1, (uint32_t[]){ DATA + 0xab0 });
    uint32_t ib = call("_GetCursor", 1, (uint32_t[]){ 1 });
    check("GetCursor(iBeamCursor)", ib != 0 && m32_rd(ib) != 0);
    call("_SetCursor", 1, (uint32_t[]){ m32_rd(ib) });

    /* the region and rectangle calls QuickDraw still exports cross */
    uint32_t rgn = cross(APPSERVICES, "_NewRgn", 0, NULL);
    check("NewRgn", rgn != 0);
    cross(APPSERVICES, "_RectRgn", 2, (uint32_t[]){ rgn, qd_rect(DATA + 0xac0, 1, 2, 30, 40) });
    cross(APPSERVICES, "_GetRegionBounds", 2, (uint32_t[]){ rgn, DATA + 0xac8 });
    check("RectRgn and GetRegionBounds", !memcmp(m32_h(DATA + 0xac0), m32_h(DATA + 0xac8), 8));
    check("PtInRgn takes its Point by value", (cross(APPSERVICES, "_PtInRgn", 2, (uint32_t[]){ 10u | 20u << 16, rgn }) & 0xff) == 1 &&
                                              (cross(APPSERVICES, "_PtInRgn", 2, (uint32_t[]){ 10u | 50u << 16, rgn }) & 0xff) == 0);
    call("_SetClip", 1, (uint32_t[]){ rgn });
    uint32_t clip = cross(APPSERVICES, "_NewRgn", 0, NULL);
    call("_GetClip", 1, (uint32_t[]){ clip });
    check("SetClip and GetClip", (cross(APPSERVICES, "_EqualRgn", 2, (uint32_t[]){ rgn, clip }) & 0xff) == 1);
    cross(APPSERVICES, "_DisposeRgn", 1, (uint32_t[]){ rgn });
    cross(APPSERVICES, "_DisposeRgn", 1, (uint32_t[]){ clip });
    test_agl(port);
    test_event_params(w, port);
    test_float_arrays();
    test_menus_controls(w);
    cross(CARBON, "_DisposeWindow", 1, v);
    check("DisposeWindow takes the window's port with it", call("_IsValidPort", 1, pt) == 0);
}

/* what SDL 1.2's Quartz code needs (AssaultCube): no KCHR from the Script Manager, the CPS process calls, and
 * NSBitmapImageRep planes in guest memory (src/m32_carbon.c, src/m32_libsystem.c, src/m32_objc.c) */
static void test_sdl_shims(void)
{
    check("GetScriptManagerVariable(smKCHRCache) is no KCHR", call("_GetScriptManagerVariable", 1, (uint32_t[]){ 38 }) == 0);
    memset(m32_h(DATA + 0x2200), 0, 8);
    check("CPSGetCurrentProcess", call("_CPSGetCurrentProcess", 1, (uint32_t[]){ DATA + 0x2200 }) == 0 &&
                                  (m32_rd(DATA + 0x2200) || m32_rd(DATA + 0x2204)));
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL))
        return;
    void *(*send)(void *, void *) = (void *(*)(void *, void *))objc_msgSend;
    void *(*send_s)(void *, void *, const char *) = (void *(*)(void *, void *, const char *))objc_msgSend;
    void *rep = send(objc_getClass("NSBitmapImageRep"), sel_registerName("alloc"));
    void *space = send_s(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "NSDeviceWhiteColorSpace");
    uint32_t init = m32_sel_guest(sel_registerName("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:"
                                                   "samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:"));
    /* SDL's cursor: 16 x 16, one bit per sample, white and alpha planes, no planes given */
    uint32_t r = call("_objc_msgSend", 12, (uint32_t[]){ m32_objc_to_guest(rep), init, 0, 16, 16, 1, 2, 1, 1,
                                                         m32_objc_to_guest(space), 2, 0 });
    void *hr = r ? m32_objc_to_host(r) : NULL;
    check("initWithBitmapDataPlanes: with no planes", hr != NULL);
    if (!hr)
        return;
    memset(m32_h(DATA + 0x2240), 0xff, 20);
    call("_objc_msgSend", 3, (uint32_t[]){ r, m32_sel_guest(sel_registerName("getBitmapDataPlanes:")), DATA + 0x2240 });
    uint32_t p0 = m32_rd(DATA + 0x2240), p1 = m32_rd(DATA + 0x2244);
    check("getBitmapDataPlanes: answers planes in guest memory", p0 && !m32_is_handle(p0) && p1 == p0 + 32 &&
                                                                 m32_rd(DATA + 0x2248) == 0);
    if (!p0 || m32_is_handle(p0))
        return;
    memset(m32_h(p0), 0xaa, 32);
    const uint8_t *data = send(hr, sel_registerName("bitmapData"));
    check("the rep draws from the guest's plane", data == m32_h(p0) && data[0] == 0xaa);
    /* the default output audio unit through the Component Manager, as OpenAL finds it (src/m32_audio.c) */
    const uint32_t desc[5] = { 'auou', 'def ', 'appl', 0, 0 };
    memcpy(m32_h(DATA + 0x2280), desc, sizeof desc);
    uint32_t comp = call("_FindNextComponent", 2, (uint32_t[]){ 0, DATA + 0x2280 });
    check("FindNextComponent finds the default output unit", comp != 0);
    check("CountComponents", call("_CountComponents", 1, (uint32_t[]){ DATA + 0x2280 }) >= 1);
    m32_wr(DATA + 0x22a0, 0);
    check("OpenAComponent", comp && call("_OpenAComponent", 2, (uint32_t[]){ comp, DATA + 0x22a0 }) == 0 && m32_rd(DATA + 0x22a0));
    uint32_t inst = m32_rd(DATA + 0x22a0);
    check("CallComponentVersion", inst && call("_CallComponentVersion", 1, (uint32_t[]){ inst }) != 0);
    memset(m32_h(DATA + 0x22c0), 0, 20);
    check("GetComponentInfo", call("_GetComponentInfo", 5, (uint32_t[]){ comp, DATA + 0x22c0, 0, 0, 0 }) == 0 &&
                              m32_rd(DATA + 0x22c0) == 'auou');
    check("CloseComponent", inst && call("_CloseComponent", 1, (uint32_t[]){ inst }) == 0);
    /* SDL 1.2's CoreAudio driver: Mac OS X 10.1's description ('aunt', 'out ', the unit's ID 'def ') and its render
     * callback (kAudioUnitProperty_SetInputCallback, the flags by value, one AudioBuffer) */
    const uint32_t old_desc[5] = { 'aunt', 'out ', 'def ', 0, 0 };
    memcpy(m32_h(DATA + 0x2300), old_desc, sizeof old_desc);
    uint32_t oc = call("_FindNextComponent", 2, (uint32_t[]){ 0, DATA + 0x2300 });
    check("FindNextComponent of a Mac OS X 10.1 description", oc != 0);
    m32_wr(DATA + 0x2320, 0);
    if (oc && call("_OpenAComponent", 2, (uint32_t[]){ oc, DATA + 0x2320 }) == 0) {
        uint32_t unit = m32_rd(DATA + 0x2320);
        int32_t (*render)(void *, uint32_t *, const void *, uint32_t, uint32_t, void *) =
            (int32_t (*)(void *, uint32_t *, const void *, uint32_t, uint32_t, void *))
                dlsym(dlopen("/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox", RTLD_LAZY),
                      "AudioUnitRender");
        int32_t (*init)(void *) = (int32_t (*)(void *))dlsym(RTLD_DEFAULT, "AudioUnitInitialize");
        memcpy(m32_h(CG_CODE), k_cg_code, sizeof k_cg_code);
        m32_wr(DATA + 0x2340, 0);             /* renders counted by the guest's counter */
        m32_wr(DATA + 0x2348, COUNTER);       /* AudioUnitInputCallback: inputProc, inputProcRefCon */
        m32_wr(DATA + 0x234c, DATA + 0x2340);
        int32_t st = (int32_t)call("_AudioUnitSetProperty", 6, (uint32_t[]){ unit, 7, 1, 0, DATA + 0x2348, 8 });
        check("kAudioUnitProperty_SetInputCallback", st == 0);
        static float samples[2 * 64];
        struct { uint32_t n; struct { uint32_t channels, size; void *data; } b[1]; } list = { 1, { { 2, sizeof samples, samples } } };
        struct { double sample_time; uint64_t host_time; double rate; uint64_t word; uint32_t flags, reserved; } ts = { 0, 0, 1, 0, 1, 0 };
        uint32_t flags = 0;
        if (init && render && st == 0 && init(m32_host(unit)) == 0)
            render(m32_host(unit), &flags, &ts, 0, 64, &list);
        check("... the old callback runs as guest code", m32_rd(DATA + 0x2340) == 1);
        call("_CloseComponent", 1, (uint32_t[]){ unit });
    }
}

/* what Neverball's and Wesnoth's libraries needed of libc, ATSUI and the FPCR (src/m32_stdio.c, src/m32_libsystem.c,
 * src/m32_atsu.c, src/cpu.c) */
static void test_game_libs(void)
{
    /* __srget is the next character, not the first of the next buffer (the i386 getc macro calls it every time) */
    char path[] = "/tmp/test_m32_srget.XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        char data[5000];
        for (size_t i = 0; i < sizeof data; i++)
            data[i] = (char)('a' + i % 26);
        write(fd, data, sizeof data);
        close(fd);
        FILE *f = fopen(path, "r");
        uint32_t twin = f ? m32_file(f) : 0;
        int a = (int)call("___srget", 1, (uint32_t[]){ twin }), b = (int)call("___srget", 1, (uint32_t[]){ twin }),
            c = (int)call("___srget", 1, (uint32_t[]){ twin });
        check("__srget reads the stream in order", a == 'a' && b == 'b' && c == 'c');
        if (f)
            fclose(f);
        unlink(path);
    }
    /* the guest is told 4 KB pages, as on the Intel Macs, by every call that tells it, and valloc aligns to them */
    check("getpagesize and sysconf(_SC_PAGESIZE) say 4 KB", call("_getpagesize", 0, NULL) == 4096 &&
          call("_sysconf", 1, (uint32_t[]){ _SC_PAGESIZE }) == 4096 && m32_page_size() == 4096);
    m32_wr(DATA + 0x4f00, 0);
    check("... as host_page_size does", call("_host_page_size", 2, (uint32_t[]){ 0, DATA + 0x4f00 }) == 0 &&
          m32_rd(DATA + 0x4f00) == 4096);
    check("... and vm_page_size", m32_rd(m32_special_data("_vm_page_size", NULL)) == 4096 &&
          m32_rd(m32_special_data("_vm_page_mask", NULL)) == 4095 && m32_rd(m32_special_data("_vm_page_shift", NULL)) == 12);
    uint32_t v = call("_valloc", 1, (uint32_t[]){ 100 });
    check("valloc is page aligned", v && (v % m32_page_size()) == 0);
    /* localeconv: an i386 struct lconv, ten string pointers then the chars at 40 */
    uint32_t lc = call("_localeconv", 0, NULL);
    check("localeconv's decimal point", lc && m32_rd(lc) && !strcmp((const char *)m32_h(m32_rd(lc)), localeconv()->decimal_point));
    check("... and its chars at 40", lc && *(const char *)m32_h(lc + 40) == localeconv()->int_frac_digits);
    /* an ATSUI layout made empty and given its text and style afterwards, as Pango makes it */
    uint32_t st = DATA + 0x2400, lay = DATA + 0x2404;
    call("_ATSUCreateStyle", 1, (uint32_t[]){ st });
    const uint16_t ok[2] = { 'O', 'K' };
    memcpy(m32_h(DATA + 0x2410), ok, sizeof ok);
    check("ATSUCreateTextLayout", call("_ATSUCreateTextLayout", 1, (uint32_t[]){ lay }) == 0 && m32_rd(lay));
    check("ATSUSetTextPointerLocation", call("_ATSUSetTextPointerLocation", 5, (uint32_t[]){ m32_rd(lay), DATA + 0x2410, 0, 2, 2 }) == 0);
    check("ATSUSetRunStyle", call("_ATSUSetRunStyle", 4, (uint32_t[]){ m32_rd(lay), m32_rd(st), 0, 0xffffffffu }) == 0);
    m32_wr(DATA + 0x2420, 0);
    call("_ATSUGetGlyphInfo", 5, (uint32_t[]){ m32_rd(lay), 0, 2, DATA + 0x2420, 0 });
    check("... the layout lays out its two glyphs", m32_rd(DATA + 0x2420) == 8 + 2 * 28);
    call("_ATSUDisposeTextLayout", 1, (uint32_t[]){ m32_rd(lay) });
    /* FPCR.AH: clear for host code, set for guest code, put back after */
    ocerz_afp_enable();
    if (ocerz_afp()) {
        uint64_t before = __builtin_arm_rsr64("fpcr");
        uint64_t g = ocerz_afp_guest();
        int set = (__builtin_arm_rsr64("fpcr") & 2) != 0;
        uint64_t h = ocerz_afp_host();
        int clear = !(__builtin_arm_rsr64("fpcr") & 2);
        ocerz_afp_restore(h);
        int back = (__builtin_arm_rsr64("fpcr") & 2) != 0;
        ocerz_afp_restore(g);
        check("FPCR.AH set for guest code, clear for host code, put back", set && clear && back &&
              __builtin_arm_rsr64("fpcr") == before);
    }
}

/* OSAtomic's queues link the guest's elements through its own 4-byte pointers, and a pthread_rwlockattr_t is the
 * guest's 16 bytes (src/m32_thread.c) */
static void test_queues(void)
{
    uint32_t lifo = DATA + 0x4400, fifo = DATA + 0x4410, e = DATA + 0x4420;   /* elements: {payload, link} */
    memset(m32_h(lifo), 0, 32);
    for (uint32_t i = 0; i < 3; i++) {
        m32_wr(e + 8 * i, i + 1);
        call("_OSAtomicEnqueue", 3, (uint32_t[]){ lifo, e + 8 * i, 4 });
    }
    uint32_t a = call("_OSAtomicDequeue", 2, (uint32_t[]){ lifo, 4 }), b = call("_OSAtomicDequeue", 2, (uint32_t[]){ lifo, 4 });
    check("OSAtomicDequeue is last in, first out", a == e + 16 && b == e + 8);
    call("_OSAtomicDequeue", 2, (uint32_t[]){ lifo, 4 });
    check("... and empty after", call("_OSAtomicDequeue", 2, (uint32_t[]){ lifo, 4 }) == 0);
    for (uint32_t i = 0; i < 3; i++)
        call("_OSAtomicFifoEnqueue", 3, (uint32_t[]){ fifo, e + 8 * i, 4 });
    a = call("_OSAtomicFifoDequeue", 2, (uint32_t[]){ fifo, 4 });
    b = call("_OSAtomicFifoDequeue", 2, (uint32_t[]){ fifo, 4 });
    uint32_t c = call("_OSAtomicFifoDequeue", 2, (uint32_t[]){ fifo, 4 });
    check("OSAtomicFifoDequeue is first in, first out", a == e && b == e + 8 && c == e + 16 &&
          call("_OSAtomicFifoDequeue", 2, (uint32_t[]){ fifo, 4 }) == 0 && m32_rd(fifo + 4) == 0);
    uint32_t attr = DATA + 0x4460, ps = DATA + 0x4474;
    m32_wr(attr + 16, 0xabcdu);
    check("pthread_rwlockattr_init in the guest's 16 bytes", call("_pthread_rwlockattr_init", 1, (uint32_t[]){ attr }) == 0 &&
          m32_rd(attr + 16) == 0xabcdu);
    call("_pthread_rwlockattr_setpshared", 2, (uint32_t[]){ attr, 1 });
    m32_wr(ps, 0);
    call("_pthread_rwlockattr_getpshared", 2, (uint32_t[]){ attr, ps });
    check("pthread_rwlockattr_getpshared", m32_rd(ps) == 1);
}

/* OpenGL's buffer objects: with one bound, a gl*Pointer's or a glDraw*Elements' pointer is an offset into it and
 * crosses as it is (src/m32_gl.c); and a function pointer to an export is its trap, which the JIT translates as the
 * stub it stands for, through a slot holding the trap (src/m32_bridge.c) */
static void test_gl_buffers(void)
{
    check("gl*Pointer and glDraw*Elements are recognised", m32_gl_ptr_kind("_glTexCoordPointer") == M32_GL_ARRAY &&
          m32_gl_ptr_kind("_glVertexAttribPointerARB") == M32_GL_ARRAY &&
          m32_gl_ptr_kind("_glDrawRangeElements") == M32_GL_ELEMENT && m32_gl_ptr_kind("_glBindBufferARB") == M32_GL_BIND &&
          m32_gl_ptr_kind("_glBindVertexArrayAPPLE") == M32_GL_BIND_VAO && m32_gl_ptr_kind("_glEnable") == M32_GL_NONE &&
          m32_gl_ptr_kind("_strlen") == M32_GL_NONE);
    check("no buffer bound: a pointer is a guest address", m32_gl_ptr(M32_GL_ARRAY, 0x1f350) == (uint64_t)(uintptr_t)m32_h(0x1f350) &&
          m32_gl_ptr(M32_GL_ELEMENT, 0x1f350) == (uint64_t)(uintptr_t)m32_h(0x1f350));
    m32_gl_bound(M32_GL_BIND, 0x8892, 0x80);   /* GL_ARRAY_BUFFER */
    check("an array buffer bound: a gl*Pointer's is an offset", m32_gl_ptr(M32_GL_ARRAY, 0x1f350) == 0x1f350 &&
          m32_gl_ptr(M32_GL_ELEMENT, 0x40) == (uint64_t)(uintptr_t)m32_h(0x40));
    m32_gl_bound(M32_GL_BIND, 0x8893, 0x7f);   /* GL_ELEMENT_ARRAY_BUFFER */
    check("an element buffer bound: indices are an offset", m32_gl_ptr(M32_GL_ELEMENT, 0x40) == 0x40);
    m32_gl_bound(M32_GL_BIND, 0x8892, 0);
    m32_gl_bound(M32_GL_BIND, 0x8893, 0);
    check("unbound again: guest addresses", m32_gl_ptr(M32_GL_ARRAY, 0x1f350) == (uint64_t)(uintptr_t)m32_h(0x1f350) &&
          m32_gl_ptr(M32_GL_ELEMENT, 0x40) == (uint64_t)(uintptr_t)m32_h(0x40));
    uint32_t trap = m32_export("/usr/lib/libSystem.B.dylib", "_strlen");
    uint32_t slot = trap ? m32_trap_slot(trap) : 0;
    check("an export's trap has a slot holding it", slot && m32_rd(slot) == trap && m32_trap_slot(trap) == slot);
    check("... and the window's other traps have none", m32_trap_slot(OCERZ_DYLDAPI_LO + 1) == 0);
}

/* what Adium's frameworks needed of m32's dyld: rpaths that are directories relative to the loader, and images
 * fixed up and initialized after their dependencies (src/m32_dyld.c) */
static void test_image_order(void)
{
    char dir[] = "/tmp/test_m32_rpath.XXXXXX", path[PATH_MAX], want[PATH_MAX], got[PATH_MAX];
    if (mkdtemp(dir)) {
        snprintf(path, sizeof path, "%s/MacOS", dir);   /* MacOS/.. resolves only through a directory */
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/Frameworks", dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/Frameworks/X.framework", dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/Frameworks/X.framework/X", dir);
        close(open(path, O_CREAT | O_WRONLY, 0644));
        static M32Image loader;
        snprintf(loader.path, sizeof loader.path, "%s/MacOS/prog", dir);
        loader.rpath[0] = "@loader_path/../Frameworks";
        loader.nrpath = 1;
        check("@rpath through an @loader_path directory", realpath(path, want) &&
              m32_resolve_path(&loader, "@rpath/X.framework/X", got, sizeof got) == 0 && !strcmp(got, want));
        unlink(path);
        snprintf(path, sizeof path, "%s/Frameworks/X.framework", dir);
        rmdir(path);
        snprintf(path, sizeof path, "%s/Frameworks", dir);
        rmdir(path);
        snprintf(path, sizeof path, "%s/MacOS", dir);
        rmdir(path);
        rmdir(dir);
    }
    /* loaded A, C, B, D with A needing B and B needing C: C, B, A in that order, D (independent) where it was */
    static M32Image a, b, c, d;
    a.dep_img[0] = &b;
    a.ndeps = 1;
    b.dep_img[0] = &c;
    b.ndeps = 1;
    int from = m32_nimages;
    if (from + 4 > M32_MAX_IMAGES)
        return;
    m32_images[from] = &a;
    m32_images[from + 1] = &c;
    m32_images[from + 2] = &b;
    m32_images[from + 3] = &d;
    m32_nimages = from + 4;
    M32Image *order[M32_MAX_IMAGES];
    int n = m32_dep_order(from, order), at[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < n; i++)
        at[order[i] == &a ? 0 : order[i] == &b ? 1 : order[i] == &c ? 2 : 3] = i;
    check("images in dependency order", n == 4 && at[2] < at[1] && at[1] < at[0] && at[3] >= 0);
    m32_nimages = from;
    /* a leaf name alone is found as dyld finds it, /usr/lib last: DarkPlaces dlopens "libz.dylib" */
    put_str(DATA + 0x4500, "libz.dylib");
    put_str(DATA + 0x4520, "inflateInit2_");
    put_str(DATA + 0x4540, "libm32_no_such_library.dylib");
    uint32_t z = call("_dlopen", 2, (uint32_t[]){ DATA + 0x4500, 9 });
    check("dlopen of a leaf name finds the system library", z != 0 &&
          call("_dlsym", 2, (uint32_t[]){ z, DATA + 0x4520 }) != 0);
    check("... and not one that is nowhere", call("_dlopen", 2, (uint32_t[]){ DATA + 0x4540, 9 }) == 0);
    /* a library missing where its path says falls back to its leaf name as dyld does (LinCity-NG): the system's
     * libz, the runtime's libstdc++ */
    char fb[PATH_MAX], want_cxx[PATH_MAX];
    check("a missing @executable_path library falls back to the system's",
          m32_resolve_path(NULL, "@executable_path/../Frameworks/libz.1.dylib", fb, sizeof fb) == 0 && fb[0] == 0);
    if (realpath("runtime/guest32/usr/lib/libstdc++.6.dylib", want_cxx))
        check("... and libstdc++ to the runtime's",
              m32_resolve_path(NULL, "@executable_path/../Frameworks/libstdc++.6.dylib", fb, sizeof fb) == 0 &&
              !strcmp(fb, want_cxx));
    /* a libgcc_s a program bundles answers _Unwind_* with m32's unwinder too, as the runtime's does (Pingus) */
    static M32Image gcc, other;
    snprintf(gcc.path, sizeof gcc.path, "/Applications/Game.app/Contents/libs/libgcc_s.1.dylib");
    snprintf(other.path, sizeof other.path, "/Applications/Game.app/Contents/libs/libSDL-1.2.0.dylib");
    uint32_t unwinder = m32_export("/usr/lib/libSystem.B.dylib", "__Unwind_RaiseException");
    check("a bundled libgcc_s's _Unwind_RaiseException is m32's", unwinder &&
          m32_image_symbol(&gcc, "__Unwind_RaiseException") == unwinder);
    check("... and another library's is its own", m32_image_symbol(&other, "__Unwind_RaiseException") == 0);
}

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);

/* Adium's Objective-C, crossing to AppKit and Foundation (src/m32_objc.c, src/m32_objc_types.c, src/m32_cross.c):
 * a guest class paired with a host one, its protocols, its nib outlets and its twins' lifetime; NSNotFound, NSError
 * and NSRange pointers, AppKit's 64-bit tags; sorting with a guest function; method_exchangeImplementations */
static void test_objc_pairs(void)
{
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL))
        return;
    ocerz_vm_install_handlers(&g_vm);   /* a callback finds the guest through the process's VM */
    void *pool = objc_autoreleasePoolPush();
    id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    id (*send1)(id, SEL, const void *) = (id (*)(id, SEL, const void *))objc_msgSend;
    void (*set)(id, SEL, id, id) = (void (*)(id, SEL, id, id))objc_msgSend;
#define SEL_G(s) m32_sel_guest(sel_registerName(s))
#define STR(s) send1((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), s)
    /* NSNotFound is INT64_MAX here and INT32_MAX there, in a result and in an NSRange either way */
    id empty = send((id)objc_getClass("NSArray"), sel_registerName("array")), x = STR("x");
    check("indexOfObject: answers i386's NSNotFound", call("_objc_msgSend", 3, (uint32_t[]){ m32_objc_to_guest(empty),
          SEL_G("indexOfObject:"), m32_objc_to_guest(x) }) == 0x7fffffffu);
    uint32_t loc = call("_objc_msgSend", 3, (uint32_t[]){ m32_objc_to_guest(STR("hello")), SEL_G("rangeOfString:"),
                                                          m32_objc_to_guest(STR("z")) });
    check("rangeOfString: answers {NSNotFound, 0}", loc == 0x7fffffffu && (uint32_t)cpu()->gpr[OCERZ_RDX] == 0);
    uint32_t v = call("_objc_msgSend", 4, (uint32_t[]){ m32_objc_to_guest((id)objc_getClass("NSValue")),
                                                        SEL_G("valueWithRange:"), 0x7fffffffu, 0 });
    struct { uint64_t loc, len; } (*range)(id, SEL) = (void *)objc_msgSend;
    check("... and NSNotFound reaches the host as its own", v && range(m32_objc_to_host(v), sel_registerName("rangeValue")).loc ==
          (uint64_t)INT64_MAX);
    /* an NSError ** is a 4-byte guest id, stored only when the callee stores one */
    char file[] = "/tmp/test_m32_error.XXXXXX";
    int fd = mkstemp(file);
    if (fd >= 0) {
        write(fd, "hi", 2);
        close(fd);
    }
    uint32_t err = DATA + 0x4100, string_cls = m32_objc_to_guest((id)objc_getClass("NSString")),
             read_sel = SEL_G("stringWithContentsOfFile:encoding:error:");
    m32_wr(err, 0x11111111u);
    m32_wr(err + 4, 0x22222222u);
    uint32_t r = call("_objc_msgSend", 5, (uint32_t[]){ string_cls, read_sel, m32_objc_to_guest(STR("/nonexistent/m32")), 4, err });
    id e = r ? nil : m32_objc_to_host(m32_rd(err));
    check("error: answers an NSError in a guest word", !r && e && ((BOOL (*)(id, SEL, Class))objc_msgSend)(e,
          sel_registerName("isKindOfClass:"), objc_getClass("NSError")) && m32_rd(err + 4) == 0x22222222u);
    m32_wr(err, 0x33333333u);
    r = call("_objc_msgSend", 5, (uint32_t[]){ string_cls, read_sel, m32_objc_to_guest(STR(file)), 4, err });
    check("... and is left alone when the call succeeds", r && m32_rd(err) == 0x33333333u);
    unlink(file);
    /* an NSRangePointer: the guest's 8-byte NSRange, written back, its neighbour untouched */
    id as = send1(send((id)objc_getClass("NSAttributedString"), sel_registerName("alloc")),
                  sel_registerName("initWithString:"), STR("hello"));
    uint32_t rp = DATA + 0x4200;
    m32_wr(rp, 0xffffffffu);
    m32_wr(rp + 4, 0xffffffffu);
    m32_wr(rp + 8, 0x44444444u);
    call("_objc_msgSend", 7, (uint32_t[]){ m32_objc_to_guest(as), SEL_G("attribute:atIndex:longestEffectiveRange:inRange:"),
                                           m32_objc_to_guest(STR("NSFont")), 0, rp, 0, 5 });
    check("longestEffectiveRange: fills the i386 NSRange", m32_rd(rp) == 0 && m32_rd(rp + 4) == 5 && m32_rd(rp + 8) == 0x44444444u);
    /* AppKit's tags: a small number each, the host's own back for it, freed for reuse */
    uint32_t t1 = m32_tag_guest(0x7a12345678ull), t2 = m32_tag_guest(0x7a9abcdef0ull);
    check("a 64-bit tag gets a small guest number", t1 && t2 && t1 != t2 && t1 < 0x10000 && m32_tag_guest(0x7a12345678ull) == t1 &&
          m32_tag_host(t1) == 0x7a12345678ull && m32_tag_host(t2) == 0x7a9abcdef0ull);
    m32_tag_free(t1);
    check("... and a removed one's number is reused", m32_tag_guest(0x7a55555555ull) == t1);
    /* a guest class M32TestOwner : NSObject <M32TestA> with an outlet ivar moreInfoView at 4; M32TestA adopts M32TestB,
     * which comes after it in __OBJC,__protocol */
    uint32_t G = DATA + 0x5000, S = G + 0x400;
    memset(m32_h(G), 0, 0x600);   /* S, the strings, at G + 0x400 */
    const char *names[] = { "M32TestOwner", "NSObject", "moreInfoView", "@", "M32TestA", "M32TestB",
                            "validateValue:forKey:error:", "c20@0:4^@8@12^@16" };
    uint32_t at_name[8];
    for (int i = 0, off = 0; i < 8; off += (int)strlen(names[i]) + 1, i++) {
        at_name[i] = S + (uint32_t)off;
        put_str(at_name[i], names[i]);
    }
    uint32_t cls = G, meta = G + 0x40, ivars = G + 0x80, plist = G + 0xa0, apl = G + 0xb0, protos = G + 0xc0,
             symtab = G + 0x100, module = G + 0x120, methods = G + 0x140;
    /* -validateValue:(id *)v forKey:(id)k error:(NSError **)e { id old = *v; *v = k; *e = old; return YES; } */
    static const uint8_t validate[] = { 0x8b, 0x44, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x10, 0x8b, 0x10, 0x89, 0x08, 0x8b,
                                        0x44, 0x24, 0x14, 0x89, 0x10, 0xb8, 0x01, 0, 0, 0, 0xc3 };
    memcpy(m32_h(CODE + 0x840), validate, sizeof validate);
    m32_wr(methods + 4, 1);               /* old_method_list: obsolete, count, {name, types, imp} */
    m32_wr(methods + 8, at_name[6]);
    m32_wr(methods + 12, at_name[7]);
    m32_wr(methods + 16, CODE + 0x840);
    m32_wr(ivars, 1);
    m32_wr(ivars + 4, at_name[2]);
    m32_wr(ivars + 8, at_name[3]);
    m32_wr(ivars + 12, 4);
    m32_wr(protos, 0);                    /* M32TestA: isa, name, protocol_list, instance and class methods */
    m32_wr(protos + 4, at_name[4]);
    m32_wr(protos + 8, apl);
    m32_wr(protos + 20 + 4, at_name[5]);  /* M32TestB */
    m32_wr(apl + 4, 1);                   /* protocol_list: next, count, list[] */
    m32_wr(apl + 8, protos + 20);
    m32_wr(plist + 4, 1);
    m32_wr(plist + 8, protos);
    m32_wr(cls + OC_ISA, meta);
    m32_wr(cls + OC_SUPER, at_name[1]);
    m32_wr(cls + OC_NAME, at_name[0]);
    m32_wr(cls + OC_INFO, OC_CLS_CLASS);
    m32_wr(cls + OC_ISIZE, 8);
    m32_wr(cls + OC_IVARS, ivars);
    m32_wr(cls + OC_PROTOCOLS, plist);
    m32_wr(cls + OC_METHODS, methods);
    m32_wr(meta + OC_ISA, at_name[1]);
    m32_wr(meta + OC_SUPER, at_name[1]);
    m32_wr(meta + OC_NAME, at_name[0]);
    m32_wr(meta + OC_INFO, OC_CLS_META);
    m32_wr(meta + OC_ISIZE, 48);
    m32_wr(symtab + 8, 1);                /* one class, no categories */
    m32_wr(symtab + 12, cls);
    m32_wr(module, 7);
    m32_wr(module + 4, 16);
    m32_wr(module + 12, symtab);
    static M32Image img;
    snprintf(img.path, sizeof img.path, "/test_m32/objc");
    img.sect[0] = (M32Sect){ "__OBJC", "__module_info", module, 16, 0, 0, 0, 0 };
    img.sect[1] = (M32Sect){ "__OBJC", "__protocol", protos, 40, 0, 0, 0, 0 };
    img.nsect = 2;
    m32_objc_load(&img);
    Protocol *pa = objc_getProtocol("M32TestA"), *pb = objc_getProtocol("M32TestB");
    check("guest protocols get host twins", pa && pb && m32_host(protos) == (void *)pa && protocol_conformsToProtocol(pa, pb));
    Class hc = m32_class_host(cls);
    check("the guest class is paired and adopts its protocol", hc && class_getSuperclass(hc) == objc_getClass("NSObject") &&
          class_conformsToProtocol(hc, pa));
    if (!hc) {
        objc_autoreleasePoolPop(pool);
        return;
    }
    id owner = send(send((id)hc, sel_registerName("alloc")), sel_registerName("init"));
    uint32_t twin = m32_objc_to_guest(owner);
    check("an instance has a guest twin", twin && !m32_is_handle(twin) && m32_rd(twin) == cls);
    /* a nib outlet with no setter is the ivar of that name, as NSNibOutletConnector sets it */
    id view = send((id)objc_getClass("NSObject"), sel_registerName("new")), conn = send((id)objc_getClass("NSNibOutletConnector"),
                                                                                         sel_registerName("new"));
    set(conn, sel_registerName("setValue:forKey:"), owner, STR("source"));
    set(conn, sel_registerName("setValue:forKey:"), view, STR("destination"));
    set(conn, sel_registerName("setValue:forKey:"), STR("moreInfoView"), STR("label"));
    send(conn, sel_registerName("establishConnection"));
    check("a nib outlet sets the guest ivar", m32_rd(twin + 4) == m32_objc_to_guest(view));
    /* the host calling the guest's override with an id * read and written and an NSError ** */
    id in = STR("in"), key = STR("key"), value = in, error = nil;
    BOOL ok = ((BOOL (*)(id, SEL, id *, id, id *))objc_msgSend)(owner, sel_registerName("validateValue:forKey:error:"),
                                                                &value, key, &error);
    check("a guest method gets guest copies of the host's id pointers", ok && value == key && error == in);
    /* a guest function the host hands an NSRange *: its i386 copy, written back */
    static const uint8_t bump[] = { 0x8b, 0x44, 0x24, 0x04, 0xff, 0x00, 0xc7, 0x40, 0x04, 4, 0, 0, 0, 0xc3 };
    memcpy(m32_h(CODE + 0x880), bump, sizeof bump);
    void (*bump_fn)(uint64_t *) = (void (*)(uint64_t *))m32_callback(CODE + 0x880, "v(R{uu})", "v(R{LL})");
    uint64_t hr[2] = { 9, 0 };
    if (bump_fn)
        bump_fn(hr);
    check("a guest function gets an i386 copy of the host's NSRange", hr[0] == 10 && hr[1] == 4);
    send(conn, sel_registerName("release"));
    objc_autoreleasePoolPop(pool);
    /* the twin goes with its host object: no alias is left for the next object malloc puts there */
    void *was = (void *)owner;
    send(owner, sel_registerName("release"));
    check("a deallocated object's twin is no longer its", m32_handle_twin_lookup(was) == 0 && m32_host(twin) == m32_h(twin));
    /* sortUsingFunction:context: with a guest comparator: by the ids' values, the order their handles were made in */
    static const uint8_t by_id[] = { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08, 0x39, 0xc8, 0xb8, 0, 0, 0, 0,
                                     0x0f, 0x97, 0xc0, 0x83, 0xd8, 0x00, 0xc3 };   /* (a > b) - (a < b), unsigned */
    memcpy(m32_h(CODE + 0x800), by_id, sizeof by_id);
    id n[3];
    for (int i = 0; i < 3; i++)
        n[i] = send1((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), (const char *[]){ "a", "b", "c" }[i]);
    uint32_t g0 = m32_objc_to_guest(n[0]), g1 = m32_objc_to_guest(n[1]), g2 = m32_objc_to_guest(n[2]);
    id ma = send((id)objc_getClass("NSMutableArray"), sel_registerName("new"));
    id (*add)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
    add(ma, sel_registerName("addObject:"), n[1]);
    add(ma, sel_registerName("addObject:"), n[2]);
    add(ma, sel_registerName("addObject:"), n[0]);
    call("_objc_msgSend", 4, (uint32_t[]){ m32_objc_to_guest(ma), SEL_G("sortUsingFunction:context:"), CODE + 0x800, 0 });
    id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
    uint32_t s0 = m32_objc_to_guest(at(ma, sel_registerName("objectAtIndex:"), 0)),
             s1 = m32_objc_to_guest(at(ma, sel_registerName("objectAtIndex:"), 1)),
             s2 = m32_objc_to_guest(at(ma, sel_registerName("objectAtIndex:"), 2));
    check("sortUsingFunction:context: calls the guest's function", s0 == g0 && s1 == g1 && s2 == g2 && g0 < g1 && g1 < g2);
    /* method_exchangeImplementations of two guest methods {SEL, types, IMP} */
    uint32_t m1 = DATA + 0x4300, m2 = DATA + 0x4310;
    m32_wr(m1 + 8, 0x1111u);
    m32_wr(m2 + 8, 0x2222u);
    call("_method_exchangeImplementations", 2, (uint32_t[]){ m1, m2 });
    check("method_exchangeImplementations swaps guest methods", m32_rd(m1 + 8) == 0x2222u && m32_rd(m2 + 8) == 0x1111u);
    /* the collector's write barriers store, with no collector */
    m32_wr(DATA + 0x4320, 0);
    m32_wr(DATA + 0x4328, 0);
    check("objc_assign_global stores", call("_objc_assign_global", 2, (uint32_t[]){ 0x1234u, DATA + 0x4320 }) == 0x1234u &&
                                       m32_rd(DATA + 0x4320) == 0x1234u);
    check("objc_assign_ivar stores at the offset", call("_objc_assign_ivar", 3, (uint32_t[]){ 0x5678u, DATA + 0x4320, 8 }) == 0x5678u &&
                                                   m32_rd(DATA + 0x4328) == 0x5678u);
    check("objc_read_weak reads", call("_objc_read_weak", 1, (uint32_t[]){ DATA + 0x4328 }) == 0x5678u);
    /* GetProcessInformation of this process, an i386 ProcessInfoRec: its name, number and the FSSpec * left alone */
    uint32_t psn = DATA + 0x4340, info = DATA + 0x4360, pname = DATA + 0x43c0;
    m32_wr(psn, 0);
    m32_wr(psn + 4, 2);                   /* kCurrentProcess */
    memset(m32_h(info), 0, 60);
    m32_wr(info, 60);
    m32_wr(info + 4, pname);
    m32_wr(info + 56, 0x1234u);
    memset(m32_h(pname), 0, 64);
    int16_t perr = (int16_t)call("_GetProcessInformation", 2, (uint32_t[]){ psn, info });
    check("GetProcessInformation fills an i386 ProcessInfoRec", perr == 0 && (m32_rd(info + 8) || m32_rd(info + 12)) &&
          *(const uint8_t *)m32_h(pname) > 0 && m32_rd(info + 4) == pname && m32_rd(info + 56) == 0x1234u);
#undef SEL_G
#undef STR
}

/* through the databases: theme structures packed to 2 (src/m32_cross.c), GNU libiconv's names (src/m32_bridge.c), and
 * dlsym of a function m32 cannot call (src/m32_dyld.c) */
static void test_db_shapes(void)
{
    /* HIThemeTrackDrawInfo, i386: version, kind, HIRect of floats at 6, min, max, value, reserved, attributes,
     * enableState, filler, then the scroll bar's viewsize and pressState at 42 */
    uint8_t ti[48] = { 0 };
    const float bounds[4] = { 10, 20, 15, 200 };
    const int32_t range[4] = { 0, 100, 50, 0 };
    const uint16_t show_thumb = 4;   /* kThemeTrackShowThumb */
    const int32_t viewsize = 10;
    memcpy(ti + 6, bounds, 16);
    memcpy(ti + 22, range, 16);
    memcpy(ti + 38, &show_thumb, 2);
    memcpy(ti + 42, &viewsize, 4);
    memcpy(m32_h(DATA + 0x3000), ti, sizeof ti);
    memset(m32_h(DATA + 0x3040), 0, 16);
    uint32_t st = cross(CARBON, "_HIThemeGetTrackBounds", 2, (uint32_t[]){ DATA + 0x3000, DATA + 0x3040 });
    const float *tb = m32_h(DATA + 0x3040);
    check("HIThemeGetTrackBounds reads the i386 HIThemeTrackDrawInfo", st == 0 && tb[2] > 0 && tb[3] > 100 &&   /* about the bounds */
          tb[0] >= 8 && tb[0] + tb[2] <= 27 && tb[1] >= 18 && tb[1] + tb[3] <= 222);
    /* HIThemeTextInfo, i386: 36 bytes, the font reference last; the dimensions come back as floats */
    void *(*cfstr)(void *, const char *, uint32_t) = (void *(*)(void *, const char *, uint32_t))host_fn("CFStringCreateWithCString");
    memset(m32_h(DATA + 0x3080), 0, 36);
    m32_wr(DATA + 0x3084, 1);   /* kThemeStateActive */
    memset(m32_h(DATA + 0x30c0), 0, 12);
    st = cross(CARBON, "_HIThemeGetTextDimensions", 6, (uint32_t[]){ m32_handle(cfstr(NULL, "Hello", 0x08000100)), 0,
                                                                      DATA + 0x3080, DATA + 0x30c0, DATA + 0x30c4, DATA + 0x30c8 });
    const float *dim = m32_h(DATA + 0x30c0);
    check("HIThemeGetTextDimensions", st == 0 && dim[0] > 10 && dim[0] < 200 && dim[1] > 5 && dim[1] < 50);
    /* libiconv_open and libiconv_close are iconv_open and iconv_close */
    put_str(DATA + 0x3100, "ISO-8859-1");
    put_str(DATA + 0x3110, "UTF-8");
    uint32_t cd = cross("/usr/lib/libiconv.2.dylib", "_libiconv_open", 2, (uint32_t[]){ DATA + 0x3100, DATA + 0x3110 });
    check("libiconv_open", cd != 0 && cd != 0xffffffffu);
    check("libiconv_close", cross("/usr/lib/libiconv.2.dylib", "_libiconv_close", 1, (uint32_t[]){ cd }) == 0);
    /* dlsym: NULL for a function no declaration lets m32 call, so the program takes its own fallback */
    put_str(DATA + 0x3120, "CGContextGetType");
    put_str(DATA + 0x3140, "CGFontGetGlyphPath");
    check("dlsym of a function m32 cannot call", call("_dlsym", 2, (uint32_t[]){ 0xfffffffeu, DATA + 0x3120 }) == 0);
    check("dlsym of a special", call("_dlsym", 2, (uint32_t[]){ 0xfffffffeu, DATA + 0x3140 }) != 0);
    /* a pointer variable's address stands for the host's: GLUT_BITMAP_HELVETICA_12 is &glutBitmapHelvetica12 */
    const char *glut = "/System/Library/Frameworks/GLUT.framework/Versions/A/GLUT";
    void *hfont = dlsym(dlopen(glut, RTLD_LAZY), "glutBitmapHelvetica12");
    uint32_t gfont = m32_export(glut, "_glutBitmapHelvetica12");
    check("GLUT's font variable is the host's", hfont && gfont && m32_host(gfont) == hfont);
    /* errno: a call that sets none leaves the guest's as it was, whatever the host's last was (libvorbisfile) */
    const char *libsystem = "/usr/lib/libSystem.B.dylib";
    uint32_t slot = call("___error", 0, NULL);
    m32_wr(slot, 0);
    errno = ENOENT;
    cross(libsystem, "_getpid", 0, NULL);
    check("a call that sets no errno leaves the guest's", m32_rd(slot) == 0);
    cross(libsystem, "_close", 1, (uint32_t[]){ 0xffffffffu });
    check("a call that fails sets the guest's errno", m32_rd(slot) == EBADF);
    /* Carbon's structures packed to 2: an AEDesc is {DescType, pointer}, 8 bytes on i386 and 12 on arm64, the
     * pointer at 4 on both ("R{2up}", tools/sdkgen) */
    M32Sig *packed = m32_sig_parse("v(R{2up})", 0);
    check("a packed host structure's layout", packed && packed->arg[0].ssize == 12 && packed->arg[0].moff[1] == 4);
    free(packed);
    const char *cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
    uint32_t desc = DATA + 0x3200, word = DATA + 0x3210;
    m32_wr(word, 0x12345678u);
    memset(m32_h(desc), 0xff, 12);
    int16_t aerr = (int16_t)cross(cs, "_AECreateDesc", 4, (uint32_t[]){ 'type', word, 4, desc });
    check("AECreateDesc fills an i386 AEDesc", aerr == 0 && m32_rd(desc) == 'type' && m32_rd(desc + 4) &&
          m32_rd(desc + 8) == 0xffffffffu);
    check("AEGetDescDataSize reads it", cross(cs, "_AEGetDescDataSize", 1, (uint32_t[]){ desc }) == 4);
    m32_wr(DATA + 0x3214, 0);
    cross(cs, "_AEGetDescData", 3, (uint32_t[]){ desc, DATA + 0x3214, 4 });
    check("AEGetDescData", m32_rd(DATA + 0x3214) == 0x12345678u);
    check("AEDisposeDesc", cross(cs, "_AEDisposeDesc", 1, (uint32_t[]){ desc }) == 0 && m32_rd(desc + 4) == 0);
}

/* An Objective-C 1 constant string, a 12-byte {isa, cString, length} in __OBJC,__cstring_object, stands for a host
 * string, as a __cfstring literal does (ScummVM's Info.plist keys), and the system allocators hand out guest heap
 * (src/m32_cf.c).  In the carbon run, where the bridge has CoreFoundation. */
static void test_objc1_strings(void)
{
    /* an Objective-C 1 constant string stands for a host string */
    static M32Image oimg;
    oimg.nsect = 1;
    snprintf(oimg.sect[0].seg, sizeof oimg.sect[0].seg, "__OBJC");
    snprintf(oimg.sect[0].name, sizeof oimg.sect[0].name, "__cstring_object");
    oimg.sect[0].addr = DATA + 0xa00;
    oimg.sect[0].size = 12;
    put_str(DATA + 0xa40, "CFBundleName");
    m32_wr(DATA + 0xa00, 0x1234); m32_wr(DATA + 0xa04, DATA + 0xa40); m32_wr(DATA + 0xa08, 12);
    m32_cf_alias_strings(&oimg);
    id s = (id)m32_host(DATA + 0xa00);
    const char *u = s && (void *)s != m32_h(DATA + 0xa00)
                  ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
    check("an Objective-C 1 constant string is a host string", u && !strcmp(u, "CFBundleName"));
    /* CFAllocatorAllocate with the default allocator hands out guest heap the program can write (PhysFS's paths) */
    uint32_t p = call("_CFAllocatorAllocate", 3, (uint32_t[]){ 0, 64, 0 });
    check("CFAllocatorAllocate with the default allocator is guest memory", p && p < M32_HANDLE_LO &&
          m32_in_window(m32_h(p)));
    if (p) {
        put_str(p, "/Library/Application Support/");
        uint32_t q = call("_CFAllocatorReallocate", 4, (uint32_t[]){ 0, p, 4096, 0 });
        check("CFAllocatorReallocate keeps the contents in guest memory", q && q < M32_HANDLE_LO &&
              !strcmp((const char *)m32_h(q), "/Library/Application Support/"));
        call("_CFAllocatorDeallocate", 2, (uint32_t[]){ 0, q });
    }
}


/* CGDataProviderCreate's guest callbacks feed a host sequential provider (src/m32_cg.c), as SDL_image's images from
 * an SDL_RWops are read: getBytes hands out "AAAABBBB" once, rewind starts it over */
static void test_data_provider(void)
{
    enum { GET = CODE + 0x3900, NOP = CODE + 0x3940, REWIND = CODE + 0x3950, INFO = DATA + 0x4d00, CBS = DATA + 0x4d10 };
    static const uint8_t get[] = { 0x8b, 0x44, 0x24, 0x04,                   /* mov eax, [esp+4] (info) */
                                   0x83, 0x38, 0x00, 0x75, 0x1d,             /* cmp dword [eax], 0; jne done */
                                   0xc7, 0x00, 0x01, 0x00, 0x00, 0x00,       /* mov dword [eax], 1 */
                                   0x8b, 0x54, 0x24, 0x08,                   /* mov edx, [esp+8] (buffer) */
                                   0xc7, 0x02, 0x41, 0x41, 0x41, 0x41,       /* mov dword [edx], "AAAA" */
                                   0xc7, 0x42, 0x04, 0x42, 0x42, 0x42, 0x42, /* mov dword [edx+4], "BBBB" */
                                   0xb8, 0x08, 0x00, 0x00, 0x00, 0xc3,       /* mov eax, 8; ret */
                                   0x31, 0xc0, 0xc3 };                       /* done: xor eax, eax; ret */
    static const uint8_t rewind[] = { 0x8b, 0x44, 0x24, 0x04, 0xc7, 0x00, 0, 0, 0, 0, 0xc3 };   /* [info] = 0 */
    memcpy(m32_h(GET), get, sizeof get);
    *(uint8_t *)m32_h(NOP) = 0xc3;
    memcpy(m32_h(REWIND), rewind, sizeof rewind);
    m32_wr(INFO, 0);
    const uint32_t cbs[4] = { GET, NOP, REWIND, NOP };
    memcpy(m32_h(CBS), cbs, sizeof cbs);
    cpu()->gpr[OCERZ_RSP] = STACK;
    uint32_t provider = call("_CGDataProviderCreate", 2, (uint32_t[]){ INFO, CBS });
    check("CGDataProviderCreate over guest callbacks", provider != 0);
    const void *(*copy)(const void *) = (const void *(*)(const void *))dlsym(RTLD_DEFAULT, "CGDataProviderCopyData");
    long (*length)(const void *) = (long (*)(const void *))dlsym(RTLD_DEFAULT, "CFDataGetLength");
    const uint8_t *(*bytes)(const void *) = (const uint8_t *(*)(const void *))dlsym(RTLD_DEFAULT, "CFDataGetBytePtr");
    if (!provider || !copy || !length || !bytes)
        return;
    cpu()->gpr[OCERZ_RSP] = STACK;
    const void *data = copy(m32_host(provider));
    check("... the host reads the guest's bytes through it", data && length(data) == 8 && !memcmp(bytes(data), "AAAABBBB", 8));
}

/* CGDisplayBestModeForParameters chooses among the display's modes as Mac OS X did, where today's answers some large
 * mode, never exact (src/m32_cg.c) */
static void test_display_modes(void)
{
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    uint32_t (*main_display)(void) = cg ? (uint32_t (*)(void))dlsym(cg, "CGMainDisplayID") : NULL;
    const void *(*current)(uint32_t) = cg ? (const void *(*)(uint32_t))dlsym(cg, "CGDisplayCurrentMode") : NULL;
    const void *(*get)(const void *, const void *) = (const void *(*)(const void *, const void *))dlsym(RTLD_DEFAULT, "CFDictionaryGetValue");
    int (*number)(const void *, long, void *) = (int (*)(const void *, long, void *))dlsym(RTLD_DEFAULT, "CFNumberGetValue");
    const void *(*string)(const void *, const char *, uint32_t) =
        (const void *(*)(const void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    if (!main_display || !current || !get || !number || !string)
        return;
    uint32_t display = main_display();
    const void *mode = current(display);
    if (!mode)
        return;   /* no display */
    const void *kw = string(NULL, "Width", 0x600), *kh = string(NULL, "Height", 0x600), *kb = string(NULL, "BitsPerPixel", 0x600);
    long w = 0, h = 0, b = 0;
    number(get(mode, kw), 9 /* kCFNumberLongType */, &w);
    number(get(mode, kh), 9, &h);
    number(get(mode, kb), 9, &b);
    uint32_t exact = DATA + 0x4c00;
    m32_wr(exact, 7);
    uint32_t got = call("_CGDisplayBestModeForParameters", 5, (uint32_t[]){ display, (uint32_t)b, (uint32_t)w, (uint32_t)h, exact });
    long gw = 0, gh = 0;
    if (got) {
        number(get(m32_host(got), kw), 9, &gw);
        number(get(m32_host(got), kh), 9, &gh);
    }
    check("the current mode's size and depth are an exact match", got && m32_rd(exact) == 1 && gw == w && gh == h);
    got = call("_CGDisplayBestModeForParameters", 5, (uint32_t[]){ display, (uint32_t)b, 1, 1, exact });
    gw = 0;
    if (got)
        number(get(m32_host(got), kw), 9, &gw);
    check("... a smaller size answers a mode at least that large, not exact", got && m32_rd(exact) == 0 && gw >= 1);
    got = call("_CGDisplayBestModeForParameters", 5, (uint32_t[]){ display, (uint32_t)b, 100000, 100000, exact });
    check("... and a size no mode has, the current mode", got && m32_host(got) == mode && m32_rd(exact) == 0);
}

/* an asynchronous signal's guest handler runs at the guest's next crossing (src/m32_libsystem.c): signal(), and
 * sigaction() with SA_SIGINFO and SA_RESETHAND */
static void test_signals(void)
{
    enum { HANDLER = CODE + 0x3800, SEEN = DATA + 0x4b00, SIGNO = DATA + 0x4b04, ACT = DATA + 0x4b10, OLD = DATA + 0x4b20 };
    uint8_t code[] = { 0x8b, 0x44, 0x24, 0x04,          /* mov eax, [esp+4] (sig) */
                       0xa3, 0, 0, 0, 0,                /* mov [SEEN], eax */
                       0x8b, 0x44, 0x24, 0x08,          /* mov eax, [esp+8] (siginfo) */
                       0x85, 0xc0, 0x74, 0x07,          /* test eax, eax; jz done */
                       0x8b, 0x00,                      /* mov eax, [eax] (si_signo) */
                       0xa3, 0, 0, 0, 0,                /* mov [SIGNO], eax */
                       0xc3 };                          /* done: ret */
    uint32_t seen = SEEN, signo = SIGNO;
    memcpy(code + 5, &seen, 4);
    memcpy(code + 20, &signo, 4);
    memcpy(m32_h(HANDLER), code, sizeof code);
    m32_wr(SEEN, 0);
    m32_wr(SIGNO, 0);
    check("signal() hands back SIG_DFL", call("_signal", 2, (uint32_t[]){ SIGUSR2, HANDLER }) == 0);
    kill(getpid(), SIGUSR2);
    check("the host catches the signal for the guest", (m32_sig_pending & (1u << SIGUSR2)) != 0);
    cpu()->gpr[OCERZ_RSP] = STACK;
    m32_signals_deliver(&g_vm);
    check("... and the guest's handler gets it", m32_rd(SEEN) == SIGUSR2 && !(m32_sig_pending & (1u << SIGUSR2)));

    m32_wr(SEEN, 0);
    kill(getpid(), SIGUSR2);
    uint32_t trap = m32_export("/usr/lib/libSystem.B.dylib", "_signal");
    m32_wr(STACK, RET_ADDR);
    m32_wr(STACK + 4, SIGUSR2);
    m32_wr(STACK + 8, HANDLER);
    cpu()->gpr[OCERZ_RSP] = STACK;
    cpu()->rip = trap;
    m32_trap(&g_vm, cpu());
    check("a crossing runs a caught signal's handler first", m32_rd(SEEN) == SIGUSR2 && cpu()->rip == RET_ADDR);
    call("_signal", 2, (uint32_t[]){ SIGUSR2, 0 });

    m32_wr(SEEN, 0);
    m32_wr(ACT, HANDLER);
    m32_wr(ACT + 4, 0);
    m32_wr(ACT + 8, SA_SIGINFO | SA_RESETHAND);
    check("sigaction", call("_sigaction", 3, (uint32_t[]){ SIGUSR1, ACT, 0 }) == 0);
    kill(getpid(), SIGUSR1);
    cpu()->gpr[OCERZ_RSP] = STACK;
    m32_signals_deliver(&g_vm);
    check("an SA_SIGINFO handler gets the signal and its siginfo", m32_rd(SEEN) == SIGUSR1 && m32_rd(SIGNO) == SIGUSR1);
    m32_wr(OLD, 0xdeadbeefu);
    call("_sigaction", 3, (uint32_t[]){ SIGUSR1, 0, OLD });
    check("... and SA_RESETHAND puts SIG_DFL back", m32_rd(OLD) == 0);
}

/* iret in 32-bit code to its own privilege level pops EIP, CS and EFLAGS and leaves ESP just above them, as Wine's i386
 * set_full_cpu_context expects (a zero CS keeps the current selector) */
static void test_iret32(void)
{
    enum { IRET = CODE + 0x3b80 };
    uint8_t c[] = { 0x89, 0xe1,                         /* mov ecx, esp */
                    0x9c, 0x6a, 0x00,                   /* pushfd; push 0 (CS: keep) */
                    0x68, 0, 0, 0, 0,                   /* push target */
                    0xcf,                               /* iretd */
                    0x89, 0xe0, 0x29, 0xc8, 0xc3 };     /* target: mov eax, esp; sub eax, ecx; ret */
    uint32_t target = IRET + 11;
    memcpy(c + 6, &target, 4);
    memcpy(m32_h(IRET), c, sizeof c);
    cpu()->gpr[OCERZ_RSP] = STACK;
    check("iretd in 32-bit code pops three words, not five", m32_call(&g_vm, IRET, NULL, 0, NULL, NULL) == 0);
}

/* A thread runs on the guest stack its attributes name (pthread_attr_setstack, as Wine gives each Windows thread its
 * own), or on one as large as they ask for (src/m32_thread.c) */
static void test_thread_stacks(void)
{
    enum { RECORD = CODE + 0x3c00, PROBE = CODE + 0x3c40, ATTR = DATA + 0x5000, OUT = DATA + 0x5040,
           ESP = DATA + 0x5044, GOT = DATA + 0x5048, STACKS = 0x01000000u };
    static const uint8_t record[] = { 0x8b, 0x44, 0x24, 0x04, 0x89, 0x20, 0x31, 0xc0, 0xc3 };   /* [arg] = esp; return 0 */
    static const uint8_t probe[] = { 0x8b, 0x44, 0x24, 0x04, 0x89, 0x20,          /* [arg] = esp */
                                     0x81, 0xec, 0x00, 0x00, 0x30, 0x00,          /* sub esp, 3 MB */
                                     0xc7, 0x04, 0x24, 1, 0, 0, 0,                /* mov dword [esp], 1: faults on 2 MB */
                                     0x81, 0xc4, 0x00, 0x00, 0x30, 0x00,          /* add esp, 3 MB */
                                     0x31, 0xc0, 0xc3 };
    memcpy(m32_h(RECORD), record, sizeof record);
    memcpy(m32_h(PROBE), probe, sizeof probe);
    if (ocerz_map_fixed(STACKS, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK)
        return;

    call("_pthread_attr_init", 1, (uint32_t[]){ ATTR });
    check("pthread_attr_setstack takes a page-aligned stack", call("_pthread_attr_setstack", 3, (uint32_t[]){ ATTR, STACKS, 0x10000 }) == 0);
    check("... and refuses one smaller than PTHREAD_STACK_MIN", call("_pthread_attr_setstack", 3, (uint32_t[]){ ATTR, STACKS, 0x1000 }) == EINVAL);
    m32_wr(GOT, 0);
    m32_wr(GOT + 4, 0);
    call("_pthread_attr_getstack", 3, (uint32_t[]){ ATTR, GOT, GOT + 4 });
    check("... and pthread_attr_getstack gives it back", m32_rd(GOT) == STACKS && m32_rd(GOT + 4) == 0x10000);
    m32_wr(GOT, 0);
    call("_pthread_attr_getstackaddr", 2, (uint32_t[]){ ATTR, GOT });
    check("... its top as the stack address, as Darwin keeps it", m32_rd(GOT) == STACKS + 0x10000);
    m32_wr(ESP, 0);
    check("pthread_create on that stack", call("_pthread_create", 4, (uint32_t[]){ OUT, ATTR, RECORD, ESP }) == 0);
    call("_pthread_join", 2, (uint32_t[]){ m32_rd(OUT), 0 });
    check("... runs the thread on it", m32_rd(ESP) > STACKS && m32_rd(ESP) <= STACKS + 0x10000);

    call("_pthread_attr_init", 1, (uint32_t[]){ ATTR });
    call("_pthread_attr_setstacksize", 2, (uint32_t[]){ ATTR, 4u << 20 });
    m32_wr(ESP, 0);
    check("a thread asking for a 4 MB stack", call("_pthread_create", 4, (uint32_t[]){ OUT, ATTR, PROBE, ESP }) == 0);
    call("_pthread_join", 2, (uint32_t[]){ m32_rd(OUT), 0 });
    check("... can use 3 MB of it", m32_rd(ESP) != 0);
    ocerz_unmap(STACKS, 0x10000);
}

/* A fault in i386 code reaches the guest's own handler in i386's frame, and the context the handler changes is the one
 * the code goes on with (src/m32_libsystem.c), end to end through ocerz's host fault handler: a load from page zero
 * (SIGSEGV) and a division by zero (SIGFPE), each skipped by the handler, which also sets eax.  Last, since it installs
 * the host handlers. */
static void test_fault_handlers(void)
{
    enum { HANDLER = CODE + 0x3a00, LOADER = CODE + 0x3a80, DIVIDER = CODE + 0x3ac0,
           SEEN = DATA + 0x4e00, ADDR = DATA + 0x4e04, AFTER = DATA + 0x4e08, EBX = DATA + 0x4e0c, SKIP = DATA + 0x4e10,
           ACT = DATA + 0x4e20 };
    uint8_t h[] = { 0x8b, 0x44, 0x24, 0x04, 0xa3, 0, 0, 0, 0,          /* mov eax, [esp+4]; mov [SEEN], eax */
                    0x8b, 0x44, 0x24, 0x08, 0x8b, 0x40, 0x18,          /* mov eax, [esp+8]; mov eax, [eax+24] (si_addr) */
                    0xa3, 0, 0, 0, 0,                                  /* mov [ADDR], eax */
                    0x8b, 0x44, 0x24, 0x0c, 0x8b, 0x40, 0x1c,          /* mov eax, [esp+12]; mov eax, [eax+28] (mcontext) */
                    0x8b, 0x0d, 0, 0, 0, 0, 0x01, 0x48, 0x34,          /* mov ecx, [SKIP]; add [eax+52], ecx (ss.eip) */
                    0xc7, 0x40, 0x0c, 0x77, 0, 0, 0,                   /* mov dword [eax+12], 0x77 (ss.eax) */
                    0xc3 };
    uint32_t seen = SEEN, addr = ADDR, skip = SKIP, after = AFTER, ebx = EBX;
    memcpy(h + 5, &seen, 4);
    memcpy(h + 17, &addr, 4);
    memcpy(h + 30, &skip, 4);
    memcpy(m32_h(HANDLER), h, sizeof h);
    uint8_t load[] = { 0xbb, 0x11, 0x11, 0x11, 0x11,                   /* mov ebx, 0x11111111 */
                       0xa1, 0x10, 0, 0, 0,                            /* mov eax, [0x10]: faults */
                       0xc7, 0x05, 0, 0, 0, 0, 0x22, 0x22, 0x22, 0x22, /* mov dword [AFTER], 0x22222222 */
                       0x89, 0x1d, 0, 0, 0, 0, 0xc3 };                 /* mov [EBX], ebx; ret */
    memcpy(load + 12, &after, 4);
    memcpy(load + 22, &ebx, 4);
    memcpy(m32_h(LOADER), load, sizeof load);
    static const uint8_t divide[] = { 0x31, 0xd2, 0xb8, 5, 0, 0, 0, 0x31, 0xc9,   /* xor edx,edx; mov eax,5; xor ecx,ecx */
                                      0xf7, 0xf1, 0xc3 };                        /* div ecx: faults; ret */
    memcpy(m32_h(DIVIDER), divide, sizeof divide);

    ocerz_vm_install_handlers(&g_vm);
    int was_active = m32_active;
    m32_active = 1;   /* the handler's return is a trap, which reaches m32 as under m32_run */
    m32_wr(ACT, HANDLER);
    m32_wr(ACT + 4, 0);
    m32_wr(ACT + 8, SA_SIGINFO);
    call("_sigaction", 3, (uint32_t[]){ SIGSEGV, ACT, 0 });
    call("_sigaction", 3, (uint32_t[]){ SIGFPE, ACT, 0 });

    m32_wr(SKIP, 5);
    cpu()->gpr[OCERZ_RSP] = STACK;
    uint32_t eax = m32_call(&g_vm, LOADER, NULL, 0, NULL, NULL);
    check("a load from page zero reaches the guest's SIGSEGV handler with its address",
          m32_rd(SEEN) == SIGSEGV && m32_rd(ADDR) == 0x10);
    check("... and goes on after it with the context the handler changed",
          eax == 0x77 && m32_rd(AFTER) == 0x22222222 && m32_rd(EBX) == 0x11111111);

    m32_wr(SEEN, 0);
    m32_wr(SKIP, 2);
    cpu()->gpr[OCERZ_RSP] = STACK;
    eax = m32_call(&g_vm, DIVIDER, NULL, 0, NULL, NULL);
    check("a division by zero reaches the guest's SIGFPE handler, and goes on", m32_rd(SEEN) == SIGFPE && eax == 0x77);
    call("_sigaction", 3, (uint32_t[]){ SIGSEGV, 0, 0 });
    m32_active = was_active;
}

/* Apple's file streams on the runtime's GCC libstdc++ (src/m32_fstream.c): an ofstream and an fstream at Apple's
 * offsets, opened, written through the library's operator<< (whose sentry finds the basic_ios by the vtable, where
 * Armagetron Advanced's config save went wrong), closed and destroyed through the vtable */
static uint32_t guest_call(uint32_t fn, int n, const uint32_t *args)
{
    cpu()->gpr[OCERZ_RSP] = STACK;
    return m32_call(&g_vm, fn, args, n, NULL, NULL);
}

static uint32_t cxx_sym(uint32_t lib, const char *name)
{
    put_str(DATA + 0x4700, name);
    uint32_t v = call("_dlsym", 2, (uint32_t[]){ lib, DATA + 0x4700 });
    check(name, v != 0);
    return v;
}

static void test_fstreams(void)
{
    char lib[PATH_MAX], path[] = "/tmp/test_m32_fstream.XXXXXX";
    if (!realpath("runtime/guest32/usr/lib/libstdc++.6.dylib", lib)) {
        printf("test_m32: no runtime/guest32 libstdc++ (tools/fetch_guest32_cxx.sh): file streams not checked\n");
        return;
    }
    int was_active = m32_active;
    m32_active = 1;   /* guest code's calls of traps reach m32, as under m32_run */
    if (!m32_argc_var) {   /* what dlopen hands the initializers, which m32_run sets up */
        m32_argc_var = DATA + 0x4a00;
        m32_argv_var = DATA + 0x4a04;
        m32_environ_var = DATA + 0x4a08;
    }
    put_str(DATA + 0x4600, "/usr/lib/libstdc++.6.dylib");
    uint32_t h = call("_dlopen", 2, (uint32_t[]){ DATA + 0x4600, 2 });
    check("dlopen of the runtime's libstdc++", h != 0);
    int fd = mkstemp(path);
    if (!h || fd < 0) {
        m32_active = was_active;
        return;
    }
    close(fd);
    static const struct { const char *tv; uint32_t ios; } tvs[] = {
        { "_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE", 0x20c }, { "_ZTVSt14basic_ofstreamIwSt11char_traitsIwEE", 0x210 },
        { "_ZTVSt13basic_fstreamIcSt11char_traitsIcEE", 0x214 }, { "_ZTVSt13basic_fstreamIwSt11char_traitsIwEE", 0x218 },
        { "_ZTVSt14basic_ifstreamIcSt11char_traitsIcEE", 0x20c },
    };
    for (size_t i = 0; i < sizeof tvs / sizeof tvs[0]; i++)
        check("a file stream's vtable has Apple's basic_ios offset", m32_rd(cxx_sym(h, tvs[i].tv)) == tvs[i].ios);

    uint32_t os = m32_calloc(1, 0x300), fs = m32_calloc(1, 0x300), bad = m32_calloc(1, 0x300);
    put_str(DATA + 0x4800, path);
    put_str(DATA + 0x4900, "hello ");
    put_str(DATA + 0x4910, "/nonexistent-dir/test_m32/x");
    guest_call(cxx_sym(h, "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode"), 3,
               (uint32_t[]){ os, DATA + 0x4800, 16 });
    check("an ofstream from a file name: its filebuf at 8, its basic_ios at 0x20c, good",
          m32_rd(os + 0x20c + 0x78) == os + 8 && m32_rd(m32_rd(os) - 12) == 0x20c && m32_rd(os + 0x20c + 0x14) == 0);
    check("... is_open", (guest_call(cxx_sym(h, "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv"), 1,
                                     (uint32_t[]){ os }) & 0xff) == 1);
    check("... rdbuf", guest_call(cxx_sym(h, "_ZNKSt14basic_ofstreamIcSt11char_traitsIcEE5rdbufEv"), 1,
                                  (uint32_t[]){ os }) == os + 8);
    uint32_t put = cxx_sym(h, "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc");
    check("operator<< writes through the library's sentry",
          guest_call(put, 2, (uint32_t[]){ os, DATA + 0x4900 }) == os && m32_rd(os + 0x20c + 0x14) == 0);
    guest_call(cxx_sym(h, "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv"), 1, (uint32_t[]){ os });
    check("... close leaves it good and closed", m32_rd(os + 0x20c + 0x14) == 0 &&
          (guest_call(cxx_sym(h, "_ZNKSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv"), 1, (uint32_t[]){ os }) & 0xff) == 0);
    guest_call(m32_rd(m32_rd(os)), 1, (uint32_t[]){ os });   /* its complete destructor, from the vtable */
    check("... its destructor runs down to ios_base's, at Apple's offset", m32_rd(os + 0x20c) ==
          cxx_sym(h, "_ZTVSt8ios_base") + 8);

    /* an fstream built by its default constructor and opened afterwards: in | out | app */
    guest_call(cxx_sym(h, "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev"), 1, (uint32_t[]){ fs });
    guest_call(cxx_sym(h, "_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode"), 3,
               (uint32_t[]){ fs, DATA + 0x4800, 8 | 16 | 1 });
    check("an fstream: its filebuf at 16, its basic_ios at 0x214, open",
          m32_rd(fs + 0x214 + 0x78) == fs + 16 && m32_rd(m32_rd(fs) - 12) == 0x214 && m32_rd(fs + 0x214 + 0x14) == 0);
    check("... its ostream's vtable finds the same basic_ios", m32_rd(m32_rd(fs + 8) - 12) == 0x20c);
    guest_call(put, 2, (uint32_t[]){ fs + 8, DATA + 0x4900 });
    guest_call(m32_rd(m32_rd(fs) + 4), 1, (uint32_t[]){ fs });   /* deleting destructor: closes, frees */

    guest_call(cxx_sym(h, "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode"), 3,
               (uint32_t[]){ bad, DATA + 0x4910, 16 });
    check("an ofstream that cannot open is failed", (m32_rd(bad + 0x20c + 0x14) & 4) != 0);
    guest_call(m32_rd(m32_rd(bad)), 1, (uint32_t[]){ bad });

    char got[32] = { 0 };
    FILE *f = fopen(path, "r");
    if (f) {
        fread(got, 1, sizeof got - 1, f);
        fclose(f);
    }
    check("the file has what both streams wrote", !strcmp(got, "hello hello "));
    unlink(path);
    m32_free(os);
    m32_free(bad);
    m32_active = was_active;
}

/* What WineHQ's 32-bit Wine and the free games ScummVM, DOSBox and Armagetron Advanced needed (src/m32_libsystem.c,
 * src/m32_dyld.c, src/m32_selfload.c, src/m32_cf.c): the preloader's dyld calls and raw mincore, mmap hints, the
 * calls whose i386 arrays the database converted one element of (kevent, iovecs), getopt_long, the LDT,
 * getsegmentdata over a 32-bit header, ___dso_handle, Foundation's memory pages and Objective-C 1 constant strings */
static void test_unix_calls(void)
{
    put_str(DATA + 0x100, "__dyld_dlopen");
    uint32_t lk[2] = { DATA + 0x100, DATA + 0x200 };
    check("_dyld_func_lookup answers the preloader's __dyld_dlopen", call("__dyld_func_lookup", 2, lk) == 1 &&
                                                                     m32_rd(DATA + 0x200) != 0);
    /* mincore: a mapped page is in core, a range with no mapping is ENOMEM */
    uint32_t in = sysenter(78, 3, (uint32_t[]){ DATA, 0x1000, DATA + 0x300 });
    check("raw mincore of a mapped page", !carry() && in == 0 && *(uint8_t *)m32_h(DATA + 0x300) == 1);
    sysenter(78, 3, (uint32_t[]){ 0x31000000u, 0x2000, DATA + 0x300 });
    check("raw mincore of a free range fails", carry() && cpu()->gpr[OCERZ_RAX] == ENOMEM);
    /* an mmap hint where the range is free is taken */
    uint32_t got = call("_mmap", 7, (uint32_t[]){ 0x31000000u, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                                                   0xffffffffu, 0, 0 });
    check("an mmap hint over a free range is taken", got == 0x31000000u);
    sysenter(78, 3, (uint32_t[]){ 0x31000000u, 0x1000, DATA + 0x300 });
    check("... and mincore sees the mapping", !carry());
    call("_munmap", 2, (uint32_t[]){ 0x31000000u, 0x4000 });
    /* readv and writev over two i386 iovecs */
    int fds[2];
    if (pipe(fds) == 0) {
        put_str(DATA + 0x400, "ab");
        put_str(DATA + 0x410, "cde");
        uint32_t iov = DATA + 0x420;
        m32_wr(iov, DATA + 0x400); m32_wr(iov + 4, 2); m32_wr(iov + 8, DATA + 0x410); m32_wr(iov + 12, 3);
        check("writev of two i386 iovecs", call("_writev", 3, (uint32_t[]){ (uint32_t)fds[1], iov, 2 }) == 5);
        memset(m32_h(DATA + 0x440), 0, 16);
        m32_wr(iov, DATA + 0x440); m32_wr(iov + 4, 3); m32_wr(iov + 8, DATA + 0x450); m32_wr(iov + 12, 2);
        check("readv into two i386 iovecs", call("_readv", 3, (uint32_t[]){ (uint32_t)fds[0], iov, 2 }) == 5 &&
              !memcmp(m32_h(DATA + 0x440), "abc", 3) && !memcmp(m32_h(DATA + 0x450), "de", 2));
        /* kevent: two changes in, an event out, with a zero timeout */
        int kq = kqueue(), other[2];
        if (kq >= 0 && pipe(other) == 0) {
            write(fds[1], "xyz", 3);
            uint32_t ch = DATA + 0x500, ev = DATA + 0x540, ts = DATA + 0x5a0;
            memset(m32_h(ch), 0, 0x40);
            uint16_t add = EV_ADD;
            int16_t rd = EVFILT_READ;
            uint32_t ids[2] = { (uint32_t)fds[0], (uint32_t)other[0] }, ud[2] = { 0x1111, 0x2222 };
            for (int i = 0; i < 2; i++) {
                uint8_t *g = m32_h(ch + 20 * (uint32_t)i);
                memcpy(g, &ids[i], 4); memcpy(g + 4, &rd, 2); memcpy(g + 6, &add, 2); memcpy(g + 16, &ud[i], 4);
            }
            m32_wr(ts, 0); m32_wr(ts + 4, 0);
            uint32_t n = call("_kevent", 6, (uint32_t[]){ (uint32_t)kq, ch, 2, ev, 4, ts });
            const uint8_t *e = m32_h(ev);
            uint32_t ident, data, udata;
            memcpy(&ident, e, 4); memcpy(&data, e + 12, 4); memcpy(&udata, e + 16, 4);
            check("kevent takes two i386 changes and answers the readable one", n == 1 && ident == (uint32_t)fds[0] &&
                  data == 3 && udata == 0x1111);
            close(other[0]);
            close(other[1]);
        }
        if (kq >= 0)
            close(kq);
        close(fds[0]);
        close(fds[1]);
    }
    /* sendmsg and recvmsg with i386 msghdrs */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        uint32_t iov = DATA + 0x600, msg = DATA + 0x620;
        put_str(DATA + 0x640, "hi");
        put_str(DATA + 0x650, "there");
        m32_wr(iov, DATA + 0x640); m32_wr(iov + 4, 2); m32_wr(iov + 8, DATA + 0x650); m32_wr(iov + 12, 5);
        memset(m32_h(msg), 0, 28);
        m32_wr(msg + 8, iov); m32_wr(msg + 12, 2);
        check("sendmsg of an i386 msghdr", call("_sendmsg", 3, (uint32_t[]){ (uint32_t)sv[0], msg, 0 }) == 7);
        memset(m32_h(DATA + 0x660), 0, 16);
        m32_wr(iov, DATA + 0x660); m32_wr(iov + 4, 16);
        memset(m32_h(msg), 0, 28);
        m32_wr(msg + 8, iov); m32_wr(msg + 12, 1); m32_wr(msg + 24, 0xffffffffu);
        check("recvmsg into an i386 msghdr", call("_recvmsg", 3, (uint32_t[]){ (uint32_t)sv[1], msg, 0 }) == 7 &&
              !memcmp(m32_h(DATA + 0x660), "hithere", 7) && m32_rd(msg + 24) == 0);
        close(sv[0]);
        close(sv[1]);
    }
    /* getopt_long over a guest argv and a 16-byte i386 struct option */
    put_str(DATA + 0x700, "prog");
    put_str(DATA + 0x708, "-a");
    put_str(DATA + 0x710, "--foreground");
    put_str(DATA + 0x720, "ab:");
    put_str(DATA + 0x728, "foreground");
    uint32_t argv = DATA + 0x740, lo = DATA + 0x760;
    m32_wr(argv, DATA + 0x700); m32_wr(argv + 4, DATA + 0x708); m32_wr(argv + 8, DATA + 0x710); m32_wr(argv + 12, 0);
    memset(m32_h(lo), 0, 32);
    m32_wr(lo, DATA + 0x728); m32_wr(lo + 12, 'f');
    optreset = 1;
    optind = 1;
    uint32_t ga[5] = { 3, argv, DATA + 0x720, lo, 0 };
    uint32_t r1 = call("_getopt_long", 5, ga), r2 = call("_getopt_long", 5, ga), r3 = call("_getopt_long", 5, ga);
    check("getopt_long reads a guest argv and long options", r1 == 'a' && r2 == 'f' && r3 == 0xffffffffu);
    /* i386_set_ldt with LDT_AUTO_ALLOC: an index whose selector has the descriptor's base */
    uint64_t d = 0x0fffull | (0x45000ull << 16) | (0x12ull << 56) | (0xf2ull << 40) | (0x4ull << 52);
    memcpy(m32_h(DATA + 0x800), &d, 8);
    uint32_t idx = call("_i386_set_ldt", 3, (uint32_t[]){ 0xffffffffu, DATA + 0x800, 1 });
    check("i386_set_ldt allocates an entry", idx > 0 && idx < 8192 && ocerz_ldt_base((idx << 3) | 7) == 0x12045000u);
    /* getsegmentdata over a 32-bit header slid to DATA + 0x6000 */
    uint32_t mh = DATA + 0x6000;
    memset(m32_h(mh), 0, 0x200);
    m32_wr(mh, 0xfeedface); m32_wr(mh + 4, 7); m32_wr(mh + 12, 2); m32_wr(mh + 16, 2); m32_wr(mh + 20, 2 * 56);
    uint32_t cmd = mh + 28;
    m32_wr(cmd, LC_SEGMENT); m32_wr(cmd + 4, 56); memcpy(m32_h(cmd + 8), "__TEXT", 7);
    m32_wr(cmd + 24, 0x1000); m32_wr(cmd + 28, 0x2000);
    cmd += 56;
    m32_wr(cmd, LC_SEGMENT); m32_wr(cmd + 4, 56); memcpy(m32_h(cmd + 8), "__DATA", 7);
    m32_wr(cmd + 24, 0x3000); m32_wr(cmd + 28, 0x1000);
    put_str(DATA + 0x6300, "__DATA");
    uint32_t seg = call("_getsegmentdata", 3, (uint32_t[]){ mh, DATA + 0x6300, DATA + 0x6310 });
    check("getsegmentdata walks a 32-bit header", seg == 0x3000 + (mh - 0x1000) && m32_rd(DATA + 0x6310) == 0x1000);
    /* ___dso_handle is the importing image's header */
    static M32Image img;
    img.base = 0x7654000;
    check("___dso_handle resolves to the image's header", m32_resolve(&img, 0, "___dso_handle", 0) == 0x7654000);
    /* Foundation's memory pages are window pages */
    uint32_t pages = call("_NSAllocateMemoryPages", 1, (uint32_t[]){ 0x5000 });
    check("NSAllocateMemoryPages hands out zeroed window pages", pages && !(pages & 0xfff) && pages < M32_HANDLE_LO &&
          m32_rd(pages) == 0 && m32_rd(pages + 0x4ffc) == 0);
    put_str(DATA + 0x900, "copied");
    call("_NSCopyMemoryPages", 3, (uint32_t[]){ DATA + 0x900, pages, 7 });
    check("NSCopyMemoryPages copies in the window", !strcmp((const char *)m32_h(pages), "copied"));
    call("_NSDeallocateMemoryPages", 2, (uint32_t[]){ pages, 0x5000 });
}

int main(int argc, char **argv)
{
    if (ocerz_mem_init(0, M32_WINDOW) != OCERZ_OK ||
        ocerz_map_fixed(STACK - 0x10000, 0x10000 + 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(DATA, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(CODE, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC) != OCERZ_OK ||
        ocerz_map_fixed(M32_HANDLE_LO, M32_HANDLE_HI - M32_HANDLE_LO, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "test_m32: cannot set up the window\n");
        return 2;
    }
    char g32[PATH_MAX];
    if (!getenv("OCERZ_GUEST32_ROOT") && realpath("runtime/guest32", g32))
        setenv("OCERZ_GUEST32_ROOT", g32, 1);   /* the i386 runtime, as ocerz finds it beside itself */
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;
    ocerz_cpu_reset(cpu());
    cpu()->mode32 = 1;
    m32_heap_init();
    if (argc > 1 && !strcmp(argv[1], "carbon")) {
        char db[PATH_MAX];
        if (!realpath("runtime/apis32", db)) {
            fprintf(stderr, "test_m32: no runtime/apis32 here (make apis32, run from the repository)\n");
            return 2;
        }
        setenv("OCERZ_APIDB32", db, 0);   /* unless the caller named one */
        char db64[PATH_MAX];
        if (realpath("runtime/apis", db64))
            setenv("OCERZ_APIDB", db64, 0);   /* the host side: frameworks the bridge opens by install name */
        test_carbon();
        test_db_shapes();
        test_objc_pairs();
        test_objc1_strings();
        test_fstreams();
        test_display_modes();
        test_data_provider();
        printf("test_m32 carbon: %d checks, %d failed\n", g_checks, g_failures);
        return g_failures != 0;
    }
    test_keymgr();
    test_dyld_func_lookup();
    test_wide_conversions();
    test_nslogv();
    test_raw_syscalls();
    test_quickdraw();
    test_fonts();
    test_tsearch();
    test_positional();
    test_map_table();
    test_font_tables();
    test_sF();
    test_cg();
    test_removed_calls();
    test_sdl_shims();
    test_game_libs();
    test_image_order();
    test_queues();
    test_gl_buffers();
    test_unix_calls();
    test_signals();
    test_iret32();
    test_thread_stacks();
    test_fault_handlers();
    printf("test_m32: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
