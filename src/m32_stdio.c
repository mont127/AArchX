/*
 * m32's variadic stdio (printf and scanf families), the guest heap's entry points and errno
 * (include/ocerz/m32.h).
 *
 * An i386 variadic call leaves its arguments on the stack in order, and an i386 va_list is just a pointer to the
 * next one.  An Apple arm64 va_list is a pointer to 8-byte slots, one per argument.  m32_printf_slots walks a
 * format the way printf will, reads each argument at its i386 size and writes the slot arm64 printf expects: the
 * same format then runs on the host's v-form.  scanf's arguments are pointers the host writes through; where the
 * host would write more than the guest's variable holds (%ld, %zu, %p, %Lf) the slot points at a scratch cell that
 * is narrowed back after the call.
 */
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <wchar.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_objc.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

#define M32_MAX_SLOTS 64

/* ---- errno ---- */

static __thread uint32_t t_errno;   /* the calling thread's guest errno variable */

static uint32_t errno_slot(void)
{
    if (!t_errno)
        t_errno = m32_static_alloc(4, 4);
    return t_errno;
}

void m32_save_errno(OcerzCPU *cpu)
{
    (void)cpu;
    int e = errno;
    m32_wr(errno_slot(), (uint32_t)e);
}

static int sp___error(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_ret(cpu, errno_slot(), 0, 0);
    return OCERZ_STEP_OK;
}

/* ---- printf ---- */

static double x87_ext_to_double(const uint8_t *p)   /* an 80-bit extended value, as i386 passes long double */
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int exp = se & 0x7fff;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (!exp && !mant)
        return 0.0 * sign;
    if (exp == 0x7fff)
        return (mant << 1) ? __builtin_nan("") : sign * __builtin_inf();
    return sign * __builtin_ldexp((double)mant, exp - 16383 - 63);
}

/* returns the number of slots written, or -1 when the format uses something this does not handle (logged) */
int m32_printf_slots(const char *fmt, uint32_t ap, uint64_t *slots, int max, const char *who)
{
    int n = 0;
#define TAKE32() (ap += 4, m32_rd(ap - 4))
#define TAKE64() (ap += 8, (uint64_t)m32_rd(ap - 8) | (uint64_t)m32_rd(ap - 4) << 32)
#define PUT(v) do { if (n >= max) goto too_many; slots[n++] = (uint64_t)(v); } while (0)
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '%')
            continue;
        while (*p && strchr("-+ #0'", *p))
            p++;
        if (*p == '*') {
            PUT((int64_t)(int32_t)TAKE32());
            p++;
        } else {
            while (isdigit((unsigned char)*p))
                p++;
            if (*p == '$') {
                m32_log_once("positional printf arguments are not supported:", who);
                return -1;
            }
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                PUT((int64_t)(int32_t)TAKE32());
                p++;
            } else
                while (isdigit((unsigned char)*p))
                    p++;
        }
        int len = 0;   /* 1 hh, 2 h, 3 l, 4 ll q j, 5 z t, 6 L */
        for (;; p++) {
            if (*p == 'h') len = len == 2 ? 1 : 2;
            else if (*p == 'l') len = len == 3 ? 4 : 3;
            else if (*p == 'q' || *p == 'j') len = 4;
            else if (*p == 'z' || *p == 't') len = 5;
            else if (*p == 'L') len = 6;
            else break;
        }
        switch (*p) {
        case 'd': case 'i':
            if (len == 4) PUT(TAKE64());
            else PUT((int64_t)(int32_t)TAKE32());
            break;
        case 'u': case 'o': case 'x': case 'X': case 'c': case 'C':
            if (len == 4) PUT(TAKE64());
            else PUT(TAKE32());
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (len == 6) {
                uint8_t ext[12];
                memcpy(ext, m32_h(ap), 12);
                ap += 12;
                d = x87_ext_to_double(ext);
            } else {
                uint64_t bits = TAKE64();
                memcpy(&d, &bits, 8);
            }
            uint64_t bits;
            memcpy(&bits, &d, 8);
            PUT(bits);
            break;
        }
        case 's': case 'S':
            PUT((uintptr_t)m32_host(TAKE32()));
            break;
        case '@':
            PUT((uintptr_t)m32_objc_to_host(TAKE32()));
            break;
        case 'p':
            PUT(TAKE32());   /* the guest's own address is what it wants printed */
            break;
        case 'n':
            if (len == 3 || len == 4 || len == 5) {
                m32_log_once("%ln and wider are not supported:", who);
                return -1;
            }
            PUT((uintptr_t)m32_h(TAKE32()));
            break;
        default:
            m32_log_once("unknown printf conversion:", who);
            return -1;
        }
    }
    return n;
