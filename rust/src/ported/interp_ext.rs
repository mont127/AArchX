//! Interpreter ops that are neither core integer nor SSE: string ops, CPUID,
//! CRC-32C and the rest of the long tail.
//!
//! String ops count in rCX and step rSI/rDI at the ADDRESS size - RCX/RSI/RDI at
//! 8, ECX/ESI/EDI at 4, CX/SI/DI at 2.  The 2 case is 32-bit mode with a 0x67
//! prefix and cannot arise in long mode, where addrsize is only ever 8 or 4.  A
//! 16-bit step writes back only the low half of the register, which is why it
//! goes through the register accessor rather than a direct store.
//!
//! rep movs and rep stos run forward at the host's own memmove and fill when
//! the range is one piece of host memory: wholly on one side of the low window's
//! edge and clear of the top strip and the commpage.  Wine's msvcrt memcpy is rep
//! movsb above a few hundred bytes, and an element at a time made it twenty times
//! Rosetta's time.  A movs whose destination starts inside its source copies a
//! pattern forward, which only the loop reproduces, and a backward one, a segment
//! override or a short count keeps the loop too.  Pages holding translated code
//! are disarmed and their translations dropped first, as a syscall does before
//! the kernel writes.  rsi, rdi and rcx move only once the whole range is done,
//! so a fault inside it - a guard page Wine commits on demand - reaches the guest
//! with the instruction not yet started, and running it again does the same work.
//!
//! CRC-32C is done bitwise: correctness over speed, since the JIT does not
//! translate it and hashing loops run interpreted anyway.  CPUID reports SSE3,
//! SSSE3, CX16, SSE4.1, SSE4.2 and POPCNT, all of which are implemented in full
//! - and Steam's bootstrapper refuses to start the client on a CPU without
//! SSE4.2.

use core::sync::atomic::{AtomicU8, Ordering};

use crate::ffi::*;
use crate::inline::{
    OCERZ_AF, OCERZ_CF, OCERZ_DF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF, ocerz_flag_assign, ocerz_mask,
    ocerz_msb, ocerz_sext, ocerz_trunc,
};
use crate::interp_common::*;
use crate::ported::flags::ocerz_flags_sub;

const STEP_OK: i32 = OCERZ_STEP_OK as i32;
const EUNSUP: i32 = OCERZ_EUNSUP as i32;
const RAX: usize = OCERZ_RAX as usize;
const RBX: usize = OCERZ_RBX as usize;
const RCX: usize = OCERZ_RCX as usize;
const RDX: usize = OCERZ_RDX as usize;
const RSI: u32 = OCERZ_RSI as u32;
const RDI: u32 = OCERZ_RDI as u32;

unsafe extern "C" {
    fn memset_pattern4(b: *mut core::ffi::c_void, pattern4: *const core::ffi::c_void, len: usize);
    fn memset_pattern8(b: *mut core::ffi::c_void, pattern8: *const core::ffi::c_void, len: usize);
}

#[inline(always)]
fn ext_rcx_read(cpu: &OcerzCPU, insn: &X86Insn) -> u64 {
    if insn.addrsize == 4 {
        return cpu.gpr[RCX] as u32 as u64;
    }
    if insn.addrsize == 2 {
        return cpu.gpr[RCX] as u16 as u64;
    }
    cpu.gpr[RCX]
}

#[inline(always)]
unsafe fn ext_rcx_write(cpu: &mut OcerzCPU, insn: &X86Insn, v: u64) {
    if insn.addrsize == 4 {
        cpu.gpr[RCX] = v as u32 as u64;
    } else if insn.addrsize == 2 {
        unsafe { ocerz_write_gpr(cpu, RCX as u32, 2, 0, v) };
    } else {
        cpu.gpr[RCX] = v;
    }
}

#[inline(always)]
fn ext_ptr_read(cpu: &OcerzCPU, insn: &X86Insn, reg: u32) -> u64 {
    let v = unsafe { *cpu.gpr.get_unchecked(reg as usize) };
    if insn.addrsize == 4 {
        return v as u32 as u64;
    }
    if insn.addrsize == 2 {
        return v as u16 as u64;
    }
    v
}

