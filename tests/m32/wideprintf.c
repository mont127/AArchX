/* The wide printf and scanf families from i386 code: swprintf, vswprintf (Left 4 Dead 2's vgui formats its text with
 * them) and swscanf; wchar_t is 4 bytes on both sides. wideprintf.expected is the x86_64 build's output under Rosetta. */
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

static int fmt_v(wchar_t *out, size_t n, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

int main(void)
{
    wchar_t buf[128];
    int r = swprintf(buf, 128, L"%d %ls %s %.2f %lld %x", 42, L"wide", "narrow", 2.5, 1LL << 40, 0xbeef);
    printf("swprintf %d [%ls]\n", r, buf);
    r = fmt_v(buf, 128, L"%ls/%05u", L"dir", 7u);
    printf("vswprintf %d [%ls]\n", r, buf);
    int a = 0, b = 0;
    wchar_t word[16] = L"";
    r = swscanf(L"12 34 name", L"%d %d %ls", &a, &b, word);
    printf("swscanf %d %d %d [%ls]\n", r, a, b, word);
    return 0;
}
