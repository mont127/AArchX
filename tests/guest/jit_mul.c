/* One-operand mul/imul (64/32, register and memory source) with CF/OF via setcc,
 * printed raw; golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    volatile g_u64 vals[6] = { 3, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x100000000ULL, 0x7fffffffULL, 0xfffffffeULL };
    for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
        g_u64 a = vals[i], b = vals[j], lo, hi, cf, of;
        __asm__ volatile("mulq %4\n\tsetc %b2\n\tseto %b3" : "=a"(lo), "=d"(hi), "=r"(cf), "=r"(of) : "r"(b), "a"(a) : "cc");
        put("mulq lo ", lo); put("mulq hi ", hi); put("mulq cf ", cf & 0xff); put("mulq of ", of & 0xff);
        __asm__ volatile("imulq %4\n\tsetc %b2\n\tseto %b3" : "=a"(lo), "=d"(hi), "=r"(cf), "=r"(of) : "r"(b), "a"(a) : "cc");
        put("imulq lo ", lo); put("imulq hi ", hi); put("imulq cf ", cf & 0xff);
        g_u32 a32 = (g_u32)a, b32 = (g_u32)b, lo32, hi32;
        __asm__ volatile("mull %4\n\tsetc %b2\n\tseto %b3" : "=a"(lo32), "=d"(hi32), "=r"(cf), "=r"(of) : "r"(b32), "a"(a32) : "cc");
        put("mull lo ", lo32); put("mull hi ", hi32); put("mull cf ", cf & 0xff);
        __asm__ volatile("imull %4\n\tsetc %b2\n\tseto %b3" : "=a"(lo32), "=d"(hi32), "=r"(cf), "=r"(of) : "r"(b32), "a"(a32) : "cc");
        put("imull lo ", lo32); put("imull hi ", hi32); put("imull cf ", cf & 0xff);
        volatile g_u64 mb = b;
        __asm__ volatile("mulq %3\n\tsetc %b2" : "=a"(lo), "=d"(hi), "=r"(cf) : "m"(mb), "a"(a) : "cc");
        put("mulq mem lo ", lo); put("mulq mem hi ", hi); put("mulq mem cf ", cf & 0xff);
    }
    return 0;
}
