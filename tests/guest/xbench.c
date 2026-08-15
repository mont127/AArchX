/* xbench: broad throughput suite for Ocerz-vs-Rosetta comparison.
 * Usage: xbench <kernel> <scale>   -- work is linear in <scale>.
 * Each kernel prints a checksum so both engines can be diffed. */
#include "gsys.h"

static g_u64 parse(const char *p, g_u64 dflt)
{
    g_u64 v = 0; int any = 0;
    for (; *p >= '0' && *p <= '9'; p++) { v = v * 10u + (g_u64)(*p - '0'); any = 1; }
    return any ? v : dflt;
}

/* ---- 1. icall: indirect calls through a function-pointer table (vtable-like) */
static g_u64 f0(g_u64 x) { return x + 1; }
static g_u64 f1(g_u64 x) { return x ^ 0x9e37; }
static g_u64 f2(g_u64 x) { return x * 3; }
static g_u64 f3(g_u64 x) { return (x << 1) | (x >> 63); }
static g_u64 f4(g_u64 x) { return x - 7; }
static g_u64 f5(g_u64 x) { return x ^ (x >> 5); }
static g_u64 f6(g_u64 x) { return x + (x << 3); }
static g_u64 f7(g_u64 x) { return ~x; }
typedef g_u64 (*fn_t)(g_u64);
static fn_t tbl[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
static g_u64 k_icall(g_u64 n)
{
    g_u64 x = 1, s = 12345;
    for (g_u64 i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        x = tbl[(s >> 59) & 7](x);
    }
    return x;
}

/* ---- 2. jtab: dense switch -> jump table with unpredictable selector */
static g_u64 k_jtab(g_u64 n)
{
    g_u64 x = 0, s = 777;
    for (g_u64 i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        switch ((s >> 60) & 15) {
        case 0: x += 1; break;      case 1: x ^= s; break;
        case 2: x -= 3; break;      case 3: x = x * 5 + 1; break;
        case 4: x |= s >> 7; break; case 5: x &= s | 0xff; break;
        case 6: x <<= 1; break;     case 7: x >>= 1; break;
        case 8: x += s >> 3; break; case 9: x ^= x >> 11; break;
        case 10: x = ~x; break;     case 11: x -= s; break;
        case 12: x += x << 2; break;case 13: x ^= 0xdead; break;
        case 14: x = x * 7; break;  default: x = x + s; break;
        }
    }
    return x;
}

/* ---- 3. depchain: long serial dependency chain (latency-bound ALU) */
static g_u64 k_depchain(g_u64 n)
{
    g_u64 x = 0x123456789abcdefULL;
    for (g_u64 i = 0; i < n; i++) {
        x = x * 0x9E3779B97F4A7C15ULL;
        x ^= x >> 29;
        x += 0x165667B19E3779F9ULL;
        x ^= x >> 32;
        x = (x << 13) | (x >> 51);
    }
    return x;
}

/* ---- 4. brmiss: data-dependent unpredictable branches (cmov-hostile) */
static g_u64 k_brmiss(g_u64 n)
{
    g_u64 s = 99, a = 0, b = 0, c = 0;
    for (g_u64 i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        if (s & 1) a += s >> 3;
        else if (s & 2) b ^= s;
        else if (s & 4) c += 1;
        else a -= b;
        if ((s >> 20) & 1) { if (a > b) c++; else c--; }
    }
    return a ^ b ^ c;
}

/* ---- 5. memcpyk: memcpy of mixed sizes (rep movsb / lowering paths) */
static unsigned char sbuf[1 << 16], dbuf[1 << 16];
static g_u64 k_memcpy(g_u64 n)
{
    for (g_u64 i = 0; i < sizeof sbuf; i++) sbuf[i] = (unsigned char)(i * 31);
    g_u64 s = 3, sum = 0;
    for (g_u64 i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        g_u64 len = 8 + ((s >> 40) & 4095);
        g_u64 so = (s >> 12) & 0x7fff, dof = (s >> 27) & 0x7fff;
        __builtin_memcpy(dbuf + dof, sbuf + so, len);
        sum += dbuf[dof + (len >> 1)];
    }
    return sum;
}

/* ---- 6. strk: strlen/strcmp-like byte loops on short strings */
static g_u64 k_str(g_u64 n)
{
    static char strs[64][40];
    for (int i = 0; i < 64; i++) {
        int L = 5 + (i * 7) % 30;
        for (int j = 0; j < L; j++) strs[i][j] = 'a' + ((i + j) % 26);
        strs[i][L] = 0;
    }
    g_u64 acc = 0;
    for (g_u64 i = 0; i < n; i++) {
        const char *a = strs[i & 63], *b = strs[(i * 7 + 3) & 63];
        g_u64 la = 0; while (a[la]) la++;
        int cmp = 0; g_u64 k = 0;
        while (a[k] && a[k] == b[k]) k++;
        cmp = (unsigned char)a[k] - (unsigned char)b[k];
        acc += la + (g_u64)(cmp > 0) + k;
    }
    return acc;
}

/* ---- 7. hash: FNV/xxhash-style mixing over a buffer (rotates, muls, loads) */
static g_u64 k_hash(g_u64 n)
{
    static g_u64 data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (g_u64)i * 0x9E3779B97F4A7C15ULL;
    g_u64 h = 0xcbf29ce484222325ULL;
    for (g_u64 r = 0; r < n; r++) {
        for (int i = 0; i < 4096; i += 4) {
            h ^= data[i];     h *= 0x100000001b3ULL; h = (h << 31) | (h >> 33);
            h ^= data[i + 1]; h *= 0x100000001b3ULL; h = (h << 27) | (h >> 37);
            h ^= data[i + 2]; h *= 0x100000001b3ULL; h = (h << 33) | (h >> 31);
            h ^= data[i + 3]; h *= 0x100000001b3ULL; h = (h << 29) | (h >> 35);
        }
        data[r & 4095] += h;
    }
    return h;
}

/* ---- 8. idiv: integer division/modulo (microcoded, slow on both) */
static g_u64 k_idiv(g_u64 n)
{
    g_u64 acc = 0, s = 1;
    for (g_u64 i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        g_u64 d = (s >> 40) | 1;
        acc += (s / d) + (s % d) + ((s >> 3) / ((i & 1023) + 1));
    }
    return acc;
}

/* ---- 9. fpsse: scalar double math (SSE2 mulsd/addsd/divsd/sqrtsd) */
static g_u64 k_fpsse(g_u64 n)
{
    double x = 1.0, y = 0.5, acc = 0.0;
    for (g_u64 i = 0; i < n; i++) {
        x = x * 1.0000001 + 0.3;
        y = y * 0.9999999 - x * 0.25;
        acc += __builtin_sqrt(x * x + y * y) / (1.0 + (double)(i & 7));
        if (x > 1e6) x = 1.0;
        if (y < -1e6) y = 0.5;
    }
    union { double d; g_u64 u; } u; u.d = acc;
    return u.u >> 8;
}

/* ---- 10. fpvec: vectorizable float loop (packed SSE / auto-vec) */
static g_u64 k_fpvec(g_u64 n)
{
    static float a[8192], b[8192], c[8192];
    for (int i = 0; i < 8192; i++) { a[i] = (float)i * 0.5f; b[i] = 1.0f + (float)(i & 15); c[i] = 0.f; }
    for (g_u64 r = 0; r < n; r++) {
        for (int i = 0; i < 8192; i++)
            c[i] = c[i] * 0.999f + a[i] * b[i] + 1.0f;
    }
    float s = 0.f; for (int i = 0; i < 8192; i++) s += c[i];
    union { float f; g_u32 u; } u; u.f = s;
    return u.u >> 4;
}

/* ---- 11. chase: pointer chasing through a shuffled linked list (cache-latency) */
static g_u64 k_chase(g_u64 n)
{
    static g_u64 next[1 << 16];
    g_u64 N = 1 << 16, s = 42;
    for (g_u64 i = 0; i < N; i++) next[i] = i;
    for (g_u64 i = N - 1; i > 0; i--) {   /* Fisher-Yates */
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        g_u64 j = (s >> 33) % (i + 1);
        g_u64 t = next[i]; next[i] = next[j]; next[j] = t;
    }
    g_u64 p = 0, acc = 0;
    for (g_u64 i = 0; i < n; i++) { p = next[p]; acc += p; }
    return acc;
}

/* ---- 12. qsort: recursive quicksort of an int array (calls + branches + memory) */
static void qs(int *a, int lo, int hi)
{
    while (lo < hi) {
        int p = a[(lo + hi) >> 1], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
    }
}
static g_u64 k_qsort(g_u64 n)
{
    static int arr[65536];
    g_u64 acc = 0, s = 7;
    for (g_u64 r = 0; r < n; r++) {
        for (int i = 0; i < 65536; i++) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; arr[i] = (int)(s >> 35); }
        qs(arr, 0, 65535);
        acc += (g_u64)(unsigned)arr[1000] + (g_u64)(unsigned)arr[60000];
    }
    return acc;
}

