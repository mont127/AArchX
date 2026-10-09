//! The core single-step interpreter: reference semantics for x86_64, and the
//! oracle the JIT is checked against by the differential gates.
//!
//! x86 makes a LOCK-prefixed access atomic at any alignment - the bus or
//! cache-line lock covers a split access - while ARM64's LSE atomics fault on a
//! misaligned address, as does clang's __atomic_*.  Wine's CRITICAL_SECTION
//! inside a packed Valve struct sits at +0x446, so every EnterCriticalSection in
//! Steam's CEF host is a `lock cmpxchg` on a dword that is 2 mod 4.  The JIT
//! sends those here, and a misaligned access takes a lock striped by address and
//! does the read-modify-write with plain unaligned copies: atomic against every
//! other misaligned access to the same location, which is the guarantee that
//! actually matters.  cmpxchg's flags follow Rosetta, the golden oracle for the
//! guest tests: flags as (dest - accumulator), and the accumulator written in
//! both cases, so the 32-bit form zero-extends rax on a match too.
//!
//! A divide fault is delivered as a real fault - rip stays at the div and the
//! guest handler sees SIGFPE with FPE_INTDIV/FPE_INTOVF, as on Darwin - and
//! stays fatal when no handler is installed.  Windows' int $0x29 (__fastfail)
//! dies here instead of being handed to the guest as a #GP: dispatching the
//! exception so Wine can raise STATUS_STACK_BUFFER_OVERRUN was tried and
//! measured worse, because the thread that fast-fails is already deep, the
//! dispatch overflows its stack, and it wedges holding ntdll's loader_section so
//! the process can never exit.
//!
//! ---- the i386 slice ----
//! The CPU is never asked "am I in 32-bit mode?" at each instruction: the
//! interpreter chooses a decode mode in exactly one place, and every far
//! transfer - JMPF, CALLF, RETF, IRET - funnels through a single helper, so the
//! mode, the selector and EIP/RIP can never disagree.  EIP is re-wrapped after
//! execution keyed on the mode the CPU is in NOW, because a far transfer that
//! just left 32-bit mode has already produced a full 64-bit rip.
//!
//! The details that bite are the ones the SDM states and that are easy to miss.
//! With (E)SP as a base or index of a POP's memory destination the effective
//! address is computed AFTER the pop adjusts (E)SP: V8's TailCallRuntime
//! trampoline moves its return address with `pop qword [rsp+0x98]`, and computed
//! with the old rsp it lands one slot low and CEntry returns into a stale heap
//! pointer.  POP ESP takes its new value from the slot, not the adjustment.  A
//! selector slot holds 16 bits and hardware ignores the rest - Wine's
//! I386_CONTEXT stores SegCs/SegSs as DWORDs that RtlCaptureContext fills with a
//! 16-bit store, so their upper halves are stack garbage by the time wow64cpu
//! pushes them for an iretq - and a zero CS or SS on the stack is a frame this
//! emulator built itself, not a mode decision, so the current selector is kept.
//! DAS's second test has no ELSE, so a CF raised by the first adjustment
//! survives when the second does not fire.  The BCD and adjust handlers leave
//! every flag the SDM calls UNDEFINED untouched.  FS and GS are the two segments
//! whose base is applied to addresses, so loading one moves the base; ES/SS/DS
//! are flat and carry no base.
//!
//! ---- diagnostics ----
//! Being the slow path makes this the right place to watch from.  OCERZ_EXCLOG
//! traps the guest's _objc_exception_throw and __cxa_throw and recovers the
//! exception name, reason and throw-site chain straight out of guest memory,
//! falling back to raw words so an unfamiliar string class is still decodable by
//! hand rather than lost.  OCERZ_REGTRAP dumps every register at chosen guest
//! addresses, prefixed by the frame-pointer return chain that got there (with
//! OCERZ_REGTRAP_DEREF for the memory behind them, because a heap address is
//! never the same twice and a watch cannot be aimed at it in a later run, and
//! OCERZ_REGTRAP_MAX to stop after that many hits, since a trap on a recursing
//! function otherwise prints until the stack runs out), OCERZ_RIPTRAP is the
//! older fixed-field form, and OCERZ_V8DUMP
//! reads V8's Ignition dispatch table and bytecode header.  These are interpreter
//! only, so they are paired with OCERZ_INTERP_LO/HI to bring chosen code here.

use core::ffi::{c_char, c_int, c_uint};
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};

use crate::ffi::*;
use crate::inline::{
    OCERZ_AF, OCERZ_CF, OCERZ_DF, OCERZ_FLAG_FIXED1, OCERZ_IF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_TF,
    OCERZ_ZF, ocerz_flag_assign, ocerz_mask, ocerz_msb, ocerz_sext, ocerz_trunc,
};
use crate::interp_common::*;
use crate::log::stderr;
use crate::ported::flags::{
    ocerz_cc_eval, ocerz_flags_add, ocerz_flags_dec, ocerz_flags_imul, ocerz_flags_inc, ocerz_flags_logic,
    ocerz_flags_mul, ocerz_flags_sar, ocerz_flags_shl, ocerz_flags_shr, ocerz_flags_sub,
};

const STEP_OK: i32 = OCERZ_STEP_OK as i32;
const STEP_FATAL: i32 = OCERZ_STEP_FATAL as i32;
const STEP_REDIRECT: i32 = OCERZ_STEP_REDIRECT as i32;
const RAX: usize = OCERZ_RAX as usize;
const RCX: usize = OCERZ_RCX as usize;
const RDX: usize = OCERZ_RDX as usize;
const RSP: usize = OCERZ_RSP as usize;
const RBP: usize = OCERZ_RBP as usize;
const RSI: usize = OCERZ_RSI as usize;
const RDI: usize = OCERZ_RDI as usize;
const R10: usize = OCERZ_R10 as usize;
const R11: usize = OCERZ_R11 as usize;

macro_rules! op {
    ($insn:expr, $i:expr) => {
        (*$insn).ops.as_ptr().add($i)
    };
}

unsafe fn dump_raw_bytes(out: *mut libc::FILE, rip: u64, mut len: u32) {
    unsafe {
        let p = ocerz_g2h(rip) as *const u8;
        if len == 0 || len > 15 {
            len = 15;
        }
        for i in 0..len as usize {
            libc::fprintf(out, c"%02x ".as_ptr(), *p.add(i) as c_uint);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_unimpl(
    _vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    insn: *const X86Insn,
    why: *const c_char,
) -> c_int {
    unsafe {
        libc::fprintf(
            stderr(),
            c"ocerz: fatal: unimplemented instruction %s at rip=%#llx (%s)\n  bytes: ".as_ptr(),
            ocerz_op_name((*insn).op as c_uint),
            (*insn).rip as libc::c_ulonglong,
            if why.is_null() { c"".as_ptr() } else { why },
        );
        dump_raw_bytes(stderr(), (*insn).rip, (*insn).len as u32);
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
    STEP_FATAL
}

#[unsafe(no_mangle)]
pub static mut ocerz_cftrap_on: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cftrap(cpu: *const OcerzCPU, src: u64, target: u64, kind: *const c_char) {
    unsafe {
        let g = &(*cpu).gpr;
        libc::fprintf(
            stderr(),
            c"ocerz: CFTRAP %s cpu=%d src=%#llx target=%#llx rax=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r10=%#llx r11=%#llx [rsp]=%#llx\n".as_ptr(),
            kind,
            (*cpu).cpu_number as c_int,
            src as libc::c_ulonglong,
            target as libc::c_ulonglong,
            g[RAX] as libc::c_ulonglong,
            g[RCX] as libc::c_ulonglong,
            g[RDX] as libc::c_ulonglong,
            g[RSI] as libc::c_ulonglong,
            g[RDI] as libc::c_ulonglong,
            g[R10] as libc::c_ulonglong,
            g[R11] as libc::c_ulonglong,
            ocerz_ld(g[RSP], 8) as libc::c_ulonglong,
        );
    }
}

#[cold]
unsafe fn trap_fatal(insn: *const X86Insn, msg: *const c_char) -> i32 {
    unsafe {
        libc::fprintf(stderr(), c"ocerz: fatal: %s at rip=%#llx\n  bytes: ".as_ptr(), msg, (*insn).rip as libc::c_ulonglong);
        dump_raw_bytes(stderr(), (*insn).rip, (*insn).len as u32);
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
    STEP_FATAL
}

#[cold]
unsafe fn div_trap(cpu: *mut OcerzCPU, insn: *const X86Insn, code: u32, msg: *const c_char) -> i32 {
    unsafe {
        let next = (*cpu).rip;
        (*cpu).rip = (*insn).rip;
        if ocerz_signal_deliver(cpu, OCERZ_SIGFPE as _, (*insn).rip, code as _, 0) != 0 {
            return STEP_REDIRECT;
        }
        (*cpu).rip = next;
        trap_fatal(insn, msg)
    }
}

#[inline]
unsafe fn lea_addr(cpu: *const OcerzCPU, insn: *const X86Insn, op: *const X86Operand) -> u64 {
    unsafe {
        let op = &*op;
        let mut a = op.disp as u64;
        if op.riprel != 0 {
            return a;
        }
        if op.base != REG_NONE {
            a = a.wrapping_add(*(*cpu).gpr.get_unchecked(op.base as usize));
        }
        if op.index != REG_NONE {
            a = a.wrapping_add(*(*cpu).gpr.get_unchecked(op.index as usize) << op.scale);
        }
        if (*insn).addrsize == 4 {
            a = a as u32 as u64;
        } else if (*insn).addrsize == 2 {
            a = a as u16 as u64;
        }
        a
    }
}

#[inline(always)]
unsafe fn read_acc(cpu: *const OcerzCPU, size: i32) -> u64 {
    unsafe { ocerz_read_gpr(cpu, RAX as u32, size, 0) }
}

#[inline(always)]
unsafe fn write_acc(cpu: *mut OcerzCPU, size: i32, v: u64) {
    unsafe { ocerz_write_gpr(cpu, RAX as u32, size, 0, v) }
}

static G_UNAL_LOCK: [AtomicU32; 256] = [const { AtomicU32::new(0) }; 256];

#[inline(always)]
fn unal_lock_for(gaddr: u64) -> &'static AtomicU32 {
    unsafe { G_UNAL_LOCK.get_unchecked(((gaddr >> 3) & 255) as usize) }
}
#[inline(always)]
fn unal_lock(l: &AtomicU32) {
    while l.swap(1, Ordering::Acquire) != 0 {
        while l.load(Ordering::Relaxed) != 0 {
            unsafe { core::arch::asm!("yield", options(nomem, nostack, preserves_flags)) };
        }
    }
}
#[inline(always)]
fn unal_unlock(l: &AtomicU32) {
    l.store(0, Ordering::Release);
}
#[inline(always)]
fn unal_misaligned(gaddr: u64, size: i32) -> bool {
    (gaddr & (size as u64).wrapping_sub(1)) != 0
}
#[inline(always)]
unsafe fn unal_load(p: *const u8, size: i32) -> u64 {
    let mut v = 0u64;
    unsafe { core::ptr::copy_nonoverlapping(p, &mut v as *mut u64 as *mut u8, size as usize) };
    v
}
#[inline(always)]
unsafe fn unal_store(p: *mut u8, size: i32, v: u64) {
    unsafe { core::ptr::copy_nonoverlapping(&v as *const u64 as *const u8, p, size as usize) };
}

#[inline(always)]
unsafe fn watch_check(gaddr: u64, size: i32, lo: u64, hi: u64) {
    unsafe {
        if ocerz_watch_addr != 0
            && gaddr < ocerz_watch_addr.wrapping_add(ocerz_watch_len)
            && gaddr.wrapping_add(size as u64) > ocerz_watch_addr
        {
            ocerz_watch_hit(gaddr, size, lo, hi);
        }
    }
}

unsafe fn ocerz_atomic_xchg(gaddr: u64, size: i32, v: u64) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        let old;
        if unal_misaligned(gaddr, size) {
            let l = unal_lock_for(gaddr);
            unal_lock(l);
            old = unal_load(p, size);
            unal_store(p, size, v);
            unal_unlock(l);
        } else {
            old = match size {
                1 => AtomicU8::from_ptr(p).swap(v as u8, Ordering::SeqCst) as u64,
                2 => AtomicU16::from_ptr(p as *mut u16).swap(v as u16, Ordering::SeqCst) as u64,
                4 => AtomicU32::from_ptr(p as *mut u32).swap(v as u32, Ordering::SeqCst) as u64,
                _ => AtomicU64::from_ptr(p as *mut u64).swap(v, Ordering::SeqCst),
            };
        }
        watch_check(gaddr, size, v, 0);
        old
    }
}

unsafe fn ocerz_atomic_fetch_add(gaddr: u64, size: i32, v: u64) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        let old;
        if unal_misaligned(gaddr, size) {
            let l = unal_lock_for(gaddr);
            unal_lock(l);
            old = unal_load(p, size);
            unal_store(p, size, ocerz_trunc(old.wrapping_add(v), size));
            unal_unlock(l);
        } else {
            old = match size {
                1 => AtomicU8::from_ptr(p).fetch_add(v as u8, Ordering::SeqCst) as u64,
                2 => AtomicU16::from_ptr(p as *mut u16).fetch_add(v as u16, Ordering::SeqCst) as u64,
                4 => AtomicU32::from_ptr(p as *mut u32).fetch_add(v as u32, Ordering::SeqCst) as u64,
                _ => AtomicU64::from_ptr(p as *mut u64).fetch_add(v, Ordering::SeqCst),
            };
        }
        watch_check(gaddr, size, ocerz_trunc(old.wrapping_add(v), size), 0);
        old
    }
}

