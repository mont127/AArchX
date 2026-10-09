/*
 * Conditions after inc and dec, which src/jit.c evaluates from the lazy flags
 * record by redoing the add or sub of 1 (emit_cc_predicate_ex), and whose old
 * CF it rebuilds only when a later instruction reads it (incdec_cf_live).
 *
 * Each case sets CF with a compare, then incs or decs a register or a memory
 * word of 32 or 64 bits, and reads one condition with a jcc: every condition
 * but parity, across the values where ZF, SF and OF turn (0, 1, -1 and the
 * signed limits).  The carry conditions see the CF from before the inc or dec.
 * Last, a reference count released in a loop: dec of a memory word, je.
 * Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

#define COND_CASE(name, op, jcc)                                                    \
    static int name(uint64_t v, int carry)                                          \
    {                                                                               \
        uint64_t r = v, m = v;                                                      \
        int t;                                                                      \
        __asm__ volatile(                                                           \
            "cmp %3, %4\n\t"                                                        \
            op "\n\t"                                                               \
            jcc " 1f\n\t" "mov $0, %0\n\t" "jmp 2f\n\t" "1: mov $1, %0\n\t" "2:\n\t"  \
            : "=&r"(t), "+r"(r), "+m"(m) : "r"((uint64_t)carry), "r"((uint64_t)0)   \
            : "cc");                                                                \
        return t;                                                                   \
    }

#define CONDS(stem, op)                                                              \
    COND_CASE(stem##_e, op, "je") COND_CASE(stem##_ne, op, "jne")                    \
    COND_CASE(stem##_s, op, "js") COND_CASE(stem##_ns, op, "jns")                    \
    COND_CASE(stem##_l, op, "jl") COND_CASE(stem##_ge, op, "jge")                    \
    COND_CASE(stem##_le, op, "jle") COND_CASE(stem##_g, op, "jg")                    \
    COND_CASE(stem##_o, op, "jo") COND_CASE(stem##_no, op, "jno")                    \
    COND_CASE(stem##_b, op, "jb") COND_CASE(stem##_ae, op, "jae")                    \
    COND_CASE(stem##_be, op, "jbe") COND_CASE(stem##_a, op, "ja")

CONDS(dec64r, "dec %1")
CONDS(inc64r, "inc %1")
CONDS(dec32r, "dec %k1")
CONDS(inc32r, "inc %k1")
CONDS(dec64m, "decq %2")
CONDS(inc64m, "incq %2")
CONDS(dec32m, "decl %2")
CONDS(inc32m, "incl %2")

typedef int (*cond_fn)(uint64_t, int);
#define ROW(stem) {#stem, {stem##_e, stem##_ne, stem##_s, stem##_ns, stem##_l, stem##_ge, stem##_le, \
                           stem##_g, stem##_o, stem##_no, stem##_b, stem##_ae, stem##_be, stem##_a}}

struct obj { long pad[7]; long refcnt; };

__attribute__((noinline)) static long release(struct obj *o)
{
    long freed = 0;
    __asm__ volatile("decq 0x38(%1)\n\t" "jne 1f\n\t" "mov $1, %0\n\t" "1:\n\t"
                     : "+r"(freed) : "r"(o) : "cc", "memory");
    return freed;
}

int main(void)
{
    static const struct { const char *name; cond_fn f[14]; } rows[] = {
        ROW(dec64r), ROW(inc64r), ROW(dec32r), ROW(inc32r),
        ROW(dec64m), ROW(inc64m), ROW(dec32m), ROW(inc32m),
    };
    static const uint64_t vals[] = {
        0, 1, 2, 0xffffffffffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull,
        0x8000000000000001ull, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0x100000000ull,
    };
    for (unsigned r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        printf("%s", rows[r].name);
        for (unsigned v = 0; v < sizeof vals / sizeof vals[0]; v++) {
            unsigned bits = 0;
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < 14; k++)
                    bits = bits * 2 + (unsigned)rows[r].f[k](vals[v], c);
            printf(" %07x", bits);
        }
        printf("\n");
    }
    struct obj o = {{0}, 0};
    long freed = 0;
    for (long i = 0; i < 1000000; i++) {
        o.refcnt = 1 + (i & 1);
        freed += release(&o);
    }
    printf("released %ld\n", freed);
    return 0;
}
