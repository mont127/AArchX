//! One-byte opcode groups and the i386-only decode path.

use super::*;

unsafe fn group1(s: &mut DecState, op: u8) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let byte_form = op == 0x80 || op == 0x82;
    let size = if byte_form { 1 } else { opsize_default(s) };
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let out = s.out;
    set_op(s, alu_op_for(m.reg & 7));
    (*out).opsize = size as u8;
    (*out).nops = 2;
    if byte_form {
        place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
    } else {
        place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
    }
    if op == 0x80 || op == 0x82 {
        read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
    } else if op == 0x83 {
        read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), size)
    } else {
        read_imm_sized(s, ptr::addr_of_mut!((*out).ops[1]), size)
    }
}

unsafe fn group2(s: &mut DecState, op: u8) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let byte_form = op == 0xc0 || op == 0xd0 || op == 0xd2;
    let size = if byte_form { 1 } else { opsize_default(s) };
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let out = s.out;
    set_op(s, shift_op_for(m.reg & 7));
    (*out).opsize = size as u8;
    (*out).nops = 2;
    if byte_form {
        place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
    } else {
        place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
    }
    if op == 0xc0 || op == 0xc1 {
        read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
    } else if op == 0xd0 || op == 0xd1 {
        set_imm(ptr::addr_of_mut!((*out).ops[1]), 1, 1);
        OCERZ_OK as c_int
    } else {
        set_reg(ptr::addr_of_mut!((*out).ops[1]), OCERZ_RCX as c_int, 1);
        OCERZ_OK as c_int
    }
}

unsafe fn group3(s: &mut DecState, op: u8) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let byte_form = op == 0xf6;
    let size = if byte_form { 1 } else { opsize_default(s) };
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let idx = m.reg & 7;
    let out = s.out;
    (*out).opsize = size as u8;
    match idx {
        0 | 1 => {
            set_op(s, OCERZ_OP_TEST as c_int);
            (*out).nops = 2;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
                read_imm_sized(s, ptr::addr_of_mut!((*out).ops[1]), size)
            }
        }
        2 => {
            set_op(s, OCERZ_OP_NOT as c_int);
            (*out).nops = 1;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            }
            OCERZ_OK as c_int
        }
        3 => {
            set_op(s, OCERZ_OP_NEG as c_int);
            (*out).nops = 1;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            }
            OCERZ_OK as c_int
        }
        4..=7 => {
            set_op(
                s,
                match idx {
                    4 => OCERZ_OP_MUL as c_int,
                    5 => OCERZ_OP_IMUL as c_int,
                    6 => OCERZ_OP_DIV as c_int,
                    _ => OCERZ_OP_IDIV as c_int,
                },
            );
            (*out).nops = 1;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            }
            OCERZ_OK as c_int
        }
        _ => unreachable!(),
    }
}

unsafe fn group45(s: &mut DecState, op: u8) -> c_int {
    let mut m: ModRM = mem::zeroed();
    if op == 0xfe {
        let e = decode_modrm(s, &mut m, 1);
        if e != 0 {
            return e;
        }
        let idx = m.reg & 7;
        if idx == 0 {
            set_op(s, OCERZ_OP_INC as c_int);
        } else if idx == 1 {
            set_op(s, OCERZ_OP_DEC as c_int);
        } else {
            return OCERZ_EUNDEF as c_int;
        }
        let out = s.out;
        (*out).opsize = 1;
        (*out).nops = 1;
        place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
        return OCERZ_OK as c_int;
    }
    let size = opsize_default(s);
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let idx = m.reg & 7;
    let out = s.out;
    match idx {
        0 | 1 => {
            set_op(
                s,
                if idx == 0 {
                    OCERZ_OP_INC as c_int
                } else {
                    OCERZ_OP_DEC as c_int
                },
            );
            (*out).opsize = size as u8;
            (*out).nops = 1;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            OCERZ_OK as c_int
        }
        2 | 4 => {
            let bsize = opsize_nearbranch(s);
            set_op(
                s,
                if idx == 2 {
                    OCERZ_OP_CALL as c_int
                } else {
                    OCERZ_OP_JMP as c_int
                },
            );
            (*out).opsize = bsize as u8;
            (*out).nops = 1;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), bsize, true);
            OCERZ_OK as c_int
        }
        6 => {
            let psize = opsize_stack(s);
            set_op(s, OCERZ_OP_PUSH as c_int);
            (*out).opsize = psize as u8;
            (*out).nops = 1;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), psize, true);
            OCERZ_OK as c_int
        }
        3 | 5 => {
            if rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(
                s,
                if idx == 3 {
                    OCERZ_OP_CALLF as c_int
                } else {
                    OCERZ_OP_JMPF as c_int
                },
            );
            (*out).opsize = size as u8;
            (*out).nops = 1;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            OCERZ_OK as c_int
        }
        _ => OCERZ_EUNDEF as c_int,
    }
}