unsafe fn ocerz_atomic_cmpxchg(gaddr: u64, size: i32, expected: &mut u64, desired: u64) -> bool {
    unsafe {
        let p = ocerz_g2h(gaddr);
        let ok;
        if unal_misaligned(gaddr, size) {
            let l = unal_lock_for(gaddr);
            unal_lock(l);
            let cur = unal_load(p, size);
            ok = cur == ocerz_trunc(*expected, size);
            if ok {
                unal_store(p, size, desired);
            }
            unal_unlock(l);
            *expected = cur;
        } else {
            let r = match size {
                1 => AtomicU8::from_ptr(p)
                    .compare_exchange(*expected as u8, desired as u8, Ordering::SeqCst, Ordering::SeqCst)
                    .map(|x| x as u64)
                    .map_err(|x| x as u64),
                2 => AtomicU16::from_ptr(p as *mut u16)
                    .compare_exchange(*expected as u16, desired as u16, Ordering::SeqCst, Ordering::SeqCst)
                    .map(|x| x as u64)
                    .map_err(|x| x as u64),
                4 => AtomicU32::from_ptr(p as *mut u32)
                    .compare_exchange(*expected as u32, desired as u32, Ordering::SeqCst, Ordering::SeqCst)
                    .map(|x| x as u64)
                    .map_err(|x| x as u64),
                _ => AtomicU64::from_ptr(p as *mut u64).compare_exchange(
                    *expected,
                    desired,
                    Ordering::SeqCst,
                    Ordering::SeqCst,
                ),
            };
            match r {
                Ok(x) => {
                    ok = true;
                    *expected = x;
                }
                Err(x) => {
                    ok = false;
                    *expected = x;
                }
            }
        }
        watch_check(gaddr, size, desired, if ok { 0xCA5 } else { 0xFA11 });
        ok
    }
}

unsafe fn ocerz_atomic_load(gaddr: u64, size: i32) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        if unal_misaligned(gaddr, size) {
            let l = unal_lock_for(gaddr);
            unal_lock(l);
            let v = unal_load(p, size);
            unal_unlock(l);
            return v;
        }
        match size {
            1 => AtomicU8::from_ptr(p).load(Ordering::SeqCst) as u64,
            2 => AtomicU16::from_ptr(p as *mut u16).load(Ordering::SeqCst) as u64,
            4 => AtomicU32::from_ptr(p as *mut u32).load(Ordering::SeqCst) as u64,
            _ => AtomicU64::from_ptr(p as *mut u64).load(Ordering::SeqCst),
        }
    }
}

unsafe fn op_arith(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let opc = (*insn).op as u32;
        if (*insn).lock != 0 && (*insn).ops[0].kind as u32 == OCERZ_OPK_MEM {
            let addr = ocerz_ea(cpu, insn, op!(insn, 0));
            let b = ocerz_read_op(cpu, insn, op!(insn, 1));
            let cin = ((*cpu).rflags & OCERZ_CF != 0) as i32;
            loop {
                let a = ocerz_atomic_load(addr, size);
                let res;
                match opc {
                    OCERZ_OP_ADD => { res = a.wrapping_add(b); ocerz_flags_add(cpu, size, a, b, 0, res); }
                    OCERZ_OP_ADC => { res = a.wrapping_add(b).wrapping_add(cin as u64); ocerz_flags_add(cpu, size, a, b, cin, res); }
                    OCERZ_OP_SUB => { res = a.wrapping_sub(b); ocerz_flags_sub(cpu, size, a, b, 0, res); }
                    OCERZ_OP_SBB => { res = a.wrapping_sub(b).wrapping_sub(cin as u64); ocerz_flags_sub(cpu, size, a, b, cin, res); }
                    OCERZ_OP_AND => { res = a & b; ocerz_flags_logic(cpu, size, res); }
                    OCERZ_OP_OR => { res = a | b; ocerz_flags_logic(cpu, size, res); }
                    OCERZ_OP_XOR => { res = a ^ b; ocerz_flags_logic(cpu, size, res); }
                    _ => return ocerz_unimpl(vm, cpu, insn, c"locked-arith".as_ptr()),
                }
                let mut seen = a;
                if ocerz_atomic_cmpxchg(addr, size, &mut seen, res) {
                    break;
                }
            }
            return STEP_OK;
        }
        let a = ocerz_read_op(cpu, insn, op!(insn, 0));
        let b = ocerz_read_op(cpu, insn, op!(insn, 1));
        let cin = ((*cpu).rflags & OCERZ_CF != 0) as i32;
        let res;
        match opc {
            OCERZ_OP_ADD => {
                res = a.wrapping_add(b);
                ocerz_flags_add(cpu, size, a, b, 0, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_ADC => {
                res = a.wrapping_add(b).wrapping_add(cin as u64);
                ocerz_flags_add(cpu, size, a, b, cin, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_SUB => {
                res = a.wrapping_sub(b);
                ocerz_flags_sub(cpu, size, a, b, 0, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_SBB => {
                res = a.wrapping_sub(b).wrapping_sub(cin as u64);
                ocerz_flags_sub(cpu, size, a, b, cin, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_AND => {
                res = a & b;
                ocerz_flags_logic(cpu, size, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_OR => {
                res = a | b;
                ocerz_flags_logic(cpu, size, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_XOR => {
                res = a ^ b;
                ocerz_flags_logic(cpu, size, res);
                ocerz_write_op(cpu, insn, op!(insn, 0), res);
            }
            OCERZ_OP_CMP => {
                res = a.wrapping_sub(b);
                ocerz_flags_sub(cpu, size, a, b, 0, res);
            }
            OCERZ_OP_TEST => {
                res = a & b;
                ocerz_flags_logic(cpu, size, res);
            }
            _ => return ocerz_unimpl(vm, cpu, insn, c"arith".as_ptr()),
        }
        STEP_OK
    }
}

unsafe fn op_incdecnegnot(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let opc = (*insn).op as u32;
        if (*insn).lock != 0 && (*insn).ops[0].kind as u32 == OCERZ_OPK_MEM {
            let addr = ocerz_ea(cpu, insn, op!(insn, 0));
            loop {
                let a = ocerz_atomic_load(addr, size);
                let res;
                match opc {
                    OCERZ_OP_INC => { res = a.wrapping_add(1); ocerz_flags_inc(cpu, size, res); }
                    OCERZ_OP_DEC => { res = a.wrapping_sub(1); ocerz_flags_dec(cpu, size, res); }
                    OCERZ_OP_NEG => { res = 0u64.wrapping_sub(a); ocerz_flags_sub(cpu, size, 0, a, 0, res); }
                    OCERZ_OP_NOT => { res = !a; }
                    _ => return ocerz_unimpl(vm, cpu, insn, c"locked-incdec".as_ptr()),
                }
                let mut seen = a;
                if ocerz_atomic_cmpxchg(addr, size, &mut seen, res) {
                    break;
                }
            }
            return STEP_OK;
        }
        let a = ocerz_read_op(cpu, insn, op!(insn, 0));
        let res;
        match opc {
            OCERZ_OP_INC => {
                res = a.wrapping_add(1);
                ocerz_flags_inc(cpu, size, res);
            }
            OCERZ_OP_DEC => {
                res = a.wrapping_sub(1);
                ocerz_flags_dec(cpu, size, res);
            }
            OCERZ_OP_NEG => {
                res = 0u64.wrapping_sub(a);
                ocerz_flags_sub(cpu, size, 0, a, 0, res);
            }
            OCERZ_OP_NOT => {
                res = !a;
            }
            _ => return ocerz_unimpl(vm, cpu, insn, c"incdec".as_ptr()),
        }
        ocerz_write_op(cpu, insn, op!(insn, 0), res);
        STEP_OK
    }
}

unsafe fn op_mul(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        if (*insn).op as u32 == OCERZ_OP_MUL {
            let src = ocerz_read_op(cpu, insn, op!(insn, 0));
            let acc = read_acc(cpu, size);
            if size == 8 {
                let p = (acc as u128) * (src as u128);
                let lo = p as u64;
                let hi = (p >> 64) as u64;
                (*cpu).gpr[RAX] = lo;
                (*cpu).gpr[RDX] = hi;
                ocerz_flags_mul(cpu, size, lo, hi);
            } else {
                let p = ocerz_trunc(acc, size).wrapping_mul(ocerz_trunc(src, size));
                let lo = ocerz_trunc(p, size);
                let hi = ocerz_trunc(p >> (size * 8), size);
                if size == 1 {
                    ocerz_write_gpr(cpu, RAX as u32, 2, 0, p & 0xffff);
                } else {
                    write_acc(cpu, size, lo);
                    ocerz_write_gpr(cpu, RDX as u32, size, 0, hi);
                }
                ocerz_flags_mul(cpu, size, lo, hi);
            }
            return STEP_OK;
        }

        if (*insn).nops == 1 {
            let src = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 0)), size);
            let acc = ocerz_sext(read_acc(cpu, size), size);
            if size == 8 {
                let p = (acc as i128) * (src as i128);
                let lo = p as u64;
                let hi = (p >> 64) as u64;
                (*cpu).gpr[RAX] = lo;
                (*cpu).gpr[RDX] = hi;
                ocerz_flags_imul(cpu, size, lo, hi);
            } else {
                let p = acc.wrapping_mul(src);
                let lo = ocerz_trunc(p as u64, size);
                let hi = ocerz_trunc((p as u64) >> (size * 8), size);
                if size == 1 {
                    ocerz_write_gpr(cpu, RAX as u32, 2, 0, (p as u64) & 0xffff);
                } else {
                    write_acc(cpu, size, lo);
                    ocerz_write_gpr(cpu, RDX as u32, size, 0, hi);
                }
                ocerz_flags_imul(cpu, size, lo, hi);
            }
            return STEP_OK;
        }

        let (a, b);
        if (*insn).nops == 3 {
            a = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 1)), (*insn).ops[1].size as i32);
            b = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 2)), (*insn).ops[2].size as i32);
        } else {
            a = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 0)), size);
            b = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 1)), size);
        }
        if size == 8 {
            let p = (a as i128) * (b as i128);
            let lo = p as u64;
            let hi = (p >> 64) as u64;
            ocerz_flags_imul(cpu, size, lo, hi);
            ocerz_write_op(cpu, insn, op!(insn, 0), lo);
        } else {
            let p = a.wrapping_mul(b);
            let lo = ocerz_trunc(p as u64, size);
            let hi = ocerz_trunc((p as u64) >> (size * 8), size);
            ocerz_flags_imul(cpu, size, lo, hi);
            ocerz_write_op(cpu, insn, op!(insn, 0), lo);
        }
        STEP_OK
    }
}

