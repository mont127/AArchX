/* String and memory-movement instructions over a 64KB buffer. */
#include "gsys.h"

void *memcpy(void *dst, const void *src, g_u64 n);
void *memset(void *dst, int c, g_u64 n);
void *memmove(void *dst, const void *src, g_u64 n);
int memcmp(const void *a, const void *b, g_u64 n);
g_u64 strlen(const char *s);
int strcmp(const char *a, const char *b);

#define BUFSZ (64 * 1024)

static unsigned char src[BUFSZ];
static unsigned char dst[BUFSZ];
static unsigned char tmp[BUFSZ];
static unsigned char fillbuf[4096];
static unsigned char movbuf[4096];

static g_u64 checksum(const unsigned char *p, g_u64 n)
{
    g_u64 h = 1469598103934665603ULL;
    for (g_u64 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int main(int argc, char **argv, char **envp)
{
    for (int i = 0; i < BUFSZ; i++)
        src[i] = (unsigned char)((i * 31 + (i >> 3) * 17 + 7) & 0xff);
    g_putu64(checksum(src, BUFSZ));

    memset(dst, 0, BUFSZ);
    g_putu64(checksum(dst, BUFSZ));

    memcpy(dst, src, BUFSZ);
    g_putu64(checksum(dst, BUFSZ));

    g_putu64((g_u64)memcmp(dst, src, BUFSZ));

    memmove(dst + 100, dst, BUFSZ - 100);
    g_putu64(checksum(dst, BUFSZ));

    memmove(tmp, src + 200, BUFSZ - 200);
    g_putu64(checksum(tmp, BUFSZ - 200));

    char s1[] = "the quick brown fox jumps over the lazy dog";
    char s2[] = "the quick brown fox jumps over the lazy dog";
    char s3[] = "the quick brown fox jumps over the lazy cat";
    g_putu64(strlen(s1));
    g_putu64((g_u64)(strcmp(s1, s2) == 0 ? 1 : 0));
    g_putu64((g_u64)(strcmp(s1, s3) != 0 ? 1 : 0));

    g_u64 stracc = 0;

    {
        void *p = fillbuf;
        g_u64 cnt = sizeof(fillbuf);
        int val = 0x5a;
        __asm__ volatile(
            "cld\n\t"
            "rep stosb"
            : "+D"(p), "+c"(cnt)
            : "a"(val)
            : "memory");
    }
    stracc = checksum(fillbuf, sizeof(fillbuf));
    g_putu64(stracc);

    {
        const void *s = fillbuf;
        void *d = movbuf;
        g_u64 cnt = sizeof(movbuf);
        __asm__ volatile(
            "cld\n\t"
            "rep movsb"
            : "+S"(s), "+D"(d), "+c"(cnt)
            :
            : "memory");
    }
    g_putu64(checksum(movbuf, sizeof(movbuf)));

    fillbuf[1234] = 0x99;
    {
        const void *p = fillbuf;
        g_u64 cnt = sizeof(fillbuf);
        int needle = 0x99;
        g_u64 remaining;
        __asm__ volatile(
            "cld\n\t"
            "repne scasb"
            : "+D"(p), "+c"(cnt)
            : "a"(needle)
            : "memory", "cc");
        remaining = cnt;
        g_putu64(sizeof(fillbuf) - remaining - 1);
    }

    return 0;
}
