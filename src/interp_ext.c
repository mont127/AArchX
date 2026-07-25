/*
 * src/interp_ext.c
 *
 * The "extended" interpreter translation unit: every op that interp.c does
 * not handle directly and that is not an SSE op. ocerz_interp_ext() is the
 * entry point named in interp.h; for any op it does not own it returns
 * OCERZ_EUNSUP so interp.c can fall through to its own fatal diagnostic.
 * Every SSE op (>= OCERZ_OP_SSE_FIRST) is explicitly NOT ours and returns
 * OCERZ_EUNSUP — interp.c routes those to ocerz_interp_sse().
 *
 * The instruction has already had cpu->rip advanced by insn->len before we
 * are called (the RIP protocol in decode.h), so handlers see the
 * architectural next-rip; none of the ops here branch, so we never touch
 * cpu->rip.
 *
 * Op families implemented here:
 *
 *  - String ops MOVS/STOS/LODS/SCAS/CMPS. The full REP loop runs inside one
 *    call. Element width is insn->opsize and the direction comes from DF.
 *    The count register is RCX read/written as ecx when insn->addrsize==4,
 *    full 64-bit otherwise; RSI/RDI advance the same way. REP repeats
 *    unconditionally for MOVS/STOS/LODS; REPE/REPNE for SCAS/CMPS terminate
 *    on ZF mismatch with the rep kind. CMPS/SCAS compute flags via
 *    ocerz_flags_sub on the LAST executed iteration only (a no-iteration REP
 *    leaves flags untouched, which is architectural).
 *
 *  - Bit ops BT/BTS/BTR/BTC and bit scans. For a register destination the
 *    bit index is masked to the operand width. For a MEMORY destination with
 *    a REGISTER bit offset the FULL signed bit offset addresses memory beyond
 *    the operand: byte address = ea + (sext(bitoff) >> 3) (arithmetic shift),
 *    bit = bitoff & 7. An IMMEDIATE bit offset masks to the width and is then
 *    folded into byte addressing the same way (ea += bit >> 3, bit &= 7) so
 *    indices above 7 test the right byte of the operand — _Block_copy's
 *    BLOCK_IS_GLOBAL probe is `bt dword [blk+8], 0x1c`, and reading only the
 *    lowest byte made every global-block check fail, heap-copying libdispatch's
 *    sentinel destructor blocks and breaking their pointer-identity contract.
 *    CF receives the tested bit; BTS/BTR/BTC write back the modified
 *    addressed unit (one byte for the memory path, the operand width for the
 *    register path). Other flags are left unchanged, as the architecture
 *    specifies for these instructions.
 *
 *  - BSF/BSR with the AMD zero-source behavior: a zero source sets ZF=1 and
 *    leaves the destination UNCHANGED (real code relies on this); otherwise
 *    ZF=0 and the destination is the bit index. POPCNT clears CF/OF/AF/SF/PF
 *    and sets ZF from the popcount. TZCNT/LZCNT yield the operand width on a
 *    zero source, set CF = (source == 0) and ZF = (result == 0).
 *
 *  - The system/misc cluster: CPUID with a fixed feature surface, RDTSC/
 *    RDTSCP via mach_absolute_time() scaled to nanoseconds, XGETBV,
 *    LDMXCSR/STMXCSR, FXSAVE/FXRSTOR, EMMS and FWAIT.
 *  - SGDT/SIDT write a zero descriptor-table base and put the current guest
 *    thread's cpu_number in the limit field's low 12 bits. libdispatch's
 *    per-CPU continuation magazine reads (limit & 0xfff) via SIDT to pick a
 *    cache; a constant there would make every real host worker thread share one
 *    magazine and race, so each guest thread carries a distinct cpu_number.
 *
 *  - The whole x87 subset (OCERZ_OP_X87_FIRST..OCERZ_OP_SSE_FIRST) modeled in
 *    double precision over cpu->fpr[8] with ftop/fcw/fsw, per cpu.h.
 *
 * Deviations / simplifications, all deliberate:
 *
 *  - x87 is double precision, not 80-bit extended. FLD m80 decodes the real
 *    10-byte format into the nearest double (sign, 15-bit exponent biased by
 *    16383, explicit integer bit, 53-bit-rounded mantissa) and FSTP m80
 *    re-encodes a double into the 80-bit format, so an 80-bit value that fits
 *    in a double round-trips, but extended-only precision is lost.
 *
 *  - FXSAVE/FXRSTOR store fcw at +0, fsw at +2, an abridged ftw at +4, mxcsr
 *    at +24, and the eight ST registers as 16-byte slots from +32, each slot
 *    holding the raw IEEE-754 double bits of fpr[] in its low 8 bytes with
 *    the upper 8 bytes zeroed. This is NOT the architectural 80-bit slot
 *    layout; it is a self-consistent double-backed approximation that
 *    round-trips through our own FXRSTOR, matching the model in cpu.h.
 *
 *  - The tag word is maintained lazily exactly as cpu.h prescribes: ftw is
 *    set to 0xff after any push and to 0 by FNINIT/FNCLEX-of-empty; per
 *    register tags are not modeled beyond that.
 *
 *  - FIST/FISTP and FRNDINT round per the fcw RC field (bits 10..11) using a
 *    local fesetround()/rint()/restore so the process rounding mode is never
 *    left changed.
 *
 *  - mach_timebase_info is queried once and cached; RDTSC returns nanoseconds
 *    rather than a real cycle counter, which is monotonic and good enough for
 *    the timing the guest observes.
 *
 *  - x87 register-form arithmetic carries exactly one ST operand in ops[0],
 *    which the decode contract cannot use to distinguish the D8-map form
 *    FADD ST(0), ST(i) (destination ST(0)) from the DC-map form
 *    FADD ST(i), ST(0) (destination ST(i)). We resolve the ambiguity to the
 *    D8 form for the non-pop opcodes (destination ST(0), the form compilers
 *    overwhelmingly emit) and to the architectural FxxxP form for the pop
 *    opcodes (destination ST(i), source ST(0), pop after). A bare pop form
 *    with no operand is FxxxP ST(1), ST(0). Memory/integer forms always have
 *    destination ST(0). The reverse opcodes (FSUBR/FDIVR and their P/I
 *    variants) compute source-minus-destination, matching x86.
 */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/vm.h"

