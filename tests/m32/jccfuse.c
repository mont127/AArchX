/* Compare-and-branch shapes from Batman's hot code (GCC i386): the flags producer and its jcc with a load,
 * store, lea or plain move between them, a memory operand on the producer, flags still read after the branch, and a
 * gap load that crosses a 16-byte granule (the JIT rebuilds such a block after a fault in the middle of the pair). */
#include <stdio.h>
#include <string.h>

int t_or(int a, int b);
int t_loadgap(int a, int b);
int t_memprod(int a);
int t_leagap(unsigned a, unsigned b);
int t_storegap(int a);
int t_bytes(const unsigned char *p, const unsigned char *q);
int t_live(unsigned a, unsigned b);
int t_crossgap(int a, const unsigned char *p);
int t_movgap(int a, int b);

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl _t_or\n_t_or:\n"
    "  mov eax, [esp+4]\n  mov edx, [esp+8]\n  or edx, eax\n  jne 1f\n  mov eax, 2\n  ret\n1: mov eax, 1\n  ret\n"

    ".globl _t_loadgap\n_t_loadgap:\n"
    "  push ebp\n  mov ebp, esp\n  mov eax, [ebp+8]\n  cmp eax, 5\n  mov ecx, [ebp+12]\n  jne 1f\n  neg ecx\n"
    "1: mov eax, ecx\n  pop ebp\n  ret\n"

    ".globl _t_memprod\n_t_memprod:\n"
    "  push ebp\n  mov ebp, esp\n  cmp dword ptr [ebp+8], 2\n  mov eax, 0\n  jg 1f\n  pop ebp\n  ret\n"
    "1: mov eax, 7\n  pop ebp\n  ret\n"

    ".globl _t_leagap\n_t_leagap:\n"
    "  mov eax, [esp+4]\n  mov ecx, [esp+8]\n  cmp eax, ecx\n  lea edx, [eax-4]\n  jb 1f\n  mov edx, ecx\n"
    "1: mov eax, edx\n  ret\n"

    ".globl _t_storegap\n_t_storegap:\n"
    "  push ebp\n  mov ebp, esp\n  sub esp, 8\n  mov eax, [ebp+8]\n  test eax, eax\n  mov [ebp-4], eax\n  je 1f\n"
    "  mov eax, [ebp-4]\n  add esp, 8\n  pop ebp\n  ret\n1: mov eax, 9\n  add esp, 8\n  pop ebp\n  ret\n"

    ".globl _t_bytes\n_t_bytes:\n"
    "  push ebp\n  mov ebp, esp\n  push ebx\n  push edi\n  sub esp, 0x18\n  mov ebx, [ebp+8]\n  mov eax, [ebp+12]\n"
    "  movzx edi, byte ptr [ebx]\n  mov [ebp-0x14], edi\n  movzx edi, byte ptr [eax]\n  cmp edi, [ebp-0x14]\n  jne 2f\n"
    "  movzx edi, byte ptr [ebx+1]\n  mov [ebp-0x14], edi\n  movzx edi, byte ptr [eax+1]\n  cmp edi, [ebp-0x14]\n  jne 2f\n"
    "  mov eax, 1\n  jmp 3f\n2: xor eax, eax\n3: add esp, 0x18\n  pop edi\n  pop ebx\n  pop ebp\n  ret\n"

    ".globl _t_live\n_t_live:\n"
    "  mov eax, [esp+4]\n  mov edx, [esp+8]\n  cmp eax, edx\n  mov ecx, [esp+4]\n  jne 1f\n  sbb eax, eax\n  add eax, 100\n  ret\n"
    "1: sbb eax, eax\n  add eax, ecx\n  ret\n"

    ".globl _t_crossgap\n_t_crossgap:\n"
    "  push ebp\n  mov ebp, esp\n  mov edx, [ebp+12]\n  cmp dword ptr [ebp+8], 5\n  mov ecx, [edx+14]\n  jne 1f\n  neg ecx\n"
    "1: mov eax, ecx\n  pop ebp\n  ret\n"

    ".globl _t_movgap\n_t_movgap:\n"
    "  mov eax, [esp+4]\n  test eax, eax\n  mov edx, [esp+8]\n  mov ecx, eax\n  js 1f\n  lea eax, [ecx+edx]\n  ret\n"
    "1: lea eax, [edx-1]\n  ret\n"
    ".att_syntax\n");

int main(void)
{
    static unsigned char buf[64] __attribute__((aligned(16)));
    for (int i = 0; i < 64; i++)
        buf[i] = (unsigned char)(i * 37 + 11);
    unsigned sum[9] = { 0 };
    for (int i = 0; i < 400000; i++) {
        int a = (int)((unsigned)i * 2654435761u >> 28) - 7, b = (i % 13) - 6;
        sum[0] = sum[0] * 31 + (unsigned)t_or(a & (i & 1 ? -1 : 0), b & (i & 2 ? -1 : 0));
        sum[1] = sum[1] * 31 + (unsigned)t_loadgap(i % 9 == 0 ? 5 : a, b);
        sum[2] = sum[2] * 31 + (unsigned)t_memprod(a);
        sum[3] = sum[3] * 31 + (unsigned)t_leagap((unsigned)a, (unsigned)b);
        sum[4] = sum[4] * 31 + (unsigned)t_storegap(i % 5 == 0 ? 0 : a);
        unsigned char p[2] = { (unsigned char)(i & 3), (unsigned char)(i >> 2 & 1) }, q[2] = { 1, 0 };
        sum[5] = sum[5] * 31 + (unsigned)t_bytes(p, q);
        sum[6] = sum[6] * 31 + (unsigned)t_live((unsigned)(i % 7), (unsigned)(i % 5));
        sum[7] = sum[7] * 31 + (unsigned)t_crossgap(i % 3 == 0 ? 5 : a, buf + (i & 16));
        sum[8] = sum[8] * 31 + (unsigned)t_movgap(a, b);
    }
    for (int k = 0; k < 9; k++)
        printf("%d %08x\n", k, sum[k]);
    return 0;
}
