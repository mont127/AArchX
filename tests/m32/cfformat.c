/* Variadic calls the API database cannot describe: CFStringCreateWithFormat (and its va_list form), whose CFString
 * format can hold %@, and ioctl's one argument. MW3 released the NULL the unsupported call returned.
 * cfformat.expected is the x86_64 build's output under Rosetta. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static CFStringRef fmt_v(CFStringRef fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, fmt, ap);
    va_end(ap);
    return s;
}

static void show(const char *what, CFStringRef s)
{
    char buf[256] = "(null)";
    if (s) {
        CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(s);
    }
    printf("%s [%s]\n", what, buf);
}

int main(void)
{
    show("format", CFStringCreateWithFormat(NULL, NULL, CFSTR("%d %s %@ %.2f %lld %x"), 42, "text", CFSTR("object"), 2.5,
                                            1LL << 40, 0xbeef));
    show("va_list", fmt_v(CFSTR("%@/%05u"), CFSTR("dir"), 7u));
    char path[] = "/tmp/m32cfformatXXXXXX";
    int fd = mkstemp(path), n = -1;
    write(fd, "hello", 5);
    lseek(fd, 0, SEEK_SET);
    int r = ioctl(fd, FIONREAD, &n);
    close(fd);
    unlink(path);
    printf("ioctl %d FIONREAD %d\n", r, n);
    return 0;
}
