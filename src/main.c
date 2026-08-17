/* Command-line parsing and the entry point. */
#include <string.h>
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/dyld.h"

#include <stdlib.h>

extern char **environ;

static void usage(void)
{
    fprintf(stderr, "usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-path file] [--] program [args...]\n");
}

char ocerz_cmdline_summary[256];   /* guest argv tail for diagnostics (EXITLOG) */
int main(int argc, char **argv)
{
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

    if (ocerz_verbose >= 2)
        trace = 1;

    static OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.trace = trace;
    vm.strace = strace;
    vm.jit_enabled = !nojit && getenv("OCERZ_NOJIT") == NULL;

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
