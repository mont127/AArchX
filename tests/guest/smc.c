/* Runtime-generated code rewritten in place with plain stores (no mprotect):
 * a guest JIT like V8's writes new machine code into its RWX code range and
 * jumps to it.  The emulator must not keep running a translation made from
 * the old bytes.  Three shapes: the same buffer rewritten between calls, a
 * hot loop whose callee is rewritten while the loop runs, and a partial
 * rewrite in the middle of a function.  Golden from the native run. */
#include "gsys.h"

static void emit_ret_imm(unsigned char *p, unsigned v)
{
    /* mov eax, imm32 ; ret */
    p[0] = 0xb8; p[1] = (unsigned char)v; p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)(v >> 16); p[4] = (unsigned char)(v >> 24); p[5] = 0xc3;
}
typedef unsigned (*fn_t)(void);

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    unsigned char *code = (unsigned char *)sys_mmap(0, 4096, 7 /* rwx */, 0x1002 /* private|anon */, -1, 0);
    if ((long)code < 0 && (long)code > -4096) { g_puts("mmap failed\n"); return 1; }
    fn_t f = (fn_t)(void *)code;
    /* 1: rewrite between calls */
    emit_ret_imm(code, 1); g_putu64(f());
    emit_ret_imm(code, 2); g_putu64(f());
    emit_ret_imm(code, 3); g_putu64(f());
    /* 2: a hot caller loop; the callee is rewritten while the loop is chained */
    g_u64 sum = 0;
    for (int i = 0; i < 2000; i++) {
        if (i == 1000) emit_ret_imm(code, 10);
        sum += f();
    }
    g_putu64(sum);                       /* 1000*3 + 1000*10 */
    /* 3: partial rewrite in the middle of a longer function:
     * mov eax, 5 ; add eax, 7 ; ret  ->  patch the add's immediate to 9 */
    unsigned char *g = code + 64;
    g[0] = 0xb8; g[1] = 5; g[2] = 0; g[3] = 0; g[4] = 0;          /* mov eax,5 */
    g[5] = 0x83; g[6] = 0xc0; g[7] = 7;                            /* add eax,7 */
    g[8] = 0xc3;
    fn_t h = (fn_t)(void *)g;
    g_putu64(h());
    g[7] = 9;
    g_putu64(h());
    /* 4: rewrite the first bytes of a function that was already executed
     * from a chained loop, with a longer new body */
    unsigned char *k = code + 128;
    emit_ret_imm(k, 100);
    fn_t kf = (fn_t)(void *)k;
    sum = 0;
    for (int i = 0; i < 300; i++) sum += kf();
    k[0] = 0xb8; k[1] = 200; k[2] = 0; k[3] = 0; k[4] = 0;        /* mov eax,200 */
    k[5] = 0x05; k[6] = 1; k[7] = 0; k[8] = 0; k[9] = 0;          /* add eax,1 */
    k[10] = 0xc3;
    for (int i = 0; i < 300; i++) sum += kf();
    g_putu64(sum);                       /* 300*100 + 300*201 */
    return 0;
}
