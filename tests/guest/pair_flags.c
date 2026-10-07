/* add ; inc fused into one pair, with a reader of the add's carry after it in
 * the same block.  The translate loop went on naming the add as the latest
 * flag producer, so a setb or adc after the pair evaluated the inc's record
 * as an add's and took its carry from the record's CF bit plus the result.
 * The seto keeps the inc's own flags live, which is what makes the JIT fuse
 * the pair.  Golden from the native run.
 */
#include "gsys.h"

static volatile unsigned int A32[5] = { 0x10u, 0x10u, 0x7fffffffu, 0xffffffffu, 0xfffffff0u };
static volatile unsigned int B32[5] = { 0xfffffff2u, 0x20u, 0x0u, 0x1u, 0x0fu };
static volatile g_u64 A64[3] = { 0x10ull, 0x7fffffffffffffffull, 0xffffffffffffff00ull };
static volatile g_u64 B64[3] = { 0xfffffffffffffff2ull, 0x0ull, 0x100ull };

static __attribute__((noinline)) g_u64 setb32(unsigned a, unsigned b)
{
    unsigned char cf, of;
    __asm__ __volatile__("addl %[b], %[a]\n\tincl %[a]\n\tsetb %[cf]\n\tseto %[of]\n\t"
                         : [a] "+r"(a), [cf] "=&q"(cf), [of] "=&q"(of) : [b] "r"(b) : "cc");
    return (g_u64)a | ((g_u64)cf << 32) | ((g_u64)of << 40);
}

static __attribute__((noinline)) g_u64 adc32(unsigned a, unsigned b)
{
    unsigned c = 100;
    unsigned char of;
    __asm__ __volatile__("addl %[b], %[a]\n\tincl %[a]\n\tadcl $0, %[c]\n\tseto %[of]\n\t"
                         : [a] "+r"(a), [c] "+r"(c), [of] "=&q"(of) : [b] "r"(b) : "cc");
    return (g_u64)a | ((g_u64)c << 32) | ((g_u64)of << 48);
}

static __attribute__((noinline)) g_u64 setb64(g_u64 a, g_u64 b, g_u64 *res)
{
    unsigned char cf, of;
    __asm__ __volatile__("addq %[b], %[a]\n\tincq %[a]\n\tsetb %[cf]\n\tseto %[of]\n\t"
                         : [a] "+r"(a), [cf] "=&q"(cf), [of] "=&q"(of) : [b] "r"(b) : "cc");
    *res = a;
    return (g_u64)cf | ((g_u64)of << 8);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 5; i++) {
            g_putu64_nonl(setb32(A32[i], B32[i])); g_puts(" ");
            g_putu64_nonl(adc32(A32[i], B32[i])); g_puts(" ");
        }
        for (int i = 0; i < 3; i++) {
            g_u64 r = 0;
            g_putu64_nonl(setb64(A64[i], B64[i], &r)); g_puts(" ");
            g_putu64_nonl(r); g_puts(i == 2 ? "" : " ");
        }
        g_puts("\n");
    }
    return 0;
}
