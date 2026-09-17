/*
 * The signature-driven crossing, checked against the two ABIs rather than
 * against itself.
 *
 * Every argument here is written into the guest CPU by a second implementation
 * of the System V AMD64 convention that lives in this file: integers into rdi,
 * rsi, rdx, rcx, r8, r9 and then into eight-byte stack slots above the return
 * address, floating-point into xmm0 to xmm7 and then into those same slots,
 * the two sequences advancing independently.  Asking ocerz_abi_read_guest
 * where an argument goes and then putting it there would make the test agree
 * with the engine whatever the engine did, so the placement is derived from
 * the signature by code that has never seen the engine's, and the callee on
 * the far side is a real C function of exactly that prototype.  What is
 * compared is therefore the engine against clang's idea of both conventions.
 *
 * That is also why several signatures spill on one side and not on the other,
 * and why those are here deliberately.  x86-64 has six integer argument
 * registers and arm64 has eight, so L(ddddLLLLLLLL) leaves two arguments on
 * the guest stack and none on the host's; Apple's arm64 packs a stack argument
 * at its natural size while System V rounds every one up to eight bytes, so
 * l(LLLLLLLLiiL) hands the host two 32-bit arguments sharing one eight-byte
 * word.  An engine that copied the guest's spilled words straight through
 * would pass every signature whose two walks happen to agree and fail exactly
 * these.  The narrow classes push the same point further: l(iiiiiiiibhBHi)
 * leaves b, h, B, H and i one eightbyte each on the guest stack and packs them
 * into twelve bytes on the host's, and h(bhBHbhBHbhBHbhBH) and
 * L(LLLLLLLLbiBlhL) do the same with every narrow width and with gaps that
 * only alignment explains.  A callee reading its own stack never looks at the
 * bytes between its arguments, so the host frame is also checked byte for
 * byte, straight out of ocerz_abi_read_guest, against the layout clang was
 * seen to use, with every byte between the arguments required to be zero.
 *
 * Where an ABI leaves bits undefined this test writes nonsense into them and
 * expects the crossing to ignore it.  The excess bits of an INTEGER value
 * narrower than its register are unspecified, so a 32-bit argument is placed
 * with a distinct garbage word in the upper half of its register or stack slot
 * and a 32-bit result is compared only over eax.  An 8- or 16-bit argument gets
 * garbage in every bit above its width, and is expected to arrive extended from
 * its own low bits to 64, by sign for b and h and by zero for B and H.  That is
 * observable from inside a real callee because clang's arm64 callee trusts its
 * caller to have extended a char or short to 32 bits and turns it into a 64-bit
 * value with a bare sxtw, so a crossing that left garbage between bit 8 and bit
 * 31 is recorded as garbage.  A narrow result is compared over all of rax,
 * since the crossing extends it to 64, and 0x80 must come back as -128 for b
 * and 128 for B.  The upper half of an xmm holding a double, and the upper half
 * of an eight-byte slot holding a float, are filled the same way, because a
 * guest reaches such a slot with movss and leaves whatever was there.  For the
 * same reason a void result asserts nothing whatever about rax: the ABI does
 * not define it, and demanding a value would be demanding an invention.
 *
 * Floating-point arguments are bit patterns rather than numbers, so one pass
 * over the floating-point signatures uses quiet and signalling NaNs, both
 * infinities, negative zero, the smallest denormal and the largest finite.
 * Those are the values a crossing that converts through a C double instead of
 * copying bits would quietly change.
 *
 * The rounding-mode checks are the reason the engine touches FPCR at all.  The
 * guest mxcsr is driven into the host through ocerz_apply_mxcsr_round exactly
 * as the JIT drives it, and then three things are asserted: the callee runs in
 * the default mode, FPCR comes back bit-identical, and cpu->mxcsr is untouched.
 * FPCR is read with mrs rather than through fegetround so that the middle one
 * is bit-identical and not merely equal in the rounding field.
 *
 * Some narrow checks need a callee that does something other than record.  A
 * host function returning its own argument shows the result extension and the
 * sign on its own: 0xdeadbeefcafe12ff in rdi comes back as 0xff for B(B) and as
 * all ones for b(b).  Checksums over narrow registers and over narrow stack
 * slots, each full of garbage above the low bits, show the argument side in
 * arithmetic the callee does itself.  A host function declared to return a
 * full word, 0xdeadbeefcafe8080, called under b(), B(), h() and H(), shows that
 * the crossing reads only the low bits of x0 rather than trusting the callee to
 * have extended them.  Every scalar class is also parsed as both a result and
 * an argument, and malformed notations built from the narrow letters are
 * refused alongside the rest.
 *
 * Structures passed and returned by value are checked the same way, with the
 * callees and their table generated because there are 141 of them.  Every
 * structure is a real C type, and its size, alignment and member offsets come
 * from sizeof, _Alignof and offsetof, so the layout ocerz_abi_parse produces for
 * each notation is compared with clang's and not with arithmetic done here.
 * Each shape runs as S(S) and as S(LSdS), a result and two arguments separated
 * by an integer and a double, once with ordinary values and once with the
 * floating-point edge values and null pointers.  The shapes cover every class a
 * structure can take on either side: one integer, pointer, float or double
 * member, {LL}, {iL} and {Li}, aggregates of one to five floats and of one to
 * five doubles, the mixed {df}, {dL}, {Ld}, {fd} and {bf}, {fi} whose float and
 * int share one INTEGER eightbyte, {{dd}{dd}}, which is four v registers on
 * arm64 and MEMORY on x86, {LLL} and {pdi}, which are MEMORY on x86 and a copy
 * on arm64, {bBhHiu}, padding inside a structure and at its end, nesting eight
 * deep, and sixteen members both as {dddddddddddddddd} and padded out to 128
 * bytes.  The guest side is written by this file's own System V: a structure
 * over sixteen bytes goes to the stack, a smaller one is classified eightbyte
 * by eightbyte and goes to registers only if every eightbyte fits, the bytes
 * past its end in its last eightbyte are garbage, and a MEMORY result gets a
 * pointer in rdi to space full of garbage.  The callee records every member and
 * returns a structure whose members all differ, which is compared member by
 * member where System V puts it: rax and rdx, xmm0 and xmm1 with their upper
 * halves zero, or the result space, which must hold nothing past the structure
 * and whose address rax must hold.
 *
 * Spills have signatures of their own, chosen where the two ABIs part company.
 * L(LLLLL{LL}L) stacks the structure on x86 but not on arm64; L(LLLLLLL{LL}L)
 * stacks it on arm64 and closes x7; d(ddddddd{dd}d) and f(dddddd{fff}f) do the
 * same to the v registers; the eight-long and eight-double runs from ocerz/abi.h's
 * probes put small structures and aggregates between scalars on Apple's stack;
 * and L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f) exhausts both kinds of arm64
 * register in six arguments before five more follow.  The Objective-C shapes
 * appear as the methods that pass them: {{dd}{dd}}(pp{{dd}{dd}}p),
 * {LL}(pppL{LL}), {dd}(pp{dd}p), v(pp{{dd}{dd}}), {dddddd}(pp) and {dddd}(pp).
 * For twenty signatures the host frame is also read straight out of
 * ocerz_abi_read_guest: each x and v word, each stacked byte at the offset clang
 * was seen to use with zero everywhere else, x8 pointing at the OcerzAbiCall's
 * own result buffer exactly when the result comes back through it, and every
 * structure passed by copy pointing inside that OcerzAbiCall rather than at the
 * guest's stack, since an arm64 callee may write to its copy.  Malformed
 * structure notations - empty braces, braces never closed or never opened, v or
 * c inside, seventeen members, nine levels - are refused at parse time, and
 * hand-built layouts no notation produces - no members, overlapping or
 * misaligned ones, a member past the end, an eightbyte no member starts in, the
 * wrong alignment, more than 256 bytes, more than sixteen members - are refused
 * before any call is made.
 *
 * The map is the identity one, as in test_bridge.c, because that is the map
 * native mode runs in; under it a guest pointer and a host pointer are the
 * same number, so a pointer argument and a pointer result can be checked for
 * the right value rather than for being non-zero.
 */
#include "ocerz/abi.h"
#include "ocerz/cpu.h"
#include "ocerz/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA       (4ull << 30)
#define SCRATCH     0x10000ull
#define TRAP_RIP    0x00000000aabbcc00ull
#define RET_ADDR    0x0000000044332200ull
#define RAX_POISON  0xfeedfacecafebeefull
#define XMM_LO_POISON 0xdeadbeef0badf00dull
#define XMM_HI_POISON 0x0f0f0f0fa5a5a5a5ull
#define MXCSR_DEFAULT 0x1f80u

#define RES_I  ((int32_t)0xc0ffee11)
#define RES_U  ((uint32_t)0xdec0de22u)
#define RES_L  ((int64_t)0x51de0033f00dba11ll)
#define RES_LU ((uint64_t)0xa11ce044badf00d5ull)
#define RES_I8  ((int8_t)-128)
#define RES_U8  ((uint8_t)0x80)
#define RES_I16 ((int16_t)-32768)
#define RES_U16 ((uint16_t)0x8000)

static const uint32_t kResFBits = 0x4caffee1u;
static const uint64_t kResDBits = 0x41deface0badf00dull;

static int checks;
static int failures;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

#define REC_MAX 96

static uint64_t g_rec[REC_MAX];
static int g_nrec;
static int g_entered;
static uint64_t g_fpcr_in;
static uint64_t g_deref;
static uint64_t g_ptr_base;
static uint64_t g_stack_base;
static uint32_t g_mxcsr = MXCSR_DEFAULT;
static void *g_res_ptr;
static OcerzCPU g_cpu;

static uint64_t rd_fpcr(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}

static void enter(void)
{
    g_entered++;
    g_fpcr_in = rd_fpcr();
    g_nrec = 0;
}

static void rec(uint64_t v)
{
    if (g_nrec < REC_MAX)
        g_rec[g_nrec] = v;
    g_nrec++;
}

static void rec_f(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    rec(b);
}

static void rec_d(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    rec(b);
}

static void rec_s8(int8_t v)
{
    rec((uint64_t)(int64_t)v);
}

static void rec_u8(uint8_t v)
{
    rec((uint64_t)v);
}

static void rec_s16(int16_t v)
{
    rec((uint64_t)(int64_t)v);
}

static void rec_u16(uint16_t v)
{
    rec((uint64_t)v);
}

static float res_f(void)
{
    float f;
    memcpy(&f, &kResFBits, sizeof f);
    return f;
}

static double res_d(void)
{
    double d;
    memcpy(&d, &kResDBits, sizeof d);
    return d;
}

static uint64_t fn_L0(void)
{
    enter();
    return RES_LU;
}

static uint64_t fn_L1(uint64_t a0)
{
    enter();
    rec(a0);
    return RES_LU;
}

static uint64_t fn_L2(uint64_t a0, uint64_t a1)
{
    enter();
    rec(a0); rec(a1);
    return RES_LU;
}

static uint64_t fn_L3(uint64_t a0, uint64_t a1, uint64_t a2)
{
    enter();
    rec(a0); rec(a1); rec(a2);
    return RES_LU;
}

static uint64_t fn_L4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3);
    return RES_LU;
}

static uint64_t fn_L5(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4);
    return RES_LU;
}

static uint64_t fn_L6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5);
    return RES_LU;
}

static uint64_t fn_L7(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6);
    return RES_LU;
}

static uint64_t fn_L8(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    return RES_LU;
}

static uint64_t fn_L9(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                      uint64_t a8)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec(a8);
    return RES_LU;
}

static uint64_t fn_L10(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                       uint64_t a8, uint64_t a9)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec(a8); rec(a9);
    return RES_LU;
}

static uint64_t fn_L11(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                       uint64_t a8, uint64_t a9, uint64_t a10)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec(a8); rec(a9); rec(a10);
    return RES_LU;
}

static uint64_t fn_L12(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                       uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec(a8); rec(a9); rec(a10); rec(a11);
    return RES_LU;
}

static double fn_D0(void)
{
    enter();
    return res_d();
}

static double fn_D1(double a0)
{
    enter();
    rec_d(a0);
    return res_d();
}

static double fn_D2(double a0, double a1)
{
    enter();
    rec_d(a0); rec_d(a1);
    return res_d();
}

static double fn_D3(double a0, double a1, double a2)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2);
    return res_d();
}

static double fn_D4(double a0, double a1, double a2, double a3)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3);
    return res_d();
}

static double fn_D5(double a0, double a1, double a2, double a3, double a4)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4);
    return res_d();
}

static double fn_D6(double a0, double a1, double a2, double a3, double a4,
                    double a5)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    return res_d();
}

static double fn_D7(double a0, double a1, double a2, double a3, double a4,
                    double a5, double a6)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6);
    return res_d();
}

static double fn_D8(double a0, double a1, double a2, double a3, double a4,
                    double a5, double a6, double a7)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7);
    return res_d();
}

static double fn_D9(double a0, double a1, double a2, double a3, double a4,
                    double a5, double a6, double a7, double a8)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7); rec_d(a8);
    return res_d();
}

static double fn_D10(double a0, double a1, double a2, double a3, double a4,
                     double a5, double a6, double a7, double a8, double a9)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7); rec_d(a8); rec_d(a9);
    return res_d();
}

static int64_t fn_l8(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5, int64_t a6, int64_t a7)
{
    enter();
    rec((uint64_t)a0); rec((uint64_t)a1); rec((uint64_t)a2); rec((uint64_t)a3);
    rec((uint64_t)a4); rec((uint64_t)a5); rec((uint64_t)a6); rec((uint64_t)a7);
    return RES_L;
}

static int32_t fn_i12(int32_t a0, int32_t a1, int32_t a2, int32_t a3,
                      int32_t a4, int32_t a5, int32_t a6, int32_t a7,
                      int32_t a8, int32_t a9, int32_t a10, int32_t a11)
{
    enter();
    rec((uint32_t)a0); rec((uint32_t)a1); rec((uint32_t)a2); rec((uint32_t)a3);
    rec((uint32_t)a4); rec((uint32_t)a5); rec((uint32_t)a6); rec((uint32_t)a7);
    rec((uint32_t)a8); rec((uint32_t)a9); rec((uint32_t)a10);
    rec((uint32_t)a11);
    return RES_I;
}

static uint32_t fn_u8(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                      uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    return RES_U;
}

static int64_t fn_mixw(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                       int32_t a8, int32_t a9, uint64_t a10)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec((uint32_t)a8); rec((uint32_t)a9); rec(a10);
    return RES_L;
}

static float fn_F8(float a0, float a1, float a2, float a3, float a4, float a5,
                   float a6, float a7)
{
    enter();
    rec_f(a0); rec_f(a1); rec_f(a2); rec_f(a3); rec_f(a4); rec_f(a5);
    rec_f(a6); rec_f(a7);
    return res_f();
}

static float fn_F12(float a0, float a1, float a2, float a3, float a4, float a5,
                    float a6, float a7, float a8, float a9, float a10,
                    float a11)
{
    enter();
    rec_f(a0); rec_f(a1); rec_f(a2); rec_f(a3); rec_f(a4); rec_f(a5);
    rec_f(a6); rec_f(a7); rec_f(a8); rec_f(a9); rec_f(a10); rec_f(a11);
    return res_f();
}

static float fn_FD12(float a0, double a1, float a2, double a3, float a4,
                     double a5, float a6, double a7, float a8, double a9,
                     float a10, double a11)
{
    enter();
    rec_f(a0); rec_d(a1); rec_f(a2); rec_d(a3); rec_f(a4); rec_d(a5);
    rec_f(a6); rec_d(a7); rec_f(a8); rec_d(a9); rec_f(a10); rec_d(a11);
    return res_f();
}

static double fn_F8D2(float a0, float a1, float a2, float a3, float a4,
                      float a5, float a6, float a7, double a8, double a9)
{
    enter();
    rec_f(a0); rec_f(a1); rec_f(a2); rec_f(a3); rec_f(a4); rec_f(a5);
    rec_f(a6); rec_f(a7); rec_d(a8); rec_d(a9);
    return res_d();
}

static uint64_t fn_ALT12(uint64_t a0, double a1, uint64_t a2, double a3,
                         uint64_t a4, double a5, uint64_t a6, double a7,
                         uint64_t a8, double a9, uint64_t a10, double a11)
{
    enter();
    rec(a0); rec_d(a1); rec(a2); rec_d(a3); rec(a4); rec_d(a5);
    rec(a6); rec_d(a7); rec(a8); rec_d(a9); rec(a10); rec_d(a11);
    return RES_LU;
}

static uint64_t fn_I8D2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                        double a8, double a9)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec_d(a8); rec_d(a9);
    return RES_LU;
}

static double fn_I2D10(uint64_t a0, uint64_t a1, double a2, double a3,
                       double a4, double a5, double a6, double a7, double a8,
                       double a9, double a10, double a11)
{
    enter();
    rec(a0); rec(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7); rec_d(a8); rec_d(a9); rec_d(a10); rec_d(a11);
    return res_d();
}

static uint64_t fn_D4I8(double a0, double a1, double a2, double a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                        uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3);
    rec(a4); rec(a5); rec(a6); rec(a7); rec(a8); rec(a9); rec(a10); rec(a11);
    return RES_LU;
}

static uint64_t fn_I8D4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                        double a8, double a9, double a10, double a11)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec_d(a8); rec_d(a9); rec_d(a10); rec_d(a11);
    return RES_LU;
}

static uint64_t fn_D8I8(double a0, double a1, double a2, double a3, double a4,
                        double a5, double a6, double a7, uint64_t a8,
                        uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12,
                        uint64_t a13, uint64_t a14, uint64_t a15)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7);
    rec(a8); rec(a9); rec(a10); rec(a11); rec(a12); rec(a13); rec(a14);
    rec(a15);
    return RES_LU;
}

static int64_t fn_7I9D(uint64_t a0, double a1, uint64_t a2, double a3,
                       uint64_t a4, double a5, uint64_t a6, double a7,
                       uint64_t a8, double a9, uint64_t a10, double a11,
                       uint64_t a12, double a13, double a14, double a15)
{
    enter();
    rec(a0); rec_d(a1); rec(a2); rec_d(a3); rec(a4); rec_d(a5);
    rec(a6); rec_d(a7); rec(a8); rec_d(a9); rec(a10); rec_d(a11);
    rec(a12); rec_d(a13); rec_d(a14); rec_d(a15);
    return RES_L;
}

static double fn_D8IFI(double a0, double a1, double a2, double a3, double a4,
                       double a5, double a6, double a7, int32_t a8, float a9,
                       int32_t a10)
{
    enter();
    rec_d(a0); rec_d(a1); rec_d(a2); rec_d(a3); rec_d(a4); rec_d(a5);
    rec_d(a6); rec_d(a7);
    rec((uint32_t)a8); rec_f(a9); rec((uint32_t)a10);
    return res_d();
}

static void fn_F10I2(float a0, float a1, float a2, float a3, float a4,
                     float a5, float a6, float a7, float a8, float a9,
                     int32_t a10, int32_t a11)
{
    enter();
    rec_f(a0); rec_f(a1); rec_f(a2); rec_f(a3); rec_f(a4); rec_f(a5);
    rec_f(a6); rec_f(a7); rec_f(a8); rec_f(a9);
    rec((uint32_t)a10); rec((uint32_t)a11);
}

static int8_t fn_b1(int8_t a0)
{
    enter();
    rec_s8(a0);
    return RES_I8;
}

static uint8_t fn_B1(uint8_t a0)
{
    enter();
    rec_u8(a0);
    return RES_U8;
}

static int16_t fn_h1(int16_t a0)
{
    enter();
    rec_s16(a0);
    return RES_I16;
}

static uint16_t fn_H1(uint16_t a0)
{
    enter();
    rec_u16(a0);
    return RES_U16;
}

static uint8_t fn_B4(int8_t a0, uint8_t a1, int16_t a2, uint16_t a3)
{
    enter();
    rec_s8(a0); rec_u8(a1); rec_s16(a2); rec_u16(a3);
    return RES_U8;
}

static uint64_t fn_bBhH8(int8_t a0, uint8_t a1, int16_t a2, uint16_t a3,
                         int8_t a4, uint8_t a5, int16_t a6, uint16_t a7)
{
    enter();
    rec_s8(a0); rec_u8(a1); rec_s16(a2); rec_u16(a3);
    rec_s8(a4); rec_u8(a5); rec_s16(a6); rec_u16(a7);
    return RES_LU;
}

static int64_t fn_i8bhBHi(int32_t a0, int32_t a1, int32_t a2, int32_t a3,
                          int32_t a4, int32_t a5, int32_t a6, int32_t a7,
                          int8_t a8, int16_t a9, uint8_t a10, uint16_t a11,
                          int32_t a12)
{
    enter();
    rec((uint32_t)a0); rec((uint32_t)a1); rec((uint32_t)a2); rec((uint32_t)a3);
    rec((uint32_t)a4); rec((uint32_t)a5); rec((uint32_t)a6); rec((uint32_t)a7);
    rec_s8(a8); rec_s16(a9); rec_u8(a10); rec_u16(a11); rec((uint32_t)a12);
    return RES_L;
}

static int16_t fn_n16(int8_t a0, int16_t a1, uint8_t a2, uint16_t a3,
                      int8_t a4, int16_t a5, uint8_t a6, uint16_t a7,
                      int8_t a8, int16_t a9, uint8_t a10, uint16_t a11,
                      int8_t a12, int16_t a13, uint8_t a14, uint16_t a15)
{
    enter();
    rec_s8(a0); rec_s16(a1); rec_u8(a2); rec_u16(a3);
    rec_s8(a4); rec_s16(a5); rec_u8(a6); rec_u16(a7);
    rec_s8(a8); rec_s16(a9); rec_u8(a10); rec_u16(a11);
    rec_s8(a12); rec_s16(a13); rec_u8(a14); rec_u16(a15);
    return RES_I16;
}

static uint64_t fn_align(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                         int8_t a8, int32_t a9, uint8_t a10, int64_t a11,
                         int16_t a12, uint64_t a13)
{
    enter();
    rec(a0); rec(a1); rec(a2); rec(a3); rec(a4); rec(a5); rec(a6); rec(a7);
    rec_s8(a8); rec((uint32_t)a9); rec_u8(a10); rec((uint64_t)a11);
    rec_s16(a12); rec(a13);
    return RES_LU;
}

static double fn_nfp(int8_t a0, double a1, uint8_t a2, double a3, int16_t a4,
                     float a5, uint16_t a6, float a7)
{
    enter();
    rec_s8(a0); rec_d(a1); rec_u8(a2); rec_d(a3); rec_s16(a4); rec_f(a5);
    rec_u16(a6); rec_f(a7);
    return res_d();
}

static uint16_t fn_F9h(float a0, float a1, float a2, float a3, float a4,
                       float a5, float a6, float a7, float a8, int16_t a9,
                       uint8_t a10, int8_t a11, uint16_t a12, int64_t a13,
                       uint8_t a14, int16_t a15)
{
    enter();
    rec_f(a0); rec_f(a1); rec_f(a2); rec_f(a3); rec_f(a4); rec_f(a5);
    rec_f(a6); rec_f(a7); rec_f(a8);
    rec_s16(a9); rec_u8(a10); rec_s8(a11); rec_u16(a12); rec((uint64_t)a13);
    rec_u8(a14); rec_s16(a15);
    return RES_U16;
}

