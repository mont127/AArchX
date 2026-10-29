/*
 * ocerz -report [flags] program [args...]: a field report a tester can send (include/ocerz/report.h).
 *
 * A tester's log used to be whatever the terminal kept, from a command line built for that one bug, and it said
 * nothing about the Mac, the AArchX build or the program.  The report gathers that once and the same way for
 * everyone, into one text file:
 *
 *   - the AArchX version and when this ocerz was built, the Mac's model, chip and memory, the macOS version, whether
 *     Rosetta is installed and where the x86_64 shared cache cache mode maps is, if anywhere;
 *   - the program: its bundle's identifier and versions, its executable's architectures and the macOS each was
 *     built for, or the interpreter of a script;
 *   - the OCERZ_* and DYLD_* variables set, and the mode the program will run in and why;
 *   - for an x86_64 program, what ocerz -check says of it in native mode;
 *   - the run itself, made as `ocerz [flags] program [args]` would make it with OCERZ_ABORT_REPORT=1, so an abort
 *     in host code names its frames: how it ended and how long it took, every line ocerz printed (a crash report
 *     with its register lines), and the last lines of the program's own standard error.
 *
 * The run's standard error is shown on the terminal as it comes and kept; its standard output, the program's own,
 * is the terminal's and is not kept.  Ctrl-C ends the program, not the report.  Everything written is anonymized:
 * the home directory reads ~, any /Users/<name> /Users/<user>, the user's login and full name <user> and <name>,
 * and the computer's name <host>.  The file goes to the Desktop (OCERZ_REPORT_DIR names another directory), and
 * the last lines say where it is.
 */
#include <crt_externs.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ocerz/bundle.h"
#include "ocerz/report.h"
#include "ocerz/version.h"

