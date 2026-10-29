/*
 * Command-line parsing and the entry point.
 *
 * Three startup decisions are made here rather than lazily.  Some shared-cache
 * images' Objective-C categories must be visible before any class is realized,
 * because a dlopen registers them too late for classes that already exist:
 * CoreSpotlight adds encodeWithCSCoder: categories to Foundation's collection
 * classes, and without them the indexing AppKit kicks off about 20 s into a
 * session throws an unrecognized-selector NSException inside a dispatch block
 * and takes the process down.  A default list is preloaded;
 * OCERZ_PRELOAD_OBJC=@cat preloads every category-bearing image instead.
 *
 * The process also starts in the single-observer ("plain") memory model and
 * only retires it when a second observer actually appears - a thread, a
 * fork/spawn, a hostwq worker or a writable shared mapping - which the syscall
 * layer does through ocerz_jit_require_ordered().  Whether the processor keeps
 * the order for it (hardware TSO, src/cpu.c) is decided here as well, on the
 * main thread and after OCERZ_EXE_ENV, so a per-program OCERZ_NO_HW_TSO counts.
 * OCERZ_NOJIT_EXE interprets only the processes whose command line matches,
 * and since the environment inherits through Wine's exec chain that singles
 * one process out for the full-visibility interpreter while the rest stay on
 * the JIT.
 * OCERZ_STRACE_EXE turns on the syscall trace the same way, for one process of
 * Wine's tree instead of all of them, and OCERZ_EXE_ENV sets variables the same
 * way: "cs2.exe:OCERZ_NO_SUPERBLOCK=1,OCERZ_TSO_STRICT=1;other.exe:..." gives
 * each listed process its own JIT knobs before any of them is read.  A process whose loader is named wine is
 * marked as one of Wine's, which fixes the base of its memory arena (see mem.c).
 * So is one whose program carries a WINE_RESERVE segment, the range Wine's
 * preloader keeps for Windows: CrossOver starts each process through a link
 * named after the program, and for a hard link realpath keeps that name, so its
 * cmd.exe started with the ordinary layout and died reserving the low-shadow
 * base its Wine parent handed down.  OCERZ_NO_WINE_SEGMENT_DETECT=1 goes back
 * to the name alone.
 * OCERZ_STDERR_FILE appends every process's standard error to one file before
 * anything else runs: Chromium and others start their children with standard
 * error closed or pointed at nothing, and a child that dies early under ocerz
 * otherwise leaves no trace of why.
 *
 * The third decision is which universe the guest binds against.  -native and
 * -cache pick the mode outright and the last one on the command line wins;
 * with neither, OCERZ_MODE decides, which is how a child inherits the mode
 * across a spawn or an exec, so a value there that is neither native nor cache
 * is refused rather than quietly ignored.  Native mode has no static loader
 * path at all - a program that links against nothing has nothing to bridge
 * into - so a non-dynamic image is refused before the VM starts.
 *
 * The loader is handed a copy of the environment, not environ itself.  Loading
 * a guest in native mode opens the host's own frameworks, and their
 * initializers run then: CoreFoundation's calls setenv for
 * __CF_USER_TEXT_ENCODING, which reallocates environ and frees the array a
 * pointer taken earlier still names.  The guest's initial stack is built from
 * that pointer only after loading, so it read freed memory, and whether that
 * crashed depended on whether the block had been reused yet - which it was when
 * the environment happened to be the size bash passes and not the size zsh
 * passes.  The copy is taken once, owns its strings, and lives as long as the
 * process, so no framework the guest makes ocerz open can pull the guest's
 * environment out from under it.  It is also older than the CFProcessPath
 * variable bridge.c sets for the length of CoreFoundation's initializer to give
 * native CoreFoundation the guest's executable as the process path, so the
 * guest's initial stack never carries that variable, and the host environment,
 * which the guest's environ names, holds it only while that initializer runs.
 *
 * A program named by its application bundle runs the bundle's executable:
 * CFBundleExecutable from Contents/Info.plist, read as XML or as a binary
 * property list, or, when that cannot be read, the bundle's own name, under
 * Contents/MacOS.  Remote Desktop Manager's Info.plist is binary and its
 * executable is RemoteDesktopManager, so the name alone did not find it.  Steam's launch
 * options substitute the bundle for %command%, so without this a game
 * launched from Steam as "ocerz %command%" ended at once with "cannot read".
 * The guest's argv[0] becomes the executable's path, as it is for a bundle
 * started by the system.
 *
 * DYLD_INSERT_LIBRARIES names libraries for the guest, but the host's dyld
 * reads it first and loads their arm64 slices into ocerz itself.  Steam sets
 * it to its loader and overlay for every game it launches, and inside ocerz
 * they broke startup within half a second.  So ocerz started with the variable
 * set moves it to OCERZ_GUEST_DYLD_INSERT_LIBRARIES, removes it and re-executes
 * itself, which keeps the process ID Steam tracks; the clean copy puts the
 * variable back before the guest's environment is taken, so the guest sees it
 * as it was set and the loader inserts the libraries into the guest alone.
 *
 * `ocerz -aot image...` fills the translation cache with the images' code
 * without running it (src/dyld.c loads, src/jit.c translates; see "Ahead of
 * time" there), and with them the disk dylibs and bundles the program's
 * earlier launches loaded, by dlopen too (its profile's .images), or, when
 * there is no record of any, every disk dylib it loads statically;
 * -aot-deps does every static one whatever the profile says.  `ocerz
 * -aot-all` does every program that has a profile (its .path), the most
 * recently launched first, which is what a new build of ocerz, whose store
 * starts empty, wants run once.  An executable is loaded as the main image of a process, so
 * with more than one each image is done by a child of its own, one after the
 * other; a dylib or bundle named after a single executable is loaded into that
 * executable's process, and one named alone gets a process with no main image.
 * Before that, -aot reads which programs the named executables were seen to
 * execute (their profiles, src/tcache.c) and, when there are any, does the
 * named images in a child as above and then each of those programs in a child
 * of its own; the children get OCERZ_AOT_NO_EXECS=1, so nothing is done twice.
 */
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include "ocerz/version.h"
#include "ocerz/vm.h"
#include "ocerz/m32.h"
#include "ocerz/mem.h"
#include "ocerz/dyld.h"
#include "ocerz/fex.h"
#include "ocerz/mode.h"
#include "ocerz/tcache.h"
#include "ocerz/cache.h"

