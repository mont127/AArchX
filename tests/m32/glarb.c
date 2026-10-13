/* ARB shader objects: GLhandleARB is void * on the Mac but holds an object name, which crosses as the integer it is.
 * As a pointer it became an m32 handle and Left 4 Dead 2's glUseProgram got 0xe0001030. glarb.expected is the x86_64
 * build's output under Rosetta. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, 0 };
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj ctx;
    CGLChoosePixelFormat(at, &pf, &n);
    CGLCreateContext(pf, NULL, &ctx);
    CGLSetCurrentContext(ctx);
    const GLcharARB *src = "void main() { gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }";
    GLhandleARB sh = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
    glShaderSourceARB(sh, 1, &src, NULL);
    glCompileShaderARB(sh);
    GLint compiled = 0, linked = 0;
    glGetObjectParameterivARB(sh, GL_OBJECT_COMPILE_STATUS_ARB, &compiled);
    GLhandleARB prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, sh);
    glLinkProgramARB(prog);
    glGetObjectParameterivARB(prog, GL_OBJECT_LINK_STATUS_ARB, &linked);
    glUseProgramObjectARB(prog);
    int current = glGetHandleARB(GL_PROGRAM_OBJECT_ARB) == prog;
    GLenum err1 = glGetError();
    glUseProgram((GLuint)(uintptr_t)prog);   /* the same name through the core entry point, as Source's ToGL does */
    GLenum err2 = glGetError();
    printf("names small %d compiled %d linked %d current %d errors %#x %#x\n",
           (uintptr_t)sh < 0x10000 && (uintptr_t)prog < 0x10000, compiled, linked, current, err1, err2);
    glDeleteObjectARB(prog);
    glDeleteObjectARB(sh);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return 0;
}