static void fn_V(void *a0, uint64_t a1, double a2)
{
    enter();
    rec((uint64_t)(uintptr_t)a0); rec(a1); rec_d(a2);
}

static void fn_V0(void)
{
    enter();
}

static int32_t fn_I(const void *a0, const void *a1)
{
    enter();
    rec((uint64_t)(uintptr_t)a0); rec((uint64_t)(uintptr_t)a1);
    return RES_I;
}

static uint32_t fn_U(uint64_t a0)
{
    enter();
    rec(a0);
    return RES_U;
}

static void *fn_P(void *a0, uint64_t a1, uint64_t a2)
{
    enter();
    rec((uint64_t)(uintptr_t)a0); rec(a1); rec(a2);
    return g_res_ptr;
}

static void *fn_PNULL(void *a0)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    return NULL;
}

static void *fn_P8(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5,
                   void *a6, void *a7)
{
    enter();
    rec((uint64_t)(uintptr_t)a0); rec((uint64_t)(uintptr_t)a1);
    rec((uint64_t)(uintptr_t)a2); rec((uint64_t)(uintptr_t)a3);
    rec((uint64_t)(uintptr_t)a4); rec((uint64_t)(uintptr_t)a5);
    rec((uint64_t)(uintptr_t)a6); rec((uint64_t)(uintptr_t)a7);
    return g_res_ptr;
}

static uint64_t fn_DEREF(const void *a0)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    memcpy(&g_deref, a0, sizeof g_deref);
    return RES_LU;
}

#define FN(f) ((void (*)(void))(f))

typedef struct AbiCase {
    const char *sig;
    void (*fn)(void);
    int edge;
    int nullret;
} AbiCase;

static const int kIntReg[6] = {
    OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
};

static const uint32_t kEdgeF[8] = {
    0x7fc0dead, 0x7f800001, 0xff800000, 0x80000000,
    0x00000001, 0x7f7fffff, 0x7f800000, 0xc2f6e979,
};

static const uint64_t kEdgeD[8] = {
    0x7ff8000012345678ull, 0x7ff0000000000001ull, 0xfff0000000000000ull,
    0x8000000000000000ull, 0x0000000000000001ull, 0x7fefffffffffffffull,
    0x7ff0000000000000ull, 0xc0f86a3210fedcbaull,
};

static const void *fnptr(void (*f)(void))
{
    return (const void *)(uintptr_t)f;
}

static int is_fp(char c)
{
    return c == 'f' || c == 'd';
}

static uint32_t arg_bits32(int i)
{
    return 0xc0de0100u + (uint32_t)i * 0x11u;
}

static uint64_t arg_bits64(int i)
{
    return 0x5eed0000face0000ull | ((uint64_t)(i + 1) << 40) |
           ((uint64_t)(i + 1) << 8);
}

static uint32_t arg_fbits(int i)
{
    return 0x4caf0100u + (uint32_t)i * 0x11u;
}

static uint64_t arg_dbits(int i)
{
    return 0x41d0face00000100ull + (uint64_t)i * 0x11u;
}

static uint64_t garbage32(int i)
{
    return 0xbad0f00dull ^ ((uint64_t)(i + 1) * 0x01010101ull);
}

static uint64_t garbage64(int i)
{
    return 0xc0ffee00badc0de0ull + (uint64_t)i;
}

static const uint8_t kEdge8[8] = {
    0x80, 0xff, 0x7f, 0x00, 0x01, 0xfe, 0x81, 0x40,
};

static const uint16_t kEdge16[8] = {
    0x8000, 0xffff, 0x7fff, 0x0000, 0x00ff, 0xff80, 0x8001, 0x0080,
};

static uint8_t arg_bits8(int i)
{
    return (i & 1) ? (uint8_t)(0x7f - i) : (uint8_t)(0x80 + 3 * i);
}

static uint16_t arg_bits16(int i)
{
    return (i & 1) ? (uint16_t)(0x7fff - 0x0101 * i)
                   : (uint16_t)(0x8000 + 0x0203 * i);
}

static uint64_t garbage_above(int i, unsigned bits)
{
    uint64_t g = 0xdeadbeefcafe1234ull ^ ((uint64_t)(i + 1) * 0x0101010101010101ull);
    return (g >> bits) << bits;
}

static uint64_t extend(char c, uint64_t v)
{
    switch (c) {
    case 'b': return (uint64_t)(int64_t)(int8_t)v;
    case 'B': return (uint8_t)v;
    case 'h': return (uint64_t)(int64_t)(int16_t)v;
    case 'H': return (uint16_t)v;
    default:  return v;
    }
}

static int sig_classes(const char *s, char *out)
{
    const char *p;
    int n = 0;

    if (!s[0] || s[1] != '(')
        return -1;
    for (p = s + 2; *p && *p != ')'; p++) {
        if (n >= OCERZ_ABI_MAX_ARGS)
            return -1;
        out[n++] = *p;
    }
    if (*p != ')' || p[1] != 0)
        return -1;
    out[n] = 0;
    return n;
}

static int g_nslot;
static uint64_t g_slot[OCERZ_ABI_MAX_STACK];

static uint64_t setup_call(OcerzCPU *cpu, const char *cls, int n, int edge,
                           uint64_t *want)
{
    int ni = 0, nf = 0, nn = 0, i;
    uint64_t sp;

    ocerz_cpu_reset(cpu);
    cpu->mxcsr = g_mxcsr;
    cpu->gpr[OCERZ_RAX] = RAX_POISON;
    for (i = 0; i < 16; i++) {
        cpu->xmm[i].lo = XMM_LO_POISON + (uint64_t)i;
        cpu->xmm[i].hi = XMM_HI_POISON + (uint64_t)i;
    }

    g_nslot = 0;
    for (i = 0; i < n; i++) {
        char c = cls[i];
        uint64_t raw;

        if (c == 'f') {
            uint32_t b = edge ? kEdgeF[nf & 7] : arg_fbits(i);
            want[i] = b;
            raw = (garbage32(i) << 32) | b;
        } else if (c == 'd') {
            uint64_t b = edge ? kEdgeD[nf & 7] : arg_dbits(i);
            want[i] = b;
            raw = b;
        } else if (c == 'i' || c == 'u') {
            uint32_t b = arg_bits32(i);
            want[i] = b;
            raw = (garbage32(i) << 32) | b;
        } else if (c == 'b' || c == 'B') {
            uint8_t b = edge ? kEdge8[nn++ & 7] : arg_bits8(i);
            want[i] = extend(c, b);
            raw = garbage_above(i, 8) | b;
        } else if (c == 'h' || c == 'H') {
            uint16_t b = edge ? kEdge16[nn++ & 7] : arg_bits16(i);
            want[i] = extend(c, b);
            raw = garbage_above(i, 16) | b;
        } else if (c == 'p') {
            uint64_t b = (edge && (i & 1) == 0)
                             ? 0
                             : g_ptr_base + 0x40ull * (uint64_t)(i + 1);
            want[i] = b;
            raw = b;
        } else {
            uint64_t b = arg_bits64(i);
            want[i] = b;
            raw = b;
        }

        if (is_fp(c)) {
            if (nf < 8) {
                cpu->xmm[nf].lo = raw;
                cpu->xmm[nf].hi = garbage64(i);
                nf++;
            } else {
                g_slot[g_nslot++] = raw;
            }
        } else {
            if (ni < 6) {
                cpu->gpr[kIntReg[ni]] = raw;
                ni++;
            } else {
                g_slot[g_nslot++] = raw;
            }
        }
    }

    sp = g_stack_base - 8;
    ocerz_st(sp, 8, RET_ADDR);
    for (i = 0; i < g_nslot; i++)
        ocerz_st(sp + 8 + 8ull * (uint64_t)i, 8, g_slot[i]);
    for (i = g_nslot; i < g_nslot + 4; i++)
        ocerz_st(sp + 8 + 8ull * (uint64_t)i, 8, 0xfeedc0de00000000ull +
                 (uint64_t)i);

    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = TRAP_RIP;
    return sp;
}

static void check_result(const AbiCase *c, const OcerzCPU *cpu)
{
    uint64_t rax = cpu->gpr[OCERZ_RAX];
    uint64_t xmm = cpu->xmm[0].lo;

    switch (c->sig[0]) {
    case 'v':
        break;
    case 'i':
        CHECK((uint32_t)rax == (uint32_t)RES_I,
              "%s: eax is %#x after the crossing, want %#x", c->sig,
              (unsigned)(uint32_t)rax, (unsigned)(uint32_t)RES_I);
        break;
    case 'u':
        CHECK((uint32_t)rax == RES_U,
              "%s: eax is %#x after the crossing, want %#x", c->sig,
              (unsigned)(uint32_t)rax, (unsigned)RES_U);
        break;
    case 'b':
        CHECK(rax == (uint64_t)(int64_t)RES_I8,
              "%s: rax is %#llx after the crossing, want the int8_t %d "
              "sign-extended, %#llx", c->sig, (unsigned long long)rax, RES_I8,
              (unsigned long long)(int64_t)RES_I8);
        break;
    case 'B':
        CHECK(rax == (uint64_t)RES_U8,
              "%s: rax is %#llx after the crossing, want the uint8_t %u "
              "zero-extended", c->sig, (unsigned long long)rax, RES_U8);
        break;
    case 'h':
        CHECK(rax == (uint64_t)(int64_t)RES_I16,
              "%s: rax is %#llx after the crossing, want the int16_t %d "
              "sign-extended, %#llx", c->sig, (unsigned long long)rax, RES_I16,
              (unsigned long long)(int64_t)RES_I16);
        break;
    case 'H':
        CHECK(rax == (uint64_t)RES_U16,
              "%s: rax is %#llx after the crossing, want the uint16_t %u "
              "zero-extended", c->sig, (unsigned long long)rax, RES_U16);
        break;
    case 'l':
        CHECK(rax == (uint64_t)RES_L,
              "%s: rax is %#llx after the crossing, want %#llx", c->sig,
              (unsigned long long)rax, (unsigned long long)RES_L);
        break;
    case 'L':
        CHECK(rax == RES_LU,
              "%s: rax is %#llx after the crossing, want %#llx", c->sig,
              (unsigned long long)rax, (unsigned long long)RES_LU);
        break;
    case 'p': {
        uint64_t want = c->nullret ? 0 : (uint64_t)(uintptr_t)g_res_ptr;
        CHECK(rax == want,
              "%s: rax is %#llx after the crossing, want the returned pointer "
              "%#llx", c->sig, (unsigned long long)rax,
              (unsigned long long)want);
        break;
    }
    case 'f':
        CHECK((uint32_t)xmm == kResFBits,
              "%s: xmm0 holds %#x after the crossing, want the float %#x",
              c->sig, (unsigned)(uint32_t)xmm, (unsigned)kResFBits);
        break;
    case 'd':
        CHECK(xmm == kResDBits,
              "%s: xmm0 holds %#llx after the crossing, want the double %#llx",
              c->sig, (unsigned long long)xmm,
              (unsigned long long)kResDBits);
        break;
    default:
        CHECK(0, "%s: the test does not know result class '%c'", c->sig,
              c->sig[0]);
        break;
    }
}

static void run_case(const AbiCase *c)
{
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiSig sig;
    char cls[OCERZ_ABI_MAX_ARGS + 1];
    uint64_t want[OCERZ_ABI_MAX_ARGS];
    uint64_t fpcr_before, sp;
    int n, r, i;

    n = sig_classes(c->sig, cls);
    if (n < 0) {
        CHECK(0, "%s: the test's own table entry is not a signature", c->sig);
        return;
    }

    memset(&sig, 0, sizeof sig);
    r = ocerz_abi_parse(c->sig, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK",
          c->sig, r);
    if (r != OCERZ_OK)
        return;
    CHECK(sig.ret == c->sig[0], "%s: parsed result class '%c', want '%c'",
          c->sig, sig.ret ? sig.ret : '?', c->sig[0]);
    CHECK(sig.nargs == n, "%s: parsed %d arguments, want %d", c->sig,
          sig.nargs, n);
    if (sig.nargs != n)
        return;
    for (i = 0; i < n; i++)
        CHECK(sig.arg[i] == cls[i],
              "%s: parsed argument %d as class '%c', want '%c'", c->sig, i,
              sig.arg[i] ? sig.arg[i] : '?', cls[i]);

    memset(g_rec, 0, sizeof g_rec);
    g_nrec = -1;
    g_entered = 0;
    g_deref = 0;
    g_fpcr_in = ~0ull;

    sp = setup_call(cpu, cls, n, c->edge, want);
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    fpcr_before = rd_fpcr();

    r = ocerz_abi_perform(&sig, fnptr(c->fn), cpu);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_perform returned %d, want OCERZ_OK",
          c->sig, r);

    CHECK(g_entered == 1, "%s: the host function ran %d times, want once",
          c->sig, g_entered);
    if (g_entered != 1)
        return;

    CHECK(g_nrec == n, "%s: the host function saw %d arguments, want %d",
          c->sig, g_nrec, n);
    for (i = 0; i < n && i < g_nrec && i < REC_MAX; i++)
        CHECK(g_rec[i] == want[i],
              "%s: argument %d (class '%c') arrived as %#llx, want %#llx",
              c->sig, i, cls[i], (unsigned long long)g_rec[i],
              (unsigned long long)want[i]);

    CHECK(rd_fpcr() == fpcr_before,
          "%s: fpcr is %#llx after the crossing, want the guest's %#llx",
          c->sig, (unsigned long long)rd_fpcr(),
          (unsigned long long)fpcr_before);
    CHECK(((g_fpcr_in >> 22) & 3) == 0,
          "%s: the host function ran with fpcr rounding mode %llu, want the "
          "default 0", c->sig, (unsigned long long)((g_fpcr_in >> 22) & 3));
    CHECK(cpu->mxcsr == g_mxcsr,
          "%s: mxcsr is %#x after the crossing, want the guest's %#x", c->sig,
          (unsigned)cpu->mxcsr, (unsigned)g_mxcsr);

    check_result(c, cpu);

    CHECK(cpu->rip == RET_ADDR,
          "%s: rip is %#llx after the crossing, want the return address %#llx",
          c->sig, (unsigned long long)cpu->rip, (unsigned long long)RET_ADDR);
    CHECK(cpu->gpr[OCERZ_RSP] == sp + 8,
          "%s: rsp is %#llx after the crossing, want %#llx (the call's return "
          "address popped)", c->sig, (unsigned long long)cpu->gpr[OCERZ_RSP],
          (unsigned long long)(sp + 8));

    for (i = 0; i < g_nslot; i++) {
        uint64_t got = ocerz_ld(sp + 8 + 8ull * (uint64_t)i, 8);
        CHECK(got == g_slot[i],
              "%s: the crossing changed the guest stack slot %d from %#llx to "
              "%#llx", c->sig, i, (unsigned long long)g_slot[i],
              (unsigned long long)got);
    }
}

static const AbiCase kCases[] = {
    { "L()", FN(fn_L0), 0, 0 },
    { "L(L)", FN(fn_L1), 0, 0 },
    { "L(LL)", FN(fn_L2), 0, 0 },
    { "L(LLL)", FN(fn_L3), 0, 0 },
    { "L(LLLL)", FN(fn_L4), 0, 0 },
    { "L(LLLLL)", FN(fn_L5), 0, 0 },
    { "L(LLLLLL)", FN(fn_L6), 0, 0 },
    { "L(LLLLLLL)", FN(fn_L7), 0, 0 },
    { "L(LLLLLLLL)", FN(fn_L8), 0, 0 },
    { "L(LLLLLLLLL)", FN(fn_L9), 0, 0 },
    { "L(LLLLLLLLLL)", FN(fn_L10), 0, 0 },
    { "L(LLLLLLLLLLL)", FN(fn_L11), 0, 0 },
    { "L(LLLLLLLLLLLL)", FN(fn_L12), 0, 0 },

    { "d()", FN(fn_D0), 0, 0 },
    { "d(d)", FN(fn_D1), 0, 0 },
    { "d(dd)", FN(fn_D2), 0, 0 },
    { "d(ddd)", FN(fn_D3), 0, 0 },
    { "d(dddd)", FN(fn_D4), 0, 0 },
    { "d(ddddd)", FN(fn_D5), 0, 0 },
    { "d(dddddd)", FN(fn_D6), 0, 0 },
    { "d(ddddddd)", FN(fn_D7), 0, 0 },
    { "d(dddddddd)", FN(fn_D8), 0, 0 },
    { "d(ddddddddd)", FN(fn_D9), 0, 0 },
    { "d(dddddddddd)", FN(fn_D10), 0, 0 },
    { "d(dddddddd)", FN(fn_D8), 1, 0 },
    { "d(dddddddddd)", FN(fn_D10), 1, 0 },

    { "l(llllllll)", FN(fn_l8), 0, 0 },
    { "i(iiiiiiiiiiii)", FN(fn_i12), 0, 0 },
    { "u(uuuuuuuu)", FN(fn_u8), 0, 0 },
    { "l(LLLLLLLLiiL)", FN(fn_mixw), 0, 0 },

    { "f(ffffffff)", FN(fn_F8), 0, 0 },
    { "f(ffffffff)", FN(fn_F8), 1, 0 },
    { "f(ffffffffffff)", FN(fn_F12), 0, 0 },
    { "f(fdfdfdfdfdfd)", FN(fn_FD12), 0, 0 },
    { "f(fdfdfdfdfdfd)", FN(fn_FD12), 1, 0 },
    { "d(ffffffffdd)", FN(fn_F8D2), 0, 0 },

    { "L(LdLdLdLdLdLd)", FN(fn_ALT12), 0, 0 },
    { "L(LLLLLLLLdd)", FN(fn_I8D2), 0, 0 },
    { "d(LLdddddddddd)", FN(fn_I2D10), 0, 0 },
    { "L(ddddLLLLLLLL)", FN(fn_D4I8), 0, 0 },
    { "L(LLLLLLLLdddd)", FN(fn_I8D4), 0, 0 },
    { "L(ddddddddLLLLLLLL)", FN(fn_D8I8), 0, 0 },
    { "l(LdLdLdLdLdLdLddd)", FN(fn_7I9D), 0, 0 },
    { "l(LdLdLdLdLdLdLddd)", FN(fn_7I9D), 1, 0 },
    { "d(ddddddddifi)", FN(fn_D8IFI), 0, 0 },
    { "v(ffffffffffii)", FN(fn_F10I2), 0, 0 },

    { "b(b)", FN(fn_b1), 0, 0 },
    { "b(b)", FN(fn_b1), 1, 0 },
    { "B(B)", FN(fn_B1), 0, 0 },
    { "B(B)", FN(fn_B1), 1, 0 },
    { "h(h)", FN(fn_h1), 0, 0 },
    { "h(h)", FN(fn_h1), 1, 0 },
    { "H(H)", FN(fn_H1), 0, 0 },
    { "H(H)", FN(fn_H1), 1, 0 },
    { "B(bBhH)", FN(fn_B4), 0, 0 },
    { "B(bBhH)", FN(fn_B4), 1, 0 },
    { "L(bBhHbBhH)", FN(fn_bBhH8), 0, 0 },
    { "L(bBhHbBhH)", FN(fn_bBhH8), 1, 0 },
    { "l(iiiiiiiibhBHi)", FN(fn_i8bhBHi), 0, 0 },
    { "l(iiiiiiiibhBHi)", FN(fn_i8bhBHi), 1, 0 },
    { "h(bhBHbhBHbhBHbhBH)", FN(fn_n16), 0, 0 },
    { "h(bhBHbhBHbhBHbhBH)", FN(fn_n16), 1, 0 },
    { "L(LLLLLLLLbiBlhL)", FN(fn_align), 0, 0 },
    { "L(LLLLLLLLbiBlhL)", FN(fn_align), 1, 0 },
    { "d(bdBdhfHf)", FN(fn_nfp), 0, 0 },
    { "d(bdBdhfHf)", FN(fn_nfp), 1, 0 },
    { "H(fffffffffhBbHlBh)", FN(fn_F9h), 0, 0 },
    { "H(fffffffffhBbHlBh)", FN(fn_F9h), 1, 0 },

    { "v()", FN(fn_V0), 0, 0 },
    { "v(pLd)", FN(fn_V), 0, 0 },
    { "i(pp)", FN(fn_I), 0, 0 },
    { "i(pp)", FN(fn_I), 1, 0 },
    { "u(L)", FN(fn_U), 0, 0 },
    { "p(pLL)", FN(fn_P), 0, 0 },
    { "p(p)", FN(fn_PNULL), 0, 1 },
    { "p(pppppppp)", FN(fn_P8), 0, 0 },
    { "p(pppppppp)", FN(fn_P8), 1, 0 },
};
#define NCASES (sizeof kCases / sizeof kCases[0])

static void check_host_map(const char *notation, const int *xi, int nx,
                           const int *vi, int nv)
{
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiSig sig;
    OcerzAbiCall call;
    char cls[OCERZ_ABI_MAX_ARGS + 1];
    uint64_t want[OCERZ_ABI_MAX_ARGS];
    int n, r, i;

    n = sig_classes(notation, cls);
    if (n < 0) {
        CHECK(0, "%s: the test's own entry is not a signature", notation);
        return;
    }
    memset(&sig, 0, sizeof sig);
    r = ocerz_abi_parse(notation, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK",
          notation, r);
    if (r != OCERZ_OK)
        return;

    setup_call(cpu, cls, n, 0, want);
    memset(&call, 0, sizeof call);
    r = ocerz_abi_read_guest(&sig, cpu, &call);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_read_guest returned %d, want OCERZ_OK",
          notation, r);
    if (r != OCERZ_OK)
        return;

    for (i = 0; i < nx; i++)
        CHECK(call.x[i] == want[xi[i]],
              "%s: x%d holds %#llx, want argument %d's %#llx", notation, i,
              (unsigned long long)call.x[i], xi[i],
              (unsigned long long)want[xi[i]]);
    for (i = 0; i < nv; i++)
        CHECK(call.v[i] == want[vi[i]],
              "%s: v%d holds %#llx, want argument %d's %#llx", notation, i,
              (unsigned long long)call.v[i], vi[i],
              (unsigned long long)want[vi[i]]);
    CHECK(call.nstack == 0,
          "%s: the host side spilled %d arguments, but arm64 has eight "
          "integer and eight floating-point argument registers and this "
          "signature fits in them", notation, call.nstack);
}