unsafe fn op_div(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let divisor = ocerz_read_op(cpu, insn, op!(insn, 0));
        if ocerz_trunc(divisor, size) == 0 {
            return div_trap(cpu, insn, OCERZ_FPE_INTDIV, c"integer divide by zero".as_ptr());
        }
        let ovf = c"divide quotient overflow".as_ptr();

        if (*insn).op as u32 == OCERZ_OP_DIV {
            if size == 1 {
                let num = ocerz_read_gpr(cpu, RAX as u32, 2, 0);
                let d = divisor & 0xff;
                let q = num / d;
                let r = num % d;
                if q > 0xff {
                    return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
                }
                ocerz_write_gpr(cpu, RAX as u32, 1, 0, q);
                ocerz_write_gpr(cpu, RAX as u32, 1, 1, r);
            } else if size == 8 {
                let num = (((*cpu).gpr[RDX] as u128) << 64) | (*cpu).gpr[RAX] as u128;
                let d = divisor as u128;
                let q = num / d;
                let r = num % d;
                if q > u64::MAX as u128 {
                    return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
                }
                (*cpu).gpr[RAX] = q as u64;
                (*cpu).gpr[RDX] = r as u64;
            } else {
                let lo = read_acc(cpu, size);
                let hi = ocerz_read_gpr(cpu, RDX as u32, size, 0);
                let num = (hi << (size * 8)) | lo;
                let d = ocerz_trunc(divisor, size);
                let q = num / d;
                let r = num % d;
                if q > ocerz_mask(size) {
                    return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
                }
                write_acc(cpu, size, q);
                ocerz_write_gpr(cpu, RDX as u32, size, 0, r);
            }
            return STEP_OK;
        }

        if size == 1 {
            let num = ocerz_sext(ocerz_read_gpr(cpu, RAX as u32, 2, 0), 2);
            let d = ocerz_sext(divisor, 1);
            let q = num / d;
            let r = num % d;
            if !(-128..=127).contains(&q) {
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
            }
            ocerz_write_gpr(cpu, RAX as u32, 1, 0, q as u64);
            ocerz_write_gpr(cpu, RAX as u32, 1, 1, r as u64);
        } else if size == 8 {
            let num = ((((*cpu).gpr[RDX] as i64 as i128) << 64) as u128 | (*cpu).gpr[RAX] as u128) as i128;
            let d = divisor as i64 as i128;
            if num == i128::MIN && d == -1 {
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
            }
            let q = num / d;
            let r = num % d;
            if q < i64::MIN as i128 || q > i64::MAX as i128 {
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
            }
            (*cpu).gpr[RAX] = q as u64;
            (*cpu).gpr[RDX] = r as u64;
        } else {
            let lo = read_acc(cpu, size);
            let hi = ocerz_read_gpr(cpu, RDX as u32, size, 0);
            let num = ocerz_sext((hi << (size * 8)) | lo, size * 2);
            let d = ocerz_sext(divisor, size);
            if num == i64::MIN && d == -1 {
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
            }
            let q = num / d;
            let r = num % d;
            let qmin = -1i64 - (ocerz_mask(size) >> 1) as i64;
            let qmax = (ocerz_mask(size) >> 1) as i64;
            if q < qmin || q > qmax {
                return div_trap(cpu, insn, OCERZ_FPE_INTDIV, ovf);
            }
            write_acc(cpu, size, q as u64);
            ocerz_write_gpr(cpu, RDX as u32, size, 0, r as u64);
        }
        STEP_OK
    }
}

unsafe fn op_shift(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let mask: u32 = if size == 8 { 63 } else { 31 };
        let val = ocerz_read_op(cpu, insn, op!(insn, 0));
        let cnt = (ocerz_read_op(cpu, insn, op!(insn, 1)) as u32) & mask;
        if cnt == 0 {
            return STEP_OK;
        }
        let res;
        match (*insn).op as u32 {
            OCERZ_OP_SHL => {
                res = ocerz_trunc(val << cnt, size);
                ocerz_flags_shl(cpu, size, val, cnt, res);
            }
            OCERZ_OP_SHR => {
                res = ocerz_trunc(val, size) >> cnt;
                ocerz_flags_shr(cpu, size, val, cnt, res);
            }
            OCERZ_OP_SAR => {
                res = ocerz_trunc((ocerz_sext(val, size) >> cnt) as u64, size);
                ocerz_flags_sar(cpu, size, val, cnt, res);
            }
            _ => return ocerz_unimpl(vm, cpu, insn, c"shift".as_ptr()),
        }
        ocerz_write_op(cpu, insn, op!(insn, 0), res);
        STEP_OK
    }
}

unsafe fn op_rotate(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let bits = (size * 8) as u32;
        let mask: u32 = if size == 8 { 63 } else { 31 };
        let val = ocerz_trunc(ocerz_read_op(cpu, insn, op!(insn, 0)), size);
        let masked = (ocerz_read_op(cpu, insn, op!(insn, 1)) as u32) & mask;
        let cin = ((*cpu).rflags & OCERZ_CF != 0) as i32;
        let opc = (*insn).op as u32;

        if opc == OCERZ_OP_ROL || opc == OCERZ_OP_ROR {
            let rc = masked % bits;
            if masked == 0 {
                return STEP_OK;
            }
            let res = if rc == 0 {
                val
            } else if opc == OCERZ_OP_ROL {
                ocerz_trunc((val << rc) | (val >> (bits - rc)), size)
            } else {
                ocerz_trunc((val >> rc) | (val << (bits - rc)), size)
            };
            let cf = if opc == OCERZ_OP_ROL { (res & 1) as i32 } else { ocerz_msb(res, size) };
            let mut of = 0;
            if masked == 1 {
                of = if opc == OCERZ_OP_ROL {
                    cf ^ ocerz_msb(res, size)
                } else {
                    ocerz_msb(res, size) ^ ((res >> (bits - 2)) & 1) as i32
                };
            }
            ocerz_flag_assign(cpu, OCERZ_CF, cf);
            if masked == 1 {
                ocerz_flag_assign(cpu, OCERZ_OF, of);
            }
            ocerz_write_op(cpu, insn, op!(insn, 0), res);
            return STEP_OK;
        }

        if masked == 0 {
            return STEP_OK;
        }
        let mut res = val;
        let mut carry = cin;
        for _ in 0..masked {
            if opc == OCERZ_OP_RCL {
                let top = ocerz_msb(res, size);
                res = ocerz_trunc((res << 1) | carry as u64, size);
                carry = top;
            } else {
                let bot = (res & 1) as i32;
                res = ocerz_trunc((res >> 1) | ((carry as u64) << (bits - 1)), size);
                carry = bot;
            }
        }
        let mut of = 0;
        if masked == 1 {
            of = if opc == OCERZ_OP_RCL {
                carry ^ ocerz_msb(res, size)
            } else {
                ocerz_msb(res, size) ^ ((res >> (bits - 2)) & 1) as i32
            };
        }
        ocerz_flag_assign(cpu, OCERZ_CF, carry);
        if masked == 1 {
            ocerz_flag_assign(cpu, OCERZ_OF, of);
        }
        ocerz_write_op(cpu, insn, op!(insn, 0), res);
        STEP_OK
    }
}

unsafe fn op_shiftd(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let size = (*insn).ops[0].size as i32;
        let bits = (size * 8) as u32;
        let mask: u32 = if size == 8 { 63 } else { 31 };
        let dst = ocerz_trunc(ocerz_read_op(cpu, insn, op!(insn, 0)), size);
        let src = ocerz_trunc(ocerz_read_op(cpu, insn, op!(insn, 1)), size);
        let cnt = (ocerz_read_op(cpu, insn, op!(insn, 2)) as u32) & mask;
        if cnt == 0 {
            return STEP_OK;
        }
        let res;
        if (*insn).op as u32 == OCERZ_OP_SHLD {
            let mut big = ((dst as u128) << bits) | src as u128;
            big <<= cnt;
            res = ocerz_trunc((big >> bits) as u64, size);
            ocerz_flags_shl(cpu, size, dst, cnt, res);
        } else {
            let mut big = ((src as u128) << bits) | dst as u128;
            big >>= cnt;
            res = ocerz_trunc(big as u64, size);
            ocerz_flags_shr(cpu, size, dst, cnt, res);
        }
        ocerz_write_op(cpu, insn, op!(insn, 0), res);
        STEP_OK
    }
}

unsafe fn op_mov_family(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        match (*insn).op as u32 {
            OCERZ_OP_MOV => {
                let v = ocerz_read_op(cpu, insn, op!(insn, 1));
                ocerz_write_op(cpu, insn, op!(insn, 0), v);
                STEP_OK
            }
            OCERZ_OP_MOVZX => {
                let v = ocerz_trunc(ocerz_read_op(cpu, insn, op!(insn, 1)), (*insn).ops[1].size as i32);
                ocerz_write_op(cpu, insn, op!(insn, 0), v);
                STEP_OK
            }
            OCERZ_OP_MOVSX | OCERZ_OP_MOVSXD => {
                let v = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 1)), (*insn).ops[1].size as i32) as u64;
                ocerz_write_op(cpu, insn, op!(insn, 0), v);
                STEP_OK
            }
            OCERZ_OP_LEA => {
                let a = lea_addr(cpu, insn, op!(insn, 1));
                ocerz_write_op(cpu, insn, op!(insn, 0), a);
                STEP_OK
            }
            OCERZ_OP_XCHG => {
                if (*insn).ops[0].kind as u32 == OCERZ_OPK_MEM {
                    let b = ocerz_read_op(cpu, insn, op!(insn, 1));
                    let addr = ocerz_ea(cpu, insn, op!(insn, 0));
                    let a = ocerz_atomic_xchg(addr, (*insn).ops[0].size as i32, b);
                    ocerz_write_op(cpu, insn, op!(insn, 1), a);
                    return STEP_OK;
                }
                if (*insn).ops[1].kind as u32 == OCERZ_OPK_MEM {
                    let a = ocerz_read_op(cpu, insn, op!(insn, 0));
                    let addr = ocerz_ea(cpu, insn, op!(insn, 1));
                    let b = ocerz_atomic_xchg(addr, (*insn).ops[1].size as i32, a);
                    ocerz_write_op(cpu, insn, op!(insn, 0), b);
                    return STEP_OK;
                }
                let a = ocerz_read_op(cpu, insn, op!(insn, 0));
                let b = ocerz_read_op(cpu, insn, op!(insn, 1));
                ocerz_write_op(cpu, insn, op!(insn, 0), b);
                ocerz_write_op(cpu, insn, op!(insn, 1), a);
                STEP_OK
            }
            OCERZ_OP_BSWAP => {
                let size = (*insn).ops[0].size as i32;
                let v = ocerz_read_op(cpu, insn, op!(insn, 0));
                let r = if size == 8 { v.swap_bytes() } else { (v as u32).swap_bytes() as u64 };
                ocerz_write_op(cpu, insn, op!(insn, 0), r);
                STEP_OK
            }
            OCERZ_OP_CMOVCC => {
                let src = ocerz_read_op(cpu, insn, op!(insn, 1));
                let take = ocerz_cc_eval(cpu, (*insn).cc as u32);
                let dst = ocerz_read_op(cpu, insn, op!(insn, 0));
                ocerz_write_op(cpu, insn, op!(insn, 0), if take != 0 { src } else { dst });
                STEP_OK
            }
            OCERZ_OP_SETCC => {
                let take = ocerz_cc_eval(cpu, (*insn).cc as u32);
                ocerz_write_op(cpu, insn, op!(insn, 0), (take != 0) as u64);
                STEP_OK
            }
            _ => ocerz_unimpl(vm, cpu, insn, c"mov-family".as_ptr()),
        }
    }
}

#[inline(always)]
fn opsize_or(insn: &X86Insn, dflt: i32) -> i32 {
    if insn.opsize != 0 { insn.opsize as i32 } else { dflt }
}

