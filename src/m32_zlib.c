/*
 * Zlib and bzip2 streams for i386 guests (include/ocerz/m32.h).
 *
 * A z_stream (and a bz_stream) is pointers and longs: 56 bytes on i386, 112 on the host, so the host library cannot
 * work on the guest's.  Each guest stream gets a host one, kept until its End call; before every call the guest's
 * buffer pointers and counts are copied in, and after it they, the totals, adler and msg come back.  The guest's
 * zalloc/zfree are not used (the host library allocates its own state).  The buffer-to-buffer calls (uncompress,
 * compress, crc32, adler32) need nothing of this and stay generic.
 *
 * ponytail: a fixed table of 256 live streams under one lock; grow it when a game holds more at once.
 */
#include <bzlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"

typedef struct Stream { uint32_t g; void *h; int bz; } Stream;
static Stream g_streams[256];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *zsym(const char *name, int bz)
{
    void *f = ocerz_bridge_host_symbol(bz ? "/usr/lib/libbz2.1.0.dylib" : "/usr/lib/libz.1.dylib", name);
    if (!f)
        fprintf(stderr, "ocerz: m32: no host %s\n", name);
    return f;
}

static void *stream_host(uint32_t g, int bz, int create)
{
    void *h = NULL;
    pthread_mutex_lock(&g_lock);
    int free_at = -1;
    for (int i = 0; i < 256 && !h; i++) {
        if (g_streams[i].g == g)
            h = g_streams[i].h;
        else if (!g_streams[i].g && free_at < 0)
            free_at = i;
    }
    if (!h && create && free_at >= 0) {
        h = calloc(1, bz ? sizeof(bz_stream) : sizeof(z_stream));
        g_streams[free_at] = (Stream){ g, h, bz };
    }
    pthread_mutex_unlock(&g_lock);
    return h;
}
static void stream_forget(uint32_t g)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 256; i++)
        if (g_streams[i].g == g) {
            free(g_streams[i].h);
            g_streams[i] = (Stream){ 0, NULL, 0 };
        }
    pthread_mutex_unlock(&g_lock);
}

/* i386 z_stream: next_in 0, avail_in 4, total_in 8, next_out 12, avail_out 16, total_out 20, msg 24, state 28,
 * zalloc 32, zfree 36, opaque 40, data_type 44, adler 48, reserved 52 */
static void z_in(uint32_t g, z_stream *h)
{
    uint32_t ni = m32_rd(g), no = m32_rd(g + 12);
    h->next_in = ni ? m32_h(ni) : NULL;
    h->avail_in = m32_rd(g + 4);
    h->next_out = no ? m32_h(no) : NULL;
    h->avail_out = m32_rd(g + 16);
}
static void z_out(uint32_t g, const z_stream *h)
{
    m32_wr(g, h->next_in ? m32_g(h->next_in) : 0);
    m32_wr(g + 4, h->avail_in);
    m32_wr(g + 8, (uint32_t)h->total_in);
    m32_wr(g + 12, h->next_out ? m32_g(h->next_out) : 0);
    m32_wr(g + 16, h->avail_out);
    m32_wr(g + 20, (uint32_t)h->total_out);
    m32_wr(g + 24, h->msg ? m32_cstring(h->msg) : 0);
    m32_wr(g + 28, g);   /* state: non-NULL, as zlib's own checks in guest code expect */
    m32_wr(g + 44, (uint32_t)h->data_type);
    m32_wr(g + 48, (uint32_t)h->adler);
}

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

/* inflateInit_(strm, version, size) / inflateInit2_(strm, windowBits, version, size) and the deflate ones */
static int z_init(OcerzCPU *cpu, const char *sym, int nint)
{
    uint32_t g = m32_arg(cpu, 0);
    z_stream *h = g ? stream_host(g, 0, 1) : NULL;
    if (!h)
        RET(Z_STREAM_ERROR);
    memset(h, 0, sizeof *h);
    z_in(g, h);
    int ints[4] = { 0 };
    for (int i = 0; i < nint; i++)
        ints[i] = (int)m32_arg(cpu, 1 + i);
    const char *version = (const char *)m32_h(m32_arg(cpu, 1 + nint));
    int r = Z_STREAM_ERROR;
    if (nint == 0) {
        int (*f)(z_stream *, const char *, int) = zsym(sym, 0);
        r = f ? f(h, version, (int)sizeof *h) : r;
    } else if (nint == 1) {
        int (*f)(z_stream *, int, const char *, int) = zsym(sym, 0);
        r = f ? f(h, ints[0], version, (int)sizeof *h) : r;
    } else {   /* deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, size) */
        int (*f)(z_stream *, int, int, int, int, int, const char *, int) = zsym(sym, 0);
        int all[5];
        for (int i = 0; i < 5; i++)
            all[i] = (int)m32_arg(cpu, 1 + i);
        version = (const char *)m32_h(m32_arg(cpu, 6));
        r = f ? f(h, all[0], all[1], all[2], all[3], all[4], version, (int)sizeof *h) : r;
    }
    if (r == Z_OK)
        z_out(g, h);
    else
        stream_forget(g);
    RET(r);
}
static int sp_inflateInit_(struct OcerzVM *vm, OcerzCPU *cpu) { return z_init(cpu, "inflateInit_", 0); }
static int sp_inflateInit2_(struct OcerzVM *vm, OcerzCPU *cpu) { return z_init(cpu, "inflateInit2_", 1); }
static int sp_deflateInit_(struct OcerzVM *vm, OcerzCPU *cpu) { return z_init(cpu, "deflateInit_", 1); }
static int sp_deflateInit2_(struct OcerzVM *vm, OcerzCPU *cpu) { return z_init(cpu, "deflateInit2_", 5); }

