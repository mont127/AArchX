#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int (*check)(int *);
static int result, saved_errno;

static void *probe(void *arg)
{
    (void)arg;
    result = check(&saved_errno);
    return NULL;
}

int main(void)
{
    char path[PATH_MAX];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) {
        printf("BAD nsget\n");
        return 1;
    }
    char *slash = strrchr(path, '/');
    if (!slash) {
        printf("BAD exe path\n");
        return 1;
    }
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libmac_syscall_low_stack.dylib");
    void *h = dlopen(path, RTLD_NOW);
    check = h ? (int (*)(int *))dlsym(h, "low_stack_sandbox_check") : NULL;
    if (!check) {
        printf("BAD dlopen: %s\n", dlerror());
        return 1;
    }
    size_t size = 1 << 20;
    void *stack = mmap((void *)0x40000000ull, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (stack == MAP_FAILED) {
        printf("BAD mmap\n");
        return 1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, size);
    pthread_t t;
    if (pthread_create(&t, &attr, probe, NULL) != 0) {
        printf("BAD thread\n");
        return 1;
    }
    pthread_join(t, NULL);
    if (result != 0) {
        printf("BAD sandbox_check %d errno %d\n", result, saved_errno);
        return 1;
    }
    printf("OK\n");
    return 0;
}