/* ---- 13. leafcall: many small non-recursive calls (call/ret + prologue) */
__attribute__((noinline)) static g_u64 leaf(g_u64 a, g_u64 b) { return (a * 3) ^ (b >> 2); }
__attribute__((noinline)) static g_u64 leaf2(g_u64 a) { return leaf(a, a + 1) + leaf(a ^ 5, a); }
static g_u64 k_leafcall(g_u64 n)
{
    g_u64 acc = 0;
    for (g_u64 i = 0; i < n; i++) acc = leaf2(acc + i);
    return acc;
}

/* ---- 14. mixed: a "real program" style mix: struct updates, branches, small loops */
struct particle { double x, y, vx, vy; g_u64 id; };
static g_u64 k_mixed(g_u64 n)
{
    static struct particle ps[1024];
    for (int i = 0; i < 1024; i++) { ps[i].x = i; ps[i].y = -i; ps[i].vx = 0.5; ps[i].vy = 0.25; ps[i].id = (g_u64)i; }
    g_u64 acc = 0;
    for (g_u64 r = 0; r < n; r++) {
        for (int i = 0; i < 1024; i++) {
            struct particle *p = &ps[i];
            p->x += p->vx; p->y += p->vy;
            if (p->x > 100.0) { p->x = 0; p->vx = -p->vx; }
            if (p->y < -100.0) { p->y = 0; p->vy = -p->vy; }
            if ((p->id ^ (g_u64)r) & 1) acc += (g_u64)p->x; else acc ^= (g_u64)(-p->y);
        }
    }
    return acc;
}

