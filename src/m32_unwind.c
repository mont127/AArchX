/*
 * C++ exception unwinding for i386 guests (include/ocerz/m32.h).
 *
 * The guest's libstdc++ (runtime/guest32) imports the _Unwind_* calls from libSystem, so they land here: the Itanium
 * ABI's two-phase unwinder (search, then cleanup, as LLVM libunwind does it) over the guest images' __TEXT,__eh_frame,
 * calling each frame's own personality routine (the guest's __gxx_personality_v0) in the guest.  A frame is the
 * registers at its call site; Darwin's i386 eh_frame numbers them eax 0, ecx 1, edx 2, ebx 3, ebp 4, esp 5, esi 6,
 * edi 7, eip 8 (ebp and esp swapped from the SysV numbering).  The context the personality is handed is a Frame in
 * the guest heap that it only passes back to the calls below; a landing pad is entered by loading the guest CPU.
 *
 * ponytail: __eh_frame only (a function with nothing but a compact __unwind_info entry ends the stack, logged); no
 * forced unwinding (_Unwind_ForcedUnwind); DWARF expressions in CFI end the stack too.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_EBP, R_ESP, R_ESI, R_EDI, R_EIP, NREG };
enum { URC_FATAL_PHASE2 = 2, URC_FATAL_PHASE1 = 3, URC_END_OF_STACK = 5, URC_HANDLER_FOUND = 6,
       URC_INSTALL_CONTEXT = 7, URC_CONTINUE_UNWIND = 8, INSTALLED = -1 };
enum { UA_SEARCH = 1, UA_CLEANUP = 2, UA_HANDLER = 4 };

typedef struct Frame {
    uint32_t reg[NREG];                            /* at the frame's call site: eip is the return address */
    uint32_t func, lsda, personality, args_size;   /* from the FDE covering eip - 1 */
} Frame;

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

/* ---- reading eh_frame in guest memory ---- */

static uint32_t rd8(uint32_t g) { return *(const uint8_t *)m32_h(g); }
static uint32_t rd16(uint32_t g) { return rd8(g) | rd8(g + 1) << 8; }
static uint32_t uleb(uint32_t *p)
{
    uint32_t r = 0, b;
    int s = 0;
    do {
        b = rd8((*p)++);
        if (s < 32)
            r |= (b & 0x7f) << s;
        s += 7;
    } while (b & 0x80);
    return r;
}
static int32_t sleb(uint32_t *p)
{
    uint32_t r = 0, b;
    int s = 0;
    do {
        b = rd8((*p)++);
        if (s < 32)
            r |= (b & 0x7f) << s;
        s += 7;
    } while (b & 0x80);
    if (s < 32 && (b & 0x40))
        r |= ~0u << s;
    return (int32_t)r;
}
/* a DW_EH_PE-encoded pointer at *p */
static uint32_t enc_ptr(uint32_t *p, uint32_t enc)
{
    uint32_t at = *p, v;
    switch (enc & 0x0f) {
    case 0x00: case 0x03: case 0x0b: v = m32_rd(at); *p += 4; break;   /* absptr, udata4, sdata4 */
    case 0x04: case 0x0c: v = m32_rd(at); *p += 8; break;
    case 0x02: v = rd16(at); *p += 2; break;
    case 0x0a: v = (uint32_t)(int16_t)rd16(at); *p += 2; break;
    case 0x01: v = uleb(p); break;
    case 0x09: v = (uint32_t)sleb(p); break;
    default: return 0;
    }
    if ((enc & 0x70) == 0x10)   /* pcrel */
        v += at;
    if (enc & 0x80)             /* indirect */
        v = m32_rd(v);
    return v;
}

typedef struct Cie {
    uint32_t code_align, ra, renc, lenc, personality, insns, end;
    int32_t data_align;
    int z;
} Cie;

static int parse_cie(uint32_t c, Cie *cie)
{
    uint32_t len = m32_rd(c), p = c + 8;   /* length, CIE id 0, then the version */
    if (!len || len == 0xffffffffu)
        return -1;
    memset(cie, 0, sizeof *cie);
    cie->lenc = 0xff;
    cie->end = c + 4 + len;
    uint32_t version = rd8(p++);
    const char *aug = (const char *)m32_h(p);
    p += (uint32_t)strlen(aug) + 1;
    cie->code_align = uleb(&p);
    cie->data_align = sleb(&p);
    cie->ra = version == 1 ? rd8(p++) : uleb(&p);
    if (aug[0] == 'z') {
        uint32_t alen = uleb(&p), aend = p + alen;
        for (const char *a = aug + 1; *a; a++) {
            if (*a == 'P') {
                uint32_t e = rd8(p++);
                cie->personality = enc_ptr(&p, e);
            } else if (*a == 'L') {
                cie->lenc = rd8(p++);
            } else if (*a == 'R') {
                cie->renc = rd8(p++);
            }
        }
        p = aend;
        cie->z = 1;
    }
    cie->insns = p;
    return cie->ra == R_EIP ? 0 : -1;
}