typedef struct Buf { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

__attribute__((format(printf, 2, 3))) static void bprintf(Buf *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        bput(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* ---- anonymizing ---- */

static char g_home[PATH_MAX], g_user[256], g_name[256], g_host[256];

static void anon_init(void)
{
    struct passwd *pw = getpwuid(getuid());
    const char *home = getenv("HOME");
    snprintf(g_home, sizeof g_home, "%s", home && *home ? home : pw && pw->pw_dir ? pw->pw_dir : "");
    if (strlen(g_home) <= 1)
        g_home[0] = '\0';
    snprintf(g_user, sizeof g_user, "%s", pw && pw->pw_name ? pw->pw_name : getenv("USER") ? getenv("USER") : "");
    snprintf(g_name, sizeof g_name, "%s", pw && pw->pw_gecos ? pw->pw_gecos : "");
    char *comma = strchr(g_name, ',');
    if (comma)
        *comma = '\0';
    if (gethostname(g_host, sizeof g_host) != 0)
        g_host[0] = '\0';
    char *dot = strstr(g_host, ".local");
    if (dot)
        *dot = '\0';
}

static int word_at(const char *s, size_t n, size_t i, const char *w)
{
    size_t k = strlen(w);
    if (k < 2 || i + k > n || memcmp(s + i, w, k) != 0)
        return 0;
    int before = i == 0 || !isalnum((unsigned char)s[i - 1]);
    int after = i + k == n || !isalnum((unsigned char)s[i + k]);
    return before && after ? (int)k : 0;
}

/* s with the home directory, user directories, the login, the full name and the host name replaced */
static void anon(Buf *out, const char *s, size_t n)
{
    size_t home = strlen(g_home);
    for (size_t i = 0; i < n;) {
        int k;
        if (home && i + home <= n && memcmp(s + i, g_home, home) == 0 &&
            (i + home == n || s[i + home] == '/' || !isalnum((unsigned char)s[i + home]))) {
            bput(out, "~", 1);
            i += home;
        } else if (i + 7 <= n && memcmp(s + i, "/Users/", 7) == 0 && i + 7 < n && s[i + 7] != '/' &&
                   strncmp(s + i + 7, "Shared", 6) != 0) {
            bput(out, "/Users/<user>", 13);
            for (i += 7; i < n && s[i] != '/' && s[i] != '\n' && s[i] != '\'' && s[i] != '"'; i++)
                ;
        } else if ((k = word_at(s, n, i, g_name)) && strlen(g_name) >= 3) {
            bput(out, "<name>", 6);
            i += (size_t)k;
        } else if ((k = word_at(s, n, i, g_user))) {
            bput(out, "<user>", 6);
            i += (size_t)k;
        } else if ((k = word_at(s, n, i, g_host))) {
            bput(out, "<host>", 6);
            i += (size_t)k;
        } else {
            bput(out, s + i, 1);
            i++;
        }
    }
}

static void anon_str(Buf *out, const char *s) { anon(out, s, strlen(s)); }

/* ---- the Mac ---- */

static int sysctl_text(const char *name, char *out, size_t n)
{
    size_t len = n;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0 || len == 0)
        return 0;
    out[len < n ? len : n - 1] = '\0';
    return 1;
}

static uint64_t sysctl_num(const char *name)
{
    uint64_t v = 0;
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : 0;
}

static int plist_file_string(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char buf[16384], tag[256];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    snprintf(tag, sizeof tag, "<key>%s</key>", key);
    const char *k = strstr(buf, tag);
    const char *v = k ? strstr(k, "<string>") : NULL;
    const char *e = v ? strstr(v, "</string>") : NULL;
    if (!e || (size_t)(e - v - 8) >= n)
        return 0;
    memcpy(out, v + 8, (size_t)(e - v - 8));
    out[e - v - 8] = '\0';
    return 1;
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void mac_facts(Buf *r, const char *self)
{
    char s[256], t[256];
    struct stat st;
    bprintf(r, "AArchX %s", OCERZ_VERSION);
    if (stat(self, &st) == 0) {
        struct tm tm;
        gmtime_r(&st.st_mtime, &tm);
        strftime(t, sizeof t, "%Y-%m-%d %H:%M UTC", &tm);
        bprintf(r, ", this ocerz built %s", t);
    }
    bprintf(r, "\nocerz: ");
    anon_str(r, self);
    bprintf(r, "\n");
    if (sysctl_text("hw.model", s, sizeof s))
        bprintf(r, "Mac: %s", s);
    if (sysctl_text("machdep.cpu.brand_string", t, sizeof t))
        bprintf(r, ", %s", t);
    bprintf(r, ", %llu GB, %llu cores\n", (unsigned long long)(sysctl_num("hw.memsize") >> 30),
            (unsigned long long)sysctl_num("hw.ncpu"));
    if (plist_file_string("/System/Library/CoreServices/SystemVersion.plist", "ProductVersion", s, sizeof s)) {
        bprintf(r, "macOS %s", s);
        if (plist_file_string("/System/Library/CoreServices/SystemVersion.plist", "ProductBuildVersion", t, sizeof t))
            bprintf(r, " (%s)", t);
        bprintf(r, "\n");
    }
    bprintf(r, "Rosetta: %s\n", exists("/Library/Apple/usr/libexec/oah/libRosettaRuntime") ? "installed" : "not installed");
    static const char *const caches[] = {
        "/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld/dyld_shared_cache_x86_64",
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64" };
    const char *cache = NULL;
    for (int i = 0; i < 2 && !cache; i++)
        if (exists(caches[i]))
            cache = caches[i];
    bprintf(r, "x86_64 shared cache: %s\n", cache ? cache : "none (cache mode cannot run)");
}

/* ---- the program ---- */

static const char *cpu_name(int32_t type, int32_t sub)
{
    switch (type) {
    case CPU_TYPE_X86_64: return "x86_64";
    case CPU_TYPE_I386: return "i386";
    case CPU_TYPE_ARM64: return (sub & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E ? "arm64e" : "arm64";
    case CPU_TYPE_POWERPC: return "ppc";
    case CPU_TYPE_POWERPC64: return "ppc64";
    default: return "unknown";
    }
}

static void version_text(uint32_t v, char *out, size_t n)
{
    if (v & 0xff)
        snprintf(out, n, "%u.%u.%u", v >> 16, (v >> 8) & 0xff, v & 0xff);
    else
        snprintf(out, n, "%u.%u", v >> 16, (v >> 8) & 0xff);
}

/* the macOS a slice was built for and against, from its load commands */
static void slice_versions(int fd, uint64_t off, Buf *r)
{
    uint8_t buf[65536];
    ssize_t got = pread(fd, buf, sizeof buf, (off_t)off);
    if (got < (ssize_t)sizeof(struct mach_header))
        return;
    uint32_t magic = *(const uint32_t *)buf;
    size_t hdr = magic == MH_MAGIC_64 ? sizeof(struct mach_header_64) : magic == MH_MAGIC ? sizeof(struct mach_header) : 0;
    if (!hdr)
        return;
    const struct mach_header *mh = (const struct mach_header *)buf;
    size_t at = hdr;
    for (uint32_t i = 0; i < mh->ncmds && at + 8 <= (size_t)got; i++) {
        const struct load_command *lc = (const struct load_command *)(buf + at);
        if (lc->cmdsize < 8 || at + lc->cmdsize > (size_t)got)
            break;
        char a[32], b[32];
        if (lc->cmd == LC_BUILD_VERSION && lc->cmdsize >= sizeof(struct build_version_command)) {
            const struct build_version_command *bv = (const void *)lc;
            version_text(bv->minos, a, sizeof a);
            version_text(bv->sdk, b, sizeof b);
            bprintf(r, " (macOS %s and later, SDK %s)", a, b);
            return;
        }
        if (lc->cmd == LC_VERSION_MIN_MACOSX && lc->cmdsize >= sizeof(struct version_min_command)) {
            const struct version_min_command *vm = (const void *)lc;
            version_text(vm->version, a, sizeof a);
            version_text(vm->sdk, b, sizeof b);
            bprintf(r, " (macOS %s and later, SDK %s)", a, b);
            return;
        }
        at += lc->cmdsize;
    }
}

/* the executable's architectures; *x86_64 and *i386 say which it has */
static void slices(const char *path, Buf *r, int *x86_64, int *i386)
{
    *x86_64 = *i386 = 0;
    int fd = open(path, O_RDONLY);
    uint8_t buf[4096];
    ssize_t got = fd >= 0 ? pread(fd, buf, sizeof buf, 0) : -1;
    if (got < 8) {
        bprintf(r, "  not readable\n");
        if (fd >= 0)
            close(fd);
        return;
    }
    uint32_t magic = *(const uint32_t *)buf;
    if (buf[0] == '#' && buf[1] == '!') {
        char line[256];
        size_t k = 0;
        while (k + 2 < (size_t)got && k < sizeof line - 1 && buf[k + 2] != '\n') {
            line[k] = (char)buf[k + 2];
            k++;
        }
        line[k] = '\0';
        bprintf(r, "  a script for ");
        anon_str(r, line);
        bprintf(r, "\n");
    } else if (magic == MH_MAGIC_64 || magic == MH_MAGIC) {
        const struct mach_header *mh = (const struct mach_header *)buf;
        bprintf(r, "  %s", cpu_name(mh->cputype, mh->cpusubtype));
        slice_versions(fd, 0, r);
        bprintf(r, "\n");
        *x86_64 = mh->cputype == CPU_TYPE_X86_64;
        *i386 = mh->cputype == CPU_TYPE_I386;
    } else if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
        uint32_t n = __builtin_bswap32(*(const uint32_t *)(buf + 4));
        int wide = magic == FAT_CIGAM_64;
        size_t step = wide ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
        for (uint32_t i = 0; i < n && 8 + (i + 1) * step <= (size_t)got; i++) {
            const uint8_t *a = buf + 8 + i * step;
            int32_t type = (int32_t)__builtin_bswap32(*(const uint32_t *)a);
            int32_t sub = (int32_t)__builtin_bswap32(*(const uint32_t *)(a + 4));
            uint64_t off = wide ? __builtin_bswap64(*(const uint64_t *)(a + 8)) : __builtin_bswap32(*(const uint32_t *)(a + 8));
            bprintf(r, "  %s", cpu_name(type, sub));
            slice_versions(fd, off, r);
            bprintf(r, "\n");
            *x86_64 |= type == CPU_TYPE_X86_64;
            *i386 |= type == CPU_TYPE_I386;
        }
    } else {
        bprintf(r, "  not a Mach-O executable\n");
    }
    close(fd);
}

/* ---- running ---- */

/* argv run as a child of ours with OCERZ_ABORT_REPORT=1, its standard error kept in err (and shown when tee), and its
 * standard output too when both; answers the wait status, or -1 if it did not start */
static int run_capture(const char *self, char *const argv[], Buf *err, int tee, int both, int timeout_s, double *secs)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    if (both)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);

    char **env = *_NSGetEnviron();
    size_t ne = 0;
    while (env[ne])
        ne++;
    char **envp = calloc(ne + 2, sizeof *envp);
    size_t k = 0;
    for (size_t i = 0; i < ne; i++)
        if (strncmp(env[i], "OCERZ_ABORT_REPORT=", 19) != 0)
            envp[k++] = env[i];
    envp[k++] = "OCERZ_ABORT_REPORT=1";

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid;
    int rc = posix_spawn(&pid, self, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(envp);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    char chunk[16384];
    for (;;) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        int ready = poll(&p, 1, 1000);
        if (ready > 0) {
            ssize_t got = read(fds[0], chunk, sizeof chunk);
            if (got <= 0)
                break;
            if (tee)
                (void)!write(2, chunk, (size_t)got);
            if (err->n < (64u << 20))
                bput(err, chunk, (size_t)got);
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (timeout_s > 0 && t1.tv_sec - t0.tv_sec > timeout_s) {
            kill(pid, SIGKILL);
            bprintf(err, "\n(stopped after %d s)\n", timeout_s);
            break;
        }
    }
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (secs)
        *secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    return st;
}

static void signal_name(int sig, char *out, size_t n)
{
    snprintf(out, n, "SIG%s", sig > 0 && sig < NSIG ? sys_signame[sig] : "?");
    for (char *p = out + 3; *p; p++)
        *p = (char)toupper((unsigned char)*p);
}

static void status_text(int st, Buf *r)
{
    char sn[32];
    if (WIFSIGNALED(st)) {
        signal_name(WTERMSIG(st), sn, sizeof sn);
        bprintf(r, "killed by signal %d (%s)", WTERMSIG(st), sn);
    } else if (WEXITSTATUS(st) > 128 && WEXITSTATUS(st) < 128 + NSIG) {
        /* ocerz ends a program that crashed with 128 and the signal, after its crash report */
        signal_name(WEXITSTATUS(st) - 128, sn, sizeof sn);
        bprintf(r, "exit %d: it crashed (%s)", WEXITSTATUS(st), sn);
    } else if (WEXITSTATUS(st) == 71)
        bprintf(r, "exit 71: an import did not bind, so native mode did not start it");
    else if (WEXITSTATUS(st) == 72)
        bprintf(r, "exit 72: it called something native mode has no crossing for");
    else
        bprintf(r, "exit %d", WEXITSTATUS(st));
}

/* ocerz's lines (and the indented lines a crash report continues with), repeats counted, then the program's own */
static void log_sections(Buf *r, const Buf *err)
{
    enum { OCERZ_MAX = 3000, OTHER_MAX = 200 };
    const char *s = err->p ? err->p : "";
    size_t nlines = 0, nocerz = 0, nother = 0;
    for (const char *p = s; *p; p++)
        nlines += *p == '\n';
    const char **other = calloc(nlines + 2, sizeof *other);
    size_t *olen = calloc(nlines + 2, sizeof *olen);
    bprintf(r, "\n== What ocerz printed\n");
    const char *last = NULL;
    size_t lastlen = 0, repeats = 0;
    int in_ocerz = 0;
    for (const char *p = s; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int ours = n >= 6 && (memcmp(p, "ocerz:", 6) == 0 || memcmp(p, "ocerz[", 6) == 0);
        int cont = !ours && in_ocerz && n > 0 && (p[0] == ' ' || p[0] == '\t');
        if (ours || cont) {
            in_ocerz = 1;
            if (last && n == lastlen && memcmp(p, last, n) == 0) {
                repeats++;
            } else {
                if (repeats)
                    bprintf(r, "  (the line above %zu more times)\n", repeats);
                repeats = 0;
                if (nocerz < OCERZ_MAX) {
                    anon(r, p, n);
                    bput(r, "\n", 1);
                }
                nocerz++;
                last = p;
                lastlen = n;
            }
        } else if (n > 0) {
            in_ocerz = 0;
            other[nother] = p;
            olen[nother++] = n;
        }
        p += n + (e ? 1 : 0);
    }
    if (repeats)
        bprintf(r, "  (the line above %zu more times)\n", repeats);
    if (nocerz > OCERZ_MAX)
        bprintf(r, "(%zu more lines left out)\n", nocerz - OCERZ_MAX);
    if (!nocerz)
        bprintf(r, "(nothing)\n");
    bprintf(r, "\n== The program's own standard error, last %d lines\n", OTHER_MAX);
    for (size_t i = nother > OTHER_MAX ? nother - OTHER_MAX : 0; i < nother; i++) {
        anon(r, other[i], olen[i]);
        bput(r, "\n", 1);
    }
    if (!nother)
        bprintf(r, "(nothing)\n");
    free(other);
    free(olen);
}

static void file_name_part(const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/');
    b = b && b[1] ? b + 1 : path;
    size_t k = 0;
    for (; *b && k + 1 < n; b++)
        out[k++] = isalnum((unsigned char)*b) || *b == '.' || *b == '-' ? *b : '_';
    out[k] = '\0';
    char *app = strstr(out, ".app");
    if (app && app[4] == '\0')
        *app = '\0';
}

int ocerz_report_main(const char *self, int argc, char **argv)
{
    anon_init();
    int first = 0;
    const char *mode_flag = NULL;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        if (strcmp(argv[first], "-native") == 0 || strcmp(argv[first], "-cache") == 0 || strcmp(argv[first], "-fex") == 0)
            mode_flag = argv[first];
        if (strcmp(argv[first], "-path") == 0 && first + 1 < argc)
            first++;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: ocerz -report [flags] program [args...]\n");
        return 64;
    }
    const char *program = argv[first];
    char exe[PATH_MAX], full[PATH_MAX];
    struct stat st;
    const char *where = realpath(program, full) ? full : program;
    int bundle = stat(where, &st) == 0 && S_ISDIR(st.st_mode) && ocerz_bundle_executable(where, exe, sizeof exe);
    if (!bundle && !realpath(program, exe))
        snprintf(exe, sizeof exe, "%s", program);

    Buf r = { 0 };
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm);
    bprintf(&r, "AArchX field report, %s\n\n== This Mac and this AArchX\n", when);
    mac_facts(&r, self);

    bprintf(&r, "\n== The program\n");
    anon_str(&r, program);
    bprintf(&r, "\n");
    if (bundle) {
        static const char *const keys[] = { "CFBundleIdentifier", "CFBundleName", "CFBundleShortVersionString",
                                            "CFBundleVersion", "LSMinimumSystemVersion", "CFBundleExecutable" };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            char v[512];
            if (ocerz_bundle_plist(program, keys[i], v, sizeof v)) {
                bprintf(&r, "  %s: ", keys[i]);
                anon_str(&r, v);
                bprintf(&r, "\n");
            }
        }
    }
    int x86_64 = 0, i386 = 0;
    bprintf(&r, "executable ");
    anon_str(&r, exe);
    bprintf(&r, ":\n");
    slices(exe, &r, &x86_64, &i386);

    bprintf(&r, "\n== How it runs\ncommand: ocerz");
    for (int i = 0; i < argc; i++) {
        bprintf(&r, " ");
        anon_str(&r, argv[i]);
    }
    const char *menv = getenv("OCERZ_MODE");
    bprintf(&r, "\nmode: %s\n",
            mode_flag ? (strcmp(mode_flag, "-native") == 0 ? "native (-native)" : strcmp(mode_flag, "-cache") == 0
                         ? "cache (-cache)" : "FEXCore (-fex)")
            : menv && *menv ? (strcmp(menv, "native") == 0 ? "native (OCERZ_MODE)" : "cache (OCERZ_MODE)")
            : !x86_64 && i386 ? "native, the 32-bit personality (an i386 program)"
            : "cache (the default)");
    char **env = *_NSGetEnviron();
    int nenv = 0;
    for (char **e = env; *e; e++)
        if (strncmp(*e, "OCERZ_", 6) == 0 || strncmp(*e, "DYLD_", 5) == 0) {
            bprintf(&r, "%s", nenv++ ? "  " : "environment:\n  ");
            anon_str(&r, *e);
            bprintf(&r, "\n");
        }
    if (!nenv)
        bprintf(&r, "environment: no OCERZ_ or DYLD_ variables\n");

    if (x86_64) {
        fprintf(stderr, "ocerz: report: checking how %s binds in native mode\n", exe);
        char *cargv[] = { (char *)"ocerz", (char *)"-check", exe, NULL };
        Buf out = { 0 };
        int cst = run_capture(self, cargv, &out, 0, 1, 180, NULL);
        bprintf(&r, "\n== ocerz -check (native mode)\n");
        if (out.p)
            anon(&r, out.p, out.n);
        if (cst >= 0 && WIFEXITED(cst))
            bprintf(&r, "%s\n", WEXITSTATUS(cst) == 0 ? "(everything binds)" : WEXITSTATUS(cst) == 1
                                    ? "(it binds, with stubs)" : WEXITSTATUS(cst) == 71 ? "(native mode would refuse it)"
                                    : "(the check did not finish)");
        free(out.p);
    }

    fprintf(stderr, "ocerz: report: running it; quit it as you normally would, or press Ctrl-C\n");
    char **rargv = calloc((size_t)argc + 2, sizeof *rargv);
    rargv[0] = (char *)"ocerz";
    for (int i = 0; i < argc; i++)
        rargv[i + 1] = argv[i];
    struct sigaction ign = { 0 }, oint, oterm;
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &oint);
    sigaction(SIGTERM, &ign, &oterm);
    signal(SIGPIPE, SIG_IGN);
    Buf err = { 0 };
    double secs = 0;
    int rst = run_capture(self, rargv, &err, 1, 0, 0, &secs);
    sigaction(SIGINT, &oint, NULL);
    sigaction(SIGTERM, &oterm, NULL);
    free(rargv);

    bprintf(&r, "\n== The run\n");
    if (rst < 0)
        bprintf(&r, "it did not start: %s\n", strerror(errno));
    else {
        status_text(rst, &r);
        bprintf(&r, " after %.2f s\n", secs);
    }
    log_sections(&r, &err);
    free(err.p);

    char dir[PATH_MAX], name[128], path[PATH_MAX], stamp[32];
    const char *d = getenv("OCERZ_REPORT_DIR");
    if (d && *d)
        snprintf(dir, sizeof dir, "%s", d);
    else if (g_home[0] && (snprintf(dir, sizeof dir, "%s/Desktop", g_home), access(dir, W_OK) == 0))
        ;
    else if (!getcwd(dir, sizeof dir))
        snprintf(dir, sizeof dir, ".");
    file_name_part(program, name, sizeof name);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(path, sizeof path, "%s/AArchX-report-%s-%s.txt", dir, name, stamp);
    FILE *f = fopen(path, "w");
    if (!f || fwrite(r.p, 1, r.n, f) != r.n) {
        fprintf(stderr, "ocerz: report: cannot write %s: %s\n", path, strerror(errno));
        if (f)
            fclose(f);
        free(r.p);
        return 74;
    }
    fclose(f);
    free(r.p);
    Buf shown = { 0 };
    anon_str(&shown, path);
    fprintf(stderr, "\nocerz: the report is %s\n"
                    "ocerz: it holds what it says about this Mac and the program and what ocerz printed while it ran,"
                    " with your name and home folder taken out; read it, then send it\n", shown.p ? shown.p : path);
    free(shown.p);
    if (rst < 0)
        return 70;
    return WIFSIGNALED(rst) ? 128 + WTERMSIG(rst) : WEXITSTATUS(rst);
}
