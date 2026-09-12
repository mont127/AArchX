/* A scalar-double op immediately followed by cvttsd2si of its result: the JIT
 * merges the op's NaN branch into the conversion's check, and the out-of-line
 * arm must first repair the NaN exactly (x86 sign for a generated one, the
 * destination's payload when both inputs are NaN), then produce the integer
 * indefinite.  The repaired value is stored afterwards, so a wrong payload
 * would show.  Saturation (inf, huge) must take the same arm without touching
 * the value.  Goldens come from the native binary. */
#include "gsys.h"

static double bits(g_u64 u) { union { g_u64 u; double d; } c; c.u = u; return c.d; }
static g_u64 ubits(double d) { union { g_u64 u; double d; } c; c.d = d; return c.u; }

static void one(const char *tag, double a, double b)
{
    g_u64 r;
    double ra = a;
    __asm__ __volatile__("subsd %[b], %[a]\n\tcvttsd2si %[a], %[r]"
                         : [a] "+x"(ra), [r] "=r"(r) : [b] "x"(b));
    g_puts(tag); g_puts(" r "); g_putu64(r); g_puts(" d "); g_putu64(ubits(ra));
    g_u64 r32;
    double rb = a;
    __asm__ __volatile__("mulsd %[b], %[a]\n\tcvttsd2si %[a], %k[r]"
                         : [a] "+x"(rb), [r] "=r"(r32) : [b] "x"(b));
    g_puts(tag); g_puts(" r32 "); g_putu64(r32 & 0xffffffffull); g_puts(" d "); g_putu64(ubits(rb));
}

int main(int argc, char **argv, char **envp)
{
    const double inf = bits(0x7ff0000000000000ull);
    const double qa = bits(0x7ff8000000000aaaull), sb = bits(0x7ff4000000000bbbull);
    one("gen  ", inf, inf);
    one("prop ", qa, 1.0);
    one("props", 1.0, sb);
    one("both ", qa, sb);
    one("sat  ", 1e300, -1e300);
    one("big  ", 9.3e18, 0.0);
    one("neg  ", -9.3e18, 0.0);
    one("norm ", 123456.75, 0.25);
    one("zero ", 0.5, 0.5);
    return 0;
}