too_many:
    m32_log_once("too many printf arguments:", who);
    return -1;
#undef TAKE32
#undef TAKE64
#undef PUT
}

/* fmt at argument index fi; the variadic arguments follow it, or a v-form's va_list is argument vi */
static int fmt_args(OcerzCPU *cpu, int fi, int vi, const char **fmt, uint64_t *slots, const char *who)
{
    uint32_t f = m32_arg(cpu, fi);
    *fmt = f ? (const char *)m32_h(f) : NULL;
    if (!*fmt)
        return -1;
    uint32_t ap = vi >= 0 ? m32_arg(cpu, vi) : (uint32_t)cpu->gpr[OCERZ_RSP] + 4 + 4 * (fi + 1);
    return m32_printf_slots(*fmt, ap, slots, M32_MAX_SLOTS, who);
}

#define M32_VA(slots) ((va_list)(void *)(slots))
#define RET(v) do { uint32_t r_ = (uint32_t)(v); m32_save_errno(cpu); m32_ret(cpu, r_, 0, 0); return OCERZ_STEP_OK; } while (0)
#define FAIL() RET(-1)

static int sp_printf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 0, -1, &fmt, s, "printf") < 0) FAIL();
    RET(vprintf(fmt, M32_VA(s)));
}
static int sp_vprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 0, 1, &fmt, s, "vprintf") < 0) FAIL();
    RET(vprintf(fmt, M32_VA(s)));
}
static int sp_fprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, -1, &fmt, s, "fprintf") < 0) FAIL();
    RET(vfprintf(m32_host(m32_arg(cpu, 0)), fmt, M32_VA(s)));
}
static int sp_vfprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, 2, &fmt, s, "vfprintf") < 0) FAIL();
    RET(vfprintf(m32_host(m32_arg(cpu, 0)), fmt, M32_VA(s)));
}
static int sp_dprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, -1, &fmt, s, "dprintf") < 0) FAIL();
    RET(vdprintf((int)m32_arg(cpu, 0), fmt, M32_VA(s)));
}
static int sp_sprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, -1, &fmt, s, "sprintf") < 0) FAIL();
    RET(vsprintf(m32_h(m32_arg(cpu, 0)), fmt, M32_VA(s)));
}
static int sp_vsprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, 2, &fmt, s, "vsprintf") < 0) FAIL();
    RET(vsprintf(m32_h(m32_arg(cpu, 0)), fmt, M32_VA(s)));
}
static int sp_snprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 2, -1, &fmt, s, "snprintf") < 0) FAIL();
    uint32_t d = m32_arg(cpu, 0), size = m32_arg(cpu, 1);
    RET(vsnprintf(d ? m32_h(d) : NULL, size, fmt, M32_VA(s)));
}
static int sp_vsnprintf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 2, 3, &fmt, s, "vsnprintf") < 0) FAIL();
    uint32_t d = m32_arg(cpu, 0), size = m32_arg(cpu, 1);
    RET(vsnprintf(d ? m32_h(d) : NULL, size, fmt, M32_VA(s)));
}
/* __sprintf_chk(dst, flag, dstlen, fmt, ...) and friends: the length is honored, the check was the guest's */
static int sp_sprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 3, -1, &fmt, s, "__sprintf_chk") < 0) FAIL();
    RET(vsnprintf(m32_h(m32_arg(cpu, 0)), m32_arg(cpu, 2), fmt, M32_VA(s)));
}
static int sp_vsprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 3, 4, &fmt, s, "__vsprintf_chk") < 0) FAIL();
    RET(vsnprintf(m32_h(m32_arg(cpu, 0)), m32_arg(cpu, 2), fmt, M32_VA(s)));
}
static int sp_snprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu)   /* (dst, maxlen, flag, dstlen, fmt, ...) */
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 4, -1, &fmt, s, "__snprintf_chk") < 0) FAIL();
    uint32_t d = m32_arg(cpu, 0);
    RET(vsnprintf(d ? m32_h(d) : NULL, m32_arg(cpu, 1), fmt, M32_VA(s)));
}
static int sp_vsnprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 4, 5, &fmt, s, "__vsnprintf_chk") < 0) FAIL();
    uint32_t d = m32_arg(cpu, 0);
    RET(vsnprintf(d ? m32_h(d) : NULL, m32_arg(cpu, 1), fmt, M32_VA(s)));
}
static int asprintf_common(OcerzCPU *cpu, int vi)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, vi, &fmt, s, "asprintf") < 0) FAIL();
    char *out = NULL;
    int r = vasprintf(&out, fmt, M32_VA(s));
    uint32_t g = r >= 0 ? m32_strdup_host(out) : 0;
    free(out);
    m32_wr(m32_arg(cpu, 0), g);
    RET(g ? r : -1);
}
static int sp_asprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return asprintf_common(cpu, -1); }
static int sp_vasprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return asprintf_common(cpu, 2); }
static int sp_syslog(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt; uint64_t s[M32_MAX_SLOTS];
    if (fmt_args(cpu, 1, -1, &fmt, s, "syslog") >= 0)
        vsyslog((int)m32_arg(cpu, 0), fmt, M32_VA(s));
    RET(0);
}

