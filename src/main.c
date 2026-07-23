/*
 * src/main.c
 *
 * Command-line entry point: ocerz [-v] [-trace] [-strace] [-no-jit] [--]
 * program [args...]
 *
 * Boot order matters and is fixed here: reserve the guest arena first
 * (before anything else can fragment the address space), then load the
 * x86_64 image into it, then build the exec-style guest stack with the
 * guest's argv (everything after the program path on Ocerz's own command
 * line) and the host environment passed through verbatim, then point the
 * virtual CPU at the entry point and hand control to the run loop. The
 * emulator's exit status is the guest's exit status, so Ocerz composes
 * with shell scripts and test harnesses exactly like the real program
 * would.
 *
 * Dynamic (LC_MAIN) executables are detected and rejected with a pointed
 * message naming the dyld-emulation phase that will lift the restriction;
 * static LC_UNIXTHREAD binaries run today.
 *
 * Options: -v bumps verbosity (repeatable; at -v -v every instruction is
 * traced), -trace forces instruction tracing, -strace logs guest syscalls,
 * -no-jit pins execution to the interpreter tier. -path FILE overrides the
 * file loaded as the executable while the trailing `program` token is still
 * used as guest argv[0]; this decouples the two the way a real execve(path,
 * argv, envp) does (argv[0] may differ from path), and is how sys_execve
 * re-enters ocerz onto a guest target that asked for a custom argv[0].
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/dyld.h"

#include <stdlib.h>

extern char **environ;

static void usage(void)
{
    fprintf(stderr, "usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-path file] [--] program [args...]\n");
}

int main(int argc, char **argv)
{
    int trace = 0;
    int strace = 0;
    int nojit = 0;
    const char *load_path = NULL;
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
    if (dynamic)
        return ocerz_dyld_run(&vm, load_path, argc - i, argv + i, environ);

    /* LC_UNIXTHREAD guests start with one virtual CPU and no shared mappings.
     * The JIT may use its single-CPU memory tier until a syscall explicitly
     * requests concurrency or shared memory, at which point it retires those
     * translations before admitting the new observer. */
    vm.jit_plain_mem = 1;

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
