/* Minimal freestanding C runtime for the guest tests. */
typedef unsigned long um_size;

__attribute__((no_builtin)) void *memcpy(void *dst, const void *src, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (um_size i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

__attribute__((no_builtin)) void *memset(void *dst, int c, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    for (um_size i = 0; i < n; i++)
        d[i] = v;
    return dst;
}

__attribute__((no_builtin)) void __bzero(void *dst, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    for (um_size i = 0; i < n; i++)
        d[i] = 0;
}

__attribute__((no_builtin)) void bzero(void *dst, um_size n)
{
    __bzero(dst, n);
}

__attribute__((no_builtin)) void *memmove(void *dst, const void *src, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        for (um_size i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (um_size i = n; i != 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

__attribute__((no_builtin)) int memcmp(const void *a, const void *b, um_size n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (um_size i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

__attribute__((no_builtin)) um_size strlen(const char *s)
{
    um_size n = 0;
    while (s[n])
        n++;
    return n;
}

__attribute__((no_builtin)) int strcmp(const char *a, const char *b)
{
    um_size i = 0;
    while (a[i] != 0 && a[i] == b[i])
        i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
