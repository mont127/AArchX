//! The x87 opcode map and register stack helpers.

use super::*;

static X87_REAL_OPS: [c_int; 8] = [
    OCERZ_OP_FADD as c_int,
    OCERZ_OP_FMUL as c_int,
    OCERZ_OP_FCOM as c_int,
    OCERZ_OP_FCOMP as c_int,
    OCERZ_OP_FSUB as c_int,
    OCERZ_OP_FSUBR as c_int,
    OCERZ_OP_FDIV as c_int,
    OCERZ_OP_FDIVR as c_int,
];
static X87_INT_OPS: [c_int; 8] = [
    OCERZ_OP_FIADD as c_int,
    OCERZ_OP_FIMUL as c_int,
    OCERZ_OP_FICOM as c_int,
    OCERZ_OP_FICOMP as c_int,
    OCERZ_OP_FISUB as c_int,
    OCERZ_OP_FISUBR as c_int,
    OCERZ_OP_FIDIV as c_int,
    OCERZ_OP_FIDIVR as c_int,
];
static X87_CC: [u8; 4] = [
    OCERZ_CC_B as u8,
    OCERZ_CC_E as u8,
    OCERZ_CC_BE as u8,
    OCERZ_CC_P as u8,
];
static X87_NCC: [u8; 4] = [
    OCERZ_CC_AE as u8,
    OCERZ_CC_NE as u8,
    OCERZ_CC_A as u8,
    OCERZ_CC_NP as u8,
];
static X87_DC_REG_OPS: [c_int; 8] = [
    OCERZ_OP_FADD as c_int,
    OCERZ_OP_FMUL as c_int,
    OCERZ_OP_FCOM as c_int,
    OCERZ_OP_FCOMP as c_int,
    OCERZ_OP_FSUBR as c_int,
    OCERZ_OP_FSUB as c_int,
    OCERZ_OP_FDIVR as c_int,
    OCERZ_OP_FDIV as c_int,
];
static X87_DE_REG_OPS: [c_int; 8] = [
    OCERZ_OP_FADDP as c_int,
    OCERZ_OP_FMULP as c_int,
    OCERZ_OP_FCOMP as c_int,
    OCERZ_OP_FCOMP as c_int,
    OCERZ_OP_FSUBRP as c_int,
    OCERZ_OP_FSUBP as c_int,
    OCERZ_OP_FDIVRP as c_int,
    OCERZ_OP_FDIVP as c_int,
];

unsafe fn x87_mem(s: &mut DecState, m: &ModRM, op: c_int, size: c_int) -> c_int {
    set_op(s, op);
    let out = s.out;
    (*out).opsize = size as u8;
    (*out).nops = 1;
    (*out).ops[0] = m.mem;
    (*out).ops[0].size = size as u8;
    OCERZ_OK as c_int
}

unsafe fn x87_env(s: &mut DecState, m: &ModRM, op: c_int, size16: c_int, size32: c_int) -> c_int {
    x87_mem(s, m, op, if s.has_66 != 0 { size16 } else { size32 })
}

unsafe fn x87_st1(s: &mut DecState, op: c_int, i: c_int) -> c_int {
    set_op(s, op);
    let out = s.out;
    (*out).opsize = 10;
    (*out).nops = 1;
    set_st(ptr::addr_of_mut!((*out).ops[0]), i);
    OCERZ_OK as c_int
}

unsafe fn x87_st2(s: &mut DecState, op: c_int, dst: c_int, src: c_int) -> c_int {
    set_op(s, op);
    let out = s.out;
    (*out).opsize = 10;
    (*out).nops = 2;
    set_st(ptr::addr_of_mut!((*out).ops[0]), dst);
    set_st(ptr::addr_of_mut!((*out).ops[1]), src);
    OCERZ_OK as c_int
}

unsafe fn x87_bare(s: &mut DecState, op: c_int) -> c_int {
    set_op(s, op);
    (*s.out).nops = 0;
    OCERZ_OK as c_int
}