unsafe fn op_stack(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let m32 = (*insn).mode32 as i32;
        match (*insn).op as u32 {
            OCERZ_OP_PUSH => {
                let size = (*insn).opsize as i32;
                let v = ocerz_read_op(cpu, insn, op!(insn, 0));
                ocerz_push_mode(cpu, size, v, m32);
                STEP_OK
            }
            OCERZ_OP_POP => {
                let size = (*insn).opsize as i32;
                let v = ocerz_ld((*cpu).gpr[RSP], size);
                let d = op!(insn, 0);
                if (*d).kind as u32 == OCERZ_OPK_MEM && ((*d).base == RSP as u8 || (*d).index == RSP as u8) {
                    let old = (*cpu).gpr[RSP];
                    let bumped = ocerz_stack_wrap(old.wrapping_add(size as u64), m32);
                    (*cpu).gpr[RSP] = bumped;
                    let ea = ocerz_ea(cpu, insn, d);
                    (*cpu).gpr[RSP] = old;
                    ocerz_st(ea, size, v);
                    (*cpu).gpr[RSP] = bumped;
                    return STEP_OK;
                }
                ocerz_write_op(cpu, insn, d, v);
                if !((*d).kind as u32 == OCERZ_OPK_REG && (*d).reg == RSP as u8) {
                    (*cpu).gpr[RSP] = ocerz_stack_wrap((*cpu).gpr[RSP].wrapping_add(size as u64), m32);
                }
                STEP_OK
            }
            OCERZ_OP_PUSHF => {
                ocerz_push_mode(cpu, opsize_or(&*insn, 8), (*cpu).rflags, m32);
                STEP_OK
            }
            OCERZ_OP_POPF => {
                let v = ocerz_pop_mode(cpu, opsize_or(&*insn, 8), m32);
                let writable =
                    OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_TF | OCERZ_DF | OCERZ_OF;
                (*cpu).rflags = (v & writable) | OCERZ_FLAG_FIXED1 | OCERZ_IF;
                STEP_OK
            }
            OCERZ_OP_LAHF => {
                let ah = ((*cpu).rflags & (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF)) | OCERZ_FLAG_FIXED1;
                ocerz_write_gpr(cpu, RAX as u32, 1, 1, ah);
                STEP_OK
            }
            OCERZ_OP_SAHF => {
                let ah = ocerz_read_gpr(cpu, RAX as u32, 1, 1);
                let m = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF;
                (*cpu).rflags = ((*cpu).rflags & !m) | (ah & m);
                STEP_OK
            }
            OCERZ_OP_LEAVE => {
                let size = opsize_or(&*insn, 8);
                let bp = if m32 != 0 { (*cpu).gpr[RBP] as u32 as u64 } else { (*cpu).gpr[RBP] };
                let v = ocerz_ld(bp, size);
                (*cpu).gpr[RSP] = ocerz_stack_wrap(bp.wrapping_add(size as u64), m32);
                ocerz_write_gpr(cpu, RBP as u32, size, 0, v);
                STEP_OK
            }
            _ => ocerz_unimpl(vm, cpu, insn, c"stack".as_ptr()),
        }
    }
}

unsafe fn op_cbw_cwd(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let osz = (*insn).opsize;
        if (*insn).op as u32 == OCERZ_OP_CBW {
            if osz == 2 {
                let v = ocerz_sext(ocerz_read_gpr(cpu, RAX as u32, 1, 0), 1);
                ocerz_write_gpr(cpu, RAX as u32, 2, 0, v as u64);
            } else if osz == 4 {
                let v = ocerz_sext(ocerz_read_gpr(cpu, RAX as u32, 2, 0), 2);
                ocerz_write_gpr(cpu, RAX as u32, 4, 0, v as u64);
            } else {
                let v = ocerz_sext(ocerz_read_gpr(cpu, RAX as u32, 4, 0), 4);
                (*cpu).gpr[RAX] = v as u64;
            }
            return STEP_OK;
        }
        if osz == 2 {
            let v = ocerz_msb(ocerz_read_gpr(cpu, RAX as u32, 2, 0), 2);
            ocerz_write_gpr(cpu, RDX as u32, 2, 0, if v != 0 { 0xffff } else { 0 });
        } else if osz == 4 {
            let v = ocerz_msb(ocerz_read_gpr(cpu, RAX as u32, 4, 0), 4);
            ocerz_write_gpr(cpu, RDX as u32, 4, 0, if v != 0 { 0xffffffff } else { 0 });
        } else {
            let v = ocerz_msb((*cpu).gpr[RAX], 8);
            (*cpu).gpr[RDX] = if v != 0 { !0u64 } else { 0 };
        }
        STEP_OK
    }
}

unsafe fn op_branch(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let m32 = (*insn).mode32 as i32;
        match (*insn).op as u32 {
            OCERZ_OP_JMP => {
                if (*insn).ops[0].kind as u32 == OCERZ_OPK_IMM {
                    (*cpu).rip = (*insn).ops[0].imm;
                } else {
                    (*cpu).rip = ocerz_read_op(cpu, insn, op!(insn, 0));
                    if ocerz_cftrap_on != 0 && (*cpu).rip.wrapping_sub(0x7ff840000000) < 0x10000000 {
                        ocerz_cftrap(cpu, (*insn).rip, (*cpu).rip, c"jmp".as_ptr());
                    }
                }
                STEP_OK
            }
            OCERZ_OP_JCC => {
                if ocerz_cc_eval(cpu, (*insn).cc as u32) != 0 {
                    (*cpu).rip = (*insn).ops[0].imm;
                }
                STEP_OK
            }
            OCERZ_OP_JRCXZ => {
                let mut c = (*cpu).gpr[RCX];
                if (*insn).addrsize == 4 {
                    c = c as u32 as u64;
                } else if (*insn).addrsize == 2 {
                    c = c as u16 as u64;
                }
                if c == 0 {
                    (*cpu).rip = (*insn).ops[0].imm;
                }
                STEP_OK
            }
            opc @ (OCERZ_OP_LOOP | OCERZ_OP_LOOPE | OCERZ_OP_LOOPNE) => {
                let c;
                if (*insn).addrsize == 4 {
                    let t = ((*cpu).gpr[RCX] as u32).wrapping_sub(1) as u64;
                    ocerz_write_gpr(cpu, RCX as u32, 4, 0, t);
                    c = t as u32 as u64;
                } else if (*insn).addrsize == 2 {
                    c = ((*cpu).gpr[RCX] as u16).wrapping_sub(1) as u64;
                    ocerz_write_gpr(cpu, RCX as u32, 2, 0, c);
                } else {
                    c = (*cpu).gpr[RCX].wrapping_sub(1);
                    (*cpu).gpr[RCX] = c;
                }
                let zf = (*cpu).rflags & OCERZ_ZF != 0;
                let mut branch = c != 0;
                if opc == OCERZ_OP_LOOPE {
                    branch = branch && zf;
                } else if opc == OCERZ_OP_LOOPNE {
                    branch = branch && !zf;
                }
                if branch {
                    (*cpu).rip = (*insn).ops[0].imm;
                }
                STEP_OK
            }
            OCERZ_OP_CALL => {
                let target;
                if (*insn).ops[0].kind as u32 == OCERZ_OPK_IMM {
                    target = (*insn).ops[0].imm;
                } else {
                    target = ocerz_read_op(cpu, insn, op!(insn, 0));
                    if ocerz_cftrap_on != 0 && target.wrapping_sub(0x7ff840000000) < 0x10000000 {
                        ocerz_cftrap(cpu, (*insn).rip, target, c"call".as_ptr());
                    }
                }
                ocerz_push_mode(cpu, opsize_or(&*insn, 8), (*cpu).rip, m32);
                (*cpu).rip = target;
                STEP_OK
            }
            OCERZ_OP_RET => {
                let size = opsize_or(&*insn, 8);
                let ret = ocerz_pop_mode(cpu, size, m32);
                if (*insn).nops == 1 {
                    (*cpu).gpr[RSP] =
                        ocerz_stack_wrap((*cpu).gpr[RSP].wrapping_add(ocerz_trunc((*insn).ops[0].imm, 2)), m32);
                }
                (*cpu).rip = ret;
                STEP_OK
            }
            OCERZ_OP_MOVSEG => {
                let sel = ocerz_read_op(cpu, insn, op!(insn, 0)) as u16 as u32;
                let seg = (*insn).ops[1].imm as u32;
                let base = ocerz_ldt_base(sel);
                if base != 0 {
                    if seg == 4 {
                        (*cpu).fs_base = base;
                    } else if seg == 5 {
                        (*cpu).gs_base = base;
                    }
                }
                if seg < 6 {
                    (*cpu).seg_sel[seg as usize] = sel as u16;
                }
                if seg == 1 {
                    (*cpu).cs_sel = sel as u16;
                }
                STEP_OK
            }
            OCERZ_OP_MOVFROMSEG => {
                let seg = (*insn).ops[1].imm as u32;
                let sel = if seg == OCERZ_SREG_CS {
                    (*cpu).cs_sel as u64
                } else if seg < 6 {
                    (*cpu).seg_sel[seg as usize] as u64
                } else {
                    0
                };
                ocerz_write_op(cpu, insn, op!(insn, 0), sel);
                STEP_OK
            }
            opc @ (OCERZ_OP_JMPF | OCERZ_OP_CALLF) => {
                let sz = opsize_or(&*insn, 4);
                let ssz = if m32 != 0 { sz } else { 8 };
                let off;
                let sel;
                if (*insn).nops == 2 {
                    sel = (*insn).ops[0].imm as u16 as u32;
                    off = ocerz_trunc((*insn).ops[1].imm, sz);
                } else {
                    let ea = ocerz_ea(cpu, insn, op!(insn, 0));
                    off = ocerz_ld(ea, sz);
                    sel = ocerz_ld(ea.wrapping_add(sz as u64), 2) as u32;
                }
                if opc == OCERZ_OP_CALLF {
                    ocerz_push_mode(cpu, ssz, (*cpu).cs_sel as u64, m32);
                    ocerz_push_mode(cpu, ssz, (*insn).rip.wrapping_add((*insn).len as u64), m32);
                }
                far_transfer(cpu, sel, off)
            }
            OCERZ_OP_RETF => {
                let ssz = if m32 != 0 { opsize_or(&*insn, 4) } else { 8 };
                let off = ocerz_pop_mode(cpu, ssz, m32);
                let sel = ocerz_pop_mode(cpu, ssz, m32) as u16 as u32;
                if (*insn).nops == 1 {
                    (*cpu).gpr[RSP] =
                        ocerz_stack_wrap((*cpu).gpr[RSP].wrapping_add(ocerz_trunc((*insn).ops[0].imm, 2)), m32);
                }
                far_transfer(cpu, sel, off)
            }
            OCERZ_OP_IRET => {
                let sz = opsize_or(&*insn, 8);
                let sp = (*cpu).gpr[RSP];
                let szu = sz as u64;
                let rip = ocerz_ld(sp, sz);
                let cs = ocerz_ld(sp.wrapping_add(szu), sz) as u16 as u32;
                let flags = ocerz_ld(sp.wrapping_add(szu * 2), sz);
                let newsp = ocerz_ld(sp.wrapping_add(szu * 3), sz);
                let ss = ocerz_ld(sp.wrapping_add(szu * 4), sz) as u16 as u32;
                (*cpu).rflags = flags | 0x2;
                (*cpu).gpr[RSP] = ocerz_stack_wrap(newsp, m32);
                if ss != 0 {
                    (*cpu).seg_sel[OCERZ_SREG_SS as usize] = ss as u16;
                }
                if cs == 0 {
                    (*cpu).rip = if m32 != 0 { rip as u32 as u64 } else { rip };
                    return STEP_OK;
                }
                far_transfer(cpu, cs, rip)
            }
            _ => ocerz_unimpl(vm, cpu, insn, c"branch".as_ptr()),
        }
    }
}

unsafe fn cs_is_32bit(sel: u32) -> bool {
    unsafe {
        if ocerz_ldt_is_long(sel) != 0 {
            return false;
        }
        ocerz_ldt_is_big(sel) != 0
    }
}

static MODELOG: AtomicI32 = AtomicI32::new(-1);

unsafe fn far_transfer(cpu: *mut OcerzCPU, sel: u32, off: u64) -> i32 {
    unsafe {
        let to32 = cs_is_32bit(sel);
        (*cpu).cs_sel = sel as u16;
        (*cpu).seg_sel[OCERZ_SREG_CS as usize] = sel as u16;
        (*cpu).mode32 = to32 as u8;
        (*cpu).rip = if to32 { off as u32 as u64 } else { off };
        let mut modelog = MODELOG.load(Ordering::Relaxed);
        if modelog < 0 {
            modelog = (!libc::getenv(c"OCERZ_MODELOG".as_ptr()).is_null()) as i32;
            MODELOG.store(modelog, Ordering::Relaxed);
        }
        if modelog != 0 {
            libc::fprintf(
                stderr(),
                c"ocerz: far transfer cs=%#x -> %s eip=%#llx\n".as_ptr(),
                sel as c_uint,
                if to32 { c"i386".as_ptr() } else { c"long".as_ptr() },
                (*cpu).rip as libc::c_ulonglong,
            );
        }
    }
    STEP_OK
}