/* ---- scanf ---- */

typedef struct ScanBack { uint32_t guest; int kind; uint64_t cell; } ScanBack;   /* kind 4: narrow to 32, 12: long double */

static int scan_slots(const char *fmt, uint32_t ap, uint64_t *slots, ScanBack *back, int *nback, const char *who)
{
    int n = 0;
    *nback = 0;
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '%')
            continue;
        int suppress = *p == '*';
        if (suppress)
            p++;
        while (isdigit((unsigned char)*p))
            p++;
        int len = 0;
        for (;; p++) {
            if (*p == 'h') len = len == 2 ? 1 : 2;
            else if (*p == 'l') len = len == 3 ? 4 : 3;
            else if (*p == 'q' || *p == 'j') len = 4;
            else if (*p == 'z' || *p == 't') len = 5;
            else if (*p == 'L') len = 6;
            else break;
        }
        char c = *p;
        if (c == '[') {
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']') p++;
        }
        if (suppress)
            continue;
        if (n >= M32_MAX_SLOTS || *nback >= M32_MAX_SLOTS) {
            m32_log_once("too many scanf arguments:", who);
            return -1;
        }
        uint32_t g = m32_rd(ap);
        ap += 4;
        int narrow = 0;
        if (strchr("dioxXun", c))
            narrow = len == 3 || len == 5;
        else if (c == 'p')
            narrow = 1;
        else if (strchr("eEfFgGaA", c) && len == 6)
            narrow = 12;
        else if (!strchr("eEfFgGaAscSC[", c)) {
            m32_log_once("unknown scanf conversion:", who);
            return -1;
        }
        if (narrow) {
            ScanBack *b = &back[(*nback)++];
            b->guest = g;
            b->kind = narrow == 12 ? 12 : 4;
            b->cell = 0;
            slots[n++] = (uintptr_t)&b->cell;
        } else
            slots[n++] = (uintptr_t)(g ? m32_h(g) : NULL);
    }
    return n;
}

