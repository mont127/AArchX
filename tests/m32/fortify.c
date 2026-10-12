/* __strlcpy_chk and __strlcat_chk, which _FORTIFY_SOURCE builds for macOS 10.9 and later call for strlcpy and
 * strlcat (MW3's game program does): called directly, since this program targets 10.8 */
#include <stdio.h>
#include <string.h>

size_t __strlcpy_chk(char *, const char *, size_t, size_t);
size_t __strlcat_chk(char *, const char *, size_t, size_t);

int main(void)
{
    char buf[8];
    size_t n = __strlcpy_chk(buf, "truncated here", sizeof buf, sizeof buf);
    printf("strlcpy %zu [%s]\n", n, buf);
    __strlcpy_chk(buf, "ab", sizeof buf, sizeof buf);
    n = __strlcat_chk(buf, "cdefgh", sizeof buf, sizeof buf);
    printf("strlcat %zu [%s]\n", n, buf);
    return 0;
}
