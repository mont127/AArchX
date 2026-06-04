/*
 * src/stack.c
 *
 * ocerz_setup_stack() builds the initial process-image stack exactly the way
 * XNU's exec path lays it out for an x86_64 binary, so guest crt0/_start code
 * and (later) dyld find argc, argv, envp and apple[] where they expect.
 *
 * A fresh 8 MiB guest stack is mapped via ocerz_map_anywhere(); stack_hi is
 * the high end (one past the top), stack_lo the base, both recorded in the VM
 * so later phases (guard pages, stack growth) have the bounds.
 *
 * The image is built working DOWN from stack_hi:
 *   1. 16 zero bytes at the very top (XNU leaves a small zero pad there).
 *   2. the strings area: first the apple[0] string, formatted exactly
 *      "executable_path=<img->path>"; then every envp[] string; then every
 *      argv[] string (argv[0] is the guest program path as given). Strings are
 *      copied verbatim including their NUL terminators. They are placed from
 *      high to low, but the recorded guest pointer for each string still
 *      points at its first byte, so order within the area is irrelevant to
 *      correctness.
 *   3. the strings base (lowest string byte) is rounded DOWN to 16.
 *   4. the pointer area, whose total size is computed first so that the FINAL
 *      rsp lands 16-byte aligned. For an LC_UNIXTHREAD entry the layout from
 *      rsp upward is:
 *        [rsp]            argc
 *        argv[0..argc-1]  pointers
 *        NULL
 *        envp[0..]        pointers
 *        NULL
 *        apple[0]         pointer (executable_path)
 *        NULL
 *      For a dyld-style entry (entry_is_unixthread==0) the first word is
 *      instead mh_gaddr and argc moves to [rsp+8], which is the shape dyld's
 *      _dyld_start consumes:
 *        [rsp]            mh_gaddr
 *        [rsp+8]          argc
 *        argv... NULL envp... NULL apple... NULL
 *
 * rsp is chosen as the strings base minus the pointer-area size, rounded down
 * to a 16-byte boundary; rounding down only grows the (harmless) gap between
 * the end of the pointer vectors and the strings, and guarantees rsp itself
 * is 16-byte aligned, which is what the System V x86_64 entry contract
 * requires. All stored pointers are GUEST addresses inside this stack.
 *
 * cpu->gpr[RSP] is set to rsp and rbp is zeroed; every other register keeps
 * its reset value so the entry point sees a clean architectural state.
 */
#include "ocerz/loader.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"

#include <stdlib.h>
#include <sys/mman.h>

#define OCERZ_GUEST_STACK_SIZE (8ull << 20)
#define OCERZ_STACK_TOP_PAD 16

static uint64_t align_down16(uint64_t v)
{
    return v & ~(uint64_t)0xf;
}

static uint64_t push_string(uint64_t *top, const char *s)
{
    size_t n = strlen(s) + 1;
    uint64_t at = *top - n;
    memcpy(ocerz_g2h(at), s, n);
    *top = at;
    return at;
}

int ocerz_setup_stack(struct OcerzVM *vm, const OcerzImage *img,
                      int argc, char **argv, char **envp)
{
    uint64_t base = ocerz_map_anywhere(OCERZ_GUEST_STACK_SIZE,
                                       PROT_READ | PROT_WRITE);
    if (base == 0) {
        OCERZ_FATAL("cannot map guest stack\n");
        return OCERZ_ENOMEM;
    }
    vm->stack_lo = base;
    vm->stack_hi = base + OCERZ_GUEST_STACK_SIZE;

    int envc = 0;
    while (envp != NULL && envp[envc] != NULL)
        envc++;

    uint64_t top = vm->stack_hi;

    top -= OCERZ_STACK_TOP_PAD;
    memset(ocerz_g2h(top), 0, OCERZ_STACK_TOP_PAD);

    size_t pathlen = strlen(img->path);
    char *applestr = (char *)malloc(pathlen + sizeof("executable_path="));
    if (applestr == NULL) {
        OCERZ_FATAL("out of memory building apple[0]\n");
        return OCERZ_ENOMEM;
    }
    memcpy(applestr, "executable_path=", sizeof("executable_path=") - 1);
    memcpy(applestr + sizeof("executable_path=") - 1, img->path, pathlen + 1);

    uint64_t apple_gaddr = push_string(&top, applestr);
    free(applestr);

    uint64_t *env_gaddr = NULL;
    if (envc > 0) {
        env_gaddr = (uint64_t *)malloc((size_t)envc * sizeof(uint64_t));
        if (env_gaddr == NULL) {
            OCERZ_FATAL("out of memory building envp vector\n");
            return OCERZ_ENOMEM;
        }
    }
    for (int i = 0; i < envc; i++)
        env_gaddr[i] = push_string(&top, envp[i]);

    uint64_t *arg_gaddr = NULL;
    if (argc > 0) {
        arg_gaddr = (uint64_t *)malloc((size_t)argc * sizeof(uint64_t));
        if (arg_gaddr == NULL) {
            OCERZ_FATAL("out of memory building argv vector\n");
            free(env_gaddr);
            return OCERZ_ENOMEM;
        }
    }
    for (int i = 0; i < argc; i++)
        arg_gaddr[i] = push_string(&top, argv[i]);

    uint64_t strings_base = align_down16(top);

    uint64_t nwords = 0;
    if (!img->entry_is_unixthread)
        nwords += 1;
    nwords += 1;
    nwords += (uint64_t)argc + 1;
    nwords += (uint64_t)envc + 1;
    nwords += 1 + 1;

    uint64_t ptr_area = nwords * 8;
    uint64_t rsp = align_down16(strings_base - ptr_area);

    uint64_t w = rsp;
    if (!img->entry_is_unixthread) {
        ocerz_st(w, 8, img->mh_gaddr);
        w += 8;
    }
    ocerz_st(w, 8, (uint64_t)(uint32_t)argc);
    w += 8;
    for (int i = 0; i < argc; i++) {
        ocerz_st(w, 8, arg_gaddr[i]);
        w += 8;
    }
    ocerz_st(w, 8, 0);
    w += 8;
    for (int i = 0; i < envc; i++) {
        ocerz_st(w, 8, env_gaddr[i]);
        w += 8;
    }
    ocerz_st(w, 8, 0);
    w += 8;
    ocerz_st(w, 8, apple_gaddr);
    w += 8;
    ocerz_st(w, 8, 0);
    w += 8;

    free(arg_gaddr);
    free(env_gaddr);

    vm->cpu.gpr[OCERZ_RSP] = rsp;
    vm->cpu.gpr[OCERZ_RBP] = 0;

    OCERZ_LOG("guest stack [%#llx,%#llx) rsp=%#llx argc=%d envc=%d %s\n",
              (unsigned long long)vm->stack_lo,
              (unsigned long long)vm->stack_hi,
              (unsigned long long)rsp, argc, envc,
              img->entry_is_unixthread ? "unixthread" : "dyld");
    return OCERZ_OK;
}
