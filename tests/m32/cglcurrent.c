/* CGLGetCurrentContext from i386 code, which Valve's ToGL asks before its OpenGL calls: the context made current,
 * another after a switch, none after clearing it, and per thread (expected output written by hand). */
#include <OpenGL/OpenGL.h>
#include <pthread.h>
#include <stdio.h>

static CGLContextObj a, b;

static void *other(void *arg)
{
    (void)arg;
    int none = CGLGetCurrentContext() == NULL;
    CGLSetCurrentContext(b);
    int mine = CGLGetCurrentContext() == b;
    CGLSetCurrentContext(NULL);
    printf("other thread: none at first %d, its own %d\n", none, mine);
    return NULL;
}

int main(void)
{
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, 0 };
    CGLPixelFormatObj pf;
    GLint n;
    CGLChoosePixelFormat(at, &pf, &n);
    CGLCreateContext(pf, NULL, &a);
    CGLCreateContext(pf, NULL, &b);
    int ok = 1;
    for (int i = 0; i < 1000; i++) {
        CGLSetCurrentContext(i & 1 ? b : a);
        ok &= CGLGetCurrentContext() == (i & 1 ? b : a);
    }
    printf("switches %d\n", ok);
    CGLSetCurrentContext(a);
    pthread_t t;
    pthread_create(&t, NULL, other, NULL);
    pthread_join(t, NULL);
    printf("main still %d\n", CGLGetCurrentContext() == a);
    CGLSetCurrentContext(NULL);
    printf("cleared %d\n", CGLGetCurrentContext() == NULL);
    CGLDestroyContext(a);
    CGLDestroyContext(b);
    return 0;
}