#[inline(always)]
unsafe fn ext_ptr_write(cpu: &mut OcerzCPU, insn: &X86Insn, reg: u32, v: u64) {
    unsafe {
        if insn.addrsize == 4 {
            *cpu.gpr.get_unchecked_mut(reg as usize) = v as u32 as u64;
        } else if insn.addrsize == 2 {
            ocerz_write_gpr(cpu, reg, 2, 0, v);
        } else {
            *cpu.gpr.get_unchecked_mut(reg as usize) = v;
        }
    }
}

unsafe fn ext_host_span(g: u64, len: u64) -> *mut u8 {
    unsafe {
        let end = g.wrapping_add(len);
        if len == 0 || end < g || !ocerz_pin_map.is_null() {
            return core::ptr::null_mut();
        }
        if ocerz_low_base != 0
            && !(end <= OCERZ_LOW_LIMIT || (g >= OCERZ_LOW_LIMIT && end <= OCERZ_TOP_LO))
        {
            return core::ptr::null_mut();
        }
        if !ocerz_commpage.is_null() && g < OCERZ_COMMPAGE_HI && end > OCERZ_COMMPAGE_LO {
            return core::ptr::null_mut();
        }
        let h0 = ocerz_g2h(g);
        let h1 = ocerz_g2h(end - 1);
        if (h1 as isize).wrapping_sub(h0 as isize) == (len - 1) as isize { h0 } else { core::ptr::null_mut() }
    }
}

unsafe fn ext_disarm(cpu: &mut OcerzCPU, g: u64, len: u64) {
    unsafe {
        if ocerz_mem_armed_any() == 0 {
            return;
        }
        let mut pages = [0u64; 64];
        loop {
            let n = ocerz_mem_disarm_range(g, g.wrapping_add(len), pages.as_mut_ptr(), 64);
            if n <= 0 {
                break;
            }
            for &p in pages.iter().take(n as usize) {
                ocerz_jit_invalidate_range(cpu.vm, p, OCERZ_HOST_PAGE_SIZE as u64);
            }
            if n < 64 {
                break;
            }
        }
    }
}

unsafe fn ext_string_bulk(cpu: &mut OcerzCPU, insn: &X86Insn) -> bool {
    unsafe {
        let op = insn.op as u32;
        let size = insn.opsize as u64;
        if insn.rep as u32 != OCERZ_REP_REP
            || (cpu.rflags & OCERZ_DF) != 0
            || insn.seg as u32 != OCERZ_SEG_NONE
            || (op != OCERZ_OP_MOVS && op != OCERZ_OP_STOS)
        {
            return false;
        }
        let n = ext_rcx_read(cpu, insn);
        if n < 32 || n > (1u64 << 36) {
            return false;
        }
        let len = n * size;
        let lim = if insn.addrsize == 8 { 0 } else { 1u64 << (insn.addrsize as u32 * 8) };
        let d = ext_ptr_read(cpu, insn, RDI);
        let mut s = 0;
        if lim != 0 && d + len > lim {
            return false;
        }
        let hd = ext_host_span(d, len);
        let mut hs = core::ptr::null_mut();
        if hd.is_null() {
            return false;
        }
        if op == OCERZ_OP_MOVS {
            s = ext_ptr_read(cpu, insn, RSI);
            if (lim != 0 && s + len > lim) || (d > s && d < s.wrapping_add(len)) {
                return false;
            }
            hs = ext_host_span(s, len);
            if hs.is_null() {
                return false;
            }
        }
        ext_disarm(cpu, d, len);
        if op == OCERZ_OP_MOVS {
            core::ptr::copy(hs as *const u8, hd, len as usize);
            ext_ptr_write(cpu, insn, RSI, s.wrapping_add(len));
        } else {
            let v = cpu.gpr[RAX];
            if size == 1 {
                core::ptr::write_bytes(hd, v as u8, len as usize);
            } else if size == 2 {
                let p: u32 = ((v & 0xffff) as u32).wrapping_mul(0x10001);
                memset_pattern4(hd as *mut _, &p as *const u32 as *const _, len as usize);
            } else if size == 4 {
                let p: u32 = v as u32;
                memset_pattern4(hd as *mut _, &p as *const u32 as *const _, len as usize);
            } else {
                memset_pattern8(hd as *mut _, &v as *const u64 as *const _, len as usize);
            }
        }
        ext_ptr_write(cpu, insn, RDI, d.wrapping_add(len));
        ext_rcx_write(cpu, insn, 0);
        true
    }
}