#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <spawn.h>
#include <sys/wait.h>

static char **env_snapshot(char **env)
{
    size_t n = 0;
    while (env && env[n])
        n++;
    char **copy = calloc(n + 1, sizeof *copy);
    if (!copy)
        return env;
    for (size_t k = 0; k < n; k++) {
        copy[k] = strdup(env[k]);
        if (!copy[k])
            return env;
    }
    return copy;
}

extern char **environ;

static int aot_spawn(const char *self, int deps, int n, char **images)
{
    char *args[64];
    int k = 0, status = 0;
    pid_t pid;
    args[k++] = (char *)self;
    args[k++] = (char *)(deps ? "-aot-deps" : "-aot");
    for (int i = 0; i < n && k < 63; i++)
        args[k++] = images[i];
    args[k] = NULL;
    return posix_spawn(&pid, self, NULL, NULL, args, environ) != 0 || waitpid(pid, &status, 0) < 0 ||
           !WIFEXITED(status) || WEXITSTATUS(status);
}

/* The programs the named executables were seen to execute, and those they
 * did, each once and none of the named ones. */
static int aot_execs(int n, char **images, char **out, int cap)
{
    char seen[64][PATH_MAX];
    int ns = 0, no = 0;
    for (int k = 0; k < n && ns < 64; k++)
        if (realpath(images[k], seen[ns]))
            ns++;
    for (int at = 0; at < ns && ns < 64; at++) {
        char *list = ocerz_tcache_profile_execs(ocerz_dyld_file_ident(seen[at]));
        for (char *line = list ? strtok(list, "\n") : NULL; line && ns < 64 && no < cap; line = strtok(NULL, "\n")) {
            char abs[PATH_MAX];
            int dup = !realpath(line, abs) || ocerz_dyld_filetype(abs) != MH_EXECUTE;
            for (int j = 0; j < ns && !dup; j++)
                dup = !strcmp(seen[j], abs);
            if (dup)
                continue;
            snprintf(seen[ns++], PATH_MAX, "%s", abs);
            out[no++] = strdup(abs);
        }
        free(list);
    }
    return no;
}

/* -aot-all: every program with a profile, the most recently launched first,
 * each in a process of its own.  A program whose file is gone, or is another
 * build now (its identity changed), is passed over; programs it executed have
 * profiles of their own, so their children get OCERZ_AOT_NO_EXECS=1. */
