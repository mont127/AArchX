/* 8/16-bit register ops: shl/shr/sar by immediate (incl. counts >= width),
 * not/neg, and add/sub/and/or/xor with a memory source; results and flags. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const g_u64 vals[6] = { 0x1122334455667788ULL, 0x00000000000000ffULL, 0x0000000000008001ULL, 0xffffffffffffff80ULL, 0x0123456789ab7f00ULL, 0 };
    volatile unsigned char m8 = 0x5a; volatile unsigned short m16 = 0xa55a;
    g_u64 r, f;
    for (int i = 0; i < 6; i++) {
        g_u64 v = vals[i];
        /* OF is architecturally undefined for shift counts other than 1: masked out there */
#define SH8(insn, cnt, tag, mask) r = v; __asm__ volatile(insn " $" #cnt ", %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) :: "cc"); put(tag " r ", r); put(tag " f ", f & (mask));
#define SH16(insn, cnt, tag, mask) r = v; __asm__ volatile(insn " $" #cnt ", %w0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) :: "cc"); put(tag " r ", r); put(tag " f ", f & (mask));
        SH8("shlb", 1, "shl8_1", 0x8d5) SH8("shlb", 7, "shl8_7", 0xd5) SH8("shlb", 9, "shl8_9", 0xd5) SH8("shrb", 1, "shr8_1", 0x8d5) SH8("shrb", 3, "shr8_3", 0xd5) SH8("shrb", 8, "shr8_8", 0xd5) SH8("sarb", 1, "sar8_1", 0x8d5) SH8("sarb", 2, "sar8_2", 0xd5) SH8("sarb", 15, "sar8_15", 0xd5)
        SH16("shlw", 1, "shl16_1", 0x8d5) SH16("shlw", 4, "shl16_4", 0xd5) SH16("shlw", 17, "shl16_17", 0xd5) SH16("shrw", 9, "shr16_9", 0xd5) SH16("sarw", 1, "sar16_1", 0x8d5) SH16("sarw", 31, "sar16_31", 0xd5)
        r = v; __asm__ volatile("notb %b0" : "+q"(r)); put("not8 ", r);
        r = v; __asm__ volatile("notw %w0" : "+r"(r)); put("not16 ", r);
        r = v; __asm__ volatile("negb %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) :: "cc"); put("neg8 r ", r); put("neg8 f ", f & 0x8d5);
        r = v; __asm__ volatile("negw %w0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) :: "cc"); put("neg16 r ", r); put("neg16 f ", f & 0x8d5);
        /* narrow ALU with a memory source, dead and live flags */
        r = v; __asm__ volatile("addb %1, %b0" : "+q"(r) : "m"(m8)); put("add8m ", r);
        r = v; __asm__ volatile("orb %2, %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) : "m"(m8) : "cc"); put("or8m r ", r); put("or8m f ", f & 0x8d5);
        r = v; __asm__ volatile("subw %2, %w0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "m"(m16) : "cc"); put("sub16m r ", r); put("sub16m f ", f & 0x8d5);
        r = v; __asm__ volatile("xorw %1, %w0" : "+r"(r) : "m"(m16)); put("xor16m ", r);
        r = v; __asm__ volatile("andb %2, %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) : "m"(m8) : "cc"); put("and8m r ", r); put("and8m f ", f & 0x8d5);
    }
    return 0;
}