unsafe fn ext_string(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let size = insn.opsize as i32;
        let step: u64 = if (cpu.rflags & OCERZ_DF) != 0 { (-(size as i64)) as u64 } else { size as u64 };
        let rep = insn.rep as u32;
        let op = insn.op as u32;

        if rep != OCERZ_REP_NONE && ext_rcx_read(cpu, insn) == 0 {
            return STEP_OK;
        }
        if rep != OCERZ_REP_NONE && ext_string_bulk(cpu, insn) {
            return STEP_OK;
        }

        loop {
            let mut a: u64 = 0;
            let mut b: u64 = 0;
            let mut did_cmp = false;
            match op {
                OCERZ_OP_MOVS => {
                    let s = ext_ptr_read(cpu, insn, RSI);
                    let d = ext_ptr_read(cpu, insn, RDI);
                    ocerz_st(d, size, ocerz_ld(s, size));
                    ext_ptr_write(cpu, insn, RSI, s.wrapping_add(step));
                    ext_ptr_write(cpu, insn, RDI, d.wrapping_add(step));
                }
                OCERZ_OP_STOS => {
                    let d = ext_ptr_read(cpu, insn, RDI);
                    ocerz_st(d, size, ocerz_trunc(cpu.gpr[RAX], size));
                    ext_ptr_write(cpu, insn, RDI, d.wrapping_add(step));
                }
                OCERZ_OP_LODS => {
                    let s = ext_ptr_read(cpu, insn, RSI);
                    let v = ocerz_ld(s, size);
                    ocerz_write_gpr(cpu, RAX as u32, size, 0, v);
                    ext_ptr_write(cpu, insn, RSI, s.wrapping_add(step));
                }
                OCERZ_OP_SCAS => {
                    let d = ext_ptr_read(cpu, insn, RDI);
                    a = ocerz_trunc(cpu.gpr[RAX], size);
                    b = ocerz_ld(d, size);
                    ext_ptr_write(cpu, insn, RDI, d.wrapping_add(step));
                    did_cmp = true;
                }
                OCERZ_OP_CMPS => {
                    let s = ext_ptr_read(cpu, insn, RSI);
                    let d = ext_ptr_read(cpu, insn, RDI);
                    a = ocerz_ld(s, size);
                    b = ocerz_ld(d, size);
                    ext_ptr_write(cpu, insn, RSI, s.wrapping_add(step));
                    ext_ptr_write(cpu, insn, RDI, d.wrapping_add(step));
                    did_cmp = true;
                }
                _ => return EUNSUP,
            }

            if rep == OCERZ_REP_NONE {
                if did_cmp {
                    ocerz_flags_sub(cpu, size, a, b, 0, a.wrapping_sub(b));
                }
                return STEP_OK;
            }

            let cnt = ext_rcx_read(cpu, insn).wrapping_sub(1);
            ext_rcx_write(cpu, insn, cnt);

            if did_cmp {
                ocerz_flags_sub(cpu, size, a, b, 0, a.wrapping_sub(b));
                let zf = (cpu.rflags & OCERZ_ZF) != 0;
                if rep == OCERZ_REP_REP && !zf {
                    return STEP_OK;
                }
                if rep == OCERZ_REP_REPNE && zf {
                    return STEP_OK;
                }
            }

            if cnt == 0 {
                return STEP_OK;
            }
        }
    }
}

