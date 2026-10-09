//! Shared inline operand machinery for the interpreter modules, the Rust twin
//! of include/ocerz/interp_common.h together with the guest memory accessors
//! of mem.h they are built on (bindgen does not bind `static inline`).
//!
//! The stack pointer is RSP in long mode and ESP in i386 mode, so in 32-bit mode
//! every update wraps at 32 bits instead of 64, and the address is SS:ESP, which
//! wraps the same way.  The mode always comes from the decoded instruction,
//! never from a test inside the emitted code.  The plain push/pop entry points
//! delegate with mode32 = 0, which is the identity, so no 64-bit behaviour
//! changes; the JIT's 32-bit POP/RET express the same rule with a UXTW-indexed
//! load at zero instruction cost.
//!
//! The absolute gs:[0x58] form is special-cased.  Wine on macOS runs 64-bit PE
//! code with gs at the Darwin TSD and mirrors the TEB fields PE code reads into
//! the wine-reserved slots, but ThreadLocalStoragePointer is the one field that
//! changes after the mirror is taken - so a thread whose snapshot preceded its
//! TLS setup reads 0 and MSVC __declspec(thread) code crashes.  Slot 6 holds the
//! TEB self pointer on wine threads and is 0 on everything else, so indirecting
//! through it yields the live field and is self-selecting.
//!
//! Every helper is `#[inline(always)]` so the interpreter's hot dispatch sees
//! straight-line loads and stores, exactly as the C build did.
#![allow(dead_code)]

use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering, fence};

use crate::ffi::{
    OCERZ_COMMPAGE_HI, OCERZ_COMMPAGE_LO, OCERZ_LOW_LIMIT, OCERZ_TOP_HI, OCERZ_TOP_LO,
    OCERZ_OPK_IMM, OCERZ_OPK_MEM, OCERZ_OPK_REG, OCERZ_OPK_XMM, OCERZ_RSP, OCERZ_SEG_FS,
    OCERZ_SEG_GS, Ocerz128, OcerzCPU, X86Insn, X86Operand, ocerz_commpage, ocerz_guest_base,
    ocerz_low_base, ocerz_pin_map, ocerz_top_base, ocerz_watch_addr, ocerz_watch_hit,
    ocerz_watch_len, ocerz_watch_shadow, ocerz_watch_val,
};
use crate::inline::ocerz_trunc;

pub const REG_NONE: u8 = 0xff;
const OCERZ_NULL_LIMIT: u64 = crate::ffi::OCERZ_NULL_LIMIT as u64;

#[inline(always)]
pub unsafe fn ocerz_pinned_page(gaddr: u64) -> bool {
    unsafe {
        !ocerz_pin_map.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*ocerz_pin_map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
    }
}

#[inline(always)]
pub unsafe fn ocerz_g2h(gaddr: u64) -> *mut u8 {
    unsafe {
        if !ocerz_commpage.is_null() && gaddr >= OCERZ_COMMPAGE_LO && gaddr < OCERZ_COMMPAGE_HI {
            return ocerz_commpage.add((gaddr - OCERZ_COMMPAGE_LO) as usize);
        }
        if ocerz_low_base != 0 {
            if gaddr < OCERZ_LOW_LIMIT {
                if (gaddr < OCERZ_NULL_LIMIT && !ocerz_pin_map.is_null()) || ocerz_pinned_page(gaddr) {
                    return gaddr as usize as *mut u8;
                }
                return gaddr.wrapping_add(ocerz_low_base) as usize as *mut u8;
            }
            if gaddr.wrapping_sub(OCERZ_TOP_LO) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return (gaddr.wrapping_sub(OCERZ_TOP_LO).wrapping_add(ocerz_top_base)) as usize
                    as *mut u8;
            }
        }
        gaddr.wrapping_add(ocerz_guest_base) as usize as *mut u8
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld(gaddr: u64, size: i32) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        match size {
            1 => return AtomicU8::from_ptr(p).load(Ordering::Acquire) as u64,
            2 => {
                if (p as usize) & 1 == 0 {
                    return AtomicU16::from_ptr(p as *mut u16).load(Ordering::Acquire) as u64;
                }
            }
            4 => {
                if (p as usize) & 3 == 0 {
                    return AtomicU32::from_ptr(p as *mut u32).load(Ordering::Acquire) as u64;
                }
            }
            8 => {
                if (p as usize) & 7 == 0 {
                    return AtomicU64::from_ptr(p as *mut u64).load(Ordering::Acquire);
                }
            }
            _ => {}
        }
        let mut v: u64 = 0;
        core::ptr::copy_nonoverlapping(p as *const u8, &mut v as *mut u64 as *mut u8, size as usize);
        fence(Ordering::Acquire);
        v
    }
}

