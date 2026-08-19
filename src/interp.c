/* The core single-step interpreter: reference semantics for x86_64. */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/vm.h"
#include "ocerz/syscall.h"
#include "ocerz/dyldapi.h"
#include "ocerz/dyld.h"
#include <stdlib.h>

static int far_transfer(OcerzCPU *cpu, uint32_t sel, uint64_t off);

/* Reading order for the whole i386 slice of this file: the CPU never asks
 * "am I in 32-bit mode?" at run time.  Every decoded instruction carries the
 * mode it was decoded in (insn->mode32), the decoder has already given every
 * operand and every implicit stack slot its 32-bit width, and the handlers
 * below just use those widths.  In long mode insn->mode32 is 0 and each of the
 * expressions added here collapses to the constant that was written there
 * before, which is why none of the 64-bit unit tests move. */

static void dump_raw_bytes(FILE *out, uint64_t rip, unsigned len)
{
    const uint8_t *p = (const uint8_t *)ocerz_g2h(rip);
    if (len == 0 || len > 15)
        len = 15;
    for (unsigned i = 0; i < len; i++)
        fprintf(out, "%02x ", p[i]);
}

int ocerz_unimpl(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn, const char *why)
{
    (void)vm;
    (void)cpu;
    fprintf(stderr, "ocerz: fatal: unimplemented instruction %s at rip=%#llx (%s)\n  bytes: ",
            ocerz_op_name(insn->op), (unsigned long long)insn->rip, why ? why : "");
    dump_raw_bytes(stderr, insn->rip, insn->len);
    fprintf(stderr, "\n");
    return OCERZ_STEP_FATAL;
}

int ocerz_cftrap_on;

void ocerz_cftrap(const OcerzCPU *cpu, uint64_t src, uint64_t target, const char *kind)
{
    fprintf(stderr, "ocerz: CFTRAP %s cpu=%d src=%#llx target=%#llx "
            "rax=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r10=%#llx r11=%#llx [rsp]=%#llx\n",
            kind, cpu->cpu_number, (unsigned long long)src, (unsigned long long)target,
            (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)cpu->gpr[OCERZ_RCX],
            (unsigned long long)cpu->gpr[OCERZ_RDX], (unsigned long long)cpu->gpr[OCERZ_RSI],
            (unsigned long long)cpu->gpr[OCERZ_RDI], (unsigned long long)cpu->gpr[OCERZ_R10],
            (unsigned long long)cpu->gpr[OCERZ_R11],
            (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8));
}

static int trap_fatal(const X86Insn *insn, const char *msg)
{
    fprintf(stderr, "ocerz: fatal: %s at rip=%#llx\n  bytes: ",
            msg, (unsigned long long)insn->rip);
    dump_raw_bytes(stderr, insn->rip, insn->len);
    fprintf(stderr, "\n");
    return OCERZ_STEP_FATAL;
}

/* #DE (divide by zero / quotient overflow): a fault, so the guest handler
 * sees rip at the div; SIGFPE with FPE_INTDIV / FPE_INTOVF like Darwin.
 * With no guest handler installed it stays fatal. */
static int div_trap(OcerzCPU *cpu, const X86Insn *insn, int code, const char *msg)
{
    uint64_t next = cpu->rip;
    cpu->rip = insn->rip;
    if (ocerz_signal_deliver(cpu, OCERZ_SIGFPE, insn->rip, code, 0))
        return OCERZ_STEP_REDIRECT;
    cpu->rip = next;
    return trap_fatal(insn, msg);
}

static uint64_t lea_addr(const OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t a = (uint64_t)op->disp;
    if (op->riprel)
        return a;
    if (op->base != OCERZ_REG_NONE)
        a += cpu->gpr[op->base];
    if (op->index != OCERZ_REG_NONE)
        a += cpu->gpr[op->index] << op->scale;
    if (insn->addrsize == 4)
        a = (uint32_t)a;
    else if (insn->addrsize == 2)
        a = (uint16_t)a;   /* 32-bit mode + 0x67; addrsize is never 2 in long mode */
    return a;
}

static uint64_t read_acc(const OcerzCPU *cpu, int size)
{
    return ocerz_read_gpr(cpu, OCERZ_RAX, size, 0);
}

static void write_acc(OcerzCPU *cpu, int size, uint64_t v)
{
    ocerz_write_gpr(cpu, OCERZ_RAX, size, 0, v);
}

static uint64_t ocerz_atomic_xchg(uint64_t gaddr, int size, uint64_t v)
{
    void *p = ocerz_g2h(gaddr);
    uint64_t old;
    switch (size) {
    case 1: { uint8_t  x = (uint8_t)v;  old = __atomic_exchange_n((uint8_t  *)p, x, __ATOMIC_SEQ_CST); break; }
    case 2: { uint16_t x = (uint16_t)v; old = __atomic_exchange_n((uint16_t *)p, x, __ATOMIC_SEQ_CST); break; }
    case 4: { uint32_t x = (uint32_t)v; old = __atomic_exchange_n((uint32_t *)p, x, __ATOMIC_SEQ_CST); break; }
    default:{ uint64_t x = v;           old = __atomic_exchange_n((uint64_t *)p, x, __ATOMIC_SEQ_CST); break; }
    }
    if (ocerz_watch_addr && gaddr < ocerz_watch_addr + ocerz_watch_len &&
        gaddr + (uint64_t)size > ocerz_watch_addr)
        ocerz_watch_hit(gaddr, size, v, 0);
    return old;
}

static uint64_t ocerz_atomic_fetch_add(uint64_t gaddr, int size, uint64_t v)
{
    void *p = ocerz_g2h(gaddr);
    uint64_t old;
    switch (size) {
    case 1: old = __atomic_fetch_add((uint8_t  *)p, (uint8_t)v,  __ATOMIC_SEQ_CST); break;
    case 2: old = __atomic_fetch_add((uint16_t *)p, (uint16_t)v, __ATOMIC_SEQ_CST); break;
    case 4: old = __atomic_fetch_add((uint32_t *)p, (uint32_t)v, __ATOMIC_SEQ_CST); break;
    default:old = __atomic_fetch_add((uint64_t *)p, v,           __ATOMIC_SEQ_CST); break;
    }
    if (ocerz_watch_addr && gaddr < ocerz_watch_addr + ocerz_watch_len &&
        gaddr + (uint64_t)size > ocerz_watch_addr)
        ocerz_watch_hit(gaddr, size, ocerz_trunc(old + v, size), 0);
    return old;
}