unsafe fn ext_bit(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let dst = &insn.ops[0];
        let off = &insn.ops[1];
        let size = dst.size as i32;
        let op = insn.op as u32;
        let testbit: i32;

        if dst.kind as u32 == OCERZ_OPK_REG {
            let bit = (ocerz_read_op(cpu, insn, off) & (size as u64 * 8 - 1)) as u32;
            let val = ocerz_read_gpr(cpu, dst.reg as u32, size, dst.high8 as i32);
            let testbit = ((val >> bit) & 1) as i32;
            ocerz_flag_assign(cpu, OCERZ_CF, testbit);
            if op != OCERZ_OP_BT {
                let mut nv = val;
                if op == OCERZ_OP_BTS {
                    nv |= 1u64 << bit;
                } else if op == OCERZ_OP_BTR {
                    nv &= !(1u64 << bit);
                } else {
                    nv ^= 1u64 << bit;
                }
                ocerz_write_gpr(cpu, dst.reg as u32, size, dst.high8 as i32, nv);
            }
            return STEP_OK;
        }

        let mut ea = ocerz_ea(cpu, insn, dst);
        let bit: u64;
        if off.kind as u32 == OCERZ_OPK_IMM {
            let b = ocerz_read_op(cpu, insn, off) & (size as u64 * 8 - 1);
            ea = ea.wrapping_add(b >> 3);
            bit = b & 7;
        } else {
            let sbit = ocerz_sext(ocerz_read_op(cpu, insn, off), off.size as i32);
            ea = ea.wrapping_add((sbit >> 3) as u64);
            bit = (sbit as u64) & 7;
        }

        if op != OCERZ_OP_BT && insn.lock != 0 {
            let hp = AtomicU8::from_ptr(ocerz_g2h(ea));
            let mut cur = hp.load(Ordering::SeqCst);
            loop {
                let mut nv = cur;
                if op == OCERZ_OP_BTS {
                    nv |= 1u8 << bit;
                } else if op == OCERZ_OP_BTR {
                    nv &= !(1u8 << bit);
                } else {
                    nv ^= 1u8 << bit;
                }
                match hp.compare_exchange(cur, nv, Ordering::SeqCst, Ordering::SeqCst) {
                    Ok(_) => {
                        testbit = ((cur >> bit) & 1) as i32;
                        break;
                    }
                    Err(c) => cur = c,
                }
            }
            ocerz_flag_assign(cpu, OCERZ_CF, testbit);
            if ocerz_watch_addr != 0 && ocerz_watch_addr.wrapping_sub(ea) < 1 {
                ocerz_watch_hit(ea, 1, hp.load(Ordering::SeqCst) as u64, 0);
            }
            return STEP_OK;
        }

        let mut byte = ocerz_ld(ea, 1) as u8;
        testbit = ((byte >> bit) & 1) as i32;
        ocerz_flag_assign(cpu, OCERZ_CF, testbit);
        if op != OCERZ_OP_BT {
            if op == OCERZ_OP_BTS {
                byte |= 1u8 << bit;
            } else if op == OCERZ_OP_BTR {
                byte &= !(1u8 << bit);
            } else {
                byte ^= 1u8 << bit;
            }
            ocerz_st(ea, 1, byte as u64);
        }
        STEP_OK
    }
}

unsafe fn ext_crc32(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let mut crc = cpu.gpr[insn.ops[0].reg as usize] as u32;
        let v = ocerz_read_op(cpu, insn, &insn.ops[1]);
        for i in 0..insn.ops[1].size as u32 {
            crc ^= (v >> (8 * i)) as u8 as u32;
            for _ in 0..8 {
                crc = (crc >> 1) ^ (0x82f63b78u32 & (-((crc & 1) as i32)) as u32);
            }
        }
        ocerz_write_op(cpu, insn, &insn.ops[0], crc as u64);
        STEP_OK
    }
}