static void scan_back(const ScanBack *back, int nback)
{
    for (int i = 0; i < nback; i++) {
        if (back[i].kind == 4) {
            m32_wr(back[i].guest, (uint32_t)back[i].cell);
        } else {   /* long double scanned as a host double: store it as an 80-bit extended */
            double d;
            memcpy(&d, &back[i].cell, 8);
            long double ld = d;   /* host long double is double: build the extended value by hand */
            (void)ld;
            uint8_t ext[10] = { 0 };
            if (d != 0) {
                int e;
                double m = __builtin_frexp(d < 0 ? -d : d, &e);
                uint64_t mant = (uint64_t)__builtin_ldexp(m, 64);
                uint16_t se = (uint16_t)((e - 1 + 16383) | (d < 0 ? 0x8000 : 0));
                memcpy(ext, &mant, 8);
                memcpy(ext + 8, &se, 2);
            }
            memcpy(m32_h(back[i].guest), ext, 10);
        }
    }
}

static int scan_common(OcerzCPU *cpu, int fi, int vi, int (*call)(void *src, const char *fmt, va_list ap), void *src, const char *who)
{
    uint32_t f = m32_arg(cpu, fi);
    const char *fmt = f ? m32_h(f) : NULL;
    uint64_t slots[M32_MAX_SLOTS];
    ScanBack back[M32_MAX_SLOTS];
    int nback;
    uint32_t ap = vi >= 0 ? m32_arg(cpu, vi) : (uint32_t)cpu->gpr[OCERZ_RSP] + 4 + 4 * (fi + 1);
    if (!fmt || scan_slots(fmt, ap, slots, back, &nback, who) < 0)
        FAIL();
    int r = call(src, fmt, M32_VA(slots));
    scan_back(back, nback);
    RET(r);
}
static int call_vsscanf(void *src, const char *fmt, va_list ap) { return vsscanf(src, fmt, ap); }
static int call_vfscanf(void *src, const char *fmt, va_list ap) { return vfscanf(src, fmt, ap); }
static int sp_sscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return scan_common(cpu, 1, -1, call_vsscanf, m32_h(m32_arg(cpu, 0)), "sscanf"); }
static int sp_vsscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return scan_common(cpu, 1, 2, call_vsscanf, m32_h(m32_arg(cpu, 0)), "vsscanf"); }
static int sp_fscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return scan_common(cpu, 1, -1, call_vfscanf, m32_host(m32_arg(cpu, 0)), "fscanf"); }
static int sp_vfscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return scan_common(cpu, 1, 2, call_vfscanf, m32_host(m32_arg(cpu, 0)), "vfscanf"); }
static int sp_scanf(struct OcerzVM *vm, OcerzCPU *cpu) { return scan_common(cpu, 0, -1, call_vfscanf, stdin, "scanf"); }
/* vswscanf(str, fmt, ap): wchar_t is 4 bytes for both, so only the walk over the format needs a narrow copy */
static int sp_vswscanf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t s = m32_arg(cpu, 0), f = m32_arg(cpu, 1);
    const wchar_t *wfmt = f ? (const wchar_t *)m32_h(f) : NULL;
    char fmt[512];
    size_t i = 0;
    for (; wfmt && wfmt[i] && i < sizeof fmt - 1; i++)
        fmt[i] = wfmt[i] < 0x80 ? (char)wfmt[i] : '?';
    fmt[i] = 0;
    uint64_t slots[M32_MAX_SLOTS];
    ScanBack back[M32_MAX_SLOTS];
    int nback;
    if (!s || !wfmt || wfmt[i] || scan_slots(fmt, m32_arg(cpu, 2), slots, back, &nback, "vswscanf") < 0)
        FAIL();
    int r = vswscanf((const wchar_t *)m32_h(s), wfmt, M32_VA(slots));
    scan_back(back, nback);
    RET(r);
}

