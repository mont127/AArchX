/*
 * Apple's layout of std::ofstream and std::fstream, on the runtime's GCC libstdc++ (include/ocerz/m32_image.h).
 *
 * Programs built for i386 with Apple's GCC 4 put a file stream's basic_filebuf on an 8-byte boundary, as Apple's
 * libstdc++.6.dylib did: an ofstream's filebuf at 8 and its basic_ios virtual base at 0x20c, where the GCC 15
 * libstdc++ in runtime/guest32 (tools/fetch_guest32_cxx.sh) has them at 4 and 0x208, as clang lays them out too.  An
 * ifstream comes out the same either way, its istream's gcount word ending at 8.  Such a program builds a stream
 * with inline code at its own offsets and then reaches the basic_ios through the library's vtable: Armagetron
 * Advanced's config save found its ios 4 bytes early, took the locale there for the tied stream and called flush
 * through it.
 *
 * So the library gets Apple's layout for basic_ofstream and basic_fstream, of char and of wchar_t.  Once it is bound,
 * the offsets in their vtables and construction vtables (the virtual base offset, and the ios's offset to top and
 * vcall offset), found through their VTTs, move by 4.  The members that use the filebuf's or the ios's offset
 * directly are answered here: the constructors (default and from a file name), the destructors (the library's D0
 * and thunks reach D1 through their stubs), open, close, is_open and rdbuf.  The work itself is the library's
 * (basic_ios, basic_ostream or basic_iostream, basic_filebuf), called with m32_call.  A failed open or close sets the
 * state as basic_ios::clear does and, when exceptions() asks for it, continues in __throw_ios_failure in place of
 * the call, so the exception unwinds straight into the program.  The C++11 members (move, swap, the string
 * overloads) keep GCC 15's code: nothing built for Apple's library calls them.  OCERZ_NO_M32_APPLE_FSTREAM=1 leaves
 * the library as it is.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"

#define IOS_EXCEPTIONS 0x10   /* ios_base::_M_exception */
#define IOS_STATE      0x14   /* ios_base::_M_streambuf_state */
#define FAILBIT        4
#define BADBIT         1
#define OPENMODE_OUT   16

enum { OP_C1, OP_C2, OP_C1_NAME, OP_C2_NAME, OP_D1, OP_D2, OP_D4, OP_OPEN, OP_CLOSE, OP_IS_OPEN, OP_IS_OPEN_CONST,
       OP_RDBUF, NOPS };
static const char *const g_ops[NOPS] = {
    "__ZN%sC1Ev", "__ZN%sC2Ev", "__ZN%sC1EPKcSt13_Ios_Openmode", "__ZN%sC2EPKcSt13_Ios_Openmode",
    "__ZN%sD1Ev", "__ZN%sD2Ev", "__ZN%sD4Ev", "__ZN%s4openEPKcSt13_Ios_Openmode", "__ZN%s5closeEv",
    "__ZN%s7is_openEv", "__ZNK%s7is_openEv", "__ZNK%s5rdbufEv",
};

/* the library calls a member makes; '@' is the character type */
enum { H_IOS_C2, H_IOS_D2, H_INIT, H_BASE_C2, H_BASE_D2, H_FB_C1, H_FB_D1, H_FB_OPEN, H_FB_CLOSE, H_FB_IS_OPEN, NHELP };
static const char *const g_helpers[NHELP] = {
    "__ZNSt9basic_iosI@St11char_traitsI@EEC2Ev", "__ZNSt9basic_iosI@St11char_traitsI@EED2Ev",
    "__ZNSt9basic_iosI@St11char_traitsI@EE4initEPSt15basic_streambufI@S1_E", NULL, NULL,
    "__ZNSt13basic_filebufI@St11char_traitsI@EEC1Ev", "__ZNSt13basic_filebufI@St11char_traitsI@EED1Ev",
    "__ZNSt13basic_filebufI@St11char_traitsI@EE4openEPKcSt13_Ios_Openmode",
    "__ZNSt13basic_filebufI@St11char_traitsI@EE5closeEv", "__ZNKSt13basic_filebufI@St11char_traitsI@EE7is_openEv",
};