static void test_host_registers(void)
{
    static const int xa[6] = { 0, 1, 2, 3, 4, 5 };
    static const int vb[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const int xc[4] = { 0, 2, 4, 6 };
    static const int vc[4] = { 1, 3, 5, 7 };
    static const int xd[8] = { 4, 5, 6, 7, 8, 9, 10, 11 };
    static const int vd[4] = { 0, 1, 2, 3 };

    check_host_map("L(LLLLLL)", xa, 6, NULL, 0);
    check_host_map("d(dddddddd)", NULL, 0, vb, 8);
    check_host_map("L(LdLdLdLd)", xc, 4, vc, 4);
    check_host_map("L(ddddLLLLLLLL)", xd, 8, vd, 4);
    check_host_map("L(bBhHbBhH)", vb, 8, NULL, 0);
    check_host_map("B(dbdBdhdH)", vc, 4, xc, 4);
}

typedef struct StackPlace {
    int arg;
    int at;
    int size;
} StackPlace;

static void check_host_stack(const char *notation, const StackPlace *place,
                             int nplace, int nstack)
{
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiSig sig;
    OcerzAbiCall call;
    char cls[OCERZ_ABI_MAX_ARGS + 1];
    uint64_t want[OCERZ_ABI_MAX_ARGS];
    uint8_t expect[sizeof call.stack];
    const uint8_t *got;
    int n, r, i, k;

    n = sig_classes(notation, cls);
    r = n < 0 ? OCERZ_EFORMAT : ocerz_abi_parse(notation, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK",
          notation, r);
    if (r != OCERZ_OK)
        return;

    for (i = 0; i < 2; i++) {
        setup_call(cpu, cls, n, i, want);
        memset(&call, 0x5a, sizeof call);
        r = ocerz_abi_read_guest(&sig, cpu, &call);
        CHECK(r == OCERZ_OK,
              "%s: ocerz_abi_read_guest returned %d, want OCERZ_OK", notation, r);
        if (r != OCERZ_OK)
            return;

        memset(expect, 0, sizeof expect);
        for (k = 0; k < nplace; k++) {
            uint64_t v = want[place[k].arg];
            memcpy(expect + place[k].at, &v, (size_t)place[k].size);
        }

        CHECK(call.nstack == nstack,
              "%s: the host side spilled %d eightbytes, want %d", notation,
              call.nstack, nstack);
        got = (const uint8_t *)call.stack;
        for (k = 0; k < (int)sizeof expect; k++)
            CHECK(got[k] == expect[k],
                  "%s%s: host stack byte %d is %#x, want %#x (clang packs "
                  "this signature's stacked arguments at their own size and "
                  "alignment)", notation, i ? " with edge values" : "", k,
                  got[k], expect[k]);
    }
}

static void test_host_stack(void)
{
    static const StackPlace kMix[] = {
        { 8, 0, 1 }, { 9, 2, 2 }, { 10, 4, 1 }, { 11, 6, 2 }, { 12, 8, 4 },
    };
    static const StackPlace kAlign[] = {
        { 8, 0, 1 }, { 9, 4, 4 }, { 10, 8, 1 }, { 11, 16, 8 }, { 12, 24, 2 },
        { 13, 32, 8 },
    };
    static const StackPlace kAll[] = {
        { 8, 0, 1 }, { 9, 2, 2 }, { 10, 4, 1 }, { 11, 6, 2 },
        { 12, 8, 1 }, { 13, 10, 2 }, { 14, 12, 1 }, { 15, 14, 2 },
    };
    static const StackPlace kFloat[] = {
        { 8, 0, 4 },
    };

    check_host_stack("l(iiiiiiiibhBHi)", kMix, 5, 2);
    check_host_stack("L(LLLLLLLLbiBlhL)", kAlign, 6, 5);
    check_host_stack("h(bhBHbhBHbhBHbhBH)", kAll, 8, 2);
    check_host_stack("H(fffffffffhBbHlBh)", kFloat, 1, 1);
}

static int8_t echo_b(int8_t a)
{
    enter();
    return a;
}

static uint8_t echo_B(uint8_t a)
{
    enter();
    return a;
}

static int16_t echo_h(int16_t a)
{
    enter();
    return a;
}

static uint16_t echo_H(uint16_t a)
{
    enter();
    return a;
}

static int64_t sum_bhBH(int8_t b, int16_t h, uint8_t B, uint16_t H)
{
    enter();
    return (int64_t)b + 3 * (int64_t)h + 5 * (int64_t)B + 7 * (int64_t)H;
}

static int64_t sum_spill(int32_t a0, int32_t a1, int32_t a2, int32_t a3,
                         int32_t a4, int32_t a5, int32_t a6, int32_t a7,
                         int8_t b, int16_t h, uint8_t B, uint16_t H, int32_t i)
{
    enter();
    return (int64_t)a0 + 2 * (int64_t)a1 + 3 * (int64_t)a2 + 4 * (int64_t)a3 +
           5 * (int64_t)a4 + 6 * (int64_t)a5 + 7 * (int64_t)a6 +
           8 * (int64_t)a7 + 11 * (int64_t)b + 13 * (int64_t)h +
           17 * (int64_t)B + 19 * (int64_t)H + 23 * (int64_t)i;
}

static uint64_t garbage_result(void)
{
    enter();
    return 0xdeadbeefcafe8080ull;
}

static uint64_t narrow_call(const char *notation, void (*fn)(void),
                            const uint64_t *reg, int nreg,
                            const uint64_t *slot, int nslot)
{
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiSig sig;
    uint64_t sp;
    int r, i;

    r = ocerz_abi_parse(notation, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK",
          notation, r);
    if (r != OCERZ_OK)
        return RAX_POISON;

    ocerz_cpu_reset(cpu);
    cpu->mxcsr = MXCSR_DEFAULT;
    cpu->gpr[OCERZ_RAX] = RAX_POISON;
    for (i = 0; i < nreg && i < 6; i++)
        cpu->gpr[kIntReg[i]] = reg[i];
    sp = g_stack_base - 8;
    ocerz_st(sp, 8, RET_ADDR);
    for (i = 0; i < nslot; i++)
        ocerz_st(sp + 8 + 8ull * (uint64_t)i, 8, slot[i]);
    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = TRAP_RIP;

    g_entered = 0;
    r = ocerz_abi_perform(&sig, fnptr(fn), cpu);
    CHECK(r == OCERZ_OK && g_entered == 1,
          "%s: ocerz_abi_perform returned %d and ran the host function %d "
          "times, want OCERZ_OK and once", notation, r, g_entered);
    CHECK(cpu->rip == RET_ADDR && cpu->gpr[OCERZ_RSP] == sp + 8,
          "%s: rip %#llx rsp %#llx after the crossing, want %#llx and %#llx",
          notation, (unsigned long long)cpu->rip,
          (unsigned long long)cpu->gpr[OCERZ_RSP], (unsigned long long)RET_ADDR,
          (unsigned long long)(sp + 8));
    return cpu->gpr[OCERZ_RAX];
}

static void test_narrow_native(void)
{
    static const struct {
        const char *sig;
        void (*fn)(void);
        uint64_t in;
        uint64_t out;
    } kEcho[] = {
        { "B(B)", FN(echo_B), 0xdeadbeefcafe12ffull, 0xffull },
        { "b(b)", FN(echo_b), 0xdeadbeefcafe12ffull, 0xffffffffffffffffull },
        { "B(B)", FN(echo_B), 0xdeadbeefcafe1280ull, 0x80ull },
        { "b(b)", FN(echo_b), 0xdeadbeefcafe1280ull, 0xffffffffffffff80ull },
        { "b(b)", FN(echo_b), 0xdeadbeefcafe807full, 0x7full },
        { "H(H)", FN(echo_H), 0xdeadbeefcafe80ffull, 0x80ffull },
        { "h(h)", FN(echo_h), 0xdeadbeefcafe80ffull, 0xffffffffffff80ffull },
        { "H(H)", FN(echo_H), 0xdeadbeef00008000ull, 0x8000ull },
        { "h(h)", FN(echo_h), 0xdeadbeef00008000ull, 0xffffffffffff8000ull },
        { "h(h)", FN(echo_h), 0xdeadbeefcaff7fffull, 0x7fffull },
    };
    static const struct {
        const char *sig;
        uint64_t out;
    } kGarbage[] = {
        { "b()", 0xffffffffffffff80ull },
        { "B()", 0x80ull },
        { "h()", 0xffffffffffff8080ull },
        { "H()", 0x8080ull },
    };
    static const uint64_t kSumReg[4] = {
        0xdeadbeefcafe12ffull, 0xdeadbeefcafe8001ull, 0x0123456789abcdfeull,
        0xfedcba9876548001ull,
    };
    static const uint64_t kSpillReg[6] = {
        0xdeadbeef80000001ull, 0x0123456700000002ull, 0xffffffff7ffffffdull,
        0x00000001fffffffcull, 0xa5a5a5a580000005ull, 0x5a5a5a5a00000006ull,
    };
    static const uint64_t kSpillSlot[7] = {
        0xdeadbeef80000007ull, 0xfeedfacefffffff8ull, 0xdeadbeefcafe12feull,
        0xdeadbeefcafe8002ull, 0xdeadbeefcafe12feull, 0xdeadbeefcafe8002ull,
        0xdeadbeef80000009ull,
    };
    size_t i;
    uint64_t got;
    int64_t want;

    for (i = 0; i < sizeof kEcho / sizeof kEcho[0]; i++) {
        got = narrow_call(kEcho[i].sig, kEcho[i].fn, &kEcho[i].in, 1, NULL, 0);
        CHECK(got == kEcho[i].out,
              "%s: a host function returning its argument, passed %#llx in "
              "rdi, left rax %#llx, want %#llx", kEcho[i].sig,
              (unsigned long long)kEcho[i].in, (unsigned long long)got,
              (unsigned long long)kEcho[i].out);
    }

    for (i = 0; i < sizeof kGarbage / sizeof kGarbage[0]; i++) {
        got = narrow_call(kGarbage[i].sig, FN(garbage_result), NULL, 0, NULL, 0);
        CHECK(got == kGarbage[i].out,
              "%s: a host function leaving 0xdeadbeefcafe8080 in x0 left rax "
              "%#llx, want only its low bits extended, %#llx", kGarbage[i].sig,
              (unsigned long long)got, (unsigned long long)kGarbage[i].out);
    }

    want = (int64_t)(int8_t)kSumReg[0] + 3 * (int64_t)(int16_t)kSumReg[1] +
           5 * (int64_t)(uint8_t)kSumReg[2] + 7 * (int64_t)(uint16_t)kSumReg[3];
    got = narrow_call("l(bhBH)", FN(sum_bhBH), kSumReg, 4, NULL, 0);
    CHECK(got == (uint64_t)want,
          "l(bhBH): a host checksum of registers holding garbage above their "
          "low bits returned %lld, want %lld", (long long)got, (long long)want);

    want = 0;
    for (i = 0; i < 6; i++)
        want += (int64_t)(i + 1) * (int64_t)(int32_t)kSpillReg[i];
    want += 7 * (int64_t)(int32_t)kSpillSlot[0] + 8 * (int64_t)(int32_t)kSpillSlot[1] +
            11 * (int64_t)(int8_t)kSpillSlot[2] + 13 * (int64_t)(int16_t)kSpillSlot[3] +
            17 * (int64_t)(uint8_t)kSpillSlot[4] + 19 * (int64_t)(uint16_t)kSpillSlot[5] +
            23 * (int64_t)(int32_t)kSpillSlot[6];
    got = narrow_call("l(iiiiiiiibhBHi)", FN(sum_spill), kSpillReg, 6,
                      kSpillSlot, 7);
    CHECK(got == (uint64_t)want,
          "l(iiiiiiiibhBHi): a host checksum of eight ints and b, h, B, H, i "
          "spilled from garbage-filled guest slots returned %lld, want %lld",
          (long long)got, (long long)want);
}

static void test_accept_classes(void)
{
    static const char kScalar[] = "bBhHiulLpfd";
    char notation[8];
    size_t r, a;

    for (r = 0; r < sizeof kScalar; r++) {
        for (a = 0; a < sizeof kScalar - 1; a++) {
            OcerzAbiSig sig;
            int rc;

            notation[0] = r < sizeof kScalar - 1 ? kScalar[r] : 'v';
            notation[1] = '(';
            notation[2] = kScalar[a];
            notation[3] = ')';
            notation[4] = 0;
            memset(&sig, 0xa5, sizeof sig);
            rc = ocerz_abi_parse(notation, &sig);
            CHECK(rc == OCERZ_OK && sig.ret == notation[0] && sig.nargs == 1 &&
                      sig.arg[0] == notation[2],
                  "ocerz_abi_parse(\"%s\") returned %d with result '%c' and "
                  "%d argument(s), want OCERZ_OK, '%c' and one '%c'", notation,
                  rc, sig.ret, sig.nargs, notation[0], notation[2]);
        }
    }
}

static void test_rounding(void)
{
    static const AbiCase kSmall = { "d(dd)", FN(fn_D2), 0, 0 };
    static const AbiCase kWide = { "l(LdLdLdLdLdLdLddd)", FN(fn_7I9D), 0, 0 };
    static const unsigned kFpcrRmode[4] = { 0, 2, 1, 3 };
    int rc;

    for (rc = 0; rc < 4; rc++) {
        uint64_t fpcr;

        g_mxcsr = MXCSR_DEFAULT | ((uint32_t)rc << 13);
        ocerz_apply_mxcsr_round(g_mxcsr);
        fpcr = rd_fpcr();
        CHECK(((unsigned)(fpcr >> 22) & 3u) == kFpcrRmode[rc],
              "mxcsr rounding %d drove fpcr rounding mode %u, want %u; the "
              "crossing's rounding checks would not be testing anything",
              rc, (unsigned)(fpcr >> 22) & 3u, kFpcrRmode[rc]);
        run_case(&kSmall);
        run_case(&kWide);
    }

    g_mxcsr = MXCSR_DEFAULT;
    ocerz_apply_mxcsr_round(g_mxcsr);
}

static const char *const kBadSigs[] = {
    "", "v", "L", "(", ")", "()", "(pp)", "ipp", "i(pp", "ipp)", "i(",
    "i)", "i(pp))", "i(pp)x", "i()x", "i((p))",
    "z()", "x(i)", "n(i)", "V()", "I()", "U(i)", "P(p)", "F(f)", "D(d)",
    "i(v)", "v(v)", "d(vd)", "d(dv)", "i(V)", "i(I)", "i(U)", "i(F)", "i(D)",
    "s()", "s(s)", "S(i)", "i(s)", "i(S)", "{}()",
    "i(t)", "i(0)", "i(p,p)", "i(p p)", " i(p)", "i (p)",
    "L(LLLLLLLLLLLLLLLLL)",
    "b(", "B(bB", "h(hH))", "H(H)x", "b(s)", "i(bv)", "v(Hh", "bh(i)",
    "(bBhH)", "b()h", "B(b h)", "H(b,h)", "g()", "i(k)", "c(b)", "C()",
    "h(bBhHbBhHbBhHbBhHb)",
};
#define NBAD (sizeof kBadSigs / sizeof kBadSigs[0])

static void test_reject_parse(void)
{
    size_t i;

    for (i = 0; i < NBAD; i++) {
        OcerzAbiSig sig;
        int r;

        memset(&sig, 0xa5, sizeof sig);
        r = ocerz_abi_parse(kBadSigs[i], &sig);
        CHECK(r != OCERZ_OK,
              "ocerz_abi_parse(\"%s\") accepted a notation this change does "
              "not support", kBadSigs[i]);
    }
}

static const OcerzAbiSig kBadCalls[] = {
    { .ret = 'i', .arg = { 's' }, .nargs = 1 },
    { .ret = 's', .arg = { 0 }, .nargs = 0 },
    { .ret = 'S', .arg = { 'p' }, .nargs = 1 },
    { .ret = 'i', .arg = { 'v' }, .nargs = 1 },
    { .ret = 'v', .arg = { 'p', 'S' }, .nargs = 2 },
    { .ret = 'v', .arg = { '{' }, .nargs = 1 },
    { .ret = 'i', .arg = { 'p' }, .nargs = OCERZ_ABI_MAX_ARGS + 1 },
    { .ret = '{', .nargs = 0 },
    { .ret = '{', .nargs = 0,
      .ret_struct = { .size = 8, .align = 8, .nmember = 1, .member = { 'c' } } },
    { .ret = '{', .nargs = 0,
      .ret_struct = { .size = 24, .align = 8, .nmember = 3, .member = { 'L', 'L', 'L' },
                      .offset = { 0, 8, 8 } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 16, .align = 8, .nmember = 2, .member = { 'd', 'd' },
                        .offset = { 0, 4 } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 16, .align = 8, .nmember = 1, .member = { 'd' } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 8, .align = 4, .nmember = 2, .member = { 'i', 'i' },
                        .offset = { 0, 2 } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 8, .align = 8, .nmember = 1, .member = { 'v' } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 8, .align = 4, .nmember = 1, .member = { 'd' } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 8, .align = 8, .nmember = 2, .member = { 'd', 'd' },
                        .offset = { 0, 8 } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 264, .align = 8, .nmember = 1, .member = { 'd' } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 12, .align = 4, .nmember = 2, .member = { 'f', 'f' },
                        .offset = { 0, 4 } } } },
    { .ret = 'v', .arg = { '{' }, .nargs = 1,
      .arg_struct = { { .size = 136, .align = 8, .nmember = OCERZ_ABI_STRUCT_MEMBERS + 1 } } },
    { .ret = 'v', .arg = { 'p', '{' }, .nargs = 2,
      .arg_struct = { [1] = { .size = 4, .align = 4, .nmember = 1, .member = { '{' } } } },
};
#define NBADCALLS (sizeof kBadCalls / sizeof kBadCalls[0])

static void test_reject_perform(void)
{
    OcerzCPU *cpu = &g_cpu;
    uint64_t want[OCERZ_ABI_MAX_ARGS];
    size_t i;

    for (i = 0; i < NBADCALLS; i++) {
        OcerzAbiCall call;
        uint64_t sp;
        int r;

        g_entered = 0;
        sp = setup_call(cpu, "", 0, 0, want);

        memset(&call, 0, sizeof call);
        r = ocerz_abi_read_guest(&kBadCalls[i], cpu, &call);
        CHECK(r != OCERZ_OK,
              "ocerz_abi_read_guest read a signature whose result class is "
              "'%c' and whose first argument class is '%c'", kBadCalls[i].ret,
              kBadCalls[i].arg[0] ? kBadCalls[i].arg[0] : '-');

        r = ocerz_abi_perform(&kBadCalls[i], fnptr(FN(fn_V0)), cpu);
        CHECK(r != OCERZ_OK,
              "ocerz_abi_perform ran a signature whose result class is '%c' "
              "and whose first argument class is '%c'", kBadCalls[i].ret,
              kBadCalls[i].arg[0] ? kBadCalls[i].arg[0] : '-');
        CHECK(g_entered == 0,
              "ocerz_abi_perform called the host function for a rejected "
              "signature ('%c' result, '%c' first argument)", kBadCalls[i].ret,
              kBadCalls[i].arg[0] ? kBadCalls[i].arg[0] : '-');
        CHECK(cpu->rip == TRAP_RIP,
              "a rejected signature moved rip to %#llx; the guest never "
              "reached the call, so the return address must still be unspent",
              (unsigned long long)cpu->rip);
        CHECK(cpu->gpr[OCERZ_RSP] == sp,
              "a rejected signature moved rsp to %#llx, want %#llx",
              (unsigned long long)cpu->gpr[OCERZ_RSP],
              (unsigned long long)sp);
        CHECK(cpu->gpr[OCERZ_RAX] == RAX_POISON,
              "a rejected signature wrote %#llx to rax",
              (unsigned long long)cpu->gpr[OCERZ_RAX]);
    }
}

typedef struct StType {
    const char *notation;
    size_t size;
    size_t align;
    int n;
    const char *cls;
    uint8_t off[OCERZ_ABI_STRUCT_MEMBERS];
} StType;

typedef struct StCase {
    const char *sig;
    void (*fn)(void);
    char ret;
    int ret_type;
    int nargs;
    char arg[OCERZ_ABI_MAX_ARGS];
    int type[OCERZ_ABI_MAX_ARGS];
} StCase;

#define ST_STACK_OFF 0x8000ull
#define ST_RET_OFF   0xa000ull
#define ST_RET_SPAN  0x200

typedef struct { uint8_t m0; } S_oBc;
typedef struct { int16_t m0; } S_ohc;
typedef struct { int32_t m0; } S_oic;
typedef struct { uint64_t m0; } S_oLc;
typedef struct { void *m0; } S_opc;
typedef struct { uint64_t m0; uint64_t m1; } S_oLLc;
typedef struct { int32_t m0; uint64_t m1; } S_oiLc;
typedef struct { uint64_t m0; int32_t m1; } S_oLic;
typedef struct { float m0; } S_ofc;
typedef struct { float m0; float m1; } S_offc;
typedef struct { float m0; float m1; float m2; } S_offfc;
typedef struct { float m0; float m1; float m2; float m3; } S_offffc;
typedef struct { float m0; float m1; float m2; float m3; float m4; } S_offfffc;
typedef struct { double m0; } S_odc;
typedef struct { double m0; double m1; } S_oddc;
typedef struct { double m0; double m1; double m2; } S_odddc;
typedef struct { double m0; double m1; double m2; double m3; } S_oddddc;
typedef struct { double m0; double m1; double m2; double m3; double m4; } S_odddddc;
typedef struct { double m0; float m1; } S_odfc;
typedef struct { double m0; uint64_t m1; } S_odLc;
typedef struct { uint64_t m0; double m1; } S_oLdc;
typedef struct { struct { double m0; double m1; } m0; struct { double m0; double m1; } m1; } S_ooddcoddcc;
typedef struct { uint64_t m0; uint64_t m1; uint64_t m2; } S_oLLLc;
typedef struct { int8_t m0; uint8_t m1; int16_t m2; uint16_t m3; int32_t m4; uint32_t m5; } S_obBhHiuc;
typedef struct { void *m0; double m1; int32_t m2; } S_opdic;
typedef struct { int8_t m0; double m1; } S_obdc;
typedef struct { int8_t m0; int16_t m1; int8_t m2; } S_obhbc;
typedef struct { uint64_t m0; int8_t m1; } S_oLbc;
typedef struct { float m0; double m1; } S_ofdc;
typedef struct { int8_t m0; float m1; } S_obfc;
typedef struct { float m0; int32_t m1; } S_ofic;
typedef struct { struct { double m0; int8_t m1; } m0; int8_t m1; } S_oodbcbc;
typedef struct { struct { int8_t m0; int16_t m1; } m0; int32_t m1; } S_oobhcic;
typedef struct { uint8_t m0; uint8_t m1; uint8_t m2; } S_oBBBc;
typedef struct { struct { float m0; float m1; } m0; struct { double m0; double m1; } m1; } S_ooffcoddcc;
typedef struct { double m0; double m1; double m2; double m3; double m4; double m5; } S_oddddddc;
typedef struct { double m0; double m1; double m2; double m3; double m4; double m5; double m6; double m7; double m8; double m9; double m10; double m11; double m12; double m13; double m14; double m15; } S_oddddddddddddddddc;
typedef struct { struct { float m0; } m0; struct { float m0; } m1; } S_oofcofcc;
typedef struct { struct { struct { double m0; } m0; } m0; } S_ooodccc;
typedef struct { uint16_t m0; uint32_t m1; } S_oHuc;
typedef struct { int64_t m0; uint16_t m1; } S_olHc;
typedef struct { struct { float m0; float m1; } m0; float m1; } S_ooffcfc;
typedef struct { int16_t m0; int8_t m1; } S_ohbc;
typedef struct { uint32_t m0; int8_t m1; } S_oubc;
typedef struct { int8_t m0; struct { double m0; } m1; int8_t m2; struct { double m0; } m3; int8_t m4; struct { double m0; } m5; int8_t m6; struct { double m0; } m7; int8_t m8; struct { double m0; } m9; int8_t m10; struct { double m0; } m11; int8_t m12; struct { double m0; } m13; int8_t m14; struct { double m0; } m15; } S_obodcbodcbodcbodcbodcbodcbodcbodcc;
typedef struct { struct { struct { struct { struct { struct { struct { struct { double m0; } m0; } m0; } m0; } m0; } m0; } m0; } m0; } S_oooooooodcccccccc;
typedef struct { struct { double m0; int8_t m1; } m0; struct { double m0; int8_t m1; } m1; int8_t m2; } S_oodbcodbcbc;
typedef struct { struct { int8_t m0; int16_t m1; int8_t m2; } m0; struct { uint64_t m0; int8_t m1; } m1; } S_oobhbcoLbcc;
typedef struct { uint32_t m0; uint64_t m1; int8_t m2; } S_ouLbc;