/* ---- the heap's entry points and malloc zones ----
 * Until the program registers a malloc zone of its own, malloc and friends are m32's dlmalloc heap.  A game that
 * brings tcmalloc (Batman: Arkham Asylum) registers its zone and swaps it in as the default, as on macOS; from then
 * on malloc, calloc, valloc and realloc run the guest zone's own functions: the trap jumps to a small i386 thunk
 * that calls zone->fn(zone, args...) on the caller's frame.  free, realloc and malloc_size go by who owns the
 * pointer, the way malloc_zone_from_ptr does: m32's heap (strdup, early allocations) or the guest zone.
 * i386 malloc_zone_t: size @8, malloc @12, calloc @16, valloc @20, free @24, realloc @28, version @52, memalign @56. */
static uint32_t g_zone_cell;            /* what malloc_default_zone and malloc_create_zone answer */
static uint32_t g_gzone;                /* the guest's registered zone, 0 until one is registered */
static uint32_t g_thunk[64];            /* per zone-function offset: thunk calling zone->fn(zone, a0, a1, a2) */

static uint32_t the_zone(void)
{
    if (!g_zone_cell)
        g_zone_cell = m32_static_alloc(128, 16);
    return g_zone_cell;
}
static int is_ours(uint32_t p) { return m32_msize(p) != 0; }

/* push ebp; mov ebp,esp; and esp,-16; sub esp,16; copy three args; [esp]=zone; call [zone+off]; leave; ret */
static uint32_t zone_thunk(uint32_t off)
{
    if (g_thunk[off / 4])
        return g_thunk[off / 4];
    static const uint8_t pre[] = { 0x55, 0x89, 0xe5, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x10,
                                   0x8b, 0x45, 0x08, 0x89, 0x44, 0x24, 0x04,
                                   0x8b, 0x45, 0x0c, 0x89, 0x44, 0x24, 0x08,
                                   0x8b, 0x45, 0x10, 0x89, 0x44, 0x24, 0x0c };
    uint8_t code[64];
    size_t n = sizeof pre;
    memcpy(code, pre, n);
    uint32_t zone = g_gzone, slot = g_gzone + off;
    code[n++] = 0xc7; code[n++] = 0x04; code[n++] = 0x24; memcpy(code + n, &zone, 4); n += 4;   /* mov [esp], zone */
    code[n++] = 0xa1; memcpy(code + n, &slot, 4); n += 4;                                        /* mov eax, [slot] */
    code[n++] = 0xff; code[n++] = 0xd0;                                                           /* call eax */
    code[n++] = 0x89; code[n++] = 0xec; code[n++] = 0x5d; code[n++] = 0xc3;                       /* mov esp,ebp; pop ebp; ret */
    uint32_t g = m32_static_code((uint32_t)n);
    memcpy(m32_h(g), code, n);
    g_thunk[off / 4] = g;
    return g;
}
/* posix_memalign(out, align, size) with a guest zone: zone->memalign(zone, align, size) stored through out, 0 or
 * ENOMEM, all in guest code.  It went through m32_call, a nested run loop with its signal-mask syscalls, which was
 * ~7% of Batman's render thread (UE3 allocates aligned all the time). */