/* ---- 15. bigswitch: interpreter-style dispatch loop (bytecode VM) */
static g_u64 k_vm(g_u64 n)
{
    static unsigned char code[256];
    g_u64 s = 5;
    for (int i = 0; i < 256; i++) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; code[i] = (unsigned char)((s >> 58) % 12); }
    g_u64 regs[4] = { 1, 2, 3, 4 };
    g_u64 pc = 0, steps = n * 256;
    while (steps--) {
        switch (code[pc & 255]) {
        case 0: regs[0] += regs[1]; break;
        case 1: regs[1] ^= regs[2]; break;
        case 2: regs[2] = regs[3] * 3 + 1; break;
        case 3: regs[3] -= regs[0] >> 2; break;
        case 4: if (regs[0] & 1) pc += 3; break;
        case 5: regs[0] = (regs[0] << 5) | (regs[0] >> 59); break;
        case 6: regs[1] += pc; break;
        case 7: if (regs[2] > regs[3]) { g_u64 t = regs[2]; regs[2] = regs[3]; regs[3] = t; } break;
        case 8: regs[3] ^= 0xf0f0; break;
        case 9: pc += (regs[1] & 7); break;
        case 10: regs[0] = regs[0] * 0x9E3779B97F4A7C15ULL; break;
        default: regs[2] += 11; break;
        }
        pc++;
    }
    return regs[0] ^ regs[1] ^ regs[2] ^ regs[3];
}

int main(int argc, char **argv)
{
    const char *k = argc > 1 ? argv[1] : "icall";
    g_u64 n = argc > 2 ? parse(argv[2], 0) : 0;
    g_u64 r = 0;
#define K(name, dflt) if (!__builtin_strcmp(k, #name)) { r = k_##name(n ? n : (dflt)); goto out; }
    K(icall, 50000000)  K(jtab, 50000000)  K(depchain, 100000000) K(brmiss, 50000000)
    K(memcpy, 2000000)  K(str, 20000000)   K(hash, 20000)          K(idiv, 10000000)
    K(fpsse, 30000000)  K(fpvec, 5000)     K(chase, 30000000)      K(qsort, 30)
    K(leafcall, 50000000) K(mixed, 20000)  K(vm, 500000)
    g_puts("unknown kernel\n");
    return 2;
out:
    g_putu64(r);
    return 0;
}
