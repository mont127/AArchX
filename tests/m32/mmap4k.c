/* MAP_FIXED anonymous mappings at 4 KB granularity, as the i386 Intel Macs allowed (MW3's VirtualAlloc commits and
 * decommits single pages this way): the pages asked for read as zeros, every other byte is untouched.
 * mmap4k.expected is the x86_64 build's output under Rosetta, which keeps 4 KB pages. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *base;

static void fixed(unsigned off, unsigned len)
{
    void *p = mmap(base + off, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    printf("fixed %#x+%#x %s\n", off, len, p == base + off ? "ok" : "failed");
}

int main(void)
{
    base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(base, 0xab, 0x10000);
    fixed(0x1000, 0x1000);   /* inside one 16 KB host page */
    fixed(0x4000, 0x1000);   /* at a 16 KB boundary */
    fixed(0x7000, 0x2000);   /* across one */
    fixed(0x9000, 0x6000);   /* a whole 16 KB page and parts of two others */
    unsigned zero = 0, kept = 0;
    for (unsigned i = 0; i < 0x10000; i++) {
        int cleared = (i >= 0x1000 && i < 0x2000) || (i >= 0x4000 && i < 0x5000) || (i >= 0x7000 && i < 0x9000) ||
                      (i >= 0x9000 && i < 0xf000);
        if (cleared)
            zero += base[i] == 0;
        else
            kept += base[i] == 0xab;
    }
    printf("zeroed %#x kept %#x\n", zero, kept);
    return 0;
}
