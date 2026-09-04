/* Machdep-class syscalls (0x3000000): thread_set_tsd_base returns through rax
 * alone.  rdx survives it (Wine's syscall dispatcher keeps its frame pointer
 * there across the call), rax is the base, and the arithmetic flags come back
 * as AF.  The unix class is different: rdx is zeroed there on success and on
 * error alike.  A freestanding program has no TLS base of its own, so the
 * test installs a static buffer and reads it back through gs.  Goldens come
 * from the native binary. */
#include "gsys.h"

static g_u64 tsd[16];

int main(int argc, char **argv, char **envp)
{
    g_u64 base = (g_u64)tsd;
    tsd[0] = 0x6d616368646570ull;              /* "machdep": what gs:0 must read afterwards */

    g_u64 rax = 0x3000003, rdx = 0x1234, fl;
    __asm__ __volatile__("syscall\n\tpushfq\n\tpopq %[fl]"
                         : "+a"(rax), "+d"(rdx), [fl] "=r"(fl) : "D"(base) : "rcx", "r11", "memory", "cc");
    g_puts("tsd_base rax_is_base "); g_putu64(rax == base);
    g_puts(" rdx "); g_putu64(rdx);
    g_puts(" flags "); g_putu64(fl & 0x8d5);

    g_u64 seen;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(seen));
    g_puts("gs0 "); g_putu64(seen);

    /* unix class for contrast: getpid with rdx preloaded */
    rax = 0x2000014; rdx = 0x5678;
    __asm__ __volatile__("syscall\n\tpushfq\n\tpopq %[fl]"
                         : "+a"(rax), "+d"(rdx), [fl] "=r"(fl) : : "rcx", "r11", "memory", "cc");
    g_puts("getpid rdx "); g_putu64(rdx); g_puts(" flags "); g_putu64(fl & 0x8d5);
    return 0;
}
