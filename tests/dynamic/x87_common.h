/* Shared helpers for the x87_*.c tests.  Rosetta 2 is the oracle: the .out
 * files next to each test are Rosetta's stdout.  Values are chosen so that the
 * printed result does not depend on 64- vs 80-bit internal precision. */
#ifndef X87_COMMON_H
#define X87_COMMON_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define X87CLOB "memory", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"

static inline uint64_t d2b(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline double b2d(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

static inline unsigned x87_sw(void)
{
    uint16_t s;
    __asm__ volatile("fnstsw %0" : "=m"(s));
    return s;
}
static inline unsigned x87_cw(void)
{
    uint16_t s;
    __asm__ volatile("fnstcw %0" : "=m"(s));
    return s;
}
static inline unsigned x87_top(void) { return (x87_sw() >> 11) & 7; }
static inline void x87_init(void) { __asm__ volatile("fninit" ::: X87CLOB); }
static inline void x87_setcw(uint16_t cw) { __asm__ volatile("fldcw %0" ::"m"(cw) : "memory"); }

/* Print the TOP field at the end of a group; every group must leave it 0. */
static inline void x87_group_end(const char *name)
{
    printf("group %s end top=%u\n", name, x87_top());
}
#endif