static int aot_all(const char *self)
{
    char *list = ocerz_tcache_profile_programs();
    int done = 0, skipped = 0, failed = 0;
    setenv("OCERZ_AOT_NO_EXECS", "1", 1);
    for (char *line = list ? strtok(list, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        char *sp = strchr(line, ' '), abs[PATH_MAX];
        if (!sp)
            continue;
        *sp = 0;
        uint64_t ident = strtoull(line, NULL, 16);
        char *path = sp + 1;
        if (!realpath(path, abs) || ocerz_dyld_file_ident(abs) != ident) {
            skipped++;
            continue;
        }
        if (aot_spawn(self, 0, 1, &path) != 0)
            failed++;
        else
            done++;
    }
    free(list);
    fprintf(stderr, "ocerz: -aot-all: %d program%s done, %d gone or rebuilt since, %d failed\n", done,
            done == 1 ? "" : "s", skipped, failed);
    return failed ? 1 : 0;
}

/* -aot with more than one program: each image in a process of its own, since
 * each program is loaded as the main image of one. */
static int aot_main(OcerzVM *vm, int n, char **images, int deps, const char *self)
{
    char *execs[32];
    int nx = getenv("OCERZ_AOT_NO_EXECS") ? 0 : aot_execs(n, images, execs, 32);
    if (nx) {
        setenv("OCERZ_AOT_NO_EXECS", "1", 1);
        int rc = aot_spawn(self, deps, n, images);
        for (int k = 0; k < nx; k++) {
            fprintf(stderr, "ocerz: -aot %s: executed by %s before\n", execs[k], images[0]);
            rc |= aot_spawn(self, deps, 1, &execs[k]);
        }
        return rc;
    }
    int nexe = 0, exe = -1;
    for (int k = 0; k < n; k++)
        if (ocerz_dyld_filetype(images[k]) == MH_EXECUTE) {
            nexe++;
            exe = k;
        }
    if (nexe > 1) {
        int rc = 0;
        for (int k = 0; k < n; k++)
            rc |= aot_spawn(self, deps, 1, &images[k]);
        return rc;
    }
    if (exe > 0) {
        char *t = images[0];
        images[0] = images[exe];
        images[exe] = t;
    }
    return ocerz_dyld_aot(vm, n, images, deps, env_snapshot(environ));
}

static void usage(void)
{
    fprintf(stderr, "usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-native|-cache] [-fex] [-path file] [--] program [args...]\n"
                    "       ocerz -check program\n"
                    "       ocerz -aot|-aot-deps image [image...]\n"
                    "       ocerz -aot-all\n"
                    "       ocerz version\n");
}

/* The load commands of the x86_64 image in a Mach-O file, read into buf; NULL if it has none or they do not fit. */
static const struct mach_header_64 *x86_header(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    ssize_t got = pread(fd, buf, cap, 0);
    uint32_t magic = got >= 8 ? *(const uint32_t *)buf : 0;
    if (magic == FAT_CIGAM) {
        uint32_t n = __builtin_bswap32(*(const uint32_t *)(buf + 4));
        off_t at = -1;
        for (uint32_t i = 0; i < n && 8 + (i + 1) * sizeof(struct fat_arch) <= (size_t)got; i++) {
            const struct fat_arch *fa = (const struct fat_arch *)(buf + 8) + i;
            if ((int32_t)__builtin_bswap32((uint32_t)fa->cputype) == CPU_TYPE_X86_64)
                at = (off_t)__builtin_bswap32(fa->offset);
        }
        got = at >= 0 ? pread(fd, buf, cap, at) : 0;
        magic = got >= 8 ? *(const uint32_t *)buf : 0;
    }
    close(fd);
    if (magic != MH_MAGIC_64 || got < (ssize_t)sizeof(struct mach_header_64))
        return NULL;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)buf;
    if (mh->cputype != CPU_TYPE_X86_64)
        return NULL;
    *len = (size_t)got;
    return mh;
}

/* Whether the x86_64 image in a Mach-O file has a WINE_RESERVE segment. */
static int has_wine_reserve(const char *path)
{
    static uint8_t buf[1 << 16];
    size_t got = 0;
    const struct mach_header_64 *mh = x86_header(path, buf, sizeof buf, &got);
    if (!mh)
        return 0;
    size_t off = sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds && off + sizeof(struct load_command) <= got; i++) {
        const struct load_command *lc = (const struct load_command *)(buf + off);
        if (lc->cmdsize < sizeof *lc || off + lc->cmdsize > got)
            break;
        if (lc->cmd == LC_SEGMENT_64 &&
            strncmp(((const struct segment_command_64 *)lc)->segname, "WINE_RESERVE", 16) == 0)
            return 1;
        off += lc->cmdsize;
    }
    return 0;
}

/* The lowest address a static x86_64 image maps, __PAGEZERO aside; 0 if there is none. */
static uint64_t image_low_vmaddr(const char *path)
{
    static uint8_t buf[1 << 16];
    size_t got = 0;
    const struct mach_header_64 *mh = x86_header(path, buf, sizeof buf, &got);
    if (!mh)
        return 0;
    uint64_t lo = UINT64_MAX;
    size_t off = sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds && off + sizeof(struct load_command) <= got; i++) {
        const struct load_command *lc = (const struct load_command *)(buf + off);
        if (lc->cmdsize < sizeof *lc || off + lc->cmdsize > got)
            break;
        const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
        if (lc->cmd == LC_SEGMENT_64 && !(sc->vmaddr == 0 && sc->initprot == 0) && sc->vmaddr < lo)
            lo = sc->vmaddr;
        off += lc->cmdsize;
    }
    return lo == UINT64_MAX ? 0 : lo;
}

static int is_wine_loader(const char *path)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved))
        path = resolved;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "wine") == 0 || strcmp(base, "wine64") == 0)
        return 1;
    return !getenv("OCERZ_NO_WINE_SEGMENT_DETECT") && has_wine_reserve(path);
}

