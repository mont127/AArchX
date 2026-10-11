/*
 * Flags read by several consumers well after the instruction that set them
 * (src/jit.c, nzcv_fuse_producer): the window a compare's NZCV is handed on
 * through, and which consumers may have siblings in it that write the
 * producer's operands.
 * - lz_bit: 7-Zip's LZMA range-decoder step as its compiler wrote it, a sub
 *   then two cmovae, two cmovb and an sbb up to 8 instructions on, with movs
 *   and the cmovs (which rewrite the sub's operands) between;
 * - adc_late: the same with an adc reading the carry instead of the sbb;
 * - edge: consumers at 7 and 8 instructions from a cmp, and one at 9, which
 *   is past the window and reads the flags another way;
 * - jcc_after: a jcc after cmov siblings that rewrote the compare's operands,
 *   which may not take the handed-on flags;
 * - lz_load: the decoder's step with the load of the next probability between
 *   the sub and its last consumers, whose guard may clobber NZCV, and the same
 *   with that load faulting once: the handler sees the sub's carry in rflags,
 *   maps the page and the step goes on;
 * - vc_load: a sub whose consumers test ZF or SF, which may read its result's
 *   register instead of NZCV, with loads between;
 * - carry_imm: adc and sbb of immediates of each sign and size, 32 and 64 bits,
 *   each after its own compare (a csinc after at most one add or sub);
 * each over inputs on both sides of every condition.  Against Rosetta.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* lz_bit(range, code, prob, sym) -> mixes every register the step writes */
    "_lz_bit:\n"
    "    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %rbp\n"
    "    mov %edi, %eax\n    mov %esi, %ebp\n    mov %edx, %ecx\n    mov %ecx, %ebx\n"
    "    mov $0x111, %r12d\n    mov $0x222, %r14d\n    mov $0x333, %r9d\n    mov $0x7ff, %r10d\n"
    "    mov %eax, %edi\n"
    "    shr $0xb, %eax\n"
    "    imul %ecx, %eax\n"
    "    sub %eax, %edi\n"
    "    mov %ebp, %esi\n"
    "    sub %eax, %ebp\n"
    "    cmovae %r14d, %r12d\n"
    "    mov %r9d, %r14d\n"
    "    cmovae %edi, %eax\n"
    "    mov $0x1f, %edi\n"
    "    cmovb %esi, %ebp\n"
    "    mov %ebx, %esi\n"
    "    cmovb %r10d, %edi\n"
    "    sbb $-1, %ebx\n"
    "    shl $7, %r12\n    xor %r12, %rax\n    shl $5, %r14\n    xor %r14, %rax\n"
    "    shl $9, %rbp\n    xor %rbp, %rax\n    shl $3, %rdi\n    xor %rdi, %rax\n"
    "    shl $11, %rsi\n    xor %rsi, %rax\n    shl $13, %rbx\n    xor %rbx, %rax\n"
    "    pop %rbp\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* adc_late(a, b, c): sub, cmov siblings rewriting the sub's operands, an adc */
    "_adc_late:\n"
    "    push %rbx\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n    mov %edx, %ebx\n    mov $5, %r8d\n    mov $9, %r9d\n"
    "    sub %ecx, %eax\n"
    "    cmovb %r8d, %eax\n"
    "    mov %ebx, %r10d\n"
    "    cmovae %r9d, %ecx\n"
    "    mov $3, %r11d\n"
    "    cmovb %r11d, %r10d\n"
    "    adc $0x40, %ebx\n"
    "    shl $16, %rcx\n    xor %rcx, %rax\n    shl $20, %rbx\n    xor %rbx, %rax\n    shl $40, %r10\n    xor %r10, %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* edge(a, b): consumers 7, 8 and 9 instructions after a cmp */
    "_edge:\n"
    "    xor %eax, %eax\n    xor %ecx, %ecx\n    xor %edx, %edx\n    mov $1, %r8d\n"
    "    cmp %esi, %edi\n"
    "    mov %edi, %r9d\n    mov %esi, %r10d\n    mov $7, %r11d\n    lea 1(%r8), %r8d\n    mov %r9d, %r11d\n    lea 2(%r10), %r10d\n"
    "    cmovl %r8d, %eax\n"
    "    setg %cl\n"
    "    setle %dl\n"
    "    shl $8, %rcx\n    or %rcx, %rax\n    shl $16, %rdx\n    or %rdx, %rax\n"
    "    shl $24, %r10\n    xor %r10, %rax\n    shl $40, %r11\n    xor %r11, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* jcc_after(a, b): a jcc after cmovs that rewrote the compare's operands */
    "_jcc_after:\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n    mov $0x55, %r8d\n    mov $0x66, %r9d\n"
    "    cmp %ecx, %eax\n"
    "    cmovb %r8d, %eax\n"
    "    cmovae %r9d, %ecx\n"
    "    jb 1f\n"
    "    lea 0x1000(%rax,%rcx), %eax\n"
    "1:  shl $20, %rcx\n    xor %rcx, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* lz_load(range, code, probs, prob): a load from probs between the sub and its last consumers */
    "_lz_load:\n"
    "    push %rbx\n    push %rbp\n"
    "    mov %edi, %eax\n    mov %esi, %ebp\n    mov %rdx, %r11\n    mov $5, %edx\n    mov $0x7ff, %r10d\n"
    "    mov %eax, %edi\n"
    "    shr $0xb, %eax\n"
    "    imul %ecx, %eax\n"
    "    sub %eax, %edi\n"
    "    mov %ebp, %esi\n"
    "    sub %eax, %ebp\n"
    "    cmovae %edi, %eax\n"
    "    movzwl 0x6(%r11), %edi\n"
    "    cmovae %edi, %edx\n"
    "    mov $0x1f, %edi\n"
    "    cmovb %esi, %ebp\n"
    "    cmovb %r10d, %edi\n"
    "    mov $2, %ebx\n"
    "    sbb $-1, %ebx\n"
    "    shl $9, %rbp\n    xor %rbp, %rax\n    shl $3, %rdi\n    xor %rdi, %rax\n"
    "    shl $17, %rdx\n    xor %rdx, %rax\n    shl $29, %rbx\n    xor %rbx, %rax\n"
    "    pop %rbp\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* vc_load(a, b, p): sub; load; cmove; load; cmovs; jne, then the same in 64 bits with an immediate */
    "_vc_load:\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n    mov $0x11, %r8d\n    mov $0x22, %r9d\n    mov $0x33, %r11d\n"
    "    sub %ecx, %eax\n"
    "    movslq 4(%rdx), %r10\n"
    "    cmove %r9d, %r10d\n"
    "    movzwl 2(%rdx), %r9d\n"
    "    cmovs %ecx, %r8d\n"
    "    jne 1f\n"
    "    add $0x77, %r11d\n"
    "1:  mov %edi, %ecx\n"
    "    sub $0x1000, %rcx\n"
    "    movzbl 1(%rdx), %esi\n"
    "    cmove %r8d, %esi\n"
    "    je 2f\n"
    "    add $0x99, %r11d\n"
    "2:  shl $20, %r10\n    xor %r10, %rax\n    shl $40, %r8\n    xor %r8, %rax\n    shl $8, %r9\n    xor %r9, %rax\n"
    "    shl $32, %rcx\n    xor %rcx, %rax\n    shl $48, %rsi\n    xor %rsi, %rax\n    shl $12, %r11\n    xor %r11, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* carry_imm(a, b, c) */
    "_carry_imm:\n"
    "    mov %edx, %r8d\n    mov %rdx, %r9\n    mov %edx, %r10d\n    mov %rdx, %r11\n    mov %rdx, %rcx\n"
    "    cmp %esi, %edi\n    sbb $-2, %r8d\n"
    "    cmp %edi, %esi\n    adc $-0x1000, %r9\n"
    "    cmp %esi, %edi\n    adc $0x12345, %r10d\n"
    "    cmp %edi, %esi\n    sbb $0x7fffffff, %r11\n"
    "    cmp %rsi, %rdi\n    sbb $-0x80000000, %rcx\n"
    "    mov %edi, %eax\n    sub %esi, %eax\n    adc $-1, %edx\n"
    "    shl $8, %r8\n    xor %r8, %rax\n    rol $16, %r9\n    xor %r9, %rax\n    shl $24, %r10\n    xor %r10, %rax\n"
    "    rol $32, %r11\n    xor %r11, %rax\n    rol $40, %rcx\n    xor %rcx, %rax\n    shl $48, %rdx\n    xor %rdx, %rax\n"
    "    ret\n");