#include <fenv.h>
#include <math.h>
#include <mach/mach_time.h>

static uint64_t ext_rcx_read(const OcerzCPU *cpu, const X86Insn *insn)
{
    if (insn->addrsize == 4)
        return (uint32_t)cpu->gpr[OCERZ_RCX];
    return cpu->gpr[OCERZ_RCX];
}

static void ext_rcx_write(OcerzCPU *cpu, const X86Insn *insn, uint64_t v)
{
    if (insn->addrsize == 4)
        cpu->gpr[OCERZ_RCX] = (uint32_t)v;
    else
        cpu->gpr[OCERZ_RCX] = v;
}

static uint64_t ext_ptr_read(const OcerzCPU *cpu, const X86Insn *insn, unsigned reg)
{
    if (insn->addrsize == 4)
        return (uint32_t)cpu->gpr[reg];
    return cpu->gpr[reg];
}

static void ext_ptr_write(OcerzCPU *cpu, const X86Insn *insn, unsigned reg, uint64_t v)
{
    if (insn->addrsize == 4)
        cpu->gpr[reg] = (uint32_t)v;
    else
        cpu->gpr[reg] = v;
}

static int ext_string(OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->opsize;
    int64_t step = (cpu->rflags & OCERZ_DF) ? -(int64_t)size : (int64_t)size;
    int rep = insn->rep;
    int op = insn->op;

    if (rep != OCERZ_REP_NONE && ext_rcx_read(cpu, insn) == 0)
        return OCERZ_STEP_OK;

    for (;;) {
        uint64_t a = 0, b = 0;
        int did_cmp = 0;

        switch (op) {
        case OCERZ_OP_MOVS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            ocerz_st(d, size, ocerz_ld(s, size));
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            break;
        }
        case OCERZ_OP_STOS: {
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            ocerz_st(d, size, ocerz_trunc(cpu->gpr[OCERZ_RAX], size));
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            break;
        }
        case OCERZ_OP_LODS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            ocerz_write_gpr(cpu, OCERZ_RAX, size, 0, ocerz_ld(s, size));
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            break;
        }
        case OCERZ_OP_SCAS: {
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            a = ocerz_trunc(cpu->gpr[OCERZ_RAX], size);
            b = ocerz_ld(d, size);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            did_cmp = 1;
            break;
        }
        case OCERZ_OP_CMPS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            a = ocerz_ld(s, size);
            b = ocerz_ld(d, size);
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            did_cmp = 1;
            break;
        }
        default:
            return OCERZ_EUNSUP;
        }

        if (rep == OCERZ_REP_NONE) {
            if (did_cmp)
                ocerz_flags_sub(cpu, size, a, b, 0, a - b);
            return OCERZ_STEP_OK;
        }

        uint64_t cnt = ext_rcx_read(cpu, insn) - 1;
        ext_rcx_write(cpu, insn, cnt);

        if (did_cmp) {
            ocerz_flags_sub(cpu, size, a, b, 0, a - b);
            int zf = (cpu->rflags & OCERZ_ZF) != 0;
            if (rep == OCERZ_REP_REP && !zf)
                return OCERZ_STEP_OK;
            if (rep == OCERZ_REP_REPNE && zf)
                return OCERZ_STEP_OK;
        }

        if (cnt == 0)
            return OCERZ_STEP_OK;
    }
}