static void apply_wine_defaults(const char *path)
{
    static const char objc_images[] =
        "/AppKit.framework/,/QuartzCore.framework/,/HIToolbox.framework/,/CoreSpotlight.framework/";
    if (!is_wine_loader(path))
        return;
    ocerz_wine_process = 1;
    const char *preload = getenv("OCERZ_PRELOAD_OBJC");
    if (!preload || strcmp(preload, "1") == 0)
        setenv("OCERZ_PRELOAD_OBJC", objc_images, 1);
}


extern char ocerz_cmdline_summary[256];

#define MOCK_KEYCHAIN_ARGS 512

static char **mock_keychain_argv(const char *path, int *argc, char **argv)
{
    static char *out[MOCK_KEYCHAIN_ARGS + 2];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "Steam Helper") != 0 || getenv("OCERZ_NO_MOCK_KEYCHAIN") || *argc < 1 ||
        *argc >= MOCK_KEYCHAIN_ARGS)
        return argv;
    for (int k = 1; k < *argc; k++)
        if (strcmp(argv[k], "--use-mock-keychain") == 0 ||
            strncmp(argv[k], "--type=crashpad-handler", 23) == 0)
            return argv;
    out[0] = argv[0];
    out[1] = (char *)"--use-mock-keychain";
    for (int k = 1; k < *argc; k++)
        out[k + 1] = argv[k];
    out[*argc + 1] = NULL;
    (*argc)++;
    return out;
}

/* A big-endian unsigned integer of size bytes, as binary property lists store them. */
static uint64_t bplist_be(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    for (unsigned k = 0; k < size; k++)
        v = v << 8 | p[k];
    return v;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    unsigned off_size, ref_size;
    uint64_t nobj, table;
} Bplist;

/* Where the object at index i starts, or 0 if the index or its offset is out of bounds. */
static size_t bplist_object(const Bplist *b, uint64_t i)
{
    if (i >= b->nobj)
        return 0;
    uint64_t at = b->table + i * b->off_size;
    if (at + b->off_size > b->len - 32)
        return 0;
    uint64_t off = bplist_be(b->buf + at, b->off_size);
    return off >= 8 && off < b->len - 32 ? (size_t)off : 0;
}

/* The count in a marker's low nibble, or the integer object after it when the nibble is 0xF; where the payload
   starts in *pos. */
static int bplist_count(const Bplist *b, size_t *pos, uint64_t *count)
{
    unsigned low = b->buf[*pos] & 0x0f;
    (*pos)++;
    if (low != 0x0f) {
        *count = low;
        return 1;
    }
    if (*pos >= b->len || (b->buf[*pos] & 0xf0) != 0x10)
        return 0;
    unsigned size = 1u << (b->buf[*pos] & 0x0f);
    if (size > 8 || *pos + 1 + size > b->len)
        return 0;
    *count = bplist_be(b->buf + *pos + 1, size);
    *pos += 1 + size;
    return 1;
}

/* An ASCII or UTF-16 string object holding only ASCII, as a C string; 0 if it is anything else. */
static int bplist_ascii(const Bplist *b, size_t pos, char *out, size_t n)
{
    unsigned type = b->buf[pos] >> 4;
    uint64_t count;
    if ((type != 5 && type != 6) || !bplist_count(b, &pos, &count) || count >= n)
        return 0;
    uint64_t bytes = type == 5 ? count : count * 2;
    if (pos + bytes > b->len)
        return 0;
    for (uint64_t k = 0; k < count; k++) {
        unsigned c = type == 5 ? b->buf[pos + k] : (unsigned)bplist_be(b->buf + pos + 2 * k, 2);
        if (c == 0 || c > 0x7e)
            return 0;
        out[k] = (char)c;
    }
    out[count] = '\0';
    return 1;
}

/* The string the top-level dictionary of a binary property list ("bplist00") gives key, bounds-checked against
   a file that may be anything. */
static int bplist_string(const uint8_t *buf, size_t len, const char *key, char *out, size_t n)
{
    if (len < 40 || memcmp(buf, "bplist00", 8) != 0)
        return 0;
    const uint8_t *t = buf + len - 32;
    Bplist b = {buf, len, t[6], t[7], bplist_be(t + 8, 8), bplist_be(t + 24, 8)};
    if (!b.off_size || b.off_size > 8 || !b.ref_size || b.ref_size > 8 || b.table >= len)
        return 0;
    size_t pos = bplist_object(&b, bplist_be(t + 16, 8));
    uint64_t count;
    if (!pos || (buf[pos] >> 4) != 0xd || !bplist_count(&b, &pos, &count) || count > len ||
        pos + 2 * count * b.ref_size > len)
        return 0;
    char name[256];
    for (uint64_t k = 0; k < count; k++) {
        size_t kp = bplist_object(&b, bplist_be(buf + pos + k * b.ref_size, b.ref_size));
        if (!kp || !bplist_ascii(&b, kp, name, sizeof name) || strcmp(name, key) != 0)
            continue;
        size_t vp = bplist_object(&b, bplist_be(buf + pos + (count + k) * b.ref_size, b.ref_size));
        return vp && bplist_ascii(&b, vp, out, n);
    }
    return 0;
}

