/* The core single-step interpreter: reference semantics for x86_64. */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/vm.h"
#include "ocerz/syscall.h"
#include "ocerz/dyldapi.h"
#include <stdlib.h>

static int ocerz_mode32_unsupported(OcerzVM *vm, OcerzCPU *cpu, uint32_t sel,
                                    uint64_t target, const char *how);

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
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < (uint64_t)size)
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
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < (uint64_t)size)
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
    if (ok && ocerz_watch_addr && ocerz_watch_addr - gaddr < (uint64_t)size)
        ocerz_watch_hit(gaddr, size, desired, 0);
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
        return trap_fatal(insn, "integer divide by zero");

    if (insn->op == OCERZ_OP_DIV) {
        if (size == 1) {
            uint64_t num = ocerz_read_gpr(cpu, OCERZ_RAX, 2, 0);
            uint64_t d = divisor & 0xff;
            uint64_t q = num / d;
            uint64_t r = num % d;
            if (q > 0xff)
                return trap_fatal(insn, "divide quotient overflow");
            ocerz_write_gpr(cpu, OCERZ_RAX, 1, 0, q);
            ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, r);
        } else if (size == 8) {
            __uint128_t num = ((__uint128_t)cpu->gpr[OCERZ_RDX] << 64) | cpu->gpr[OCERZ_RAX];
            __uint128_t d = divisor;
            __uint128_t q = num / d;
            __uint128_t r = num % d;
            if (q > (__uint128_t)~(uint64_t)0)
                return trap_fatal(insn, "divide quotient overflow");
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
                return trap_fatal(insn, "divide quotient overflow");
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
            return trap_fatal(insn, "divide quotient overflow");
        ocerz_write_gpr(cpu, OCERZ_RAX, 1, 0, (uint64_t)q);
        ocerz_write_gpr(cpu, OCERZ_RAX, 1, 1, (uint64_t)r);
    } else if (size == 8) {
        __int128_t num = ((__int128_t)(int64_t)cpu->gpr[OCERZ_RDX] << 64) | cpu->gpr[OCERZ_RAX];
        __int128_t d = (int64_t)divisor;
        __int128_t q = num / d;
        __int128_t r = num % d;
        if (q < -(__int128_t)1 - (__int128_t)((__uint128_t)~(uint64_t)0 >> 1) || q > (__int128_t)((__uint128_t)~(uint64_t)0 >> 1))
            return trap_fatal(insn, "divide quotient overflow");
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
            return trap_fatal(insn, "divide quotient overflow");
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
        ocerz_push(cpu, size, v);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_POP: {

        int size = insn->opsize;
        uint64_t v = ocerz_ld(cpu->gpr[OCERZ_RSP], size);
        ocerz_write_op(cpu, insn, &insn->ops[0], v);
        if (!(insn->ops[0].kind == OCERZ_OPK_REG && insn->ops[0].reg == OCERZ_RSP))
            cpu->gpr[OCERZ_RSP] += (uint64_t)size;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_PUSHF:
        ocerz_push(cpu, 8, cpu->rflags);
        return OCERZ_STEP_OK;
    case OCERZ_OP_POPF: {
        uint64_t v = ocerz_pop(cpu, 8);
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

        uint64_t v = ocerz_ld(cpu->gpr[OCERZ_RBP], 8);
        cpu->gpr[OCERZ_RSP] = cpu->gpr[OCERZ_RBP] + 8;
        cpu->gpr[OCERZ_RBP] = v;
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
        uint64_t c = (insn->addrsize == 4) ? (uint32_t)cpu->gpr[OCERZ_RCX] : cpu->gpr[OCERZ_RCX];
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
        ocerz_push(cpu, 8, cpu->rip);
        cpu->rip = target;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_RET: {
        uint64_t ret = ocerz_pop(cpu, 8);
        if (insn->nops == 1)
            cpu->gpr[OCERZ_RSP] += ocerz_trunc(insn->ops[0].imm, 2);
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
        if (seg == 1)
            cpu->cs_sel = (uint16_t)sel;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_JMPF:
    case OCERZ_OP_CALLF: {

        int sz = insn->opsize ? insn->opsize : 4;
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
        uint64_t off = ocerz_ld(ea, sz);
        uint32_t sel = (uint32_t)ocerz_ld(ea + (uint64_t)sz, 2);
        if (ocerz_ldt_is_big(sel))
            return ocerz_mode32_unsupported(vm, cpu, sel, off, "far jump/call");
        if (insn->op == OCERZ_OP_CALLF) {
            ocerz_push(cpu, 8, cpu->cs_sel);
            ocerz_push(cpu, 8, insn->rip + insn->len);
        }
        cpu->cs_sel = (uint16_t)sel;
        cpu->rip = off;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_RETF: {

        int sz = insn->opsize ? insn->opsize : 8;
        uint64_t off = ocerz_pop(cpu, 8);
        uint32_t sel = (uint32_t)ocerz_pop(cpu, 8);
        (void)sz;
        if (insn->nops == 1)
            cpu->gpr[OCERZ_RSP] += ocerz_trunc(insn->ops[0].imm, 2);
        if (ocerz_ldt_is_big(sel))
            return ocerz_mode32_unsupported(vm, cpu, sel, off, "far return");
        cpu->cs_sel = (uint16_t)sel;
        cpu->rip = off;
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_IRET: {
        int sz = insn->opsize ? insn->opsize : 8;
        uint64_t sp = cpu->gpr[OCERZ_RSP];
        uint64_t rip = ocerz_ld(sp, sz);

        uint32_t cs = (uint32_t)ocerz_ld(sp + (uint64_t)sz, sz);
        uint64_t flags = ocerz_ld(sp + (uint64_t)sz * 2, sz);
        uint64_t newsp = ocerz_ld(sp + (uint64_t)sz * 3, sz);
        if (ocerz_ldt_is_big(cs))
            return ocerz_mode32_unsupported(vm, cpu, cs, rip, "iret");
        if (cs)
            cpu->cs_sel = (uint16_t)cs;
        cpu->rip = rip;
        cpu->rflags = flags | 0x2;
        cpu->gpr[OCERZ_RSP] = newsp;
        return OCERZ_STEP_OK;
    }
    default:
        return ocerz_unimpl(vm, cpu, insn, "branch");
    }
}

static int ocerz_mode32_unsupported(OcerzVM *vm, OcerzCPU *cpu, uint32_t sel,
                                    uint64_t target, const char *how)
{
    (void)vm;
    cpu->mode32 = 1;
    cpu->cs_sel = (uint16_t)sel;
    fprintf(stderr,
            "ocerz: fatal: 32-bit mode entry via %s -- cs=%#x (LDT index %u, base=%#llx) "
            "target=%#llx rip=%#llx\n"
            "ocerz: the WoW64 mode switch is recognised but 32-bit instruction decoding is "
            "not implemented yet\n",
            how, sel, sel >> 3, (unsigned long long)ocerz_ldt_base(sel),
            (unsigned long long)target, (unsigned long long)cpu->rip);
    return OCERZ_STEP_FATAL;
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
        if (mem) {
            uint64_t addr = ocerz_ea(cpu, insn, &insn->ops[0]);
            uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
            uint64_t seen = acc;
            int ok = ocerz_atomic_cmpxchg(addr, size, &seen, src);
            ocerz_flags_sub(cpu, size, acc, seen, 0, acc - seen);
            if (!ok)
                write_acc(cpu, size, seen);
            return OCERZ_STEP_OK;
        }
        uint64_t d = ocerz_read_op(cpu, insn, &insn->ops[0]);
        ocerz_flags_sub(cpu, size, acc, d, 0, acc - d);
        if (ocerz_trunc(acc, size) == ocerz_trunc(d, size)) {
            uint64_t src = ocerz_read_op(cpu, insn, &insn->ops[1]);
            ocerz_write_op(cpu, insn, &insn->ops[0], src);
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

int ocerz_interp_step(struct OcerzVM *vm, OcerzCPU *cpu)
{

    ocerz_flags_materialize(cpu);
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return ocerz_dyldapi_dispatch(vm, cpu);

    X86Insn insn;
    const uint8_t *code = (const uint8_t *)ocerz_g2h(cpu->rip);
    int rc = ocerz_decode(code, 15, cpu->rip, &insn);
    if (rc != OCERZ_OK) {
        fprintf(stderr, "ocerz: fatal: decode failed (%d) at rip=%#llx\n  bytes: ",
                rc, (unsigned long long)cpu->rip);
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
    return ocerz_interp_exec(vm, cpu, &insn);
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