/* inflate(strm, flush), deflate(strm, flush), and the one-argument End/Reset calls */
static int z_call(OcerzCPU *cpu, const char *sym, int with_flush, int ends)
{
    uint32_t g = m32_arg(cpu, 0);
    z_stream *h = g ? stream_host(g, 0, 0) : NULL;
    if (!h)
        RET(Z_STREAM_ERROR);
    z_in(g, h);
    int r;
    if (with_flush) {
        int (*f)(z_stream *, int) = zsym(sym, 0);
        r = f ? f(h, (int)m32_arg(cpu, 1)) : Z_STREAM_ERROR;
    } else {
        int (*f)(z_stream *) = zsym(sym, 0);
        r = f ? f(h) : Z_STREAM_ERROR;
    }
    z_out(g, h);
    if (ends) {
        m32_wr(g + 28, 0);
        stream_forget(g);
    }
    RET(r);
}
static int sp_inflate(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "inflate", 1, 0); }
static int sp_deflate(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "deflate", 1, 0); }
static int sp_inflateEnd(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "inflateEnd", 0, 1); }
static int sp_deflateEnd(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "deflateEnd", 0, 1); }
static int sp_inflateReset(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "inflateReset", 0, 0); }
static int sp_deflateReset(struct OcerzVM *vm, OcerzCPU *cpu) { return z_call(cpu, "deflateReset", 0, 0); }

/* i386 bz_stream: next_in 0, avail_in 4, total_in_lo32 8, total_in_hi32 12, next_out 16, avail_out 20,
 * total_out_lo32 24, total_out_hi32 28, state 32, bzalloc 36, bzfree 40, opaque 44 */
static void bz_in(uint32_t g, bz_stream *h)
{
    uint32_t ni = m32_rd(g), no = m32_rd(g + 16);
    h->next_in = ni ? (char *)m32_h(ni) : NULL;
    h->avail_in = m32_rd(g + 4);
    h->next_out = no ? (char *)m32_h(no) : NULL;
    h->avail_out = m32_rd(g + 20);
}
static void bz_out(uint32_t g, const bz_stream *h)
{
    m32_wr(g, h->next_in ? m32_g(h->next_in) : 0);
    m32_wr(g + 4, h->avail_in);
    m32_wr(g + 8, h->total_in_lo32);
    m32_wr(g + 12, h->total_in_hi32);
    m32_wr(g + 16, h->next_out ? m32_g(h->next_out) : 0);
    m32_wr(g + 20, h->avail_out);
    m32_wr(g + 24, h->total_out_lo32);
    m32_wr(g + 28, h->total_out_hi32);
    m32_wr(g + 32, h->state ? g : 0);
}
/* BZ2_bzDecompressInit(strm, verbosity, small) */
static int sp_bzDecompressInit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    bz_stream *h = g ? stream_host(g, 1, 1) : NULL;
    int (*f)(bz_stream *, int, int) = zsym("BZ2_bzDecompressInit", 1);
    if (!h || !f)
        RET(BZ_PARAM_ERROR);
    memset(h, 0, sizeof *h);
    bz_in(g, h);
    int r = f(h, (int)m32_arg(cpu, 1), (int)m32_arg(cpu, 2));
    if (r == BZ_OK)
        bz_out(g, h);
    else
        stream_forget(g);
    RET(r);
}
static int bz_call(OcerzCPU *cpu, const char *sym, int ends)
{
    uint32_t g = m32_arg(cpu, 0);
    bz_stream *h = g ? stream_host(g, 1, 0) : NULL;
    int (*f)(bz_stream *) = zsym(sym, 1);
    if (!h || !f)
        RET(BZ_PARAM_ERROR);
    bz_in(g, h);
    int r = f(h);
    bz_out(g, h);
    if (ends) {
        m32_wr(g + 32, 0);
        stream_forget(g);
    }
    RET(r);
}
static int sp_bzDecompress(struct OcerzVM *vm, OcerzCPU *cpu) { return bz_call(cpu, "BZ2_bzDecompress", 0); }
static int sp_bzDecompressEnd(struct OcerzVM *vm, OcerzCPU *cpu) { return bz_call(cpu, "BZ2_bzDecompressEnd", 1); }

const M32SpecialEntry m32_zlib_specials[] = {
    { "_inflateInit_", sp_inflateInit_ }, { "_inflateInit2_", sp_inflateInit2_ }, { "_inflate", sp_inflate },
    { "_inflateEnd", sp_inflateEnd }, { "_inflateReset", sp_inflateReset },
    { "_deflateInit_", sp_deflateInit_ }, { "_deflateInit2_", sp_deflateInit2_ }, { "_deflate", sp_deflate },
    { "_deflateEnd", sp_deflateEnd }, { "_deflateReset", sp_deflateReset },
    { "_BZ2_bzDecompressInit", sp_bzDecompressInit }, { "_BZ2_bzDecompress", sp_bzDecompress },
    { "_BZ2_bzDecompressEnd", sp_bzDecompressEnd },
    { NULL, NULL }
};