enum {
    ST_oBc,
    ST_ohc,
    ST_oic,
    ST_oLc,
    ST_opc,
    ST_oLLc,
    ST_oiLc,
    ST_oLic,
    ST_ofc,
    ST_offc,
    ST_offfc,
    ST_offffc,
    ST_offfffc,
    ST_odc,
    ST_oddc,
    ST_odddc,
    ST_oddddc,
    ST_odddddc,
    ST_odfc,
    ST_odLc,
    ST_oLdc,
    ST_ooddcoddcc,
    ST_oLLLc,
    ST_obBhHiuc,
    ST_opdic,
    ST_obdc,
    ST_obhbc,
    ST_oLbc,
    ST_ofdc,
    ST_obfc,
    ST_ofic,
    ST_oodbcbc,
    ST_oobhcic,
    ST_oBBBc,
    ST_ooffcoddcc,
    ST_oddddddc,
    ST_oddddddddddddddddc,
    ST_oofcofcc,
    ST_ooodccc,
    ST_oHuc,
    ST_olHc,
    ST_ooffcfc,
    ST_ohbc,
    ST_oubc,
    ST_obodcbodcbodcbodcbodcbodcbodcbodcc,
    ST_oooooooodcccccccc,
    ST_oodbcodbcbc,
    ST_oobhbcoLbcc,
    ST_ouLbc,
    ST_COUNT
};

static const StType kStTypes[ST_COUNT] = {
    { "{B}", sizeof(S_oBc), _Alignof(S_oBc), 1, "B",
      { offsetof(S_oBc, m0) } },
    { "{h}", sizeof(S_ohc), _Alignof(S_ohc), 1, "h",
      { offsetof(S_ohc, m0) } },
    { "{i}", sizeof(S_oic), _Alignof(S_oic), 1, "i",
      { offsetof(S_oic, m0) } },
    { "{L}", sizeof(S_oLc), _Alignof(S_oLc), 1, "L",
      { offsetof(S_oLc, m0) } },
    { "{p}", sizeof(S_opc), _Alignof(S_opc), 1, "p",
      { offsetof(S_opc, m0) } },
    { "{LL}", sizeof(S_oLLc), _Alignof(S_oLLc), 2, "LL",
      { offsetof(S_oLLc, m0), offsetof(S_oLLc, m1) } },
    { "{iL}", sizeof(S_oiLc), _Alignof(S_oiLc), 2, "iL",
      { offsetof(S_oiLc, m0), offsetof(S_oiLc, m1) } },
    { "{Li}", sizeof(S_oLic), _Alignof(S_oLic), 2, "Li",
      { offsetof(S_oLic, m0), offsetof(S_oLic, m1) } },
    { "{f}", sizeof(S_ofc), _Alignof(S_ofc), 1, "f",
      { offsetof(S_ofc, m0) } },
    { "{ff}", sizeof(S_offc), _Alignof(S_offc), 2, "ff",
      { offsetof(S_offc, m0), offsetof(S_offc, m1) } },
    { "{fff}", sizeof(S_offfc), _Alignof(S_offfc), 3, "fff",
      { offsetof(S_offfc, m0), offsetof(S_offfc, m1), offsetof(S_offfc, m2) } },
    { "{ffff}", sizeof(S_offffc), _Alignof(S_offffc), 4, "ffff",
      { offsetof(S_offffc, m0), offsetof(S_offffc, m1), offsetof(S_offffc, m2), offsetof(S_offffc, m3) } },
    { "{fffff}", sizeof(S_offfffc), _Alignof(S_offfffc), 5, "fffff",
      { offsetof(S_offfffc, m0), offsetof(S_offfffc, m1), offsetof(S_offfffc, m2), offsetof(S_offfffc, m3), offsetof(S_offfffc, m4) } },
    { "{d}", sizeof(S_odc), _Alignof(S_odc), 1, "d",
      { offsetof(S_odc, m0) } },
    { "{dd}", sizeof(S_oddc), _Alignof(S_oddc), 2, "dd",
      { offsetof(S_oddc, m0), offsetof(S_oddc, m1) } },
    { "{ddd}", sizeof(S_odddc), _Alignof(S_odddc), 3, "ddd",
      { offsetof(S_odddc, m0), offsetof(S_odddc, m1), offsetof(S_odddc, m2) } },
    { "{dddd}", sizeof(S_oddddc), _Alignof(S_oddddc), 4, "dddd",
      { offsetof(S_oddddc, m0), offsetof(S_oddddc, m1), offsetof(S_oddddc, m2), offsetof(S_oddddc, m3) } },
    { "{ddddd}", sizeof(S_odddddc), _Alignof(S_odddddc), 5, "ddddd",
      { offsetof(S_odddddc, m0), offsetof(S_odddddc, m1), offsetof(S_odddddc, m2), offsetof(S_odddddc, m3), offsetof(S_odddddc, m4) } },
    { "{df}", sizeof(S_odfc), _Alignof(S_odfc), 2, "df",
      { offsetof(S_odfc, m0), offsetof(S_odfc, m1) } },
    { "{dL}", sizeof(S_odLc), _Alignof(S_odLc), 2, "dL",
      { offsetof(S_odLc, m0), offsetof(S_odLc, m1) } },
    { "{Ld}", sizeof(S_oLdc), _Alignof(S_oLdc), 2, "Ld",
      { offsetof(S_oLdc, m0), offsetof(S_oLdc, m1) } },
    { "{{dd}{dd}}", sizeof(S_ooddcoddcc), _Alignof(S_ooddcoddcc), 4, "dddd",
      { offsetof(S_ooddcoddcc, m0.m0), offsetof(S_ooddcoddcc, m0.m1), offsetof(S_ooddcoddcc, m1.m0), offsetof(S_ooddcoddcc, m1.m1) } },
    { "{LLL}", sizeof(S_oLLLc), _Alignof(S_oLLLc), 3, "LLL",
      { offsetof(S_oLLLc, m0), offsetof(S_oLLLc, m1), offsetof(S_oLLLc, m2) } },
    { "{bBhHiu}", sizeof(S_obBhHiuc), _Alignof(S_obBhHiuc), 6, "bBhHiu",
      { offsetof(S_obBhHiuc, m0), offsetof(S_obBhHiuc, m1), offsetof(S_obBhHiuc, m2), offsetof(S_obBhHiuc, m3), offsetof(S_obBhHiuc, m4), offsetof(S_obBhHiuc, m5) } },
    { "{pdi}", sizeof(S_opdic), _Alignof(S_opdic), 3, "pdi",
      { offsetof(S_opdic, m0), offsetof(S_opdic, m1), offsetof(S_opdic, m2) } },
    { "{bd}", sizeof(S_obdc), _Alignof(S_obdc), 2, "bd",
      { offsetof(S_obdc, m0), offsetof(S_obdc, m1) } },
    { "{bhb}", sizeof(S_obhbc), _Alignof(S_obhbc), 3, "bhb",
      { offsetof(S_obhbc, m0), offsetof(S_obhbc, m1), offsetof(S_obhbc, m2) } },
    { "{Lb}", sizeof(S_oLbc), _Alignof(S_oLbc), 2, "Lb",
      { offsetof(S_oLbc, m0), offsetof(S_oLbc, m1) } },
    { "{fd}", sizeof(S_ofdc), _Alignof(S_ofdc), 2, "fd",
      { offsetof(S_ofdc, m0), offsetof(S_ofdc, m1) } },
    { "{bf}", sizeof(S_obfc), _Alignof(S_obfc), 2, "bf",
      { offsetof(S_obfc, m0), offsetof(S_obfc, m1) } },
    { "{fi}", sizeof(S_ofic), _Alignof(S_ofic), 2, "fi",
      { offsetof(S_ofic, m0), offsetof(S_ofic, m1) } },
    { "{{db}b}", sizeof(S_oodbcbc), _Alignof(S_oodbcbc), 3, "dbb",
      { offsetof(S_oodbcbc, m0.m0), offsetof(S_oodbcbc, m0.m1), offsetof(S_oodbcbc, m1) } },
    { "{{bh}i}", sizeof(S_oobhcic), _Alignof(S_oobhcic), 3, "bhi",
      { offsetof(S_oobhcic, m0.m0), offsetof(S_oobhcic, m0.m1), offsetof(S_oobhcic, m1) } },
    { "{BBB}", sizeof(S_oBBBc), _Alignof(S_oBBBc), 3, "BBB",
      { offsetof(S_oBBBc, m0), offsetof(S_oBBBc, m1), offsetof(S_oBBBc, m2) } },
    { "{{ff}{dd}}", sizeof(S_ooffcoddcc), _Alignof(S_ooffcoddcc), 4, "ffdd",
      { offsetof(S_ooffcoddcc, m0.m0), offsetof(S_ooffcoddcc, m0.m1), offsetof(S_ooffcoddcc, m1.m0), offsetof(S_ooffcoddcc, m1.m1) } },
    { "{dddddd}", sizeof(S_oddddddc), _Alignof(S_oddddddc), 6, "dddddd",
      { offsetof(S_oddddddc, m0), offsetof(S_oddddddc, m1), offsetof(S_oddddddc, m2), offsetof(S_oddddddc, m3), offsetof(S_oddddddc, m4), offsetof(S_oddddddc, m5) } },
    { "{dddddddddddddddd}", sizeof(S_oddddddddddddddddc), _Alignof(S_oddddddddddddddddc), 16, "dddddddddddddddd",
      { offsetof(S_oddddddddddddddddc, m0), offsetof(S_oddddddddddddddddc, m1), offsetof(S_oddddddddddddddddc, m2), offsetof(S_oddddddddddddddddc, m3), offsetof(S_oddddddddddddddddc, m4), offsetof(S_oddddddddddddddddc, m5), offsetof(S_oddddddddddddddddc, m6), offsetof(S_oddddddddddddddddc, m7), offsetof(S_oddddddddddddddddc, m8), offsetof(S_oddddddddddddddddc, m9), offsetof(S_oddddddddddddddddc, m10), offsetof(S_oddddddddddddddddc, m11), offsetof(S_oddddddddddddddddc, m12), offsetof(S_oddddddddddddddddc, m13), offsetof(S_oddddddddddddddddc, m14), offsetof(S_oddddddddddddddddc, m15) } },
    { "{{f}{f}}", sizeof(S_oofcofcc), _Alignof(S_oofcofcc), 2, "ff",
      { offsetof(S_oofcofcc, m0.m0), offsetof(S_oofcofcc, m1.m0) } },
    { "{{{d}}}", sizeof(S_ooodccc), _Alignof(S_ooodccc), 1, "d",
      { offsetof(S_ooodccc, m0.m0.m0) } },
    { "{Hu}", sizeof(S_oHuc), _Alignof(S_oHuc), 2, "Hu",
      { offsetof(S_oHuc, m0), offsetof(S_oHuc, m1) } },
    { "{lH}", sizeof(S_olHc), _Alignof(S_olHc), 2, "lH",
      { offsetof(S_olHc, m0), offsetof(S_olHc, m1) } },
    { "{{ff}f}", sizeof(S_ooffcfc), _Alignof(S_ooffcfc), 3, "fff",
      { offsetof(S_ooffcfc, m0.m0), offsetof(S_ooffcfc, m0.m1), offsetof(S_ooffcfc, m1) } },
    { "{hb}", sizeof(S_ohbc), _Alignof(S_ohbc), 2, "hb",
      { offsetof(S_ohbc, m0), offsetof(S_ohbc, m1) } },
    { "{ub}", sizeof(S_oubc), _Alignof(S_oubc), 2, "ub",
      { offsetof(S_oubc, m0), offsetof(S_oubc, m1) } },
    { "{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}}", sizeof(S_obodcbodcbodcbodcbodcbodcbodcbodcc), _Alignof(S_obodcbodcbodcbodcbodcbodcbodcbodcc), 16, "bdbdbdbdbdbdbdbd",
      { offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m1.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m2), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m3.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m4), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m5.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m6), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m7.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m8), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m9.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m10), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m11.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m12), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m13.m0), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m14), offsetof(S_obodcbodcbodcbodcbodcbodcbodcbodcc, m15.m0) } },
    { "{{{{{{{{d}}}}}}}}", sizeof(S_oooooooodcccccccc), _Alignof(S_oooooooodcccccccc), 1, "d",
      { offsetof(S_oooooooodcccccccc, m0.m0.m0.m0.m0.m0.m0.m0) } },
    { "{{db}{db}b}", sizeof(S_oodbcodbcbc), _Alignof(S_oodbcodbcbc), 5, "dbdbb",
      { offsetof(S_oodbcodbcbc, m0.m0), offsetof(S_oodbcodbcbc, m0.m1), offsetof(S_oodbcodbcbc, m1.m0), offsetof(S_oodbcodbcbc, m1.m1), offsetof(S_oodbcodbcbc, m2) } },
    { "{{bhb}{Lb}}", sizeof(S_oobhbcoLbcc), _Alignof(S_oobhbcoLbcc), 5, "bhbLb",
      { offsetof(S_oobhbcoLbcc, m0.m0), offsetof(S_oobhbcoLbcc, m0.m1), offsetof(S_oobhbcoLbcc, m0.m2), offsetof(S_oobhbcoLbcc, m1.m0), offsetof(S_oobhbcoLbcc, m1.m1) } },
    { "{uLb}", sizeof(S_ouLbc), _Alignof(S_ouLbc), 3, "uLb",
      { offsetof(S_ouLbc, m0), offsetof(S_ouLbc, m1), offsetof(S_ouLbc, m2) } },
};

static uint64_t g_scratch;
static uint8_t g_st_bytes[OCERZ_ABI_MAX_ARGS][OCERZ_ABI_STRUCT_BYTES];
static uint64_t g_st_host[OCERZ_ABI_MAX_ARGS];
static uint64_t g_st_ret_buf;
static OcerzAbiCall g_frame_call;

static size_t cls_size(char c)
{
    switch (c) {
    case 'b':
    case 'B': return 1;
    case 'h':
    case 'H': return 2;
    case 'i':
    case 'u':
    case 'f': return 4;
    default:  return 8;
    }
}

static uint64_t res_member_bits(char c, int m)
{
    switch (c) {
    case 'f': return 0x3dcafe01u + (uint32_t)m * 0x10101u;
    case 'd': return 0x40dcafe000000001ull + (uint64_t)m * 0x1010101ull;
    case 'b':
    case 'B': return (uint8_t)(0x91 + 3 * m);
    case 'h':
    case 'H': return (uint16_t)(0x9123 + 0x111 * m);
    case 'i':
    case 'u': return (uint32_t)(0x9abc0123u + 0x1011u * (uint32_t)m);
    case 'p': return (uint64_t)(uintptr_t)g_res_ptr + 8ull * (uint64_t)m;
    default:  return 0x7e57a11c00000000ull + (uint64_t)m * 0x0001000100010001ull;
    }
}

static void st_result(void *p, int type)
{
    const StType *t = &kStTypes[type];
    int m;

    memset(p, 0, t->size);
    for (m = 0; m < t->n; m++) {
        uint64_t bits = res_member_bits(t->cls[m], m);
        memcpy((uint8_t *)p + t->off[m], &bits, cls_size(t->cls[m]));
    }
}

static uint64_t member_bits(char c, int arg, int m, int edge, int *ne)
{
    int k = arg * 16 + m;

    switch (c) {
    case 'f': return edge ? kEdgeF[(*ne)++ & 7] : arg_fbits(k);
    case 'd': return edge ? kEdgeD[(*ne)++ & 7] : arg_dbits(k);
    case 'b':
    case 'B': return edge ? kEdge8[(*ne)++ & 7] : arg_bits8(k);
    case 'h':
    case 'H': return edge ? kEdge16[(*ne)++ & 7] : arg_bits16(k);
    case 'i':
    case 'u': return arg_bits32(k);
    case 'p': return (edge && (k & 1) == 0) ? 0 : g_ptr_base + 0x40ull * (uint64_t)(k + 1);
    default:  return arg_bits64(k);
    }
}

static int st_sysv_class(const StType *t, char cls[2])
{
    int n, m;

    if (t->size > 16)
        return 0;
    n = (int)((t->size + 7) / 8);
    cls[0] = 'S';
    cls[1] = 'S';
    for (m = 0; m < t->n; m++)
        if (!is_fp(t->cls[m]))
            cls[t->off[m] / 8] = 'I';
    return n;
}

static int st_hfa(const StType *t)
{
    int m;

    if (t->n > 4 || !is_fp(t->cls[0]))
        return 0;
    for (m = 1; m < t->n; m++)
        if (t->cls[m] != t->cls[0])
            return 0;
    return 1;
}

static S_oBc sfn_0(S_oBc a0)
{
    enter();
    rec_u8(a0.m0);
    S_oBc r;
    st_result(&r, ST_oBc);
    return r;
}

static S_oBc sfn_1(uint64_t a0, S_oBc a1, double a2, S_oBc a3)
{
    enter();
    rec(a0);
    rec_u8(a1.m0);
    rec_d(a2);
    rec_u8(a3.m0);
    S_oBc r;
    st_result(&r, ST_oBc);
    return r;
}

static S_ohc sfn_2(S_ohc a0)
{
    enter();
    rec_s16(a0.m0);
    S_ohc r;
    st_result(&r, ST_ohc);
    return r;
}

static S_ohc sfn_3(uint64_t a0, S_ohc a1, double a2, S_ohc a3)
{
    enter();
    rec(a0);
    rec_s16(a1.m0);
    rec_d(a2);
    rec_s16(a3.m0);
    S_ohc r;
    st_result(&r, ST_ohc);
    return r;
}

static S_oic sfn_4(S_oic a0)
{
    enter();
    rec((uint32_t)a0.m0);
    S_oic r;
    st_result(&r, ST_oic);
    return r;
}

static S_oic sfn_5(uint64_t a0, S_oic a1, double a2, S_oic a3)
{
    enter();
    rec(a0);
    rec((uint32_t)a1.m0);
    rec_d(a2);
    rec((uint32_t)a3.m0);
    S_oic r;
    st_result(&r, ST_oic);
    return r;
}

static S_oLc sfn_6(S_oLc a0)
{
    enter();
    rec(a0.m0);
    S_oLc r;
    st_result(&r, ST_oLc);
    return r;
}

static S_oLc sfn_7(uint64_t a0, S_oLc a1, double a2, S_oLc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec_d(a2);
    rec(a3.m0);
    S_oLc r;
    st_result(&r, ST_oLc);
    return r;
}

static S_opc sfn_8(S_opc a0)
{
    enter();
    rec((uint64_t)(uintptr_t)a0.m0);
    S_opc r;
    st_result(&r, ST_opc);
    return r;
}

static S_opc sfn_9(uint64_t a0, S_opc a1, double a2, S_opc a3)
{
    enter();
    rec(a0);
    rec((uint64_t)(uintptr_t)a1.m0);
    rec_d(a2);
    rec((uint64_t)(uintptr_t)a3.m0);
    S_opc r;
    st_result(&r, ST_opc);
    return r;
}

static S_oLLc sfn_10(S_oLLc a0)
{
    enter();
    rec(a0.m0);
    rec(a0.m1);
    S_oLLc r;
    st_result(&r, ST_oLLc);
    return r;
}

static S_oLLc sfn_11(uint64_t a0, S_oLLc a1, double a2, S_oLLc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec(a1.m1);
    rec_d(a2);
    rec(a3.m0);
    rec(a3.m1);
    S_oLLc r;
    st_result(&r, ST_oLLc);
    return r;
}

static S_oiLc sfn_12(S_oiLc a0)
{
    enter();
    rec((uint32_t)a0.m0);
    rec(a0.m1);
    S_oiLc r;
    st_result(&r, ST_oiLc);
    return r;
}

static S_oiLc sfn_13(uint64_t a0, S_oiLc a1, double a2, S_oiLc a3)
{
    enter();
    rec(a0);
    rec((uint32_t)a1.m0);
    rec(a1.m1);
    rec_d(a2);
    rec((uint32_t)a3.m0);
    rec(a3.m1);
    S_oiLc r;
    st_result(&r, ST_oiLc);
    return r;
}

static S_oLic sfn_14(S_oLic a0)
{
    enter();
    rec(a0.m0);
    rec((uint32_t)a0.m1);
    S_oLic r;
    st_result(&r, ST_oLic);
    return r;
}

static S_oLic sfn_15(uint64_t a0, S_oLic a1, double a2, S_oLic a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec((uint32_t)a1.m1);
    rec_d(a2);
    rec(a3.m0);
    rec((uint32_t)a3.m1);
    S_oLic r;
    st_result(&r, ST_oLic);
    return r;
}

static S_ofc sfn_16(S_ofc a0)
{
    enter();
    rec_f(a0.m0);
    S_ofc r;
    st_result(&r, ST_ofc);
    return r;
}

static S_ofc sfn_17(uint64_t a0, S_ofc a1, double a2, S_ofc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_d(a2);
    rec_f(a3.m0);
    S_ofc r;
    st_result(&r, ST_ofc);
    return r;
}

static S_offc sfn_18(S_offc a0)
{
    enter();
    rec_f(a0.m0);
    rec_f(a0.m1);
    S_offc r;
    st_result(&r, ST_offc);
    return r;
}

static S_offc sfn_19(uint64_t a0, S_offc a1, double a2, S_offc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_f(a1.m1);
    rec_d(a2);
    rec_f(a3.m0);
    rec_f(a3.m1);
    S_offc r;
    st_result(&r, ST_offc);
    return r;
}

static S_offfc sfn_20(S_offfc a0)
{
    enter();
    rec_f(a0.m0);
    rec_f(a0.m1);
    rec_f(a0.m2);
    S_offfc r;
    st_result(&r, ST_offfc);
    return r;
}

static S_offfc sfn_21(uint64_t a0, S_offfc a1, double a2, S_offfc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_f(a1.m1);
    rec_f(a1.m2);
    rec_d(a2);
    rec_f(a3.m0);
    rec_f(a3.m1);
    rec_f(a3.m2);
    S_offfc r;
    st_result(&r, ST_offfc);
    return r;
}

static S_offffc sfn_22(S_offffc a0)
{
    enter();
    rec_f(a0.m0);
    rec_f(a0.m1);
    rec_f(a0.m2);
    rec_f(a0.m3);
    S_offffc r;
    st_result(&r, ST_offffc);
    return r;
}

static S_offffc sfn_23(uint64_t a0, S_offffc a1, double a2, S_offffc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_f(a1.m1);
    rec_f(a1.m2);
    rec_f(a1.m3);
    rec_d(a2);
    rec_f(a3.m0);
    rec_f(a3.m1);
    rec_f(a3.m2);
    rec_f(a3.m3);
    S_offffc r;
    st_result(&r, ST_offffc);
    return r;
}

static S_offfffc sfn_24(S_offfffc a0)
{
    enter();
    rec_f(a0.m0);
    rec_f(a0.m1);
    rec_f(a0.m2);
    rec_f(a0.m3);
    rec_f(a0.m4);
    S_offfffc r;
    st_result(&r, ST_offfffc);
    return r;
}