#[inline(always)]
pub unsafe fn ocerz_st(gaddr: u64, size: i32, v: u64) {
    unsafe {
        if ocerz_watch_addr != 0
            && gaddr < ocerz_watch_addr.wrapping_add(ocerz_watch_len)
            && gaddr.wrapping_add(size as u64) > ocerz_watch_addr
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_val != 0 && v == ocerz_watch_val {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_shadow != 0
            && ocerz_low_base != 0
            && size == 8
            && v.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        let p = ocerz_g2h(gaddr);
        match size {
            1 => {
                AtomicU8::from_ptr(p).store(v as u8, Ordering::Release);
                return;
            }
            2 => {
                if (p as usize) & 1 == 0 {
                    AtomicU16::from_ptr(p as *mut u16).store(v as u16, Ordering::Release);
                    return;
                }
            }
            4 => {
                if (p as usize) & 3 == 0 {
                    AtomicU32::from_ptr(p as *mut u32).store(v as u32, Ordering::Release);
                    return;
                }
            }
            8 => {
                if (p as usize) & 7 == 0 {
                    AtomicU64::from_ptr(p as *mut u64).store(v, Ordering::Release);
                    return;
                }
            }
            _ => {}
        }
        fence(Ordering::Release);
        core::ptr::copy_nonoverlapping(&v as *const u64 as *const u8, p, size as usize);
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld128(gaddr: u64) -> Ocerz128 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        if (p as usize) & 7 == 0 {
            let lo = AtomicU64::from_ptr(p as *mut u64).load(Ordering::Acquire);
            let hi = AtomicU64::from_ptr((p as *mut u64).add(1)).load(Ordering::Acquire);
            Ocerz128 { lo, hi }
        } else {
            let v = core::ptr::read_unaligned(p as *const Ocerz128);
            fence(Ordering::Acquire);
            v
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_st128(gaddr: u64, v: Ocerz128) {
    unsafe {
        if ocerz_watch_addr != 0 && ocerz_watch_addr.wrapping_sub(gaddr) < 16 {
            ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
        }
        if ocerz_watch_val != 0 && (v.lo == ocerz_watch_val || v.hi == ocerz_watch_val) {
            ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
        }
        if ocerz_watch_shadow != 0
            && ocerz_low_base != 0
            && (v.lo.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT
                || v.hi.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT)
        {
            ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
        }
        let p = ocerz_g2h(gaddr);
        if (p as usize) & 7 == 0 {
            AtomicU64::from_ptr(p as *mut u64).store(v.lo, Ordering::Release);
            AtomicU64::from_ptr((p as *mut u64).add(1)).store(v.hi, Ordering::Release);
        } else {
            fence(Ordering::Release);
            core::ptr::write_unaligned(p as *mut Ocerz128, v);
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_ea(cpu: *const OcerzCPU, insn: *const X86Insn, op: *const X86Operand) -> u64 {
    unsafe {
        let cpu = &*cpu;
        let insn = &*insn;
        let op = &*op;
        let mut a: u64;
        if op.riprel != 0 {
            a = op.disp as u64;
        } else {
            a = op.disp as u64;
            if op.base != REG_NONE {
                a = a.wrapping_add(*cpu.gpr.get_unchecked(op.base as usize));
            }
            if op.index != REG_NONE {
                a = a.wrapping_add(*cpu.gpr.get_unchecked(op.index as usize) << op.scale);
            }
            if insn.addrsize == 4 {
                a = a as u32 as u64;
            } else if insn.addrsize == 2 {
                a = a as u16 as u64;
            }
        }
        if insn.seg as u32 == OCERZ_SEG_FS {
            a = a.wrapping_add(cpu.fs_base);
        } else if insn.seg as u32 == OCERZ_SEG_GS {
            a = a.wrapping_add(cpu.gs_base);
            if op.disp == 0x58
                && op.base == REG_NONE
                && op.index == REG_NONE
                && op.riprel == 0
                && insn.addrsize == 8
                && (cpu.gs_base >> 32) != 0
            {
                let slf = ocerz_ld(cpu.gs_base.wrapping_add(0x30), 8);
                if slf != 0 && slf != cpu.gs_base {
                    a = slf.wrapping_add(0x58);
                }
            }
        }
        a
    }
}

#[inline(always)]
pub unsafe fn ocerz_read_gpr(cpu: *const OcerzCPU, reg: u32, size: i32, high8: i32) -> u64 {
    unsafe {
        let v = *(*cpu).gpr.get_unchecked(reg as usize);
        if high8 != 0 {
            return (v >> 8) & 0xff;
        }
        ocerz_trunc(v, size)
    }
}

#[inline(always)]
pub unsafe fn ocerz_write_gpr(cpu: *mut OcerzCPU, reg: u32, size: i32, high8: i32, v: u64) {
    unsafe {
        let r = (*cpu).gpr.get_unchecked_mut(reg as usize);
        if high8 != 0 {
            *r = (*r & !0xff00u64) | ((v & 0xff) << 8);
            return;
        }
        match size {
            1 => *r = (*r & !0xffu64) | (v & 0xff),
            2 => *r = (*r & !0xffffu64) | (v & 0xffff),
            4 => *r = v as u32 as u64,
            _ => *r = v,
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_read_op(cpu: *mut OcerzCPU, insn: *const X86Insn, op: *const X86Operand) -> u64 {
    unsafe {
        let o = &*op;
        match o.kind as u32 {
            OCERZ_OPK_REG => ocerz_read_gpr(cpu, o.reg as u32, o.size as i32, o.high8 as i32),
            OCERZ_OPK_IMM => ocerz_trunc(o.imm, o.size as i32),
            OCERZ_OPK_MEM => ocerz_ld(ocerz_ea(cpu, insn, op), o.size as i32),
            _ => 0,
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_write_op(cpu: *mut OcerzCPU, insn: *const X86Insn, op: *const X86Operand, v: u64) {
    unsafe {
        let o = &*op;
        if o.kind as u32 == OCERZ_OPK_REG {
            ocerz_write_gpr(cpu, o.reg as u32, o.size as i32, o.high8 as i32, v);
        } else {
            ocerz_st(ocerz_ea(cpu, insn, op), o.size as i32, v);
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_read_op128(cpu: *mut OcerzCPU, insn: *const X86Insn, op: *const X86Operand) -> Ocerz128 {
    unsafe {
        let o = &*op;
        if o.kind as u32 == OCERZ_OPK_XMM {
            return *(*cpu).xmm.get_unchecked(o.reg as usize);
        }
        if o.size == 16 {
            return ocerz_ld128(ocerz_ea(cpu, insn, op));
        }
        Ocerz128 { lo: ocerz_ld(ocerz_ea(cpu, insn, op), o.size as i32), hi: 0 }
    }
}

#[inline(always)]
pub unsafe fn ocerz_write_op128(cpu: *mut OcerzCPU, insn: *const X86Insn, op: *const X86Operand, v: Ocerz128) {
    unsafe {
        let o = &*op;
        if o.kind as u32 == OCERZ_OPK_XMM {
            *(*cpu).xmm.get_unchecked_mut(o.reg as usize) = v;
            return;
        }
        if o.size == 16 {
            ocerz_st128(ocerz_ea(cpu, insn, op), v);
        } else {
            ocerz_st(ocerz_ea(cpu, insn, op), o.size as i32, v.lo);
        }
    }
}

#[inline(always)]
pub fn ocerz_stack_wrap(sp: u64, mode32: i32) -> u64 {
    if mode32 != 0 { sp as u32 as u64 } else { sp }
}

#[inline(always)]
pub unsafe fn ocerz_push_mode(cpu: *mut OcerzCPU, size: i32, v: u64, mode32: i32) {
    unsafe {
        let sp = ocerz_stack_wrap((*cpu).gpr[OCERZ_RSP as usize].wrapping_sub(size as u64), mode32);
        ocerz_st(sp, size, v);
        (*cpu).gpr[OCERZ_RSP as usize] = sp;
    }
}

#[inline(always)]
pub unsafe fn ocerz_pop_mode(cpu: *mut OcerzCPU, size: i32, mode32: i32) -> u64 {
    unsafe {
        let v = ocerz_ld(ocerz_stack_wrap((*cpu).gpr[OCERZ_RSP as usize], mode32), size);
        (*cpu).gpr[OCERZ_RSP as usize] =
            ocerz_stack_wrap((*cpu).gpr[OCERZ_RSP as usize].wrapping_add(size as u64), mode32);
        v
    }
}

#[inline(always)]
pub unsafe fn ocerz_push(cpu: *mut OcerzCPU, size: i32, v: u64) {
    unsafe { ocerz_push_mode(cpu, size, v, 0) }
}

#[inline(always)]
pub unsafe fn ocerz_pop(cpu: *mut OcerzCPU, size: i32) -> u64 {
    unsafe { ocerz_pop_mode(cpu, size, 0) }
}
