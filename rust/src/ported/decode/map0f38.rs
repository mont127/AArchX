//! The 0F38 and 0F3A opcode maps.

use super::map0f::{
    decode_mmx_rr, decode_mmx_rri, decode_pint_imm, decode_sse_rr, decode_sse_rri, sse_prefix,
};
use super::*;

static GATHER_OPS: [c_int; 8] = [
    OCERZ_OP_VPGATHERDD as c_int,
    OCERZ_OP_VPGATHERDQ as c_int,
    OCERZ_OP_VPGATHERQD as c_int,
    OCERZ_OP_VPGATHERQQ as c_int,
    OCERZ_OP_VGATHERDPS as c_int,
    OCERZ_OP_VGATHERDPD as c_int,
    OCERZ_OP_VGATHERQPS as c_int,
    OCERZ_OP_VGATHERQPD as c_int,
];

unsafe fn decode_bmi(s: &mut DecState, op3: u8) -> c_int {
    if s.vex_l != 0 {
        return OCERZ_EUNDEF as c_int;
    }
    let mand = sse_prefix(s);
    let size = if s.vex_w != 0 { 8 } else { 4 };
    let mut op = OCERZ_OP_INVALID as c_int;
    let mut form = 0;
    match op3 {
        0xf2 => {
            if mand != MAND_NONE {
                return OCERZ_EUNDEF as c_int;
            }
            op = OCERZ_OP_ANDN as c_int;
        }
        0xf3 => {
            if mand != MAND_NONE {
                return OCERZ_EUNDEF as c_int;
            }
            form = 1;
        }
        0xf5 => match mand {
            MAND_NONE => {
                op = OCERZ_OP_BZHI as c_int;
                form = 2;
            }
            MAND_F2 => op = OCERZ_OP_PDEP as c_int,
            MAND_F3 => op = OCERZ_OP_PEXT as c_int,
            _ => return OCERZ_EUNDEF as c_int,
        },
        0xf6 => {
            if mand != MAND_F2 {
                return OCERZ_EUNDEF as c_int;
            }
            op = OCERZ_OP_MULX as c_int;
        }
        0xf7 => {
            op = if mand == MAND_NONE {
                OCERZ_OP_BEXTR as c_int
            } else if mand == MAND_66 {
                OCERZ_OP_SHLX as c_int
            } else if mand == MAND_F3 {
                OCERZ_OP_SARX as c_int
            } else {
                OCERZ_OP_SHRX as c_int
            };
            form = 2;
        }
        _ => return OCERZ_EUNDEF as c_int,
    }
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    if form == 1 {
        op = match m.reg & 7 {
            1 => OCERZ_OP_BLSR as c_int,
            2 => OCERZ_OP_BLSMSK as c_int,
            3 => OCERZ_OP_BLSI as c_int,
            _ => return OCERZ_EUNDEF as c_int,
        };
    }
    set_op(s, op);
    let out = s.out;
    (*out).opsize = size as u8;
    if form == 1 {
        (*out).nops = 2;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), s.vex_vvvv, size);
        place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
        return OCERZ_OK as c_int;
    }
    (*out).nops = 3;
    set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
    if form == 0 {
        set_reg(ptr::addr_of_mut!((*out).ops[1]), s.vex_vvvv, size);
        place_rm(&m, ptr::addr_of_mut!((*out).ops[2]), size, true);
    } else {
        place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
        set_reg(ptr::addr_of_mut!((*out).ops[2]), s.vex_vvvv, size);
    }
    OCERZ_OK as c_int
}

unsafe fn decode_movbe(s: &mut DecState, op3: u8) -> c_int {
    if sse_prefix(s) != MAND_NONE && sse_prefix(s) != MAND_66 {
        return OCERZ_EUNDEF as c_int;
    }
    let size = opsize_default(s);
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    if rm_is_reg(&m) {
        return OCERZ_EUNDEF as c_int;
    }
    let load = op3 == 0xf0;
    set_op(s, OCERZ_OP_MOVBE as c_int);
    let out = s.out;
    (*out).opsize = size as u8;
    (*out).nops = 2;
    set_reg(
        ptr::addr_of_mut!((*out).ops[if load { 0 } else { 1 }]),
        m.reg,
        size,
    );
    (*out).ops[if load { 1 } else { 0 }] = m.mem;
    (*out).ops[if load { 1 } else { 0 }].size = size as u8;
    OCERZ_OK as c_int
}