#[inline(always)]
unsafe fn get_al(cpu: *const OcerzCPU) -> u8 {
    unsafe { (*cpu).gpr[RAX] as u8 }
}
#[inline(always)]
unsafe fn get_ah(cpu: *const OcerzCPU) -> u8 {
    unsafe { ((*cpu).gpr[RAX] >> 8) as u8 }
}
#[inline(always)]
unsafe fn get_ax(cpu: *const OcerzCPU) -> u16 {
    unsafe { (*cpu).gpr[RAX] as u16 }
}
#[inline(always)]
unsafe fn set_al(cpu: *mut OcerzCPU, v: u8) {
    unsafe { ocerz_write_gpr(cpu, RAX as u32, 1, 0, v as u64) }
}
#[inline(always)]
unsafe fn set_ah(cpu: *mut OcerzCPU, v: u8) {
    unsafe { ocerz_write_gpr(cpu, RAX as u32, 1, 1, v as u64) }
}
#[inline(always)]
unsafe fn set_ax(cpu: *mut OcerzCPU, v: u16) {
    unsafe { ocerz_write_gpr(cpu, RAX as u32, 2, 0, v as u64) }
}

#[cold]
unsafe fn i386_trap(cpu: *mut OcerzCPU, insn: *const X86Insn, msg: *const c_char) -> i32 {
    unsafe {
        let next = (*cpu).rip;
        (*cpu).rip = (*insn).rip;
        if ocerz_signal_deliver(cpu, OCERZ_SIGTRAP as _, (*insn).rip, 0, 0) != 0 {
            return STEP_REDIRECT;
        }
        (*cpu).rip = next;
        trap_fatal(insn, msg)
    }
}

unsafe fn op_pusha(cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    const ORDER: [u8; 8] = [
        OCERZ_RAX as u8, OCERZ_RCX as u8, OCERZ_RDX as u8, OCERZ_RBX as u8,
        OCERZ_RSP as u8, OCERZ_RBP as u8, OCERZ_RSI as u8, OCERZ_RDI as u8,
    ];
    unsafe {
        let size = opsize_or(&*insn, 4);
        let saved_sp = (*cpu).gpr[RSP];
        for &r in ORDER.iter() {
            let v = if r as usize == RSP { saved_sp } else { (*cpu).gpr[r as usize] };
            ocerz_push_mode(cpu, size, v, (*insn).mode32 as i32);
        }
    }
    STEP_OK
}

unsafe fn op_popa(cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    const ORDER: [u8; 8] = [
        OCERZ_RDI as u8, OCERZ_RSI as u8, OCERZ_RBP as u8, OCERZ_RSP as u8,
        OCERZ_RBX as u8, OCERZ_RDX as u8, OCERZ_RCX as u8, OCERZ_RAX as u8,
    ];
    unsafe {
        let size = opsize_or(&*insn, 4);
        for &r in ORDER.iter() {
            let v = ocerz_pop_mode(cpu, size, (*insn).mode32 as i32);
            if r as usize == RSP {
                continue;
            }
            ocerz_write_gpr(cpu, r as u32, size, 0, v);
        }
    }
    STEP_OK
}

unsafe fn op_i386(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        match (*insn).op as u32 {
            OCERZ_OP_PUSHA => op_pusha(cpu, insn),
            OCERZ_OP_POPA => op_popa(cpu, insn),
            OCERZ_OP_PUSHSEG => {
                let seg = (*insn).ops[0].imm as u32;
                let size = opsize_or(&*insn, 4);
                let v = if seg < 6 { (*cpu).seg_sel[seg as usize] as u64 } else { 0 };
                ocerz_push_mode(cpu, size, v, (*insn).mode32 as i32);
                STEP_OK
            }
            OCERZ_OP_POPSEG => {
                let seg = (*insn).ops[0].imm as u32;
                let size = opsize_or(&*insn, 4);
                let sel = ocerz_pop_mode(cpu, size, (*insn).mode32 as i32) as u16 as u32;
                if seg < 6 {
                    (*cpu).seg_sel[seg as usize] = sel as u16;
                }
                let base = ocerz_ldt_base(sel);
                if base != 0 {
                    if seg == OCERZ_SREG_FS {
                        (*cpu).fs_base = base;
                    } else if seg == OCERZ_SREG_GS {
                        (*cpu).gs_base = base;
                    }
                }
                STEP_OK
            }
            OCERZ_OP_DAA => {
                let old_al = get_al(cpu);
                let old_cf = (*cpu).rflags & OCERZ_CF != 0;
                let mut al = old_al;
                let carry;
                ocerz_flag_assign(cpu, OCERZ_CF, 0);
                if (al & 0x0f) > 9 || (*cpu).rflags & OCERZ_AF != 0 {
                    let t = al as u32 + 6;
                    al = t as u8;
                    carry = old_cf || t > 0xff;
                    ocerz_flag_assign(cpu, OCERZ_AF, 1);
                } else {
                    carry = false;
                    ocerz_flag_assign(cpu, OCERZ_AF, 0);
                }
                ocerz_flag_assign(cpu, OCERZ_CF, carry as i32);
                if old_al > 0x99 || old_cf {
                    al = al.wrapping_add(0x60);
                    ocerz_flag_assign(cpu, OCERZ_CF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_CF, 0);
                }
                set_al(cpu, al);
                ocerz_flags_szp(cpu, 1, al as u64);
                STEP_OK
            }
            OCERZ_OP_DAS => {
                let old_al = get_al(cpu);
                let old_cf = (*cpu).rflags & OCERZ_CF != 0;
                let mut al = old_al;
                ocerz_flag_assign(cpu, OCERZ_CF, 0);
                if (al & 0x0f) > 9 || (*cpu).rflags & OCERZ_AF != 0 {
                    let borrow = al < 6;
                    al = al.wrapping_sub(6);
                    ocerz_flag_assign(cpu, OCERZ_CF, (old_cf || borrow) as i32);
                    ocerz_flag_assign(cpu, OCERZ_AF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_AF, 0);
                }
                if old_al > 0x99 || old_cf {
                    al = al.wrapping_sub(0x60);
                    ocerz_flag_assign(cpu, OCERZ_CF, 1);
                }
                set_al(cpu, al);
                ocerz_flags_szp(cpu, 1, al as u64);
                STEP_OK
            }
            OCERZ_OP_AAA => {
                if (get_al(cpu) & 0x0f) > 9 || (*cpu).rflags & OCERZ_AF != 0 {
                    set_ax(cpu, get_ax(cpu).wrapping_add(0x106));
                    ocerz_flag_assign(cpu, OCERZ_AF, 1);
                    ocerz_flag_assign(cpu, OCERZ_CF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_AF, 0);
                    ocerz_flag_assign(cpu, OCERZ_CF, 0);
                }
                set_al(cpu, get_al(cpu) & 0x0f);
                STEP_OK
            }
            OCERZ_OP_AAS => {
                if (get_al(cpu) & 0x0f) > 9 || (*cpu).rflags & OCERZ_AF != 0 {
                    set_ax(cpu, get_ax(cpu).wrapping_sub(6));
                    set_ah(cpu, get_ah(cpu).wrapping_sub(1));
                    ocerz_flag_assign(cpu, OCERZ_AF, 1);
                    ocerz_flag_assign(cpu, OCERZ_CF, 1);
                } else {
                    ocerz_flag_assign(cpu, OCERZ_AF, 0);
                    ocerz_flag_assign(cpu, OCERZ_CF, 0);
                }
                set_al(cpu, get_al(cpu) & 0x0f);
                STEP_OK
            }
            OCERZ_OP_AAM => {
                let base = ((*insn).ops[0].imm & 0xff) as u32;
                if base == 0 {
                    return div_trap(cpu, insn, OCERZ_FPE_INTDIV, c"AAM with base 0".as_ptr());
                }
                let al = get_al(cpu) as u32;
                set_ah(cpu, (al / base) as u8);
                set_al(cpu, (al % base) as u8);
                ocerz_flags_szp(cpu, 1, get_al(cpu) as u64);
                STEP_OK
            }
            OCERZ_OP_AAD => {
                let base = ((*insn).ops[0].imm & 0xff) as u32;
                let al = (get_al(cpu) as u32).wrapping_add((get_ah(cpu) as u32).wrapping_mul(base)) as u8;
                set_ax(cpu, al as u16);
                ocerz_flags_szp(cpu, 1, al as u64);
                STEP_OK
            }
            OCERZ_OP_SALC => {
                set_al(cpu, if (*cpu).rflags & OCERZ_CF != 0 { 0xff } else { 0x00 });
                STEP_OK
            }
            OCERZ_OP_BOUND => {
                let size = opsize_or(&*insn, 4);
                let ea = ocerz_ea(cpu, insn, op!(insn, 1));
                let idx = ocerz_sext(ocerz_read_op(cpu, insn, op!(insn, 0)), size);
                let lo = ocerz_sext(ocerz_ld(ea, size), size);
                let hi = ocerz_sext(ocerz_ld(ea.wrapping_add(size as u64), size), size);
                if idx < lo || idx > hi {
                    return i386_trap(cpu, insn, c"BOUND range exceeded (#BR)".as_ptr());
                }
                STEP_OK
            }
            OCERZ_OP_INTO => {
                if (*cpu).rflags & OCERZ_OF != 0 {
                    return i386_trap(cpu, insn, c"INTO with OF set (#OF)".as_ptr());
                }
                STEP_OK
            }
            opc @ (OCERZ_OP_LES | OCERZ_OP_LDS) => {
                let size = opsize_or(&*insn, 4);
                let ea = ocerz_ea(cpu, insn, op!(insn, 1));
                let off = ocerz_ld(ea, size);
                let sel = ocerz_ld(ea.wrapping_add(size as u64), 2) as u32;
                let seg = if opc == OCERZ_OP_LES { OCERZ_SREG_ES } else { OCERZ_SREG_DS };
                ocerz_write_gpr(cpu, (*insn).ops[0].reg as u32, size, 0, off);
                (*cpu).seg_sel[seg as usize] = sel as u16;
                STEP_OK
            }
            _ => ocerz_unimpl(vm, cpu, insn, c"i386-only".as_ptr()),
        }
    }
}

#[inline(always)]
unsafe fn cas128(p: *mut u8, expected: &mut u128, store: u128) -> bool {
    let mut lo = *expected as u64;
    let mut hi = (*expected >> 64) as u64;
    let (elo, ehi) = (lo, hi);
    unsafe {
        core::arch::asm!(
            "caspal x0, x1, x2, x3, [{p}]",
            p = in(reg) p,
            inout("x0") lo,
            inout("x1") hi,
            in("x2") store as u64,
            in("x3") (store >> 64) as u64,
            options(nostack, preserves_flags),
        );
    }
    *expected = ((hi as u128) << 64) | lo as u128;
    lo == elo && hi == ehi
}

#[cold]
unsafe fn cas16_log(cpu: *mut OcerzCPU, insn: *const X86Insn, addr: u64) {
    static ONCE16: AtomicBool = AtomicBool::new(false);
    unsafe {
        let ull = |x: u64| x as libc::c_ulonglong;
        libc::fprintf(
            stderr(),
            c"ocerz: CMPXCHG16B MISALIGNED addr=%#llx rip=%#llx\n".as_ptr(),
            ull(addr),
            ull((*insn).rip),
        );
        if !ONCE16.load(Ordering::Relaxed) {
            ONCE16.store(true, Ordering::Relaxed);
            let g = &(*cpu).gpr;
            libc::fprintf(
                stderr(),
                c"ocerz: CAS16DUMP rax=%#llx rdx=%#llx rbx=%#llx rcx=%#llx r9=%#llx r10=%#llx r11=%#llx rsi=%#llx rsp=%#llx rbp=%#llx\n".as_ptr(),
                ull(g[RAX]), ull(g[RDX]), ull(g[OCERZ_RBX as usize]), ull(g[RCX]),
                ull(g[OCERZ_R9 as usize]), ull(g[R10]), ull(g[R11]), ull(g[RSI]),
                ull(g[RSP]), ull(g[RBP]),
            );
            let base = addr & !15u64;
            for i in 0..=4u64 {
                let a = base + i * 8;
                if ocerz_addr_committed(a) == 1 {
                    libc::fprintf(stderr(), c"  mem[%#llx]=%#llx\n".as_ptr(), ull(a), ull(ocerz_ld(a, 8)));
                } else {
                    libc::fprintf(stderr(), c"  mem[%#llx]=<unc>\n".as_ptr(), ull(a));
                }
            }
            let mut fp = g[RBP];
            libc::fprintf(stderr(), c"  bt:".as_ptr());
            let mut d = 0;
            while d < 16 && fp > 0x300000000 {
                if ocerz_addr_committed(fp + 8) != 1 {
                    break;
                }
                libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(ocerz_ld(fp + 8, 8)));
                if ocerz_addr_committed(fp) != 1 {
                    break;
                }
                let nf = ocerz_ld(fp, 8);
                if nf <= fp {
                    break;
                }
                fp = nf;
                d += 1;
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }
    }
}