static int bundle_executable(const char *dir, char *out, size_t n)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    char plist[PATH_MAX];
    char name[PATH_MAX] = "";
    snprintf(plist, sizeof plist, "%s/Contents/Info.plist", dir);
    FILE *f = fopen(plist, "rb");
    if (f) {
        static char buf[1 << 20];
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = '\0';
        const char *k = strstr(buf, "<key>CFBundleExecutable</key>");
        const char *v = k ? strstr(k, "<string>") : NULL;
        const char *e = v ? strstr(v, "</string>") : NULL;
        if (e && (size_t)(e - v - 8) < sizeof name) {
            memcpy(name, v + 8, (size_t)(e - v - 8));
            name[e - v - 8] = '\0';
        } else if (!bplist_string((const uint8_t *)buf, len, "CFBundleExecutable", name, sizeof name) ||
                   strchr(name, '/')) {
            name[0] = '\0';
        }
    }
    if (!name[0]) {
        const char *b = strrchr(dir, '/');
        b = b ? b + 1 : dir;
        size_t bl = strlen(b);
        while (bl > 0 && b[bl - 1] == '/')
            bl--;
        if (bl > 4 && strncmp(b + bl - 4, ".app", 4) == 0)
            bl -= 4;
        if (bl == 0 || bl >= sizeof name)
            return 0;
        memcpy(name, b, bl);
        name[bl] = '\0';
    }
    int len = snprintf(out, n, "%s/Contents/MacOS/%s", dir, name);
    return len > 0 && (size_t)len < n && stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

/* whether path is an x86_64 Mach-O or a universal file with an x86_64 slice */
static int has_x86_64(const char *path)
{
    uint8_t buf[4096];
    int fd = open(path, O_RDONLY);
    ssize_t got = fd >= 0 ? pread(fd, buf, sizeof buf, 0) : -1;
    if (fd >= 0)
        close(fd);
    if (got < 8)
        return 0;
    uint32_t magic = *(const uint32_t *)buf;
    if (magic == MH_MAGIC_64)
        return ((const struct mach_header_64 *)buf)->cputype == CPU_TYPE_X86_64;
    if (magic != FAT_CIGAM)
        return 0;
    uint32_t n = __builtin_bswap32(*(const uint32_t *)(buf + 4));
    for (uint32_t i = 0; i < n && 8 + (i + 1) * sizeof(struct fat_arch) <= (size_t)got; i++)
        if ((int32_t)__builtin_bswap32((uint32_t)((const struct fat_arch *)(buf + 8))[i].cputype) == CPU_TYPE_X86_64)
            return 1;
    return 0;
}

/*
 * A launch script rather than a program.  Steam's %command% for a Source game on the Mac is hl2.sh, which sets
 * DYLD_LIBRARY_PATH and starts the game as `${GAME_DEBUGGER} <game> <args>`.  The script runs as itself with
 * GAME_DEBUGGER naming this ocerz and the flags before the script, so `ocerz -native %command%` serves those games as
 * it serves the rest; a GAME_DEBUGGER already set wins.  The scripts expand it unquoted, so an ocerz path with a space
 * would split: it is refused with a message instead.
 *
 * Any other script (Nexuiz.app's executable: set DYLD_LIBRARY_PATH, then `exec "$0"-bin "$@"`) runs on its
 * interpreter's x86_64 slice under this ocerz with the same flags, so the programs it starts are ocerz's to run: run
 * natively, /bin/sh handed the i386 game to the kernel, which refused it ("Bad CPU type in executable").  An
 * interpreter with no x86_64 slice runs the script natively, as before.  -1 when path is not a script.
 */