static S_offfffc sfn_25(uint64_t a0, S_offfffc a1, double a2, S_offfffc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_f(a1.m1);
    rec_f(a1.m2);
    rec_f(a1.m3);
    rec_f(a1.m4);
    rec_d(a2);
    rec_f(a3.m0);
    rec_f(a3.m1);
    rec_f(a3.m2);
    rec_f(a3.m3);
    rec_f(a3.m4);
    S_offfffc r;
    st_result(&r, ST_offfffc);
    return r;
}

static S_odc sfn_26(S_odc a0)
{
    enter();
    rec_d(a0.m0);
    S_odc r;
    st_result(&r, ST_odc);
    return r;
}

static S_odc sfn_27(uint64_t a0, S_odc a1, double a2, S_odc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a2);
    rec_d(a3.m0);
    S_odc r;
    st_result(&r, ST_odc);
    return r;
}

static S_oddc sfn_28(S_oddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    S_oddc r;
    st_result(&r, ST_oddc);
    return r;
}

static S_oddc sfn_29(uint64_t a0, S_oddc a1, double a2, S_oddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    S_oddc r;
    st_result(&r, ST_oddc);
    return r;
}

static S_odddc sfn_30(S_odddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    S_odddc r;
    st_result(&r, ST_odddc);
    return r;
}

static S_odddc sfn_31(uint64_t a0, S_odddc a1, double a2, S_odddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    rec_d(a3.m2);
    S_odddc r;
    st_result(&r, ST_odddc);
    return r;
}

static S_oddddc sfn_32(S_oddddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    S_oddddc r;
    st_result(&r, ST_oddddc);
    return r;
}

static S_oddddc sfn_33(uint64_t a0, S_oddddc a1, double a2, S_oddddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    rec_d(a3.m2);
    rec_d(a3.m3);
    S_oddddc r;
    st_result(&r, ST_oddddc);
    return r;
}

static S_odddddc sfn_34(S_odddddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a0.m4);
    S_odddddc r;
    st_result(&r, ST_odddddc);
    return r;
}

static S_odddddc sfn_35(uint64_t a0, S_odddddc a1, double a2, S_odddddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a1.m4);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    rec_d(a3.m2);
    rec_d(a3.m3);
    rec_d(a3.m4);
    S_odddddc r;
    st_result(&r, ST_odddddc);
    return r;
}

static S_odfc sfn_36(S_odfc a0)
{
    enter();
    rec_d(a0.m0);
    rec_f(a0.m1);
    S_odfc r;
    st_result(&r, ST_odfc);
    return r;
}

static S_odfc sfn_37(uint64_t a0, S_odfc a1, double a2, S_odfc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_f(a1.m1);
    rec_d(a2);
    rec_d(a3.m0);
    rec_f(a3.m1);
    S_odfc r;
    st_result(&r, ST_odfc);
    return r;
}

static S_odLc sfn_38(S_odLc a0)
{
    enter();
    rec_d(a0.m0);
    rec(a0.m1);
    S_odLc r;
    st_result(&r, ST_odLc);
    return r;
}

static S_odLc sfn_39(uint64_t a0, S_odLc a1, double a2, S_odLc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec(a1.m1);
    rec_d(a2);
    rec_d(a3.m0);
    rec(a3.m1);
    S_odLc r;
    st_result(&r, ST_odLc);
    return r;
}

static S_oLdc sfn_40(S_oLdc a0)
{
    enter();
    rec(a0.m0);
    rec_d(a0.m1);
    S_oLdc r;
    st_result(&r, ST_oLdc);
    return r;
}

static S_oLdc sfn_41(uint64_t a0, S_oLdc a1, double a2, S_oLdc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec_d(a1.m1);
    rec_d(a2);
    rec(a3.m0);
    rec_d(a3.m1);
    S_oLdc r;
    st_result(&r, ST_oLdc);
    return r;
}

static S_ooddcoddcc sfn_42(S_ooddcoddcc a0)
{
    enter();
    rec_d(a0.m0.m0);
    rec_d(a0.m0.m1);
    rec_d(a0.m1.m0);
    rec_d(a0.m1.m1);
    S_ooddcoddcc r;
    st_result(&r, ST_ooddcoddcc);
    return r;
}

static S_ooddcoddcc sfn_43(uint64_t a0, S_ooddcoddcc a1, double a2, S_ooddcoddcc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0.m0);
    rec_d(a1.m0.m1);
    rec_d(a1.m1.m0);
    rec_d(a1.m1.m1);
    rec_d(a2);
    rec_d(a3.m0.m0);
    rec_d(a3.m0.m1);
    rec_d(a3.m1.m0);
    rec_d(a3.m1.m1);
    S_ooddcoddcc r;
    st_result(&r, ST_ooddcoddcc);
    return r;
}

static S_oLLLc sfn_44(S_oLLLc a0)
{
    enter();
    rec(a0.m0);
    rec(a0.m1);
    rec(a0.m2);
    S_oLLLc r;
    st_result(&r, ST_oLLLc);
    return r;
}

static S_oLLLc sfn_45(uint64_t a0, S_oLLLc a1, double a2, S_oLLLc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec(a1.m1);
    rec(a1.m2);
    rec_d(a2);
    rec(a3.m0);
    rec(a3.m1);
    rec(a3.m2);
    S_oLLLc r;
    st_result(&r, ST_oLLLc);
    return r;
}

static S_obBhHiuc sfn_46(S_obBhHiuc a0)
{
    enter();
    rec_s8(a0.m0);
    rec_u8(a0.m1);
    rec_s16(a0.m2);
    rec_u16(a0.m3);
    rec((uint32_t)a0.m4);
    rec(a0.m5);
    S_obBhHiuc r;
    st_result(&r, ST_obBhHiuc);
    return r;
}

static S_obBhHiuc sfn_47(uint64_t a0, S_obBhHiuc a1, double a2, S_obBhHiuc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0);
    rec_u8(a1.m1);
    rec_s16(a1.m2);
    rec_u16(a1.m3);
    rec((uint32_t)a1.m4);
    rec(a1.m5);
    rec_d(a2);
    rec_s8(a3.m0);
    rec_u8(a3.m1);
    rec_s16(a3.m2);
    rec_u16(a3.m3);
    rec((uint32_t)a3.m4);
    rec(a3.m5);
    S_obBhHiuc r;
    st_result(&r, ST_obBhHiuc);
    return r;
}

static S_opdic sfn_48(S_opdic a0)
{
    enter();
    rec((uint64_t)(uintptr_t)a0.m0);
    rec_d(a0.m1);
    rec((uint32_t)a0.m2);
    S_opdic r;
    st_result(&r, ST_opdic);
    return r;
}

static S_opdic sfn_49(uint64_t a0, S_opdic a1, double a2, S_opdic a3)
{
    enter();
    rec(a0);
    rec((uint64_t)(uintptr_t)a1.m0);
    rec_d(a1.m1);
    rec((uint32_t)a1.m2);
    rec_d(a2);
    rec((uint64_t)(uintptr_t)a3.m0);
    rec_d(a3.m1);
    rec((uint32_t)a3.m2);
    S_opdic r;
    st_result(&r, ST_opdic);
    return r;
}

static S_obdc sfn_50(S_obdc a0)
{
    enter();
    rec_s8(a0.m0);
    rec_d(a0.m1);
    S_obdc r;
    st_result(&r, ST_obdc);
    return r;
}

static S_obdc sfn_51(uint64_t a0, S_obdc a1, double a2, S_obdc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0);
    rec_d(a1.m1);
    rec_d(a2);
    rec_s8(a3.m0);
    rec_d(a3.m1);
    S_obdc r;
    st_result(&r, ST_obdc);
    return r;
}

static S_obhbc sfn_52(S_obhbc a0)
{
    enter();
    rec_s8(a0.m0);
    rec_s16(a0.m1);
    rec_s8(a0.m2);
    S_obhbc r;
    st_result(&r, ST_obhbc);
    return r;
}

static S_obhbc sfn_53(uint64_t a0, S_obhbc a1, double a2, S_obhbc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0);
    rec_s16(a1.m1);
    rec_s8(a1.m2);
    rec_d(a2);
    rec_s8(a3.m0);
    rec_s16(a3.m1);
    rec_s8(a3.m2);
    S_obhbc r;
    st_result(&r, ST_obhbc);
    return r;
}

static S_oLbc sfn_54(S_oLbc a0)
{
    enter();
    rec(a0.m0);
    rec_s8(a0.m1);
    S_oLbc r;
    st_result(&r, ST_oLbc);
    return r;
}

static S_oLbc sfn_55(uint64_t a0, S_oLbc a1, double a2, S_oLbc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec_s8(a1.m1);
    rec_d(a2);
    rec(a3.m0);
    rec_s8(a3.m1);
    S_oLbc r;
    st_result(&r, ST_oLbc);
    return r;
}

static S_ofdc sfn_56(S_ofdc a0)
{
    enter();
    rec_f(a0.m0);
    rec_d(a0.m1);
    S_ofdc r;
    st_result(&r, ST_ofdc);
    return r;
}

static S_ofdc sfn_57(uint64_t a0, S_ofdc a1, double a2, S_ofdc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec_d(a1.m1);
    rec_d(a2);
    rec_f(a3.m0);
    rec_d(a3.m1);
    S_ofdc r;
    st_result(&r, ST_ofdc);
    return r;
}

static S_obfc sfn_58(S_obfc a0)
{
    enter();
    rec_s8(a0.m0);
    rec_f(a0.m1);
    S_obfc r;
    st_result(&r, ST_obfc);
    return r;
}

static S_obfc sfn_59(uint64_t a0, S_obfc a1, double a2, S_obfc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0);
    rec_f(a1.m1);
    rec_d(a2);
    rec_s8(a3.m0);
    rec_f(a3.m1);
    S_obfc r;
    st_result(&r, ST_obfc);
    return r;
}

static S_ofic sfn_60(S_ofic a0)
{
    enter();
    rec_f(a0.m0);
    rec((uint32_t)a0.m1);
    S_ofic r;
    st_result(&r, ST_ofic);
    return r;
}

static S_ofic sfn_61(uint64_t a0, S_ofic a1, double a2, S_ofic a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0);
    rec((uint32_t)a1.m1);
    rec_d(a2);
    rec_f(a3.m0);
    rec((uint32_t)a3.m1);
    S_ofic r;
    st_result(&r, ST_ofic);
    return r;
}

static S_oodbcbc sfn_62(S_oodbcbc a0)
{
    enter();
    rec_d(a0.m0.m0);
    rec_s8(a0.m0.m1);
    rec_s8(a0.m1);
    S_oodbcbc r;
    st_result(&r, ST_oodbcbc);
    return r;
}

static S_oodbcbc sfn_63(uint64_t a0, S_oodbcbc a1, double a2, S_oodbcbc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0.m0);
    rec_s8(a1.m0.m1);
    rec_s8(a1.m1);
    rec_d(a2);
    rec_d(a3.m0.m0);
    rec_s8(a3.m0.m1);
    rec_s8(a3.m1);
    S_oodbcbc r;
    st_result(&r, ST_oodbcbc);
    return r;
}

static S_oobhcic sfn_64(S_oobhcic a0)
{
    enter();
    rec_s8(a0.m0.m0);
    rec_s16(a0.m0.m1);
    rec((uint32_t)a0.m1);
    S_oobhcic r;
    st_result(&r, ST_oobhcic);
    return r;
}

static S_oobhcic sfn_65(uint64_t a0, S_oobhcic a1, double a2, S_oobhcic a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0.m0);
    rec_s16(a1.m0.m1);
    rec((uint32_t)a1.m1);
    rec_d(a2);
    rec_s8(a3.m0.m0);
    rec_s16(a3.m0.m1);
    rec((uint32_t)a3.m1);
    S_oobhcic r;
    st_result(&r, ST_oobhcic);
    return r;
}

static S_oBBBc sfn_66(S_oBBBc a0)
{
    enter();
    rec_u8(a0.m0);
    rec_u8(a0.m1);
    rec_u8(a0.m2);
    S_oBBBc r;
    st_result(&r, ST_oBBBc);
    return r;
}

static S_oBBBc sfn_67(uint64_t a0, S_oBBBc a1, double a2, S_oBBBc a3)
{
    enter();
    rec(a0);
    rec_u8(a1.m0);
    rec_u8(a1.m1);
    rec_u8(a1.m2);
    rec_d(a2);
    rec_u8(a3.m0);
    rec_u8(a3.m1);
    rec_u8(a3.m2);
    S_oBBBc r;
    st_result(&r, ST_oBBBc);
    return r;
}

static S_ooffcoddcc sfn_68(S_ooffcoddcc a0)
{
    enter();
    rec_f(a0.m0.m0);
    rec_f(a0.m0.m1);
    rec_d(a0.m1.m0);
    rec_d(a0.m1.m1);
    S_ooffcoddcc r;
    st_result(&r, ST_ooffcoddcc);
    return r;
}

static S_ooffcoddcc sfn_69(uint64_t a0, S_ooffcoddcc a1, double a2, S_ooffcoddcc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0.m0);
    rec_f(a1.m0.m1);
    rec_d(a1.m1.m0);
    rec_d(a1.m1.m1);
    rec_d(a2);
    rec_f(a3.m0.m0);
    rec_f(a3.m0.m1);
    rec_d(a3.m1.m0);
    rec_d(a3.m1.m1);
    S_ooffcoddcc r;
    st_result(&r, ST_ooffcoddcc);
    return r;
}

static S_oddddddc sfn_70(S_oddddddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a0.m4);
    rec_d(a0.m5);
    S_oddddddc r;
    st_result(&r, ST_oddddddc);
    return r;
}

static S_oddddddc sfn_71(uint64_t a0, S_oddddddc a1, double a2, S_oddddddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a1.m4);
    rec_d(a1.m5);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    rec_d(a3.m2);
    rec_d(a3.m3);
    rec_d(a3.m4);
    rec_d(a3.m5);
    S_oddddddc r;
    st_result(&r, ST_oddddddc);
    return r;
}

static S_oddddddddddddddddc sfn_72(S_oddddddddddddddddc a0)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a0.m4);
    rec_d(a0.m5);
    rec_d(a0.m6);
    rec_d(a0.m7);
    rec_d(a0.m8);
    rec_d(a0.m9);
    rec_d(a0.m10);
    rec_d(a0.m11);
    rec_d(a0.m12);
    rec_d(a0.m13);
    rec_d(a0.m14);
    rec_d(a0.m15);
    S_oddddddddddddddddc r;
    st_result(&r, ST_oddddddddddddddddc);
    return r;
}

static S_oddddddddddddddddc sfn_73(uint64_t a0, S_oddddddddddddddddc a1, double a2, S_oddddddddddddddddc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a1.m4);
    rec_d(a1.m5);
    rec_d(a1.m6);
    rec_d(a1.m7);
    rec_d(a1.m8);
    rec_d(a1.m9);
    rec_d(a1.m10);
    rec_d(a1.m11);
    rec_d(a1.m12);
    rec_d(a1.m13);
    rec_d(a1.m14);
    rec_d(a1.m15);
    rec_d(a2);
    rec_d(a3.m0);
    rec_d(a3.m1);
    rec_d(a3.m2);
    rec_d(a3.m3);
    rec_d(a3.m4);
    rec_d(a3.m5);
    rec_d(a3.m6);
    rec_d(a3.m7);
    rec_d(a3.m8);
    rec_d(a3.m9);
    rec_d(a3.m10);
    rec_d(a3.m11);
    rec_d(a3.m12);
    rec_d(a3.m13);
    rec_d(a3.m14);
    rec_d(a3.m15);
    S_oddddddddddddddddc r;
    st_result(&r, ST_oddddddddddddddddc);
    return r;
}

static S_oofcofcc sfn_74(S_oofcofcc a0)
{
    enter();
    rec_f(a0.m0.m0);
    rec_f(a0.m1.m0);
    S_oofcofcc r;
    st_result(&r, ST_oofcofcc);
    return r;
}

static S_oofcofcc sfn_75(uint64_t a0, S_oofcofcc a1, double a2, S_oofcofcc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0.m0);
    rec_f(a1.m1.m0);
    rec_d(a2);
    rec_f(a3.m0.m0);
    rec_f(a3.m1.m0);
    S_oofcofcc r;
    st_result(&r, ST_oofcofcc);
    return r;
}

static S_ooodccc sfn_76(S_ooodccc a0)
{
    enter();
    rec_d(a0.m0.m0.m0);
    S_ooodccc r;
    st_result(&r, ST_ooodccc);
    return r;
}

static S_ooodccc sfn_77(uint64_t a0, S_ooodccc a1, double a2, S_ooodccc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0.m0.m0);
    rec_d(a2);
    rec_d(a3.m0.m0.m0);
    S_ooodccc r;
    st_result(&r, ST_ooodccc);
    return r;
}

static S_oHuc sfn_78(S_oHuc a0)
{
    enter();
    rec_u16(a0.m0);
    rec(a0.m1);
    S_oHuc r;
    st_result(&r, ST_oHuc);
    return r;
}

static S_oHuc sfn_79(uint64_t a0, S_oHuc a1, double a2, S_oHuc a3)
{
    enter();
    rec(a0);
    rec_u16(a1.m0);
    rec(a1.m1);
    rec_d(a2);
    rec_u16(a3.m0);
    rec(a3.m1);
    S_oHuc r;
    st_result(&r, ST_oHuc);
    return r;
}

static S_olHc sfn_80(S_olHc a0)
{
    enter();
    rec((uint64_t)a0.m0);
    rec_u16(a0.m1);
    S_olHc r;
    st_result(&r, ST_olHc);
    return r;
}

static S_olHc sfn_81(uint64_t a0, S_olHc a1, double a2, S_olHc a3)
{
    enter();
    rec(a0);
    rec((uint64_t)a1.m0);
    rec_u16(a1.m1);
    rec_d(a2);
    rec((uint64_t)a3.m0);
    rec_u16(a3.m1);
    S_olHc r;
    st_result(&r, ST_olHc);
    return r;
}

static S_ooffcfc sfn_82(S_ooffcfc a0)
{
    enter();
    rec_f(a0.m0.m0);
    rec_f(a0.m0.m1);
    rec_f(a0.m1);
    S_ooffcfc r;
    st_result(&r, ST_ooffcfc);
    return r;
}

static S_ooffcfc sfn_83(uint64_t a0, S_ooffcfc a1, double a2, S_ooffcfc a3)
{
    enter();
    rec(a0);
    rec_f(a1.m0.m0);
    rec_f(a1.m0.m1);
    rec_f(a1.m1);
    rec_d(a2);
    rec_f(a3.m0.m0);
    rec_f(a3.m0.m1);
    rec_f(a3.m1);
    S_ooffcfc r;
    st_result(&r, ST_ooffcfc);
    return r;
}

static S_ohbc sfn_84(S_ohbc a0)
{
    enter();
    rec_s16(a0.m0);
    rec_s8(a0.m1);
    S_ohbc r;
    st_result(&r, ST_ohbc);
    return r;
}

static S_ohbc sfn_85(uint64_t a0, S_ohbc a1, double a2, S_ohbc a3)
{
    enter();
    rec(a0);
    rec_s16(a1.m0);
    rec_s8(a1.m1);
    rec_d(a2);
    rec_s16(a3.m0);
    rec_s8(a3.m1);
    S_ohbc r;
    st_result(&r, ST_ohbc);
    return r;
}

static S_oubc sfn_86(S_oubc a0)
{
    enter();
    rec(a0.m0);
    rec_s8(a0.m1);
    S_oubc r;
    st_result(&r, ST_oubc);
    return r;
}

static S_oubc sfn_87(uint64_t a0, S_oubc a1, double a2, S_oubc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec_s8(a1.m1);
    rec_d(a2);
    rec(a3.m0);
    rec_s8(a3.m1);
    S_oubc r;
    st_result(&r, ST_oubc);
    return r;
}

static S_obodcbodcbodcbodcbodcbodcbodcbodcc sfn_88(S_obodcbodcbodcbodcbodcbodcbodcbodcc a0)
{
    enter();
    rec_s8(a0.m0);
    rec_d(a0.m1.m0);
    rec_s8(a0.m2);
    rec_d(a0.m3.m0);
    rec_s8(a0.m4);
    rec_d(a0.m5.m0);
    rec_s8(a0.m6);
    rec_d(a0.m7.m0);
    rec_s8(a0.m8);
    rec_d(a0.m9.m0);
    rec_s8(a0.m10);
    rec_d(a0.m11.m0);
    rec_s8(a0.m12);
    rec_d(a0.m13.m0);
    rec_s8(a0.m14);
    rec_d(a0.m15.m0);
    S_obodcbodcbodcbodcbodcbodcbodcbodcc r;
    st_result(&r, ST_obodcbodcbodcbodcbodcbodcbodcbodcc);
    return r;
}

static S_obodcbodcbodcbodcbodcbodcbodcbodcc sfn_89(uint64_t a0, S_obodcbodcbodcbodcbodcbodcbodcbodcc a1, double a2, S_obodcbodcbodcbodcbodcbodcbodcbodcc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0);
    rec_d(a1.m1.m0);
    rec_s8(a1.m2);
    rec_d(a1.m3.m0);
    rec_s8(a1.m4);
    rec_d(a1.m5.m0);
    rec_s8(a1.m6);
    rec_d(a1.m7.m0);
    rec_s8(a1.m8);
    rec_d(a1.m9.m0);
    rec_s8(a1.m10);
    rec_d(a1.m11.m0);
    rec_s8(a1.m12);
    rec_d(a1.m13.m0);
    rec_s8(a1.m14);
    rec_d(a1.m15.m0);
    rec_d(a2);
    rec_s8(a3.m0);
    rec_d(a3.m1.m0);
    rec_s8(a3.m2);
    rec_d(a3.m3.m0);
    rec_s8(a3.m4);
    rec_d(a3.m5.m0);
    rec_s8(a3.m6);
    rec_d(a3.m7.m0);
    rec_s8(a3.m8);
    rec_d(a3.m9.m0);
    rec_s8(a3.m10);
    rec_d(a3.m11.m0);
    rec_s8(a3.m12);
    rec_d(a3.m13.m0);
    rec_s8(a3.m14);
    rec_d(a3.m15.m0);
    S_obodcbodcbodcbodcbodcbodcbodcbodcc r;
    st_result(&r, ST_obodcbodcbodcbodcbodcbodcbodcbodcc);
    return r;
}

static S_oooooooodcccccccc sfn_90(S_oooooooodcccccccc a0)
{
    enter();
    rec_d(a0.m0.m0.m0.m0.m0.m0.m0.m0);
    S_oooooooodcccccccc r;
    st_result(&r, ST_oooooooodcccccccc);
    return r;
}

static S_oooooooodcccccccc sfn_91(uint64_t a0, S_oooooooodcccccccc a1, double a2, S_oooooooodcccccccc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0.m0.m0.m0.m0.m0.m0.m0);
    rec_d(a2);
    rec_d(a3.m0.m0.m0.m0.m0.m0.m0.m0);
    S_oooooooodcccccccc r;
    st_result(&r, ST_oooooooodcccccccc);
    return r;
}

static S_oodbcodbcbc sfn_92(S_oodbcodbcbc a0)
{
    enter();
    rec_d(a0.m0.m0);
    rec_s8(a0.m0.m1);
    rec_d(a0.m1.m0);
    rec_s8(a0.m1.m1);
    rec_s8(a0.m2);
    S_oodbcodbcbc r;
    st_result(&r, ST_oodbcodbcbc);
    return r;
}