unsafe fn ext_scan(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let dst = &insn.ops[0];
        let src = &insn.ops[1];
        let size = dst.size as i32;
        let bits = (size * 8) as u64;
        let s = ocerz_trunc(ocerz_read_op(cpu, insn, src), size);
        match insn.op as u32 {
            OCERZ_OP_BSF => {
                if s == 0 {
                    ocerz_flag_assign(cpu, OCERZ_ZF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_ZF, 0);
                    ocerz_write_op(cpu, insn, dst, s.trailing_zeros() as u64);
                }
                STEP_OK
            }
            OCERZ_OP_BSR => {
                if s == 0 {
                    ocerz_flag_assign(cpu, OCERZ_ZF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_ZF, 0);
                    ocerz_write_op(cpu, insn, dst, (63 - s.leading_zeros()) as u64);
                }
                STEP_OK
            }
            OCERZ_OP_POPCNT => {
                let r = s.count_ones() as u64;
                ocerz_write_op(cpu, insn, dst, r);
                cpu.rflags &= !(OCERZ_CF | OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_PF);
                ocerz_flag_assign(cpu, OCERZ_ZF, (r == 0) as i32);
                STEP_OK
            }
            OCERZ_OP_TZCNT => {
                let r = if s == 0 { bits } else { s.trailing_zeros() as u64 };
                ocerz_write_op(cpu, insn, dst, r);
                ocerz_flag_assign(cpu, OCERZ_CF, (s == 0) as i32);
                ocerz_flag_assign(cpu, OCERZ_ZF, (r == 0) as i32);
                STEP_OK
            }
            OCERZ_OP_LZCNT => {
                let r = if s == 0 { bits } else { bits - 1 - (63 - s.leading_zeros() as u64) };
                ocerz_write_op(cpu, insn, dst, r);
                ocerz_flag_assign(cpu, OCERZ_CF, (s == 0) as i32);
                ocerz_flag_assign(cpu, OCERZ_ZF, (r == 0) as i32);
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

fn bmi_pdep(v: u64, mut mask: u64) -> u64 {
    let mut r = 0;
    let mut bit: u64 = 1;
    while mask != 0 {
        if v & bit != 0 {
            r |= mask & 0u64.wrapping_sub(mask);
        }
        mask &= mask - 1;
        bit <<= 1;
    }
    r
}

fn bmi_pext(v: u64, mut mask: u64) -> u64 {
    let mut r = 0;
    let mut bit: u64 = 1;
    while mask != 0 {
        if v & mask & 0u64.wrapping_sub(mask) != 0 {
            r |= bit;
        }
        mask &= mask - 1;
        bit <<= 1;
    }
    r
}

unsafe fn ext_bmi(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let o0 = &insn.ops[0];
        let o1 = &insn.ops[1];
        let o2 = &insn.ops[2];
        let size = o0.size as i32;
        let bits = size as u32 * 8;
        let m = ocerz_mask(size);
        let a = ocerz_read_op(cpu, insn, o1) & m;
        let r: u64;
        let mut cf = false;
        match insn.op as u32 {
            OCERZ_OP_ANDN => r = !a & ocerz_read_op(cpu, insn, o2) & m,
            OCERZ_OP_BLSR => {
                r = a & a.wrapping_sub(1);
                cf = a == 0;
            }
            OCERZ_OP_BLSMSK => {
                r = (a ^ a.wrapping_sub(1)) & m;
                cf = a == 0;
            }
            OCERZ_OP_BLSI => {
                r = a & 0u64.wrapping_sub(a);
                cf = a != 0;
            }
            OCERZ_OP_BZHI => {
                let n = (ocerz_read_op(cpu, insn, o2) & 0xff) as u32;
                r = if n < bits { a & ((1u64 << n) - 1) } else { a };
                cf = n > bits - 1;
            }
            OCERZ_OP_BEXTR => {
                let ctl = ocerz_read_op(cpu, insn, o2);
                let start = (ctl & 0xff) as u32;
                let len = ((ctl >> 8) & 0xff) as u32;
                let mut r = if start < bits { a >> start } else { 0 };
                if len < bits {
                    r &= (1u64 << len) - 1;
                }
                ocerz_write_op(cpu, insn, o0, r);
                ocerz_flag_assign(cpu, OCERZ_ZF, (r == 0) as i32);
                ocerz_flag_assign(cpu, OCERZ_CF, 0);
                ocerz_flag_assign(cpu, OCERZ_OF, 0);
                return STEP_OK;
            }
            OCERZ_OP_PDEP => {
                let v = bmi_pdep(a, ocerz_read_op(cpu, insn, o2) & m);
                ocerz_write_op(cpu, insn, o0, v);
                return STEP_OK;
            }
            OCERZ_OP_PEXT => {
                let v = bmi_pext(a, ocerz_read_op(cpu, insn, o2) & m);
                ocerz_write_op(cpu, insn, o0, v);
                return STEP_OK;
            }
            OCERZ_OP_MULX => {
                let p = ((cpu.gpr[RDX] & m) as u128) * ((ocerz_read_op(cpu, insn, o2) & m) as u128);
                ocerz_write_op(cpu, insn, o1, (p as u64) & m);
                ocerz_write_op(cpu, insn, o0, ((p >> bits) as u64) & m);
                return STEP_OK;
            }
            OCERZ_OP_RORX => {
                let c = (o2.imm & (bits as u64 - 1)) as u32;
                let v = if c != 0 { ((a >> c) | (a << (bits - c))) & m } else { a };
                ocerz_write_op(cpu, insn, o0, v);
                return STEP_OK;
            }
            OCERZ_OP_SHLX | OCERZ_OP_SHRX | OCERZ_OP_SARX => {
                let c = (ocerz_read_op(cpu, insn, o2) & (bits as u64 - 1)) as u32;
                let r = if insn.op as u32 == OCERZ_OP_SHLX {
                    (a << c) & m
                } else if insn.op as u32 == OCERZ_OP_SHRX {
                    a >> c
                } else {
                    ((ocerz_sext(a, size) >> c) as u64) & m
                };
                ocerz_write_op(cpu, insn, o0, r);
                return STEP_OK;
            }
            OCERZ_OP_MOVBE => {
                let r = if size == 2 {
                    (a as u16).swap_bytes() as u64
                } else if size == 4 {
                    (a as u32).swap_bytes() as u64
                } else {
                    a.swap_bytes()
                };
                ocerz_write_op(cpu, insn, o0, r);
                return STEP_OK;
            }
            OCERZ_OP_RDRAND => {
                let mut r: u64 = 0;
                libc::arc4random_buf(&mut r as *mut u64 as *mut _, 8);
                ocerz_write_op(cpu, insn, o0, r & m);
                cpu.rflags &= !(OCERZ_OF | OCERZ_SF | OCERZ_ZF | OCERZ_AF | OCERZ_PF);
                cpu.rflags |= OCERZ_CF;
                return STEP_OK;
            }
            _ => return EUNSUP,
        }
        ocerz_write_op(cpu, insn, o0, r);
        ocerz_flag_assign(cpu, OCERZ_CF, cf as i32);
        ocerz_flag_assign(cpu, OCERZ_ZF, (r == 0) as i32);
        ocerz_flag_assign(cpu, OCERZ_SF, ocerz_msb(r, size) as i32);
        ocerz_flag_assign(cpu, OCERZ_OF, 0);
        STEP_OK
    }
}

fn cpuid_brand(leaf: u32, regs: &mut [u32; 4]) {
    let mut brand = [0u8; 48];
    let s = b"Ocerz x86_64 Emulated CPU";
    brand[..s.len()].copy_from_slice(s);
    let idx = (leaf - 0x80000002) as usize;
    for (k, r) in regs.iter_mut().enumerate() {
        let o = (idx * 4 + k) * 4;
        *r = u32::from_le_bytes([brand[o], brand[o + 1], brand[o + 2], brand[o + 3]]);
    }
}

fn ext_cpuid(cpu: &mut OcerzCPU) -> i32 {
    let leaf = cpu.gpr[RAX] as u32;
    let mut r = [0u32; 4];
    if leaf == 0 {
        r = [0xd, 0x756e6547, 0x6c65746e, 0x49656e69];
    } else if leaf == 1 {
        r = [0x000306a9, 0x00100800, 0x00982201, 0x078bfbff];
    } else if leaf == 0xd {
        let sub = cpu.gpr[RCX] as u32;
        if sub == 0 {
            r[0] = 7;
            r[1] = 0x340;
            r[2] = 0x340;
        } else if sub == 2 {
            r[0] = 0x100;
            r[1] = 0x240;
        }
    } else if leaf == 0x80000000 {
        r[0] = 0x80000004;
    } else if leaf == 0x80000001 {
        r[2] = 0x00000001;
        r[3] = 0x28100800;
    } else if (0x80000002..=0x80000004).contains(&leaf) {
        cpuid_brand(leaf, &mut r);
    }
    cpu.gpr[RAX] = r[0] as u64;
    cpu.gpr[RBX] = r[1] as u64;
    cpu.gpr[RCX] = r[2] as u64;
    cpu.gpr[RDX] = r[3] as u64;
    STEP_OK
}

static mut TB_NUMER: u32 = 0;
static mut TB_DENOM: u32 = 0;
static mut HAVE_TB: i32 = 0;

#[allow(deprecated)]
fn ext_rdtsc_ns() -> u64 {
    unsafe {
        if HAVE_TB == 0 {
            let mut tb = libc::mach_timebase_info_data_t { numer: 0, denom: 0 };
            libc::mach_timebase_info(&mut tb);
            TB_NUMER = tb.numer;
            TB_DENOM = tb.denom;
            HAVE_TB = 1;
        }
        let t = libc::mach_absolute_time();
        t.wrapping_mul(TB_NUMER as u64) / TB_DENOM as u64
    }
}

fn ext_rdtsc(cpu: &mut OcerzCPU, rdtscp: bool) -> i32 {
    let ns = ext_rdtsc_ns();
    cpu.gpr[RAX] = ns as u32 as u64;
    cpu.gpr[RDX] = (ns >> 32) as u32 as u64;
    if rdtscp {
        cpu.gpr[RCX] = 0;
    }
    STEP_OK
}

unsafe fn ext_fxsave(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let ea = ocerz_ea(cpu, insn, &insn.ops[0]);
        crate::ported::x87::ocerz_x87_fxsave(cpu, ea);
        ocerz_st(ea + 24, 4, cpu.mxcsr as u64);
        ocerz_st(ea + 28, 4, 0x0000ffff);
        for i in 0..16 {
            ocerz_st128(ea + 160 + (i as u64) * 16, cpu.xmm[i]);
        }
        STEP_OK
    }
}

unsafe fn ext_fxrstor(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let ea = ocerz_ea(cpu, insn, &insn.ops[0]);
        crate::ported::x87::ocerz_x87_fxrstor(cpu, ea);
        cpu.mxcsr = ocerz_ld(ea + 24, 4) as u32;
        ocerz_apply_mxcsr_round(cpu.mxcsr);
        for i in 0..16 {
            cpu.xmm[i] = ocerz_ld128(ea + 160 + (i as u64) * 16);
        }
        STEP_OK
    }
}