static int ocerz_atomic_cmpxchg(uint64_t gaddr, int size, uint64_t *expected, uint64_t desired)
{
    void *p = ocerz_g2h(gaddr);
    int ok;
    switch (size) {
    case 1: { uint8_t  e = (uint8_t)*expected;  ok = __atomic_compare_exchange_n((uint8_t  *)p, &e, (uint8_t)desired,  0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; break; }
    case 2: { uint16_t e = (uint16_t)*expected; ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; break; }
    case 4: { uint32_t e = (uint32_t)*expected; ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; break; }
    default:{ uint64_t e = *expected;           ok = __atomic_compare_exchange_n((uint64_t *)p, &e, desired,           0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; break; }
    }
    if (ocerz_watch_addr && gaddr < ocerz_watch_addr + ocerz_watch_len &&
        gaddr + (uint64_t)size > ocerz_watch_addr)
        ocerz_watch_hit(gaddr, size, desired, ok ? 0xCA5 : 0xFA11);
    return ok;
}

static uint64_t ocerz_atomic_load(uint64_t gaddr, int size)
{
    void *p = ocerz_g2h(gaddr);
    switch (size) {
    case 1:  return __atomic_load_n((uint8_t  *)p, __ATOMIC_SEQ_CST);
    case 2:  return __atomic_load_n((uint16_t *)p, __ATOMIC_SEQ_CST);
    case 4:  return __atomic_load_n((uint32_t *)p, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)p, __ATOMIC_SEQ_CST);
    }
}

static int op_arith(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    (void)vm;
    if (insn->lock && insn->ops[0].kind == OCERZ_OPK_MEM) {
        uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
        uint64_t b = ocerz_read_op(cpu, insn, &insn->ops[1]);

        int cin = (cpu->rflags & OCERZ_CF) ? 1 : 0;
        for (;;) {
            uint64_t a = ocerz_atomic_load(addr, size);
            uint64_t res;
            switch (insn->op) {
            case OCERZ_OP_ADD: res = a + b; ocerz_flags_add(cpu, size, a, b, 0, res); break;
            case OCERZ_OP_ADC: res = a + b + (uint64_t)cin; ocerz_flags_add(cpu, size, a, b, cin, res); break;
            case OCERZ_OP_SUB: res = a - b; ocerz_flags_sub(cpu, size, a, b, 0, res); break;
            case OCERZ_OP_SBB: res = a - b - (uint64_t)cin; ocerz_flags_sub(cpu, size, a, b, cin, res); break;
            case OCERZ_OP_AND: res = a & b; ocerz_flags_logic(cpu, size, res); break;
            case OCERZ_OP_OR:  res = a | b; ocerz_flags_logic(cpu, size, res); break;
            case OCERZ_OP_XOR: res = a ^ b; ocerz_flags_logic(cpu, size, res); break;
            default: return ocerz_unimpl(vm, cpu, insn, "locked-arith");
            }
            uint64_t seen = a;
            if (ocerz_atomic_cmpxchg(addr, size, &seen, res))
                break;
        }
        return OCERZ_STEP_OK;
    }
    uint64_t a = ocerz_read_op(cpu, insn, &insn->ops[0]);
    uint64_t b = ocerz_read_op(cpu, insn, &insn->ops[1]);
    int cin = (cpu->rflags & OCERZ_CF) ? 1 : 0;
    uint64_t res;
    switch (insn->op) {
    case OCERZ_OP_ADD:
        res = a + b;
        ocerz_flags_add(cpu, size, a, b, 0, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_ADC:
        res = a + b + (uint64_t)cin;
        ocerz_flags_add(cpu, size, a, b, cin, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_SUB:
        res = a - b;
        ocerz_flags_sub(cpu, size, a, b, 0, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_SBB:
        res = a - b - (uint64_t)cin;
        ocerz_flags_sub(cpu, size, a, b, cin, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_AND:
        res = a & b;
        ocerz_flags_logic(cpu, size, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_OR:
        res = a | b;
        ocerz_flags_logic(cpu, size, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_XOR:
        res = a ^ b;
        ocerz_flags_logic(cpu, size, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_CMP:
        res = a - b;
        ocerz_flags_sub(cpu, size, a, b, 0, res);
        break;
    case OCERZ_OP_TEST:
        res = a & b;
        ocerz_flags_logic(cpu, size, res);
        break;
    default:
        return ocerz_unimpl(vm, cpu, insn, "arith");
    }
    return OCERZ_STEP_OK;
}

static int op_incdecnegnot(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    (void)vm;
    if (insn->lock && insn->ops[0].kind == OCERZ_OPK_MEM) {
        uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
        for (;;) {
            uint64_t a = ocerz_atomic_load(addr, size);
            uint64_t res;
            switch (insn->op) {
            case OCERZ_OP_INC: res = a + 1; ocerz_flags_inc(cpu, size, res); break;
            case OCERZ_OP_DEC: res = a - 1; ocerz_flags_dec(cpu, size, res); break;
            case OCERZ_OP_NEG: res = (uint64_t)0 - a; ocerz_flags_sub(cpu, size, 0, a, 0, res); break;
            case OCERZ_OP_NOT: res = ~a; break;
            default: return ocerz_unimpl(vm, cpu, insn, "locked-incdec");
            }
            uint64_t seen = a;
            if (ocerz_atomic_cmpxchg(addr, size, &seen, res))
                break;
        }
        return OCERZ_STEP_OK;
    }
    uint64_t a = ocerz_read_op(cpu, insn, &insn->ops[0]);
    uint64_t res;
    switch (insn->op) {
    case OCERZ_OP_INC:
        res = a + 1;
        ocerz_flags_inc(cpu, size, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_DEC:
        res = a - 1;
        ocerz_flags_dec(cpu, size, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_NEG:
        res = (uint64_t)0 - a;
        ocerz_flags_sub(cpu, size, 0, a, 0, res);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    case OCERZ_OP_NOT:
        res = ~a;
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        break;
    default:
        return ocerz_unimpl(vm, cpu, insn, "incdec");
    }
    return OCERZ_STEP_OK;
}

static int op_mul(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    (void)vm;
    if (insn->op == OCERZ_OP_MUL) {
        uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[0]);
        uint64_t acc = read_acc(cpu, size);
        if (size == 8) {
            __uint128_t p = (__uint128_t)acc * (__uint128_t)src;
            uint64_t lo = (uint64_t)p;
            uint64_t hi = (uint64_t)(p >> 64);
            cpu->gpr[OCERZ_RAX] = lo;
            cpu->gpr[OCERZ_RDX] = hi;
            ocerz_flags_mul(cpu, size, lo, hi);
        } else {
            uint64_t p = ocerz_trunc(acc, size) * ocerz_trunc(src, size);
            uint64_t lo = ocerz_trunc(p, size);
            uint64_t hi = ocerz_trunc(p >> (size * 8), size);
            if (size == 1) {
                ocerz_write_gpr(cpu, OCERZ_RAX, 2, 0, p & 0xffff);
            } else {
                write_acc(cpu, size, lo);
                ocerz_write_gpr(cpu, OCERZ_RDX, size, 0, hi);
            }
            ocerz_flags_mul(cpu, size, lo, hi);
        }
        return OCERZ_STEP_OK;
    }

    if (insn->nops == 1) {
        int64_t src = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[0]), size);
        int64_t acc = ocerz_sext(read_acc(cpu, size), size);
        if (size == 8) {
            __int128_t p = (__int128_t)acc * (__int128_t)src;
            uint64_t lo = (uint64_t)p;
            uint64_t hi = (uint64_t)(p >> 64);
            cpu->gpr[OCERZ_RAX] = lo;
            cpu->gpr[OCERZ_RDX] = hi;
            ocerz_flags_imul(cpu, size, lo, hi);
        } else {
            int64_t p = acc * src;
            uint64_t lo = ocerz_trunc((uint64_t)p, size);
            uint64_t hi = ocerz_trunc((uint64_t)p >> (size * 8), size);
            if (size == 1) {
                ocerz_write_gpr(cpu, OCERZ_RAX, 2, 0, (uint64_t)p & 0xffff);
            } else {
                write_acc(cpu, size, lo);
                ocerz_write_gpr(cpu, OCERZ_RDX, size, 0, hi);
            }
            ocerz_flags_imul(cpu, size, lo, hi);
        }
        return OCERZ_STEP_OK;
    }

    int64_t a, b;
    if (insn->nops == 3) {
        a = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[1]), insn->ops[1].size);
        b = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[2]), insn->ops[2].size);
    } else {
        a = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[0]), size);
        b = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[1]), size);
    }
    if (size == 8) {
        __int128_t p = (__int128_t)a * (__int128_t)b;
        uint64_t lo = (uint64_t)p;
        uint64_t hi = (uint64_t)(p >> 64);
        ocerz_flags_imul(cpu, size, lo, hi);
        ocerz_write_op(cpu, insn, &insn->ops[0], lo);
    } else {
        int64_t p = a * b;
        uint64_t lo = ocerz_trunc((uint64_t)p, size);
        uint64_t hi = ocerz_trunc((uint64_t)p >> (size * 8), size);
        ocerz_flags_imul(cpu, size, lo, hi);
        ocerz_write_op(cpu, insn, &insn->ops[0], lo);
    }
    return OCERZ_STEP_OK;
}

static int op_div(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    uint64_t divisor = ocerz_read_op(cpu, insn, &insn->ops[0]);
    (void)vm;
    if (ocerz_trunc(divisor, size) == 0)
        return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "integer divide by zero");

    if (insn->op == OCERZ_OP_DIV) {
        if (size == 1) {
            uint64_t num = ocerz_read_gpr(cpu, OCERZ_RAX, 2, 0);
            uint64_t d = divisor & 0xff;
            uint64_t q = num / d;
            uint64_t r = num % d;
            if (q > 0xff)
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
            ocerz_write_gpr(cpu, OCERZ_RAX, 1, 0, q);
            ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, r);
        } else if (size == 8) {
            __uint128_t num = ((__uint128_t)cpu->gpr[OCERZ_RDX] << 64) | cpu->gpr[OCERZ_RAX];
            __uint128_t d = divisor;
            __uint128_t q = num / d;
            __uint128_t r = num % d;
            if (q > (__uint128_t)~(uint64_t)0)
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
            cpu->gpr[OCERZ_RAX] = (uint64_t)q;
            cpu->gpr[OCERZ_RDX] = (uint64_t)r;
        } else {
            uint64_t lo = read_acc(cpu, size);
            uint64_t hi = ocerz_read_gpr(cpu, OCERZ_RDX, size, 0);
            uint64_t num = (hi << (size * 8)) | lo;
            uint64_t d = ocerz_trunc(divisor, size);
            uint64_t q = num / d;
            uint64_t r = num % d;
            if (q > ocerz_mask(size))
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
            write_acc(cpu, size, q);
            ocerz_write_gpr(cpu, OCERZ_RDX, size, 0, r);
        }
        return OCERZ_STEP_OK;
    }

    if (size == 1) {
        int64_t num = ocerz_sext(ocerz_read_gpr(cpu, OCERZ_RAX, 2, 0), 2);
        int64_t d = ocerz_sext(divisor, 1);
        int64_t q = num / d;
        int64_t r = num % d;
        if (q < -128 || q > 127)
            return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
        ocerz_write_gpr(cpu, OCERZ_RAX, 1, 0, (uint64_t)q);
        ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, (uint64_t)r);
    } else if (size == 8) {
        __int128_t num = ((__int128_t)(int64_t)cpu->gpr[OCERZ_RDX] << 64) | cpu->gpr[OCERZ_RAX];
        __int128_t d = (int64_t)divisor;
        __int128_t q = num / d;
        __int128_t r = num % d;
        if (q < -(__int128_t)1 - (__int128_t)((__uint128_t)~(uint64_t)0 >> 1) || q > (__int128_t)((__uint128_t)~(uint64_t)0 >> 1))
            return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
        cpu->gpr[OCERZ_RAX] = (uint64_t)q;
        cpu->gpr[OCERZ_RDX] = (uint64_t)r;
    } else {
        uint64_t lo = read_acc(cpu, size);
        uint64_t hi = ocerz_read_gpr(cpu, OCERZ_RDX, size, 0);
        int64_t num = ocerz_sext((hi << (size * 8)) | lo, size * 2);
        int64_t d = ocerz_sext(divisor, size);
        int64_t q = num / d;
        int64_t r = num % d;
        int64_t qmin = -(int64_t)1 - (int64_t)(ocerz_mask(size) >> 1);
        int64_t qmax = (int64_t)(ocerz_mask(size) >> 1);
        if (q < qmin || q > qmax)
            return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "divide quotient overflow");   /* x86 #DE: Darwin reports FPE_INTDIV for both */
        write_acc(cpu, size, (uint64_t)q);
        ocerz_write_gpr(cpu, OCERZ_RDX, size, 0, (uint64_t)r);
    }
    return OCERZ_STEP_OK;
}

