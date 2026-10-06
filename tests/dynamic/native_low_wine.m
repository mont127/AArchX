/*
 * Native mode under a low shadow, the layout Wine's loader asks for: built
 * like it (no PIE, a 4 KB page zero, the image at 8 GB), so guest addresses
 * below 12 GB are not the host's.  Each part is a wall native Wine hit:
 *
 * - objc_autoreleasePoolPush on a thread with no pool answers objc's empty-pool
 *   token, 1, which has to reach objc_autoreleasePoolPop as 1, not as its
 *   shadow alias.
 * - A __block variable on a stack below 12 GB, captured by a block that
 *   dispatch_sync runs, forwards to a guest address libclosure must not follow.
 * - An anonymous mach_vm_map anywhere is memory the guest writes and native
 *   code reads: write(2) must see what the guest stored there.
 * - Wine's anon_mmap_tryfixed: a fixed anonymous mach_vm_map, then
 *   mmap(MAP_FIXED) over it; a second fixed map of the same range is refused.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *token);

static int g_byref_result;

static void byref_work(void)
{
    __block int counter = 40;
    dispatch_queue_t q = dispatch_queue_create("ocerz.low", NULL);
    dispatch_sync(q, ^{ counter += 1; });
    void (^copy)(void) = Block_copy(^{ counter += 1; });
    copy();
    Block_release(copy);
    g_byref_result = counter;
}

/* ocerz gives a guest thread a stack of its own, so the frame is moved by hand,
   as Wine moves its threads onto the stacks it allocated. */
static void call_on_stack(void (*fn)(void), void *top)
{
    __asm__ volatile("mov %%rsp, %%rbx\n\t"
                     "mov %0, %%rsp\n\t"
                     "call *%1\n\t"
                     "mov %%rbx, %%rsp\n\t"
                     :
                     : "r"(top), "r"(fn)
                     : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc",
                       "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
                       "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static void byref_on_low_stack(void)
{
    static const mach_vm_address_t candidates[] = { 0x2e0000000ull, 0x2f0000000ull, 0x120000000ull, 0x130000000ull,
                                                    0x140000000ull };
    mach_vm_size_t size = 1 << 20;
    mach_vm_address_t stack = 0;
    for (unsigned k = 0; k < sizeof candidates / sizeof candidates[0] && !stack; k++) {
        mach_vm_address_t a = candidates[k];
        if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) == KERN_SUCCESS)
            stack = a;
    }
    if (stack)
        call_on_stack(byref_work, (void *)(uintptr_t)(stack + size - 64));
    printf("__block on a stack below 12 GB (%d): %d\n", stack && stack < 0x300000000ull, g_byref_result);
}

static void coherent_anywhere(void)
{
    mach_vm_address_t a = 0;
    kern_return_t kr = mach_vm_map(mach_task_self(), &a, 0x4000, 0, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, 0,
                                   VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY);
    int fds[2];
    char back[8] = { 0 };
    int same = 0;
    if (kr == KERN_SUCCESS && pipe(fds) == 0) {
        memcpy((void *)(uintptr_t)a, "coherent", 8);
        same = write(fds[1], (void *)(uintptr_t)a, 8) == 8 && read(fds[0], back, 8) == 8 &&
               memcmp(back, "coherent", 8) == 0;
        close(fds[0]);
        close(fds[1]);
        mach_vm_deallocate(mach_task_self(), a, 0x4000);
    }
    printf("anonymous mach_vm_map anywhere: kr=%d, native code reads the guest's bytes: %d\n", kr, same);
}

static void tryfixed(void)
{
    mach_vm_address_t want = 0x500000000000ull, a = want;
    mach_vm_size_t size = 0x400000000ull;
    kern_return_t kr = mach_vm_map(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, 0, 0,
                                   VM_PROT_ALL, VM_INHERIT_COPY);
    void *p = MAP_FAILED;
    if (kr == KERN_SUCCESS)
        p = mmap((void *)(uintptr_t)want, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    mach_vm_address_t b = want;
    kern_return_t again = mach_vm_map(mach_task_self(), &b, 0x4000, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, 0,
                                      0, VM_PROT_ALL, VM_INHERIT_COPY);
    char *q = mmap((void *)(uintptr_t)want, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1,
                   0);
    int usable = q == (char *)(uintptr_t)want && (q[0] = 7, q[0] == 7);
    if (kr == KERN_SUCCESS)
        mach_vm_deallocate(mach_task_self(), want, size);
    printf("16 GB fixed reservation: kr=%d, mmap over it: %d, fixed again: %d, committed page usable: %d\n", kr,
           p == (void *)(uintptr_t)want, again, usable);
}

static void *pool_thread(void *arg)
{
    void *token = objc_autoreleasePoolPush();
    *(int *)arg = token == (void *)1;
    objc_autoreleasePoolPop(token);
    return NULL;
}

int main(void)
{
    int placeholder = 0;
    pthread_t t;
    pthread_create(&t, NULL, pool_thread, &placeholder);
    pthread_join(t, NULL);
    printf("empty autorelease pool popped, token was the placeholder: %d\n", placeholder);
    byref_on_low_stack();
    coherent_anywhere();
    tryfixed();
    return 0;
}