static uint32_t g_pm_thunk;
static uint32_t memalign_thunk(void)
{
    if (g_pm_thunk)
        return g_pm_thunk;
    uint32_t zone = g_gzone, slot = g_gzone + 56;
    uint8_t c[96];
    size_t n = 0;
    static const uint8_t check[] = { 0x8b, 0x44, 0x24, 0x08,                               /* mov eax, [esp+8] (align) */
                                     0x83, 0xf8, 0x04, 0x72, 0x07,                         /* cmp eax, 4; jb einval */
                                     0x8d, 0x48, 0xff, 0x85, 0xc8, 0x74, 0x06,             /* lea ecx,[eax-1]; test eax,ecx; jz ok */
                                     0xb8, 22, 0, 0, 0, 0xc3 };                            /* einval: mov eax, EINVAL; ret */
    memcpy(c, check, sizeof check); n = sizeof check;
    static const uint8_t pre[] = { 0x55, 0x89, 0xe5, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x10,   /* ok: frame, 16-aligned */
                                   0x8b, 0x45, 0x0c, 0x89, 0x44, 0x24, 0x04,               /* [esp+4] = align */
                                   0x8b, 0x45, 0x10, 0x89, 0x44, 0x24, 0x08 };             /* [esp+8] = size */
    memcpy(c + n, pre, sizeof pre); n += sizeof pre;
    c[n++] = 0xc7; c[n++] = 0x04; c[n++] = 0x24; memcpy(c + n, &zone, 4); n += 4;   /* mov [esp], zone */
    c[n++] = 0xa1; memcpy(c + n, &slot, 4); n += 4;                                 /* mov eax, [zone+56] */
    c[n++] = 0xff; c[n++] = 0xd0;                                                    /* call eax */
    c[n++] = 0x8b; c[n++] = 0x4d; c[n++] = 0x08;                                     /* mov ecx, [ebp+8] */
    c[n++] = 0x85; c[n++] = 0xc0;                                                    /* test eax, eax */
    c[n++] = 0x74; c[n++] = 0x08;                                                    /* jz fail */
    c[n++] = 0x89; c[n++] = 0x01;                                                    /* mov [ecx], eax */
    c[n++] = 0x31; c[n++] = 0xc0;                                                    /* xor eax, eax */
    c[n++] = 0x89; c[n++] = 0xec; c[n++] = 0x5d; c[n++] = 0xc3;                      /* leave; ret */
    c[n++] = 0xb8; c[n++] = 12; c[n++] = 0; c[n++] = 0; c[n++] = 0;                  /* fail: mov eax, ENOMEM */
    c[n++] = 0x89; c[n++] = 0xec; c[n++] = 0x5d; c[n++] = 0xc3;                      /* leave; ret */
    uint32_t g = m32_static_code((uint32_t)n);
    memcpy(m32_h(g), c, n);
    g_pm_thunk = g;
    return g;
}

static int zone_memalign_ok(void) { return g_gzone && m32_rd(g_gzone + 52) >= 5 && m32_rd(g_gzone + 56); }

/* continue in the guest zone's function: the trapped call's own frame becomes the thunk's */
static int to_zone(OcerzCPU *cpu, uint32_t off)
{
    cpu->rip = zone_thunk(off);
    return OCERZ_STEP_OK;
}

