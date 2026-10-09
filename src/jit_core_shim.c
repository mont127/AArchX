/*
 * Translation's guest decode loop, kept in C because it runs under
 * sigsetjmp: a fault while reading guest code siglongjmps back here through
 * ocerz_jit_decode_recover, and Rust cannot host a returns-twice call. The
 * loop and its call splicing / superblock extension are otherwise the Rust
 * core's (rust/src/ported/jit.rs), which exports the splice and per-decode
 * reset hooks used here.
 */
#include <setjmp.h>
#include "ocerz/jit_internal.h"

void jit_core_decode_reset(void);
void jit_core_ic_kind_set(int at, int kind);
int jit_core_splice_callee(uint64_t target, uint64_t ret_rip, uint64_t self_rip,
                           X86Insn *scratch, int *vn, int depth);
int jit_core_decode(uint64_t rip, int mode32, X86Insn *scratch, uint64_t *pc_out);

int jit_core_decode(uint64_t rip, int mode32, X86Insn *scratch, uint64_t *pc_out)
{
    volatile int vn = 0;
    volatile int vext = 0;
    volatile uint64_t vpc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev_dr = ocerz_jit_decode_recover;
    jit_core_decode_reset();
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (; vn < JIT_MAX_BLOCK_INSNS; ) {
            int rc = jit_decode(vpc, &scratch[vn], mode32);
            if (rc != OCERZ_OK)
                break;
            unsigned op = scratch[vn].op;
            uint8_t len = scratch[vn].len;
            if (op == OCERZ_OP_CALL && !mode32 &&
                scratch[vn].ops[0].kind == OCERZ_OPK_IMM &&
                vn + 48 < JIT_MAX_BLOCK_INSNS) {
                int at = (int)vn;
                int vni = (int)vn + 1;
                jit_core_ic_kind_set(at, 1);
                if (jit_core_splice_callee(scratch[at].ops[0].imm, vpc + len, rip,
                                           scratch, &vni, 1)) {
                    vn = vni;
                    vpc += len;
                    continue;
                }
                jit_core_ic_kind_set(at, 0);
            }
            vn++;
            if (is_terminator(op)) {
                if (op == OCERZ_OP_JCC && !mode32 && superblock_enabled() && !g_no_chain &&
                    vext < SIDE_MAX && vn < JIT_MAX_BLOCK_INSNS - 1 &&
                    scratch[vn - 1].ops[0].kind == OCERZ_OPK_IMM &&
                    (scratch[vn - 1].ops[0].imm > vpc + len ||
                     (superblock_back_enabled() && scratch[vn - 1].ops[0].imm != rip &&
                      scratch[vn - 1].ops[0].imm < vpc))) {
                    vext++;
                    if (scratch[vn - 1].ops[0].imm > vpc + len &&
                        jcc_flip_wanted(scratch[vn - 1].rip)) {
                        g_tc_learned = 1;
                        uint64_t tgt = scratch[vn - 1].ops[0].imm;
                        scratch[vn - 1].ops[0].imm = vpc + len;
                        scratch[vn - 1].cc ^= 1;
                        vpc = tgt;
                        continue;
                    }
                    vpc += len;
                    continue;
                }
                break;
            }
            vpc += len;
        }
    }
    ocerz_jit_decode_recover = prev_dr;
    *pc_out = vpc;
    return vn;
}
