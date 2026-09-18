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
 * layer does through ocerz_jit_require_ordered().  OCERZ_NOJIT_EXE interprets
 * only the processes whose command line matches, and since the environment
 * inherits through Wine's exec chain that singles one process out for the
 * full-visibility interpreter while the rest stay on the JIT.
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
 */
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include "ocerz/version.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/dyld.h"
#include "ocerz/mode.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

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

static void usage(void)
{
    fprintf(stderr, "usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-native|-cache] [-path file] [--] program [args...]\n"
                    "       ocerz version\n");
}

static int is_wine_loader(const char *path)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved))
        path = resolved;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "wine") == 0 || strcmp(base, "wine64") == 0;
}

static void apply_wine_defaults(const char *path)
{
    static const char objc_images[] =
        "/AppKit.framework/,/QuartzCore.framework/,/HIToolbox.framework/,/CoreSpotlight.framework/";
    if (!is_wine_loader(path))
        return;
    const char *preload = getenv("OCERZ_PRELOAD_OBJC");
    if (!preload || strcmp(preload, "1") == 0)
        setenv("OCERZ_PRELOAD_OBJC", objc_images, 1);
}


extern char ocerz_cmdline_summary[256];

int main(int argc, char **argv)
{
    if (getenv("OCERZ_HOSTMASKLOG")) {
        sigset_t hm_; unsigned hv_ = 0;
        if (pthread_sigmask(SIG_BLOCK, NULL, &hm_) == 0)
            for (int sg_ = 1; sg_ < 32; sg_++) if (sigismember(&hm_, sg_)) hv_ |= 1u << sg_;
        fprintf(stderr, "ocerz: HOSTMASK-START[%d] mask=%#x argv1=%s\n", (int)getpid(), hv_, argc > 1 ? argv[1] : "");
    }
    if (getenv("OCERZ_EXECLOG")) {
        fprintf(stderr, "ocerz: EXECSTART[%d]", (int)getpid());
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
        } else if (strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            load_path = argv[++i];
        } else {
            usage();
            return 64;
        }
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
    if (i >= argc) {
        usage();
        return 64;
    }
    if (!load_path)
        load_path = argv[i];

    apply_wine_defaults(load_path);

    if (ocerz_verbose >= 2)
        trace = 1;

    static OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.trace = trace;
    vm.strace = strace;
    vm.jit_enabled = !nojit && getenv("OCERZ_NOJIT") == NULL;
    {
        const char *nx = getenv("OCERZ_NOJIT_EXE");
        extern char ocerz_cmdline_summary[];
        if (nx && *nx && strstr(ocerz_cmdline_summary, nx)) {
            vm.jit_enabled = 0;
            fprintf(stderr, "ocerz: NOJIT-EXE interpreting '%s'\n",
                    ocerz_cmdline_summary);
        }
    }

    int dynamic = ocerz_peek_dynamic(load_path);
    if (dynamic < 0) {
        OCERZ_FATAL("cannot read %s\n", load_path);
        return 65;
    }
    if (!dynamic && ocerz_mode == OCERZ_MODE_NATIVE) {
        OCERZ_FATAL("native mode cannot run the static image %s\n", load_path);
        return 64;
    }
    vm.jit_plain_mem = getenv("OCERZ_NO_PLAIN_MEM") ? 0 : 1;

    if (dynamic)
        return ocerz_dyld_run(&vm, load_path, argc - i, argv + i, env_snapshot(environ));

    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK)
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