unsafe fn x87_fcmov(s: &mut DecState, idx: c_int, i: c_int, negate: bool) -> c_int {
    x87_st2(s, OCERZ_OP_FCMOVCC as c_int, 0, i);
    (*s.out).cc = if negate {
        *X87_NCC.get_unchecked(idx as usize)
    } else {
        *X87_CC.get_unchecked(idx as usize)
    };
    OCERZ_OK as c_int
}

unsafe fn x87_d8(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return x87_mem(s, m, *X87_REAL_OPS.get_unchecked(idx as usize), 4);
    }
    let i = m.rm & 7;
    if idx == 2 || idx == 3 {
        x87_st1(s, *X87_REAL_OPS.get_unchecked(idx as usize), i)
    } else {
        x87_st2(s, *X87_REAL_OPS.get_unchecked(idx as usize), 0, i)
    }
}

unsafe fn x87_d9(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return match idx {
            0 => x87_mem(s, m, OCERZ_OP_FLD as c_int, 4),
            2 => x87_mem(s, m, OCERZ_OP_FST as c_int, 4),
            3 => x87_mem(s, m, OCERZ_OP_FSTP as c_int, 4),
            4 => x87_env(s, m, OCERZ_OP_FLDENV as c_int, 14, 28),
            5 => x87_mem(s, m, OCERZ_OP_FLDCW as c_int, 2),
            6 => x87_env(s, m, OCERZ_OP_FNSTENV as c_int, 14, 28),
            7 => x87_mem(s, m, OCERZ_OP_FNSTCW as c_int, 2),
            _ => OCERZ_EUNDEF as c_int,
        };
    }
    let i = m.rm & 7;
    let modrm = (0xc0 | ((m.reg & 7) << 3) | (m.rm & 7)) as u8;
    match idx {
        0 => return x87_st1(s, OCERZ_OP_FLD as c_int, i),
        1 => return x87_st1(s, OCERZ_OP_FXCH as c_int, i),
        3 => return x87_st1(s, OCERZ_OP_FSTP as c_int, i),
        _ => {}
    }
    match modrm {
        0xd0 => x87_bare(s, OCERZ_OP_NOP as c_int),
        0xe0 => x87_bare(s, OCERZ_OP_FCHS as c_int),
        0xe1 => x87_bare(s, OCERZ_OP_FABS as c_int),
        0xe4 => x87_bare(s, OCERZ_OP_FTST as c_int),
        0xe5 => x87_bare(s, OCERZ_OP_FXAM as c_int),
        0xe8 => x87_bare(s, OCERZ_OP_FLD1 as c_int),
        0xe9 => x87_bare(s, OCERZ_OP_FLDL2T as c_int),
        0xea => x87_bare(s, OCERZ_OP_FLDL2E as c_int),
        0xeb => x87_bare(s, OCERZ_OP_FLDPI as c_int),
        0xec => x87_bare(s, OCERZ_OP_FLDLG2 as c_int),
        0xed => x87_bare(s, OCERZ_OP_FLDLN2 as c_int),
        0xee => x87_bare(s, OCERZ_OP_FLDZ as c_int),
        0xf0 => x87_bare(s, OCERZ_OP_F2XM1 as c_int),
        0xf1 => x87_bare(s, OCERZ_OP_FYL2X as c_int),
        0xf2 => x87_bare(s, OCERZ_OP_FPTAN as c_int),
        0xf3 => x87_bare(s, OCERZ_OP_FPATAN as c_int),
        0xf4 => x87_bare(s, OCERZ_OP_FXTRACT as c_int),
        0xf5 => x87_bare(s, OCERZ_OP_FPREM1 as c_int),
        0xf6 => x87_bare(s, OCERZ_OP_FDECSTP as c_int),
        0xf7 => x87_bare(s, OCERZ_OP_FINCSTP as c_int),
        0xf8 => x87_bare(s, OCERZ_OP_FPREM as c_int),
        0xf9 => x87_bare(s, OCERZ_OP_FYL2XP1 as c_int),
        0xfa => x87_bare(s, OCERZ_OP_FSQRT as c_int),
        0xfb => x87_bare(s, OCERZ_OP_FSINCOS as c_int),
        0xfc => x87_bare(s, OCERZ_OP_FRNDINT as c_int),
        0xfd => x87_bare(s, OCERZ_OP_FSCALE as c_int),
        0xfe => x87_bare(s, OCERZ_OP_FSIN as c_int),
        0xff => x87_bare(s, OCERZ_OP_FCOS as c_int),
        _ => OCERZ_EUNDEF as c_int,
    }
}

