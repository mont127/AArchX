/* Mapped buffers keep a mirror in guest memory: a map copies the buffer in only when what it holds may have changed
 * outside the guest's own calls.  The way Valve's ToGL fills a dynamic buffer (orphan with glBufferData, map, write,
 * flush the written range under GL_APPLE_flush_buffer_range, unmap), a partial write without flushing, glBufferData
 * with data, glBufferSubData, a READ_ONLY map, and a name deleted and made again.  Each result is read back from the
 * driver with glGetBufferSubData.  glmirror.expected is the x86_64 build's output under Rosetta. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <string.h>

static unsigned sum(const unsigned char *p, int n)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++)
        s = s * 31 + p[i];
    return s;
}

static unsigned driver(int n)
{
    static unsigned char back[4096];
    glGetBufferSubDataARB(GL_ARRAY_BUFFER, 0, n, back);
    return sum(back, n);
}

int main(void)
{
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, 0 };
    CGLPixelFormatObj pf;
    GLint np;
    CGLContextObj ctx;
    CGLChoosePixelFormat(at, &pf, &np);
    CGLCreateContext(pf, NULL, &ctx);
    CGLSetCurrentContext(ctx);
    unsigned char want[4096];
    GLuint b;
    glGenBuffersARB(1, &b);
    glBindBufferARB(GL_ARRAY_BUFFER, b);

    /* ToGL's dynamic buffer, three frames */
    glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE);
    for (int frame = 0; frame < 3; frame++) {
        glBufferDataARB(GL_ARRAY_BUFFER, 4096, NULL, GL_STREAM_DRAW);
        unsigned char *p = glMapBufferARB(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        for (int i = 0; i < 1024; i++)
            p[i] = want[i] = (unsigned char)(i * 7 + frame);
        glFlushMappedBufferRangeAPPLE(GL_ARRAY_BUFFER, 0, 1024);
        glUnmapBufferARB(GL_ARRAY_BUFFER);
        printf("frame %d flushed range %d\n", frame, driver(1024) == sum(want, 1024));
    }
    glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_TRUE);

    /* the whole buffer known, then part of it written through a map: the rest keeps its bytes */
    for (int i = 0; i < 4096; i++)
        want[i] = (unsigned char)(i * 13);
    glBufferDataARB(GL_ARRAY_BUFFER, 4096, want, GL_DYNAMIC_DRAW);
    unsigned char *p = glMapBufferARB(GL_ARRAY_BUFFER, GL_READ_WRITE);
    printf("data then map reads it %d\n", sum(p, 4096) == sum(want, 4096));
    for (int i = 100; i < 200; i++)
        p[i] = want[i] = 0x5a;
    glUnmapBufferARB(GL_ARRAY_BUFFER);
    printf("partial write kept the rest %d\n", driver(4096) == sum(want, 4096));

    /* glBufferSubData, then a READ_ONLY map */
    memset(want + 1000, 0x33, 50);
    glBufferSubDataARB(GL_ARRAY_BUFFER, 1000, 50, want + 1000);
    p = glMapBufferARB(GL_ARRAY_BUFFER, GL_READ_ONLY);
    printf("sub data seen through a map %d\n", sum(p, 4096) == sum(want, 4096));
    glUnmapBufferARB(GL_ARRAY_BUFFER);
    printf("read only map changed nothing %d\n", driver(4096) == sum(want, 4096));

    /* deleted and made again: the new buffer's own contents */
    glDeleteBuffersARB(1, &b);
    glGenBuffersARB(1, &b);
    glBindBufferARB(GL_ARRAY_BUFFER, b);
    for (int i = 0; i < 4096; i++)
        want[i] = (unsigned char)(255 - i);
    glBufferDataARB(GL_ARRAY_BUFFER, 4096, want, GL_STATIC_DRAW);
    p = glMapBufferARB(GL_ARRAY_BUFFER, GL_READ_ONLY);
    printf("new buffer's contents %d\n", sum(p, 4096) == sum(want, 4096));
    glUnmapBufferARB(GL_ARRAY_BUFFER);
    printf("errors %#x\n", glGetError());
    glDeleteBuffersARB(1, &b);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return 0;
}
