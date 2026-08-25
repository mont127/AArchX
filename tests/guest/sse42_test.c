/* SSE4.2: crc32 and the string units, against values verified under Rosetta. */
#include "gsys.h"

static const char msg[] = "The quick brown fox jumps over the lazy dog";

int main(void) {
    unsigned long long crc = 0xffffffffull;
    for (int i = 0; msg[i]; i++)
        __asm__("crc32b %1, %0" : "+r"(crc) : "r"(msg[i]));
    crc ^= 0xffffffffull;
    g_puts("crc "); g_putu64(crc); g_puts("\n");

    __attribute__((aligned(16))) char a1[16] = "Hello world 1234";
    __attribute__((aligned(16))) char b1[16] = "Hello world 12x4";
    unsigned long long idx;
    __asm__("movdqa %1, %%xmm1; movdqa %2, %%xmm2;"
            "pcmpistri $0x18, %%xmm2, %%xmm1; movq %%rcx, %0"
            : "=r"(idx) : "m"(a1), "m"(b1) : "rcx", "xmm1", "xmm2");
    g_puts("mismatch "); g_putu64(idx); g_puts("\n");

    __attribute__((aligned(16))) char nee[16] = "fox";
    __attribute__((aligned(16))) char hay[16] = "quick brown fox";
    __asm__("movdqa %1, %%xmm1; movdqa %2, %%xmm2;"
            "pcmpistri $0x0c, %%xmm2, %%xmm1; movq %%rcx, %0"
            : "=r"(idx) : "m"(nee), "m"(hay) : "rcx", "xmm1", "xmm2");
    g_puts("substr "); g_putu64(idx); g_puts("\n");
    return 0;
}