unsafe fn decode_i386_only(s: &mut DecState, op: u8, handled: &mut bool) -> c_int {
    *handled = true;
    if (0x40..=0x4f).contains(&op) {
        let size = opsize_default(s);
        set_op(
            s,
            if op < 0x48 {
                OCERZ_OP_INC as c_int
            } else {
                OCERZ_OP_DEC as c_int
            },
        );
        let out = s.out;
        (*out).opsize = size as u8;
        (*out).nops = 1;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), (op & 7) as c_int, size);
        return OCERZ_OK as c_int;
    }
    match op {
        0x06 | 0x0e | 0x16 | 0x1e | 0x07 | 0x17 | 0x1f => {
            const SREG_OF: [u32; 4] = [OCERZ_SREG_ES, OCERZ_SREG_CS, OCERZ_SREG_SS, OCERZ_SREG_DS];
            let is_pop = op & 1 != 0;
            set_op(
                s,
                if is_pop {
                    OCERZ_OP_POPSEG as c_int
                } else {
                    OCERZ_OP_PUSHSEG as c_int
                },
            );
            let out = s.out;
            (*out).opsize = opsize_stack(s) as u8;
            (*out).nops = 1;
            set_imm(
                ptr::addr_of_mut!((*out).ops[0]),
                *SREG_OF.get_unchecked(((op >> 3) & 3) as usize) as u64,
                1,
            );
            OCERZ_OK as c_int
        }
        0x27 | 0x2f | 0x37 | 0x3f => {
            let (insn_op, size) = match op {
                0x27 => (OCERZ_OP_DAA as c_int, 1),
                0x2f => (OCERZ_OP_DAS as c_int, 1),
                0x37 => (OCERZ_OP_AAA as c_int, 2),
                _ => (OCERZ_OP_AAS as c_int, 2),
            };
            set_op(s, insn_op);
            let out = s.out;
            (*out).opsize = size;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x60 | 0x61 => {
            set_op(
                s,
                if op == 0x60 {
                    OCERZ_OP_PUSHA as c_int
                } else {
                    OCERZ_OP_POPA as c_int
                },
            );
            let out = s.out;
            (*out).opsize = opsize_stack(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x62 => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            let e = decode_modrm(s, &mut m, size * 2);
            if e != 0 {
                return e;
            }
            if rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_BOUND as c_int);
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size * 2, true);
            OCERZ_OK as c_int
        }
        0x82 => group1(s, op),
        0x9a | 0xea => {
            let size = opsize_default(s);
            let mut off: X86Operand = mem::zeroed();
            let mut sel: X86Operand = mem::zeroed();
            let e = read_imm_sized(s, &mut off, size);
            if e != 0 {
                return e;
            }
            let e = read_imm16(s, &mut sel);
            if e != 0 {
                return e;
            }
            set_op(
                s,
                if op == 0x9a {
                    OCERZ_OP_CALLF as c_int
                } else {
                    OCERZ_OP_JMPF as c_int
                },
            );
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            (*out).ops[0] = sel;
            (*out).ops[1] = off;
            OCERZ_OK as c_int
        }
        0xc4 | 0xc5 => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            let e = decode_modrm(s, &mut m, size + 2);
            if e != 0 {
                return e;
            }
            if rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(
                s,
                if op == 0xc4 {
                    OCERZ_OP_LES as c_int
                } else {
                    OCERZ_OP_LDS as c_int
                },
            );
            let out = s.out;
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size + 2, true);
            OCERZ_OK as c_int
        }
        0xce => {
            set_op(s, OCERZ_OP_INTO as c_int);
            (*s.out).nops = 0;
            OCERZ_OK as c_int
        }
        0xd4 | 0xd5 => {
            set_op(
                s,
                if op == 0xd4 {
                    OCERZ_OP_AAM as c_int
                } else {
                    OCERZ_OP_AAD as c_int
                },
            );
            let out = s.out;
            (*out).opsize = 2;
            (*out).nops = 1;
            let mut b = 0u8;
            let e = fetch8(s, &mut b);
            if e != 0 {
                return e;
            }
            set_imm(ptr::addr_of_mut!((*out).ops[0]), b as u64, 1);
            OCERZ_OK as c_int
        }
        0xd6 => {
            set_op(s, OCERZ_OP_SALC as c_int);
            let out = s.out;
            (*out).opsize = 1;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        _ => {
            *handled = false;
            OCERZ_EUNDEF as c_int
        }
    }
}