/* ---- finding a pc's FDE: each image's FDEs sorted by start, indexed at its first throw ---- */

typedef struct Fde { uint32_t start, end, at; } Fde;
typedef struct EhIndex { int built; Fde *f; int n; } EhIndex;
static EhIndex g_index[M32_MAX_IMAGES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int fde_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Fde *)a)->start, y = ((const Fde *)b)->start;
    return x < y ? -1 : x > y;
}

static void build_index(EhIndex *x, const M32Image *img)
{
    x->built = 1;
    uint32_t eh = 0, size = 0;
    for (int s = 0; s < img->nsect; s++)
        if (!strcmp(img->sect[s].seg, "__TEXT") && !strcmp(img->sect[s].name, "__eh_frame")) {
            eh = img->sect[s].addr + (uint32_t)img->slide;
            size = img->sect[s].size;
        }
    int cap = 0;
    uint32_t cie_at = 0;
    Cie cie;
    for (uint32_t p = eh; eh && p + 8 <= eh + size;) {
        uint32_t len = m32_rd(p), id = m32_rd(p + 4);
        if (!len || len == 0xffffffffu)
            break;
        if (id) {   /* an FDE: its CIE is id bytes back from this field */
            uint32_t c = p + 4 - id, q = p + 8;
            if (c != cie_at && !parse_cie(c, &cie))
                cie_at = c;
            if (c == cie_at) {
                uint32_t start = enc_ptr(&q, cie.renc), range = enc_ptr(&q, cie.renc & 0x0f);
                if (x->n == cap)
                    x->f = realloc(x->f, (size_t)(cap = cap ? 2 * cap : 1024) * sizeof *x->f);
                x->f[x->n++] = (Fde){ start, start + range, p };
            }
        }
        p += 4 + len;
    }
    qsort(x->f, (size_t)x->n, sizeof *x->f, fde_cmp);
}

static uint32_t find_fde(uint32_t pc)
{
    for (int i = 0; i < m32_nimages; i++) {
        const M32Image *img = m32_images[i];
        if (!img || pc < img->lo || pc >= img->hi)
            continue;
        EhIndex *x = &g_index[i];
        pthread_mutex_lock(&g_lock);
        if (!x->built)
            build_index(x, img);
        pthread_mutex_unlock(&g_lock);
        int lo = 0, hi = x->n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (x->f[mid].start > pc)
                hi = mid - 1;
            else if (pc >= x->f[mid].end)
                lo = mid + 1;
            else
                return x->f[mid].at;
        }
        fprintf(stderr, "ocerz: m32: no eh_frame unwind information for %#x (%s)\n", pc, img->path);
        return 0;
    }
    return 0;   /* outside the guest images: a host crossing or the thread's start, the end of the stack */
}

/* ---- running a frame's CFI up to its call site ---- */

enum { RULE_SAME, RULE_OFFSET, RULE_VAL, RULE_REG };
typedef struct Row {
    uint32_t cfa_reg, args_size;
    int32_t cfa_off;
    uint8_t kind[NREG];
    int32_t off[NREG];
} Row;

static void set_rule(Row *row, uint32_t reg, int kind, int32_t off)
{
    if (reg < NREG) {
        row->kind[reg] = (uint8_t)kind;
        row->off[reg] = off;
    }
}

