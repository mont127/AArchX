#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xlocale.h>

extern int checked_vsprintf(char *, int, size_t, const char *, va_list) __asm__("___vsprintf_chk");
extern int checked_vsnprintf(char *, size_t, int, size_t, const char *, va_list) __asm__("___vsnprintf_chk");

static void check_list(const char *expected, const char *format, va_list args)
{
    char buffer[1024];
    va_list copy;
    int length = (int)strlen(expected);
    va_copy(copy, args);
    assert(vsnprintf(buffer, sizeof buffer, format, copy) == length);
    va_end(copy);
    assert(strcmp(buffer, expected) == 0);
    va_copy(copy, args);
    assert(vsnprintf(buffer, 4, format, copy) == length);
    va_end(copy);
    assert(strncmp(buffer, expected, 3) == 0 && buffer[3] == 0);
    va_copy(copy, args);
    assert(vsnprintf(NULL, 0, format, copy) == length);
    va_end(copy);
    va_copy(copy, args);
    assert(vsprintf(buffer, format, copy) == length);
    va_end(copy);
    assert(strcmp(buffer, expected) == 0);
    char *allocated = NULL;
    va_copy(copy, args);
    assert(vasprintf(&allocated, format, copy) == length);
    va_end(copy);
    assert(allocated && strcmp(allocated, expected) == 0);
    free(allocated);
    va_copy(copy, args);
    assert(checked_vsprintf(buffer, 0, sizeof buffer, format, copy) == length);
    va_end(copy);
    assert(strcmp(buffer, expected) == 0);
    va_copy(copy, args);
    assert(checked_vsnprintf(buffer, sizeof buffer, 0, sizeof buffer, format, copy) == length);
    va_end(copy);
    assert(strcmp(buffer, expected) == 0);
    locale_t locale = newlocale(LC_ALL_MASK, "C", NULL);
    assert(locale);
    va_copy(copy, args);
    assert(vsnprintf_l(buffer, sizeof buffer, locale, format, copy) == length);
    va_end(copy);
    freelocale(locale);
    assert(strcmp(buffer, expected) == 0);
    FILE *file = tmpfile();
    assert(file);
    va_copy(copy, args);
    assert(vfprintf(file, format, copy) == length);
    va_end(copy);
    rewind(file);
    memset(buffer, 0, sizeof buffer);
    assert(fread(buffer, 1, sizeof buffer, file) == (size_t)length);
    assert(strcmp(buffer, expected) == 0);
    fclose(file);
    int fds[2];
    assert(pipe(fds) == 0);
    va_copy(copy, args);
    assert(vdprintf(fds[1], format, copy) == length);
    va_end(copy);
    close(fds[1]);
    memset(buffer, 0, sizeof buffer);
    assert(read(fds[0], buffer, sizeof buffer) == length);
    assert(strcmp(buffer, expected) == 0);
    close(fds[0]);
    va_copy(copy, args);
    errno = 0;
    assert(vdprintf(-1, format, copy) == -1 && errno == EBADF);
    va_end(copy);
    va_copy(copy, args);
    assert(vprintf(format, copy) == length);
    va_end(copy);
    puts("");
}

static void check(const char *expected, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    check_list(expected, format, args);
    va_end(args);
}

static void check_consumed(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    assert(va_arg(args, int) == 123);
    assert(va_arg(args, double) == 2.5);
    check_list("consumed -9 3.25", format, args);
    va_end(args);
}

static void refuse_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *result = NULL;
    vasprintf(&result, format, args);
    va_end(args);
    free(result);
}

static char *allocate_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *result = NULL;
    assert(vasprintf(&result, format, args) > 0);
    va_end(args);
    return result;
}

static void *format_thread(void *arg)
{
    int index = (int)(uintptr_t)arg;
    for (int i = 0; i < 128; i++) {
        char expected[128];
        snprintf(expected, sizeof expected, "thread=%d item=%d value=%.2f text=ok", index, i, index + 0.5);
        char *result = allocate_format("thread=%d item=%d value=%.2f text=%s", index, i, index + 0.5, "ok");
        assert(result && strcmp(result, expected) == 0);
        free(result);
    }
    return arg;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "reject-positional") == 0)
            refuse_format("%1$d", 1);
        else if (strcmp(argv[1], "reject-long-double") == 0)
            refuse_format("%Lf", (long double)1.5);
        else if (strcmp(argv[1], "reject-writeback") == 0) {
            int count = 0;
            refuse_format("%n", &count);
        }
        return 1;
    }
    check("plain percent %", "plain percent %%");
    check("-7 text 3.50 -5000000000 0x1234 k%", "%d %s %.*f %lld %p %c%%",
          -7, "text", 2, 3.5, -5000000000LL, (void *)0x1234, 'k');
    check("1/1.5 2/2.5 3/3.5 4/4.5 5/5.5 6/6.5 7/7.5 8/8.5 9/9.5 10/10.5",
          "%d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f %d/%.1f",
          1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5);
    check_consumed("consumed %d %.2f", 123, 2.5, -9, 3.25);
    check("    3.50", "%*.*f", 8, 2, 3.5);
    check("255 65535 4294967295 18446744073709551615 -1 -2 42 -43",
          "%hhu %hu %u %llu %hhd %hd %zu %td", 255, 65535, ~0u, ~0ull, -1, -2, (size_t)42, (ptrdiff_t)-43);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, format_thread, (void *)i) == 0);
    for (uintptr_t i = 0; i < 4; i++) {
        void *result = NULL;
        assert(pthread_join(threads[i], &result) == 0 && result == (void *)i);
    }
    puts("native va_list formats ok");
    return 0;
}
