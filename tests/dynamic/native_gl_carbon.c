#define GL_SILENCE_DEPRECATION
#include <Carbon/Carbon.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>
#include <stdio.h>
#include <string.h>

static void opengl(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAllowOfflineRenderers, kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;
    CGLError e1 = CGLChoosePixelFormat(attrs, &pf, &npf);
    CGLContextObj ctx = NULL;
    CGLError e2 = pf ? CGLCreateContext(pf, NULL, &ctx) : -1;
    CGLError e3 = ctx ? CGLSetCurrentContext(ctx) : -1;
    printf("cgl %d %d %d formats=%d\n", e1, e2, e3, npf > 0);
    if (!ctx)
        return;
    GLuint fbo = 0, rb = 0;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 4, 4);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    glViewport(0, 0, 4, 4);
    glClearColor(0.25f, 0.5f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 2, 2);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    unsigned char px[4 * 4 * 4];
    memset(px, 0, sizeof px);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLint viewport[4] = { 0 };
    glGetIntegerv(GL_VIEWPORT, viewport);
    printf("gl status=%#x err=%#x viewport=%d,%d,%d,%d px0=%u,%u,%u,%u px15=%u,%u,%u,%u version=%d\n", status,
           glGetError(), viewport[0], viewport[1], viewport[2], viewport[3], px[0], px[1], px[2], px[3], px[60],
           px[61], px[62], px[63], glGetString(GL_VERSION) != NULL);
    /* gluCheckExtension, which Wine's opengl32.so checks extension strings with. */
    printf("glu has=%d hasnot=%d last=%d\n",
           gluCheckExtension((const GLubyte *)"GL_EXT_b", (const GLubyte *)"GL_EXT_a GL_EXT_b GL_EXT_c"),
           gluCheckExtension((const GLubyte *)"GL_EXT_d", (const GLubyte *)"GL_EXT_a GL_EXT_b GL_EXT_c"),
           gluCheckExtension((const GLubyte *)"GL_EXT_c", (const GLubyte *)"GL_EXT_a GL_EXT_b GL_EXT_c"));
    glDeleteRenderbuffersEXT(1, &rb);
    glDeleteFramebuffersEXT(1, &fbo);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pf);
}

static void carbon(void)
{
    AbsoluteTime a = UpTime();
    Nanoseconds na = AbsoluteToNanoseconds(a);
    AbsoluteTime one = DurationToAbsolute(1000);
    Nanoseconds n1 = AbsoluteToNanoseconds(one);
    AbsoluteTime later = AddAbsoluteToAbsolute(a, one);
    Nanoseconds nl = AbsoluteToNanoseconds(later);
    uint64_t va = UnsignedWideToUInt64(na), v1 = UnsignedWideToUInt64(n1), vl = UnsignedWideToUInt64(nl);
    Duration back = AbsoluteToDuration(one);
    printf("carbon second=%llu sum=%d back=%d\n", (unsigned long long)v1, vl - va == v1, (int)back);
    EventHotKeyID id = { 'ocrz', 7 };
    EventHotKeyRef ref = NULL;
    OSStatus st = RegisterEventHotKey(0x2F, cmdKey | optionKey | controlKey | shiftKey, id,
                                      GetApplicationEventTarget(), 0, &ref);
    OSStatus un = ref ? UnregisterEventHotKey(ref) : -1;
    printf("hotkey %d %d %d\n", (int)st, ref != NULL, (int)un);
}

int main(void)
{
    opengl();
    carbon();
    return 0;
}