static int sp_malloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (g_gzone) return to_zone(cpu, 12); RET(m32_malloc(m32_arg(cpu, 0))); }
static int sp_calloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (g_gzone) return to_zone(cpu, 16); RET(m32_calloc(m32_arg(cpu, 0), m32_arg(cpu, 1))); }
static int sp_valloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (g_gzone) return to_zone(cpu, 20); RET(m32_memalign(4096, m32_arg(cpu, 0))); }
static int sp_free(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0);
    if (g_gzone && p && !is_ours(p))
        return to_zone(cpu, 24);
    m32_free(p);
    RET(0);
}
static int sp_realloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0), n = m32_arg(cpu, 1);
    if (!g_gzone || (p && is_ours(p) && !g_gzone))
        RET(m32_realloc(p, n));
    if (!p || !is_ours(p))
        return to_zone(cpu, 28);
    /* an early m32 allocation grows into the guest zone */
    uint32_t args[2] = { g_gzone, n };
    uint32_t q = m32_call(vm, m32_rd(g_gzone + 12), args, 2, NULL, NULL);
    if (q) {
        uint32_t have = m32_msize(p);
        memcpy(m32_h(q), m32_h(p), have < n ? have : n);
        m32_free(p);
    }
    RET(q);
}
static int sp_reallocf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (g_gzone)
        return sp_realloc(vm, cpu);   /* ponytail: reallocf's free-on-failure is lost with a guest zone */
    uint32_t g = m32_realloc(m32_arg(cpu, 0), m32_arg(cpu, 1));
    if (!g)
        m32_free(m32_arg(cpu, 0));
    RET(g);
}
static int sp_posix_memalign(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t align = m32_arg(cpu, 1), size = m32_arg(cpu, 2), g;
    if (align < 4 || (align & (align - 1)))
        RET(EINVAL);
    if (zone_memalign_ok()) {
        cpu->rip = memalign_thunk();
        return OCERZ_STEP_OK;
    }
    g = m32_memalign(align, size);
    static int log = -1;
    static unsigned long calls, fails;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "memalign");
    calls++;
    if (log && (!g || calls % 100000 == 0))
        fprintf(stderr, "ocerz: m32: posix_memalign #%lu align %#x size %#x -> %#x (failed %lu)\n", calls, align, size, g,
                fails + !g);
    if (!g) {
        fails++;
        RET(ENOMEM);
    }
    m32_wr(m32_arg(cpu, 0), g);
    RET(0);
}
static int sp_malloc_size(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0);
    if (g_gzone && p && !is_ours(p))
        return to_zone(cpu, 8);
    RET(m32_msize(p));
}
static int sp_malloc_good_size(struct OcerzVM *vm, OcerzCPU *cpu) { RET((m32_arg(cpu, 0) + 15) & ~15u); }
static int sp_malloc_default_zone(struct OcerzVM *vm, OcerzCPU *cpu) { RET(the_zone()); }
static int sp_zone_register(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t z = m32_arg(cpu, 0);
    if (z && z != the_zone() && !g_gzone) {
        g_gzone = z;
        char name[64] = "";
        uint32_t np = m32_rd(z + 36);
        if (np)
            snprintf(name, sizeof name, "%s", (const char *)m32_h(np));
        m32_log_once("the program registered its own malloc zone; malloc now runs it:", name[0] ? name : "(unnamed)");
        zone_thunk(12), zone_thunk(16), zone_thunk(20);
        if (zone_memalign_ok())
            memalign_thunk();
    }
    RET(0);
}
static int sp_zone_unregister(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (m32_arg(cpu, 0) == g_gzone && g_gzone) {
        g_gzone = 0;
        memset(g_thunk, 0, sizeof g_thunk);
        g_pm_thunk = 0;
    }
    RET(0);
}
/* malloc_zone_*(zone, ...) with a guest zone: its own function, on this very frame */
static int zone_call(OcerzCPU *cpu, uint32_t off)
{
    uint32_t z = m32_arg(cpu, 0);
    cpu->rip = m32_rd(z + off);
    return OCERZ_STEP_OK;
}
static int guest_zone_arg(OcerzCPU *cpu) { uint32_t z = m32_arg(cpu, 0); return z && z != the_zone() && z == g_gzone; }
static int sp_zone_malloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (guest_zone_arg(cpu)) return zone_call(cpu, 12); RET(m32_malloc(m32_arg(cpu, 1))); }
static int sp_zone_calloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (guest_zone_arg(cpu)) return zone_call(cpu, 16); RET(m32_calloc(m32_arg(cpu, 1), m32_arg(cpu, 2))); }
static int sp_zone_valloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (guest_zone_arg(cpu)) return zone_call(cpu, 20); RET(m32_memalign(4096, m32_arg(cpu, 1))); }
static int sp_zone_free(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 1);
    if (p && !is_ours(p) && g_gzone) {   /* whatever zone was named, the pointer's owner frees it */
        m32_wr((uint32_t)cpu->gpr[OCERZ_RSP] + 4, g_gzone);
        return zone_call(cpu, 24);
    }
    m32_free(p);
    RET(0);
}
static int sp_zone_realloc(struct OcerzVM *vm, OcerzCPU *cpu) { if (guest_zone_arg(cpu)) return zone_call(cpu, 28); RET(m32_realloc(m32_arg(cpu, 1), m32_arg(cpu, 2))); }
static int sp_zone_memalign(struct OcerzVM *vm, OcerzCPU *cpu) { if (guest_zone_arg(cpu)) return zone_call(cpu, 56); RET(m32_memalign(m32_arg(cpu, 1), m32_arg(cpu, 2))); }
static int sp_noop0(struct OcerzVM *vm, OcerzCPU *cpu) { RET(0); }
static int sp_zone_from_ptr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0);
    RET(!p ? 0 : is_ours(p) ? the_zone() : g_gzone);
}