static int op_shift(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    int bits = size * 8;
    unsigned mask = (size == 8) ? 63u : 31u;
    uint64_t val = ocerz_read_op(cpu, insn, &insn->ops[0]);
    unsigned cnt = (unsigned)(ocerz_read_op(cpu, insn, &insn->ops[1]) & mask);
    uint64_t res;
    (void)vm;
    if (cnt == 0)
        return OCERZ_STEP_OK;
    switch (insn->op) {
    case OCERZ_OP_SHL:
        res = ocerz_trunc(val << cnt, size);
        ocerz_flags_shl(cpu, size, val, cnt, res);
        break;
    case OCERZ_OP_SHR:
        res = ocerz_trunc(val, size) >> cnt;
        ocerz_flags_shr(cpu, size, val, cnt, res);
        break;
    case OCERZ_OP_SAR:
        res = ocerz_trunc((uint64_t)(ocerz_sext(val, size) >> cnt), size);
        ocerz_flags_sar(cpu, size, val, cnt, res);
        break;
    default:
        return ocerz_unimpl(vm, cpu, insn, "shift");
    }
    (void)bits;
    ocerz_write_op(cpu, insn, &insn->ops[0], res);
    return OCERZ_STEP_OK;
}

static int op_rotate(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    int bits = size * 8;
    unsigned mask = (size == 8) ? 63u : 31u;
    uint64_t val = ocerz_trunc(ocerz_read_op(cpu, insn, &insn->ops[0]), size);
    unsigned masked = (unsigned)(ocerz_read_op(cpu, insn, &insn->ops[1]) & mask);
    int cin = (cpu->rflags & OCERZ_CF) ? 1 : 0;
    (void)vm;

    if (insn->op == OCERZ_OP_ROL || insn->op == OCERZ_OP_ROR) {
        unsigned rc = masked % (unsigned)bits;
        if (masked == 0)
            return OCERZ_STEP_OK;
        uint64_t res;
        if (rc == 0)
            res = val;
        else if (insn->op == OCERZ_OP_ROL)
            res = ocerz_trunc((val << rc) | (val >> (bits - rc)), size);
        else
            res = ocerz_trunc((val >> rc) | (val << (bits - rc)), size);
        int cf, of = 0;
        if (insn->op == OCERZ_OP_ROL)
            cf = (int)(res & 1);
        else
            cf = ocerz_msb(res, size);
        if (masked == 1) {
            if (insn->op == OCERZ_OP_ROL)
                of = cf ^ ocerz_msb(res, size);
            else
                of = ocerz_msb(res, size) ^ (int)((res >> (bits - 2)) & 1);
        }
        ocerz_flag_assign(cpu, OCERZ_CF, cf);
        if (masked == 1)
            ocerz_flag_assign(cpu, OCERZ_OF, of);
        ocerz_write_op(cpu, insn, &insn->ops[0], res);
        return OCERZ_STEP_OK;
    }

    if (masked == 0)
        return OCERZ_STEP_OK;
    uint64_t res = val;
    int carry = cin;
    for (unsigned i = 0; i < masked; i++) {
        if (insn->op == OCERZ_OP_RCL) {
            int top = ocerz_msb(res, size);
            res = ocerz_trunc((res << 1) | (uint64_t)carry, size);
            carry = top;
        } else {
            int bot = (int)(res & 1);
            res = ocerz_trunc((res >> 1) | ((uint64_t)carry << (bits - 1)), size);
            carry = bot;
        }
    }
    int of = 0;
    if (masked == 1) {
        if (insn->op == OCERZ_OP_RCL)
            of = carry ^ ocerz_msb(res, size);
        else
            of = ocerz_msb(res, size) ^ (int)((res >> (bits - 2)) & 1);
    }
    ocerz_flag_assign(cpu, OCERZ_CF, carry);
    if (masked == 1)
        ocerz_flag_assign(cpu, OCERZ_OF, of);
    ocerz_write_op(cpu, insn, &insn->ops[0], res);
    return OCERZ_STEP_OK;
}

static int op_shiftd(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->ops[0].size;
    int bits = size * 8;
    unsigned mask = (size == 8) ? 63u : 31u;
    uint64_t dst = ocerz_trunc(ocerz_read_op(cpu, insn, &insn->ops[0]), size);
    uint64_t src = ocerz_trunc(ocerz_read_op(cpu, insn, &insn->ops[1]), size);
    unsigned cnt = (unsigned)(ocerz_read_op(cpu, insn, &insn->ops[2]) & mask);
    (void)vm;
    if (cnt == 0)
        return OCERZ_STEP_OK;
    uint64_t res;
    if (insn->op == OCERZ_OP_SHLD) {
        __uint128_t big = ((__uint128_t)dst << bits) | src;
        big <<= cnt;
        res = ocerz_trunc((uint64_t)(big >> bits), size);
        ocerz_flags_shl(cpu, size, dst, cnt, res);
    } else {
        __uint128_t big = ((__uint128_t)src << bits) | dst;
        big >>= cnt;
        res = ocerz_trunc((uint64_t)big, size);
        ocerz_flags_shr(cpu, size, dst, cnt, res);
    }
    ocerz_write_op(cpu, insn, &insn->ops[0], res);
    return OCERZ_STEP_OK;
}