/* the rows of [p, end) whose location is at most target; init is the CIE's row (for DW_CFA_restore) */
static int run_cfi(uint32_t p, uint32_t end, const Cie *cie, uint32_t loc, uint32_t target, Row *row, const Row *init)
{
    Row saved[8];
    int nsaved = 0;
    while (p < end) {
        uint32_t op = rd8(p++), reg;
        int32_t da = cie->data_align;
        switch (op >> 6) {
        case 1: loc += (op & 0x3f) * cie->code_align; if (loc > target) return 0; continue;
        case 2: set_rule(row, op & 0x3f, RULE_OFFSET, (int32_t)uleb(&p) * da); continue;
        case 3:
            if (init && (op & 0x3f) < NREG)
                set_rule(row, op & 0x3f, init->kind[op & 0x3f], init->off[op & 0x3f]);
            continue;
        }
        switch (op) {
        case 0x00: break;
        case 0x01: loc = enc_ptr(&p, cie->renc); if (loc > target) return 0; break;
        case 0x02: loc += rd8(p) * cie->code_align; p += 1; if (loc > target) return 0; break;
        case 0x03: loc += rd16(p) * cie->code_align; p += 2; if (loc > target) return 0; break;
        case 0x04: loc += m32_rd(p) * cie->code_align; p += 4; if (loc > target) return 0; break;
        case 0x05: reg = uleb(&p); set_rule(row, reg, RULE_OFFSET, (int32_t)uleb(&p) * da); break;
        case 0x06: reg = uleb(&p); if (init && reg < NREG) set_rule(row, reg, init->kind[reg], init->off[reg]); break;
        case 0x07: case 0x08: set_rule(row, uleb(&p), RULE_SAME, 0); break;   /* undefined, same value */
        case 0x09: reg = uleb(&p); set_rule(row, reg, RULE_REG, (int32_t)uleb(&p)); break;
        case 0x0a: if (nsaved == 8) return -1; saved[nsaved++] = *row; break;
        case 0x0b: if (!nsaved) return -1; *row = saved[--nsaved]; break;
        case 0x0c: row->cfa_reg = uleb(&p); row->cfa_off = (int32_t)uleb(&p); break;
        case 0x0d: row->cfa_reg = uleb(&p); break;
        case 0x0e: row->cfa_off = (int32_t)uleb(&p); break;
        case 0x11: reg = uleb(&p); set_rule(row, reg, RULE_OFFSET, sleb(&p) * da); break;
        case 0x12: row->cfa_reg = uleb(&p); row->cfa_off = sleb(&p) * da; break;
        case 0x13: row->cfa_off = sleb(&p) * da; break;
        case 0x14: reg = uleb(&p); set_rule(row, reg, RULE_VAL, (int32_t)uleb(&p) * da); break;
        case 0x15: reg = uleb(&p); set_rule(row, reg, RULE_VAL, sleb(&p) * da); break;
        case 0x2e: row->args_size = uleb(&p); break;   /* DW_CFA_GNU_args_size */
        case 0x2f: reg = uleb(&p); set_rule(row, reg, RULE_OFFSET, -(int32_t)uleb(&p) * da); break;
        default:
            fprintf(stderr, "ocerz: m32: unwinding: DW_CFA %#x not handled\n", op);
            return -1;
        }
    }
    return 0;
}

/* f's FDE fields and the CFI row at its call site; 0 when it cannot be unwound */
static int frame_info(Frame *f, Row *row)
{
    uint32_t pc = f->reg[R_EIP] - 1, fde = find_fde(pc);
    Cie cie;
    if (!fde || parse_cie(fde + 4 - m32_rd(fde + 4), &cie))
        return 0;
    uint32_t p = fde + 8;
    f->func = enc_ptr(&p, cie.renc);
    enc_ptr(&p, cie.renc & 0x0f);
    f->lsda = 0;
    if (cie.z) {
        uint32_t alen = uleb(&p), aend = p + alen, q = p;
        if (cie.lenc != 0xff && enc_ptr(&q, cie.lenc & 0x0f)) {   /* a zero field is no LSDA, whatever its encoding */
            q = p;
            f->lsda = enc_ptr(&q, cie.lenc);
        }
        p = aend;
    }
    f->personality = cie.personality;
    memset(row, 0, sizeof *row);
    if (run_cfi(cie.insns, cie.end, &cie, f->func, ~0u, row, NULL))
        return 0;
    Row init = *row;
    if (run_cfi(p, fde + 4 + m32_rd(fde), &cie, f->func, pc, row, &init))
        return 0;
    f->args_size = row->args_size;
    return 1;
}

/* f becomes its caller's frame */
static int step(Frame *f, const Row *row)
{
    if (row->cfa_reg >= NREG)
        return 0;
    uint32_t cfa = f->reg[row->cfa_reg] + (uint32_t)row->cfa_off, old[NREG];
    memcpy(old, f->reg, sizeof old);
    for (int r = 0; r < NREG; r++) {
        if (row->kind[r] == RULE_OFFSET)
            f->reg[r] = m32_rd(cfa + (uint32_t)row->off[r]);
        else if (row->kind[r] == RULE_VAL)
            f->reg[r] = cfa + (uint32_t)row->off[r];
        else if (row->kind[r] == RULE_REG && (uint32_t)row->off[r] < NREG)
            f->reg[r] = old[row->off[r]];
    }
    f->reg[R_ESP] = cfa;
    return f->reg[R_EIP] != 0;
}