unsafe fn decode_maskmov(s: &mut DecState, op3: u8) -> c_int {
    let op = if op3 == 0x8c || op3 == 0x8e {
        if s.vex_w != 0 {
            OCERZ_OP_VPMASKMOVQ as c_int
        } else {
            OCERZ_OP_VPMASKMOVD as c_int
        }
    } else if s.vex_w != 0 {
        return OCERZ_EUNDEF as c_int;
    } else if op3 & 1 != 0 {
        OCERZ_OP_VMASKMOVPD as c_int
    } else {
        OCERZ_OP_VMASKMOVPS as c_int
    };
    let store = op3 == 0x2e || op3 == 0x2f || op3 == 0x8e;
    let e = decode_sse_rr(s, op, 16, !store);
    if e != 0 {
        return e;
    }
    if (*s.out).ops[if store { 0 } else { 1 }].kind == OCERZ_OPK_MEM as u8 {
        OCERZ_OK as c_int
    } else {
        OCERZ_EUNDEF as c_int
    }
}

unsafe fn decode_gather(s: &mut DecState, op3: u8) -> c_int {
    if s.end.offset_from(s.p) < 2 {
        return OCERZ_ETRUNC as c_int;
    }
    if (*s.p >> 6) == 3 || (*s.p & 7) != 4 {
        return OCERZ_EUNDEF as c_int;
    }
    let index = (((*s.p.add(1) >> 3) & 7) | (if s.rex_x != 0 { 8 } else { 0 })) as c_int;
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, 16);
    if e != 0 {
        return e;
    }
    let table_index = ((op3 - 0x90) as usize) * 2 + if s.vex_w != 0 { 1 } else { 0 };
    set_op(s, *GATHER_OPS.get_unchecked(table_index));
    let out = s.out;
    (*out).opsize = 16;
    (*out).nops = 2;
    set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
    (*out).ops[1] = m.mem;
    (*out).ops[1].index = index as u8;
    OCERZ_OK as c_int
}