typedef struct FsClass {
    const char *cls;
    char ch;                     /* 'c' or 'w' */
    int fstream;                 /* an iostream base, its ostream at 8; else an ostream */
    uint32_t ios_gcc;            /* where GCC 15 has the basic_ios; Apple's is 4 further */
    const char *base_c2, *base_d2;
    uint32_t tt, fn[NHELP], trap[NOPS];
} FsClass;

static FsClass g_cls[] = {
    { "St14basic_ofstreamIcSt11char_traitsIcEE", 'c', 0, 0x208, "__ZNSoC2Ev", "__ZNSoD2Ev", 0, { 0 }, { 0 } },
    { "St14basic_ofstreamIwSt11char_traitsIwEE", 'w', 0, 0x20c, "__ZNSt13basic_ostreamIwSt11char_traitsIwEEC2Ev",
      "__ZNSt13basic_ostreamIwSt11char_traitsIwEED2Ev", 0, { 0 }, { 0 } },
    { "St13basic_fstreamIcSt11char_traitsIcEE", 'c', 1, 0x210, "__ZNSdC2Ev", "__ZNSdD2Ev", 0, { 0 }, { 0 } },
    { "St13basic_fstreamIwSt11char_traitsIwEE", 'w', 1, 0x214, "__ZNSt14basic_iostreamIwSt11char_traitsIwEEC2Ev",
      "__ZNSt14basic_iostreamIwSt11char_traitsIwEED2Ev", 0, { 0 }, { 0 } },
};
#define NCLS ((int)(sizeof g_cls / sizeof g_cls[0]))

enum { UNCHECKED, CHECKING, READY, OFF };
static int g_state;
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;   /* check's own lookups come back in */
static uint32_t g_throw, g_throw_what;   /* __throw_ios_failure(const char *), and "basic_ios::clear" */

static uint32_t fb_off(const FsClass *c) { return c->fstream ? 16 : 8; }
static uint32_t ios_off(const FsClass *c) { return c->ios_gcc + 4; }
static uint32_t streambuf_off(const FsClass *c) { return c->ch == 'w' ? 0x7c : 0x78; }   /* basic_ios::_M_streambuf */
static int vtt_ios(const FsClass *c) { return c->fstream ? 8 : 3; }

static uint32_t helper(M32Image *img, const char *pattern, char ch)
{
    char name[160];
    snprintf(name, sizeof name, "%s", pattern);
    for (char *p = name; *p; p++)
        if (*p == '@')
            *p = ch;
    return m32_image_symbol(img, name);
}

/* whether img is the libstdc++ this file knows: each class's vtable has GCC 15's offset, and every call is there */
static const char *check(M32Image *img)
{
    if (getenv("OCERZ_NO_M32_APPLE_FSTREAM"))
        return "OCERZ_NO_M32_APPLE_FSTREAM";
    char name[160];
    for (int i = 0; i < NCLS; i++) {
        FsClass *c = &g_cls[i];
        snprintf(name, sizeof name, "__ZTV%s", c->cls);
        uint32_t tv = m32_image_symbol(img, name);
        snprintf(name, sizeof name, "__ZTT%s", c->cls);
        c->tt = m32_image_symbol(img, name);
        if (!tv || !c->tt || m32_rd(tv) != c->ios_gcc)
            return "a vtable is not GCC 15's";
        for (int h = 0; h < NHELP; h++) {
            c->fn[h] = h == H_BASE_C2 ? m32_image_symbol(img, c->base_c2)
                     : h == H_BASE_D2 ? m32_image_symbol(img, c->base_d2) : helper(img, g_helpers[h], c->ch);
            if (!c->fn[h])
                return "a member it calls is missing";
        }
    }
    g_throw = m32_image_symbol(img, "__ZSt19__throw_ios_failurePKc");
    static const char what[] = "basic_ios::clear";
    g_throw_what = m32_static_alloc(sizeof what, 4);
    memcpy(m32_h(g_throw_what), what, sizeof what);
    return NULL;
}

static int sp_fstream(struct OcerzVM *vm, OcerzCPU *cpu);