/* ---- the two phases ---- */

static uint32_t personality(struct OcerzVM *vm, const Frame *f, uint32_t actions, uint32_t exc, uint32_t ctx)
{
    memcpy(m32_h(ctx), f, sizeof *f);
    uint32_t args[6] = { 1, actions, m32_rd(exc), m32_rd(exc + 4), exc, ctx };
    return m32_call(vm, f->personality, args, 6, NULL, NULL);
}

/* the frame (its esp) whose handler will catch exc */
static int search(struct OcerzVM *vm, Frame f, uint32_t exc, uint32_t ctx, uint32_t *catcher)
{
    for (;;) {
        Row row;
        if (!frame_info(&f, &row))
            return URC_END_OF_STACK;
        if (f.personality) {
            uint32_t r = personality(vm, &f, UA_SEARCH, exc, ctx);
            if (r == URC_HANDLER_FOUND) {
                *catcher = f.reg[R_ESP];
                return URC_HANDLER_FOUND;
            }
            if (r != URC_CONTINUE_UNWIND)
                return URC_FATAL_PHASE1;
        }
        if (!step(&f, &row))
            return URC_END_OF_STACK;
    }
}

/* runs the cleanups up to the catching frame (exc's private_2), entering the first landing pad there is */
static int cleanup(struct OcerzVM *vm, OcerzCPU *cpu, Frame f, uint32_t exc, uint32_t ctx)
{
    uint32_t catcher = m32_rd(exc + 16);
    for (;;) {
        Row row;
        if (!frame_info(&f, &row))
            return URC_FATAL_PHASE2;
        if (f.personality) {
            int handler = f.reg[R_ESP] == catcher;
            uint32_t r = personality(vm, &f, UA_CLEANUP | (handler ? UA_HANDLER : 0), exc, ctx);
            if (r == URC_INSTALL_CONTEXT) {
                memcpy(&f, m32_h(ctx), sizeof f);
                static const int gpr[8] = { OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX, OCERZ_RBP, OCERZ_RSP,
                                            OCERZ_RSI, OCERZ_RDI };
                for (int i = 0; i < 8; i++)
                    cpu->gpr[gpr[i]] = f.reg[i];
                cpu->rip = f.reg[R_EIP];
                return INSTALLED;
            }
            if (r != URC_CONTINUE_UNWIND || handler)
                return URC_FATAL_PHASE2;
        }
        if (!step(&f, &row))
            return URC_FATAL_PHASE2;
    }
}

/* the frame that called the _Unwind function now running */
static Frame caller(const OcerzCPU *cpu)
{
    Frame f;
    memset(&f, 0, sizeof f);
    static const int gpr[8] = { OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX, OCERZ_RBP, OCERZ_RSP, OCERZ_RSI, OCERZ_RDI };
    for (int i = 0; i < 8; i++)
        f.reg[i] = (uint32_t)cpu->gpr[gpr[i]];
    f.reg[R_EIP] = m32_rd(f.reg[R_ESP]);
    f.reg[R_ESP] += 4;
    return f;
}

/* what was thrown, when it is a string (Unreal throws its error message as a UTF-16 TCHAR *) */
static void print_thrown(uint32_t ue)
{
    for (uint32_t off = 16; off <= 64; off += 4) {   /* the thrown object follows the header */
        uint32_t p = m32_rd(ue + off), n = 0;
        if (p < 0x1000 || p >= M32_HANDLE_LO)
            continue;
        char out[256];
        for (; n < sizeof out - 1; n++) {
            uint16_t c;
            memcpy(&c, m32_h(p + 2 * n), 2);
            if (!c || c > 0x7e || (c < 0x20 && c != '\n'))
                break;
            out[n] = (char)c;
        }
        out[n] = 0;
        if (n >= 4) {
            fprintf(stderr, "ocerz: m32: guest threw: \"%s\"\n", out);
            return;
        }
    }
}

/* _Unwind_RaiseException(exc), and _Unwind_Resume_or_Rethrow(exc) (never a forced unwind here) */
static int sp_raise(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int log = -1;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "throws");
    uint32_t exc = m32_arg(cpu, 0), ctx = m32_malloc(sizeof(Frame)), catcher = 0;
    Frame f = caller(cpu);
    if (log)
        print_thrown(exc);
    int r = search(vm, f, exc, ctx, &catcher);
    if (r == URC_HANDLER_FOUND) {
        m32_wr(exc + 12, 0);   /* private_1: no forced-unwind stop function */
        m32_wr(exc + 16, catcher);
        r = cleanup(vm, cpu, f, exc, ctx);
    } else {
        if (!log)
            print_thrown(exc);
        fprintf(stderr, "ocerz: m32: C++ exception not caught (%d), thrown from %#x\n", r, f.reg[R_EIP]);
    }
    m32_free(ctx);
    if (r == INSTALLED)
        return OCERZ_STEP_OK;
    RET(r);
}