static int sp_strdup(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t s = m32_arg(cpu, 0);
    RET(s ? m32_strdup_host(m32_h(s)) : 0);
}
static int sp_strndup(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t s = m32_arg(cpu, 0), n = m32_arg(cpu, 1);
    if (!s)
        RET(0);
    size_t len = strnlen(m32_h(s), n);
    uint32_t g = m32_malloc((uint32_t)len + 1);
    if (g) {
        memcpy(m32_h(g), m32_h(s), len);
        ((char *)m32_h(g))[len] = 0;
    }
    RET(g);
}
static int sp_realpath(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0), out = m32_arg(cpu, 1);
    char buf[1024];
    const char *r = p ? realpath(m32_h(p), buf) : NULL;
    if (!r)
        RET(0);
    if (out) {
        strcpy(m32_h(out), r);
        RET(out);
    }
    RET(m32_strdup_host(r));
}
static int sp_getcwd(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t out = m32_arg(cpu, 0), size = m32_arg(cpu, 1);
    char buf[1024];
    if (!getcwd(buf, sizeof buf))
        RET(0);
    if (!out)
        RET(m32_strdup_host(buf));
    if (strlen(buf) + 1 > size) {
        errno = ERANGE;
        RET(0);
    }
    strcpy(m32_h(out), buf);
    RET(out);
}

const M32SpecialEntry m32_stdio_specials[] = {
    { "___error", sp___error },
    { "_printf", sp_printf }, { "_vprintf", sp_vprintf }, { "_fprintf", sp_fprintf }, { "_vfprintf", sp_vfprintf },
    { "_dprintf", sp_dprintf }, { "_sprintf", sp_sprintf }, { "_vsprintf", sp_vsprintf },
    { "_snprintf", sp_snprintf }, { "_vsnprintf", sp_vsnprintf }, { "___sprintf_chk", sp_sprintf_chk },
    { "___vsprintf_chk", sp_vsprintf_chk }, { "___snprintf_chk", sp_snprintf_chk },
    { "___vsnprintf_chk", sp_vsnprintf_chk }, { "_asprintf", sp_asprintf }, { "_vasprintf", sp_vasprintf },
    { "_syslog", sp_syslog },
    { "_sscanf", sp_sscanf }, { "_vsscanf", sp_vsscanf }, { "_fscanf", sp_fscanf }, { "_vfscanf", sp_vfscanf },
    { "_scanf", sp_scanf }, { "_vswscanf", sp_vswscanf },
    { "_malloc", sp_malloc }, { "_calloc", sp_calloc }, { "_realloc", sp_realloc }, { "_reallocf", sp_reallocf },
    { "_free", sp_free }, { "_valloc", sp_valloc }, { "_posix_memalign", sp_posix_memalign },
    { "_malloc_size", sp_malloc_size }, { "_malloc_good_size", sp_malloc_good_size },
    { "_malloc_default_zone", sp_malloc_default_zone }, { "_malloc_default_purgeable_zone", sp_malloc_default_zone },
    { "_malloc_create_zone", sp_malloc_default_zone }, { "_malloc_zone_malloc", sp_zone_malloc },
    { "_malloc_zone_calloc", sp_zone_calloc }, { "_malloc_zone_realloc", sp_zone_realloc },
    { "_malloc_zone_free", sp_zone_free }, { "_malloc_zone_memalign", sp_zone_memalign },
    { "_malloc_zone_valloc", sp_zone_valloc }, { "_malloc_set_zone_name", sp_noop0 },
    { "_malloc_zone_register", sp_zone_register }, { "_malloc_zone_unregister", sp_zone_unregister },
    { "_malloc_destroy_zone", sp_noop0 }, { "_malloc_zone_from_ptr", sp_zone_from_ptr },
    { "_strdup", sp_strdup }, { "_strndup", sp_strndup }, { "_realpath", sp_realpath }, { "_getcwd", sp_getcwd },
    { NULL, NULL }
};
