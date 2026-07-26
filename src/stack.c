/* Builds the initial process stack the way XNU's exec path lays it out. */
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