pub(super) unsafe fn decode_one_byte(s: &mut DecState, op: u8) -> c_int {
    if s.mode32 != 0 {
        let mut handled = false;
        let e = decode_i386_only(s, op, &mut handled);
        if handled {
            return e;
        }
    }
    if op <= 0x3d {
        let hi = (op >> 3) as c_int;
        let lo = op & 7;
        if hi <= 7 && lo <= 5 {
            return match lo {
                0 => alu_rm_r(s, hi, true, false),
                1 => alu_rm_r(s, hi, false, false),
                2 => alu_rm_r(s, hi, true, true),
                3 => alu_rm_r(s, hi, false, true),
                4 => alu_acc_imm(s, hi, true),
                _ => alu_acc_imm(s, hi, false),
            };
        }
        return OCERZ_EUNDEF as c_int;
    }
    if (0x50..=0x57).contains(&op) {
        let reg = ((op - 0x50) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
        let size = opsize_stack(s);
        set_op(s, OCERZ_OP_PUSH as c_int);
        let out = s.out;
        (*out).opsize = size as u8;
        (*out).nops = 1;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), reg, size);
        return OCERZ_OK as c_int;
    }
    if (0x58..=0x5f).contains(&op) {
        let reg = ((op - 0x58) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
        let size = opsize_stack(s);
        set_op(s, OCERZ_OP_POP as c_int);
        let out = s.out;
        (*out).opsize = size as u8;
        (*out).nops = 1;
        set_reg(ptr::addr_of_mut!((*out).ops[0]), reg, size);
        return OCERZ_OK as c_int;
    }
    let out = s.out;
    match op {
        0x63 => {
            let mut m: ModRM = mem::zeroed();
            let dsize = if s.rex_w != 0 { 8 } else { 4 };
            let e = decode_modrm(s, &mut m, 4);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_MOVSXD as c_int);
            (*out).opsize = dsize as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, dsize);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), 4, true);
            OCERZ_OK as c_int
        }
        0x68 | 0x6a => {
            let size = opsize_stack(s);
            set_op(s, OCERZ_OP_PUSH as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 1;
            if op == 0x68 {
                read_imm_sized(s, ptr::addr_of_mut!((*out).ops[0]), size)
            } else {
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[0]), size)
            }
        }
        0x69 | 0x6b => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_IMUL as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 3;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
            if op == 0x6b {
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[2]), size)
            } else {
                read_imm_sized(s, ptr::addr_of_mut!((*out).ops[2]), size)
            }
        }
        0x70..=0x7f => branch_rel(s, OCERZ_OP_JCC as c_int, 1, (op & 0xf) as c_int),
        0x80 | 0x81 | 0x83 => group1(s, op),
        0x84 | 0x85 => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op == 0x84;
            let size = if byte_form { 1 } else { opsize_default(s) };
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_TEST as c_int);
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
        0x86 | 0x87 => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op == 0x86;
            let size = if byte_form { 1 } else { opsize_default(s) };
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_XCHG as c_int);
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
        0x88..=0x8b => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op == 0x88 || op == 0x8a;
            let reg_is_dst = op == 0x8a || op == 0x8b;
            let size = if byte_form { 1 } else { opsize_default(s) };
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_MOV as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            if reg_is_dst {
                if byte_form {
                    set_reg8(s, ptr::addr_of_mut!((*out).ops[0]), m.reg);
                    place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[1]));
                } else {
                    set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
                    place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
                }
            } else if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
                set_reg8(s, ptr::addr_of_mut!((*out).ops[1]), m.reg);
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
                set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
            }
            OCERZ_OK as c_int
        }
        0x8c => {
            let mut m: ModRM = mem::zeroed();
            let e = decode_modrm(s, &mut m, 2);
            if e != 0 {
                return e;
            }
            if (m.reg & 7) > 5 {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_MOVFROMSEG as c_int);
            (*out).nops = 2;
            if rm_is_reg(&m) {
                let size = opsize_default(s);
                (*out).opsize = size as u8;
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            } else {
                (*out).opsize = 2;
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), 2, true);
            }
            set_imm(ptr::addr_of_mut!((*out).ops[1]), (m.reg & 7) as u64, 1);
            OCERZ_OK as c_int
        }
        0x8e => {
            let mut m: ModRM = mem::zeroed();
            let e = decode_modrm(s, &mut m, 2);
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_MOVSEG as c_int);
            (*out).opsize = 2;
            (*out).nops = 2;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), 2, true);
            set_imm(ptr::addr_of_mut!((*out).ops[1]), (m.reg & 7) as u64, 1);
            OCERZ_OK as c_int
        }
        0x8d => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_default(s);
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            if rm_is_reg(&m) {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_LEA as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
            (*out).ops[1] = m.mem;
            (*out).ops[1].size = size as u8;
            OCERZ_OK as c_int
        }
        0x8f => {
            let mut m: ModRM = mem::zeroed();
            let size = opsize_stack(s);
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            if (m.reg & 7) != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_POP as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 1;
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
            OCERZ_OK as c_int
        }
        0x90 => {
            if s.mand == MAND_F3 {
                set_op(s, OCERZ_OP_PAUSE as c_int);
                (*out).nops = 0;
                return OCERZ_OK as c_int;
            }
            if s.rex_b != 0 {
                let size = opsize_default(s);
                set_op(s, OCERZ_OP_XCHG as c_int);
                (*out).opsize = size as u8;
                (*out).nops = 2;
                set_reg(ptr::addr_of_mut!((*out).ops[0]), OCERZ_RAX as c_int, size);
                set_reg(ptr::addr_of_mut!((*out).ops[1]), 8, size);
                return OCERZ_OK as c_int;
            }
            set_op(s, OCERZ_OP_NOP as c_int);
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x91..=0x97 => {
            let size = opsize_default(s);
            let reg = ((op - 0x90) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
            set_op(s, OCERZ_OP_XCHG as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), OCERZ_RAX as c_int, size);
            set_reg(ptr::addr_of_mut!((*out).ops[1]), reg, size);
            OCERZ_OK as c_int
        }
        0x98 | 0x99 => {
            let size = opsize_default(s);
            set_op(
                s,
                if op == 0x98 {
                    OCERZ_OP_CBW as c_int
                } else {
                    OCERZ_OP_CWD as c_int
                },
            );
            (*out).opsize = size as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x9b => {
            set_op(s, OCERZ_OP_FWAIT as c_int);
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x9c | 0x9d => {
            set_op(
                s,
                if op == 0x9c {
                    OCERZ_OP_PUSHF as c_int
                } else {
                    OCERZ_OP_POPF as c_int
                },
            );
            (*out).opsize = opsize_stack_implicit(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0x9e | 0x9f => {
            set_op(
                s,
                if op == 0x9e {
                    OCERZ_OP_SAHF as c_int
                } else {
                    OCERZ_OP_LAHF as c_int
                },
            );
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xa0..=0xa3 => {
            let byte_form = op == 0xa0 || op == 0xa2;
            let size = if byte_form { 1 } else { opsize_default(s) };
            let store = op == 0xa2 || op == 0xa3;
            let mut off = 0u64;
            let e = match (*out).addrsize {
                2 => {
                    let mut o16 = 0u16;
                    let e = fetch16(s, &mut o16);
                    off = o16 as u64;
                    e
                }
                4 => {
                    let mut o32 = 0u32;
                    let e = fetch32(s, &mut o32);
                    off = o32 as u64;
                    e
                }
                _ => fetch64(s, &mut off),
            };
            if e != 0 {
                return e;
            }
            set_op(s, OCERZ_OP_MOV as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            let mut mem_op: X86Operand = mem::zeroed();
            mem_op.kind = OCERZ_OPK_MEM as u8;
            mem_op.reg = 0;
            mem_op.size = size as u8;
            mem_op.high8 = 0;
            mem_op.base = OCERZ_REG_NONE as u8;
            mem_op.index = OCERZ_REG_NONE as u8;
            mem_op.scale = 0;
            mem_op.riprel = 0;
            mem_op.disp = off as i64;
            mem_op.imm = 0;
            let mut acc: X86Operand = mem::zeroed();
            if byte_form {
                set_reg8(s, &mut acc, 0);
            } else {
                set_reg(&mut acc, 0, size);
            }
            if store {
                (*out).ops[0] = mem_op;
                (*out).ops[1] = acc;
            } else {
                (*out).ops[0] = acc;
                (*out).ops[1] = mem_op;
            }
            OCERZ_OK as c_int
        }
        0xa4 | 0xa5 | 0xa6 | 0xa7 | 0xaa | 0xab | 0xac | 0xad | 0xae | 0xaf => {
            let size = if op & 1 == 0 { 1 } else { opsize_default(s) };
            let insn_op = match op {
                0xa4 | 0xa5 => OCERZ_OP_MOVS,
                0xa6 | 0xa7 => OCERZ_OP_CMPS,
                0xaa | 0xab => OCERZ_OP_STOS,
                0xac | 0xad => OCERZ_OP_LODS,
                _ => OCERZ_OP_SCAS,
            };
            set_op(s, insn_op as c_int);
            (*out).opsize = size as u8;
            (*out).rep = s.rep as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xa8 | 0xa9 => {
            let byte_form = op == 0xa8;
            let size = if byte_form { 1 } else { opsize_default(s) };
            set_op(s, OCERZ_OP_TEST as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            if byte_form {
                set_reg8(s, ptr::addr_of_mut!((*out).ops[0]), 0);
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
            } else {
                set_reg(ptr::addr_of_mut!((*out).ops[0]), 0, size);
                read_imm_sized(s, ptr::addr_of_mut!((*out).ops[1]), size)
            }
        }
        0xb0..=0xb7 => {
            let reg = ((op - 0xb0) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
            set_op(s, OCERZ_OP_MOV as c_int);
            (*out).opsize = 1;
            (*out).nops = 2;
            if s.rex_present != 0 || reg >= 8 {
                set_reg(ptr::addr_of_mut!((*out).ops[0]), reg, 1);
            } else {
                set_reg8(s, ptr::addr_of_mut!((*out).ops[0]), (op - 0xb0) as c_int);
            }
            read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
        }
        0xb8..=0xbf => {
            let reg = ((op - 0xb8) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
            let size = opsize_default(s);
            set_op(s, OCERZ_OP_MOV as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            set_reg(ptr::addr_of_mut!((*out).ops[0]), reg, size);
            if size == 8 {
                read_imm64(s, ptr::addr_of_mut!((*out).ops[1]))
            } else if size == 2 {
                read_imm16(s, ptr::addr_of_mut!((*out).ops[1]))
            } else {
                let mut d = 0u32;
                let e = fetch32(s, &mut d);
                if e != 0 {
                    return e;
                }
                set_imm(ptr::addr_of_mut!((*out).ops[1]), d as u64, 4);
                OCERZ_OK as c_int
            }
        }
        0xc0 | 0xc1 | 0xd0..=0xd3 => group2(s, op),
        0xc2 => {
            set_op(s, OCERZ_OP_RET as c_int);
            (*out).opsize = opsize_stack_implicit(s) as u8;
            (*out).nops = 1;
            let mut w = 0u16;
            let e = fetch16(s, &mut w);
            if e != 0 {
                return e;
            }
            set_imm(ptr::addr_of_mut!((*out).ops[0]), w as u64, 2);
            OCERZ_OK as c_int
        }
        0xc3 => {
            set_op(s, OCERZ_OP_RET as c_int);
            (*out).opsize = opsize_stack_implicit(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xc6 | 0xc7 => {
            let mut m: ModRM = mem::zeroed();
            let byte_form = op == 0xc6;
            let size = if byte_form { 1 } else { opsize_default(s) };
            let e = decode_modrm(s, &mut m, size);
            if e != 0 {
                return e;
            }
            if m.reg & 7 != 0 {
                return OCERZ_EUNDEF as c_int;
            }
            set_op(s, OCERZ_OP_MOV as c_int);
            (*out).opsize = size as u8;
            (*out).nops = 2;
            if byte_form {
                place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
                read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
            } else {
                place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
                read_imm_sized(s, ptr::addr_of_mut!((*out).ops[1]), size)
            }
        }
        0xc9 => {
            set_op(s, OCERZ_OP_LEAVE as c_int);
            (*out).opsize = opsize_stack_implicit(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xca => {
            set_op(s, OCERZ_OP_RETF as c_int);
            (*out).opsize = opsize_default(s) as u8;
            (*out).nops = 1;
            read_imm16(s, ptr::addr_of_mut!((*out).ops[0]))
        }
        0xcb => {
            set_op(s, OCERZ_OP_RETF as c_int);
            (*out).opsize = opsize_default(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xcc => {
            set_op(s, OCERZ_OP_INT3 as c_int);
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xcd => {
            set_op(s, OCERZ_OP_INT as c_int);
            (*out).nops = 1;
            let mut b = 0u8;
            let e = fetch8(s, &mut b);
            if e != 0 {
                return e;
            }
            set_imm(ptr::addr_of_mut!((*out).ops[0]), b as u64, 1);
            OCERZ_OK as c_int
        }
        0xcf => {
            set_op(s, OCERZ_OP_IRET as c_int);
            (*out).opsize = opsize_default(s) as u8;
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xe0 => branch_rel(s, OCERZ_OP_LOOPNE as c_int, 1, -1),
        0xe1 => branch_rel(s, OCERZ_OP_LOOPE as c_int, 1, -1),
        0xe2 => branch_rel(s, OCERZ_OP_LOOP as c_int, 1, -1),
        0xe3 => branch_rel(s, OCERZ_OP_JRCXZ as c_int, 1, -1),
        0xe8 => branch_rel(s, OCERZ_OP_CALL as c_int, 4, -1),
        0xe9 => branch_rel(s, OCERZ_OP_JMP as c_int, 4, -1),
        0xeb => branch_rel(s, OCERZ_OP_JMP as c_int, 1, -1),
        0xf4 | 0xf5 | 0xf8 | 0xf9 | 0xfc | 0xfd => {
            let insn_op = match op {
                0xf4 => OCERZ_OP_HLT,
                0xf5 => OCERZ_OP_CMC,
                0xf8 => OCERZ_OP_CLC,
                0xf9 => OCERZ_OP_STC,
                0xfc => OCERZ_OP_CLD,
                _ => OCERZ_OP_STD,
            };
            set_op(s, insn_op as c_int);
            (*out).nops = 0;
            OCERZ_OK as c_int
        }
        0xf6 | 0xf7 => group3(s, op),
        0xfe | 0xff => group45(s, op),
        _ => OCERZ_EUNDEF as c_int,
    }
}