unsafe fn x87_da(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return x87_mem(s, m, *X87_INT_OPS.get_unchecked(idx as usize), 4);
    }
    let i = m.rm & 7;
    let modrm = (0xc0 | ((m.reg & 7) << 3) | (m.rm & 7)) as u8;
    if modrm == 0xe9 {
        return x87_bare(s, OCERZ_OP_FUCOMPP as c_int);
    }
    if idx < 4 {
        return x87_fcmov(s, idx, i, false);
    }
    OCERZ_EUNDEF as c_int
}

unsafe fn x87_db(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return match idx {
            0 => x87_mem(s, m, OCERZ_OP_FILD as c_int, 4),
            1 => x87_mem(s, m, OCERZ_OP_FISTTP as c_int, 4),
            2 => x87_mem(s, m, OCERZ_OP_FIST as c_int, 4),
            3 => x87_mem(s, m, OCERZ_OP_FISTP as c_int, 4),
            5 => x87_mem(s, m, OCERZ_OP_FLD as c_int, 10),
            7 => x87_mem(s, m, OCERZ_OP_FSTP as c_int, 10),
            _ => OCERZ_EUNDEF as c_int,
        };
    }
    let i = m.rm & 7;
    let modrm = (0xc0 | ((m.reg & 7) << 3) | (m.rm & 7)) as u8;
    match modrm {
        0xe0 | 0xe1 | 0xe4 => return x87_bare(s, OCERZ_OP_NOP as c_int),
        0xe2 => return x87_bare(s, OCERZ_OP_FNCLEX as c_int),
        0xe3 => return x87_bare(s, OCERZ_OP_FNINIT as c_int),
        _ => {}
    }
    if idx < 4 {
        return x87_fcmov(s, idx, i, true);
    }
    if idx == 5 {
        return x87_st2(s, OCERZ_OP_FUCOMI as c_int, 0, i);
    }
    if idx == 6 {
        return x87_st2(s, OCERZ_OP_FCOMI as c_int, 0, i);
    }
    OCERZ_EUNDEF as c_int
}

unsafe fn x87_dc(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return x87_mem(s, m, *X87_REAL_OPS.get_unchecked(idx as usize), 8);
    }
    let i = m.rm & 7;
    if idx == 2 || idx == 3 {
        x87_st1(s, *X87_DC_REG_OPS.get_unchecked(idx as usize), i)
    } else {
        x87_st2(s, *X87_DC_REG_OPS.get_unchecked(idx as usize), i, 0)
    }
}