pub(super) unsafe fn decode_0f38(s: &mut DecState) -> c_int {
    let mut op3 = 0u8;
    let e = fetch8(s, &mut op3);
    if e != 0 {
        return e;
    }
    if s.vex != 0 && op3 >= 0xf0 {
        return decode_bmi(s, op3);
    }
    if op3 == 0xf0 || op3 == 0xf1 {
        if sse_prefix(s) != MAND_F2 {
            return decode_movbe(s, op3);
        }
        let dsize = if s.rex_w != 0 { 8 } else { 4 };
        let ssize = if op3 == 0xf0 {
            1
        } else if s.rex_w != 0 {
            8
        } else if s.has_66 != 0 {
            2
        } else {
            4
        };
        let mut m: ModRM = mem::zeroed();
        let e = decode_modrm(s, &mut m, ssize);
        if e != 0 {
            return e;
        }
        set_op(s, OCERZ_OP_CRC32 as c_int);
        let out = s.out;
        (*out).opsize = dsize as u8;
        (*out).nops = 2;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, dsize);
        if ssize == 1 {
            place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[1]));
        } else {
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), ssize, true);
        }
        return OCERZ_OK as c_int;
    }
    let mmx =
        sse_prefix(s) == MAND_NONE && s.vex == 0 && (op3 <= 0x0b || (0x1c..=0x1e).contains(&op3));
    if sse_prefix(s) != MAND_66 && !mmx {
        return OCERZ_EUNDEF as c_int;
    }
    let op = match op3 {
        0x00 => OCERZ_OP_PSHUFB as c_int,
        0x01 => OCERZ_OP_PHADDW as c_int,
        0x02 => OCERZ_OP_PHADDD as c_int,
        0x03 => OCERZ_OP_PHADDSW as c_int,
        0x04 => OCERZ_OP_PMADDUBSW as c_int,
        0x05 => OCERZ_OP_PHSUBW as c_int,
        0x06 => OCERZ_OP_PHSUBD as c_int,
        0x07 => OCERZ_OP_PHSUBSW as c_int,
        0x08 => OCERZ_OP_PSIGNB as c_int,
        0x09 => OCERZ_OP_PSIGNW as c_int,
        0x0a => OCERZ_OP_PSIGND as c_int,
        0x0b => OCERZ_OP_PMULHRSW as c_int,
        0x0c => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPERMILPS as c_int
        }
        0x0d => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPERMILPD as c_int
        }
        0x0e => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VTESTPS as c_int
        }
        0x0f => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VTESTPD as c_int
        }
        0x13 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VCVTPH2PS as c_int
        }
        0x18 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VBROADCASTSS as c_int
        }
        0x19 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VBROADCASTSD as c_int
        }
        0x1a => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VBROADCASTF128 as c_int
        }
        0x16 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPERMPS as c_int
        }
        0x36 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPERMD as c_int
        }
        0x45 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            if s.vex_w != 0 {
                OCERZ_OP_VPSRLVQ as c_int
            } else {
                OCERZ_OP_VPSRLVD as c_int
            }
        }
        0x46 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPSRAVD as c_int
        }
        0x47 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            if s.vex_w != 0 {
                OCERZ_OP_VPSLLVQ as c_int
            } else {
                OCERZ_OP_VPSLLVD as c_int
            }
        }
        0x58 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPBROADCASTD as c_int
        }
        0x59 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPBROADCASTQ as c_int
        }
        0x5a => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VBROADCASTI128 as c_int
        }
        0x78 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPBROADCASTB as c_int
        }
        0x79 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            OCERZ_OP_VPBROADCASTW as c_int
        }
        0x2c | 0x2d | 0x2e | 0x2f | 0x8c | 0x8e => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            return decode_maskmov(s, op3);
        }
        0x90..=0x93 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            return decode_gather(s, op3);
        }
        0x10 => OCERZ_OP_PBLENDVB as c_int,
        0x14 => OCERZ_OP_BLENDVPS as c_int,
        0x15 => OCERZ_OP_BLENDVPD as c_int,
        0x17 => OCERZ_OP_PTEST as c_int,
        0x1c => OCERZ_OP_PABSB as c_int,
        0x1d => OCERZ_OP_PABSW as c_int,
        0x1e => OCERZ_OP_PABSD as c_int,
        0x20 => OCERZ_OP_PMOVSXBW as c_int,
        0x21 => OCERZ_OP_PMOVSXBD as c_int,
        0x22 => OCERZ_OP_PMOVSXBQ as c_int,
        0x23 => OCERZ_OP_PMOVSXWD as c_int,
        0x24 => OCERZ_OP_PMOVSXWQ as c_int,
        0x25 => OCERZ_OP_PMOVSXDQ as c_int,
        0x28 => OCERZ_OP_PMULDQ as c_int,
        0x29 => OCERZ_OP_PCMPEQQ as c_int,
        0x2a => OCERZ_OP_MOVDQA as c_int,
        0x30 => OCERZ_OP_PMOVZXBW as c_int,
        0x31 => OCERZ_OP_PMOVZXBD as c_int,
        0x32 => OCERZ_OP_PMOVZXBQ as c_int,
        0x33 => OCERZ_OP_PMOVZXWD as c_int,
        0x34 => OCERZ_OP_PMOVZXWQ as c_int,
        0x35 => OCERZ_OP_PMOVZXDQ as c_int,
        0x37 => OCERZ_OP_PCMPGTQ as c_int,
        0x38 => OCERZ_OP_PMINSB as c_int,
        0x39 => OCERZ_OP_PMINSD as c_int,
        0x3a => OCERZ_OP_PMINUW as c_int,
        0x3b => OCERZ_OP_PMINUD as c_int,
        0x3c => OCERZ_OP_PMAXSB as c_int,
        0x3d => OCERZ_OP_PMAXSD as c_int,
        0x3e => OCERZ_OP_PMAXUW as c_int,
        0x3f => OCERZ_OP_PMAXUD as c_int,
        0x40 => OCERZ_OP_PMULLD as c_int,
        0x41 => OCERZ_OP_PHMINPOSUW as c_int,
        0x2b => OCERZ_OP_PACKUSDW as c_int,
        0xdb => OCERZ_OP_AESIMC as c_int,
        0xdc => OCERZ_OP_AESENC as c_int,
        0xdd => OCERZ_OP_AESENCLAST as c_int,
        0xde => OCERZ_OP_AESDEC as c_int,
        0xdf => OCERZ_OP_AESDECLAST as c_int,
        _ => {
            if s.vex != 0 && (0x96..=0xbf).contains(&op3) && (op3 & 0x0f) >= 6 {
                let lo = op3 & 0x0f;
                let op = OCERZ_OP_VFMA_FIRST as c_int
                    + ((((op3 >> 4) as c_int - 9) * 10 + lo as c_int - 6) * 2)
                    + if s.vex_w != 0 { 1 } else { 0 };
                let size = if lo >= 9 && lo & 1 != 0 {
                    if s.vex_w != 0 { 8 } else { 4 }
                } else {
                    16
                };
                return decode_sse_rr(s, op, size, true);
            }
            return OCERZ_EUNDEF as c_int;
        }
    };
    if mmx {
        decode_mmx_rr(s, op, true)
    } else {
        decode_sse_rr(s, op, 16, true)
    }
}

