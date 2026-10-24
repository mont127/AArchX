/*
 * A compare's flags read past loads with no record behind them
 * (src/jit.c, nzcv_regen_from_regs): when nothing between a cmp, sub or add
 * and its consumers writes its registers, NZCV comes back from them after a
 * load that clobbered it, and a fault at such a load rebuilds the flags from
 * them too (JFF_CMP_OPS, JFF_SUB_RESULT_SRC, JFF_ADD_RESULT_SRC), so a
 * handler sees what the hardware leaves in rflags.  Each shape runs over
 * inputs on both sides of its conditions, then with its load faulting once
 * per call: the handler records CF, ZF, SF and OF and maps the page.
 * - cmp_load: cmp of two 32-bit registers, a movzx load, setb seta setl sete;
 * - sub_load: sub of 32-bit registers, two loads, setb seto sete, a jcc;
 * - add_load: add of 64-bit registers, a movsxd load, setc sets seto;
 * - cmp_imm: cmp of a register and an immediate, a mov load, setl setb setg.
 * Against Rosetta.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* cmp_load(a, b, p) */
    "_cmp_load:\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n"
    "    cmp %ecx, %eax\n"
    "    movzwl 0x6(%rdx), %r8d\n"
    "    setb %r9b\n    seta %r10b\n    setl %r11b\n    sete %sil\n"
    "    movzbl %r9b, %r9d\n    movzbl %r10b, %r10d\n    movzbl %r11b, %r11d\n    movzbl %sil, %esi\n"
    "    shl $1, %r10\n    shl $2, %r11\n    shl $3, %rsi\n    or %r10, %r9\n    or %r11, %r9\n    or %rsi, %r9\n"
    "    shl $8, %r8\n    lea (%r8,%r9), %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* sub_load(a, b, p) */
    "_sub_load:\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n"
    "    sub %ecx, %eax\n"
    "    movzwl 0x6(%rdx), %r8d\n"
    "    mov 0x8(%rdx), %r9d\n"
    "    setb %r10b\n    seto %r11b\n    sete %sil\n"
    "    jbe 1f\n    or $0x100, %r9d\n1:\n"
    "    movzbl %r10b, %r10d\n    movzbl %r11b, %r11d\n    movzbl %sil, %esi\n"
    "    shl $1, %r11\n    shl $2, %rsi\n    or %r11, %r10\n    or %rsi, %r10\n"
    "    shl $32, %rax\n    xor %rax, %r10\n    shl $8, %r8\n    xor %r8, %r10\n    shl $40, %r9\n    xor %r9, %r10\n"
    "    mov %r10, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* add_load(a, b, p) */
    "_add_load:\n"
    "    mov %rdi, %rax\n    mov %rsi, %rcx\n"
    "    add %rcx, %rax\n"
    "    movslq 0x4(%rdx), %r8\n"
    "    setc %r9b\n    sets %r10b\n    seto %r11b\n"
    "    movzbl %r9b, %r9d\n    movzbl %r10b, %r10d\n    movzbl %r11b, %r11d\n"
    "    shl $1, %r10\n    shl $2, %r11\n    or %r10, %r9\n    or %r11, %r9\n"
    "    xor %r8, %rax\n    shl $3, %rax\n    or %r9, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* cmp_imm(a, p) */
    "_cmp_imm:\n"
    "    mov %edi, %eax\n"
    "    cmp $0x1234, %eax\n"
    "    mov 0x4(%rsi), %r8d\n"
    "    setl %r9b\n    setb %r10b\n    setg %r11b\n"
    "    movzbl %r9b, %r9d\n    movzbl %r10b, %r10d\n    movzbl %r11b, %r11d\n"
    "    shl $1, %r10\n    shl $2, %r11\n    or %r10, %r9\n    or %r11, %r9\n"
    "    shl $8, %r8\n    lea (%r8,%r9), %rax\n"
    "    ret\n");

uint64_t cmp_load(uint32_t a, uint32_t b, const void *p);
uint64_t sub_load(uint32_t a, uint32_t b, const void *p);
uint64_t add_load(uint64_t a, uint64_t b, const void *p);
uint64_t cmp_imm(uint32_t a, const void *p);

static uint8_t *g_page;
static volatile int g_faults;
static volatile uint64_t g_fault_flags;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ctx;
    uint64_t f = uc->uc_mcontext->__ss.__rflags;
    g_faults++;
    g_fault_flags = g_fault_flags << 4 | (f & 1) | (f >> 5 & 2) | (f >> 5 & 4) | (f >> 8 & 8);
    mprotect(g_page, 16384, PROT_READ | PROT_WRITE);
}

int main(void)
{
    static const uint32_t edge[] = { 0, 1, 0x1233, 0x1234, 0x1235, 0x7fffffff, 0x80000000u, 0xffffffffu };
    static const uint64_t edge64[] = { 0, 1, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
                                       0xfffffffffffffffeull };
    uint8_t buf[16];
    for (int k = 0; k < 16; k++) buf[k] = (uint8_t)(0x31 * k + 7);
    uint64_t h[4] = { 0, 0, 0, 0 }, s = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 200000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (i & 3) ? (uint32_t)(s >> 32) : edge[(s >> 8) & 7], y = (i & 5) ? (uint32_t)s : edge[(s >> 16) & 7];
        uint64_t x64 = (i & 7) ? s : edge64[(s >> 8) % 6], y64 = (i & 3) ? s * 3 : edge64[(s >> 20) % 6];
        buf[6] = (uint8_t)s;
        h[0] = h[0] * 31 + cmp_load(x, y, buf);
        h[1] = h[1] * 31 + sub_load(x, y, buf);
        h[2] = h[2] * 31 + add_load(x64, y64, buf);
        h[3] = h[3] * 31 + cmp_imm(x, buf);
    }
    printf("cmp_load %llx sub_load %llx add_load %llx cmp_imm %llx\n", (unsigned long long)h[0],
           (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint8_t *map = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { printf("map\n"); return 1; }
    g_page = map + 16384;
    uint64_t hf = 0;
    for (int r = 0; r < 8; r++) {
        uint32_t a = edge[r], b = edge[(r + 3) & 7];
        mprotect(g_page, 16384, PROT_NONE);
        hf = hf * 31 + cmp_load(a, b, g_page - 6);
        mprotect(g_page, 16384, PROT_NONE);
        hf = hf * 31 + sub_load(a, b, g_page - 6);
        mprotect(g_page, 16384, PROT_NONE);
        hf = hf * 31 + add_load(edge64[r % 6], edge64[(r + 2) % 6], g_page - 4);
        mprotect(g_page, 16384, PROT_NONE);
        hf = hf * 31 + cmp_imm(a, g_page - 4);
        printf("round %d faults %d flags %llx\n", r, g_faults, (unsigned long long)(g_fault_flags & 0xffff));
    }
    printf("fault %llx faults %d\n", (unsigned long long)hf, g_faults);
    return 0;
}