fn ext_xgetbv(cpu: &mut OcerzCPU) -> i32 {
    if cpu.gpr[RCX] as u32 != 0 {
        crate::ocerz_fatal!("xgetbv with ecx=%u is unsupported\n\0", cpu.gpr[RCX] as u32 as libc::c_uint);
        return OCERZ_STEP_FATAL as i32;
    }
    cpu.gpr[RAX] = 7;
    cpu.gpr[RDX] = 0;
    STEP_OK
}

unsafe fn ext_xsave(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let ea = ocerz_ea(cpu, insn, &insn.ops[0]);
        let rfbm = (cpu.gpr[RAX] as u32 as u64) & 7;
        if rfbm & 1 != 0 {
            crate::ported::x87::ocerz_x87_fxsave(cpu, ea);
        }
        if rfbm & 6 != 0 {
            ocerz_st(ea + 24, 4, cpu.mxcsr as u64);
            ocerz_st(ea + 28, 4, 0x0000ffff);
        }
        if rfbm & 2 != 0 {
            for i in 0..16 {
                ocerz_st128(ea + 160 + (i as u64) * 16, cpu.xmm[i]);
            }
        }
        if rfbm & 4 != 0 {
            for i in 0..16 {
                ocerz_st128(ea + 576 + (i as u64) * 16, cpu.ymmh[i]);
            }
        }
        ocerz_st(ea + 512, 8, ((ocerz_ld(ea + 512, 8) & !rfbm) | rfbm) & 7);
        STEP_OK
    }
}

