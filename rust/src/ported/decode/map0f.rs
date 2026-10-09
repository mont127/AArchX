//! The 0F opcode map and its SSE prefix helpers.

use super::*;

pub(super) fn sse_prefix(s: &DecState) -> c_int {
    s.mand
}

pub(super) unsafe fn decode_sse_rr(
    s: &mut DecState,
    op: c_int,
    size: c_int,
    reg_is_dst: bool,
) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let out = s.out;
    set_op(s, op);
    (*out).opsize = size as u8;
    (*out).nops = 2;
    if reg_is_dst {
        set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
        place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, false);
    } else {
        place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, false);
        set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
    }
    OCERZ_OK as c_int
}

pub(super) unsafe fn decode_sse_rri(
    s: &mut DecState,
    op: c_int,
    size: c_int,
    reg_is_dst: bool,
) -> c_int {
    let e = decode_sse_rr(s, op, size, reg_is_dst);
    if e != 0 {
        return e;
    }
    let out = s.out;
    (*out).nops = 3;
    read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
}

pub(super) unsafe fn decode_mmx_rr(s: &mut DecState, op: c_int, reg_is_dst: bool) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, 8);
    if e != 0 {
        return e;
    }
    let out = s.out;
    set_op(s, op);
    (*out).opsize = 8;
    (*out).nops = 2;
    let r = if reg_is_dst {
        ptr::addr_of_mut!((*out).ops[0])
    } else {
        ptr::addr_of_mut!((*out).ops[1])
    };
    let x = if reg_is_dst {
        ptr::addr_of_mut!((*out).ops[1])
    } else {
        ptr::addr_of_mut!((*out).ops[0])
    };
    set_mmx(r, m.reg);
    if rm_is_reg(&m) {
        set_mmx(x, m.rm);
    } else {
        *x = m.mem;
        (*x).size = 8;
    }
    OCERZ_OK as c_int
}

pub(super) unsafe fn decode_mmx_rri(s: &mut DecState, op: c_int) -> c_int {
    let e = decode_mmx_rr(s, op, true);
    if e != 0 {
        return e;
    }
    let out = s.out;
    (*out).nops = 3;
    read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
}

fn select_packed(s: &DecState, ps: c_int, pd: c_int, ss: c_int, sd: c_int) -> c_int {
    match sse_prefix(s) {
        MAND_NONE => ps,
        MAND_66 => pd,
        MAND_F3 => ss,
        _ => sd,
    }
}

fn sse_scalar_size(s: &DecState, packed_def: c_int) -> c_int {
    match sse_prefix(s) {
        MAND_F3 => 4,
        MAND_F2 => 8,
        _ => packed_def,
    }
}

unsafe fn decode_arith_sse(s: &mut DecState, ps: c_int, pd: c_int, ss: c_int, sd: c_int) -> c_int {
    let op = select_packed(s, ps, pd, ss, sd);
    if op < 0 {
        return OCERZ_EUNDEF as c_int;
    }
    decode_sse_rr(s, op, sse_scalar_size(s, 16), true)
}

unsafe fn decode_pint(s: &mut DecState, op: c_int, reg_is_dst: bool) -> c_int {
    if sse_prefix(s) == MAND_NONE
        && s.vex == 0
        && op != OCERZ_OP_PUNPCKLQDQ as c_int
        && op != OCERZ_OP_PUNPCKHQDQ as c_int
    {
        return decode_mmx_rr(s, op, reg_is_dst);
    }
    if sse_prefix(s) != MAND_66 {
        return OCERZ_EUNDEF as c_int;
    }
    decode_sse_rr(s, op, 16, reg_is_dst)
}

pub(super) unsafe fn decode_pint_imm(s: &mut DecState, op: c_int) -> c_int {
    if sse_prefix(s) != MAND_66 {
        return OCERZ_EUNDEF as c_int;
    }
    decode_sse_rri(s, op, 16, true)
}