/* whether img is checked and found to be the library this file knows, checking it the first time; g_lock held */
static int ready(M32Image *img)
{
    if (g_state == UNCHECKED) {
        g_state = CHECKING;   /* its lookups, back through m32_fstream_symbol, answer nothing meanwhile */
        const char *why = check(img);
        g_state = why ? OFF : READY;
        if (why && strcmp(why, "OCERZ_NO_M32_APPLE_FSTREAM"))
            fprintf(stderr, "ocerz: m32: %s: file streams keep GCC's layout (%s)\n", img->path, why);
    }
    return g_state == READY;
}

/* The trap answering name, when img is the runtime's libstdc++ and name one of the members above; else 0. */
uint32_t m32_fstream_symbol(M32Image *img, const char *name)
{
    if (strncmp(name, "__ZN", 4) || !strstr(name, "fstreamI"))
        return 0;
    uint32_t v = 0;
    char want[160];
    pthread_mutex_lock(&g_lock);
    for (int i = 0; ready(img) && !v && i < NCLS; i++) {
        if (!strstr(name, g_cls[i].cls))
            continue;
        for (int op = 0; op < NOPS; op++) {
            snprintf(want, sizeof want, g_ops[op], g_cls[i].cls);
            if (strcmp(name, want))
                continue;
            if (!g_cls[i].trap[op])
                g_cls[i].trap[op] = m32_export_special(name, sp_fstream);
            v = g_cls[i].trap[op];
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* a vtable's offset word, moved by 4 away from 0 when it reaches the basic_ios (it is ios or ios - 8 from where it
 * is read); each word once, as two VTT entries could share an address point */
static void move_offset(uint32_t at, uint32_t ios)
{
    static uint32_t done[4 * 10 * 2];
    static int ndone;
    for (int i = 0; i < ndone; i++)
        if (done[i] == at)
            return;
    int32_t w = (int32_t)m32_rd(at), lo = (int32_t)ios - 8, hi = (int32_t)ios;
    if (w >= lo && w <= hi)
        w += 4;
    else if (w <= -lo && w >= -hi)
        w -= 4;
    else
        return;
    m32_wr(at, (uint32_t)w);
    if (ndone < (int)(sizeof done / sizeof done[0]))
        done[ndone++] = at;
}

/* Apple's offsets in the vtables of the runtime's libstdc++, once it is bound and before it is protected */
void m32_fstream_fixup(M32Image *img)
{
    if (!img->runtime_cxx)
        return;
    pthread_mutex_lock(&g_lock);
    int ok = ready(img);
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        return;
    for (int i = 0; i < NCLS; i++) {
        const FsClass *c = &g_cls[i];
        for (int k = 0; k < (c->fstream ? 10 : 4); k++) {
            uint32_t point = m32_rd(c->tt + 4 * (uint32_t)k);   /* an address point: its words before it */
            move_offset(point - 12, c->ios_gcc);   /* virtual base offset, or the ios's vcall offset */
            move_offset(point - 8, c->ios_gcc);    /* offset to top */
        }
    }
}

static uint32_t call(struct OcerzVM *vm, uint32_t fn, int n, uint32_t a0, uint32_t a1, uint32_t a2)
{
    const uint32_t args[3] = { a0, a1, a2 };
    return m32_call(vm, fn, args, n, NULL, NULL);
}

/* where self's basic_ios is, by its vtable (the library's, or the construction vtable of a class built on it) */
static uint32_t ios_of(uint32_t self) { return self + m32_rd(m32_rd(self) - 12); }

/* the vptrs of self from vtt: the complete class's VTT (FsClass.tt) or the one a derived class passed */
static void set_vptrs(const FsClass *c, uint32_t self, uint32_t vtt)
{
    uint32_t primary = m32_rd(vtt);
    m32_wr(self, primary);
    m32_wr(self + m32_rd(primary - 12), m32_rd(vtt + 4 * (uint32_t)vtt_ios(c)));
    if (c->fstream)
        m32_wr(self + 8, m32_rd(vtt + 36));   /* the ostream inside the iostream */
}

/* as GCC's constructors: the virtual base when complete, the stream base, the vptrs, the filebuf, init */
static void construct(struct OcerzVM *vm, const FsClass *c, uint32_t self, uint32_t vtt, int complete)
{
    if (complete) {
        vtt = c->tt;
        call(vm, c->fn[H_IOS_C2], 1, self + ios_off(c), 0, 0);
    }
    call(vm, c->fn[H_BASE_C2], 2, self, vtt + 4, 0);
    set_vptrs(c, self, vtt);
    call(vm, c->fn[H_FB_C1], 1, self + fb_off(c), 0, 0);
    call(vm, c->fn[H_INIT], 2, ios_of(self), self + fb_off(c), 0);
}

/* as GCC's destructors: flags bit 1 destroys the virtual base too (D1); vtt is a derived class's for D2 */
static void destroy(struct OcerzVM *vm, const FsClass *c, uint32_t self, uint32_t flags, uint32_t vtt)
{
    if (flags)
        vtt = c->tt;
    set_vptrs(c, self, vtt);
    call(vm, c->fn[H_FB_D1], 1, self + fb_off(c), 0, 0);
    call(vm, c->fn[H_BASE_D2], 2, self, vtt + 4, 0);
    if (flags & 2)
        call(vm, c->fn[H_IOS_D2], 1, self + ios_off(c), 0, 0);
}

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

/* basic_ios::clear(state) on ios, then the return from the trapped call, or the throw clear makes in its place */
static int clear_and_return(OcerzCPU *cpu, const FsClass *c, uint32_t ios, uint32_t state)
{
    if (!m32_rd(ios + streambuf_off(c)))
        state |= BADBIT;
    m32_wr(ios + IOS_STATE, state);
    if (!(m32_rd(ios + IOS_EXCEPTIONS) & state) || !g_throw)
        RET(0);
    m32_wr((uint32_t)cpu->gpr[OCERZ_RSP] + 4, g_throw_what);   /* the call's own first argument slot */
    cpu->rip = g_throw;
    return OCERZ_STEP_OK;
}

static int open_and_return(struct OcerzVM *vm, OcerzCPU *cpu, const FsClass *c, uint32_t self, uint32_t name,
                           uint32_t mode)
{
    uint32_t ok = call(vm, c->fn[H_FB_OPEN], 3, self + fb_off(c), name, mode | (c->fstream ? 0 : OPENMODE_OUT));
    uint32_t ios = ios_of(self);
    return clear_and_return(cpu, c, ios, ok ? 0 : m32_rd(ios + IOS_STATE) | FAILBIT);
}

static int sp_fstream(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t trap = (uint32_t)cpu->rip;
    const FsClass *c = NULL;
    int op = 0;
    for (int i = 0; !c && i < NCLS; i++)
        for (op = 0; op < NOPS; op++)
            if (g_cls[i].trap[op] == trap) {
                c = &g_cls[i];
                break;
            }
    if (!c)
        RET(0);
    uint32_t self = m32_arg(cpu, 0), a1 = m32_arg(cpu, 1);
    switch (op) {
    case OP_C1: construct(vm, c, self, 0, 1); RET(0);
    case OP_C2: construct(vm, c, self, a1, 0); RET(0);
    case OP_C1_NAME:
        construct(vm, c, self, 0, 1);
        return open_and_return(vm, cpu, c, self, a1, m32_arg(cpu, 2));
    case OP_C2_NAME:
        construct(vm, c, self, a1, 0);
        return open_and_return(vm, cpu, c, self, m32_arg(cpu, 2), m32_arg(cpu, 3));
    case OP_D1: destroy(vm, c, self, 2, 0); RET(0);
    case OP_D2: destroy(vm, c, self, 0, a1); RET(0);
    case OP_D4: destroy(vm, c, self, a1, m32_arg(cpu, 2)); RET(0);
    case OP_OPEN: return open_and_return(vm, cpu, c, self, a1, m32_arg(cpu, 2));
    case OP_CLOSE:
        if (call(vm, c->fn[H_FB_CLOSE], 1, self + fb_off(c), 0, 0))
            RET(0);
        return clear_and_return(cpu, c, ios_of(self), m32_rd(ios_of(self) + IOS_STATE) | FAILBIT);
    case OP_IS_OPEN:
    case OP_IS_OPEN_CONST: RET(call(vm, c->fn[H_FB_IS_OPEN], 1, self + fb_off(c), 0, 0) & 0xff);
    case OP_RDBUF: RET(self + fb_off(c));
    }
    RET(0);
}
