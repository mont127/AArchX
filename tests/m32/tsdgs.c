/* A thread's specific data read straight through gs, as V8 (Left 4 Dead 2's Chromium) does: on i386 Darwin a key's
 * value is gs:[key * 4] (gs:[key * 8] on x86_64), on the main thread and on others. tsdgs.expected is the x86_64
 * build's output under Rosetta. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static pthread_key_t key;

static uintptr_t through_gs(uintptr_t k)
{
    uintptr_t v;
#if defined(__i386__)
    __asm__ volatile("movl %%gs:(,%1,4), %0" : "=r"(v) : "r"(k));
#else
    __asm__ volatile("movq %%gs:(,%1,8), %0" : "=r"(v) : "r"(k));
#endif
    return v;
}

static void *check(void *name)
{
    pthread_setspecific(key, (void *)0x1234CAFE);
    int set = through_gs(key) == 0x1234CAFE;
    pthread_setspecific(key, NULL);
    printf("%s: set %d cleared %d\n", (const char *)name, set, through_gs(key) == 0);
    return NULL;
}

int main(void)
{
    pthread_key_create(&key, NULL);
    check("main thread");
    pthread_t t;
    pthread_create(&t, NULL, check, "other thread");
    pthread_join(t, NULL);
    return 0;
}