unsafe fn op_atomic(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let mem = (*insn).ops[0].kind as u32 == OCERZ_OPK_MEM;
        match (*insn).op as u32 {
            OCERZ_OP_XADD => {
                let size = (*insn).ops[0].size as i32;
                let src = ocerz_read_op(cpu, insn, op!(insn, 1));
                if mem {
                    let addr = ocerz_ea(cpu, insn, op!(insn, 0));
                    let dst = ocerz_atomic_fetch_add(addr, size, src);
                    let sum = dst.wrapping_add(src);
                    ocerz_flags_add(cpu, size, dst, src, 0, sum);
                    ocerz_write_op(cpu, insn, op!(insn, 1), dst);
                    return STEP_OK;
                }
                let dst = ocerz_read_op(cpu, insn, op!(insn, 0));
                let sum = dst.wrapping_add(src);
                ocerz_flags_add(cpu, size, dst, src, 0, sum);
                ocerz_write_op(cpu, insn, op!(insn, 1), dst);
                ocerz_write_op(cpu, insn, op!(insn, 0), sum);
                STEP_OK
            }
            OCERZ_OP_CMPXCHG => {
                let size = (*insn).ops[0].size as i32;
                let acc = read_acc(cpu, size);
                if mem {
                    let addr = ocerz_ea(cpu, insn, op!(insn, 0));
                    let src = ocerz_read_op(cpu, insn, op!(insn, 1));
                    let mut seen = acc;
                    let ok = ocerz_atomic_cmpxchg(addr, size, &mut seen, src);
                    ocerz_flags_sub(cpu, size, seen, acc, 0, seen.wrapping_sub(acc));
                    write_acc(cpu, size, if ok { acc } else { seen });
                    return STEP_OK;
                }
                let d = ocerz_read_op(cpu, insn, op!(insn, 0));
                ocerz_flags_sub(cpu, size, d, acc, 0, d.wrapping_sub(acc));
                if ocerz_trunc(acc, size) == ocerz_trunc(d, size) {
                    let src = ocerz_read_op(cpu, insn, op!(insn, 1));
                    ocerz_write_op(cpu, insn, op!(insn, 0), src);
                    write_acc(cpu, size, acc);
                } else {
                    write_acc(cpu, size, d);
                }
                STEP_OK
            }
            OCERZ_OP_CMPXCHGXB => {
                let addr = ocerz_ea(cpu, insn, op!(insn, 0));
                let g = &mut (*cpu).gpr;
                if (*insn).opsize == 8 {
                    let expected = ((g[RDX] as u32 as u64) << 32) | g[RAX] as u32 as u64;
                    let store = ((g[RCX] as u32 as u64) << 32) | g[OCERZ_RBX as usize] as u32 as u64;
                    let mut seen = expected;
                    let ok = ocerz_atomic_cmpxchg(addr, 8, &mut seen, store);
                    if ok {
                        ocerz_flag_assign(cpu, OCERZ_ZF, 1);
                    } else {
                        ocerz_write_gpr(cpu, RAX as u32, 4, 0, seen as u32 as u64);
                        ocerz_write_gpr(cpu, RDX as u32, 4, 0, (seen >> 32) as u32 as u64);
                        ocerz_flag_assign(cpu, OCERZ_ZF, 0);
                    }
                } else {
                    if addr & 15 != 0 && !libc::getenv(c"OCERZ_CASLOG".as_ptr()).is_null() {
                        cas16_log(cpu, insn, addr);
                    }
                    let g = &mut (*cpu).gpr;
                    let expected = ((g[RDX] as u128) << 64) | g[RAX] as u128;
                    let store = ((g[RCX] as u128) << 64) | g[OCERZ_RBX as usize] as u128;
                    let mut e = expected;
                    let ok = cas128(ocerz_g2h(addr), &mut e, store);
                    if ok {
                        if ocerz_watch_addr != 0 && ocerz_watch_addr.wrapping_sub(addr) < 16 {
                            ocerz_watch_hit(addr, 16, store as u64, (store >> 64) as u64);
                        }
                        ocerz_flag_assign(cpu, OCERZ_ZF, 1);
                    } else {
                        (*cpu).gpr[RAX] = e as u64;
                        (*cpu).gpr[RDX] = (e >> 64) as u64;
                        ocerz_flag_assign(cpu, OCERZ_ZF, 0);
                    }
                }
                STEP_OK
            }
            _ => ocerz_unimpl(vm, cpu, insn, c"atomic".as_ptr()),
        }
    }
}

unsafe fn op_flagctl(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        match (*insn).op as u32 {
            OCERZ_OP_CLC => ocerz_flag_assign(cpu, OCERZ_CF, 0),
            OCERZ_OP_STC => ocerz_flag_assign(cpu, OCERZ_CF, 1),
            OCERZ_OP_CMC => (*cpu).rflags ^= OCERZ_CF,
            OCERZ_OP_CLD => ocerz_flag_assign(cpu, OCERZ_DF, 0),
            OCERZ_OP_STD => ocerz_flag_assign(cpu, OCERZ_DF, 1),
            _ => return ocerz_unimpl(vm, cpu, insn, c"flagctl".as_ptr()),
        }
    }
    STEP_OK
}

unsafe fn ocerz_exc_read_cfstr(slot: u64, out: *mut c_char, cap: usize) -> *const c_char {
    unsafe {
        if ocerz_addr_readable(slot) == 0 {
            return core::ptr::null();
        }
        let obj = ocerz_ld(slot, 8);
        if obj == 0 || (obj & 7) != 0 || ocerz_addr_readable(obj + 0x18) == 0 {
            return core::ptr::null();
        }
        let cstr = ocerz_ld(obj + 0x10, 8);
        let len = ocerz_ld(obj + 0x18, 8);
        if cstr != 0
            && len != 0
            && len < cap as u64
            && ocerz_addr_readable(cstr) != 0
            && ocerz_addr_readable(cstr.wrapping_add(len)) != 0
        {
            let mut i = 0u64;
            while i < len {
                let c = ocerz_ld(cstr + i, 1);
                if !(0x20..=0x7e).contains(&c) {
                    break;
                }
                *out.add(i as usize) = c as c_char;
                i += 1;
            }
            if i == len {
                *out.add(len as usize) = 0;
                return out;
            }
        }
        if ocerz_addr_readable(obj + 0x11) != 0 {
            let ilen = ocerz_ld(obj + 0x10, 1);
            if ilen != 0 && ilen < cap as u64 && ocerz_addr_readable(obj + 0x11 + ilen) != 0 {
                let mut i = 0u64;
                while i < ilen {
                    let c = ocerz_ld(obj + 0x11 + i, 1);
                    if !(0x20..=0x7e).contains(&c) {
                        break;
                    }
                    *out.add(i as usize) = c as c_char;
                    i += 1;
                }
                if i == ilen {
                    *out.add(ilen as usize) = 0;
                    return out;
                }
            }
        }
        core::ptr::null()
    }
}

static EXCLOG: AtomicI32 = AtomicI32::new(-1);
static mut RTR: [u64; 8] = [0; 8];
static mut NRTR: i32 = -1;
static mut RTR_LEFT: libc::c_long = -1;
static mut TRAPS: [u64; 8] = [0; 8];
static mut NTRAPS: i32 = -1;
static STEPLOG: AtomicI32 = AtomicI32::new(-1);

unsafe extern "C" {
    static mut ocerz_cxa_throw_rip: u64;
}

#[inline(always)]
fn ull(x: u64) -> libc::c_ulonglong {
    x as libc::c_ulonglong
}

#[inline(always)]
unsafe fn rd8(a: u64) -> u64 {
    unsafe { if ocerz_addr_readable(a) != 0 { ocerz_ld(a, 8) } else { 0 } }
}

unsafe fn parse_list(e: *const c_char, out: *mut u64) -> i32 {
    unsafe {
        let mut n = 0;
        let mut e = e;
        while !e.is_null() && *e != 0 && n < 8 {
            *out.add(n as usize) = libc::strtoull(e, core::ptr::null_mut(), 0);
            n += 1;
            e = libc::strchr(e, 44);
            if !e.is_null() {
                e = e.add(1);
            }
        }
        n
    }
}

#[cold]
#[inline(never)]
unsafe fn exclog_init() -> i32 {
    unsafe {
        let x = (!libc::getenv(c"OCERZ_EXCLOG".as_ptr()).is_null()) as i32;
        EXCLOG.store(x, Ordering::Relaxed);
        if x != 0 {
            ocerz_exc_trap_rip = ocerz_dyld_resolve_guest_sym(c"_objc_exception_throw".as_ptr());
            ocerz_cxa_throw_rip = ocerz_dyld_resolve_guest_sym(c"___cxa_throw".as_ptr());
            libc::fprintf(
                stderr(),
                c"ocerz: EXCLOG _objc_exception_throw=%#llx ___cxa_throw=%#llx\n".as_ptr(),
                ull(ocerz_exc_trap_rip),
                ull(ocerz_cxa_throw_rip),
            );
        }
        x
    }
}

#[cold]
#[inline(never)]
unsafe fn cxa_throw_log(cpu: *mut OcerzCPU) {
    unsafe {
        let g = &(*cpu).gpr;
        let obj = g[7];
        let tinfo = g[6];
        let mut tn = [0 as c_char; 200];
        if ocerz_addr_readable(tinfo + 8) != 0 {
            let np = ocerz_ld(tinfo + 8, 8);
            let mut k = 0usize;
            while k < 199 && ocerz_addr_readable(np.wrapping_add(k as u64)) != 0 {
                let c = ocerz_ld(np.wrapping_add(k as u64), 1);
                if c == 0 {
                    break;
                }
                tn[k] = c as c_char;
                tn[k + 1] = 0;
                k += 1;
            }
        }
        let sp = g[4];
        let mut fp = g[5];
        let pid = libc::getpid();
        libc::fprintf(
            stderr(),
            c"ocerz: CXATHROW[%d] obj=%#llx type=\"%s\" ret=%#llx chain:".as_ptr(),
            pid,
            ull(obj),
            tn.as_ptr(),
            ull(rd8(sp)),
        );
        let mut d = 0;
        while d < 10 && fp > 0x1000 && ocerz_addr_readable(fp + 8) != 0 {
            libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(ocerz_ld(fp + 8, 8)));
            let nf = rd8(fp);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(stderr(), c" obj-words:".as_ptr());
        for w in 0..4u64 {
            libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(rd8(obj.wrapping_add(8 * w))));
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
        let nsexc = rd8(obj);
        if nsexc != 0 && ocerz_addr_readable(nsexc + 16) != 0 {
            let mut nb = [0 as c_char; 192];
            let nm = ocerz_exc_read_cfstr(nsexc + 8, nb.as_mut_ptr(), nb.len());
            let reason = ocerz_ld(nsexc + 16, 8);
            libc::fprintf(
                stderr(),
                c"ocerz: CXATHROW-NS[%d] nsexc=%#llx name=%s reason_obj=%#llx\n".as_ptr(),
                pid,
                ull(nsexc),
                if nm.is_null() { c"?".as_ptr() } else { nm },
                ull(reason),
            );
            let mut tg = [reason, 0, 0, 0, 0];
            let mut nt = 1usize;
            let mut w = 1u64;
            while w < 5 && nt < 5 {
                let q = rd8(reason.wrapping_add(8 * w));
                if q > 0x1000 && ocerz_addr_readable(q) != 0 {
                    tg[nt] = q;
                    nt += 1;
                }
                w += 1;
            }
            for &t in tg.iter().take(nt) {
                libc::fprintf(stderr(), c"ocerz: CXATHROW-HEX[%d] %#llx:".as_ptr(), pid, ull(t));
                let mut k = 0u64;
                while k < 160 && ocerz_addr_readable(t.wrapping_add(k)) != 0 {
                    libc::fprintf(stderr(), c"%02x".as_ptr(), ocerz_ld(t.wrapping_add(k), 1) as c_uint);
                    k += 1;
                }
                libc::fprintf(stderr(), c"\n".as_ptr());
            }
        }
        libc::fflush(stderr());
    }
}