unsafe fn ext_xrstor(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let ea = ocerz_ea(cpu, insn, &insn.ops[0]);
        let rfbm = (cpu.gpr[RAX] as u32 as u64) & 7;
        let bv = ocerz_ld(ea + 512, 8);
        if rfbm & 1 != 0 {
            if bv & 1 != 0 {
                crate::ported::x87::ocerz_x87_fxrstor(cpu, ea);
            } else {
                crate::ported::x87::ocerz_x87_reset(cpu);
                cpu.fpr = [0.0; 8];
                cpu.fpr_x_ok = 0;
            }
        }
        if rfbm & 6 != 0 {
            cpu.mxcsr = ocerz_ld(ea + 24, 4) as u32;
            ocerz_apply_mxcsr_round(cpu.mxcsr);
        }
        for i in 0..16 {
            if rfbm & 2 != 0 {
                if bv & 2 != 0 {
                    cpu.xmm[i] = ocerz_ld128(ea + 160 + (i as u64) * 16);
                } else {
                    cpu.xmm[i] = Ocerz128 { lo: 0, hi: 0 };
                }
            }
            if rfbm & 4 != 0 {
                if bv & 4 != 0 {
                    cpu.ymmh[i] = ocerz_ld128(ea + 576 + (i as u64) * 16);
                    cpu.ymmh_all_zero = 0;
                } else {
                    cpu.ymmh[i] = Ocerz128 { lo: 0, hi: 0 };
                }
            }
        }
        STEP_OK
    }
}

