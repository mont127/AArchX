/* Command-line parsing and the entry point. */
#include <string.h>
#include "ocerz/version.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/dyld.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

static void usage(void)
{
    fprintf(stderr, "usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-path file] [--] program [args...]\n"
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
        "/AppKit.framework/,/QuartzCore.framework/,/HIToolbox.framework/";
    if (!is_wine_loader(path))
        return;
    if (!getenv("OCERZ_HOSTWQ"))
        setenv("OCERZ_HOSTWQ", "1", 0);
    const char *preload = getenv("OCERZ_PRELOAD_OBJC");
    if (!preload || strcmp(preload, "1") == 0)
        setenv("OCERZ_PRELOAD_OBJC", objc_images, 1);
}


extern char ocerz_cmdline_summary[256];

int main(int argc, char **argv)
{
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
        } else if (strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            load_path = argv[++i];
        } else {
            usage();
            return 64;
        }
    }
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
    {   /* OCERZ_NOJIT_EXE=<substr>: interpret only processes whose command
         * line matches (e.g. explorer.exe) - the env inherits through
         * wine's exec chain, so one process can be singled out for the
         * full-visibility interpreter while the rest stay on the JIT. */
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
    /* single-observer ("plain") memory model until a second observer appears
     * (thread, fork/spawn, hostwq worker, writable shared mapping, remap):
     * the syscall layer retires it through ocerz_jit_require_ordered() */
    vm.jit_plain_mem = getenv("OCERZ_NO_PLAIN_MEM") ? 0 : 1;

    if (dynamic)
        return ocerz_dyld_run(&vm, load_path, argc - i, argv + i, environ);

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
