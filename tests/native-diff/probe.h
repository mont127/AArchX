/*
 * The probes tools/native-diff.py runs under Rosetta, cache mode and native mode and compares line by line.
 *
 * Every line is "key = value".  A value with a type prefix is compared by type: d: and f: are a double's and a
 * float's bits, D: a long double's ten bytes, so a difference can be measured in units in the last place, and a
 * long double's in what is left after rounding to a double, all native mode keeps of one.
 *
 * Each CASE runs in a child of its own (fork), so a crash, an abort or native mode's refusal (exit 71 or 72) in one
 * is that case's value ("<signal 11>", "<exit 72>") and the probe goes on.  The child builds its lines in a buffer
 * with the out_* helpers, which format numbers themselves rather than through printf (a printf probe is the place
 * printf is tested), and writes them with one write(2) at the end.  The parent does nothing but fork and wait, so it
 * stays single-threaded and CoreFoundation and Foundation work in every child, as they do for a program on an Intel
 * Mac.  probe_no_fork() runs the cases in the probe's own process instead, in order, where a crash ends the probe.
 */
#ifndef NATIVE_DIFF_PROBE_H
#define NATIVE_DIFF_PROBE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int probe_forks = 1;
static char probe_buf[1 << 16];
static size_t probe_len;
static char probe_case[256];

static void probe_no_fork(void) { probe_forks = 0; }

static void out_raw(const char *s, size_t n)
{
    if (probe_len + n > sizeof probe_buf)
        n = sizeof probe_buf - probe_len;
    memcpy(probe_buf + probe_len, s, n);
    probe_len += n;
}
static void out_s(const char *s) { if (!s) s = "(null)"; out_raw(s, strlen(s)); }

/* a value's bytes as hex, and the string printable: other bytes as \xHH */
static void out_x(uint64_t v, int digits)
{
    char b[17];
    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        b[i] = "0123456789abcdef"[v & 15];
    out_raw(b, (size_t)digits);
}
static void out_u(uint64_t v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    out_s(b + i);
}
static void out_d(int64_t v)
{
    if (v < 0) {
        out_s("-");
        out_u((uint64_t)0 - (uint64_t)v);
    } else {
        out_u((uint64_t)v);
    }
}
static void out_q(const char *s, size_t n)
{
    out_s("\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out_raw((const char *)&c, 1);
        } else {
            out_s("\\x");
            out_x(c, 2);
        }
    }
    out_s("\"");
}
static void out_dbl(double v) { uint64_t b; memcpy(&b, &v, 8); out_s("d:"); out_x(b, 16); }
static void out_flt(float v) { uint32_t b; memcpy(&b, &v, 4); out_s("f:"); out_x(b, 8); }
static void out_ldbl(long double v)
{
    unsigned char b[16] = { 0 };
    memcpy(b, &v, sizeof v < 10 ? sizeof v : 10);
    out_s("D:");
    for (int i = 9; i >= 0; i--)
        out_x(b[i], 2);
}

/* one "key = " line start; the key is the case's name and an optional suffix */
static void out_key(const char *suffix)
{
    out_s(probe_case);
    if (suffix && *suffix) {
        out_s("/");
        out_s(suffix);
    }
    out_s(" = ");
}
static void out_end(void) { out_s("\n"); }

static void probe_flush(void)
{
    size_t off = 0;
    while (off < probe_len) {
        ssize_t w = write(1, probe_buf + off, probe_len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    probe_len = 0;
}

static int probe_begin(const char *name)
{
    size_t n = strlen(name);
    if (n >= sizeof probe_case)
        n = sizeof probe_case - 1;
    memcpy(probe_case, name, n);
    probe_case[n] = 0;
    probe_flush();
    if (!probe_forks)
        return 1;
    pid_t p = fork();
    if (p == 0)
        return 1;
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) != p || WIFSIGNALED(st) || WEXITSTATUS(st)) {
        out_key(NULL);
        if (p < 0)
            out_s("<fork failed>");
        else if (WIFSIGNALED(st)) {
            out_s("<signal ");
            out_d(WTERMSIG(st));
            out_s(">");
        } else {
            out_s("<exit ");
            out_d(WEXITSTATUS(st));
            out_s(">");
        }
        out_end();
        probe_flush();
    }
    return 0;
}

static int probe_end(void)
{
    probe_flush();
    if (probe_forks)
        _exit(0);
    return 0;
}

/* CASE("name") { out_key(...); ...; out_end(); } */
#define CASE(name) for (int case_once_ = probe_begin(name); case_once_; case_once_ = probe_end())

#endif