static int run_launch_script(const char *path, char **argv, int first)
{
    char text[65536];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, text, sizeof text - 1);
    close(fd);
    if (got < 2 || text[0] != '#' || text[1] != '!')
        return -1;
    text[got] = 0;
    char line[1024], interp[PATH_MAX] = "", iarg[256] = "";
    snprintf(line, sizeof line, "%.*s", (int)strcspn(text + 2, "\r\n"), text + 2);   /* the #! line alone */
    sscanf(line, " %1023[^ \t] %255[^\n]", interp, iarg);
    for (size_t k = strlen(iarg); k && (iarg[k - 1] == ' ' || iarg[k - 1] == '\t'); k--)
        iarg[k - 1] = 0;
    if (!strstr(text, "$GAME_DEBUGGER") && !strstr(text, "${GAME_DEBUGGER") && interp[0] == '/' && has_x86_64(interp)) {
        char self[PATH_MAX], real[PATH_MAX];
        uint32_t size = sizeof self;
        int argc = first;
        while (argv[argc])
            argc++;
        char **nargv = calloc((size_t)argc + 4, sizeof *nargv);
        if (nargv && _NSGetExecutablePath(self, &size) == 0 && realpath(self, real)) {
            int n = 0;
            nargv[n++] = real;
            for (int k = 1; k < first; k++)
                nargv[n++] = argv[k];
            nargv[n++] = interp;
            if (iarg[0])
                nargv[n++] = iarg;
            for (int k = first; k < argc; k++)
                nargv[n++] = argv[k];
            execv(real, nargv);
        }
    }
    if (!getenv("GAME_DEBUGGER")) {
        char self[PATH_MAX], real[PATH_MAX], debugger[PATH_MAX + 256];
        uint32_t size = sizeof self;
        if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, real))
            return -1;
        if (strchr(real, ' ')) {
            OCERZ_FATAL("%s is a launch script, which starts its game through GAME_DEBUGGER, and ocerz's path has a "
                        "space in it (%s): move ocerz to a path without one\n", path, real);
            return 64;
        }
        snprintf(debugger, sizeof debugger, "%s", real);
        for (int k = 1; k < first; k++) {
            strlcat(debugger, " ", sizeof debugger);
            strlcat(debugger, argv[k], sizeof debugger);
        }
        setenv("GAME_DEBUGGER", debugger, 1);
    }
    execv(path, argv + first);
    OCERZ_FATAL("cannot run the launch script %s\n", path);
    return 126;
}

