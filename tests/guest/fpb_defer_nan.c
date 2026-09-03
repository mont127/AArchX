/* Deferred FP-batch checks: a packed-double add whose NaN check is deferred
 * past the store and the compares that consume it (the mixed kernel's shape:
 * addpd; movupd [mem]; ucomisd lane 0; jbe; unpckhpd; ucomisd lane 1; jbe).
 * Particles are seeded so that NaNs appear in lane 0 only, lane 1 only, both,
 * with x86's sign for a generated inf - inf, with x86's operand precedence
 * for a qNaN + sNaN pair, and on the bounce (side-exit) paths.  Every field
 * is printed bit-exactly; goldens come from the native binary. */
#include "gsys.h"

struct particle { double x, y, vx, vy; g_u64 id; };
static struct particle ps[16];
static volatile int v_n = 3;

static double bits(g_u64 u) { union { g_u64 u; double d; } c; c.u = u; return c.d; }
static g_u64 ubits(double d) { union { g_u64 u; double d; } c; c.d = d; return c.u; }

static g_u64 run(int n)
{
    g_u64 acc = 0;
    for (int r = 0; r < n; r++) {
        for (int i = 0; i < 16; i++) {
            struct particle *p = &ps[i];
            p->x += p->vx; p->y += p->vy;
            if (p->x > 100.0) { p->x = 0; p->vx = -p->vx; }
            if (p->y < -100.0) { p->y = 0; p->vy = -p->vy; }
            if ((p->id ^ (g_u64)r) & 1) acc += (g_u64)p->x; else acc ^= (g_u64)(-p->y);
        }
    }
    return acc;
}

int main(int argc, char **argv, char **envp)
{
    const double inf = bits(0x7ff0000000000000ull), ninf = bits(0xfff0000000000000ull);
    const double qa = bits(0x7ff8000000000aaaull);    /* quiet NaN, payload aaa */
    const double sb = bits(0x7ff4000000000bbbull);    /* signalling NaN, payload bbb */
    for (int i = 0; i < 16; i++) { ps[i].x = i * 7; ps[i].y = -i * 9; ps[i].vx = 0.5; ps[i].vy = 0.25; ps[i].id = (g_u64)i; }
    ps[1].x = ninf; ps[1].vx = inf;                 /* lane 0: generated NaN, no bounce */
    ps[2].y = inf;  ps[2].vy = ninf;                /* lane 1: generated NaN, no bounce */
    ps[3].x = ninf; ps[3].vx = inf; ps[3].y = inf; ps[3].vy = ninf;   /* both lanes */
    ps[4].x = qa;   ps[4].vx = sb;                  /* qNaN + sNaN: x86 keeps the destination's */
    ps[5].y = sb;   ps[5].vy = qa;                  /* sNaN + qNaN in lane 1 */
    ps[6].x = 500;  ps[6].y = inf; ps[6].vy = ninf; /* x bounces (side exit) while y turns NaN */
    ps[7].x = 500;  ps[7].y = qa;                   /* x bounces while y carries a NaN payload */
    ps[8].y = -500; ps[8].x = ninf; ps[8].vx = inf; /* y bounces while x turns NaN */
    ps[9].x = 500;  ps[9].y = -500;                 /* both bounce, no NaN */
    ps[10].x = qa;  ps[10].vx = 0.5; ps[10].y = -500; ps[10].vy = sb;   /* propagated + generated */
    g_u64 acc = run(v_n);
    g_puts("acc "); g_putu64(acc);
    for (int i = 0; i < 16; i++) {
        g_puts("p"); g_putu64((g_u64)i);
        g_puts(" x "); g_putu64(ubits(ps[i].x));
        g_puts(" y "); g_putu64(ubits(ps[i].y));
        g_puts(" vx "); g_putu64(ubits(ps[i].vx));
        g_puts(" vy "); g_putu64(ubits(ps[i].vy));
    }
    return 0;
}