static int ext_bit(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *dst = &insn->ops[0];
    const X86Operand *off = &insn->ops[1];
    int size = dst->size;
    int op = insn->op;
    int testbit;

    if (dst->kind == OCERZ_OPK_REG) {
        unsigned bit = (unsigned)(ocerz_read_op(cpu, insn, off) & (size * 8 - 1));
        uint64_t val = ocerz_read_gpr(cpu, dst->reg, size, dst->high8);
        testbit = (int)((val >> bit) & 1);
        ocerz_flag_assign(cpu, OCERZ_CF, testbit);
        if (op != OCERZ_OP_BT) {
            uint64_t nv = val;
            if (op == OCERZ_OP_BTS)
                nv |= (uint64_t)1 << bit;
            else if (op == OCERZ_OP_BTR)
                nv &= ~((uint64_t)1 << bit);
            else
                nv ^= (uint64_t)1 << bit;
            ocerz_write_gpr(cpu, dst->reg, size, dst->high8, nv);
        }
        return OCERZ_STEP_OK;
    }

    uint64_t ea = ocerz_ea(cpu, insn, dst);
    uint64_t bit;
    if (off->kind == OCERZ_OPK_IMM) {
        bit = ocerz_read_op(cpu, insn, off) & (size * 8 - 1);
        ea = ea + (bit >> 3);
        bit = bit & 7;
    } else {
        int64_t sbit = ocerz_sext(ocerz_read_op(cpu, insn, off), off->size);
        ea = ea + (uint64_t)(sbit >> 3);
        bit = (uint64_t)sbit & 7;
    }

    if (op != OCERZ_OP_BT && insn->lock) {
        uint8_t *hp = (uint8_t *)ocerz_g2h(ea);
        uint8_t cur = __atomic_load_n(hp, __ATOMIC_SEQ_CST);
        for (;;) {
            uint8_t nv = cur;
            if (op == OCERZ_OP_BTS)
                nv |= (uint8_t)(1u << bit);
            else if (op == OCERZ_OP_BTR)
                nv &= (uint8_t)~(1u << bit);
            else
                nv ^= (uint8_t)(1u << bit);
            if (__atomic_compare_exchange_n(hp, &cur, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                testbit = (int)((cur >> bit) & 1);
                break;
            }
        }
        ocerz_flag_assign(cpu, OCERZ_CF, testbit);
        if (ocerz_watch_addr && ocerz_watch_addr - ea < 1)
            ocerz_watch_hit(ea, 1, __atomic_load_n(hp, __ATOMIC_SEQ_CST), 0);
        return OCERZ_STEP_OK;
    }

    uint8_t byte = (uint8_t)ocerz_ld(ea, 1);
    testbit = (int)((byte >> bit) & 1);
    ocerz_flag_assign(cpu, OCERZ_CF, testbit);
    if (op != OCERZ_OP_BT) {
        if (op == OCERZ_OP_BTS)
            byte |= (uint8_t)(1u << bit);
        else if (op == OCERZ_OP_BTR)
            byte &= (uint8_t)~(1u << bit);
        else
            byte ^= (uint8_t)(1u << bit);
        ocerz_st(ea, 1, byte);
    }
    return OCERZ_STEP_OK;
}

static int ext_scan(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *dst = &insn->ops[0];
    const X86Operand *src = &insn->ops[1];
    int size = dst->size;
    int bits = size * 8;
    uint64_t s = ocerz_trunc(ocerz_read_op(cpu, insn, src), size);

    switch (insn->op) {
    case OCERZ_OP_BSF:
        if (s == 0) {
            ocerz_flag_assign(cpu, OCERZ_ZF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            ocerz_write_op(cpu, insn, dst, (uint64_t)__builtin_ctzll(s));
        }
        return OCERZ_STEP_OK;
    case OCERZ_OP_BSR:
        if (s == 0) {
            ocerz_flag_assign(cpu, OCERZ_ZF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            ocerz_write_op(cpu, insn, dst, (uint64_t)(63 - __builtin_clzll(s)));
        }
        return OCERZ_STEP_OK;
    case OCERZ_OP_POPCNT: {
        uint64_t r = (uint64_t)__builtin_popcountll(s);
        ocerz_write_op(cpu, insn, dst, r);
        cpu->rflags &= ~(uint64_t)(OCERZ_CF | OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_PF);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_TZCNT: {
        uint64_t r = (s == 0) ? (uint64_t)bits : (uint64_t)__builtin_ctzll(s);
        ocerz_write_op(cpu, insn, dst, r);
        ocerz_flag_assign(cpu, OCERZ_CF, s == 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LZCNT: {
        uint64_t r = (s == 0) ? (uint64_t)bits : (uint64_t)(bits - 1 - (63 - __builtin_clzll(s)));
        ocerz_write_op(cpu, insn, dst, r);
        ocerz_flag_assign(cpu, OCERZ_CF, s == 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static void cpuid_brand(uint32_t leaf, uint32_t *regs)
{
    static const char brand[48] = "Ocerz x86_64 Emulated CPU";
    uint32_t idx = leaf - 0x80000002u;
    uint32_t words[12];
    memcpy(words, brand, 48);
    regs[0] = words[idx * 4 + 0];
    regs[1] = words[idx * 4 + 1];
    regs[2] = words[idx * 4 + 2];
    regs[3] = words[idx * 4 + 3];
}

static int ext_cpuid(OcerzCPU *cpu)
{
    uint32_t leaf = (uint32_t)cpu->gpr[OCERZ_RAX];
    uint32_t r[4] = { 0, 0, 0, 0 };

    if (leaf == 0) {
        r[0] = 7;
        r[1] = 0x756e6547;
        r[2] = 0x6c65746e;
        r[3] = 0x49656e69;
    } else if (leaf == 1) {
        r[0] = 0x000306a9;
        r[1] = 0x00100800;
        r[2] = 0x00802201;
        r[3] = 0x078bfbff;
    } else if (leaf == 0x80000000u) {
        r[0] = 0x80000004;
    } else if (leaf == 0x80000001u) {
        r[2] = 0x00000001;
        r[3] = 0x28100800;
    } else if (leaf >= 0x80000002u && leaf <= 0x80000004u) {
        cpuid_brand(leaf, r);
    }

    cpu->gpr[OCERZ_RAX] = r[0];
    cpu->gpr[OCERZ_RBX] = r[1];
    cpu->gpr[OCERZ_RCX] = r[2];
    cpu->gpr[OCERZ_RDX] = r[3];
    return OCERZ_STEP_OK;
}

static uint64_t ext_rdtsc_ns(void)
{
    static mach_timebase_info_data_t tb;
    static int have_tb;
    if (!have_tb) {
        mach_timebase_info(&tb);
        have_tb = 1;
    }
    uint64_t t = mach_absolute_time();
    return t * tb.numer / tb.denom;
}

static int ext_rdtsc(OcerzCPU *cpu, int rdtscp)
{
    uint64_t ns = ext_rdtsc_ns();
    cpu->gpr[OCERZ_RAX] = (uint32_t)ns;
    cpu->gpr[OCERZ_RDX] = (uint32_t)(ns >> 32);
    if (rdtscp)
        cpu->gpr[OCERZ_RCX] = 0;
    return OCERZ_STEP_OK;
}

static int ext_xgetbv(OcerzCPU *cpu)
{
    if ((uint32_t)cpu->gpr[OCERZ_RCX] != 0) {
        OCERZ_FATAL("xgetbv with ecx=%u is unsupported\n", (unsigned)cpu->gpr[OCERZ_RCX]);
        return OCERZ_STEP_FATAL;
    }
    cpu->gpr[OCERZ_RAX] = 3;
    cpu->gpr[OCERZ_RDX] = 0;
    return OCERZ_STEP_OK;
}

static int ext_fxsave(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    ocerz_st(ea + 0, 2, cpu->fcw);
    ocerz_st(ea + 2, 2, cpu->fsw);
    ocerz_st(ea + 4, 1, cpu->ftw);
    ocerz_st(ea + 24, 4, cpu->mxcsr);
    /* MXCSR_MASK. Wine copies the first 32 bytes of this image straight into
     * CONTEXT.FltSave and reads MxCsr_Mask back out of it, so leaving it as whatever
     * happened to be on the stack hands the guest a garbage mask. */
    ocerz_st(ea + 28, 4, 0x0000ffffu);
    for (int i = 0; i < 8; i++) {
        uint64_t bits;
        memcpy(&bits, &cpu->fpr[i], 8);
        ocerz_st(ea + 32 + i * 16 + 0, 8, bits);
        ocerz_st(ea + 32 + i * 16 + 8, 8, 0);
    }
    /* XMM0-15 at +0xa0, 16 bytes apiece. These were NEVER written, and the omission is
     * not academic: Wine's __wine_syscall_dispatcher does not rely on the CPU preserving
     * registers across a syscall -- it FXSAVEs and then reloads the Win64 non-volatile
     * xmm6-xmm15 with movaps straight out of THIS image. With the area untouched, every
     * PE Nt* syscall restored whatever was on the stack (usually zero) into xmm6-15.
     * ntdll keeps acl->actctx in xmm6 across such a call, so it came back NULL and
     * wineboot died dereferencing it. ocerz preserving xmm across its own syscall path
     * (it does, and that was verified) is irrelevant here -- the guest never asked it to. */
    for (int i = 0; i < 16; i++)
        ocerz_st128(ea + 160 + (uint64_t)i * 16, cpu->xmm[i]);
    return OCERZ_STEP_OK;
}

static int ext_fxrstor(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    cpu->fcw = (uint16_t)ocerz_ld(ea + 0, 2);
    cpu->fsw = (uint16_t)ocerz_ld(ea + 2, 2);
    cpu->ftw = (uint8_t)ocerz_ld(ea + 4, 1);
    cpu->mxcsr = (uint32_t)ocerz_ld(ea + 24, 4);
    for (int i = 0; i < 8; i++) {
        uint64_t bits = ocerz_ld(ea + 32 + i * 16 + 0, 8);
        memcpy(&cpu->fpr[i], &bits, 8);
    }
    for (int i = 0; i < 16; i++)              /* mirror of the XMM save above */
        cpu->xmm[i] = ocerz_ld128(ea + 160 + (uint64_t)i * 16);
    return OCERZ_STEP_OK;
}

static int ext_misc(OcerzCPU *cpu, const X86Insn *insn)
{
    switch (insn->op) {
    case OCERZ_OP_CPUID:
        return ext_cpuid(cpu);
    case OCERZ_OP_RDTSC:
        return ext_rdtsc(cpu, 0);
    case OCERZ_OP_RDTSCP:
        return ext_rdtsc(cpu, 1);
    case OCERZ_OP_XGETBV:
        return ext_xgetbv(cpu);
    case OCERZ_OP_SGDT:
    case OCERZ_OP_SIDT: {
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
        ocerz_st(ea + 0, 2, (uint64_t)(uint16_t)(cpu->cpu_number & 0xfff));
        ocerz_st(ea + 2, 8, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LDMXCSR:
        cpu->mxcsr = (uint32_t)ocerz_ld(ocerz_ea(cpu, insn, &insn->ops[0]), 4);
        return OCERZ_STEP_OK;
    case OCERZ_OP_STMXCSR:
        ocerz_st(ocerz_ea(cpu, insn, &insn->ops[0]), 4, cpu->mxcsr);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FXSAVE:
        return ext_fxsave(cpu, insn);
    case OCERZ_OP_FXRSTOR:
        return ext_fxrstor(cpu, insn);
    case OCERZ_OP_EMMS:
        cpu->ftop = 0;
        cpu->ftw = 0;
        return OCERZ_STEP_OK;
    default:
        return OCERZ_EUNSUP;
    }
}

static double x87_st(const OcerzCPU *cpu, int i)
{
    return cpu->fpr[(cpu->ftop + i) & 7];
}

static void x87_set_st(OcerzCPU *cpu, int i, double v)
{
    cpu->fpr[(cpu->ftop + i) & 7] = v;
}

static void x87_push(OcerzCPU *cpu, double v)
{
    cpu->ftop = (cpu->ftop - 1) & 7;
    cpu->fpr[cpu->ftop] = v;
    cpu->ftw = 0xff;
}

static double x87_pop(OcerzCPU *cpu)
{
    double v = cpu->fpr[cpu->ftop];
    cpu->ftop = (cpu->ftop + 1) & 7;
    return v;
}

static int x87_round_mode(const OcerzCPU *cpu)
{
    switch ((cpu->fcw >> 10) & 3) {
    case 0: return FE_TONEAREST;
    case 1: return FE_DOWNWARD;
    case 2: return FE_UPWARD;
    default: return FE_TOWARDZERO;
    }
}

static double x87_rint_rc(const OcerzCPU *cpu, double v)
{
    int saved = fegetround();
    fesetround(x87_round_mode(cpu));
    double r = rint(v);
    fesetround(saved);
    return r;
}

static double x87_read_f80(const uint8_t *b)
{
    uint64_t mant = 0;
    memcpy(&mant, b, 8);
    uint16_t se = (uint16_t)(b[8] | (b[9] << 8));
    int sign = (se >> 15) & 1;
    int exp = se & 0x7fff;

    if (exp == 0 && mant == 0)
        return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) {
        if (mant << 1 == 0)
            return sign ? -INFINITY : INFINITY;
        return sign ? -NAN : NAN;
    }
    double m = (double)mant / 9223372036854775808.0;
    double v = ldexp(m, exp - 16383);
    return sign ? -v : v;
}

static void x87_write_f80(uint8_t *b, double v)
{
    uint64_t mant;
    uint16_t se;

    if (v == 0.0) {
        mant = 0;
        se = signbit(v) ? 0x8000 : 0;
    } else if (isinf(v)) {
        mant = (uint64_t)1 << 63;
        se = (uint16_t)(0x7fff | (v < 0 ? 0x8000 : 0));
    } else if (isnan(v)) {
        mant = ((uint64_t)1 << 63) | ((uint64_t)1 << 62);
        se = 0x7fff;
    } else {
        int sign = signbit(v);
        double a = fabs(v);
        int exp;
        double frac = frexp(a, &exp);
        int e80 = exp - 1 + 16383;
        double m = frac * 2.0;
        mant = (uint64_t)ldexp(m, 63);
        se = (uint16_t)((e80 & 0x7fff) | (sign ? 0x8000 : 0));
    }
    memcpy(b, &mant, 8);
    b[8] = (uint8_t)(se & 0xff);
    b[9] = (uint8_t)(se >> 8);
}

static double x87_load_real(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    if (op->size == 4) {
        uint32_t bits = (uint32_t)ocerz_ld(ea, 4);
        float f;
        memcpy(&f, &bits, 4);
        return (double)f;
    }
    if (op->size == 8) {
        uint64_t bits = ocerz_ld(ea, 8);
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    uint8_t buf[10];
    memcpy(buf, ocerz_g2h(ea), 10);
    return x87_read_f80(buf);
}

static void x87_store_real(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, double v)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    if (op->size == 4) {
        float f = (float)v;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        ocerz_st(ea, 4, bits);
    } else if (op->size == 8) {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        ocerz_st(ea, 8, bits);
    } else {
        uint8_t buf[10];
        x87_write_f80(buf, v);
        memcpy(ocerz_g2h(ea), buf, 10);
    }
}

static double x87_load_int(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    return (double)ocerz_sext(ocerz_ld(ea, op->size), op->size);
}

static void x87_store_int(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, double v, int round)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    double r = round ? x87_rint_rc(cpu, v) : v;
    int64_t iv = (int64_t)r;
    ocerz_st(ea, op->size, (uint64_t)iv);
}

static void x87_set_status_cc(OcerzCPU *cpu, int c0, int c2, int c3)
{
    cpu->fsw &= ~(uint16_t)((1u << 8) | (1u << 9) | (1u << 10) | (1u << 14));
    if (c0)
        cpu->fsw |= (uint16_t)(1u << 8);
    if (c2)
        cpu->fsw |= (uint16_t)(1u << 10);
    if (c3)
        cpu->fsw |= (uint16_t)(1u << 14);
}

static void x87_compare_fsw(OcerzCPU *cpu, double a, double b)
{
    if (isnan(a) || isnan(b))
        x87_set_status_cc(cpu, 1, 1, 1);
    else if (a < b)
        x87_set_status_cc(cpu, 1, 0, 0);
    else if (a > b)
        x87_set_status_cc(cpu, 0, 0, 0);
    else
        x87_set_status_cc(cpu, 0, 0, 1);
}

static void x87_compare_rflags(OcerzCPU *cpu, double a, double b)
{
    cpu->rflags &= ~(uint64_t)(OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_ZF | OCERZ_PF | OCERZ_CF);
    if (isnan(a) || isnan(b))
        cpu->rflags |= OCERZ_ZF | OCERZ_PF | OCERZ_CF;
    else if (a < b)
        cpu->rflags |= OCERZ_CF;
    else if (a == b)
        cpu->rflags |= OCERZ_ZF;
}

static uint16_t x87_synth_fsw(const OcerzCPU *cpu)
{
    uint16_t s = cpu->fsw & ~(uint16_t)(7u << 11);
    s |= (uint16_t)((cpu->ftop & 7) << 11);
    return s;
}

static int x87_is_int_form(int op)
{
    return op == OCERZ_OP_FIADD || op == OCERZ_OP_FISUB ||
           op == OCERZ_OP_FISUBR || op == OCERZ_OP_FIMUL ||
           op == OCERZ_OP_FIDIV || op == OCERZ_OP_FIDIVR;
}

static int x87_is_pop_form(int op)
{
    return op == OCERZ_OP_FADDP || op == OCERZ_OP_FSUBP ||
           op == OCERZ_OP_FSUBRP || op == OCERZ_OP_FMULP ||
           op == OCERZ_OP_FDIVP || op == OCERZ_OP_FDIVRP;
}

static int ext_x87_arith(OcerzCPU *cpu, const X86Insn *insn)
{
    int op = insn->op;
    int pop = x87_is_pop_form(op);
    int has_mem = (insn->nops > 0 && insn->ops[0].kind != OCERZ_OPK_ST);

    int dst, src_is_mem = 0;
    double other = 0.0;

    if (has_mem) {
        dst = 0;
        src_is_mem = 1;
        other = x87_is_int_form(op)
                    ? x87_load_int(cpu, insn, &insn->ops[0])
                    : x87_load_real(cpu, insn, &insn->ops[0]);
    } else if (pop) {
        dst = (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_ST)
                  ? insn->ops[0].reg : 1;
        other = x87_st(cpu, 0);
    } else {
        dst = 0;
        int sti = (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_ST)
                      ? insn->ops[0].reg : 1;
        other = x87_st(cpu, sti);
    }
    (void)src_is_mem;

    double a = x87_st(cpu, dst);
    double r;
    switch (op) {
    case OCERZ_OP_FADD: case OCERZ_OP_FADDP: case OCERZ_OP_FIADD:
        r = a + other; break;
    case OCERZ_OP_FSUB: case OCERZ_OP_FSUBP: case OCERZ_OP_FISUB:
        r = a - other; break;
    case OCERZ_OP_FSUBR: case OCERZ_OP_FSUBRP: case OCERZ_OP_FISUBR:
        r = other - a; break;
    case OCERZ_OP_FMUL: case OCERZ_OP_FMULP: case OCERZ_OP_FIMUL:
        r = a * other; break;
    case OCERZ_OP_FDIV: case OCERZ_OP_FDIVP: case OCERZ_OP_FIDIV:
        r = a / other; break;
    case OCERZ_OP_FDIVR: case OCERZ_OP_FDIVRP: case OCERZ_OP_FIDIVR:
        r = other / a; break;
    default:
        return OCERZ_EUNSUP;
    }

    x87_set_st(cpu, dst, r);
    if (pop)
        x87_pop(cpu);
    return OCERZ_STEP_OK;
}

static int ext_x87_compare(OcerzCPU *cpu, const X86Insn *insn)
{
    int op = insn->op;
    double a = x87_st(cpu, 0);
    double b;
    int pops = 0;

    switch (op) {
    case OCERZ_OP_FCOM: case OCERZ_OP_FUCOM:
        b = (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_ST)
            ? x87_st(cpu, insn->ops[0].reg)
            : (insn->nops > 0 ? x87_load_real(cpu, insn, &insn->ops[0]) : x87_st(cpu, 1));
        x87_compare_fsw(cpu, a, b);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMP: case OCERZ_OP_FUCOMP:
        b = (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_ST)
            ? x87_st(cpu, insn->ops[0].reg)
            : (insn->nops > 0 ? x87_load_real(cpu, insn, &insn->ops[0]) : x87_st(cpu, 1));
        x87_compare_fsw(cpu, a, b);
        x87_pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMPP: case OCERZ_OP_FUCOMPP:
        b = x87_st(cpu, 1);
        x87_compare_fsw(cpu, a, b);
        x87_pop(cpu);
        x87_pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FTST:
        x87_compare_fsw(cpu, a, 0.0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMI: case OCERZ_OP_FUCOMI:
        b = x87_st(cpu, insn->ops[0].kind == OCERZ_OPK_ST ? insn->ops[0].reg : 1);
        x87_compare_rflags(cpu, a, b);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMIP: case OCERZ_OP_FUCOMIP:
        b = x87_st(cpu, insn->ops[0].kind == OCERZ_OPK_ST ? insn->ops[0].reg : 1);
        x87_compare_rflags(cpu, a, b);
        x87_pop(cpu);
        return OCERZ_STEP_OK;
    default:
        (void)pops;
        return OCERZ_EUNSUP;
    }
}

static int ext_x87(OcerzCPU *cpu, const X86Insn *insn)
{
    int op = insn->op;

    switch (op) {
    case OCERZ_OP_FLD:
        x87_push(cpu, (insn->ops[0].kind == OCERZ_OPK_ST)
                          ? x87_st(cpu, insn->ops[0].reg)
                          : x87_load_real(cpu, insn, &insn->ops[0]));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FILD:
        x87_push(cpu, x87_load_int(cpu, insn, &insn->ops[0]));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FST:
        if (insn->ops[0].kind == OCERZ_OPK_ST)
            x87_set_st(cpu, insn->ops[0].reg, x87_st(cpu, 0));
        else
            x87_store_real(cpu, insn, &insn->ops[0], x87_st(cpu, 0));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FSTP:
        if (insn->ops[0].kind == OCERZ_OPK_ST)
            x87_set_st(cpu, insn->ops[0].reg, x87_st(cpu, 0));
        else
            x87_store_real(cpu, insn, &insn->ops[0], x87_st(cpu, 0));
        x87_pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FIST:
        x87_store_int(cpu, insn, &insn->ops[0], x87_st(cpu, 0), 1);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FISTP:
        x87_store_int(cpu, insn, &insn->ops[0], x87_st(cpu, 0), 1);
        x87_pop(cpu);
        return OCERZ_STEP_OK;

    case OCERZ_OP_FLDCW:
        cpu->fcw = (uint16_t)ocerz_ld(ocerz_ea(cpu, insn, &insn->ops[0]), 2);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSTCW:
        ocerz_st(ocerz_ea(cpu, insn, &insn->ops[0]), 2, cpu->fcw);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSTSW:
        if (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_REG)
            ocerz_write_gpr(cpu, insn->ops[0].reg, 2, 0, x87_synth_fsw(cpu));
        else
            ocerz_st(ocerz_ea(cpu, insn, &insn->ops[0]), 2, x87_synth_fsw(cpu));
        return OCERZ_STEP_OK;

    case OCERZ_OP_FNSTENV: {
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
        ocerz_st(ea + 0, 4, cpu->fcw);
        ocerz_st(ea + 4, 4, x87_synth_fsw(cpu));
        ocerz_st(ea + 8, 4, cpu->ftw);
        for (int i = 12; i < 28; i += 4)
            ocerz_st(ea + i, 4, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FLDENV: {
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
        cpu->fcw = (uint16_t)ocerz_ld(ea + 0, 4);
        cpu->fsw = (uint16_t)ocerz_ld(ea + 4, 4);
        cpu->ftw = (uint8_t)ocerz_ld(ea + 8, 4);
        cpu->ftop = (uint8_t)((cpu->fsw >> 11) & 7);
        return OCERZ_STEP_OK;
    }

    case OCERZ_OP_FXCH: {
        int i = (insn->nops > 0 && insn->ops[0].kind == OCERZ_OPK_ST) ? insn->ops[0].reg : 1;
        double t = x87_st(cpu, 0);
        x87_set_st(cpu, 0, x87_st(cpu, i));
        x87_set_st(cpu, i, t);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FCHS:
        x87_set_st(cpu, 0, -x87_st(cpu, 0));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FABS:
        x87_set_st(cpu, 0, fabs(x87_st(cpu, 0)));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FSQRT:
        x87_set_st(cpu, 0, __builtin_sqrt(x87_st(cpu, 0)));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FRNDINT:
        x87_set_st(cpu, 0, x87_rint_rc(cpu, x87_st(cpu, 0)));
        return OCERZ_STEP_OK;

    case OCERZ_OP_FLDZ: x87_push(cpu, 0.0); return OCERZ_STEP_OK;
    case OCERZ_OP_FLD1: x87_push(cpu, 1.0); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDPI: x87_push(cpu, 3.14159265358979311600); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDL2E: x87_push(cpu, 1.44269504088896340736); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDL2T: x87_push(cpu, 3.32192809488736234781); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDLG2: x87_push(cpu, 0.30102999566398119521); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDLN2: x87_push(cpu, 0.69314718055994530942); return OCERZ_STEP_OK;

    case OCERZ_OP_F2XM1:
        x87_set_st(cpu, 0, exp2(x87_st(cpu, 0)) - 1.0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FYL2X: {
        double r = x87_st(cpu, 1) * log2(x87_st(cpu, 0));
        x87_pop(cpu);
        x87_set_st(cpu, 0, r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FPTAN: {
        double r = tan(x87_st(cpu, 0));
        x87_set_st(cpu, 0, r);
        x87_push(cpu, 1.0);
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FPATAN: {
        double r = atan2(x87_st(cpu, 1), x87_st(cpu, 0));
        x87_pop(cpu);
        x87_set_st(cpu, 0, r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FPREM: {
        double r = fmod(x87_st(cpu, 0), x87_st(cpu, 1));
        x87_set_st(cpu, 0, r);
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FPREM1: {
        double r = remainder(x87_st(cpu, 0), x87_st(cpu, 1));
        x87_set_st(cpu, 0, r);
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FSCALE: {
        double r = ldexp(x87_st(cpu, 0), (int)trunc(x87_st(cpu, 1)));
        x87_set_st(cpu, 0, r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FSIN:
        x87_set_st(cpu, 0, sin(x87_st(cpu, 0)));
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOS:
        x87_set_st(cpu, 0, cos(x87_st(cpu, 0)));
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FSINCOS: {
        double s = sin(x87_st(cpu, 0));
        double c = cos(x87_st(cpu, 0));
        x87_set_st(cpu, 0, s);
        x87_push(cpu, c);
        x87_set_status_cc(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }

    case OCERZ_OP_FCMOVCC:
        if (ocerz_cc_eval(cpu, insn->cc)) {
            int i = (insn->ops[0].kind == OCERZ_OPK_ST) ? insn->ops[0].reg : 1;
            x87_set_st(cpu, 0, x87_st(cpu, i));
        }
        return OCERZ_STEP_OK;

    case OCERZ_OP_FNINIT:
        cpu->fcw = 0x037f;
        cpu->fsw = 0;
        cpu->ftw = 0;
        cpu->ftop = 0;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNCLEX:
        cpu->fsw &= ~(uint16_t)0x80ff;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FFREE:
        return OCERZ_STEP_OK;
    case OCERZ_OP_FINCSTP:
        cpu->ftop = (cpu->ftop + 1) & 7;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FDECSTP:
        cpu->ftop = (cpu->ftop - 1) & 7;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FWAIT:
        return OCERZ_STEP_OK;

    case OCERZ_OP_FADD: case OCERZ_OP_FADDP: case OCERZ_OP_FIADD:
    case OCERZ_OP_FSUB: case OCERZ_OP_FSUBP: case OCERZ_OP_FISUB:
    case OCERZ_OP_FSUBR: case OCERZ_OP_FSUBRP: case OCERZ_OP_FISUBR:
    case OCERZ_OP_FMUL: case OCERZ_OP_FMULP: case OCERZ_OP_FIMUL:
    case OCERZ_OP_FDIV: case OCERZ_OP_FDIVP: case OCERZ_OP_FIDIV:
    case OCERZ_OP_FDIVR: case OCERZ_OP_FDIVRP: case OCERZ_OP_FIDIVR:
        return ext_x87_arith(cpu, insn);

    case OCERZ_OP_FCOM: case OCERZ_OP_FCOMP: case OCERZ_OP_FCOMPP:
    case OCERZ_OP_FUCOM: case OCERZ_OP_FUCOMP: case OCERZ_OP_FUCOMPP:
    case OCERZ_OP_FTST:
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP:
    case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
        return ext_x87_compare(cpu, insn);

    default:
        return OCERZ_EUNSUP;
    }
}

int ocerz_interp_ext(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    int op = insn->op;

    if (op > OCERZ_OP_X87_FIRST && op < OCERZ_OP_SSE_FIRST)
        return ext_x87(cpu, insn);

    switch (op) {
    case OCERZ_OP_MOVS:
    case OCERZ_OP_STOS:
    case OCERZ_OP_LODS:
    case OCERZ_OP_SCAS:
    case OCERZ_OP_CMPS:
        return ext_string(cpu, insn);

    case OCERZ_OP_BT:
    case OCERZ_OP_BTS:
    case OCERZ_OP_BTR:
    case OCERZ_OP_BTC:
        return ext_bit(cpu, insn);

    case OCERZ_OP_BSF:
    case OCERZ_OP_BSR:
    case OCERZ_OP_POPCNT:
    case OCERZ_OP_TZCNT:
    case OCERZ_OP_LZCNT:
        return ext_scan(cpu, insn);

    case OCERZ_OP_CPUID:
    case OCERZ_OP_RDTSC:
    case OCERZ_OP_RDTSCP:
    case OCERZ_OP_XGETBV:
    case OCERZ_OP_SGDT:
    case OCERZ_OP_SIDT:
    case OCERZ_OP_LDMXCSR:
    case OCERZ_OP_STMXCSR:
    case OCERZ_OP_FXSAVE:
    case OCERZ_OP_FXRSTOR:
    case OCERZ_OP_EMMS:
        return ext_misc(cpu, insn);

    default:
        return OCERZ_EUNSUP;
    }
}