static S_oodbcodbcbc sfn_93(uint64_t a0, S_oodbcodbcbc a1, double a2, S_oodbcodbcbc a3)
{
    enter();
    rec(a0);
    rec_d(a1.m0.m0);
    rec_s8(a1.m0.m1);
    rec_d(a1.m1.m0);
    rec_s8(a1.m1.m1);
    rec_s8(a1.m2);
    rec_d(a2);
    rec_d(a3.m0.m0);
    rec_s8(a3.m0.m1);
    rec_d(a3.m1.m0);
    rec_s8(a3.m1.m1);
    rec_s8(a3.m2);
    S_oodbcodbcbc r;
    st_result(&r, ST_oodbcodbcbc);
    return r;
}

static S_oobhbcoLbcc sfn_94(S_oobhbcoLbcc a0)
{
    enter();
    rec_s8(a0.m0.m0);
    rec_s16(a0.m0.m1);
    rec_s8(a0.m0.m2);
    rec(a0.m1.m0);
    rec_s8(a0.m1.m1);
    S_oobhbcoLbcc r;
    st_result(&r, ST_oobhbcoLbcc);
    return r;
}

static S_oobhbcoLbcc sfn_95(uint64_t a0, S_oobhbcoLbcc a1, double a2, S_oobhbcoLbcc a3)
{
    enter();
    rec(a0);
    rec_s8(a1.m0.m0);
    rec_s16(a1.m0.m1);
    rec_s8(a1.m0.m2);
    rec(a1.m1.m0);
    rec_s8(a1.m1.m1);
    rec_d(a2);
    rec_s8(a3.m0.m0);
    rec_s16(a3.m0.m1);
    rec_s8(a3.m0.m2);
    rec(a3.m1.m0);
    rec_s8(a3.m1.m1);
    S_oobhbcoLbcc r;
    st_result(&r, ST_oobhbcoLbcc);
    return r;
}

static S_ouLbc sfn_96(S_ouLbc a0)
{
    enter();
    rec(a0.m0);
    rec(a0.m1);
    rec_s8(a0.m2);
    S_ouLbc r;
    st_result(&r, ST_ouLbc);
    return r;
}

static S_ouLbc sfn_97(uint64_t a0, S_ouLbc a1, double a2, S_ouLbc a3)
{
    enter();
    rec(a0);
    rec(a1.m0);
    rec(a1.m1);
    rec_s8(a1.m2);
    rec_d(a2);
    rec(a3.m0);
    rec(a3.m1);
    rec_s8(a3.m2);
    S_ouLbc r;
    st_result(&r, ST_ouLbc);
    return r;
}

static uint64_t sfn_98(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, S_oLLc a5, uint64_t a6)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5.m0);
    rec(a5.m1);
    rec(a6);
    return RES_LU;
}

static uint64_t sfn_99(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, S_oLLc a6, uint64_t a7)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6.m0);
    rec(a6.m1);
    rec(a7);
    return RES_LU;
}

static uint64_t sfn_100(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, S_oLLc a7, uint64_t a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7.m0);
    rec(a7.m1);
    rec(a8);
    return RES_LU;
}

static uint64_t sfn_101(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, S_oiLc a7, int32_t a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec((uint32_t)a7.m0);
    rec(a7.m1);
    rec((uint32_t)a8);
    return RES_LU;
}

static double sfn_102(double a0, double a1, double a2, double a3, double a4, double a5, double a6, S_oddc a7, double a8)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_d(a7.m0);
    rec_d(a7.m1);
    rec_d(a8);
    return res_d();
}

static double sfn_103(double a0, double a1, double a2, double a3, double a4, double a5, S_oddc a6, double a7)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6.m0);
    rec_d(a6.m1);
    rec_d(a7);
    return res_d();
}

static float sfn_104(double a0, double a1, double a2, double a3, double a4, double a5, S_offfc a6, float a7)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_f(a6.m0);
    rec_f(a6.m1);
    rec_f(a6.m2);
    rec_f(a7);
    return res_f();
}

static float sfn_105(double a0, double a1, double a2, double a3, double a4, double a5, double a6, S_offfc a7, float a8)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_f(a7.m0);
    rec_f(a7.m1);
    rec_f(a7.m2);
    rec_f(a8);
    return res_f();
}

static uint64_t sfn_106(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, S_odLc a5, uint64_t a6)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec_d(a5.m0);
    rec(a5.m1);
    rec(a6);
    return RES_LU;
}

static uint64_t sfn_107(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, S_odLc a6, double a7)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec_d(a6.m0);
    rec(a6.m1);
    rec_d(a7);
    return RES_LU;
}

static uint64_t sfn_108(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, S_odLc a8, uint64_t a9)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_d(a7);
    rec_d(a8.m0);
    rec(a8.m1);
    rec(a9);
    return RES_LU;
}

static uint64_t sfn_109(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, S_oBc a8, S_ohc a9, S_oic a10, S_oBc a11, int8_t a12, S_obhbc a13, S_ofic a14, S_oBBBc a15)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7);
    rec_u8(a8.m0);
    rec_s16(a9.m0);
    rec((uint32_t)a10.m0);
    rec_u8(a11.m0);
    rec_s8(a12);
    rec_s8(a13.m0);
    rec_s16(a13.m1);
    rec_s8(a13.m2);
    rec_f(a14.m0);
    rec((uint32_t)a14.m1);
    rec_u8(a15.m0);
    rec_u8(a15.m1);
    rec_u8(a15.m2);
    return RES_LU;
}

static uint64_t sfn_110(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, S_offc a8, S_ofc a9, S_oddc a10, float a11, S_offfc a12, int8_t a13, S_odc a14)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_d(a7);
    rec_f(a8.m0);
    rec_f(a8.m1);
    rec_f(a9.m0);
    rec_d(a10.m0);
    rec_d(a10.m1);
    rec_f(a11);
    rec_f(a12.m0);
    rec_f(a12.m1);
    rec_f(a12.m2);
    rec_s8(a13);
    rec_d(a14.m0);
    return RES_LU;
}

static uint64_t sfn_111(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, int32_t a8, S_oLic a9, int8_t a10, S_offfc a11, int8_t a12, S_odfc a13)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7);
    rec((uint32_t)a8);
    rec(a9.m0);
    rec((uint32_t)a9.m1);
    rec_s8(a10);
    rec_f(a11.m0);
    rec_f(a11.m1);
    rec_f(a11.m2);
    rec_s8(a12);
    rec_d(a13.m0);
    rec_f(a13.m1);
    return RES_LU;
}

static uint64_t sfn_112(S_oLLc a0, S_oLLc a1, S_oLLc a2, S_oLLc a3, S_oddddc a4, S_oddddc a5, int8_t a6, S_offc a7, int8_t a8, S_oddc a9, float a10)
{
    enter();
    rec(a0.m0);
    rec(a0.m1);
    rec(a1.m0);
    rec(a1.m1);
    rec(a2.m0);
    rec(a2.m1);
    rec(a3.m0);
    rec(a3.m1);
    rec_d(a4.m0);
    rec_d(a4.m1);
    rec_d(a4.m2);
    rec_d(a4.m3);
    rec_d(a5.m0);
    rec_d(a5.m1);
    rec_d(a5.m2);
    rec_d(a5.m3);
    rec_s8(a6);
    rec_f(a7.m0);
    rec_f(a7.m1);
    rec_s8(a8);
    rec_d(a9.m0);
    rec_d(a9.m1);
    rec_f(a10);
    return RES_LU;
}

static uint64_t sfn_113(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, S_oLLLc a7, uint64_t a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7.m0);
    rec(a7.m1);
    rec(a7.m2);
    rec(a8);
    return RES_LU;
}

static uint64_t sfn_114(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, int8_t a8, S_oLLLc a9, int8_t a10)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7);
    rec_s8(a8);
    rec(a9.m0);
    rec(a9.m1);
    rec(a9.m2);
    rec_s8(a10);
    return RES_LU;
}

static S_oLLLc sfn_115(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    S_oLLLc r;
    st_result(&r, ST_oLLLc);
    return r;
}

static S_oLLLc sfn_116(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, S_oLLLc a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec(a7);
    rec(a8.m0);
    rec(a8.m1);
    rec(a8.m2);
    S_oLLLc r;
    st_result(&r, ST_oLLLc);
    return r;
}

static S_oLLc sfn_117(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, S_oLLc a6)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6.m0);
    rec(a6.m1);
    S_oLLc r;
    st_result(&r, ST_oLLc);
    return r;
}

static S_ooddcoddcc sfn_118(S_ooddcoddcc a0, S_ooddcoddcc a1, double a2)
{
    enter();
    rec_d(a0.m0.m0);
    rec_d(a0.m0.m1);
    rec_d(a0.m1.m0);
    rec_d(a0.m1.m1);
    rec_d(a1.m0.m0);
    rec_d(a1.m0.m1);
    rec_d(a1.m1.m0);
    rec_d(a1.m1.m1);
    rec_d(a2);
    S_ooddcoddcc r;
    st_result(&r, ST_ooddcoddcc);
    return r;
}

static S_odddddc sfn_119(S_odddddc a0, uint64_t a1)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a0.m4);
    rec(a1);
    S_odddddc r;
    st_result(&r, ST_odddddc);
    return r;
}

static S_ooddcoddcc sfn_120(void * a0, void * a1, S_ooddcoddcc a2, void * a3)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    rec_d(a2.m0.m0);
    rec_d(a2.m0.m1);
    rec_d(a2.m1.m0);
    rec_d(a2.m1.m1);
    rec((uint64_t)(uintptr_t)a3);
    S_ooddcoddcc r;
    st_result(&r, ST_ooddcoddcc);
    return r;
}

static S_oLLc sfn_121(void * a0, void * a1, void * a2, uint64_t a3, S_oLLc a4)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    rec((uint64_t)(uintptr_t)a2);
    rec(a3);
    rec(a4.m0);
    rec(a4.m1);
    S_oLLc r;
    st_result(&r, ST_oLLc);
    return r;
}

static S_oddc sfn_122(void * a0, void * a1, S_oddc a2, void * a3)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    rec_d(a2.m0);
    rec_d(a2.m1);
    rec((uint64_t)(uintptr_t)a3);
    S_oddc r;
    st_result(&r, ST_oddc);
    return r;
}

static void sfn_123(void * a0, void * a1, S_ooddcoddcc a2)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    rec_d(a2.m0.m0);
    rec_d(a2.m0.m1);
    rec_d(a2.m1.m0);
    rec_d(a2.m1.m1);
}

static S_oddddddc sfn_124(void * a0, void * a1)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    S_oddddddc r;
    st_result(&r, ST_oddddddc);
    return r;
}

static S_oddddc sfn_125(void * a0, void * a1)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1);
    S_oddddc r;
    st_result(&r, ST_oddddc);
    return r;
}

static double sfn_126(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, S_odc a8, S_ofc a9, S_offc a10)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_d(a7);
    rec_d(a8.m0);
    rec_f(a9.m0);
    rec_f(a10.m0);
    rec_f(a10.m1);
    return res_d();
}

static S_offc sfn_127(float a0, float a1, float a2, float a3, float a4, float a5, S_offfc a6, float a7)
{
    enter();
    rec_f(a0);
    rec_f(a1);
    rec_f(a2);
    rec_f(a3);
    rec_f(a4);
    rec_f(a5);
    rec_f(a6.m0);
    rec_f(a6.m1);
    rec_f(a6.m2);
    rec_f(a7);
    S_offc r;
    st_result(&r, ST_offc);
    return r;
}

static S_ofic sfn_128(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, S_ofic a6, S_ofic a7)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec_f(a6.m0);
    rec((uint32_t)a6.m1);
    rec_f(a7.m0);
    rec((uint32_t)a7.m1);
    S_ofic r;
    st_result(&r, ST_ofic);
    return r;
}

static S_obfc sfn_129(int8_t a0, S_obfc a1, S_ofdc a2)
{
    enter();
    rec_s8(a0);
    rec_s8(a1.m0);
    rec_f(a1.m1);
    rec_f(a2.m0);
    rec_d(a2.m1);
    S_obfc r;
    st_result(&r, ST_obfc);
    return r;
}

static S_oLdc sfn_130(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, S_oLdc a8, S_odLc a9, uint64_t a10)
{
    enter();
    rec_d(a0);
    rec_d(a1);
    rec_d(a2);
    rec_d(a3);
    rec_d(a4);
    rec_d(a5);
    rec_d(a6);
    rec_d(a7);
    rec(a8.m0);
    rec_d(a8.m1);
    rec_d(a9.m0);
    rec(a9.m1);
    rec(a10);
    S_oLdc r;
    st_result(&r, ST_oLdc);
    return r;
}

static S_odLc sfn_131(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, S_odLc a6, S_oLdc a7, double a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec_d(a6.m0);
    rec(a6.m1);
    rec(a7.m0);
    rec_d(a7.m1);
    rec_d(a8);
    S_odLc r;
    st_result(&r, ST_odLc);
    return r;
}

static S_odfc sfn_132(S_odfc a0, S_odfc a1, S_odfc a2, S_odfc a3, S_odfc a4)
{
    enter();
    rec_d(a0.m0);
    rec_f(a0.m1);
    rec_d(a1.m0);
    rec_f(a1.m1);
    rec_d(a2.m0);
    rec_f(a2.m1);
    rec_d(a3.m0);
    rec_f(a3.m1);
    rec_d(a4.m0);
    rec_f(a4.m1);
    S_odfc r;
    st_result(&r, ST_odfc);
    return r;
}

static S_oBc sfn_133(int8_t a0, uint8_t a1, int16_t a2, uint16_t a3, S_oBc a4, S_ohc a5, S_obhbc a6, int32_t a7, uint32_t a8)
{
    enter();
    rec_s8(a0);
    rec_u8(a1);
    rec_s16(a2);
    rec_u16(a3);
    rec_u8(a4.m0);
    rec_s16(a5.m0);
    rec_s8(a6.m0);
    rec_s16(a6.m1);
    rec_s8(a6.m2);
    rec((uint32_t)a7);
    rec(a8);
    S_oBc r;
    st_result(&r, ST_oBc);
    return r;
}

static S_oddddddddddddddddc sfn_134(S_oddddddddddddddddc a0, S_oddddddddddddddddc a1)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a0.m4);
    rec_d(a0.m5);
    rec_d(a0.m6);
    rec_d(a0.m7);
    rec_d(a0.m8);
    rec_d(a0.m9);
    rec_d(a0.m10);
    rec_d(a0.m11);
    rec_d(a0.m12);
    rec_d(a0.m13);
    rec_d(a0.m14);
    rec_d(a0.m15);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a1.m4);
    rec_d(a1.m5);
    rec_d(a1.m6);
    rec_d(a1.m7);
    rec_d(a1.m8);
    rec_d(a1.m9);
    rec_d(a1.m10);
    rec_d(a1.m11);
    rec_d(a1.m12);
    rec_d(a1.m13);
    rec_d(a1.m14);
    rec_d(a1.m15);
    S_oddddddddddddddddc r;
    st_result(&r, ST_oddddddddddddddddc);
    return r;
}

static double sfn_135(S_oddddc a0, S_oddddc a1, double a2, S_odc a3)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a0.m3);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a1.m3);
    rec_d(a2);
    rec_d(a3.m0);
    return res_d();
}

static double sfn_136(S_odddc a0, S_odddc a1, S_oddc a2, double a3)
{
    enter();
    rec_d(a0.m0);
    rec_d(a0.m1);
    rec_d(a0.m2);
    rec_d(a1.m0);
    rec_d(a1.m1);
    rec_d(a1.m2);
    rec_d(a2.m0);
    rec_d(a2.m1);
    rec_d(a3);
    return res_d();
}

static float sfn_137(S_offffc a0, S_offffc a1, S_ofc a2, float a3)
{
    enter();
    rec_f(a0.m0);
    rec_f(a0.m1);
    rec_f(a0.m2);
    rec_f(a0.m3);
    rec_f(a1.m0);
    rec_f(a1.m1);
    rec_f(a1.m2);
    rec_f(a1.m3);
    rec_f(a2.m0);
    rec_f(a3);
    return res_f();
}

static S_opdic sfn_138(void * a0, S_opdic a1, void * a2, S_opc a3)
{
    enter();
    rec((uint64_t)(uintptr_t)a0);
    rec((uint64_t)(uintptr_t)a1.m0);
    rec_d(a1.m1);
    rec((uint32_t)a1.m2);
    rec((uint64_t)(uintptr_t)a2);
    rec((uint64_t)(uintptr_t)a3.m0);
    S_opdic r;
    st_result(&r, ST_opdic);
    return r;
}

static S_oodbcbc sfn_139(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, S_oodbcbc a7, S_oodbcbc a8)
{
    enter();
    rec(a0);
    rec(a1);
    rec(a2);
    rec(a3);
    rec(a4);
    rec(a5);
    rec(a6);
    rec_d(a7.m0.m0);
    rec_s8(a7.m0.m1);
    rec_s8(a7.m1);
    rec_d(a8.m0.m0);
    rec_s8(a8.m0.m1);
    rec_s8(a8.m1);
    S_oodbcbc r;
    st_result(&r, ST_oodbcbc);
    return r;
}

static uint16_t sfn_140(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8, S_oHuc a9, S_ohbc a10, int16_t a11)
{
    enter();
    rec_f(a0);
    rec_f(a1);
    rec_f(a2);
    rec_f(a3);
    rec_f(a4);
    rec_f(a5);
    rec_f(a6);
    rec_f(a7);
    rec_f(a8);
    rec_u16(a9.m0);
    rec(a9.m1);
    rec_s16(a10.m0);
    rec_s8(a10.m1);
    rec_s16(a11);
    return RES_U16;
}