unsafe fn x87_dd(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return match idx {
            0 => x87_mem(s, m, OCERZ_OP_FLD as c_int, 8),
            1 => x87_mem(s, m, OCERZ_OP_FISTTP as c_int, 8),
            2 => x87_mem(s, m, OCERZ_OP_FST as c_int, 8),
            3 => x87_mem(s, m, OCERZ_OP_FSTP as c_int, 8),
            4 => x87_env(s, m, OCERZ_OP_FRSTOR as c_int, 94, 108),
            6 => x87_env(s, m, OCERZ_OP_FNSAVE as c_int, 94, 108),
            7 => x87_mem(s, m, OCERZ_OP_FNSTSW as c_int, 2),
            _ => OCERZ_EUNDEF as c_int,
        };
    }
    let i = m.rm & 7;
    match idx {
        0 => x87_st1(s, OCERZ_OP_FFREE as c_int, i),
        1 => x87_st1(s, OCERZ_OP_FXCH as c_int, i),
        2 => x87_st1(s, OCERZ_OP_FST as c_int, i),
        3 => x87_st1(s, OCERZ_OP_FSTP as c_int, i),
        4 => x87_st1(s, OCERZ_OP_FUCOM as c_int, i),
        5 => x87_st1(s, OCERZ_OP_FUCOMP as c_int, i),
        _ => OCERZ_EUNDEF as c_int,
    }
}

unsafe fn x87_de(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return x87_mem(s, m, *X87_INT_OPS.get_unchecked(idx as usize), 2);
    }
    let i = m.rm & 7;
    let modrm = (0xc0 | ((m.reg & 7) << 3) | (m.rm & 7)) as u8;
    if modrm == 0xd9 {
        return x87_bare(s, OCERZ_OP_FCOMPP as c_int);
    }
    if idx == 2 || idx == 3 {
        return x87_st1(s, OCERZ_OP_FCOMP as c_int, i);
    }
    x87_st2(s, *X87_DE_REG_OPS.get_unchecked(idx as usize), i, 0)
}

unsafe fn x87_df(s: &mut DecState, m: &ModRM) -> c_int {
    let idx = m.reg & 7;
    if !rm_is_reg(m) {
        return match idx {
            0 => x87_mem(s, m, OCERZ_OP_FILD as c_int, 2),
            1 => x87_mem(s, m, OCERZ_OP_FISTTP as c_int, 2),
            2 => x87_mem(s, m, OCERZ_OP_FIST as c_int, 2),
            3 => x87_mem(s, m, OCERZ_OP_FISTP as c_int, 2),
            4 => x87_mem(s, m, OCERZ_OP_FBLD as c_int, 10),
            5 => x87_mem(s, m, OCERZ_OP_FILD as c_int, 8),
            6 => x87_mem(s, m, OCERZ_OP_FBSTP as c_int, 10),
            7 => x87_mem(s, m, OCERZ_OP_FISTP as c_int, 8),
            _ => OCERZ_EUNDEF as c_int,
        };
    }
    let i = m.rm & 7;
    let modrm = (0xc0 | ((m.reg & 7) << 3) | (m.rm & 7)) as u8;
    if modrm == 0xe0 {
        set_op(s, OCERZ_OP_FNSTSW as c_int);
        let out = s.out;
        (*out).opsize = 2;
        (*out).nops = 1;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), OCERZ_RAX as c_int, 2);
        return OCERZ_OK as c_int;
    }
    match idx {
        0 => x87_st1(s, OCERZ_OP_FFREEP as c_int, i),
        1 => x87_st1(s, OCERZ_OP_FXCH as c_int, i),
        2 | 3 => x87_st1(s, OCERZ_OP_FSTP as c_int, i),
        5 => x87_st2(s, OCERZ_OP_FUCOMIP as c_int, 0, i),
        6 => x87_st2(s, OCERZ_OP_FCOMIP as c_int, 0, i),
        _ => OCERZ_EUNDEF as c_int,
    }
}

pub(super) unsafe fn decode_x87(s: &mut DecState, op: u8) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let e = decode_modrm(s, &mut m, 4);
    if e != 0 {
        return e;
    }
    match op {
        0xd8 => x87_d8(s, &m),
        0xd9 => x87_d9(s, &m),
        0xda => x87_da(s, &m),
        0xdb => x87_db(s, &m),
        0xdc => x87_dc(s, &m),
        0xdd => x87_dd(s, &m),
        0xde => x87_de(s, &m),
        _ => x87_df(s, &m),
    }
}