pub(super) unsafe fn decode_0f(s: &mut DecState, op2: u8) -> c_int {
    let mut e;
    let mand = sse_prefix(s);
    match op2 {
        0x01 => {
            let mut modrm = 0u8;
            e = fetch8(s, &mut modrm);
            if e != 0 {
                return e;
            }
            if modrm == 0xd0 {
                (*s.out).nops = 0;
                set_op(s, OCERZ_OP_XGETBV as c_int);
                return OCERZ_OK as c_int;
            }
            if modrm == 0xf9 {
                (*s.out).nops = 0;
                set_op(s, OCERZ_OP_RDTSCP as c_int);
                return OCERZ_OK as c_int;
            }
            let grpreg = ((modrm >> 3) & 7) as c_int;
            if modrm < 0xc0 && (grpreg == 0 || grpreg == 1) {
                let mut m: ModRM = mem::zeroed();
                let psize = if s.mode32 != 0 { 6 } else { 10 };
                s.p = s.p.sub(1);
                e = decode_modrm(s, &mut m, psize);
                if e != 0 {
                    return e;
                }
                set_op(
                    s,
                    if grpreg == 0 {
                        OCERZ_OP_SGDT as c_int
                    } else {
                        OCERZ_OP_SIDT as c_int
                    },
                );
                let out = s.out;
                (*out).nops = 1;
                (*out).ops[0] = m.mem;
                (*out).ops[0].size = psize as u8;
                return OCERZ_OK as c_int;
            }
            (*s.out).nops = 0;
            return OCERZ_EUNDEF as c_int;
        }
        0x05 => {
            set_op(s, OCERZ_OP_SYSCALL as c_int);
            (*s.out).nops = 0;
            return OCERZ_OK as c_int;
        }
        0x0b | 0xb9 => {
            if op2 == 0xb9 {
                let mut m: ModRM = mem::zeroed();
                e = decode_modrm(s, &mut m, 4);
                if e != 0 {
                    return e;
                }
            }
            set_op(s, OCERZ_OP_UD2 as c_int);
            (*s.out).nops = 0;
            return OCERZ_OK as c_int;
        }
        0x0d => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 1);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_PREFETCH as c_int);
            let out = s.out;
            (*out).nops = 1;
            if rm_is_reg(&m) {
                set_reg(ptr::addr_of_mut!((*out).ops[0]), m.rm, 1);
            } else {
                (*out).ops[0] = m.mem;
                (*out).ops[0].size = 1;
            }
            return OCERZ_OK as c_int;
        }
        0x18..=0x1f => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 1);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_NOP as c_int);
            (*s.out).nops = 0;
            return OCERZ_OK as c_int;
        }
        0x10 | 0x11 => {
            let reg_is_dst = op2 == 0x10;
            let (op, size) = match mand {
                MAND_F3 => (OCERZ_OP_MOVSS as c_int, 4),
                MAND_F2 => (OCERZ_OP_MOVSDX as c_int, 8),
                _ => (OCERZ_OP_MOVUPS as c_int, 16),
            };
            return decode_sse_rr(s, op, size, reg_is_dst);
        }
        0x12 => {
            let mut m: ModRM = mem::zeroed();
            let size = if mand == MAND_F2 { 8 } else { 16 };
            let mut op = match mand {
                MAND_66 => OCERZ_OP_MOVLPS as c_int,
                MAND_F2 => OCERZ_OP_MOVDDUP as c_int,
                MAND_F3 => OCERZ_OP_MOVSLDUP as c_int,
                _ => OCERZ_OP_MOVLPS as c_int,
            };
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            if mand == MAND_NONE && rm_is_reg(&m) {
                op = OCERZ_OP_MOVHLPS as c_int;
            }
            set_op(s, op);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, false);
            return OCERZ_OK as c_int;
        }
        0x13 => {
            if mand != MAND_NONE && mand != MAND_66 {
                return OCERZ_EUNDEF as c_int;
            }
            return decode_sse_rr(s, OCERZ_OP_MOVLPS as c_int, 16, false);
        }
        0x14 => {
            return decode_sse_rr(
                s,
                if mand == MAND_66 {
                    OCERZ_OP_UNPCKLPD as c_int
                } else {
                    OCERZ_OP_UNPCKLPS as c_int
                },
                16,
                true,
            );
        }
        0x15 => {
            return decode_sse_rr(
                s,
                if mand == MAND_66 {
                    OCERZ_OP_UNPCKHPD as c_int
                } else {
                    OCERZ_OP_UNPCKHPS as c_int
                },
                16,
                true,
            );
        }
        0x16 => {
            let mut m: ModRM = mem::zeroed();
            let mut op = match mand {
                MAND_66 => OCERZ_OP_MOVHPS as c_int,
                MAND_F3 => OCERZ_OP_MOVSHDUP as c_int,
                _ => OCERZ_OP_MOVHPS as c_int,
            };
            e = decode_modrm(s, &mut m, 16);
            if e != 0 {
                return e;
            }
            if mand == MAND_NONE && rm_is_reg(&m) {
                op = OCERZ_OP_MOVLHPS as c_int;
            }
            set_op(s, op);
            let out = s.out;
            (*out).opsize = 16;
            (*out).nops = 2;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), 16, false);
            return OCERZ_OK as c_int;
        }
        0x17 => {
            if mand != MAND_NONE && mand != MAND_66 {
                return OCERZ_EUNDEF as c_int;
            }
            return decode_sse_rr(s, OCERZ_OP_MOVHPS as c_int, 16, false);
        }
        0x28 => return decode_sse_rr(s, OCERZ_OP_MOVAPS as c_int, 16, true),
        0x29 => return decode_sse_rr(s, OCERZ_OP_MOVAPS as c_int, 16, false),
        0x2a => {
            if (mand == MAND_NONE || mand == MAND_66) && s.vex == 0 {
                let mut m: ModRM = mem::zeroed();
                e = decode_modrm(s, &mut m, 8);
                if e != 0 {
                    return e;
                }
                set_op(
                    s,
                    if mand == MAND_NONE {
                        OCERZ_OP_CVTPI2PS as c_int
                    } else {
                        OCERZ_OP_CVTPI2PD as c_int
                    },
                );
                let out = s.out;
                (*out).opsize = 8;
                (*out).nops = 2;
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
                if rm_is_reg(&m) {
                    set_mmx(ptr::addr_of_mut!((*out).ops[1]), m.rm);
                } else {
                    (*out).ops[1] = m.mem;
                    (*out).ops[1].size = 8;
                }
                return OCERZ_OK as c_int;
            }
            if mand != MAND_F3 && mand != MAND_F2 {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            let gsize = if s.rex_w != 0 { 8 } else { 4 };
            e = decode_modrm(s, &mut m, gsize);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if mand == MAND_F3 {
                    OCERZ_OP_CVTSI2SS as c_int
                } else {
                    OCERZ_OP_CVTSI2SD as c_int
                },
            );
            let out = s.out;
            let size = if mand == MAND_F3 { 4 } else { 8 };
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), gsize, true);
            return OCERZ_OK as c_int;
        }
        0x2c | 0x2d => {
            if (mand == MAND_NONE || mand == MAND_66) && s.vex == 0 {
                let mut m: ModRM = mem::zeroed();
                let msize = if mand == MAND_NONE { 8 } else { 16 };
                e = decode_modrm(s, &mut m, msize);
                if e != 0 {
                    return e;
                }
                set_op(
                    s,
                    if op2 == 0x2c {
                        if mand == MAND_NONE {
                            OCERZ_OP_CVTTPS2PI as c_int
                        } else {
                            OCERZ_OP_CVTTPD2PI as c_int
                        }
                    } else if mand == MAND_NONE {
                        OCERZ_OP_CVTPS2PI as c_int
                    } else {
                        OCERZ_OP_CVTPD2PI as c_int
                    },
                );
                let out = s.out;
                (*out).opsize = 8;
                (*out).nops = 2;
                set_mmx(ptr::addr_of_mut!((*out).ops[0]), m.reg);
                place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), msize, false);
                return OCERZ_OK as c_int;
            }
            if mand != MAND_F3 && mand != MAND_F2 {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            let gsize = if s.rex_w != 0 { 8 } else { 4 };
            let ssize = if mand == MAND_F3 { 4 } else { 8 };
            e = decode_modrm(s, &mut m, ssize);
            if e != 0 {
                return e;
            }
            let op = if op2 == 0x2c {
                if mand == MAND_F3 {
                    OCERZ_OP_CVTTSS2SI as c_int
                } else {
                    OCERZ_OP_CVTTSD2SI as c_int
                }
            } else if mand == MAND_F3 {
                OCERZ_OP_CVTSS2SI as c_int
            } else {
                OCERZ_OP_CVTSD2SI as c_int
            };
            set_op(s, op);
            let out = s.out;
            (*out).opsize = gsize as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, gsize);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), ssize, false);
            return OCERZ_OK as c_int;
        }
        0x2e | 0x2f => {
            return decode_sse_rr(
                s,
                if op2 == 0x2e {
                    if mand == MAND_66 {
                        OCERZ_OP_UCOMISD as c_int
                    } else {
                        OCERZ_OP_UCOMISS as c_int
                    }
                } else if mand == MAND_66 {
                    OCERZ_OP_COMISD as c_int
                } else {
                    OCERZ_OP_COMISS as c_int
                },
                if mand == MAND_66 { 8 } else { 4 },
                true,
            );
        }
        0x31 => {
            set_op(s, OCERZ_OP_RDTSC as c_int);
            (*s.out).nops = 0;
            return OCERZ_OK as c_int;
        }
        0xa2 => {
            set_op(s, OCERZ_OP_CPUID as c_int);
            (*s.out).nops = 0;
            return OCERZ_OK as c_int;
        }
        _ => {}
    }
    if (0x40..=0x4f).contains(&op2) {
        let mut m: ModRM = mem::zeroed();
        let size = opsize_default(s);
        e = decode_modrm(s, &mut m, size);
        if e != 0 {
            return e;
        }
        set_op(s, OCERZ_OP_CMOVCC as c_int);
        let out = s.out;
        (*out).opsize = size as u8;
        (*out).cc = (op2 & 0xf) as u8;
        (*out).nops = 2;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
        place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
        return OCERZ_OK as c_int;
    }
    if (0x80..=0x8f).contains(&op2) {
        return branch_rel(s, OCERZ_OP_JCC as c_int, 4, (op2 & 0xf) as c_int);
    }
    if (0x90..=0x9f).contains(&op2) {
        let mut m: ModRM = mem::zeroed();
        e = decode_modrm(s, &mut m, 1);
        if e != 0 {
            return e;
        }
        set_op(s, OCERZ_OP_SETCC as c_int);
        let out = s.out;
        (*out).opsize = 1;
        (*out).cc = (op2 & 0xf) as u8;
        (*out).nops = 1;
        place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
        return OCERZ_OK as c_int;
    }
    match op2 {
        0x1e => {
            if mand == MAND_F3 && s.p < s.end && *s.p == 0xfa {
                let mut b = 0u8;
                e = fetch8(s, &mut b);
                if e != 0 {
                    return e;
                }
                set_op(s, OCERZ_OP_NOP as c_int);
                (*s.out).nops = 0;
                return OCERZ_OK as c_int;
            }
            OCERZ_EUNDEF as c_int
        }
        0x50 => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 4);
            if e != 0 {
                return e;
            }
            if !rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(
                s,
                if mand == MAND_66 {
                    OCERZ_OP_MOVMSKPD as c_int
                } else {
                    OCERZ_OP_MOVMSKPS as c_int
                },
            );
            let out = s.out;
            (*out).opsize = 4;
            (*out).nops = 2;
            set_reg(
                ptr::addr_of_mut!((*out).ops[0]),
                m.reg,
                if s.rex_w != 0 { 8 } else { 4 },
            );
            set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.rm, 16);
            OCERZ_OK as c_int
        }
        0x51 => decode_arith_sse(
            s,
            OCERZ_OP_SQRTPS as c_int,
            OCERZ_OP_SQRTPD as c_int,
            OCERZ_OP_SQRTSS as c_int,
            OCERZ_OP_SQRTSD as c_int,
        ),
        0x52 => {
            if mand == MAND_F3 {
                decode_sse_rr(s, OCERZ_OP_RSQRTSS as c_int, 4, true)
            } else if mand == MAND_NONE {
                decode_sse_rr(s, OCERZ_OP_RSQRTPS as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0x53 => {
            if mand == MAND_F3 {
                decode_sse_rr(s, OCERZ_OP_RCPSS as c_int, 4, true)
            } else if mand == MAND_NONE {
                decode_sse_rr(s, OCERZ_OP_RCPPS as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0x54 => decode_sse_rr(s, OCERZ_OP_ANDPS as c_int, 16, true),
        0x55 => decode_sse_rr(s, OCERZ_OP_ANDNPS as c_int, 16, true),
        0x56 => decode_sse_rr(s, OCERZ_OP_ORPS as c_int, 16, true),
        0x57 => decode_sse_rr(s, OCERZ_OP_XORPS as c_int, 16, true),
        0x58 => decode_arith_sse(
            s,
            OCERZ_OP_ADDPS as c_int,
            OCERZ_OP_ADDPD as c_int,
            OCERZ_OP_ADDSS as c_int,
            OCERZ_OP_ADDSD as c_int,
        ),
        0x59 => decode_arith_sse(
            s,
            OCERZ_OP_MULPS as c_int,
            OCERZ_OP_MULPD as c_int,
            OCERZ_OP_MULSS as c_int,
            OCERZ_OP_MULSD as c_int,
        ),
        0x5c => decode_arith_sse(
            s,
            OCERZ_OP_SUBPS as c_int,
            OCERZ_OP_SUBPD as c_int,
            OCERZ_OP_SUBSS as c_int,
            OCERZ_OP_SUBSD as c_int,
        ),
        0x5d => decode_arith_sse(
            s,
            OCERZ_OP_MINPS as c_int,
            OCERZ_OP_MINPD as c_int,
            OCERZ_OP_MINSS as c_int,
            OCERZ_OP_MINSD as c_int,
        ),
        0x5e => decode_arith_sse(
            s,
            OCERZ_OP_DIVPS as c_int,
            OCERZ_OP_DIVPD as c_int,
            OCERZ_OP_DIVSS as c_int,
            OCERZ_OP_DIVSD as c_int,
        ),
        0x5f => decode_arith_sse(
            s,
            OCERZ_OP_MAXPS as c_int,
            OCERZ_OP_MAXPD as c_int,
            OCERZ_OP_MAXSS as c_int,
            OCERZ_OP_MAXSD as c_int,
        ),
        0x7c => {
            if mand == MAND_66 {
                decode_sse_rr(s, OCERZ_OP_HADDPD as c_int, 16, true)
            } else if mand == MAND_F2 {
                decode_sse_rr(s, OCERZ_OP_HADDPS as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0x7d => {
            if mand == MAND_66 {
                decode_sse_rr(s, OCERZ_OP_HSUBPD as c_int, 16, true)
            } else if mand == MAND_F2 {
                decode_sse_rr(s, OCERZ_OP_HSUBPS as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0xd0 => {
            if mand == MAND_66 {
                decode_sse_rr(s, OCERZ_OP_ADDSUBPD as c_int, 16, true)
            } else if mand == MAND_F2 {
                decode_sse_rr(s, OCERZ_OP_ADDSUBPS as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0x5a => {
            let (op, isize, osize) = match mand {
                MAND_66 => (OCERZ_OP_CVTPD2PS as c_int, 16, 16),
                MAND_F3 => (OCERZ_OP_CVTSS2SD as c_int, 4, 8),
                MAND_F2 => (OCERZ_OP_CVTSD2SS as c_int, 8, 4),
                _ => (OCERZ_OP_CVTPS2PD as c_int, 16, 16),
            };
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, isize);
            if e != 0 {
                return e;
            }
            set_op(s, op);
            let out = s.out;
            (*out).opsize = osize as u8;
            (*out).nops = 2;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, osize);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), isize, false);
            OCERZ_OK as c_int
        }
        0x5b => {
            let op = match mand {
                MAND_66 => OCERZ_OP_CVTPS2DQ as c_int,
                MAND_F3 => OCERZ_OP_CVTTPS2DQ as c_int,
                MAND_NONE => OCERZ_OP_CVTDQ2PS as c_int,
                _ => return OCERZ_EUNDEF as c_int,
            };
            decode_sse_rr(s, op, 16, true)
        }
        0x2b => {
            if mand != MAND_NONE && mand != MAND_66 {
                return OCERZ_EUNDEF as c_int;
            }
            e = decode_sse_rr(s, OCERZ_OP_MOVUPS as c_int, 16, false);
            if e != 0 {
                return e;
            }
            if (*s.out).ops[0].kind != OCERZ_OPK_MEM as u8 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OK as c_int
        }
        0x60..=0x6d => {
            let op = match op2 {
                0x60 => OCERZ_OP_PUNPCKLBW,
                0x61 => OCERZ_OP_PUNPCKLWD,
                0x62 => OCERZ_OP_PUNPCKLDQ,
                0x63 => OCERZ_OP_PACKSSWB,
                0x64 => OCERZ_OP_PCMPGTB,
                0x65 => OCERZ_OP_PCMPGTW,
                0x66 => OCERZ_OP_PCMPGTD,
                0x67 => OCERZ_OP_PACKUSWB,
                0x68 => OCERZ_OP_PUNPCKHBW,
                0x69 => OCERZ_OP_PUNPCKHWD,
                0x6a => OCERZ_OP_PUNPCKHDQ,
                0x6b => OCERZ_OP_PACKSSDW,
                0x6c => OCERZ_OP_PUNPCKLQDQ,
                _ => OCERZ_OP_PUNPCKHQDQ,
            };
            decode_pint(s, op as c_int, true)
        }
        0x6e => {
            if mand != MAND_66 && (mand != MAND_NONE || s.vex != 0) {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            let gsize = if s.rex_w != 0 { 8 } else { 4 };
            e = decode_modrm(s, &mut m, gsize);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if s.rex_w != 0 {
                    OCERZ_OP_MOVQX as c_int
                } else {
                    OCERZ_OP_MOVD as c_int
                },
            );
            let out = s.out;
            (*out).opsize = gsize as u8;
            (*out).nops = 2;
            if mand == MAND_NONE {
                set_mmx(ptr::addr_of_mut!((*out).ops[0]), m.reg);
            } else {
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
            }
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), gsize, true);
            OCERZ_OK as c_int
        }
        0x6f => {
            if mand == MAND_66 {
                decode_sse_rr(s, OCERZ_OP_MOVDQA as c_int, 16, true)
            } else if mand == MAND_F3 {
                decode_sse_rr(s, OCERZ_OP_MOVDQU as c_int, 16, true)
            } else if mand == MAND_NONE && s.vex == 0 {
                decode_mmx_rr(s, OCERZ_OP_MOVQX as c_int, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0xf0 => {
            if mand == MAND_F2 {
                decode_sse_rr(s, OCERZ_OP_MOVDQU as c_int, 16, true)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0x70 => match mand {
            MAND_66 => decode_sse_rri(s, OCERZ_OP_PSHUFD as c_int, 16, true),
            MAND_F2 => decode_sse_rri(s, OCERZ_OP_PSHUFLW as c_int, 16, true),
            MAND_F3 => decode_sse_rri(s, OCERZ_OP_PSHUFHW as c_int, 16, true),
            MAND_NONE if s.vex != 0 => OCERZ_EUNDEF as c_int,
            MAND_NONE => decode_mmx_rri(s, OCERZ_OP_PSHUFLW as c_int),
            _ => OCERZ_EUNDEF as c_int,
        },
        0x71..=0x73 => {
            let mmx = mand == MAND_NONE && s.vex == 0;
            if mand != MAND_66 && !mmx {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 16);
            if e != 0 {
                return e;
            }
            if !rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            let idx = m.reg & 7;
            let op = if op2 == 0x71 {
                match idx {
                    2 => OCERZ_OP_PSRLW,
                    4 => OCERZ_OP_PSRAW,
                    6 => OCERZ_OP_PSLLW,
                    _ => OCERZ_OP_INVALID,
                }
            } else if op2 == 0x72 {
                match idx {
                    2 => OCERZ_OP_PSRLD,
                    4 => OCERZ_OP_PSRAD,
                    6 => OCERZ_OP_PSLLD,
                    _ => OCERZ_OP_INVALID,
                }
            } else {
                match idx {
                    2 => OCERZ_OP_PSRLQ,
                    3 => OCERZ_OP_PSRLDQ,
                    6 => OCERZ_OP_PSLLQ,
                    7 => OCERZ_OP_PSLLDQ,
                    _ => OCERZ_OP_INVALID,
                }
            };
            if op == OCERZ_OP_INVALID || (mmx && (op == OCERZ_OP_PSRLDQ || op == OCERZ_OP_PSLLDQ)) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, op as c_int);
            let out = s.out;
            (*out).opsize = if mmx { 8 } else { 16 };
            (*out).nops = 2;
            if mmx {
                set_mmx(ptr::addr_of_mut!((*out).ops[0]), m.rm);
            } else {
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.rm, 16);
            }
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
        }
        0x74 => decode_pint(s, OCERZ_OP_PCMPEQB as c_int, true),
        0x75 => decode_pint(s, OCERZ_OP_PCMPEQW as c_int, true),
        0x76 => decode_pint(s, OCERZ_OP_PCMPEQD as c_int, true),
        0x77 => {
            set_op(
                s,
                if s.vex != 0 {
                    if s.vex_l != 0 {
                        OCERZ_OP_VZEROALL as c_int
                    } else {
                        OCERZ_OP_VZEROUPPER as c_int
                    }
                } else {
                    OCERZ_OP_EMMS as c_int
                },
            );
            (*s.out).nops = 0;
            OCERZ_OK as c_int
        }
        0x7e => {
            if mand == MAND_F3 {
                let mut m: ModRM = mem::zeroed();
                e = decode_modrm(s, &mut m, 8);
                if e != 0 {
                    return e;
                }
                set_op(s, OCERZ_OP_MOVQX as c_int);
                let out = s.out;
                (*out).opsize = 8;
                (*out).nops = 2;
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 8);
                place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), 8, false);
                return OCERZ_OK as c_int;
            }
            if mand == MAND_66 || (mand == MAND_NONE && s.vex == 0) {
                let mut m: ModRM = mem::zeroed();
                let gsize = if s.rex_w != 0 { 8 } else { 4 };
                e = decode_modrm(s, &mut m, gsize);
                if e != 0 {
                    return e;
                }
                set_op(
                    s,
                    if s.rex_w != 0 {
                        OCERZ_OP_MOVQX as c_int
                    } else {
                        OCERZ_OP_MOVD as c_int
                    },
                );
                let out = s.out;
                (*out).opsize = gsize as u8;
                (*out).nops = 2;
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), gsize, true);
                if mand == MAND_NONE {
                    set_mmx(ptr::addr_of_mut!((*out).ops[1]), m.reg);
                } else {
                    set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.reg, 16);
                }
                return OCERZ_OK as c_int;
            }
            OCERZ_EUNDEF as c_int
        }
        0x7f => {
            if mand == MAND_66 {
                decode_sse_rr(s, OCERZ_OP_MOVDQA as c_int, 16, false)
            } else if mand == MAND_F3 {
                decode_sse_rr(s, OCERZ_OP_MOVDQU as c_int, 16, false)
            } else if mand == MAND_NONE && s.vex == 0 {
                decode_mmx_rr(s, OCERZ_OP_MOVQX as c_int, false)
            } else {
                OCERZ_EUNDEF as c_int
            }
        }
        0xa3 => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_BT as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            OCERZ_OK as c_int
        }
        0xab | 0xb3 | 0xbb => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                match op2 {
                    0xab => OCERZ_OP_BTS as c_int,
                    0xb3 => OCERZ_OP_BTR as c_int,
                    _ => OCERZ_OP_BTC as c_int,
                },
            );
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            OCERZ_OK as c_int
        }
        0xa4 | 0xa5 | 0xac | 0xad => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if op2 == 0xa4 || op2 == 0xa5 {
                    OCERZ_OP_SHLD as c_int
                } else {
                    OCERZ_OP_SHRD as c_int
                },
            );
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 3;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            if op2 == 0xa4 || op2 == 0xac {
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
            } else {
                set_reg(ptr::addr_of_mut!((*out).ops[2]), OCERZ_RCX as c_int, 1);
                OCERZ_OK as c_int
            }
        }
        0xae => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 0);
            if e != 0 {
                return e;
            }
            let idx = m.reg & 7;
            if rm_is_reg(&m) {
                let lo = (m.rm & 7) as u8;
                let fence = match idx {
                    5 if lo == 0 => Some(OCERZ_OP_LFENCE),
                    6 if lo == 0 => Some(OCERZ_OP_MFENCE),
                    7 if lo == 0 => Some(OCERZ_OP_SFENCE),
                    _ => None,
                };
                if let Some(op) = fence {
                    set_op(s, op as c_int);
                    (*s.out).nops = 0;
                    return OCERZ_OK as c_int;
                }
                return OCERZ_EUNDEF as c_int;
            }
            let op = match idx {
                0 => OCERZ_OP_FXSAVE,
                1 => OCERZ_OP_FXRSTOR,
                2 if mand != MAND_F3 => OCERZ_OP_LDMXCSR,
                3 if mand != MAND_F3 => OCERZ_OP_STMXCSR,
                4 | 5 if mand == MAND_NONE && s.vex == 0 => {
                    if idx == 4 {
                        OCERZ_OP_XSAVE
                    } else {
                        OCERZ_OP_XRSTOR
                    }
                }
                7 => OCERZ_OP_CLFLUSH,
                _ => return OCERZ_EUNDEF as c_int,
            };
            set_op(s, op as c_int);
            let out = s.out;
            (*out).nops = 1;
            (*out).ops[0] = m.mem;
            (*out).ops[0].size = 0;
            OCERZ_OK as c_int
        }
        0xaf => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_IMUL as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
            OCERZ_OK as c_int
        }
        0xb0 | 0xb1 => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op2 == 0xb0;
            let size = if byte_form { 1 } else { opsize_default(s) };
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_CMPXCHG as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
                set_reg8(s, ptr::addr_of_mut!((*out).ops[1]), m.reg);
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
                set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            }
            OCERZ_OK as c_int
        }
        0xb6 | 0xb7 | 0xbe | 0xbf => {
            let mut m: ModRM = mem::zeroed();
            let ssize = if op2 == 0xb6 || op2 == 0xbe { 1 } else { 2 };
            let dsize = opsize_default(s);
            e = decode_modrm(s, &mut m, ssize);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if op2 == 0xb6 || op2 == 0xb7 {
                    OCERZ_OP_MOVZX as c_int
                } else {
                    OCERZ_OP_MOVSX as c_int
                },
            );
            let out = s.out;
            (*out).opsize = dsize as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, dsize);
            if ssize == 1 {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[1]));
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), ssize, true);
            }
            OCERZ_OK as c_int
        }
        0xb8 => {
            if mand != MAND_F3 {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_POPCNT as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
            OCERZ_OK as c_int
        }
        0xba => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            let op = match m.reg & 7 {
                4 => OCERZ_OP_BT,
                5 => OCERZ_OP_BTS,
                6 => OCERZ_OP_BTR,
                7 => OCERZ_OP_BTC,
                _ => return OCERZ_EUNDEF as c_int,
            };
            set_op(s, op as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
        }
        0xbc | 0xbd => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            let op = if op2 == 0xbc {
                if mand == MAND_F3 {
                    OCERZ_OP_TZCNT
                } else {
                    OCERZ_OP_BSF
                }
            } else if mand == MAND_F3 {
                OCERZ_OP_LZCNT
            } else {
                OCERZ_OP_BSR
            };
            set_op(s, op as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
            OCERZ_OK as c_int
        }
        0xc0 | 0xc1 => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op2 == 0xc0;
            let size = if byte_form { 1 } else { opsize_default(s) };
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_XADD as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
                set_reg8(s, ptr::addr_of_mut!((*out).ops[1]), m.reg);
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
                set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            }
            OCERZ_OK as c_int
        }
        0xc2 => {
            let op = select_packed(
                s,
                OCERZ_OP_CMPPS as c_int,
                OCERZ_OP_CMPPD as c_int,
                OCERZ_OP_CMPSS as c_int,
                OCERZ_OP_CMPSDX as c_int,
            );
            decode_sse_rri(s, op, sse_scalar_size(s, 16), true)
        }
        0xc3 => {
            let mut m: ModRM = mem::zeroed();
            let mut size = opsize_default(s);
            if size < 4 {
                size = 4;
            }
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            if rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_MOVNTI as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            (*out).ops[0] = m.mem;
            (*out).ops[0].size = size as u8;
            set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            OCERZ_OK as c_int
        }
        0xc4 => {
            if mand != MAND_66 && (mand != MAND_NONE || s.vex != 0) {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 4);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_PINSRW as c_int);
            let out = s.out;
            (*out).opsize = if mand == MAND_NONE { 8 } else { 16 };
            (*out).nops = 3;
            if mand == MAND_NONE {
                set_mmx(ptr::addr_of_mut!((*out).ops[0]), m.reg);
            } else {
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
            }
            if rm_is_reg(&m) {
                set_reg(ptr::addr_of_mut!((*out).ops[1]), m.rm, 4);
            } else {
                (*out).ops[1] = m.mem;
                (*out).ops[1].size = 2;
            }
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0xc5 => {
            if mand != MAND_66 && (mand != MAND_NONE || s.vex != 0) {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 4);
            if e != 0 {
                return e;
            }
            if !rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_PEXTRW as c_int);
            let out = s.out;
            (*out).opsize = 4;
            (*out).nops = 3;
            set_reg(
                ptr::addr_of_mut!((*out).ops[0]),
                m.reg,
                if s.rex_w != 0 { 8 } else { 4 },
            );
            if mand == MAND_NONE {
                set_mmx(ptr::addr_of_mut!((*out).ops[1]), m.rm);
            } else {
                set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.rm, 16);
            }
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0xc6 => decode_sse_rri(
            s,
            if mand == MAND_66 {
                OCERZ_OP_SHUFPD as c_int
            } else {
                OCERZ_OP_SHUFPS as c_int
            },
            16,
            true,
        ),
        0xc7 => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 8);
            if e != 0 {
                return e;
            }
            let idx = m.reg & 7;
            if idx == 6 && rm_is_reg(&m) {
                if mand == MAND_F3 || mand == MAND_F2 || s.vex != 0 {
                    return OCERZ_EUNDEF as c_int;
                }
                let size = opsize_default(s);
                set_op(s, OCERZ_OP_RDRAND as c_int);
                let out = s.out;
                (*out).opsize = size as u8;
                (*out).nops = 1;
                set_reg(ptr::addr_of_mut!((*out).ops[0]), m.rm, size);
                return OCERZ_OK as c_int;
            }
            if idx != 1 || rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_CMPXCHGXB as c_int);
            let out = s.out;
            (*out).opsize = if s.rex_w != 0 { 16 } else { 8 };
            (*out).nops = 1;
            (*out).ops[0] = m.mem;
            (*out).ops[0].size = if s.rex_w != 0 { 16 } else { 8 };
            OCERZ_OK as c_int
        }
        0xc8..=0xcf => {
            let reg = ((op2 - 0xc8) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
            let size = if s.rex_w != 0 { 8 } else { 4 };
            set_op(s, OCERZ_OP_BSWAP as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 1;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), reg, size);
            OCERZ_OK as c_int
        }
        0xd1 => decode_pint(s, OCERZ_OP_PSRLW as c_int, true),
        0xd2 => decode_pint(s, OCERZ_OP_PSRLD as c_int, true),
        0xd3 => decode_pint(s, OCERZ_OP_PSRLQ as c_int, true),
        0xd4 => decode_pint(s, OCERZ_OP_PADDQ as c_int, true),
        0xd5 => decode_pint(s, OCERZ_OP_PMULLW as c_int, true),
        0xd6 => {
            if mand == MAND_66 {
                return decode_sse_rr(s, OCERZ_OP_MOVQX as c_int, 8, false);
            }
            if (mand != MAND_F3 && mand != MAND_F2) || s.vex != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 8);
            if e != 0 {
                return e;
            }
            if !rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_MOVQX as c_int);
            let out = s.out;
            (*out).opsize = 8;
            (*out).nops = 2;
            if mand == MAND_F3 {
                set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
                set_mmx(ptr::addr_of_mut!((*out).ops[1]), m.rm);
            } else {
                set_mmx(ptr::addr_of_mut!((*out).ops[0]), m.reg);
                set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.rm, 16);
            }
            OCERZ_OK as c_int
        }
        0xd7 => {
            if mand != MAND_66 && (mand != MAND_NONE || s.vex != 0) {
                return OCERZ_EUNDEF as c_int;
            }
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 16);
            if e != 0 {
                return e;
            }
            if !rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_PMOVMSKB as c_int);
            let out = s.out;
            (*out).opsize = 4;
            (*out).nops = 2;
            set_reg(
                ptr::addr_of_mut!((*out).ops[0]),
                m.reg,
                if s.rex_w != 0 { 8 } else { 4 },
            );
            if mand == MAND_NONE {
                set_mmx(ptr::addr_of_mut!((*out).ops[1]), m.rm);
            } else {
                set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.rm, 16);
            }
            OCERZ_OK as c_int
        }
        0xd8 => decode_pint(s, OCERZ_OP_PSUBUSB as c_int, true),
        0xd9 => decode_pint(s, OCERZ_OP_PSUBUSW as c_int, true),
        0xda => decode_pint(s, OCERZ_OP_PMINUB as c_int, true),
        0xdb => decode_pint(s, OCERZ_OP_PAND as c_int, true),
        0xdc => decode_pint(s, OCERZ_OP_PADDUSB as c_int, true),
        0xdd => decode_pint(s, OCERZ_OP_PADDUSW as c_int, true),
        0xde => decode_pint(s, OCERZ_OP_PMAXUB as c_int, true),
        0xdf => decode_pint(s, OCERZ_OP_PANDN as c_int, true),
        0xe0 => decode_pint(s, OCERZ_OP_PAVGB as c_int, true),
        0xe1 => decode_pint(s, OCERZ_OP_PSRAW as c_int, true),
        0xe2 => decode_pint(s, OCERZ_OP_PSRAD as c_int, true),
        0xe3 => decode_pint(s, OCERZ_OP_PAVGW as c_int, true),
        0xe4 => decode_pint(s, OCERZ_OP_PMULHUW as c_int, true),
        0xe5 => decode_pint(s, OCERZ_OP_PMULHW as c_int, true),
        0xe6 => {
            let op = match mand {
                MAND_66 => OCERZ_OP_CVTTPD2DQ as c_int,
                MAND_F3 => OCERZ_OP_CVTDQ2PD as c_int,
                MAND_F2 => OCERZ_OP_CVTPD2DQ as c_int,
                _ => return OCERZ_EUNDEF as c_int,
            };
            decode_sse_rr(s, op, 16, true)
        }
        0xe7 => {
            if mand == MAND_NONE && s.vex == 0 {
                e = decode_mmx_rr(s, OCERZ_OP_MOVQX as c_int, false);
                if e != 0 {
                    return e;
                }
                return if (*s.out).ops[0].kind == OCERZ_OPK_MEM as u8 {
                    OCERZ_OK as c_int
                } else {
                    OCERZ_EUNDEF as c_int
                };
            }
            if mand != MAND_66 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_sse_rr(s, OCERZ_OP_MOVDQA as c_int, 16, false)
        }
        0xe8 => decode_pint(s, OCERZ_OP_PSUBSB as c_int, true),
        0xe9 => decode_pint(s, OCERZ_OP_PSUBSW as c_int, true),
        0xea => decode_pint(s, OCERZ_OP_PMINSW as c_int, true),
        0xeb => decode_pint(s, OCERZ_OP_POR as c_int, true),
        0xec => decode_pint(s, OCERZ_OP_PADDSB as c_int, true),
        0xed => decode_pint(s, OCERZ_OP_PADDSW as c_int, true),
        0xee => decode_pint(s, OCERZ_OP_PMAXSW as c_int, true),
        0xef => decode_pint(s, OCERZ_OP_PXOR as c_int, true),
        0xf1 => decode_pint(s, OCERZ_OP_PSLLW as c_int, true),
        0xf2 => decode_pint(s, OCERZ_OP_PSLLD as c_int, true),
        0xf3 => decode_pint(s, OCERZ_OP_PSLLQ as c_int, true),
        0xf4 => decode_pint(s, OCERZ_OP_PMULUDQ as c_int, true),
        0xf5 => decode_pint(s, OCERZ_OP_PMADDWD as c_int, true),
        0xf6 => decode_pint(s, OCERZ_OP_PSADBW as c_int, true),
        0xf8 => decode_pint(s, OCERZ_OP_PSUBB as c_int, true),
        0xf9 => decode_pint(s, OCERZ_OP_PSUBW as c_int, true),
        0xfa => decode_pint(s, OCERZ_OP_PSUBD as c_int, true),
        0xfb => decode_pint(s, OCERZ_OP_PSUBQ as c_int, true),
        0xfc => decode_pint(s, OCERZ_OP_PADDB as c_int, true),
        0xfd => decode_pint(s, OCERZ_OP_PADDW as c_int, true),
        0xfe => decode_pint(s, OCERZ_OP_PADDD as c_int, true),
        _ => OCERZ_EUNDEF as c_int,
    }
}