static const StCase kStCases[] = {
    { "{B}({B})", FN(sfn_0), '{', ST_oBc, 1, { '{' }, { ST_oBc } },
    { "{B}(L{B}d{B})", FN(sfn_1), '{', ST_oBc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oBc, -1, ST_oBc } },
    { "{h}({h})", FN(sfn_2), '{', ST_ohc, 1, { '{' }, { ST_ohc } },
    { "{h}(L{h}d{h})", FN(sfn_3), '{', ST_ohc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ohc, -1, ST_ohc } },
    { "{i}({i})", FN(sfn_4), '{', ST_oic, 1, { '{' }, { ST_oic } },
    { "{i}(L{i}d{i})", FN(sfn_5), '{', ST_oic, 4, { 'L', '{', 'd', '{' }, { -1, ST_oic, -1, ST_oic } },
    { "{L}({L})", FN(sfn_6), '{', ST_oLc, 1, { '{' }, { ST_oLc } },
    { "{L}(L{L}d{L})", FN(sfn_7), '{', ST_oLc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLc, -1, ST_oLc } },
    { "{p}({p})", FN(sfn_8), '{', ST_opc, 1, { '{' }, { ST_opc } },
    { "{p}(L{p}d{p})", FN(sfn_9), '{', ST_opc, 4, { 'L', '{', 'd', '{' }, { -1, ST_opc, -1, ST_opc } },
    { "{LL}({LL})", FN(sfn_10), '{', ST_oLLc, 1, { '{' }, { ST_oLLc } },
    { "{LL}(L{LL}d{LL})", FN(sfn_11), '{', ST_oLLc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLLc, -1, ST_oLLc } },
    { "{iL}({iL})", FN(sfn_12), '{', ST_oiLc, 1, { '{' }, { ST_oiLc } },
    { "{iL}(L{iL}d{iL})", FN(sfn_13), '{', ST_oiLc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oiLc, -1, ST_oiLc } },
    { "{Li}({Li})", FN(sfn_14), '{', ST_oLic, 1, { '{' }, { ST_oLic } },
    { "{Li}(L{Li}d{Li})", FN(sfn_15), '{', ST_oLic, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLic, -1, ST_oLic } },
    { "{f}({f})", FN(sfn_16), '{', ST_ofc, 1, { '{' }, { ST_ofc } },
    { "{f}(L{f}d{f})", FN(sfn_17), '{', ST_ofc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ofc, -1, ST_ofc } },
    { "{ff}({ff})", FN(sfn_18), '{', ST_offc, 1, { '{' }, { ST_offc } },
    { "{ff}(L{ff}d{ff})", FN(sfn_19), '{', ST_offc, 4, { 'L', '{', 'd', '{' }, { -1, ST_offc, -1, ST_offc } },
    { "{fff}({fff})", FN(sfn_20), '{', ST_offfc, 1, { '{' }, { ST_offfc } },
    { "{fff}(L{fff}d{fff})", FN(sfn_21), '{', ST_offfc, 4, { 'L', '{', 'd', '{' }, { -1, ST_offfc, -1, ST_offfc } },
    { "{ffff}({ffff})", FN(sfn_22), '{', ST_offffc, 1, { '{' }, { ST_offffc } },
    { "{ffff}(L{ffff}d{ffff})", FN(sfn_23), '{', ST_offffc, 4, { 'L', '{', 'd', '{' }, { -1, ST_offffc, -1, ST_offffc } },
    { "{fffff}({fffff})", FN(sfn_24), '{', ST_offfffc, 1, { '{' }, { ST_offfffc } },
    { "{fffff}(L{fffff}d{fffff})", FN(sfn_25), '{', ST_offfffc, 4, { 'L', '{', 'd', '{' }, { -1, ST_offfffc, -1, ST_offfffc } },
    { "{d}({d})", FN(sfn_26), '{', ST_odc, 1, { '{' }, { ST_odc } },
    { "{d}(L{d}d{d})", FN(sfn_27), '{', ST_odc, 4, { 'L', '{', 'd', '{' }, { -1, ST_odc, -1, ST_odc } },
    { "{dd}({dd})", FN(sfn_28), '{', ST_oddc, 1, { '{' }, { ST_oddc } },
    { "{dd}(L{dd}d{dd})", FN(sfn_29), '{', ST_oddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oddc, -1, ST_oddc } },
    { "{ddd}({ddd})", FN(sfn_30), '{', ST_odddc, 1, { '{' }, { ST_odddc } },
    { "{ddd}(L{ddd}d{ddd})", FN(sfn_31), '{', ST_odddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_odddc, -1, ST_odddc } },
    { "{dddd}({dddd})", FN(sfn_32), '{', ST_oddddc, 1, { '{' }, { ST_oddddc } },
    { "{dddd}(L{dddd}d{dddd})", FN(sfn_33), '{', ST_oddddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oddddc, -1, ST_oddddc } },
    { "{ddddd}({ddddd})", FN(sfn_34), '{', ST_odddddc, 1, { '{' }, { ST_odddddc } },
    { "{ddddd}(L{ddddd}d{ddddd})", FN(sfn_35), '{', ST_odddddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_odddddc, -1, ST_odddddc } },
    { "{df}({df})", FN(sfn_36), '{', ST_odfc, 1, { '{' }, { ST_odfc } },
    { "{df}(L{df}d{df})", FN(sfn_37), '{', ST_odfc, 4, { 'L', '{', 'd', '{' }, { -1, ST_odfc, -1, ST_odfc } },
    { "{dL}({dL})", FN(sfn_38), '{', ST_odLc, 1, { '{' }, { ST_odLc } },
    { "{dL}(L{dL}d{dL})", FN(sfn_39), '{', ST_odLc, 4, { 'L', '{', 'd', '{' }, { -1, ST_odLc, -1, ST_odLc } },
    { "{Ld}({Ld})", FN(sfn_40), '{', ST_oLdc, 1, { '{' }, { ST_oLdc } },
    { "{Ld}(L{Ld}d{Ld})", FN(sfn_41), '{', ST_oLdc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLdc, -1, ST_oLdc } },
    { "{{dd}{dd}}({{dd}{dd}})", FN(sfn_42), '{', ST_ooddcoddcc, 1, { '{' }, { ST_ooddcoddcc } },
    { "{{dd}{dd}}(L{{dd}{dd}}d{{dd}{dd}})", FN(sfn_43), '{', ST_ooddcoddcc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ooddcoddcc, -1, ST_ooddcoddcc } },
    { "{LLL}({LLL})", FN(sfn_44), '{', ST_oLLLc, 1, { '{' }, { ST_oLLLc } },
    { "{LLL}(L{LLL}d{LLL})", FN(sfn_45), '{', ST_oLLLc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLLLc, -1, ST_oLLLc } },
    { "{bBhHiu}({bBhHiu})", FN(sfn_46), '{', ST_obBhHiuc, 1, { '{' }, { ST_obBhHiuc } },
    { "{bBhHiu}(L{bBhHiu}d{bBhHiu})", FN(sfn_47), '{', ST_obBhHiuc, 4, { 'L', '{', 'd', '{' }, { -1, ST_obBhHiuc, -1, ST_obBhHiuc } },
    { "{pdi}({pdi})", FN(sfn_48), '{', ST_opdic, 1, { '{' }, { ST_opdic } },
    { "{pdi}(L{pdi}d{pdi})", FN(sfn_49), '{', ST_opdic, 4, { 'L', '{', 'd', '{' }, { -1, ST_opdic, -1, ST_opdic } },
    { "{bd}({bd})", FN(sfn_50), '{', ST_obdc, 1, { '{' }, { ST_obdc } },
    { "{bd}(L{bd}d{bd})", FN(sfn_51), '{', ST_obdc, 4, { 'L', '{', 'd', '{' }, { -1, ST_obdc, -1, ST_obdc } },
    { "{bhb}({bhb})", FN(sfn_52), '{', ST_obhbc, 1, { '{' }, { ST_obhbc } },
    { "{bhb}(L{bhb}d{bhb})", FN(sfn_53), '{', ST_obhbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_obhbc, -1, ST_obhbc } },
    { "{Lb}({Lb})", FN(sfn_54), '{', ST_oLbc, 1, { '{' }, { ST_oLbc } },
    { "{Lb}(L{Lb}d{Lb})", FN(sfn_55), '{', ST_oLbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oLbc, -1, ST_oLbc } },
    { "{fd}({fd})", FN(sfn_56), '{', ST_ofdc, 1, { '{' }, { ST_ofdc } },
    { "{fd}(L{fd}d{fd})", FN(sfn_57), '{', ST_ofdc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ofdc, -1, ST_ofdc } },
    { "{bf}({bf})", FN(sfn_58), '{', ST_obfc, 1, { '{' }, { ST_obfc } },
    { "{bf}(L{bf}d{bf})", FN(sfn_59), '{', ST_obfc, 4, { 'L', '{', 'd', '{' }, { -1, ST_obfc, -1, ST_obfc } },
    { "{fi}({fi})", FN(sfn_60), '{', ST_ofic, 1, { '{' }, { ST_ofic } },
    { "{fi}(L{fi}d{fi})", FN(sfn_61), '{', ST_ofic, 4, { 'L', '{', 'd', '{' }, { -1, ST_ofic, -1, ST_ofic } },
    { "{{db}b}({{db}b})", FN(sfn_62), '{', ST_oodbcbc, 1, { '{' }, { ST_oodbcbc } },
    { "{{db}b}(L{{db}b}d{{db}b})", FN(sfn_63), '{', ST_oodbcbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oodbcbc, -1, ST_oodbcbc } },
    { "{{bh}i}({{bh}i})", FN(sfn_64), '{', ST_oobhcic, 1, { '{' }, { ST_oobhcic } },
    { "{{bh}i}(L{{bh}i}d{{bh}i})", FN(sfn_65), '{', ST_oobhcic, 4, { 'L', '{', 'd', '{' }, { -1, ST_oobhcic, -1, ST_oobhcic } },
    { "{BBB}({BBB})", FN(sfn_66), '{', ST_oBBBc, 1, { '{' }, { ST_oBBBc } },
    { "{BBB}(L{BBB}d{BBB})", FN(sfn_67), '{', ST_oBBBc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oBBBc, -1, ST_oBBBc } },
    { "{{ff}{dd}}({{ff}{dd}})", FN(sfn_68), '{', ST_ooffcoddcc, 1, { '{' }, { ST_ooffcoddcc } },
    { "{{ff}{dd}}(L{{ff}{dd}}d{{ff}{dd}})", FN(sfn_69), '{', ST_ooffcoddcc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ooffcoddcc, -1, ST_ooffcoddcc } },
    { "{dddddd}({dddddd})", FN(sfn_70), '{', ST_oddddddc, 1, { '{' }, { ST_oddddddc } },
    { "{dddddd}(L{dddddd}d{dddddd})", FN(sfn_71), '{', ST_oddddddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oddddddc, -1, ST_oddddddc } },
    { "{dddddddddddddddd}({dddddddddddddddd})", FN(sfn_72), '{', ST_oddddddddddddddddc, 1, { '{' }, { ST_oddddddddddddddddc } },
    { "{dddddddddddddddd}(L{dddddddddddddddd}d{dddddddddddddddd})", FN(sfn_73), '{', ST_oddddddddddddddddc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oddddddddddddddddc, -1, ST_oddddddddddddddddc } },
    { "{{f}{f}}({{f}{f}})", FN(sfn_74), '{', ST_oofcofcc, 1, { '{' }, { ST_oofcofcc } },
    { "{{f}{f}}(L{{f}{f}}d{{f}{f}})", FN(sfn_75), '{', ST_oofcofcc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oofcofcc, -1, ST_oofcofcc } },
    { "{{{d}}}({{{d}}})", FN(sfn_76), '{', ST_ooodccc, 1, { '{' }, { ST_ooodccc } },
    { "{{{d}}}(L{{{d}}}d{{{d}}})", FN(sfn_77), '{', ST_ooodccc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ooodccc, -1, ST_ooodccc } },
    { "{Hu}({Hu})", FN(sfn_78), '{', ST_oHuc, 1, { '{' }, { ST_oHuc } },
    { "{Hu}(L{Hu}d{Hu})", FN(sfn_79), '{', ST_oHuc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oHuc, -1, ST_oHuc } },
    { "{lH}({lH})", FN(sfn_80), '{', ST_olHc, 1, { '{' }, { ST_olHc } },
    { "{lH}(L{lH}d{lH})", FN(sfn_81), '{', ST_olHc, 4, { 'L', '{', 'd', '{' }, { -1, ST_olHc, -1, ST_olHc } },
    { "{{ff}f}({{ff}f})", FN(sfn_82), '{', ST_ooffcfc, 1, { '{' }, { ST_ooffcfc } },
    { "{{ff}f}(L{{ff}f}d{{ff}f})", FN(sfn_83), '{', ST_ooffcfc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ooffcfc, -1, ST_ooffcfc } },
    { "{hb}({hb})", FN(sfn_84), '{', ST_ohbc, 1, { '{' }, { ST_ohbc } },
    { "{hb}(L{hb}d{hb})", FN(sfn_85), '{', ST_ohbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ohbc, -1, ST_ohbc } },
    { "{ub}({ub})", FN(sfn_86), '{', ST_oubc, 1, { '{' }, { ST_oubc } },
    { "{ub}(L{ub}d{ub})", FN(sfn_87), '{', ST_oubc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oubc, -1, ST_oubc } },
    { "{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}}({b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}})", FN(sfn_88), '{', ST_obodcbodcbodcbodcbodcbodcbodcbodcc, 1, { '{' }, { ST_obodcbodcbodcbodcbodcbodcbodcbodcc } },
    { "{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}}(L{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}}d{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}})", FN(sfn_89), '{', ST_obodcbodcbodcbodcbodcbodcbodcbodcc, 4, { 'L', '{', 'd', '{' }, { -1, ST_obodcbodcbodcbodcbodcbodcbodcbodcc, -1, ST_obodcbodcbodcbodcbodcbodcbodcbodcc } },
    { "{{{{{{{{d}}}}}}}}({{{{{{{{d}}}}}}}})", FN(sfn_90), '{', ST_oooooooodcccccccc, 1, { '{' }, { ST_oooooooodcccccccc } },
    { "{{{{{{{{d}}}}}}}}(L{{{{{{{{d}}}}}}}}d{{{{{{{{d}}}}}}}})", FN(sfn_91), '{', ST_oooooooodcccccccc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oooooooodcccccccc, -1, ST_oooooooodcccccccc } },
    { "{{db}{db}b}({{db}{db}b})", FN(sfn_92), '{', ST_oodbcodbcbc, 1, { '{' }, { ST_oodbcodbcbc } },
    { "{{db}{db}b}(L{{db}{db}b}d{{db}{db}b})", FN(sfn_93), '{', ST_oodbcodbcbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oodbcodbcbc, -1, ST_oodbcodbcbc } },
    { "{{bhb}{Lb}}({{bhb}{Lb}})", FN(sfn_94), '{', ST_oobhbcoLbcc, 1, { '{' }, { ST_oobhbcoLbcc } },
    { "{{bhb}{Lb}}(L{{bhb}{Lb}}d{{bhb}{Lb}})", FN(sfn_95), '{', ST_oobhbcoLbcc, 4, { 'L', '{', 'd', '{' }, { -1, ST_oobhbcoLbcc, -1, ST_oobhbcoLbcc } },
    { "{uLb}({uLb})", FN(sfn_96), '{', ST_ouLbc, 1, { '{' }, { ST_ouLbc } },
    { "{uLb}(L{uLb}d{uLb})", FN(sfn_97), '{', ST_ouLbc, 4, { 'L', '{', 'd', '{' }, { -1, ST_ouLbc, -1, ST_ouLbc } },
    { "L(LLLLL{LL}L)", FN(sfn_98), 'L', -1, 7, { 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, ST_oLLc, -1 } },
    { "L(LLLLLL{LL}L)", FN(sfn_99), 'L', -1, 8, { 'L', 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, -1, ST_oLLc, -1 } },
    { "L(LLLLLLL{LL}L)", FN(sfn_100), 'L', -1, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, -1, -1, ST_oLLc, -1 } },
    { "L(LLLLLLL{iL}i)", FN(sfn_101), 'L', -1, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', 'i' }, { -1, -1, -1, -1, -1, -1, -1, ST_oiLc, -1 } },
    { "d(ddddddd{dd}d)", FN(sfn_102), 'd', -1, 9, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', 'd' }, { -1, -1, -1, -1, -1, -1, -1, ST_oddc, -1 } },
    { "d(dddddd{dd}d)", FN(sfn_103), 'd', -1, 8, { 'd', 'd', 'd', 'd', 'd', 'd', '{', 'd' }, { -1, -1, -1, -1, -1, -1, ST_oddc, -1 } },
    { "f(dddddd{fff}f)", FN(sfn_104), 'f', -1, 8, { 'd', 'd', 'd', 'd', 'd', 'd', '{', 'f' }, { -1, -1, -1, -1, -1, -1, ST_offfc, -1 } },
    { "f(ddddddd{fff}f)", FN(sfn_105), 'f', -1, 9, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', 'f' }, { -1, -1, -1, -1, -1, -1, -1, ST_offfc, -1 } },
    { "L(LLLLL{dL}L)", FN(sfn_106), 'L', -1, 7, { 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, ST_odLc, -1 } },
    { "L(LLLLLL{dL}d)", FN(sfn_107), 'L', -1, 8, { 'L', 'L', 'L', 'L', 'L', 'L', '{', 'd' }, { -1, -1, -1, -1, -1, -1, ST_odLc, -1 } },
    { "L(dddddddd{dL}L)", FN(sfn_108), 'L', -1, 10, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', 'L' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_odLc, -1 } },
    { "L(LLLLLLLL{B}{h}{i}{B}b{bhb}{fi}{BBB})", FN(sfn_109), 'L', -1, 16, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', '{', '{', '{', 'b', '{', '{', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oBc, ST_ohc, ST_oic, ST_oBc, -1, ST_obhbc, ST_ofic, ST_oBBBc } },
    { "L(dddddddd{ff}{f}{dd}f{fff}b{d})", FN(sfn_110), 'L', -1, 15, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', '{', '{', 'f', '{', 'b', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_offc, ST_ofc, ST_oddc, -1, ST_offfc, -1, ST_odc } },
    { "L(LLLLLLLLi{Li}b{fff}b{df})", FN(sfn_111), 'L', -1, 14, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'i', '{', 'b', '{', 'b', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, ST_oLic, -1, ST_offfc, -1, ST_odfc } },
    { "L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f)", FN(sfn_112), 'L', -1, 11, { '{', '{', '{', '{', '{', '{', 'b', '{', 'b', '{', 'f' }, { ST_oLLc, ST_oLLc, ST_oLLc, ST_oLLc, ST_oddddc, ST_oddddc, -1, ST_offc, -1, ST_oddc, -1 } },
    { "L(LLLLLLL{LLL}L)", FN(sfn_113), 'L', -1, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', 'L' }, { -1, -1, -1, -1, -1, -1, -1, ST_oLLLc, -1 } },
    { "L(LLLLLLLLb{LLL}b)", FN(sfn_114), 'L', -1, 11, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'b', '{', 'b' }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, ST_oLLLc, -1 } },
    { "{LLL}(LLLLLL)", FN(sfn_115), '{', ST_oLLLc, 6, { 'L', 'L', 'L', 'L', 'L', 'L' }, { -1, -1, -1, -1, -1, -1 } },
    { "{LLL}(LLLLLLLL{LLL})", FN(sfn_116), '{', ST_oLLLc, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oLLLc } },
    { "{LL}(LLLLLL{LL})", FN(sfn_117), '{', ST_oLLc, 7, { 'L', 'L', 'L', 'L', 'L', 'L', '{' }, { -1, -1, -1, -1, -1, -1, ST_oLLc } },
    { "{{dd}{dd}}({{dd}{dd}}{{dd}{dd}}d)", FN(sfn_118), '{', ST_ooddcoddcc, 3, { '{', '{', 'd' }, { ST_ooddcoddcc, ST_ooddcoddcc, -1 } },
    { "{ddddd}({ddddd}L)", FN(sfn_119), '{', ST_odddddc, 2, { '{', 'L' }, { ST_odddddc, -1 } },
    { "{{dd}{dd}}(pp{{dd}{dd}}p)", FN(sfn_120), '{', ST_ooddcoddcc, 4, { 'p', 'p', '{', 'p' }, { -1, -1, ST_ooddcoddcc, -1 } },
    { "{LL}(pppL{LL})", FN(sfn_121), '{', ST_oLLc, 5, { 'p', 'p', 'p', 'L', '{' }, { -1, -1, -1, -1, ST_oLLc } },
    { "{dd}(pp{dd}p)", FN(sfn_122), '{', ST_oddc, 4, { 'p', 'p', '{', 'p' }, { -1, -1, ST_oddc, -1 } },
    { "v(pp{{dd}{dd}})", FN(sfn_123), 'v', -1, 3, { 'p', 'p', '{' }, { -1, -1, ST_ooddcoddcc } },
    { "{dddddd}(pp)", FN(sfn_124), '{', ST_oddddddc, 2, { 'p', 'p' }, { -1, -1 } },
    { "{dddd}(pp)", FN(sfn_125), '{', ST_oddddc, 2, { 'p', 'p' }, { -1, -1 } },
    { "d(dddddddd{d}{f}{ff})", FN(sfn_126), 'd', -1, 11, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', '{', '{' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_odc, ST_ofc, ST_offc } },
    { "{ff}(ffffff{fff}f)", FN(sfn_127), '{', ST_offc, 8, { 'f', 'f', 'f', 'f', 'f', 'f', '{', 'f' }, { -1, -1, -1, -1, -1, -1, ST_offfc, -1 } },
    { "{fi}(LLLLLL{fi}{fi})", FN(sfn_128), '{', ST_ofic, 8, { 'L', 'L', 'L', 'L', 'L', 'L', '{', '{' }, { -1, -1, -1, -1, -1, -1, ST_ofic, ST_ofic } },
    { "{bf}(b{bf}{fd})", FN(sfn_129), '{', ST_obfc, 3, { 'b', '{', '{' }, { -1, ST_obfc, ST_ofdc } },
    { "{Ld}(dddddddd{Ld}{dL}L)", FN(sfn_130), '{', ST_oLdc, 11, { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', '{', '{', 'L' }, { -1, -1, -1, -1, -1, -1, -1, -1, ST_oLdc, ST_odLc, -1 } },
    { "{dL}(LLLLLL{dL}{Ld}d)", FN(sfn_131), '{', ST_odLc, 9, { 'L', 'L', 'L', 'L', 'L', 'L', '{', '{', 'd' }, { -1, -1, -1, -1, -1, -1, ST_odLc, ST_oLdc, -1 } },
    { "{df}({df}{df}{df}{df}{df})", FN(sfn_132), '{', ST_odfc, 5, { '{', '{', '{', '{', '{' }, { ST_odfc, ST_odfc, ST_odfc, ST_odfc, ST_odfc } },
    { "{B}(bBhH{B}{h}{bhb}iu)", FN(sfn_133), '{', ST_oBc, 9, { 'b', 'B', 'h', 'H', '{', '{', '{', 'i', 'u' }, { -1, -1, -1, -1, ST_oBc, ST_ohc, ST_obhbc, -1, -1 } },
    { "{dddddddddddddddd}({dddddddddddddddd}{dddddddddddddddd})", FN(sfn_134), '{', ST_oddddddddddddddddc, 2, { '{', '{' }, { ST_oddddddddddddddddc, ST_oddddddddddddddddc } },
    { "d({dddd}{dddd}d{d})", FN(sfn_135), 'd', -1, 4, { '{', '{', 'd', '{' }, { ST_oddddc, ST_oddddc, -1, ST_odc } },
    { "d({ddd}{ddd}{dd}d)", FN(sfn_136), 'd', -1, 4, { '{', '{', '{', 'd' }, { ST_odddc, ST_odddc, ST_oddc, -1 } },
    { "f({ffff}{ffff}{f}f)", FN(sfn_137), 'f', -1, 4, { '{', '{', '{', 'f' }, { ST_offffc, ST_offffc, ST_ofc, -1 } },
    { "{pdi}(p{pdi}p{p})", FN(sfn_138), '{', ST_opdic, 4, { 'p', '{', 'p', '{' }, { -1, ST_opdic, -1, ST_opc } },
    { "{{db}b}(LLLLLLL{{db}b}{{db}b})", FN(sfn_139), '{', ST_oodbcbc, 9, { 'L', 'L', 'L', 'L', 'L', 'L', 'L', '{', '{' }, { -1, -1, -1, -1, -1, -1, -1, ST_oodbcbc, ST_oodbcbc } },
    { "H(fffffffff{Hu}{hb}h)", FN(sfn_140), 'H', -1, 12, { 'f', 'f', 'f', 'f', 'f', 'f', 'f', 'f', 'f', '{', '{', 'h' }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, ST_oHuc, ST_ohbc, -1 } },
};
#define NSTCASES (sizeof kStCases / sizeof kStCases[0])

static const StCase *st_find(const char *sig)
{
    size_t i;

    for (i = 0; i < NSTCASES; i++)
        if (strcmp(kStCases[i].sig, sig) == 0)
            return &kStCases[i];
    return NULL;
}

static uint64_t st_word(const uint8_t *bytes, size_t at)
{
    uint64_t w;

    memcpy(&w, bytes + at, 8);
    return w;
}

static uint64_t st_setup(OcerzCPU *cpu, const StCase *c, int edge, uint64_t *want,
                         int *nwant)
{
    int ni = 0, nf = 0, ne = 0, i, m, k;
    uint64_t sp;

    ocerz_cpu_reset(cpu);
    cpu->mxcsr = MXCSR_DEFAULT;
    for (i = 0; i < 16; i++)
        cpu->gpr[i] = garbage64(0x40 + i);
    cpu->gpr[OCERZ_RAX] = RAX_POISON;
    for (i = 0; i < 16; i++) {
        cpu->xmm[i].lo = XMM_LO_POISON + (uint64_t)i;
        cpu->xmm[i].hi = XMM_HI_POISON + (uint64_t)i;
    }

    g_nslot = 0;
    *nwant = 0;
    g_st_ret_buf = 0;
    if (c->ret == '{' && kStTypes[c->ret_type].size > 16) {
        g_st_ret_buf = g_scratch + ST_RET_OFF;
        for (k = 0; k < ST_RET_SPAN; k++)
            ocerz_st(g_st_ret_buf + (uint64_t)k, 1, (uint8_t)(0xe1 ^ k));
        cpu->gpr[kIntReg[ni++]] = g_st_ret_buf;
    }

    for (i = 0; i < c->nargs; i++) {
        char a = c->arg[i];

        if (a != '{') {
            uint64_t bits = member_bits(a, i, 15, edge, &ne), raw = bits;

            if (a == 'f' || a == 'i' || a == 'u')
                raw = (garbage32(i) << 32) | bits;
            else if (a == 'b' || a == 'B')
                raw = garbage_above(i, 8) | bits;
            else if (a == 'h' || a == 'H')
                raw = garbage_above(i, 16) | bits;
            want[(*nwant)++] = extend(a, bits);
            g_st_host[i] = a == 'i' ? (uint64_t)(int64_t)(int32_t)bits : extend(a, bits);
            if (is_fp(a) ? nf < 8 : ni < 6) {
                if (is_fp(a)) {
                    cpu->xmm[nf].lo = raw;
                    cpu->xmm[nf].hi = garbage64(i);
                    nf++;
                } else {
                    cpu->gpr[kIntReg[ni++]] = raw;
                }
            } else {
                g_slot[g_nslot++] = raw;
            }
            continue;
        }

        const StType *t = &kStTypes[c->type[i]];
        uint8_t *bytes = g_st_bytes[i];
        uint8_t placed[OCERZ_ABI_STRUCT_BYTES];
        int words = (int)((t->size + 7) / 8);
        char cls[2];
        int n = st_sysv_class(t, cls), need_i = 0, need_s = 0;

        memset(bytes, 0, OCERZ_ABI_STRUCT_BYTES);
        for (m = 0; m < t->n; m++) {
            uint64_t bits = member_bits(t->cls[m], i, m, edge, &ne);
            want[(*nwant)++] = extend(t->cls[m], bits);
            memcpy(bytes + t->off[m], &bits, cls_size(t->cls[m]));
        }
        memcpy(placed, bytes, sizeof placed);
        for (k = (int)t->size; k < words * 8; k++)
            placed[k] = (uint8_t)(0xb0 | k);

        for (k = 0; k < n; k++) {
            if (cls[k] == 'I')
                need_i++;
            else
                need_s++;
        }
        if (n && ni + need_i <= 6 && nf + need_s <= 8) {
            for (k = 0; k < n; k++) {
                if (cls[k] == 'I') {
                    cpu->gpr[kIntReg[ni++]] = st_word(placed, 8 * (size_t)k);
                } else {
                    cpu->xmm[nf].lo = st_word(placed, 8 * (size_t)k);
                    cpu->xmm[nf].hi = garbage64(i + k);
                    nf++;
                }
            }
        } else {
            for (k = 0; k < words; k++)
                g_slot[g_nslot++] = st_word(placed, 8 * (size_t)k);
        }
    }

    sp = g_scratch + ST_STACK_OFF - 8;
    ocerz_st(sp, 8, RET_ADDR);
    for (i = 0; i < g_nslot; i++)
        ocerz_st(sp + 8 + 8ull * (uint64_t)i, 8, g_slot[i]);
    for (i = g_nslot; i < g_nslot + 4; i++)
        ocerz_st(sp + 8 + 8ull * (uint64_t)i, 8, 0xfeedc0de00000000ull + (uint64_t)i);

    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = TRAP_RIP;
    return sp;
}

static void st_check_layout(const char *sig, const char *what, const OcerzAbiStruct *got,
                            const StType *t)
{
    int m;

    CHECK(got->size == t->size && got->align == t->align && got->nmember == t->n,
          "%s: %s parsed as %u bytes aligned to %u with %u members, but clang lays %s "
          "out as %zu bytes aligned to %zu with %d", sig, what, (unsigned)got->size,
          (unsigned)got->align, (unsigned)got->nmember, t->notation, t->size, t->align,
          t->n);
    for (m = 0; m < t->n && m < got->nmember && m < OCERZ_ABI_STRUCT_MEMBERS; m++)
        CHECK(got->member[m] == t->cls[m] && got->offset[m] == t->off[m],
              "%s: %s member %d parsed as '%c' at offset %u, but clang puts '%c' at %u",
              sig, what, m, got->member[m] ? got->member[m] : '?',
              (unsigned)got->offset[m], t->cls[m], (unsigned)t->off[m]);
}

static void st_check_result(const StCase *c, const OcerzCPU *cpu, int edge)
{
    const StType *t;
    uint8_t want[OCERZ_ABI_STRUCT_BYTES], got[ST_RET_SPAN];
    char cls[2];
    int n, k, m, ri = 0, si = 0;

    if (c->ret != '{') {
        AbiCase ac = { c->sig, c->fn, edge, 0 };
        check_result(&ac, cpu);
        return;
    }

    t = &kStTypes[c->ret_type];
    st_result(want, c->ret_type);
    memset(got, 0, sizeof got);
    n = st_sysv_class(t, cls);

    if (!n) {
        CHECK(cpu->gpr[OCERZ_RAX] == g_st_ret_buf,
              "%s: rax is %#llx after returning a MEMORY structure, want the result "
              "pointer %#llx the guest passed in rdi", c->sig,
              (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)g_st_ret_buf);
        for (k = 0; k < ST_RET_SPAN; k++)
            got[k] = (uint8_t)ocerz_ld(g_st_ret_buf + (uint64_t)k, 1);
        for (k = (int)t->size; k < ST_RET_SPAN; k++)
            if (got[k] != (uint8_t)(0xe1 ^ k))
                break;
        CHECK(k == ST_RET_SPAN,
              "%s: the crossing wrote byte %d of the guest's result space, past the "
              "%zu-byte structure", c->sig, k, t->size);
    } else {
        for (k = 0; k < n; k++) {
            uint64_t w;

            if (cls[k] == 'I') {
                w = cpu->gpr[ri++ == 0 ? OCERZ_RAX : OCERZ_RDX];
            } else {
                CHECK(cpu->xmm[si].hi == 0,
                      "%s: xmm%d's upper half is %#llx after a structure result, want "
                      "zero", c->sig, si, (unsigned long long)cpu->xmm[si].hi);
                w = cpu->xmm[si].lo;
                si++;
            }
            memcpy(got + 8 * k, &w, 8);
        }
    }

    for (m = 0; m < t->n; m++) {
        uint64_t g = 0, w = 0;
        size_t size = cls_size(t->cls[m]);

        memcpy(&g, got + t->off[m], size);
        memcpy(&w, want + t->off[m], size);
        CHECK(g == w,
              "%s: result member %d ('%c' at offset %u) reached the guest as %#llx, "
              "want %#llx (%s)", c->sig, m, t->cls[m], (unsigned)t->off[m],
              (unsigned long long)g, (unsigned long long)w,
              n == 0 ? "through the result pointer" : n == 1 ? "in one register" : "in two registers");
    }
}

static void st_run(const StCase *c, int edge)
{
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiSig sig;
    uint64_t want[REC_MAX], sp;
    char what[32];
    int nwant, r, i;

    memset(&sig, 0xa5, sizeof sig);
    r = ocerz_abi_parse(c->sig, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d, want OCERZ_OK", c->sig, r);
    if (r != OCERZ_OK)
        return;
    CHECK(sig.ret == c->ret && sig.nargs == c->nargs,
          "%s: parsed result '%c' and %d arguments, want '%c' and %d", c->sig,
          sig.ret ? sig.ret : '?', sig.nargs, c->ret, c->nargs);
    if (sig.nargs != c->nargs)
        return;
    if (c->ret == '{')
        st_check_layout(c->sig, "the result", &sig.ret_struct, &kStTypes[c->ret_type]);
    for (i = 0; i < c->nargs; i++) {
        CHECK(sig.arg[i] == c->arg[i], "%s: parsed argument %d as '%c', want '%c'",
              c->sig, i, sig.arg[i] ? sig.arg[i] : '?', c->arg[i]);
        if (c->arg[i] == '{') {
            snprintf(what, sizeof what, "argument %d", i);
            st_check_layout(c->sig, what, &sig.arg_struct[i], &kStTypes[c->type[i]]);
        }
    }

    memset(g_rec, 0, sizeof g_rec);
    g_nrec = -1;
    g_entered = 0;
    sp = st_setup(cpu, c, edge, want, &nwant);
    ocerz_apply_mxcsr_round(cpu->mxcsr);

    r = ocerz_abi_perform(&sig, fnptr(c->fn), cpu);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_perform returned %d, want OCERZ_OK", c->sig, r);
    CHECK(g_entered == 1, "%s: the host function ran %d times, want once", c->sig,
          g_entered);
    if (g_entered != 1)
        return;

    CHECK(g_nrec == nwant, "%s: the host function recorded %d values, want %d", c->sig,
          g_nrec, nwant);
    for (i = 0; i < nwant && i < g_nrec && i < REC_MAX; i++)
        CHECK(g_rec[i] == want[i],
              "%s%s: value %d the host function read is %#llx, want %#llx", c->sig,
              edge ? " with edge values" : "", i, (unsigned long long)g_rec[i],
              (unsigned long long)want[i]);

    st_check_result(c, cpu, edge);

    CHECK(cpu->rip == RET_ADDR && cpu->gpr[OCERZ_RSP] == sp + 8,
          "%s: rip %#llx rsp %#llx after the crossing, want %#llx and %#llx", c->sig,
          (unsigned long long)cpu->rip, (unsigned long long)cpu->gpr[OCERZ_RSP],
          (unsigned long long)RET_ADDR, (unsigned long long)(sp + 8));
    for (i = 0; i < g_nslot; i++) {
        uint64_t got = ocerz_ld(sp + 8 + 8ull * (uint64_t)i, 8);
        CHECK(got == g_slot[i],
              "%s: the crossing changed guest stack slot %d from %#llx to %#llx", c->sig,
              i, (unsigned long long)g_slot[i], (unsigned long long)got);
    }
}

static void test_structs(void)
{
    size_t i;

    for (i = 0; i < NSTCASES; i++) {
        st_run(&kStCases[i], 0);
        st_run(&kStCases[i], 1);
    }
}

typedef struct FramePut {
    char kind;
    int at;
    int arg;
    int part;
} FramePut;

static void st_check_copy(const char *sig, const StCase *c, int arg, uint64_t ptr,
                          const OcerzAbiCall *call)
{
    const StType *t = &kStTypes[c->type[arg]];
    uint64_t lo = (uint64_t)(uintptr_t)call, hi = lo + sizeof *call;

    CHECK(ptr >= lo && ptr + t->size <= hi,
          "%s: argument %d reaches the host as the address %#llx, which is not a copy "
          "inside the OcerzAbiCall [%#llx, %#llx), and an arm64 callee may write to "
          "its copy", sig, arg, (unsigned long long)ptr, (unsigned long long)lo,
          (unsigned long long)hi);
    if (ptr >= lo && ptr + t->size <= hi)
        CHECK(memcmp((const void *)(uintptr_t)ptr, g_st_bytes[arg], t->size) == 0,
              "%s: the copy of argument %d at %#llx does not hold the structure's bytes",
              sig, arg, (unsigned long long)ptr);
}

static void st_frame(const char *notation, const FramePut *put, int nput, int nstack)
{
    const StCase *c = st_find(notation);
    OcerzCPU *cpu = &g_cpu;
    OcerzAbiCall *call = &g_frame_call;
    OcerzAbiSig sig;
    uint64_t want[REC_MAX];
    uint8_t expect[sizeof call->stack];
    const uint8_t *got;
    int nwant, r, k;

    CHECK(c != NULL, "%s: the frame table names a signature with no generated case",
          notation);
    if (!c)
        return;
    r = ocerz_abi_parse(c->sig, &sig);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_parse returned %d", notation, r);
    if (r != OCERZ_OK)
        return;

    st_setup(cpu, c, 0, want, &nwant);
    memset(call, 0x5a, sizeof *call);
    r = ocerz_abi_read_guest(&sig, cpu, call);
    CHECK(r == OCERZ_OK, "%s: ocerz_abi_read_guest returned %d", notation, r);
    if (r != OCERZ_OK)
        return;

    memset(expect, 0, sizeof expect);
    for (k = 0; k < nput; k++) {
        const FramePut *p = &put[k];
        const StType *t = c->arg[p->arg] == '{' ? &kStTypes[c->type[p->arg]] : NULL;
        const uint8_t *bytes = g_st_bytes[p->arg];
        uint64_t w = 0;

        switch (p->kind) {
        case 'x':
            w = t ? st_word(bytes, 8 * (size_t)p->part) : g_st_host[p->arg];
            CHECK(call->x[p->at] == w,
                  "%s: x%d holds %#llx, want %#llx from argument %d (word %d)", notation,
                  p->at, (unsigned long long)call->x[p->at], (unsigned long long)w,
                  p->arg, p->part);
            break;
        case 'v':
            if (t)
                memcpy(&w, bytes + t->off[p->part], cls_size(t->cls[p->part]));
            else
                w = g_st_host[p->arg];
            CHECK(call->v[p->at] == w,
                  "%s: v%d holds %#llx, want %#llx from argument %d (member %d)", notation,
                  p->at, (unsigned long long)call->v[p->at], (unsigned long long)w,
                  p->arg, p->part);
            break;
        case 's':
            if (t) {
                memcpy(expect + p->at, bytes, st_hfa(t) ? t->size : (t->size + 7) / 8 * 8);
            } else {
                w = g_st_host[p->arg];
                memcpy(expect + p->at, &w, cls_size(c->arg[p->arg]));
            }
            break;
        case 'P':
            st_check_copy(notation, c, p->arg, call->x[p->at], call);
            break;
        case 'Q':
            memcpy(&w, (const uint8_t *)call->stack + p->at, 8);
            st_check_copy(notation, c, p->arg, w, call);
            memcpy(expect + p->at, &w, 8);
            break;
        default:
            CHECK(0, "%s: the frame table has a placement of kind '%c'", notation, p->kind);
            break;
        }
    }

    CHECK(call->nstack == nstack, "%s: the host side stacked %d eightbytes, want %d",
          notation, call->nstack, nstack);
    got = (const uint8_t *)call->stack;
    for (k = 0; k < (int)sizeof expect; k++)
        CHECK(got[k] == expect[k],
              "%s: host stack byte %d is %#x, want %#x (Apple stacks a small structure as "
              "whole words at 8, an aggregate at its member's size, and zero elsewhere)",
              notation, k, got[k], expect[k]);

    if (c->ret == '{' && kStTypes[c->ret_type].size > 16 && !st_hfa(&kStTypes[c->ret_type]))
        CHECK(call->x8 == (void *)call->ret,
              "%s: x8 is %p, want the OcerzAbiCall's own result buffer %p", notation,
              call->x8, (void *)call->ret);
    else
        CHECK(call->x8 == NULL, "%s: x8 is %p for a result that does not come back through it",
              notation, call->x8);
}

#define FPUT(k, at, arg, part) { k, at, arg, part }

static void test_struct_frames(void)
{
    static const FramePut kLL[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 1, 1, 0), FPUT('x', 2, 2, 0), FPUT('x', 3, 3, 0),
        FPUT('x', 4, 4, 0), FPUT('x', 5, 5, 0), FPUT('x', 6, 6, 0),
        FPUT('s', 0, 7, 0), FPUT('s', 16, 8, 0),
    };
    static const FramePut kiL[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 6, 6, 0), FPUT('s', 0, 7, 0), FPUT('s', 16, 8, 0),
    };
    static const FramePut kdd[] = {
        FPUT('v', 0, 0, 0), FPUT('v', 6, 6, 0), FPUT('s', 0, 7, 0), FPUT('s', 16, 8, 0),
    };
    static const FramePut kfff[] = {
        FPUT('v', 0, 0, 0), FPUT('v', 5, 5, 0), FPUT('s', 0, 6, 0), FPUT('s', 12, 7, 0),
    };
    static const FramePut kSmall[] = {
        FPUT('x', 7, 7, 0),
        FPUT('s', 0, 8, 0), FPUT('s', 8, 9, 0), FPUT('s', 16, 10, 0), FPUT('s', 24, 11, 0),
        FPUT('s', 32, 12, 0), FPUT('s', 40, 13, 0), FPUT('s', 48, 14, 0), FPUT('s', 56, 15, 0),
    };
    static const FramePut kHfa[] = {
        FPUT('v', 7, 7, 0),
        FPUT('s', 0, 8, 0), FPUT('s', 8, 9, 0), FPUT('s', 16, 10, 0), FPUT('s', 32, 11, 0),
        FPUT('s', 36, 12, 0), FPUT('x', 0, 13, 0), FPUT('s', 48, 14, 0),
    };
    static const FramePut kMix[] = {
        FPUT('x', 7, 7, 0), FPUT('s', 0, 8, 0), FPUT('s', 8, 9, 0), FPUT('s', 24, 10, 0),
        FPUT('v', 0, 11, 0), FPUT('v', 1, 11, 1), FPUT('v', 2, 11, 2),
        FPUT('s', 25, 12, 0), FPUT('s', 32, 13, 0),
    };
    static const FramePut kBig[] = {
        FPUT('x', 7, 7, 0), FPUT('s', 0, 8, 0), FPUT('Q', 8, 9, 0), FPUT('s', 16, 10, 0),
    };
    static const FramePut kBigReg[] = {
        FPUT('x', 6, 6, 0), FPUT('P', 7, 7, 0), FPUT('s', 0, 8, 0),
    };
    static const FramePut kBigRet[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 7, 7, 0), FPUT('Q', 0, 8, 0),
    };
    static const FramePut kRects[] = {
        FPUT('v', 0, 0, 0), FPUT('v', 1, 0, 1), FPUT('v', 2, 0, 2), FPUT('v', 3, 0, 3),
        FPUT('v', 4, 1, 0), FPUT('v', 5, 1, 1), FPUT('v', 6, 1, 2), FPUT('v', 7, 1, 3),
        FPUT('s', 0, 2, 0),
    };
    static const FramePut kdL6[] = {
        FPUT('x', 5, 5, 0), FPUT('x', 6, 6, 0), FPUT('x', 7, 6, 1), FPUT('v', 0, 7, 0),
    };
    static const FramePut kdL5[] = {
        FPUT('x', 4, 4, 0), FPUT('x', 5, 5, 0), FPUT('x', 6, 5, 1), FPUT('x', 7, 6, 0),
    };
    static const FramePut kRange[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 1, 1, 0), FPUT('x', 2, 2, 0), FPUT('x', 3, 3, 0),
        FPUT('x', 4, 4, 0), FPUT('x', 5, 4, 1),
    };
    static const FramePut kBoth[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 1, 0, 1), FPUT('x', 2, 1, 0), FPUT('x', 3, 1, 1),
        FPUT('x', 4, 2, 0), FPUT('x', 5, 2, 1), FPUT('x', 6, 3, 0), FPUT('x', 7, 3, 1),
        FPUT('v', 0, 4, 0), FPUT('v', 3, 4, 3), FPUT('v', 4, 5, 0), FPUT('v', 7, 5, 3),
        FPUT('s', 0, 6, 0), FPUT('s', 4, 7, 0), FPUT('s', 12, 8, 0), FPUT('s', 16, 9, 0),
        FPUT('s', 32, 10, 0),
    };
    static const FramePut kOneEach[] = {
        FPUT('v', 7, 7, 0), FPUT('s', 0, 8, 0), FPUT('s', 8, 9, 0), FPUT('s', 12, 10, 0),
    };
    static const FramePut kFloats[] = {
        FPUT('v', 5, 5, 0), FPUT('s', 0, 6, 0), FPUT('s', 12, 7, 0),
    };
    static const FramePut kFive[] = {
        FPUT('P', 0, 0, 0), FPUT('x', 1, 1, 0),
    };
    static const FramePut kPdi[] = {
        FPUT('x', 0, 0, 0), FPUT('P', 1, 1, 0), FPUT('x', 2, 2, 0), FPUT('x', 3, 3, 0),
    };
    static const FramePut kTiny[] = {
        FPUT('x', 0, 0, 0), FPUT('x', 1, 1, 0), FPUT('x', 2, 2, 0), FPUT('x', 3, 3, 0),
        FPUT('x', 4, 4, 0), FPUT('x', 5, 5, 0), FPUT('x', 6, 6, 0), FPUT('x', 7, 7, 0),
        FPUT('s', 0, 8, 0),
    };

#define FRAME(sig, table, nstack) st_frame(sig, table, (int)(sizeof table / sizeof table[0]), nstack)
    FRAME("L(LLLLLLL{LL}L)", kLL, 3);
    FRAME("L(LLLLLLL{iL}i)", kiL, 3);
    FRAME("d(ddddddd{dd}d)", kdd, 3);
    FRAME("f(dddddd{fff}f)", kfff, 2);
    FRAME("L(LLLLLLLL{B}{h}{i}{B}b{bhb}{fi}{BBB})", kSmall, 8);
    FRAME("L(dddddddd{ff}{f}{dd}f{fff}b{d})", kHfa, 7);
    FRAME("L(LLLLLLLLi{Li}b{fff}b{df})", kMix, 6);
    FRAME("L(LLLLLLLLb{LLL}b)", kBig, 3);
    FRAME("L(LLLLLLL{LLL}L)", kBigReg, 1);
    FRAME("{LLL}(LLLLLLLL{LLL})", kBigRet, 1);
    FRAME("{{dd}{dd}}({{dd}{dd}}{{dd}{dd}}d)", kRects, 1);
    FRAME("L(LLLLLL{dL}d)", kdL6, 0);
    FRAME("L(LLLLL{dL}L)", kdL5, 0);
    FRAME("{LL}(pppL{LL})", kRange, 0);
    FRAME("L({LL}{LL}{LL}{LL}{dddd}{dddd}b{ff}b{dd}f)", kBoth, 5);
    FRAME("d(dddddddd{d}{f}{ff})", kOneEach, 3);
    FRAME("{ff}(ffffff{fff}f)", kFloats, 2);
    FRAME("{ddddd}({ddddd}L)", kFive, 0);
    FRAME("{pdi}(p{pdi}p{p})", kPdi, 0);
    FRAME("{B}(bBhH{B}{h}{bhb}iu)", kTiny, 1);
#undef FRAME
}

static void test_struct_parse(void)
{
    static const char *const kAccept[] = {
        "{d}()", "v({d})", "{{{{{{{{d}}}}}}}}({{{{{{{{d}}}}}}}})",
        "v({LLLLLLLLLLLLLLLL})", "{{dddddddd}{dddddddd}}()",
        "{b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}}(p)", "v(c{v({dd})}{dd})",
        "{bBhHiulLpfd}({bBhHiulLpfd})",
    };
    static const char *const kReject[] = {
        "{}()", "v({})", "v({{}})", "v({d{}})", "v({{}d})", "{}(d)",
        "v({dd)", "v({dd", "{dd(v)", "{dd", "{", "v({", "v({{dd})", "{{dd}(p)",
        "v({dv})", "v({vd})", "v({v})", "{v}()", "v({{v}})",
        "v({c})", "v({dc})", "v({c{v()}})", "{c}()", "v({{c}})",
        "v({LLLLLLLLLLLLLLLLL})", "v({{LLLLLLLL}{LLLLLLLLL}})",
        "{{dddddddd}{ddddddddd}}()", "v({b{d}b{d}b{d}b{d}b{d}b{d}b{d}b{d}b})",
        "v({{{{{{{{{d}}}}}}}}})", "{{{{{{{{{d}}}}}}}}}()",
        "v({d}})", "v(})", "}(d)", "{d}}(d)", "v({d)}", "v({(d)})",
        "v({d,d})", "v({ d})", "v({s})", "v({x})", "v({D})", "{dd}", "{dd}()x",
        "v({[d]})", "v([d])", "v({d}[d])", "{d}{d}()", "v({d}}{d})",
    };
    size_t i;

    for (i = 0; i < sizeof kAccept / sizeof kAccept[0]; i++) {
        OcerzAbiSig sig;
        int r;

        memset(&sig, 0xa5, sizeof sig);
        r = ocerz_abi_parse(kAccept[i], &sig);
        CHECK(r == OCERZ_OK, "ocerz_abi_parse(\"%s\") returned %d, want OCERZ_OK",
              kAccept[i], r);
    }
    for (i = 0; i < sizeof kReject / sizeof kReject[0]; i++) {
        OcerzAbiSig sig;
        int r;

        memset(&sig, 0xa5, sizeof sig);
        r = ocerz_abi_parse(kReject[i], &sig);
        CHECK(r != OCERZ_OK,
              "ocerz_abi_parse(\"%s\") accepted a structure notation that is not well "
              "formed", kReject[i]);
    }
}

static int report(void)
{
    printf("test_abi: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    static const uint64_t kDerefWord = 0x0123456789abcdefull;
    static const AbiCase kDeref = { "L(p)", FN(fn_DEREF), 0, 0 };
    uint64_t scratch;
    size_t i;

    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }
    scratch = ocerz_map_anywhere(SCRATCH, PROT_READ | PROT_WRITE);
    if (scratch == 0) {
        fprintf(stderr, "scratch alloc failed\n");
        return 2;
    }

    g_scratch = scratch;
    g_ptr_base = scratch + 0x100;
    g_stack_base = (scratch + SCRATCH - 0x200) & ~0xfull;
    g_res_ptr = ocerz_g2h(scratch + 0x80);

    for (i = 0; i < NCASES; i++)
        run_case(&kCases[i]);

    ocerz_st(g_ptr_base + 0x40, 8, kDerefWord);
    run_case(&kDeref);
    CHECK(g_deref == kDerefWord,
          "L(p): the host function read %#llx through its pointer argument, "
          "want %#llx", (unsigned long long)g_deref,
          (unsigned long long)kDerefWord);

    test_host_registers();
    test_host_stack();
    test_narrow_native();
    test_accept_classes();
    test_rounding();
    test_reject_parse();
    test_reject_perform();
    test_structs();
    test_struct_frames();
    test_struct_parse();

    return report();
}
