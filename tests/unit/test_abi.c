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

#define REC_MAX 20

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
    "s()", "s(s)", "S(i)", "i(s)", "i(S)", "{}()", "i({ii})", "{ii}(i)",
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

    return report();
}