static int op_mov_family(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    switch (insn->op) {
    case OCERZ_OP_MOV: {
        uint64_t v = ocerz_read_op(cpu, insn, &insn->ops[1]);
        ocerz_write_op(cpu, insn, &insn->ops[0], v);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_MOVZX: {
        uint64_t v = ocerz_trunc(ocerz_read_op(cpu, insn, &insn->ops[1]), insn->ops[1].size);
        ocerz_write_op(cpu, insn, &insn->ops[0], v);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_MOVSX:
    case OCERZ_OP_MOVSXD: {
        uint64_t v = (uint64_t)ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[1]), insn->ops[1].size);
        ocerz_write_op(cpu, insn, &insn->ops[0], v);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LEA: {
        uint64_t a = lea_addr(cpu, insn, &insn->ops[1]);
        ocerz_write_op(cpu, insn, &insn->ops[0], a);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_XCHG: {
        if (insn->ops[0].kind == OCERZ_OPK_MEM) {
            uint64_t b = ocerz_read_op(cpu, insn, &insn->ops[1]);
            uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
            uint64_t a = ocerz_atomic_xchg(addr, insn->ops[0].size, b);
            ocerz_write_op(cpu, insn, &insn->ops[1], a);
            return OCERZ_STEP_OK;
        }
        if (insn->ops[1].kind == OCERZ_OPK_MEM) {
            uint64_t a = ocerz_read_op(cpu, insn, &insn->ops[0]);
            uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[1]);
            uint64_t b = ocerz_atomic_xchg(addr, insn->ops[1].size, a);
            ocerz_write_op(cpu, insn, &insn->ops[0], b);
            return OCERZ_STEP_OK;
        }
        uint64_t a = ocerz_read_op(cpu, insn, &insn->ops[0]);
        uint64_t b = ocerz_read_op(cpu, insn, &insn->ops[1]);
        ocerz_write_op(cpu, insn, &insn->ops[0], b);
        ocerz_write_op(cpu, insn, &insn->ops[1], a);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_BSWAP: {
        int size = insn->ops[0].size;
        uint64_t v = ocerz_read_op(cpu, insn, &insn->ops[0]);
        uint64_t r = (size == 8) ? __builtin_bswap64(v) : __builtin_bswap32((uint32_t)v);
        ocerz_write_op(cpu, insn, &insn->ops[0], r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_CMOVCC: {
        uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
        int take = ocerz_cc_eval(cpu, insn->cc);
        uint64_t dst = ocerz_read_op(cpu, insn, &insn->ops[0]);
        ocerz_write_op(cpu, insn, &insn->ops[0], take ? src : dst);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_SETCC: {
        int take = ocerz_cc_eval(cpu, insn->cc);
        ocerz_write_op(cpu, insn, &insn->ops[0], take ? 1u : 0u);
        return OCERZ_STEP_OK;
    }
    default:
        return ocerz_unimpl(vm, cpu, insn, "mov-family");
    }
}

static int op_stack(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    switch (insn->op) {
    case OCERZ_OP_PUSH: {
        int size = insn->opsize;
        uint64_t v = ocerz_read_op(cpu, insn, &insn->ops[0]);
        ocerz_push_mode(cpu, size, v, insn->mode32);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_POP: {

        int size = insn->opsize;
        uint64_t v = ocerz_ld(cpu->gpr[OCERZ_RSP], size);
        ocerz_write_op(cpu, insn, &insn->ops[0], v);
        /* POP ESP takes its new value from the slot, not from the adjustment. */
        if (!(insn->ops[0].kind == OCERZ_OPK_REG && insn->ops[0].reg == OCERZ_RSP))
            cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(cpu->gpr[OCERZ_RSP] + (uint64_t)size,
                                                   insn->mode32);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_PUSHF:
        /* opsize is 8 in long mode -- the decoder still hard-codes it there --
         * and 4 (2 under 0x66) in i386 mode. */
        ocerz_push_mode(cpu, insn->opsize ? insn->opsize : 8, cpu->rflags, insn->mode32);
        return OCERZ_STEP_OK;
    case OCERZ_OP_POPF: {
        uint64_t v = ocerz_pop_mode(cpu, insn->opsize ? insn->opsize : 8, insn->mode32);
        /* Every writable bit lives below bit 12, so POPFW and POPFD restore
         * the same set here; only the number of stack bytes consumed differs,
         * and that came from opsize above. */
        uint64_t writable = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF |
                            OCERZ_TF | OCERZ_DF | OCERZ_OF;
        cpu->rflags = (v & writable) | OCERZ_FLAG_FIXED1 | OCERZ_IF;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LAHF: {
        uint64_t ah = (cpu->rflags & (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF))
                      | OCERZ_FLAG_FIXED1;
        ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, ah);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_SAHF: {
        uint64_t ah = ocerz_read_gpr(cpu, OCERZ_RAX, 1, 1);
        uint64_t m = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF;
        cpu->rflags = (cpu->rflags & ~m) | (ah & m);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LEAVE: {
        /* SDM: the STACK ADDRESS size moves the whole of ESP/RSP from EBP/RBP,
         * while the OPERAND size decides how much of EBP/RBP the pop rewrites.
         * The two only differ for the 0x66 form in 32-bit mode, where ESP is
         * still set in full but only BP is popped. */
        int m32 = insn->mode32;
        int size = insn->opsize ? insn->opsize : 8;
        uint64_t bp = m32 ? (uint32_t)cpu->gpr[OCERZ_RBP] : cpu->gpr[OCERZ_RBP];
        uint64_t v = ocerz_ld(bp, size);
        cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(bp + (uint64_t)size, m32);
        ocerz_write_gpr(cpu, OCERZ_RBP, size, 0, v);
        return OCERZ_STEP_OK;
    }
    default:
        return ocerz_unimpl(vm, cpu, insn, "stack");
    }
}

static int op_cbw_cwd(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    if (insn->op == OCERZ_OP_CBW) {
        if (insn->opsize == 2) {
            int64_t v = ocerz_sext(ocerz_read_gpr(cpu, OCERZ_RAX, 1, 0), 1);
            ocerz_write_gpr(cpu, OCERZ_RAX, 2, 0, (uint64_t)v);
        } else if (insn->opsize == 4) {
            int64_t v = ocerz_sext(ocerz_read_gpr(cpu, OCERZ_RAX, 2, 0), 2);
            ocerz_write_gpr(cpu, OCERZ_RAX, 4, 0, (uint64_t)v);
        } else {
            int64_t v = ocerz_sext(ocerz_read_gpr(cpu, OCERZ_RAX, 4, 0), 4);
            cpu->gpr[OCERZ_RAX] = (uint64_t)v;
        }
        return OCERZ_STEP_OK;
    }
    if (insn->opsize == 2) {
        int v = ocerz_msb(ocerz_read_gpr(cpu, OCERZ_RAX, 2, 0), 2);
        ocerz_write_gpr(cpu, OCERZ_RDX, 2, 0, v ? 0xffff : 0);
    } else if (insn->opsize == 4) {
        int v = ocerz_msb(ocerz_read_gpr(cpu, OCERZ_RAX, 4, 0), 4);
        ocerz_write_gpr(cpu, OCERZ_RDX, 4, 0, v ? 0xffffffffu : 0);
    } else {
        int v = ocerz_msb(cpu->gpr[OCERZ_RAX], 8);
        cpu->gpr[OCERZ_RDX] = v ? ~(uint64_t)0 : 0;
    }
    return OCERZ_STEP_OK;
}

static int op_branch(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    switch (insn->op) {
    case OCERZ_OP_JMP: {
        if (insn->ops[0].kind == OCERZ_OPK_IMM)
            cpu->rip = insn->ops[0].imm;
        else {
            cpu->rip = ocerz_read_op(cpu, insn, &insn->ops[0]);
            if (ocerz_cftrap_on && cpu->rip - 0x7ff840000000ull < 0x10000000ull)
                ocerz_cftrap(cpu, insn->rip, cpu->rip, "jmp");
        }
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_JCC:
        if (ocerz_cc_eval(cpu, insn->cc))
            cpu->rip = insn->ops[0].imm;
        return OCERZ_STEP_OK;
    case OCERZ_OP_JRCXZ: {
        uint64_t c = cpu->gpr[OCERZ_RCX];
        if (insn->addrsize == 4)
            c = (uint32_t)c;
        else if (insn->addrsize == 2)
            c = (uint16_t)c;   /* JCXZ: 0x67 in 32-bit mode */
        if (c == 0)
            cpu->rip = insn->ops[0].imm;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LOOP:
    case OCERZ_OP_LOOPE:
    case OCERZ_OP_LOOPNE: {
        uint64_t c;
        if (insn->addrsize == 4) {
            c = (uint32_t)cpu->gpr[OCERZ_RCX] - 1;
            ocerz_write_gpr(cpu, OCERZ_RCX, 4, 0, c);
            c = (uint32_t)c;
        } else if (insn->addrsize == 2) {
            /* LOOPW: only CX counts and only CX is written back. */
            c = (uint16_t)((uint16_t)cpu->gpr[OCERZ_RCX] - 1);
            ocerz_write_gpr(cpu, OCERZ_RCX, 2, 0, c);
        } else {
            c = cpu->gpr[OCERZ_RCX] - 1;
            cpu->gpr[OCERZ_RCX] = c;
        }
        int zf = (cpu->rflags & OCERZ_ZF) ? 1 : 0;
        int branch = (c != 0);
        if (insn->op == OCERZ_OP_LOOPE)
            branch = branch && zf;
        else if (insn->op == OCERZ_OP_LOOPNE)
            branch = branch && !zf;
        if (branch)
            cpu->rip = insn->ops[0].imm;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_CALL: {
        uint64_t target;
        if (insn->ops[0].kind == OCERZ_OPK_IMM)
            target = insn->ops[0].imm;
        else {
            target = ocerz_read_op(cpu, insn, &insn->ops[0]);
            if (ocerz_cftrap_on && target - 0x7ff840000000ull < 0x10000000ull)
                ocerz_cftrap(cpu, insn->rip, target, "call");
        }
        /* opsize is the near-branch width: 8 in long mode (forced, 0x66
         * ignored), 4 or 2 in i386 mode. */
        ocerz_push_mode(cpu, insn->opsize ? insn->opsize : 8, cpu->rip, insn->mode32);
        cpu->rip = target;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_RET: {
        int size = insn->opsize ? insn->opsize : 8;
        uint64_t ret = ocerz_pop_mode(cpu, size, insn->mode32);
        if (insn->nops == 1)
            cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(
                cpu->gpr[OCERZ_RSP] + ocerz_trunc(insn->ops[0].imm, 2), insn->mode32);
        cpu->rip = ret;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_MOVSEG: {

        uint32_t sel = (uint32_t)ocerz_read_op(cpu, insn, &insn->ops[0]);
        unsigned seg = (unsigned)insn->ops[1].imm;
        uint64_t base = ocerz_ldt_base(sel);
        if (base) {
            if (seg == 4)
                cpu->fs_base = base;
            else if (seg == 5)
                cpu->gs_base = base;
        }
        if (seg < 6)
            cpu->seg_sel[seg] = (uint16_t)sel;
        if (seg == 1)
            cpu->cs_sel = (uint16_t)sel;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_JMPF:
    case OCERZ_OP_CALLF: {
        /* Stack slots stay 8 bytes wide in long mode: that is what this path
         * has always pushed and what the 64-bit tests pin.  In i386 mode the
         * selector and the return offset each take one operand-size slot. */
        int m32 = insn->mode32;
        int sz = insn->opsize ? insn->opsize : 4;
        int ssz = m32 ? sz : 8;
        uint64_t off;
        uint32_t sel;
        if (insn->nops == 2) {
            /* ptr16:32 direct form (0x9a / 0xea), i386 only.  The decoder
             * records it selector-first; the encoding is offset-first. */
            sel = (uint32_t)insn->ops[0].imm;
            off = ocerz_trunc(insn->ops[1].imm, sz);
        } else {
            uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
            off = ocerz_ld(ea, sz);
            sel = (uint32_t)ocerz_ld(ea + (uint64_t)sz, 2);
        }
        if (insn->op == OCERZ_OP_CALLF) {
            ocerz_push_mode(cpu, ssz, cpu->cs_sel, m32);
            ocerz_push_mode(cpu, ssz, insn->rip + insn->len, m32);
        }
        return far_transfer(cpu, sel, off);
    }
    case OCERZ_OP_RETF: {
        int m32 = insn->mode32;
        int ssz = m32 ? (insn->opsize ? insn->opsize : 4) : 8;
        uint64_t off = ocerz_pop_mode(cpu, ssz, m32);
        uint32_t sel = (uint32_t)ocerz_pop_mode(cpu, ssz, m32);
        if (insn->nops == 1)
            cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(
                cpu->gpr[OCERZ_RSP] + ocerz_trunc(insn->ops[0].imm, 2), m32);
        return far_transfer(cpu, sel, off);
    }
    case OCERZ_OP_IRET: {
        int m32 = insn->mode32;
        int sz = insn->opsize ? insn->opsize : 8;
        uint64_t sp = cpu->gpr[OCERZ_RSP];
        uint64_t rip = ocerz_ld(sp, sz);

        uint32_t cs = (uint32_t)ocerz_ld(sp + (uint64_t)sz, sz);
        uint64_t flags = ocerz_ld(sp + (uint64_t)sz * 2, sz);
        uint64_t newsp = ocerz_ld(sp + (uint64_t)sz * 3, sz);
        cpu->rflags = flags | 0x2;
        cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(newsp, m32);
        /* A zero CS on the stack is not a mode decision, it is a frame this
         * emulator built itself; keep the current selector and mode. */
        if (!cs) {
            cpu->rip = m32 ? (uint32_t)rip : rip;
            return OCERZ_STEP_OK;
        }
        return far_transfer(cpu, cs, rip);
    }
    default:
        return ocerz_unimpl(vm, cpu, insn, "branch");
    }
}

/* Is this code selector a 32-bit one?
 *
 * A code descriptor's L bit (53) says 64-bit and its D bit (54) says 32-bit,
 * and the two are mutually exclusive, so the answer is "L then D".  Everything
 * this emulator can see about a selector comes from the LDT that the guest
 * installs through i386_set_ldt: ocerz_ldt_is_big() answers 0 for a GDT
 * selector and for an absent entry.
 *
 * That single fact is what makes mode ENTRY and mode EXIT the same rule read
 * in opposite directions.  wine's WoW64 thunk far-jumps to an LDT selector it
 * installed with D=1, which lands here as 1; the 32-bit side far-returns to
 * the flat 64-bit CS, which is a GDT selector and lands here as 0.  There is
 * no separate "leave 32-bit mode" path to forget to write, and no way for a
 * thread to be stranded in 32-bit mode by a transfer that did not name a
 * 32-bit code segment. */
static int cs_is_32bit(uint32_t sel)
{
    if (ocerz_ldt_is_long(sel))
        return 0;
    return ocerz_ldt_is_big(sel) ? 1 : 0;
}

/* Every far transfer -- JMPF, CALLF, RETF, IRET -- funnels through here, so
 * the mode, the selector and EIP/RIP can never disagree. */
static int far_transfer(OcerzCPU *cpu, uint32_t sel, uint64_t off)
{
    int to32 = cs_is_32bit(sel);
    cpu->cs_sel = (uint16_t)sel;
    cpu->seg_sel[OCERZ_SREG_CS] = (uint16_t)sel;
    cpu->mode32 = (uint8_t)to32;
    /* EIP has 32 significant bits; a stale high half from the 64-bit side must
     * not survive the switch. */
    cpu->rip = to32 ? (uint64_t)(uint32_t)off : off;
    { static int modelog = -1;
      if (modelog < 0) modelog = getenv("OCERZ_MODELOG") ? 1 : 0;
      if (modelog)
          fprintf(stderr, "ocerz: far transfer cs=%#x -> %s eip=%#llx\n",
                  sel, to32 ? "i386" : "long", (unsigned long long)cpu->rip); }
    return OCERZ_STEP_OK;
}

/* ---------------------------------------------------------------------------
 * The i386-only instructions.
 *
 * None of these exists in long mode -- the decoder only emits them when it was
 * called with mode32=1 -- so this whole function is unreachable from 64-bit
 * execution and nothing above it changes shape to accommodate it.
 *
 * The BCD/ASCII adjusts are transcriptions of the SDM Vol.2 pseudocode rather
 * than reimplementations of what some other emulator does, and where the SDM
 * says a flag is UNDEFINED this code says so at the site and then does one
 * specific thing: it leaves that flag alone.  That is both the cheapest choice
 * and the one that makes a mistaken dependency in guest code fail the same way
 * on every run instead of intermittently.
 * ------------------------------------------------------------------------- */

/* AL, AH and AX, spelled once so the adjust handlers read like the SDM. */
static uint8_t  get_al(const OcerzCPU *cpu) { return (uint8_t)cpu->gpr[OCERZ_RAX]; }
static uint8_t  get_ah(const OcerzCPU *cpu) { return (uint8_t)(cpu->gpr[OCERZ_RAX] >> 8); }
static uint16_t get_ax(const OcerzCPU *cpu) { return (uint16_t)cpu->gpr[OCERZ_RAX]; }
static void set_al(OcerzCPU *cpu, uint8_t v)  { ocerz_write_gpr(cpu, OCERZ_RAX, 1, 0, v); }
static void set_ah(OcerzCPU *cpu, uint8_t v)  { ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, v); }
static void set_ax(OcerzCPU *cpu, uint16_t v) { ocerz_write_gpr(cpu, OCERZ_RAX, 2, 0, v); }

/* #BR (BOUND range exceeded, vector 5) and #OF (INTO, vector 4).  Darwin has
 * no signal that means either one; both are faults, so rip is rewound to the
 * faulting instruction the way div_trap() does it, and they are reported as
 * SIGTRAP -- the signal INT3 already uses here for "the guest asked for a
 * trap".  With no guest handler installed both stay fatal. */
static int i386_trap(OcerzCPU *cpu, const X86Insn *insn, const char *msg)
{
    uint64_t next = cpu->rip;
    cpu->rip = insn->rip;
    if (ocerz_signal_deliver(cpu, OCERZ_SIGTRAP, insn->rip, 0, 0))
        return OCERZ_STEP_REDIRECT;
    cpu->rip = next;
    return trap_fatal(insn, msg);
}

/* PUSHA/POPA move the eight GPRs in the fixed order the SDM gives.  The ESP
 * slot PUSHA writes holds the value ESP had BEFORE the first push, and the one
 * POPA reads is discarded: ESP ends where the eight pops leave it, not where
 * the saved image says. */
static int op_pusha(OcerzCPU *cpu, const X86Insn *insn)
{
    static const uint8_t order[8] = {
        OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX,
        OCERZ_RSP, OCERZ_RBP, OCERZ_RSI, OCERZ_RDI,
    };
    int size = insn->opsize ? insn->opsize : 4;
    uint64_t saved_sp = cpu->gpr[OCERZ_RSP];
    for (int i = 0; i < 8; i++) {
        uint64_t v = (order[i] == OCERZ_RSP) ? saved_sp : cpu->gpr[order[i]];
        ocerz_push_mode(cpu, size, v, insn->mode32);
    }
    return OCERZ_STEP_OK;
}

static int op_popa(OcerzCPU *cpu, const X86Insn *insn)
{
    static const uint8_t order[8] = {
        OCERZ_RDI, OCERZ_RSI, OCERZ_RBP, OCERZ_RSP,
        OCERZ_RBX, OCERZ_RDX, OCERZ_RCX, OCERZ_RAX,
    };
    int size = insn->opsize ? insn->opsize : 4;
    for (int i = 0; i < 8; i++) {
        uint64_t v = ocerz_pop_mode(cpu, size, insn->mode32);
        if (order[i] == OCERZ_RSP)
            continue;   /* the saved ESP image is discarded, per the SDM */
        ocerz_write_gpr(cpu, order[i], size, 0, v);
    }
    return OCERZ_STEP_OK;
}

static int op_i386(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    switch (insn->op) {

    case OCERZ_OP_PUSHA:
        return op_pusha(cpu, insn);
    case OCERZ_OP_POPA:
        return op_popa(cpu, insn);

    case OCERZ_OP_PUSHSEG: {
        unsigned seg = (unsigned)insn->ops[0].imm;
        int size = insn->opsize ? insn->opsize : 4;
        /* PUSH sreg consumes a whole operand-size slot with the selector
         * zero-extended into it. */
        ocerz_push_mode(cpu, size, seg < 6 ? cpu->seg_sel[seg] : 0, insn->mode32);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_POPSEG: {
        unsigned seg = (unsigned)insn->ops[0].imm;
        int size = insn->opsize ? insn->opsize : 4;
        uint32_t sel = (uint32_t)ocerz_pop_mode(cpu, size, insn->mode32);
        uint64_t base;
        if (seg < 6)
            cpu->seg_sel[seg] = (uint16_t)sel;
        /* FS and GS are the two whose base this emulator applies to addresses,
         * so loading one has to move the base too -- exactly what MOVSEG does.
         * ES/SS/DS are flat here and carry no base. */
        base = ocerz_ldt_base(sel);
        if (base) {
            if (seg == OCERZ_SREG_FS)
                cpu->fs_base = base;
            else if (seg == OCERZ_SREG_GS)
                cpu->gs_base = base;
        }
        return OCERZ_STEP_OK;
    }

    /* DAA.  SDM: CF and AF as computed below, SF/ZF/PF from the result, OF
     * UNDEFINED -- left untouched. */
    case OCERZ_OP_DAA: {
        uint8_t old_al = get_al(cpu);
        int old_cf = (cpu->rflags & OCERZ_CF) != 0;
        uint8_t al = old_al;
        int carry;
        ocerz_flag_assign(cpu, OCERZ_CF, 0);
        if ((al & 0x0f) > 9 || (cpu->rflags & OCERZ_AF)) {
            unsigned t = (unsigned)al + 6;
            al = (uint8_t)t;
            carry = old_cf || (t > 0xff);
            ocerz_flag_assign(cpu, OCERZ_AF, 1);
        } else {
            carry = 0;
            ocerz_flag_assign(cpu, OCERZ_AF, 0);
        }
        ocerz_flag_assign(cpu, OCERZ_CF, carry);
        /* The second test overrides CF in BOTH directions for DAA. */
        if (old_al > 0x99 || old_cf) {
            al = (uint8_t)(al + 0x60);
            ocerz_flag_assign(cpu, OCERZ_CF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_CF, 0);
        }
        set_al(cpu, al);
        ocerz_flags_szp(cpu, 1, al);
        return OCERZ_STEP_OK;
    }

    /* DAS.  Same shape as DAA with one asymmetry that is in the SDM and is
     * easy to miss: its second test has no ELSE, so a CF raised by the first
     * adjustment survives when the second does not fire. */
    case OCERZ_OP_DAS: {
        uint8_t old_al = get_al(cpu);
        int old_cf = (cpu->rflags & OCERZ_CF) != 0;
        uint8_t al = old_al;
        ocerz_flag_assign(cpu, OCERZ_CF, 0);
        if ((al & 0x0f) > 9 || (cpu->rflags & OCERZ_AF)) {
            int borrow = (al < 6);
            al = (uint8_t)(al - 6);
            ocerz_flag_assign(cpu, OCERZ_CF, old_cf || borrow);
            ocerz_flag_assign(cpu, OCERZ_AF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_AF, 0);
        }
        if (old_al > 0x99 || old_cf) {
            al = (uint8_t)(al - 0x60);
            ocerz_flag_assign(cpu, OCERZ_CF, 1);
        }
        set_al(cpu, al);
        ocerz_flags_szp(cpu, 1, al);   /* OF UNDEFINED: left untouched */
        return OCERZ_STEP_OK;
    }

    /* AAA.  SDM: AF and CF defined, OF/SF/ZF/PF UNDEFINED -- all four left
     * untouched.  The adjustment is written as AX := AX + 0x106 because that
     * is what the SDM says; it differs from "AL += 6 with wrap, AH += 1" for
     * AL >= 0xfa, where the carry out of AL reaches AH a second time. */
    case OCERZ_OP_AAA: {
        if ((get_al(cpu) & 0x0f) > 9 || (cpu->rflags & OCERZ_AF)) {
            set_ax(cpu, (uint16_t)(get_ax(cpu) + 0x106));
            ocerz_flag_assign(cpu, OCERZ_AF, 1);
            ocerz_flag_assign(cpu, OCERZ_CF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_AF, 0);
            ocerz_flag_assign(cpu, OCERZ_CF, 0);
        }
        set_al(cpu, (uint8_t)(get_al(cpu) & 0x0f));
        return OCERZ_STEP_OK;
    }

    /* AAS.  SDM: AX := AX - 6 first (so a borrow out of AL already reaches
     * AH), then AH := AH - 1.  AF/CF defined, OF/SF/ZF/PF UNDEFINED. */
    case OCERZ_OP_AAS: {
        if ((get_al(cpu) & 0x0f) > 9 || (cpu->rflags & OCERZ_AF)) {
            set_ax(cpu, (uint16_t)(get_ax(cpu) - 6));
            set_ah(cpu, (uint8_t)(get_ah(cpu) - 1));
            ocerz_flag_assign(cpu, OCERZ_AF, 1);
            ocerz_flag_assign(cpu, OCERZ_CF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_AF, 0);
            ocerz_flag_assign(cpu, OCERZ_CF, 0);
        }
        set_al(cpu, (uint8_t)(get_al(cpu) & 0x0f));
        return OCERZ_STEP_OK;
    }

    /* AAM imm8.  A real division, so base 0 raises #DE exactly as DIV does and
     * goes out through the same delivery path.  SF/ZF/PF follow AL; OF/AF/CF
     * UNDEFINED and left untouched. */
    case OCERZ_OP_AAM: {
        unsigned base = (unsigned)(insn->ops[0].imm & 0xff);
        uint8_t al;
        if (base == 0)
            return div_trap(cpu, insn, OCERZ_FPE_INTDIV, "AAM with base 0");
        al = get_al(cpu);
        set_ah(cpu, (uint8_t)(al / base));
        set_al(cpu, (uint8_t)(al % base));
        ocerz_flags_szp(cpu, 1, get_al(cpu));
        return OCERZ_STEP_OK;
    }

    /* AAD imm8.  Cannot fault.  SF/ZF/PF follow AL; OF/AF/CF UNDEFINED. */
    case OCERZ_OP_AAD: {
        unsigned base = (unsigned)(insn->ops[0].imm & 0xff);
        uint8_t al = (uint8_t)(get_al(cpu) + (unsigned)get_ah(cpu) * base);
        set_ax(cpu, al);   /* AL := result and AH := 0, in one 16-bit write */
        ocerz_flags_szp(cpu, 1, al);
        return OCERZ_STEP_OK;
    }

    /* SALC (0xd6, undocumented): AL := CF ? 0xff : 0.  No flags. */
    case OCERZ_OP_SALC:
        set_al(cpu, (cpu->rflags & OCERZ_CF) ? 0xff : 0x00);
        return OCERZ_STEP_OK;

    /* BOUND r32, m32&32.  SIGNED comparison against a lower bound at m and an
     * upper bound at m+opsize; in range is a no-op, out of range is #BR.  No
     * flags are affected either way. */
    case OCERZ_OP_BOUND: {
        int size = insn->opsize ? insn->opsize : 4;
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[1]);
        int64_t idx = ocerz_sext(ocerz_read_op(cpu, insn, &insn->ops[0]), size);
        int64_t lo = ocerz_sext(ocerz_ld(ea, size), size);
        int64_t hi = ocerz_sext(ocerz_ld(ea + (uint64_t)size, size), size);
        if (idx < lo || idx > hi)
            return i386_trap(cpu, insn, "BOUND range exceeded (#BR)");
        return OCERZ_STEP_OK;
    }

    /* INTO: #OF if OF is set, otherwise nothing at all. */
    case OCERZ_OP_INTO:
        if (cpu->rflags & OCERZ_OF)
            return i386_trap(cpu, insn, "INTO with OF set (#OF)");
        return OCERZ_STEP_OK;

    /* LES/LDS r32, m16:32.  Offset into the register, selector into ES or DS.
     * Both segments are flat in this emulator, so the selector is recorded and
     * no base moves; the register load is the part guest code depends on. */
    case OCERZ_OP_LES:
    case OCERZ_OP_LDS: {
        int size = insn->opsize ? insn->opsize : 4;
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[1]);
        uint64_t off = ocerz_ld(ea, size);
        uint32_t sel = (uint32_t)ocerz_ld(ea + (uint64_t)size, 2);
        unsigned seg = (insn->op == OCERZ_OP_LES) ? OCERZ_SREG_ES : OCERZ_SREG_DS;
        ocerz_write_gpr(cpu, insn->ops[0].reg, size, 0, off);
        cpu->seg_sel[seg] = (uint16_t)sel;
        return OCERZ_STEP_OK;
    }

    default:
        return ocerz_unimpl(vm, cpu, insn, "i386-only");
    }
}

static int op_atomic(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    int mem = (insn->ops[0].kind == OCERZ_OPK_MEM);
    switch (insn->op) {
    case OCERZ_OP_XADD: {
        int size = insn->ops[0].size;
        uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
        if (mem) {
            uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
            uint64_t dst = ocerz_atomic_fetch_add(addr, size, src);
            uint64_t sum = dst + src;
            ocerz_flags_add(cpu, size, dst, src, 0, sum);
            ocerz_write_op(cpu, insn, &insn->ops[1], dst);
            return OCERZ_STEP_OK;
        }
        uint64_t dst = ocerz_read_op(cpu, insn, &insn->ops[0]);
        uint64_t sum = dst + src;
        ocerz_flags_add(cpu, size, dst, src, 0, sum);
        ocerz_write_op(cpu, insn, &insn->ops[1], dst);
        ocerz_write_op(cpu, insn, &insn->ops[0], sum);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_CMPXCHG: {
        int size = insn->ops[0].size;
        uint64_t acc = read_acc(cpu, size);
        /* Rosetta (the golden oracle for the guest tests) sets the flags as
         * (dest - accumulator) and writes the accumulator in both cases (so
         * the 32-bit form zero-extends rax on a match as well). */
        if (mem) {
            uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
            uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
            uint64_t seen = acc;
            int ok = ocerz_atomic_cmpxchg(addr, size, &seen, src);
            ocerz_flags_sub(cpu, size, seen, acc, 0, seen - acc);
            write_acc(cpu, size, ok ? acc : seen);
            return OCERZ_STEP_OK;
        }
        uint64_t d = ocerz_read_op(cpu, insn, &insn->ops[0]);
        ocerz_flags_sub(cpu, size, d, acc, 0, d - acc);
        if (ocerz_trunc(acc, size) == ocerz_trunc(d, size)) {
            uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
            ocerz_write_op(cpu, insn, &insn->ops[0], src);
            write_acc(cpu, size, acc);
        } else {
            write_acc(cpu, size, d);
        }
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_CMPXCHGXB: {
        uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
        if (insn->opsize == 8) {
            uint64_t expected = ((uint64_t)(uint32_t)cpu->gpr[OCERZ_RDX] << 32) |
                                (uint32_t)cpu->gpr[OCERZ_RAX];
            uint64_t store = ((uint64_t)(uint32_t)cpu->gpr[OCERZ_RCX] << 32) |
                             (uint32_t)cpu->gpr[OCERZ_RBX];
            uint64_t seen = expected;
            int ok = ocerz_atomic_cmpxchg(addr, 8, &seen, store);
            if (ok) {
                ocerz_flag_assign(cpu, OCERZ_ZF, 1);
            } else {
                ocerz_write_gpr(cpu, OCERZ_RAX, 4, 0, (uint32_t)seen);
                ocerz_write_gpr(cpu, OCERZ_RDX, 4, 0, (uint32_t)(seen >> 32));
                ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            }
        } else {
            if ((addr & 15) && getenv("OCERZ_CASLOG")) {
                fprintf(stderr, "ocerz: CMPXCHG16B MISALIGNED addr=%#llx rip=%#llx\n",
                        (unsigned long long)addr, (unsigned long long)insn->rip);
                static int once16 = 0;
                if (!once16) {
                    once16 = 1;
#define ULL(x) (unsigned long long)(x)
                    fprintf(stderr, "ocerz: CAS16DUMP rax=%#llx rdx=%#llx rbx=%#llx rcx=%#llx r9=%#llx r10=%#llx r11=%#llx rsi=%#llx rsp=%#llx rbp=%#llx\n",
                        ULL(cpu->gpr[OCERZ_RAX]), ULL(cpu->gpr[OCERZ_RDX]), ULL(cpu->gpr[OCERZ_RBX]), ULL(cpu->gpr[OCERZ_RCX]),
                        ULL(cpu->gpr[OCERZ_R9]), ULL(cpu->gpr[OCERZ_R10]), ULL(cpu->gpr[OCERZ_R11]), ULL(cpu->gpr[OCERZ_RSI]),
                        ULL(cpu->gpr[OCERZ_RSP]), ULL(cpu->gpr[OCERZ_RBP]));
                    uint64_t base = addr & ~15ull;
                    for (int i = 0; i <= 4; i++) { uint64_t a = base + (uint64_t)i * 8;
                        if (ocerz_addr_committed(a) == 1) fprintf(stderr, "  mem[%#llx]=%#llx\n", ULL(a), ULL(ocerz_ld(a, 8)));
                        else fprintf(stderr, "  mem[%#llx]=<unc>\n", ULL(a)); }
                    uint64_t fp = cpu->gpr[OCERZ_RBP];
                    fprintf(stderr, "  bt:");
                    for (int d = 0; d < 16 && fp > 0x300000000ull; d++) {
                        if (ocerz_addr_committed(fp + 8) != 1) break;
                        fprintf(stderr, " %#llx", ULL(ocerz_ld(fp + 8, 8)));
                        if (ocerz_addr_committed(fp) != 1) break;
                        uint64_t nf = ocerz_ld(fp, 8); if (nf <= fp) break; fp = nf; }
                    fprintf(stderr, "\n");
#undef ULL
                }
            }
            __uint128_t expected = ((__uint128_t)cpu->gpr[OCERZ_RDX] << 64) | cpu->gpr[OCERZ_RAX];
            __uint128_t store = ((__uint128_t)cpu->gpr[OCERZ_RCX] << 64) | cpu->gpr[OCERZ_RBX];
            __uint128_t e = expected;
            int ok = __atomic_compare_exchange_n((__uint128_t *)ocerz_g2h(addr), &e, store,
                                                 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            if (ok) {
                if (ocerz_watch_addr && ocerz_watch_addr - addr < 16)
                    ocerz_watch_hit(addr, 16, (uint64_t)store, (uint64_t)(store >> 64));
                ocerz_flag_assign(cpu, OCERZ_ZF, 1);
            } else {
                cpu->gpr[OCERZ_RAX] = (uint64_t)e;
                cpu->gpr[OCERZ_RDX] = (uint64_t)(e >> 64);
                ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            }
        }
        return OCERZ_STEP_OK;
    }
    default:
        return ocerz_unimpl(vm, cpu, insn, "atomic");
    }
}

static int op_flagctl(OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    switch (insn->op) {
    case OCERZ_OP_CLC:
        ocerz_flag_assign(cpu, OCERZ_CF, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_STC:
        ocerz_flag_assign(cpu, OCERZ_CF, 1);
        return OCERZ_STEP_OK;
    case OCERZ_OP_CMC:
        cpu->rflags ^= OCERZ_CF;
        return OCERZ_STEP_OK;
    case OCERZ_OP_CLD:
        ocerz_flag_assign(cpu, OCERZ_DF, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_STD:
        ocerz_flag_assign(cpu, OCERZ_DF, 1);
        return OCERZ_STEP_OK;
    default:
        return ocerz_unimpl(vm, cpu, insn, "flagctl");
    }
}

/* OCERZ_EXCLOG helper: slot holds an NSString*; for __NSCFConstantString the
 * C string pointer is at +0x10 and the length at +0x18.  Returns NULL unless
 * the result looks like printable ASCII of a sane length. */
static const char *ocerz_exc_read_cfstr(uint64_t slot, char *out, size_t cap)
{
    if (!ocerz_addr_readable(slot)) return NULL;
    uint64_t obj = ocerz_ld(slot, 8);
    if (!obj || (obj & 7u) != 0 || !ocerz_addr_readable(obj + 0x18)) return NULL;
    uint64_t cstr = ocerz_ld(obj + 0x10, 8);
    uint64_t len  = ocerz_ld(obj + 0x18, 8);
    if (cstr && len && len < cap &&
        ocerz_addr_readable(cstr) && ocerz_addr_readable(cstr + len)) {
        uint64_t i = 0;
        for (; i < len; i++) {
            uint64_t c = ocerz_ld(cstr + i, 1);
            if (c < 0x20 || c > 0x7e) break;
            out[i] = (char)c;
        }
        if (i == len) { out[len] = 0; return out; }
    }
    /* inline form: a length byte at +0x10 followed by the characters */
    if (ocerz_addr_readable(obj + 0x11)) {
        uint64_t ilen = ocerz_ld(obj + 0x10, 1);
        if (ilen && ilen < cap && ocerz_addr_readable(obj + 0x11 + ilen)) {
            uint64_t i = 0;
            for (; i < ilen; i++) {
                uint64_t c = ocerz_ld(obj + 0x11 + i, 1);
                if (c < 0x20 || c > 0x7e) break;
                out[i] = (char)c;
            }
            if (i == ilen) { out[ilen] = 0; return out; }
        }
    }
    return NULL;
}

int ocerz_interp_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    {   /* OCERZ_EXCLOG=1: trap the guest's _objc_exception_throw and print the
         * NSException's name (and reason when it is a constant string), read
         * straight out of guest memory.  NSException ivars: isa, name, reason.
         * A __NSCFConstantString keeps its C string pointer at +0x10 and its
         * length at +0x18. */
        static int exclog = -1;
        if (exclog < 0) {
            exclog = getenv("OCERZ_EXCLOG") ? 1 : 0;
            if (exclog) {
                ocerz_exc_trap_rip = ocerz_dyld_resolve_guest_sym("_objc_exception_throw");
                fprintf(stderr, "ocerz: EXCLOG _objc_exception_throw=%#llx\n",
                        (unsigned long long)ocerz_exc_trap_rip);
            }
        }
        if (exclog && ocerz_exc_trap_rip && (uint64_t)cpu->rip == ocerz_exc_trap_rip) {
            uint64_t exc = cpu->gpr[7];   /* RDI */
            char nbuf[192], rbuf[384];
            const char *nm = ocerz_exc_read_cfstr(exc + 8, nbuf, sizeof nbuf);
            const char *rs = ocerz_exc_read_cfstr(exc + 16, rbuf, sizeof rbuf);
            fprintf(stderr, "ocerz: EXCTHROW exc=%#llx name=%s reason=%s\n",
                    (unsigned long long)exc, nm ? nm : "<unreadable>",
                    rs ? rs : "<unreadable>");
            if (!nm || !rs) {
                /* fall back to raw words so an unfamiliar string class can
                 * still be decoded by hand instead of losing the datum */
                for (int w = 0; w < 3; w++) {
                    uint64_t slot = exc + 8 + (uint64_t)w * 8;
                    if (!ocerz_addr_readable(slot)) continue;
                    uint64_t o = ocerz_ld(slot, 8);
                    fprintf(stderr, "ocerz: EXCTHROW   ivar+%d=%#llx", 8 + w * 8,
                            (unsigned long long)o);
                    if (o && (o & 7u) == 0 && ocerz_addr_readable(o + 0x20)) {
                        for (int q = 0; q < 4; q++)
                            fprintf(stderr, " [%d]=%#llx", q * 8,
                                    (unsigned long long)ocerz_ld(o + (uint64_t)q * 8, 8));
                    }
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
        }
    }

    {   /* OCERZ_RIPTRAP=addr1[,addr2]: log guest register state whenever
         * execution reaches these addresses (interp only, diagnostics) */
        static uint64_t traps[8];
        static int ntraps = -1;
        if (ntraps < 0) {
            const char *e = getenv("OCERZ_RIPTRAP");
            ntraps = 0;
            while (e && *e && ntraps < 8) {
                traps[ntraps++] = strtoull(e, NULL, 0);
                e = strchr(e, 44);
                if (e) e++;
            }
        }
        int traphit = 0;
        for (int ti = 0; ti < ntraps; ti++)
            if (traps[ti] && cpu->rip == traps[ti]) { traphit = 1; break; }
        if (traphit) {
            fprintf(stderr,
                    "ocerz: RIPTRAP rip=%#llx tid=%#llx acnt=%#llx fwd2=%#llx blk=%#llx c20=%#llx c28=%#llx inv=%#llx dst=%#llx dstv=%#llx rdi=%#llx r13=%#llx byref=%#llx user=%#llx rsi=%#llx rdx=%#llx rax=%#llx rbx=%#llx rsp=%#llx ret0=%#llx ic=%#llx\n",
                    (unsigned long long)cpu->rip,
                    (unsigned long long)(ocerz_addr_readable(cpu->gs_base + 0x18)
                                         ? ocerz_ld(cpu->gs_base + 0x18, 8) : 0),
                    (unsigned long long)({
                        uint64_t _c = 0;
                        const char *_mb = getenv("OCERZ_MACDRVDUMP");
                        if (_mb) {
                            uint64_t _base = strtoull(_mb, NULL, 0);
                            uint64_t _slot = _base + 0x560f0;
                            if (ocerz_addr_readable(_slot)) {
                                uint64_t _ctrl = ocerz_ld(_slot, 8);
                                if (_ctrl && ocerz_addr_readable(_ctrl + 0x10)) {
                                    uint64_t _arr = ocerz_ld(_ctrl + 0x10, 8);
                                    if (_arr && ocerz_addr_readable(_arr + 0x20))
                                        _c = ocerz_ld(_arr + 0x20, 8) >> 32;
                                }
                            }
                        }
                        _c;
                    }),
                    (unsigned long long)({
                        uint64_t _f = ocerz_addr_readable(cpu->gpr[OCERZ_RBP] - 0x58)
                                      ? ocerz_ld(cpu->gpr[OCERZ_RBP] - 0x58, 8) : 0;
                        _f;
                    }),
                    (unsigned long long)({
                        uint64_t _f = ocerz_addr_readable(cpu->gpr[OCERZ_RBP] - 0x58)
                                      ? ocerz_ld(cpu->gpr[OCERZ_RBP] - 0x58, 8) : 0;
                        (_f && ocerz_addr_readable(_f + 0x28)) ? ocerz_ld(_f + 0x28, 8) : 0;
                    }),
                    (unsigned long long)(ocerz_addr_readable(cpu->gpr[OCERZ_RDI] + 0x20)
                                         ? ocerz_ld(cpu->gpr[OCERZ_RDI] + 0x20, 8) : 0),
                    (unsigned long long)(ocerz_addr_readable(cpu->gpr[OCERZ_RDI] + 0x28)
                                         ? ocerz_ld(cpu->gpr[OCERZ_RDI] + 0x28, 8) : 0),
                    (unsigned long long)({
                        uint64_t _b = ocerz_addr_readable(cpu->gpr[OCERZ_RDI] + 0x28)
                                      ? ocerz_ld(cpu->gpr[OCERZ_RDI] + 0x28, 8) : 0;
                        (_b && ocerz_addr_readable(_b + 0x10)) ? ocerz_ld(_b + 0x10, 8) : 0;
                    }),
                    (unsigned long long)cpu->gpr[OCERZ_RAX],
                    (unsigned long long)(ocerz_addr_readable(cpu->gpr[OCERZ_RAX] + 0x18)
                                         ? ocerz_ld(cpu->gpr[OCERZ_RAX] + 0x18, 1) : 0xff),
                    (unsigned long long)cpu->gpr[OCERZ_RDI],
                    (unsigned long long)cpu->gpr[OCERZ_R13],
                    (unsigned long long)(ocerz_addr_readable(cpu->gpr[OCERZ_RDI] + 0x38)
                                         ? ocerz_ld(cpu->gpr[OCERZ_RDI] + 0x38, 8) : 0),
                    (unsigned long long)(ocerz_addr_readable(cpu->gpr[OCERZ_RDI] + 0x30)
                                         ? ocerz_ld(cpu->gpr[OCERZ_RDI] + 0x30, 8) : 0),
                    (unsigned long long)cpu->gpr[OCERZ_RSI],
                    (unsigned long long)cpu->gpr[OCERZ_RDX],
                    (unsigned long long)cpu->gpr[OCERZ_RAX],
                    (unsigned long long)cpu->gpr[OCERZ_RBX],
                    (unsigned long long)cpu->gpr[OCERZ_RSP],
                    (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8),
                    (unsigned long long)vm->insn_count);
        }
    }

    ocerz_flags_materialize(cpu);
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return ocerz_dyldapi_dispatch(vm, cpu);

    X86Insn insn;
    const uint8_t *code = (const uint8_t *)ocerz_g2h(cpu->rip);
    /* The one place the interpreter chooses a decode mode.  Everything
     * downstream reads insn.mode32 rather than cpu->mode32, so a decoded
     * instruction always executes under the mode it was decoded in even if a
     * far transfer inside it changes the CPU's mode. */
    int rc = ocerz_decode_mode(code, 15, cpu->rip, &insn, cpu->mode32);
    if (rc != OCERZ_OK) {
        fprintf(stderr, "ocerz: fatal: decode failed (%d, %s mode) at rip=%#llx\n  bytes: ",
                rc, cpu->mode32 ? "i386" : "long", (unsigned long long)cpu->rip);
        dump_raw_bytes(stderr, cpu->rip, 15);
        fprintf(stderr, "\n  [rsp]=%#llx [rsp+8]=%#llx rbp-ret=%#llx\n",
                (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8),
                (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8),
                (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RBP] + 8, 8));
        return OCERZ_STEP_FATAL;
    }

    vm->insn_count++;

    if (vm->trace) {
        char buf[128];
        ocerz_format_insn(&insn, buf, sizeof buf);
        fprintf(stderr, "ocerz: %#llx: %s\n", (unsigned long long)cpu->rip, buf);
    }
    { static int sl = -1; if (sl < 0) sl = getenv("OCERZ_STEPLOG") ? 1 : 0;
      if (sl) { fprintf(stderr, "STEP %#llx", (unsigned long long)cpu->rip);
                for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)cpu->gpr[i]);
                fprintf(stderr, "\n"); } }

    cpu->cur_rip = cpu->rip;
    cpu->rip += insn.len;
    if (insn.mode32)
        cpu->rip = (uint32_t)cpu->rip;   /* EIP wraps at 32 bits */
    rc = ocerz_interp_exec(vm, cpu, &insn);
    /* EIP wrap again after execution, but keyed on the mode the CPU is in NOW:
     * a far transfer that just left 32-bit mode has already produced a full
     * 64-bit rip and must not be truncated. */
    if (cpu->mode32)
        cpu->rip = (uint32_t)cpu->rip;
    return rc;
}

int ocerz_interp_exec(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn * restrict insnp)
{
    int rc;

    switch (insnp->op) {
    case OCERZ_OP_MOV:
    case OCERZ_OP_MOVZX:
    case OCERZ_OP_MOVSX:
    case OCERZ_OP_MOVSXD:
    case OCERZ_OP_LEA:
    case OCERZ_OP_XCHG:
    case OCERZ_OP_BSWAP:
    case OCERZ_OP_CMOVCC:
    case OCERZ_OP_SETCC:
        return op_mov_family(vm, cpu, insnp);

    case OCERZ_OP_PUSH:
    case OCERZ_OP_POP:
    case OCERZ_OP_PUSHF:
    case OCERZ_OP_POPF:
    case OCERZ_OP_LAHF:
    case OCERZ_OP_SAHF:
    case OCERZ_OP_LEAVE:
        return op_stack(vm, cpu, insnp);

    case OCERZ_OP_CBW:
    case OCERZ_OP_CWD:
        return op_cbw_cwd(vm, cpu, insnp);

    case OCERZ_OP_ADD:
    case OCERZ_OP_ADC:
    case OCERZ_OP_SUB:
    case OCERZ_OP_SBB:
    case OCERZ_OP_AND:
    case OCERZ_OP_OR:
    case OCERZ_OP_XOR:
    case OCERZ_OP_CMP:
    case OCERZ_OP_TEST:
        return op_arith(vm, cpu, insnp);

    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
    case OCERZ_OP_NEG:
    case OCERZ_OP_NOT:
        return op_incdecnegnot(vm, cpu, insnp);

    case OCERZ_OP_MUL:
    case OCERZ_OP_IMUL:
        return op_mul(vm, cpu, insnp);

    case OCERZ_OP_DIV:
    case OCERZ_OP_IDIV:
        return op_div(vm, cpu, insnp);

    case OCERZ_OP_SHL:
    case OCERZ_OP_SHR:
    case OCERZ_OP_SAR:
        return op_shift(vm, cpu, insnp);

    case OCERZ_OP_ROL:
    case OCERZ_OP_ROR:
    case OCERZ_OP_RCL:
    case OCERZ_OP_RCR:
        return op_rotate(vm, cpu, insnp);

    case OCERZ_OP_SHLD:
    case OCERZ_OP_SHRD:
        return op_shiftd(vm, cpu, insnp);

    case OCERZ_OP_JMP:
    case OCERZ_OP_JCC:
    case OCERZ_OP_JRCXZ:
    case OCERZ_OP_LOOP:
    case OCERZ_OP_LOOPE:
    case OCERZ_OP_LOOPNE:
    case OCERZ_OP_CALL:
    case OCERZ_OP_RET:
    case OCERZ_OP_IRET:
    case OCERZ_OP_JMPF:
    case OCERZ_OP_CALLF:
    case OCERZ_OP_RETF:
    case OCERZ_OP_MOVSEG:
        return op_branch(vm, cpu, insnp);

    case OCERZ_OP_XADD:
    case OCERZ_OP_CMPXCHG:
    case OCERZ_OP_CMPXCHGXB:
        return op_atomic(vm, cpu, insnp);

    /* i386-only.  Unreachable from 64-bit execution: the decoder leaves every
     * one of these opcode bytes undefined in long mode. */
    case OCERZ_OP_PUSHA:
    case OCERZ_OP_POPA:
    case OCERZ_OP_PUSHSEG:
    case OCERZ_OP_POPSEG:
    case OCERZ_OP_DAA:
    case OCERZ_OP_DAS:
    case OCERZ_OP_AAA:
    case OCERZ_OP_AAS:
    case OCERZ_OP_AAM:
    case OCERZ_OP_AAD:
    case OCERZ_OP_SALC:
    case OCERZ_OP_BOUND:
    case OCERZ_OP_INTO:
    case OCERZ_OP_LES:
    case OCERZ_OP_LDS:
        return op_i386(vm, cpu, insnp);

    case OCERZ_OP_CLC:
    case OCERZ_OP_STC:
    case OCERZ_OP_CMC:
    case OCERZ_OP_CLD:
    case OCERZ_OP_STD:
        return op_flagctl(vm, cpu, insnp);

    case OCERZ_OP_NOP:
    case OCERZ_OP_PAUSE:
    case OCERZ_OP_PREFETCH:
    case OCERZ_OP_CLFLUSH:
        return OCERZ_STEP_OK;

    case OCERZ_OP_MFENCE:
    case OCERZ_OP_LFENCE:
    case OCERZ_OP_SFENCE:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return OCERZ_STEP_OK;

    case OCERZ_OP_INT3:

        if (ocerz_signal_deliver(cpu, OCERZ_SIGTRAP, cpu->rip, 0, 0))
            return OCERZ_STEP_OK;
        return trap_fatal(insnp, "guest breakpoint/interrupt");
    case OCERZ_OP_INT:
        return trap_fatal(insnp, "guest breakpoint/interrupt");

    case OCERZ_OP_UD2:

        if (ocerz_is_wqthread_exit(insnp->rip)) {
            cpu->terminated = 1;
            return OCERZ_STEP_OK;
        }
        if (getenv("OCERZ_UD2DUMP")) {
            extern void ocerz_recov_dump(FILE *);
            ocerz_recov_dump(stderr);
            uint64_t gs = cpu->gs_base;
            uint64_t gate = cpu->gpr[OCERZ_RDX];
            fprintf(stderr, "ocerz: UD2DUMP rip=%#llx gs_base=%#llx gs+0x18=%#llx "
                    "gate=%#llx gate[0]=%#llx rdi=%#llx rsi=%#llx r14=%#llx\n",
                    (unsigned long long)insnp->rip, (unsigned long long)gs,
                    (unsigned long long)ocerz_ld(gs + 0x18, 8),
                    (unsigned long long)gate,
                    (unsigned long long)(gate ? ocerz_ld(gate, 8) : 0),
                    (unsigned long long)cpu->gpr[OCERZ_RDI],
                    (unsigned long long)cpu->gpr[OCERZ_RSI],
                    (unsigned long long)cpu->gpr[OCERZ_R14]);
            uint64_t fp = cpu->gpr[OCERZ_RBP];
            fprintf(stderr, "ocerz: UD2DUMP bt:");
            for (int d = 0; d < 16 && fp >= 0x300000000ull; d++) {
                fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
                uint64_t nf = ocerz_ld(fp, 8);
                if (nf <= fp) break;
                fp = nf;
            }
            fprintf(stderr, "\n");
        }
        return trap_fatal(insnp, "guest UD2 (undefined instruction)");

    case OCERZ_OP_HLT:
        return trap_fatal(insnp, "guest HLT");

    case OCERZ_OP_SYSCALL:
        cpu->gpr[OCERZ_RCX] = cpu->rip;
        cpu->gpr[OCERZ_R11] = cpu->rflags & 0x3c7fd7ull;
        return ocerz_handle_syscall(vm, cpu);

    default:
        break;
    }

    rc = ocerz_interp_ext(vm, cpu, insnp);
    if (rc == OCERZ_EUNSUP && insnp->op >= OCERZ_OP_SSE_FIRST)
        rc = ocerz_interp_sse(vm, cpu, insnp);
    if (rc == OCERZ_EUNSUP)
        return ocerz_unimpl(vm, cpu, insnp, "no handler");
    return rc;
}
