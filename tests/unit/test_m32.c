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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
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

/* menus and controls arm64 HIToolbox dropped (src/m32_controls.c) */
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

int main(int argc, char **argv)
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
    m32_heap_init();
    if (argc > 1 && !strcmp(argv[1], "carbon")) {
        char db[PATH_MAX];
        if (!realpath("runtime/apis32", db)) {
            fprintf(stderr, "test_m32: no runtime/apis32 here (make apis32, run from the repository)\n");
            return 2;
        }
        setenv("OCERZ_APIDB32", db, 0);   /* unless the caller named one */
        test_carbon();
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
    printf("test_m32: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