#[cold]
#[inline(never)]
unsafe fn exc_throw_log(cpu: *mut OcerzCPU) {
    unsafe {
        let exc = (*cpu).gpr[7];
        let mut nbuf = [0 as c_char; 192];
        let mut rbuf = [0 as c_char; 384];
        let nm = ocerz_exc_read_cfstr(exc + 8, nbuf.as_mut_ptr(), nbuf.len());
        let rs = ocerz_exc_read_cfstr(exc + 16, rbuf.as_mut_ptr(), rbuf.len());
        libc::fprintf(
            stderr(),
            c"ocerz: EXCTHROW exc=%#llx name=%s reason=%s\n".as_ptr(),
            ull(exc),
            if nm.is_null() { c"<unreadable>".as_ptr() } else { nm },
            if rs.is_null() { c"<unreadable>".as_ptr() } else { rs },
        );
        if nm.is_null() || rs.is_null() {
            for w in 0..3u64 {
                let slot = exc + 8 + w * 8;
                if ocerz_addr_readable(slot) == 0 {
                    continue;
                }
                let o = ocerz_ld(slot, 8);
                libc::fprintf(stderr(), c"ocerz: EXCTHROW   ivar+%d=%#llx".as_ptr(), (8 + w * 8) as c_int, ull(o));
                if o != 0 && (o & 7) == 0 && ocerz_addr_readable(o + 0x20) != 0 {
                    for q in 0..4u64 {
                        libc::fprintf(stderr(), c" [%d]=%#llx".as_ptr(), (q * 8) as c_int, ull(ocerz_ld(o + q * 8, 8)));
                    }
                }
                libc::fprintf(stderr(), c"\n".as_ptr());
            }
        }
        libc::fflush(stderr());
    }
}

#[cold]
#[inline(never)]
unsafe fn regtrap_init() {
    unsafe {
        let e = libc::getenv(c"OCERZ_REGTRAP".as_ptr());
        let mx = libc::getenv(c"OCERZ_REGTRAP_MAX".as_ptr());
        RTR_LEFT = if mx.is_null() { -1 } else { libc::strtol(mx, core::ptr::null_mut(), 0) };
        NRTR = 0;
        NRTR = parse_list(e, (&raw mut RTR) as *mut u64);
    }
}

#[cold]
#[inline(never)]
unsafe fn regtrap_hit(cpu: *mut OcerzCPU) {
    const RN: [&core::ffi::CStr; 16] = [
        c"rax", c"rcx", c"rdx", c"rbx", c"rsp", c"rbp", c"rsi", c"rdi", c"r8", c"r9", c"r10", c"r11", c"r12",
        c"r13", c"r14", c"r15",
    ];
    unsafe {
        if RTR_LEFT > 0 {
            RTR_LEFT -= 1;
        }
        let g = &(*cpu).gpr;
        libc::fprintf(
            stderr(),
            c"ocerz: REGTRAP[%d] rip=%#llx ret-chain: %#llx".as_ptr(),
            libc::getpid(),
            ull((*cpu).rip),
            ull(rd8(g[RSP])),
        );
        let mut fp = g[RBP];
        let mut d = 0u64;
        while d < 40 && fp >= 0x10000 && ocerz_addr_readable(fp) != 0 && ocerz_addr_readable(fp + 8) != 0 {
            libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(ocerz_ld(fp + 8, 8)));
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
        ocerz_cpu_dump(cpu, stderr() as _);
        if !libc::getenv(c"OCERZ_REGTRAP_DEREF".as_ptr()).is_null() {
            for (gi, name) in RN.iter().enumerate() {
                let base = (*cpu).gpr[gi];
                if base < 0x1000 || ocerz_addr_readable(base) == 0 {
                    continue;
                }
                libc::fprintf(stderr(), c"  [%s=%#llx]".as_ptr(), name.as_ptr(), ull(base));
                let mut o: i32 = -0x20;
                while o < 0x40 {
                    let at = base.wrapping_add(o as i64 as u64);
                    if o == 0 {
                        libc::fprintf(stderr(), c" |".as_ptr());
                    }
                    if ocerz_addr_readable(at) != 0 {
                        libc::fprintf(stderr(), c" %+d:%016llx".as_ptr(), o, ull(ocerz_ld(at, 8)));
                    }
                    o += 8;
                }
                libc::fprintf(stderr(), c"\n".as_ptr());
            }
        }
    }
}

#[cold]
#[inline(never)]
unsafe fn riptrap_hit(vm: *mut OcerzVM, cpu: *mut OcerzCPU) {
    unsafe {
        let g = &(*cpu).gpr;
        let tid = rd8((*cpu).gs_base.wrapping_add(0x18));
        let mut acnt = 0u64;
        let mb = libc::getenv(c"OCERZ_MACDRVDUMP".as_ptr());
        if !mb.is_null() {
            let base = libc::strtoull(mb, core::ptr::null_mut(), 0);
            let slot = base.wrapping_add(0x560f0);
            if ocerz_addr_readable(slot) != 0 {
                let ctrl = ocerz_ld(slot, 8);
                if ctrl != 0 && ocerz_addr_readable(ctrl + 0x10) != 0 {
                    let arr = ocerz_ld(ctrl + 0x10, 8);
                    if arr != 0 && ocerz_addr_readable(arr + 0x20) != 0 {
                        acnt = ocerz_ld(arr + 0x20, 8) >> 32;
                    }
                }
            }
        }
        let f = rd8(g[RBP].wrapping_sub(0x58));
        let blk = if f != 0 && ocerz_addr_readable(f + 0x28) != 0 { ocerz_ld(f + 0x28, 8) } else { 0 };
        let rdi = g[RDI];
        let c28 = rd8(rdi.wrapping_add(0x28));
        let inv = if c28 != 0 && ocerz_addr_readable(c28 + 0x10) != 0 { ocerz_ld(c28 + 0x10, 8) } else { 0 };
        let dstv = if ocerz_addr_readable(g[RAX].wrapping_add(0x18)) != 0 {
            ocerz_ld(g[RAX].wrapping_add(0x18), 1)
        } else {
            0xff
        };
        libc::fprintf(
            stderr(),
            c"ocerz: RIPTRAP rip=%#llx tid=%#llx acnt=%#llx fwd2=%#llx blk=%#llx c20=%#llx c28=%#llx inv=%#llx dst=%#llx dstv=%#llx rdi=%#llx r13=%#llx byref=%#llx user=%#llx rsi=%#llx rdx=%#llx rax=%#llx rbx=%#llx rsp=%#llx ret0=%#llx ic=%#llx\n".as_ptr(),
            ull((*cpu).rip),
            ull(tid),
            ull(acnt),
            ull(f),
            ull(blk),
            ull(rd8(rdi.wrapping_add(0x20))),
            ull(c28),
            ull(inv),
            ull(g[RAX]),
            ull(dstv),
            ull(rdi),
            ull(g[OCERZ_R13 as usize]),
            ull(rd8(rdi.wrapping_add(0x38))),
            ull(rd8(rdi.wrapping_add(0x30))),
            ull(g[RSI]),
            ull(g[RDX]),
            ull(g[RAX]),
            ull(g[OCERZ_RBX as usize]),
            ull(g[RSP]),
            ull(ocerz_ld(g[RSP], 8)),
            ull((*vm).insn_count as u64),
        );
    }
}

