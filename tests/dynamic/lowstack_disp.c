/*
 * [rsp + disp] accesses as src/jit.c emits them in the Wine layout
 * (lowstack_disp_ea): one rsp + x0 shared by the stack slots between two
 * moves of rsp, the displacement carried by each load or store.  Every shape
 * that goes through it (mov loads and stores of each size, arithmetic and
 * compares with a stack operand, movzx/movsx/movsxd, read-modify-writes, xchg,
 * xmm loads and stores, a movups pair), displacements at and past the ends of
 * the scaled and unscaled encodings, and push, pop, call and a switch of rsp
 * between two accesses.  Runs on the thread's stack and on one mapped below
 * 12 GB, where the stack delta is not zero; prints each, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

__attribute__((noinline)) static uint64_t leaf(uint64_t a) { return a * 7 + 1; }

/* Runs with rsp = top (16-byte aligned, 64 KB below it usable, 40 KB above it); out[12..14] carry seed, top, leaf. */
__attribute__((noinline)) static uint64_t body(uint64_t seed, uint8_t *top, uint64_t out[16])
{
    uint64_t r;
    out[12] = seed; out[13] = (uint64_t)(uintptr_t)top; out[14] = (uint64_t)(uintptr_t)leaf;
    __asm__ volatile(
        "mov %%rsp, %%r15\n\t"
        "mov 104(%[o]), %%rsp\n\t"
        /* stores at the ends of each encoding */
        "mov 96(%[o]), %%rax\n\t"
        "mov %%rax, 0(%%rsp)\n\t"
        "lea 1(%%rax), %%rcx\n\t"  "mov %%rcx, 8(%%rsp)\n\t"
        "lea 2(%%rax), %%rcx\n\t"  "mov %%rcx, 0x4a0(%%rsp)\n\t"
        "lea 3(%%rax), %%rcx\n\t"  "mov %%rcx, 32760(%%rsp)\n\t"
        "lea 4(%%rax), %%rcx\n\t"  "mov %%rcx, 32768(%%rsp)\n\t"
        "lea 5(%%rax), %%rcx\n\t"  "mov %%rcx, -8(%%rsp)\n\t"
        "lea 6(%%rax), %%rcx\n\t"  "mov %%rcx, -256(%%rsp)\n\t"
        "lea 7(%%rax), %%rcx\n\t"  "mov %%rcx, -264(%%rsp)\n\t"
        "mov %%eax, 0x14(%%rsp)\n\t"
        "mov %%ax, 0x1a(%%rsp)\n\t"
        "mov %%al, 0x1d(%%rsp)\n\t"
        "movl $0x12345678, 0x24(%%rsp)\n\t"
        "movq $-3, 0x28(%%rsp)\n\t"
        /* loads back, mixed sizes */
        "mov 0(%%rsp), %%rbx\n\t"
        "add 8(%%rsp), %%rbx\n\t"
        "sub 0x4a0(%%rsp), %%rbx\n\t"
        "xor 32760(%%rsp), %%rbx\n\t"
        "add 32768(%%rsp), %%rbx\n\t"
        "add -8(%%rsp), %%rbx\n\t"
        "add -256(%%rsp), %%rbx\n\t"
        "add -264(%%rsp), %%rbx\n\t"
        "mov 0x14(%%rsp), %%ecx\n\t"    "add %%rcx, %%rbx\n\t"
        "movzwl 0x1a(%%rsp), %%ecx\n\t" "add %%rcx, %%rbx\n\t"
        "movzbl 0x1d(%%rsp), %%ecx\n\t" "add %%rcx, %%rbx\n\t"
        "movsbq 0x1d(%%rsp), %%rcx\n\t" "add %%rcx, %%rbx\n\t"
        "movslq 0x24(%%rsp), %%rcx\n\t" "add %%rcx, %%rbx\n\t"
        "movswl 0x26(%%rsp), %%ecx\n\t" "add %%rcx, %%rbx\n\t"
        "mov %%rbx, 0(%[o])\n\t"
        /* read-modify-writes, xchg, a compare against the stack */
        "addq $5, 8(%%rsp)\n\t"
        "subl %%eax, 0x14(%%rsp)\n\t"
        "notq 0x28(%%rsp)\n\t"
        "incw 0x1a(%%rsp)\n\t"
        "xchg %%rbx, 0x4a0(%%rsp)\n\t"
        "mov 8(%%rsp), %%rcx\n\t"       "mov %%rcx, 8(%[o])\n\t"
        "mov 0x14(%%rsp), %%ecx\n\t"    "mov %%rcx, 16(%[o])\n\t"
        "mov 0x28(%%rsp), %%rcx\n\t"    "mov %%rcx, 24(%[o])\n\t"
        "movzwl 0x1a(%%rsp), %%ecx\n\t" "mov %%rcx, 32(%[o])\n\t"
        "mov 0x4a0(%%rsp), %%rcx\n\t"   "mov %%rcx, 40(%[o])\n\t"
        "mov %%rbx, 48(%[o])\n\t"
        "xor %%ecx, %%ecx\n\t"
        "cmp %%rax, 0(%%rsp)\n\t"       "sete %%cl\n\t"
        "cmpl $0x12345678, 0x24(%%rsp)\n\t" "setne %%ch\n\t"
        "mov %%rcx, 56(%[o])\n\t"
        /* xmm slots and a movups pair */
        "movq %%rax, %%xmm0\n\t"
        "pshufd $0x44, %%xmm0, %%xmm0\n\t"
        "movaps %%xmm0, 0x30(%%rsp)\n\t"
        "movups %%xmm0, -0x18(%%rsp)\n\t"
        "movsd 0x30(%%rsp), %%xmm1\n\t"
        "movups 0x2c(%%rsp), %%xmm2\n\t"
        "movups %%xmm1, 0x40(%%rsp)\n\t"
        "movups %%xmm2, 0x50(%%rsp)\n\t"
        "movups 0x40(%%rsp), %%xmm3\n\t"
        "movups 0x50(%%rsp), %%xmm4\n\t"
        "paddq %%xmm4, %%xmm3\n\t"
        "paddq -0x18(%%rsp), %%xmm3\n\t"
        "movq %%xmm3, 64(%[o])\n\t"
        "pextrq $1, %%xmm3, 72(%[o])\n\t"
        /* push, pop and call between two slot accesses, then rsp moves */
        "mov 8(%%rsp), %%rcx\n\t"
        "push %%rcx\n\t"
        "mov 16(%%rsp), %%rdx\n\t"       /* the old 8(%rsp) */
        "pop %%rdi\n\t"
        "add %%rdx, %%rdi\n\t"
        "mov %%rdi, 0x60(%%rsp)\n\t"
        "call *112(%[o])\n\t"
        "add 0x60(%%rsp), %%rax\n\t"
        "sub $0x100, %%rsp\n\t"
        "mov %%rax, 0x108(%%rsp)\n\t"
        "mov 0x160(%%rsp), %%rcx\n\t"
        "add $0x100, %%rsp\n\t"
        "add 8(%%rsp), %%rcx\n\t"
        "mov %%rcx, 80(%[o])\n\t"
        /* rsp back to the thread's stack and a slot there, in the same block */
        "mov %%rsp, %%rdx\n\t"
        "lea -0x200(%%r15), %%rsp\n\t"
        "mov %%rax, -0x40(%%rsp)\n\t"
        "mov -0x40(%%rsp), %%rcx\n\t"
        "mov %%rdx, %%rsp\n\t"
        "add 8(%%rsp), %%rcx\n\t"
        "mov %%rcx, 88(%[o])\n\t"
        "mov %%r15, %%rsp\n\t"
        "mov %%rax, %[r]"
        : [r] "=&a"(r)
        : [o] "r"(out)
        : "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r15",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
    return r;
}

static void run(const char *name, uint8_t *top)
{
    uint64_t acc = 0, out[16];
    for (uint64_t i = 0; i < 3000; i++) {
        memset(out, 0, sizeof out);
        uint64_t r = body(0x0123456789abcdefull ^ (i * 0x9e3779b97f4a7c15ull), top, out);
        acc = acc * 1000003 + r;
        for (int k = 0; k < 12; k++) acc = acc * 31 + out[k];
        if (i == 0)
            for (int k = 0; k < 12; k++) printf("%s out[%d] %016llx\n", name, k, (unsigned long long)out[k]);
    }
    printf("%s acc %016llx\n", name, (unsigned long long)acc);
}

__attribute__((noinline)) static void on_thread_stack(void)
{
    uint8_t buf[160 * 1024] __attribute__((aligned(16)));
    run("thread", buf + 96 * 1024);
    __asm__ volatile("" : : "r"(buf) : "memory");
}

int main(void)
{
    on_thread_stack();
    /* below 12 GB, where the Wine layout maps it at low_base | address (anywhere if refused) */
    uint8_t *low = mmap((void *)0x2f0000000, 160 * 1024, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (low == MAP_FAILED)
        low = mmap(NULL, 160 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (low == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    run("low", low + 96 * 1024);
    return 0;
}