pub(super) unsafe fn decode_0f3a(s: &mut DecState) -> c_int {
    let mut op3 = 0u8;
    let mut e = fetch8(s, &mut op3);
    if e != 0 {
        return e;
    }
    if op3 == 0x0f && sse_prefix(s) == MAND_NONE && s.vex == 0 {
        return decode_mmx_rri(s, OCERZ_OP_PALIGNR as c_int);
    }
    if op3 == 0xf0 && s.vex != 0 {
        if sse_prefix(s) != MAND_F2 || s.vex_l != 0 {
            return OCERZ_EUNDEF as c_int;
        }
        let size = if s.vex_w != 0 { 8 } else { 4 };
        let mut m: ModRM = mem::zeroed();
        e = decode_modrm(s, &mut m, size);
        if e != 0 {
            return e;
        }
        set_op(s, OCERZ_OP_RORX as c_int);
        let out = s.out;
        (*out).opsize = size as u8;
        (*out).nops = 3;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
        place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
        return read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1);
    }
    if sse_prefix(s) != MAND_66 {
        return OCERZ_EUNDEF as c_int;
    }
    match op3 {
        0x00 | 0x01 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(
                s,
                if op3 == 0 {
                    OCERZ_OP_VPERMQ as c_int
                } else {
                    OCERZ_OP_VPERMPD as c_int
                },
            )
        }
        0x02 => {
            if s.vex == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VPBLENDD as c_int)
        }
        0x38 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VINSERTI128 as c_int)
        }
        0x39 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_sse_rri(s, OCERZ_OP_VEXTRACTI128 as c_int, 16, false)
        }
        0x46 => {
            if s.vex == 0 || s.vex_l == 0 || s.vex_w != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VPERM2I128 as c_int)
        }
        0x04 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VPERMILPS as c_int)
        }
        0x05 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VPERMILPD as c_int)
        }
        0x06 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VPERM2F128 as c_int)
        }
        0x18 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_pint_imm(s, OCERZ_OP_VINSERTF128 as c_int)
        }
        0x19 => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_sse_rri(s, OCERZ_OP_VEXTRACTF128 as c_int, 16, false)
        }
        0x1d => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            decode_sse_rri(s, OCERZ_OP_VCVTPS2PH as c_int, 16, false)
        }
        0x4a | 0x4b | 0x4c => {
            if s.vex == 0 {
                return OCERZ_EUNDEF as c_int;
            }
            let bop = if op3 == 0x4a {
                OCERZ_OP_BLENDVPS as c_int
            } else if op3 == 0x4b {
                OCERZ_OP_BLENDVPD as c_int
            } else {
                OCERZ_OP_PBLENDVB as c_int
            };
            e = decode_pint_imm(s, bop);
            if e != 0 {
                return e;
            }
            let out = s.out;
            let reg = (((*out).ops[2].imm >> 4) & 0xf) as c_int;
            set_xmm(ptr::addr_of_mut!((*out).ops[2]), reg, 16);
            OCERZ_OK as c_int
        }
        0x08 => decode_pint_imm(s, OCERZ_OP_ROUNDPS as c_int),
        0x09 => decode_pint_imm(s, OCERZ_OP_ROUNDPD as c_int),
        0x0a => decode_sse_rri(s, OCERZ_OP_ROUNDSS as c_int, 4, true),
        0x0b => decode_sse_rri(s, OCERZ_OP_ROUNDSD as c_int, 8, true),
        0x0c => decode_pint_imm(s, OCERZ_OP_BLENDPS as c_int),
        0x0d => decode_pint_imm(s, OCERZ_OP_BLENDPD as c_int),
        0x0e => decode_pint_imm(s, OCERZ_OP_PBLENDW as c_int),
        0x0f => decode_pint_imm(s, OCERZ_OP_PALIGNR as c_int),
        0x40 => decode_pint_imm(s, OCERZ_OP_DPPS as c_int),
        0x41 => decode_pint_imm(s, OCERZ_OP_DPPD as c_int),
        0x42 => decode_pint_imm(s, OCERZ_OP_MPSADBW as c_int),
        0x60 => decode_pint_imm(s, OCERZ_OP_PCMPESTRM as c_int),
        0x61 => decode_pint_imm(s, OCERZ_OP_PCMPESTRI as c_int),
        0x62 => decode_pint_imm(s, OCERZ_OP_PCMPISTRM as c_int),
        0x63 => decode_pint_imm(s, OCERZ_OP_PCMPISTRI as c_int),
        0x14 | 0x15 => {
            let size = if op3 == 0x14 { 1 } else { 2 };
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if op3 == 0x14 {
                    OCERZ_OP_PEXTRB as c_int
                } else {
                    OCERZ_OP_PEXTRW as c_int
                },
            );
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 3;
            if rm_is_reg(&m) {
                set_reg(
                    ptr::addr_of_mut!((*out).ops[0]),
                    m.rm,
                    if s.rex_w != 0 { 8 } else { 4 },
                );
            } else {
                (*out).ops[0] = m.mem;
                (*out).ops[0].size = size as u8;
            }
            set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.reg, 16);
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0x16 => {
            let mut m: ModRM = mem::zeroed();
            let gsize = if s.rex_w != 0 { 8 } else { 4 };
            e = decode_modrm(s, &mut m, gsize);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if s.rex_w != 0 {
                    OCERZ_OP_PEXTRQ as c_int
                } else {
                    OCERZ_OP_PEXTRD as c_int
                },
            );
            let out = s.out;
            (*out).opsize = gsize as u8;
            (*out).nops = 3;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), gsize, true);
            set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.reg, 16);
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0x17 => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 4);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_EXTRACTPS as c_int);
            let out = s.out;
            (*out).opsize = 4;
            (*out).nops = 3;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), 4, true);
            set_xmm(ptr::addr_of_mut!((*out).ops[1]), m.reg, 16);
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0x20 => {
            let mut m: ModRM = mem::zeroed();
            e = decode_modrm(s, &mut m, 1);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_PINSRB as c_int);
            let out = s.out;
            (*out).opsize = 16;
            (*out).nops = 3;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
            if rm_is_reg(&m) {
                set_reg(ptr::addr_of_mut!((*out).ops[1]), m.rm, 4);
            } else {
                (*out).ops[1] = m.mem;
                (*out).ops[1].size = 1;
            }
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        0x21 => decode_sse_rri(s, OCERZ_OP_INSERTPS as c_int, 4, true),
        0x44 => decode_pint_imm(s, OCERZ_OP_PCLMULQDQ as c_int),
        0xdf => decode_pint_imm(s, OCERZ_OP_AESKEYGENASSIST as c_int),
        0x22 => {
            let mut m: ModRM = mem::zeroed();
            let gsize = if s.rex_w != 0 { 8 } else { 4 };
            e = decode_modrm(s, &mut m, gsize);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if s.rex_w != 0 {
                    OCERZ_OP_PINSRQ as c_int
                } else {
                    OCERZ_OP_PINSRD as c_int
                },
            );
            let out = s.out;
            (*out).opsize = 16;
            (*out).nops = 3;
            set_xmm(ptr::addr_of_mut!((*out).ops[0]), m.reg, 16);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), gsize, true);
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), 1)
        }
        _ => OCERZ_EUNDEF as c_int,
    }
}