uint64_t lz_bit(uint32_t range, uint32_t code, uint32_t prob, uint32_t sym);
uint64_t adc_late(uint32_t a, uint32_t b, uint32_t c);
uint64_t edge(uint32_t a, uint32_t b);
uint64_t jcc_after(uint32_t a, uint32_t b);
uint64_t lz_load(uint32_t range, uint32_t code, const uint16_t *probs, uint32_t prob);
uint64_t vc_load(uint32_t a, uint32_t b, const uint8_t *p);
uint64_t carry_imm(uint64_t a, uint64_t b, uint64_t c);

static uint8_t *g_page;
static volatile int g_faults, g_fault_cf;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    g_faults++;
    g_fault_cf = g_fault_cf << 1 | (int)(uc->uc_mcontext->__ss.__rflags & 1);
    mprotect(g_page, 16384, PROT_READ | PROT_WRITE);
}

int main(void)
{
    uint64_t s = 0x9e3779b97f4a7c15ull, h[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 400000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(s >> 32), y = (uint32_t)s;
        uint32_t range = x | 0x1000000u, code = y % range, prob = (x >> 20) & 0x7ff;
        h[0] = h[0] * 31 + lz_bit(range, code, prob, y & 0xff);
        h[1] = h[1] * 31 + adc_late(x, (i & 1) ? x ^ (y & 15) : y, y >> 7);
        h[2] = h[2] * 31 + edge((int32_t)x >> (i & 7), (int32_t)y >> (i & 5));
        h[3] = h[3] * 31 + jcc_after(x & 0xffff, y & 0xffff);
    }
    printf("lz_bit %llx adc_late %llx edge %llx jcc_after %llx\n", (unsigned long long)h[0],
           (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3]);

    uint16_t probs[8];
    uint64_t hl = 0;
    for (int i = 0; i < 400000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(s >> 32), y = (uint32_t)s;
        for (int k = 0; k < 8; k++) probs[k] = (uint16_t)(y >> k);
        uint32_t range = x | 0x1000000u, code = y % range;
        hl = hl * 31 + lz_load(range, code, probs, (x >> 20) & 0x7ff);
    }
    /* the load faults once: probs + 6 is the first byte of a page not yet readable */
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
    for (int r = 0; r < 4; r++) {
        mprotect(g_page, 16384, PROT_NONE);
        uint32_t range = 0x1234567u + (uint32_t)r * 0x10000000u, code = (r & 1) ? 0x10 : range - 0x10;
        hf = hf * 31 + lz_load(range, code, (const uint16_t *)(void *)(g_page - 6), 0x400);
    }
    printf("lz_load %llx fault %llx faults %d cf %x\n", (unsigned long long)hl, (unsigned long long)hf, g_faults, g_fault_cf);

    static const uint32_t edge[] = { 0, 1, 0x1000, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfff, 0x1001 };
    uint8_t bytes[8];
    uint64_t hv = 0;
    for (int i = 0; i < 400000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(s >> 32), y = (uint32_t)s;
        for (int k = 0; k < 8; k++) bytes[k] = (uint8_t)(y >> (3 * k));
        if (i & 1) x = edge[(y >> 3) & 7];
        if (i & 2) y = (i & 4) ? x : edge[(y >> 9) & 7];
        hv = hv * 31 + vc_load(x, y, bytes);
    }
    printf("vc_load %llx\n", (unsigned long long)hv);

    uint64_t hc = 0;
    for (int i = 0; i < 400000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t x = s, y = s * 0x9e3779b97f4a7c15ull;
        if (i & 1) y = (i & 2) ? x : x ^ ((uint64_t)1 << (i & 63));
        hc = hc * 31 + carry_imm(x, y, s ^ (s >> 29));
    }
    printf("carry_imm %llx\n", (unsigned long long)hc);
    return 0;
}