static void reexec_without_host_insertion(char **argv)
{
    const char *list = getenv("DYLD_INSERT_LIBRARIES");
    if (!list || !list[0] || getenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES"))
        return;
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0)
        return;
    if (setenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES", list, 1) != 0)
        return;
    unsetenv("DYLD_INSERT_LIBRARIES");
    execv(self, argv);
    setenv("DYLD_INSERT_LIBRARIES", getenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES"), 1);
    unsetenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES");
}

static void restore_guest_insertion(void)
{
    const char *list = getenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES");
    if (!list)
        return;
    setenv("DYLD_INSERT_LIBRARIES", list, 1);
    unsetenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "-hold-cache") == 0)   /* before anything else: it is no launch */
        return ocerz_cache_holder_main(argc, argv);
    {
        const char *ef = getenv("OCERZ_STDERR_FILE");
        int efd = ef && ef[0] ? open(ef, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
        if (efd >= 0) {
            dup2(efd, 2);
            close(efd);
        }
    }
    ocerz_diag_init();
    reexec_without_host_insertion(argv);
    restore_guest_insertion();
    if (getenv("OCERZ_HOSTMASKLOG")) {
        sigset_t hm_; unsigned hv_ = 0;
        if (pthread_sigmask(SIG_BLOCK, NULL, &hm_) == 0)
            for (int sg_ = 1; sg_ < 32; sg_++) if (sigismember(&hm_, sg_)) hv_ |= 1u << sg_;
        fprintf(stderr, "ocerz: HOSTMASK-START[%d] mask=%#x argv1=%s\n", (int)getpid(), hv_, argc > 1 ? argv[1] : "");
    }
    if (getenv("OCERZ_EXECLOG")) {
        fprintf(stderr, "ocerz: EXECSTART[%d<-%d]", (int)getpid(), (int)getppid());
        for (int k = 0; k < argc; k++)
            fprintf(stderr, " %s", argv[k] ? argv[k] : "(null)");
        int envc = 0, noexec = -1, reserve = -1, socket = -1;
        for (; environ[envc]; envc++) {
            if (strncmp(environ[envc], "WINELOADERNOEXEC=", 17) == 0)
                noexec = envc;
            else if (strncmp(environ[envc], "WINEPRELOADRESERVE=", 19) == 0)
                reserve = envc;
            else if (strncmp(environ[envc], "WINESERVERSOCKET=", 17) == 0)
                socket = envc;
        }
        fprintf(stderr, " envc=%d wine_env=%d/%d/%d\n",
                envc, noexec, reserve, socket);
    }
    {
        char *w = ocerz_cmdline_summary, *end = ocerz_cmdline_summary + sizeof ocerz_cmdline_summary - 1;
        for (int i = 1; i < argc && w < end; i++) {
            const char *b = strrchr(argv[i], '/');
            b = b ? b + 1 : argv[i];
            if (w != ocerz_cmdline_summary && w < end) *w++ = ' ';
            while (*b && w < end) *w++ = *b++;
        }
        *w = 0;
    }
    int trace = 0;
    int strace = 0;
    int nojit = 0;
    int mode_from_flag = 0;
    int aot = 0;
    const char *load_path = NULL;
    setenv("MallocNanoZone", "0", 1);
    int i = 1;
    if (argc == 2 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "-version") == 0 ||
                      strcmp(argv[1], "--version") == 0)) {
        printf("%s %s\n", OCERZ_PROJECT, OCERZ_VERSION);
        return 0;
    }
    for (; i < argc; i++) {
        if (argv[i][0] != '-')
            break;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if (strcmp(argv[i], "-v") == 0) {
            ocerz_verbose++;
        } else if (strcmp(argv[i], "-trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "-strace") == 0) {
            strace = 1;
        } else if (strcmp(argv[i], "-no-jit") == 0) {
            nojit = 1;
        } else if (strcmp(argv[i], "-native") == 0) {
            ocerz_mode = OCERZ_MODE_NATIVE;
            mode_from_flag = 1;
        } else if (strcmp(argv[i], "-cache") == 0) {
            ocerz_mode = OCERZ_MODE_CACHE;
            mode_from_flag = 1;
        } else if (strcmp(argv[i], "-check") == 0) {
            ocerz_native_check = 1;
            ocerz_mode = OCERZ_MODE_NATIVE;
            mode_from_flag = 1;
        } else if (strcmp(argv[i], "-fex") == 0) {
            setenv("OCERZ_FEX", "1", 1);
        } else if (strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            load_path = argv[++i];
        } else if (strcmp(argv[i], "-aot") == 0 || strcmp(argv[i], "-aot-deps") == 0) {
            aot = argv[i][4] ? 2 : 1;
        } else if (strcmp(argv[i], "-aot-all") == 0) {
            aot = 3;
        } else {
            usage();
            return 64;
        }
    }
    /* -fex, or OCERZ_FEX=1, which -fex sets so that the programs a guest starts run on FEXCore as well */
    const char *fex_env = getenv("OCERZ_FEX");
    if (fex_env && *fex_env && strcmp(fex_env, "0") != 0) {
#ifdef OCERZ_HAVE_FEX
        const char *fex_needs = NULL;
        if (!ocerz_fex_supported(&fex_needs)) {
            OCERZ_FATAL("-fex needs %s; run without it to use ocerz's own translator\n", fex_needs);
            return 64;
        }
        ocerz_engine_run = ocerz_fex_run;
        ocerz_engine_fault = ocerz_fex_fault;
        ocerz_engine_async = ocerz_fex_async;
        ocerz_engine_invalidate = ocerz_fex_invalidate;
        ocerz_engine_ldt = ocerz_fex_ldt;
        ocerz_engine_gprs_at = ocerz_fex_gprs_at;
        ocerz_engine_cpuid = ocerz_fex_cpuid;
        ocerz_engine_pc_in_code = ocerz_fex_pc_in_code;
#else
        OCERZ_FATAL("this ocerz was built without FEXCore, so it has no -fex (make FEX=<dir> builds it in)\n");
        return 64;
#endif
    }
    if (!mode_from_flag) {
        const char *mode_env = getenv("OCERZ_MODE");
        if (mode_env && *mode_env) {
            if (strcmp(mode_env, "native") == 0) {
                ocerz_mode = OCERZ_MODE_NATIVE;
            } else if (strcmp(mode_env, "cache") == 0) {
                ocerz_mode = OCERZ_MODE_CACHE;
            } else {
                OCERZ_FATAL("unknown OCERZ_MODE value '%s', want native or cache\n", mode_env);
                return 64;
            }
        }
    }
    OCERZ_LOG("mode: %s\n", ocerz_mode == OCERZ_MODE_NATIVE ? "native" : "cache");
    if (!ocerz_engine_run)
        ocerz_afp_enable();
    if (aot == 3 && i >= argc) {
        char selfpath[PATH_MAX];
        uint32_t sl = sizeof selfpath;
        return aot_all(_NSGetExecutablePath(selfpath, &sl) == 0 ? selfpath : argv[0]);
    }
    if (i >= argc || aot == 3) {
        usage();
        return 64;
    }
    if (aot) {
        static OcerzVM avm;
        ocerz_vm_init(&avm);
        avm.jit_enabled = !nojit && getenv("OCERZ_NOJIT") == NULL;
        avm.jit_plain_mem = getenv("OCERZ_NO_PLAIN_MEM") ? 0 : 1;
        ocerz_hwtso_enable();
        char selfpath[PATH_MAX];
        uint32_t sl = sizeof selfpath;
        return aot_main(&avm, argc - i, argv + i, aot == 2, _NSGetExecutablePath(selfpath, &sl) == 0 ? selfpath : argv[0]);
    }
    if (!load_path)
        load_path = argv[i];
    static char bundle_exe[PATH_MAX];
    if (bundle_executable(load_path, bundle_exe, sizeof bundle_exe)) {
        if (load_path == argv[i])
            argv[i] = bundle_exe;
        load_path = bundle_exe;
    }
    int script = run_launch_script(load_path, argv, i);
    if (script >= 0)
        return script;
    /* An i386 program has no x86 shared cache to map and crosses into arm64 frameworks on every call, so it runs in
     * native mode unless asked otherwise.  Cache mode's NaN results from FPCR.AH cost it two FPCR writes per call
     * (about 17 ns on an M5), more than the checks they save: Nexuiz's benchmark demo took 8% more cycles. */
    if (!mode_from_flag && !(getenv("OCERZ_MODE") && *getenv("OCERZ_MODE")) && ocerz_mode != OCERZ_MODE_NATIVE &&
        ocerz_peek_dynamic(load_path) == -2 && m32_is_i386(load_path)) {
        ocerz_mode = OCERZ_MODE_NATIVE;
        ocerz_afp_enable();
    }

    apply_wine_defaults(load_path);

    if (ocerz_verbose >= 2)
        trace = 1;

    static OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.trace = trace;
    vm.strace = strace;
    vm.jit_enabled = !nojit && getenv("OCERZ_NOJIT") == NULL && !ocerz_engine_run;
    {
        const char *sx = getenv("OCERZ_STRACE_EXE");
        extern char ocerz_cmdline_summary[];
        if (sx && *sx && strstr(ocerz_cmdline_summary, sx))
            vm.strace = 1;
    }
    {
        const char *ev = getenv("OCERZ_EXE_ENV");
        extern char ocerz_cmdline_summary[];
        char spec[1024];
        snprintf(spec, sizeof spec, "%s", ev ? ev : "");
        for (char *rule = strtok(spec, ";"); rule; rule = strtok(NULL, ";")) {
            char *colon = strchr(rule, ':');
            if (!colon) continue;
            *colon = 0;
            if (!*rule || !strstr(ocerz_cmdline_summary, rule)) continue;
            char *save = NULL;
            for (char *kv = strtok_r(colon + 1, ",", &save); kv; kv = strtok_r(NULL, ",", &save)) {
                char *eq = strchr(kv, '=');
                if (!eq) continue;
                *eq = 0;
                setenv(kv, eq + 1, 1);
                fprintf(stderr, "ocerz: EXE-ENV %s=%s for '%s'\n", kv, eq + 1, ocerz_cmdline_summary);
            }
        }
    }
    ocerz_bigring_init();
    {
        const char *nx = getenv("OCERZ_NOJIT_EXE");
        extern char ocerz_cmdline_summary[];
        if (nx && *nx && strstr(ocerz_cmdline_summary, nx)) {
            vm.jit_enabled = 0;
            fprintf(stderr, "ocerz: NOJIT-EXE interpreting '%s'\n",
                    ocerz_cmdline_summary);
        }
    }

    ocerz_hwtso_enable();   /* after OCERZ_EXE_ENV, before any guest code (src/cpu.c) */
    int dynamic = ocerz_peek_dynamic(load_path);
    if (dynamic == -2) {
        if (m32_is_i386(load_path))
            return m32_run(&vm, load_path, argc - i, argv + i, env_snapshot(environ));
        OCERZ_FATAL("%s has no x86_64 slice: it is not an Intel program, so there is nothing to translate\n", load_path);
        return 64;
    }
    if (dynamic < 0) {
        OCERZ_FATAL("cannot read %s\n", load_path);
        return 65;
    }
    if (ocerz_native_check && (ocerz_mode != OCERZ_MODE_NATIVE || m32_is_i386(load_path))) {
        OCERZ_FATAL("-check looks at a 64-bit Intel program in native mode; %s\n",
                    ocerz_mode != OCERZ_MODE_NATIVE ? "it does not apply in cache mode" : "this one is 32-bit (m32)");
        return 64;
    }
    if (!dynamic && ocerz_mode == OCERZ_MODE_NATIVE) {
        OCERZ_FATAL("native mode cannot run the static image %s\n", load_path);
        return 64;
    }
    vm.jit_plain_mem = getenv("OCERZ_NO_PLAIN_MEM") ? 0 : 1;

    if (dynamic) {
        int gargc = argc - i;
        char **gargv = mock_keychain_argv(load_path, &gargc, argv + i);
        return ocerz_dyld_run(&vm, load_path, gargc, gargv, env_snapshot(environ));
    }

    /* FEXCore runs guest code at its own addresses, so a static image under -fex gets identity memory reserved where
     * the image wants to be. */
    if (ocerz_engine_run ? ocerz_mem_init_identity_at(image_low_vmaddr(load_path), 32ull << 30) != OCERZ_OK
                         : ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK)
        return 70;

    if (ocerz_load_image(load_path, &vm.image) != OCERZ_OK) {
        OCERZ_FATAL("cannot load %s\n", load_path);
        return 65;
    }

    if (ocerz_setup_stack(&vm, &vm.image, argc - i, argv + i, environ) != OCERZ_OK) {
        OCERZ_FATAL("cannot build guest stack\n");
        return 70;
    }

    vm.cpu.rip = vm.image.entry;
    return ocerz_vm_run(&vm);
}