/* _Unwind_Resume(exc): a cleanup landing pad is done, go on to the catching frame */
static int sp_resume(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t exc = m32_arg(cpu, 0), ctx = m32_malloc(sizeof(Frame));
    int r = cleanup(vm, cpu, caller(cpu), exc, ctx);
    m32_free(ctx);
    if (r == INSTALLED)
        return OCERZ_STEP_OK;
    fprintf(stderr, "ocerz: m32: _Unwind_Resume could not reach the catching frame\n");
    abort();
}

static int sp_delete(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t exc = m32_arg(cpu, 0), fn = m32_rd(exc + 8);
    if (fn) {
        uint32_t args[2] = { 1, exc };   /* _URC_FOREIGN_EXCEPTION_CAUGHT */
        m32_call(vm, fn, args, 2, NULL, NULL);
    }
    RET(0);
}

/* ---- what the personality asks of its context ---- */

static Frame *ctx(const OcerzCPU *cpu) { return (Frame *)m32_h(m32_arg(cpu, 0)); }
static int sp_get_gr(struct OcerzVM *vm, OcerzCPU *cpu) { uint32_t i = m32_arg(cpu, 1); RET(i < NREG ? ctx(cpu)->reg[i] : 0); }
static int sp_set_gr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t i = m32_arg(cpu, 1);
    if (i < NREG)
        ctx(cpu)->reg[i] = m32_arg(cpu, 2);
    RET(0);
}
static int sp_get_ip(struct OcerzVM *vm, OcerzCPU *cpu) { RET(ctx(cpu)->reg[R_EIP]); }
static int sp_get_ip_info(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), 0);   /* a return address, not the faulting instruction */
    RET(ctx(cpu)->reg[R_EIP]);
}
/* a landing pad expects the call's pushed arguments gone (DW_CFA_GNU_args_size), as libunwind adjusts here */
static int sp_set_ip(struct OcerzVM *vm, OcerzCPU *cpu)
{
    Frame *f = ctx(cpu);
    f->reg[R_EIP] = m32_arg(cpu, 1);
    f->reg[R_ESP] += f->args_size;
    f->args_size = 0;
    RET(0);
}
static int sp_get_lsda(struct OcerzVM *vm, OcerzCPU *cpu) { RET(ctx(cpu)->lsda); }
static int sp_get_region_start(struct OcerzVM *vm, OcerzCPU *cpu) { RET(ctx(cpu)->func); }
static int sp_get_cfa(struct OcerzVM *vm, OcerzCPU *cpu) { RET(ctx(cpu)->reg[R_ESP]); }
static int sp_zero(struct OcerzVM *vm, OcerzCPU *cpu) { RET(0); }   /* data/text-relative bases: none on Darwin */
static int sp_enclosing(struct OcerzVM *vm, OcerzCPU *cpu)
{
    Frame f;
    Row row;
    memset(&f, 0, sizeof f);
    f.reg[R_EIP] = m32_arg(cpu, 0) + 1;
    RET(frame_info(&f, &row) ? f.func : 0);
}

const M32SpecialEntry m32_unwind_specials[] = {
    { "__Unwind_RaiseException", sp_raise }, { "__Unwind_Resume_or_Rethrow", sp_raise },
    { "__Unwind_Resume", sp_resume }, { "__Unwind_DeleteException", sp_delete },
    { "__Unwind_GetGR", sp_get_gr }, { "__Unwind_SetGR", sp_set_gr },
    { "__Unwind_GetIP", sp_get_ip }, { "__Unwind_GetIPInfo", sp_get_ip_info }, { "__Unwind_SetIP", sp_set_ip },
    { "__Unwind_GetLanguageSpecificData", sp_get_lsda }, { "__Unwind_GetRegionStart", sp_get_region_start },
    { "__Unwind_GetCFA", sp_get_cfa }, { "__Unwind_GetDataRelBase", sp_zero }, { "__Unwind_GetTextRelBase", sp_zero },
    { "__Unwind_FindEnclosingFunction", sp_enclosing },
    { NULL, NULL }
};
