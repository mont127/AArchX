/* CoreFoundation allocating through a guest CFAllocator from several guest threads at once (a game's
 * allocator crossing back into the guest while other threads do the same). */
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_bad;

static void *alloc_cb(CFIndex size, CFOptionFlags hint, void *info)
{
    char *p = malloc((size_t)size + 16);
    if ((uintptr_t)p & 15)
        g_bad = 1;
    __sync_fetch_and_add((int *)info, 1);
    return p;
}
static void *realloc_cb(void *p, CFIndex size, CFOptionFlags hint, void *info) { return realloc(p, (size_t)size + 16); }
static int g_frees;
static void free_cb(void *p, void *info) { __sync_fetch_and_add(&g_frees, 1); free(p); }

static CFAllocatorRef g_alloc;
static int g_count;

static void *worker(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "thread %ld item %d", (long)arg, i);
        CFStringRef s = CFStringCreateWithCString(g_alloc, buf, kCFStringEncodingUTF8);
        CFMutableStringRef m = CFStringCreateMutableCopy(g_alloc, 0, s);
        CFStringAppend(m, CFSTR(" more"));
        CFURLRef u = CFURLCreateWithString(g_alloc, CFSTR("http://example.com/a/b"), NULL);
        if (!s || !m || !u || CFStringGetLength(m) < 10)
            g_bad = 1;
        for (int k = 0; k < 8; k++)   /* grow it through the guest allocator's reallocate */
            CFStringAppend(m, CFSTR(" and a longer tail to make the buffer move"));
        UniChar wide[512];
        CFIndex n = CFStringGetLength(m);
        CFStringGetCharacters(m, CFRangeMake(0, n < 512 ? n : 512), wide);
        char back[1024];
        if (!CFStringGetCString(m, back, sizeof back, kCFStringEncodingUTF8) || strncmp(back, buf, strlen(buf)) ||
            wide[0] != 't' || CFStringFind(m, CFSTR(" more and a longer"), 0).location != (CFIndex)strlen(buf))
            g_bad = 2;
        CFRelease(u);
        CFRelease(m);
        CFRelease(s);
    }
    return NULL;
}

int main(void)
{
    CFAllocatorContext ctx = { 0, &g_count, NULL, NULL, NULL, alloc_cb, realloc_cb, free_cb, NULL };
    g_alloc = CFAllocatorCreate(NULL, &ctx);
    pthread_t t[6];
    for (long i = 0; i < 6; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 6; i++)
        pthread_join(t[i], NULL);
    printf("allocations %s bad %d freed %s\n", g_count >= 6 * 2000 ? "many" : "few", g_bad,
           g_frees >= g_count - 64 ? "all" : "not all");
    return 0;
}
