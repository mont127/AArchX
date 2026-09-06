/* A fused cmp/test/dec + jcc that ends a block, whose TAKEN target begins
 * with another jcc reading the same flags (libcef's btree cell walk:
 * `cmp ebp,0xb ; jbe L` ... `L: ja M`).  The JIT records the producer's
 * flags on the taken side, between the conditional branch and the chain
 * tail.  Chaining used to retarget the conditional branch straight at the
 * successor's body, skipping that record, so the successor branched on a
 * stale record left by an earlier compare.  Every profile database in
 * Steam's CEF failed to open ("out of memory") from that one edge.
 * A block takes at most six side exits, so six never-taken jccs in front of
 * the producer make its jcc the block terminator (libcef's block had six).
 * Each shape runs over alternating inputs so a stale record from the
 * previous call gives a visibly wrong answer.  Golden from the native run. */
#include "gsys.h"

static volatile g_u64 g_zero = 0;
static volatile g_u64 vals_a[12] = { 5, 11, 20, 11, 5, 11, 5, 20, 11, 5, 11, 11 };
static volatile g_u64 vals_b[12][2] = { {3,9},{9,9},{12,9},{9,9},{3,9},{9,9},{9,9},{3,9},{12,9},{9,9},{9,9},{3,9} };
static volatile g_u64 vals_c[12] = { 5, 1, 0, 5, 0, 1, 0, 5, 1, 0, 0, 5 };
static volatile g_u64 vals_d[12] = { 0x10, 0x80, 0x01, 0x80, 0x10, 0x01, 0x80, 0x10, 0x80, 0x01, 0x10, 0x80 };

/* imm compare; taken target decides with jb: 1 = below, 0 = equal, 2 = above */
static __attribute__((noinline)) g_u64 shape_a(g_u64 v)
{
    g_u64 r = 7;
    __asm__ __volatile__(
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "cmpq $11, %[v]\n\t"
        "jbe 1f\n\t"
        "movq $2, %[r]\n\t"
        "jmp 3f\n\t"
        "1:\n\t"
        "jb 2f\n\t"
        "movq $0, %[r]\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "movq $1, %[r]\n\t"
        "jmp 3f\n\t"
        "9:\n\t"
        "movq $9, %[r]\n\t"
        "3:\n\t"
        : [r] "+r"(r) : [v] "r"(v), [z] "r"(g_zero) : "cc", "memory");
    return r;
}

/* reg/reg compare; taken target decides with je: 0 = equal, 1 = below, 2 = above */
static __attribute__((noinline)) g_u64 shape_b(g_u64 a, g_u64 b)
{
    g_u64 r = 7;
    __asm__ __volatile__(
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "cmpq %[b], %[a]\n\t"
        "jbe 1f\n\t"
        "movq $2, %[r]\n\t"
        "jmp 3f\n\t"
        "1:\n\t"
        "je 2f\n\t"
        "movq $1, %[r]\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "movq $0, %[r]\n\t"
        "jmp 3f\n\t"
        "9:\n\t"
        "movq $9, %[r]\n\t"
        "3:\n\t"
        : [r] "+r"(r) : [a] "r"(a), [b] "r"(b), [z] "r"(g_zero) : "cc", "memory");
    return r;
}

/* dec + jne; taken target decides with js: 0 = hit zero, 1 = positive, 2 = negative */
static __attribute__((noinline)) g_u64 shape_c(g_u64 v)
{
    g_u64 r = 7;
    __asm__ __volatile__(
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "decq %[v]\n\t"
        "jne 1f\n\t"
        "movq $0, %[r]\n\t"
        "jmp 3f\n\t"
        "1:\n\t"
        "js 2f\n\t"
        "movq $1, %[r]\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "movq $2, %[r]\n\t"
        "jmp 3f\n\t"
        "9:\n\t"
        "movq $9, %[r]\n\t"
        "3:\n\t"
        : [r] "+r"(r), [v] "+r"(v) : [z] "r"(g_zero) : "cc", "memory");
    return r;
}

/* narrow test with a two-bit mask + jne; taken target decides with js:
 * 0 = no bit, 1 = bit 4 only, 2 = bit 7 set */
static __attribute__((noinline)) g_u64 shape_d(g_u64 v)
{
    g_u64 r = 7;
    __asm__ __volatile__(
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testq %[z], %[z]\n\t"
        "jne 9f\n\t"
        "testb $0x90, %b[v]\n\t"
        "jne 1f\n\t"
        "movq $0, %[r]\n\t"
        "jmp 3f\n\t"
        "1:\n\t"
        "js 2f\n\t"
        "movq $1, %[r]\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "movq $2, %[r]\n\t"
        "jmp 3f\n\t"
        "9:\n\t"
        "movq $9, %[r]\n\t"
        "3:\n\t"
        : [r] "+r"(r) : [v] "q"(v), [z] "r"(g_zero) : "cc", "memory");
    return r;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    for (int i = 0; i < 12; i++) { g_puts("a "); g_putu64(shape_a(vals_a[i])); }
    for (int i = 0; i < 12; i++) { g_puts("b "); g_putu64(shape_b(vals_b[i][0], vals_b[i][1])); }
    for (int i = 0; i < 12; i++) { g_puts("c "); g_putu64(shape_c(vals_c[i])); }
    for (int i = 0; i < 12; i++) { g_puts("d "); g_putu64(shape_d(vals_d[i])); }
    return 0;
}