unsafe fn ext_misc(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        match insn.op as u32 {
            OCERZ_OP_CPUID => ext_cpuid(cpu),
            OCERZ_OP_RDTSC => ext_rdtsc(cpu, false),
            OCERZ_OP_RDTSCP => ext_rdtsc(cpu, true),
            OCERZ_OP_XGETBV => ext_xgetbv(cpu),
            OCERZ_OP_SGDT | OCERZ_OP_SIDT => {
                let ea = ocerz_ea(cpu, insn, &insn.ops[0]);
                ocerz_st(ea, 2, (cpu.cpu_number & 0xfff) as u16 as u64);
                ocerz_st(ea + 2, 8, 0);
                STEP_OK
            }
            OCERZ_OP_LDMXCSR => {
                cpu.mxcsr = ocerz_ld(ocerz_ea(cpu, insn, &insn.ops[0]), 4) as u32;
                ocerz_apply_mxcsr_round(cpu.mxcsr);
                STEP_OK
            }
            OCERZ_OP_STMXCSR => {
                ocerz_st(ocerz_ea(cpu, insn, &insn.ops[0]), 4, cpu.mxcsr as u64);
                STEP_OK
            }
            OCERZ_OP_FXSAVE => ext_fxsave(cpu, insn),
            OCERZ_OP_FXRSTOR => ext_fxrstor(cpu, insn),
            OCERZ_OP_XSAVE => ext_xsave(cpu, insn),
            OCERZ_OP_XRSTOR => ext_xrstor(cpu, insn),
            OCERZ_OP_EMMS => {
                cpu.ftop = 0;
                cpu.ftw = 0;
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_interp_ext(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let op = (*insn).op as u32;
        if op > OCERZ_OP_X87_FIRST && op < OCERZ_OP_SSE_FIRST {
            return crate::ported::x87::ocerz_x87_exec(cpu, insn);
        }
        let cpu = &mut *cpu;
        let insn = &*insn;
        match op {
            OCERZ_OP_MOVS | OCERZ_OP_STOS | OCERZ_OP_LODS | OCERZ_OP_SCAS | OCERZ_OP_CMPS => ext_string(cpu, insn),
            OCERZ_OP_BT | OCERZ_OP_BTS | OCERZ_OP_BTR | OCERZ_OP_BTC => ext_bit(cpu, insn),
            OCERZ_OP_CRC32 => ext_crc32(cpu, insn),
            OCERZ_OP_BSF | OCERZ_OP_BSR | OCERZ_OP_POPCNT | OCERZ_OP_TZCNT | OCERZ_OP_LZCNT => ext_scan(cpu, insn),
            OCERZ_OP_CPUID | OCERZ_OP_RDTSC | OCERZ_OP_RDTSCP | OCERZ_OP_XGETBV | OCERZ_OP_SGDT | OCERZ_OP_SIDT
            | OCERZ_OP_LDMXCSR | OCERZ_OP_STMXCSR | OCERZ_OP_FXSAVE | OCERZ_OP_FXRSTOR | OCERZ_OP_XSAVE
            | OCERZ_OP_XRSTOR | OCERZ_OP_EMMS => ext_misc(cpu, insn),
            OCERZ_OP_ANDN | OCERZ_OP_BLSR | OCERZ_OP_BLSMSK | OCERZ_OP_BLSI | OCERZ_OP_BZHI | OCERZ_OP_BEXTR
            | OCERZ_OP_PDEP | OCERZ_OP_PEXT | OCERZ_OP_MULX | OCERZ_OP_RORX | OCERZ_OP_SARX | OCERZ_OP_SHLX
            | OCERZ_OP_SHRX | OCERZ_OP_MOVBE | OCERZ_OP_RDRAND => ext_bmi(cpu, insn),
            _ => EUNSUP,
        }
    }
}
