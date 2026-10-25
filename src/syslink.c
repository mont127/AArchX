/*
 * ocerz links libSystem's sub-libraries themselves (libsystem_c, libsystem_kernel, ... : Makefile, SYSLIBS) and
 * not only libSystem.  dyld looks a symbol up in the library its import names, and through libSystem that is a walk
 * of its forty re-exported libraries: 50 thousand instructions an import, 0.75 ms of every launch of every program
 * for ocerz's 380 of them, before main.  The sub-libraries are linked weak, so an import a later macOS has moved to
 * another sub-library loads as NULL instead of stopping the launch; before anything else runs, this checks the
 * imports (the GOT, and each thread-local descriptor's thunk) and, finding one NULL, executes ocerz-umbrella beside
 * the real executable, the same program linked against libSystem alone.  It uses nothing it might be checking: the
 * kernel's executable_path, raw readlink, execve and write, its own string loops.  OCERZ_TEST_SYSLINK_FALLBACK=1
 * takes the fallback without a NULL, for the tests.
 */
#include <stddef.h>
#include <stdint.h>

extern void *const ocerz_got_start __asm__("section$start$__DATA_CONST$__got");
extern void *const ocerz_got_end __asm__("section$end$__DATA_CONST$__got");
extern char ocerz_tlv_start __asm__("section$start$__DATA$__thread_vars");
extern char ocerz_tlv_end __asm__("section$end$__DATA$__thread_vars");

#define SL_ATTR __attribute__((no_builtin, no_stack_protector, used))

SL_ATTR static long sl_sys3(long n, long a, long b, long c)
{
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80\n\tb.cc 1f\n\tneg %0, %0\n1:" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}

SL_ATTR static size_t sl_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

SL_ATTR static int sl_prefix(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

SL_ATTR static void sl_say(const char *s)
{
    sl_sys3(4 /* write */, 2, (long)s, (long)sl_len(s));
}

SL_ATTR static int sl_imports_missing(void)
{
    for (void *const *p = &ocerz_got_start; p < &ocerz_got_end; p++)
        if (!*p)
            return 1;
    for (char *d = &ocerz_tlv_start; d + 3 * sizeof(void *) <= &ocerz_tlv_end; d += 3 * sizeof(void *))
        if (!*(void *const *)(void *)d)
            return 1;
    return 0;
}

/* path, with every symbolic link along its last component followed, in buf */
SL_ATTR static int sl_resolve(const char *path, char *buf, size_t cap)
{
    size_t n = sl_len(path);
    if (n + 1 > cap)
        return 0;
    for (size_t i = 0; i <= n; i++)
        buf[i] = path[i];
    for (int hop = 0; hop < 16; hop++) {
        char link[1024];
        long got = sl_sys3(58 /* readlink */, (long)buf, (long)link, (long)sizeof link - 1);
        if (got <= 0)
            return 1;
        link[got] = 0;
        if (link[0] == '/') {
            if ((size_t)got + 1 > cap)
                return 0;
            for (long i = 0; i <= got; i++)
                buf[i] = link[i];
        } else {
            size_t dir = sl_len(buf);
            while (dir && buf[dir - 1] != '/')
                dir--;
            if (dir + (size_t)got + 1 > cap)
                return 0;
            for (long i = 0; i <= got; i++)
                buf[dir + (size_t)i] = link[i];
        }
    }
    return 1;
}

SL_ATTR __attribute__((constructor)) static void sl_check(int argc, char **argv, char **envp, char **apple)
{
    (void)argc;
    int forced = 0;
    for (char **e = envp; e && *e && !forced; e++)
        forced = sl_prefix(*e, "OCERZ_TEST_SYSLINK_FALLBACK=1");
    if (!forced && !sl_imports_missing())
        return;
    const char *exe = NULL;
    for (char **a = apple; a && *a && !exe; a++)
        if (sl_prefix(*a, "executable_path="))
            exe = *a + 16;
    char path[2048];
    static const char name[] = "ocerz-umbrella";
    if (exe && sl_resolve(exe, path, sizeof path)) {
        size_t dir = sl_len(path);
        while (dir && path[dir - 1] != '/')
            dir--;
        if (dir + sizeof name <= sizeof path) {
            for (size_t i = 0; i < sizeof name; i++)
                path[dir + i] = name[i];
            /* the umbrella copy checks too and finds nothing missing: no second hop, but for the test's knob */
            char *env[512];
            char **pass = envp;
            if (forced) {
                size_t k = 0;
                for (char **e = envp; e && *e && k < 510; e++)
                    if (!sl_prefix(*e, "OCERZ_TEST_SYSLINK_FALLBACK="))
                        env[k++] = *e;
                env[k] = NULL;
                pass = env;
            }
            sl_sys3(59 /* execve */, (long)path, (long)argv, (long)pass);
        }
    }
    sl_say("ocerz: this macOS has moved a function ocerz takes from one of libSystem's libraries, and "
           "ocerz-umbrella, the copy that needs no such thing, is not beside ocerz: reinstall AArchX\n");
    sl_sys3(1 /* exit */, 127, 0, 0);
}