#[cold]
#[inline(never)]
unsafe fn decode_fail_dump(cpu: *mut OcerzCPU, rc: c_int) {
    unsafe {
        let rip = (*cpu).rip;
        libc::fprintf(
            stderr(),
            c"ocerz: fatal: decode failed (%d, %s mode) at rip=%#llx\n  bytes: ".as_ptr(),
            rc,
            if (*cpu).mode32 != 0 { c"i386".as_ptr() } else { c"long".as_ptr() },
            ull(rip),
        );
        dump_raw_bytes(stderr(), rip, 15);
        libc::fprintf(
            stderr(),
            c"\n  [rsp]=%#llx [rsp+8]=%#llx rbp-ret=%#llx\n".as_ptr(),
            ull(ocerz_ld((*cpu).gpr[RSP], 8)),
            ull(ocerz_ld((*cpu).gpr[RSP].wrapping_add(8), 8)),
            ull(ocerz_ld((*cpu).gpr[RBP].wrapping_add(8), 8)),
        );
        let mut base: u64 = 0;
        let module = ocerz_dyld_name_for_addr(rip, &mut base);
        libc::fprintf(
            stderr(),
            c"  rip committed=%d prot=%d module=%s+%#llx\n".as_ptr(),
            ocerz_addr_committed(rip) as c_int,
            ocerz_addr_prot(rip) as c_int,
            if module.is_null() { c"(none)".as_ptr() } else { module },
            if module.is_null() { 0 } else { ull(rip.wrapping_sub(base)) },
        );
        let mut h = [0u64; 16];
        let nh = ocerz_vm_riphist(h.as_mut_ptr(), 16);
        libc::fprintf(stderr(), c"  guest riphist:".as_ptr());
        for &x in h.iter().take(nh as usize) {
            libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(x));
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
        let fd = libc::getenv(c"OCERZ_FAULTDUMP".as_ptr());
        let dreg = if fd.is_null() { -1 } else { libc::atoi(fd) };
        if (0..16).contains(&dreg) {
            let base = (*cpu).gpr[dreg as usize] & !0x7u64;
            libc::fprintf(stderr(), c"  FAULTDUMP r%d=%#llx:".as_ptr(), dreg, ull((*cpu).gpr[dreg as usize]));
            for i in 0..32u64 {
                let a = base.wrapping_add(i * 8);
                if (i & 3) == 0 {
                    libc::fprintf(stderr(), c"\n    %#llx:".as_ptr(), ull(a));
                }
                if ocerz_addr_readable(a) != 0 && ocerz_addr_readable(a.wrapping_add(7)) != 0 {
                    libc::fprintf(stderr(), c" %016llx".as_ptr(), ull(ocerz_ld(a, 8)));
                } else {
                    libc::fprintf(stderr(), c" ????????????????".as_ptr());
                }
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }
        libc::fprintf(
            stderr(),
            c"  signals: usr1 rcvd=%u delivered=%u  usr2 rcvd=%u  in_handler=%u  cpu#%u\n".as_ptr(),
            (*cpu).sig_host_rcvd[libc::SIGUSR1 as usize] as c_uint,
            (*cpu).sig_delivered[libc::SIGUSR1 as usize] as c_uint,
            (*cpu).sig_host_rcvd[libc::SIGUSR2 as usize] as c_uint,
            (*cpu).in_sighandler as c_uint,
            (*cpu).cpu_number as c_uint,
        );
        if !(*cpu).btrace.is_null() {
            let bn = (*cpu).btrace_n as u32;
            let m = (1u32 << 16) - 1;
            libc::fprintf(stderr(), c"  BTRACE n=%u:".as_ptr(), bn as c_uint);
            let mut k = 1u32;
            while k <= 96 && k <= bn {
                libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(*(*cpu).btrace.add((bn.wrapping_sub(k) & m) as usize)));
                k += 1;
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }
        if !libc::getenv(c"OCERZ_V8DUMP".as_ptr()).is_null() {
            let r13 = (*cpu).gpr[13];
            let r12 = (*cpu).gpr[12];
            let tp = rd8(r13.wrapping_add(0x4c40));
            libc::fprintf(stderr(), c"  V8DUMP [r13+0x4c40]=%#llx".as_ptr(), ull(tp));
            for i in 0..24i32 {
                let a = tp.wrapping_add(i as u64 * 8);
                if (i & 3) == 0 {
                    libc::fprintf(stderr(), c"\n    table[%2d]:".as_ptr(), i);
                }
                if tp != 0 && ocerz_addr_readable(a) != 0 && ocerz_addr_readable(a.wrapping_add(7)) != 0 {
                    libc::fprintf(stderr(), c" %016llx".as_ptr(), ull(ocerz_ld(a, 8)));
                } else {
                    libc::fprintf(stderr(), c" ????????????????".as_ptr());
                }
            }
            libc::fprintf(stderr(), c"\n  V8DUMP bytecode array r12=%#llx:".as_ptr(), ull(r12));
            for i in -1i64..8 {
                let a = r12.wrapping_add((i * 8) as u64).wrapping_sub(1);
                if ocerz_addr_readable(a) != 0 && ocerz_addr_readable(a.wrapping_add(7)) != 0 {
                    libc::fprintf(stderr(), c" %016llx".as_ptr(), ull(ocerz_ld(a, 8)));
                } else {
                    libc::fprintf(stderr(), c" ????????????????".as_ptr());
                }
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_interp_step(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut exclog = EXCLOG.load(Ordering::Relaxed);
        if exclog < 0 {
            exclog = exclog_init();
        }
        if exclog != 0 {
            if ocerz_cxa_throw_rip != 0 && (*cpu).rip == ocerz_cxa_throw_rip {
                cxa_throw_log(cpu);
            }
            if ocerz_exc_trap_rip != 0 && (*cpu).rip == ocerz_exc_trap_rip {
                exc_throw_log(cpu);
            }
        }

        if NRTR < 0 {
            regtrap_init();
        }
        let mut ti = 0;
        while ti < NRTR && RTR_LEFT != 0 {
            let t = RTR[ti as usize];
            if t != 0 && (*cpu).rip == t {
                regtrap_hit(cpu);
                break;
            }
            ti += 1;
        }

        if NTRAPS < 0 {
            NTRAPS = 0;
            NTRAPS = parse_list(libc::getenv(c"OCERZ_RIPTRAP".as_ptr()), (&raw mut TRAPS) as *mut u64);
        }
        for ti in 0..NTRAPS as usize {
            let t = TRAPS[ti];
            if t != 0 && (*cpu).rip == t {
                riptrap_hit(vm, cpu);
                break;
            }
        }

        ocerz_flags_materialize(cpu);
        if (*cpu).rip.wrapping_sub(OCERZ_DYLDAPI_LO as u64) < (OCERZ_DYLDAPI_HI as u64 - OCERZ_DYLDAPI_LO as u64) {
            return ocerz_dyldapi_dispatch(vm, cpu);
        }

        let mut insn = core::mem::MaybeUninit::<X86Insn>::uninit();
        let code = ocerz_g2h((*cpu).rip) as *const u8;
        let rc = ocerz_decode_mode(code, 15, (*cpu).rip, insn.as_mut_ptr(), (*cpu).mode32 as c_int);
        if rc != OCERZ_OK as c_int {
            decode_fail_dump(cpu, rc);
            return STEP_FATAL;
        }
        let insn = insn.assume_init_ref();

        (*vm).insn_count += 1;

        if (*vm).trace != 0 {
            let mut buf = [0 as c_char; 128];
            ocerz_format_insn(insn, buf.as_mut_ptr(), buf.len());
            libc::fprintf(stderr(), c"ocerz: %#llx: %s\n".as_ptr(), ull((*cpu).rip), buf.as_ptr());
        }
        let mut sl = STEPLOG.load(Ordering::Relaxed);
        if sl < 0 {
            sl = (!libc::getenv(c"OCERZ_STEPLOG".as_ptr()).is_null()) as i32;
            STEPLOG.store(sl, Ordering::Relaxed);
        }
        if sl != 0 {
            libc::fprintf(stderr(), c"STEP %#llx".as_ptr(), ull((*cpu).rip));
            for i in 0..16 {
                libc::fprintf(stderr(), c" %llx".as_ptr(), ull((*cpu).gpr[i]));
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }

        (*cpu).cur_rip = (*cpu).rip;
        (*cpu).rip = (*cpu).rip.wrapping_add(insn.len as u64);
        if insn.mode32 != 0 {
            (*cpu).rip = (*cpu).rip as u32 as u64;
        }
        let rc = ocerz_interp_exec(vm, cpu, insn);
        if (*cpu).mode32 != 0 {
            (*cpu).rip = (*cpu).rip as u32 as u64;
        }
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_interp_exec(vm: *mut OcerzVM, cpu: *mut OcerzCPU, insnp: *const X86Insn) -> c_int {
    unsafe {
        match (*insnp).op as u32 {
            OCERZ_OP_MOV | OCERZ_OP_MOVZX | OCERZ_OP_MOVSX | OCERZ_OP_MOVSXD | OCERZ_OP_LEA | OCERZ_OP_XCHG
            | OCERZ_OP_BSWAP | OCERZ_OP_CMOVCC | OCERZ_OP_SETCC => return op_mov_family(vm, cpu, insnp),

            OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_PUSHF | OCERZ_OP_POPF | OCERZ_OP_LAHF | OCERZ_OP_SAHF
            | OCERZ_OP_LEAVE => return op_stack(vm, cpu, insnp),

            OCERZ_OP_CBW | OCERZ_OP_CWD => return op_cbw_cwd(vm, cpu, insnp),

            OCERZ_OP_ADD | OCERZ_OP_ADC | OCERZ_OP_SUB | OCERZ_OP_SBB | OCERZ_OP_AND | OCERZ_OP_OR
            | OCERZ_OP_XOR | OCERZ_OP_CMP | OCERZ_OP_TEST => return op_arith(vm, cpu, insnp),

            OCERZ_OP_INC | OCERZ_OP_DEC | OCERZ_OP_NEG | OCERZ_OP_NOT => return op_incdecnegnot(vm, cpu, insnp),

            OCERZ_OP_MUL | OCERZ_OP_IMUL => return op_mul(vm, cpu, insnp),

            OCERZ_OP_DIV | OCERZ_OP_IDIV => return op_div(vm, cpu, insnp),

            OCERZ_OP_SHL | OCERZ_OP_SHR | OCERZ_OP_SAR => return op_shift(vm, cpu, insnp),

            OCERZ_OP_ROL | OCERZ_OP_ROR | OCERZ_OP_RCL | OCERZ_OP_RCR => return op_rotate(vm, cpu, insnp),

            OCERZ_OP_SHLD | OCERZ_OP_SHRD => return op_shiftd(vm, cpu, insnp),

            OCERZ_OP_JMP | OCERZ_OP_JCC | OCERZ_OP_JRCXZ | OCERZ_OP_LOOP | OCERZ_OP_LOOPE | OCERZ_OP_LOOPNE
            | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_IRET | OCERZ_OP_JMPF | OCERZ_OP_CALLF | OCERZ_OP_RETF
            | OCERZ_OP_MOVSEG | OCERZ_OP_MOVFROMSEG => return op_branch(vm, cpu, insnp),

            OCERZ_OP_XADD | OCERZ_OP_CMPXCHG | OCERZ_OP_CMPXCHGXB => return op_atomic(vm, cpu, insnp),

            OCERZ_OP_PUSHA | OCERZ_OP_POPA | OCERZ_OP_PUSHSEG | OCERZ_OP_POPSEG | OCERZ_OP_DAA | OCERZ_OP_DAS
            | OCERZ_OP_AAA | OCERZ_OP_AAS | OCERZ_OP_AAM | OCERZ_OP_AAD | OCERZ_OP_SALC | OCERZ_OP_BOUND
            | OCERZ_OP_INTO | OCERZ_OP_LES | OCERZ_OP_LDS => return op_i386(vm, cpu, insnp),

            OCERZ_OP_CLC | OCERZ_OP_STC | OCERZ_OP_CMC | OCERZ_OP_CLD | OCERZ_OP_STD => {
                return op_flagctl(vm, cpu, insnp);
            }

            OCERZ_OP_NOP | OCERZ_OP_PAUSE | OCERZ_OP_PREFETCH | OCERZ_OP_CLFLUSH => return STEP_OK,

            OCERZ_OP_MFENCE | OCERZ_OP_LFENCE | OCERZ_OP_SFENCE => {
                core::sync::atomic::fence(Ordering::SeqCst);
                return STEP_OK;
            }

            OCERZ_OP_INT3 => {
                let mut tl = TRAPLOG.load(Ordering::Relaxed);
                if tl < 0 {
                    tl = (!libc::getenv(c"OCERZ_TRAPLOG".as_ptr()).is_null()) as i32;
                    TRAPLOG.store(tl, Ordering::Relaxed);
                }
                if tl != 0 {
                    let ip = ((*cpu).rip.wrapping_add(ocerz_guest_base)) as usize as *const u8;
                    libc::fprintf(
                        stderr(),
                        c"ocerz: INT3 pid=%d rip=%#llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n".as_ptr(),
                        libc::getpid(),
                        (*cpu).rip as libc::c_ulonglong,
                        *ip as c_uint, *ip.add(1) as c_uint, *ip.add(2) as c_uint, *ip.add(3) as c_uint,
                        *ip.add(4) as c_uint, *ip.add(5) as c_uint, *ip.add(6) as c_uint, *ip.add(7) as c_uint,
                    );
                }
                if ocerz_signal_deliver(cpu, OCERZ_SIGTRAP as _, (*cpu).rip, 0, 0) != 0 {
                    return STEP_OK;
                }
                return trap_fatal(insnp, c"guest breakpoint/interrupt".as_ptr());
            }
            OCERZ_OP_INT => return trap_fatal(insnp, c"guest breakpoint/interrupt".as_ptr()),

            OCERZ_OP_UD2 => {
                if ocerz_is_wqthread_exit((*insnp).rip) != 0 {
                    (*cpu).terminated = 1;
                    return STEP_OK;
                }
                if !libc::getenv(c"OCERZ_UD2DUMP".as_ptr()).is_null() {
                    ud2_dump(cpu, insnp);
                }
                return trap_fatal(insnp, c"guest UD2 (undefined instruction)".as_ptr());
            }

            OCERZ_OP_HLT => return trap_fatal(insnp, c"guest HLT".as_ptr()),

            OCERZ_OP_SYSCALL => {
                if (*insnp).mode32 != 0 {
                    return ocerz_unimpl(vm, cpu, insnp, c"syscall in 32-bit mode".as_ptr());
                }
                if ocerz_signal_before_syscall(cpu, (*insnp).rip) != 0 {
                    return STEP_OK;
                }
                (*cpu).gpr[RCX] = (*cpu).rip;
                (*cpu).gpr[R11] = (*cpu).rflags & 0x3c7fd7;
                return ocerz_handle_syscall(vm, cpu);
            }

            _ => {}
        }

        let mut rc = ocerz_interp_ext(vm, cpu, insnp);
        if rc == OCERZ_EUNSUP as i32 && (*insnp).op as u32 >= OCERZ_OP_SSE_FIRST {
            rc = ocerz_interp_sse(vm, cpu, insnp);
        }
        if rc == OCERZ_EUNSUP as i32 {
            return ocerz_unimpl(vm, cpu, insnp, c"no handler".as_ptr());
        }
        rc
    }
}

static TRAPLOG: AtomicI32 = AtomicI32::new(-1);

unsafe extern "C" {
    fn ocerz_recov_dump(out: *mut libc::FILE);
}

#[cold]
unsafe fn ud2_dump(cpu: *mut OcerzCPU, insnp: *const X86Insn) {
    unsafe {
        let ull = |x: u64| x as libc::c_ulonglong;
        ocerz_recov_dump(stderr());
        let gs = (*cpu).gs_base;
        let gate = (*cpu).gpr[RDX];
        libc::fprintf(
            stderr(),
            c"ocerz: UD2DUMP rip=%#llx gs_base=%#llx gs+0x18=%#llx gate=%#llx gate[0]=%#llx rdi=%#llx rsi=%#llx r14=%#llx\n".as_ptr(),
            ull((*insnp).rip),
            ull(gs),
            ull(ocerz_ld(gs.wrapping_add(0x18), 8)),
            ull(gate),
            ull(if gate != 0 { ocerz_ld(gate, 8) } else { 0 }),
            ull((*cpu).gpr[RDI]),
            ull((*cpu).gpr[RSI]),
            ull((*cpu).gpr[OCERZ_R14 as usize]),
        );
        let mut fp = (*cpu).gpr[RBP];
        libc::fprintf(stderr(), c"ocerz: UD2DUMP bt:".as_ptr());
        let mut d = 0;
        while d < 16 && fp >= 0x300000000 {
            libc::fprintf(stderr(), c" %#llx".as_ptr(), ull(ocerz_ld(fp.wrapping_add(8), 8)));
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
}
