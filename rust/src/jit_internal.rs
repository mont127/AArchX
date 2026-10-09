//! Rust equivalents of the `static inline` helpers in
//! `include/ocerz/jit_internal.h` (129 functions), for the eight JIT-piece
//! ports. This file mirrors the header: same names, same semantics, and it
//! must change whenever the header does. Layouts come from `crate::ffi`
//! (bindgen, layout-tested); `__thread` state is declared here with the
//! scaffold's `#[thread_local]` extern pattern because bindgen's plain
//! `extern static` is wrong for TLS. Header macros bindgen skips (casts,
//! offsetof expressions) live here as `pub const`/`const fn`.
//!
//! Not translated: the `ENV_ON(name)` macro (a per-callsite static cache —
//! write a `static` + `libc::getenv` at each Rust call site), and the
//! statement-expression helpers that exist only in the .c files.
#![allow(dead_code)]
#![allow(clippy::missing_safety_doc, clippy::manual_range_contains,
    clippy::unnecessary_cast, clippy::manual_c_str_literals,
    clippy::collapsible_if, clippy::needless_range_loop)]

use core::ffi::{c_int, c_uint, c_void};
use core::sync::atomic::{fence, AtomicI32, AtomicU16, AtomicU32, AtomicU64, AtomicU8, Ordering};

use crate::ffi::*;
use crate::inline::{OCERZ_AF, OCERZ_CF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF};

unsafe extern "C" {
    #[thread_local]
    static mut jl_held: c_int;
    #[thread_local]
    static mut t_xlat_overflow: c_int;
    #[thread_local]
    static mut ocerz_jit_exec_state: c_int;
    #[thread_local]
    static mut ocerz_jit_decode_recover: *mut sigjmp_buf;
    #[thread_local]
    static mut ocerz_critical_depth: c_int;
}

pub const JIT_ARITH_FLAGS: u64 = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF;
pub const INVMAP_TOMB: u64 = u64::MAX;
pub const VENEER_REACH: isize = 120 << 20;

pub const RF_OFF: u32 = core::mem::offset_of!(OcerzCPU, rflags) as u32;
pub const RIP_OFF: u32 = core::mem::offset_of!(OcerzCPU, rip) as u32;
pub const SIDE_BLK_OFF: u32 = core::mem::offset_of!(OcerzCPU, side_blk) as u32;
pub const SIDE_IDX_OFF: u32 = core::mem::offset_of!(OcerzCPU, side_idx) as u32;
pub const INT_OFF: u32 = core::mem::offset_of!(OcerzCPU, interrupt) as u32;
pub const CC_SRC_OFF: u32 = core::mem::offset_of!(OcerzCPU, cc_src) as u32;
pub const CC_DST_OFF: u32 = core::mem::offset_of!(OcerzCPU, cc_dst) as u32;
pub const CC_OP_OFF: u32 = core::mem::offset_of!(OcerzCPU, cc_op) as u32;
pub const RAS_TOP_OFF: u32 = core::mem::offset_of!(OcerzCPU, ras_top) as u32;
pub const FCMP_MEM_OFF: u32 = core::mem::offset_of!(OcerzCPU, jit_fcmp_mem) as u32;
pub const RAS_OFF: u32 = core::mem::offset_of!(OcerzCPU, ras) as u32;
pub const JIT_FP_OFF: u32 = core::mem::offset_of!(OcerzCPU, jit_fp) as u32;
pub const YMMH_ALL_ZERO_OFF: u32 = core::mem::offset_of!(OcerzCPU, ymmh_all_zero) as u32;
pub const XMM_BASE_OFF: u32 = core::mem::offset_of!(OcerzCPU, xmm) as u32;
pub const FPCKPT_OFF: u32 = core::mem::offset_of!(OcerzCPU, fp_ckpt) as u32;
pub const YMMH_OFF: u32 = core::mem::offset_of!(OcerzCPU, ymmh) as u32;
pub const RFLAGS_OFF: u32 = RF_OFF;
pub const X87_CTL_OFF: u32 = core::mem::offset_of!(OcerzCPU, fcw) as u32;
pub const X87_FPR_OFF: u32 = core::mem::offset_of!(OcerzCPU, fpr) as u32;
pub const X87_XM_OFF: u32 = core::mem::offset_of!(OcerzCPU, fpr_xm) as u32;
pub const X87_XE_OFF: u32 = core::mem::offset_of!(OcerzCPU, fpr_xe) as u32;
pub const X87_MXCSR_OFF: u32 = core::mem::offset_of!(OcerzCPU, mxcsr) as u32;
pub const X87_TOP0_OFF: u32 = core::mem::offset_of!(OcerzCPU, jit_x87_top0) as u32;
pub const JIT_SCRATCH_OFF: u32 = core::mem::offset_of!(OcerzCPU, jit_scratch) as u32;
pub const LEAF_EPOCH_OFF: u32 = JIT_SCRATCH_OFF + 8;

#[inline(always)]
pub const fn GPR_OFF(r: c_uint) -> u32 {
    r * 8
}

#[inline(always)]
pub const fn MMX_OFF(r: u32) -> u32 {
    (core::mem::offset_of!(OcerzCPU, mmx) as u32).wrapping_add(8u32.wrapping_mul(r))
}

const _: () = assert!(CC_DST_OFF == CC_SRC_OFF + 8);
const _: () = assert!(PSC_N as usize == OCERZ_PSC_COLS as usize);
const _: () = assert!(OCERZ_TOP_LO == (1u64 << 47) - (1u64 << 25));
const _: () = assert!(core::mem::offset_of!(OcerzCPU, xmm) % 16 == 0);
const _: () = assert!(core::mem::offset_of!(OcerzCPU, fp_ckpt) % 16 == 0
    && core::mem::offset_of!(OcerzCPU, fp_ckpt) + 256 <= 65520);
const _: () = assert!(core::mem::offset_of!(OcerzCPU, ymmh) % 16 == 0
    && core::mem::offset_of!(OcerzCPU, ymmh) + 256 <= 65520);
const _: () = assert!(core::mem::offset_of!(OcerzCPU, fcw) % 8 == 0
    && core::mem::offset_of!(OcerzCPU, fsw) == core::mem::offset_of!(OcerzCPU, fcw) + 2
    && core::mem::offset_of!(OcerzCPU, ftw) == core::mem::offset_of!(OcerzCPU, fcw) + 4
    && core::mem::offset_of!(OcerzCPU, ftop) == core::mem::offset_of!(OcerzCPU, fcw) + 5
    && core::mem::offset_of!(OcerzCPU, fpr_x_ok) == core::mem::offset_of!(OcerzCPU, fcw) + 6
    && core::mem::offset_of!(OcerzCPU, fpr) >= core::mem::offset_of!(OcerzCPU, fcw) + 8);
const _: () = assert!(core::mem::offset_of!(OcerzCPU, jit_fcmp_mem) % 8 == 0
    && core::mem::offset_of!(OcerzCPU, jit_fcmp_mem) <= 4 * 4095);
const _: () = assert!(core::mem::offset_of!(OcerzCPU, fpr_xm) + 64 <= 8 * 4095
    && core::mem::offset_of!(OcerzCPU, fpr_xe) + 16 <= 2 * 4095
    && core::mem::offset_of!(OcerzCPU, jit_x87_top0) % 2 == 0
    && core::mem::offset_of!(OcerzCPU, jit_x87_top0) <= 2 * 4095
    && core::mem::offset_of!(OcerzCPU, mxcsr) % 4 == 0);
const _: () = assert!(core::mem::size_of::<TcRec>().is_multiple_of(8)
    && core::mem::size_of::<TcEdge>().is_multiple_of(8));

#[inline(always)]
pub unsafe fn ocerz_pinned_page(gaddr: u64) -> c_int {
    unsafe {
        (!ocerz_pin_map.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*ocerz_pin_map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0)
            as c_int
    }
}

#[inline(always)]
pub unsafe fn ocerz_g2h(gaddr: u64) -> *mut c_void {
    unsafe {
        if !ocerz_commpage.is_null()
            && gaddr >= OCERZ_COMMPAGE_LO
            && gaddr < OCERZ_COMMPAGE_HI
        {
            return (ocerz_commpage as *mut u8).add((gaddr - OCERZ_COMMPAGE_LO) as usize)
                as *mut c_void;
        }
        if ocerz_low_base != 0 {
            if gaddr < OCERZ_LOW_LIMIT {
                if (gaddr < OCERZ_NULL_LIMIT as u64 && !ocerz_pin_map.is_null())
                    || ocerz_pinned_page(gaddr) != 0
                {
                    return gaddr as usize as *mut c_void;
                }
                return (gaddr + ocerz_low_base) as usize as *mut c_void;
            }
            if gaddr.wrapping_sub(OCERZ_TOP_LO) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return (gaddr - OCERZ_TOP_LO + ocerz_top_base) as usize as *mut c_void;
            }
        }
        (gaddr + ocerz_guest_base) as usize as *mut c_void
    }
}

#[inline(always)]
pub unsafe fn ocerz_h2g(haddr: *const c_void) -> u64 {
    unsafe {
        let h = haddr as u64;
        if ocerz_low_base != 0 {
            if h.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT {
                return h - ocerz_low_base;
            }
            if h.wrapping_sub(ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return h - ocerz_top_base + OCERZ_TOP_LO;
            }
        }
        h - ocerz_guest_base
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr) as *mut u8;
        match size {
            1 => AtomicU8::from_ptr(p).load(Ordering::Acquire) as u64,
            2 if (p as usize) & 1 == 0 => {
                AtomicU16::from_ptr(p.cast()).load(Ordering::Acquire) as u64
            }
            4 if (p as usize) & 3 == 0 => {
                AtomicU32::from_ptr(p.cast()).load(Ordering::Acquire) as u64
            }
            8 if (p as usize) & 7 == 0 => {
                AtomicU64::from_ptr(p.cast()).load(Ordering::Acquire)
            }
            _ => {
                let mut value = 0u64;
                core::ptr::copy_nonoverlapping(
                    p,
                    &mut value as *mut u64 as *mut u8,
                    size as usize,
                );
                fence(Ordering::Acquire);
                value
            }
        }
    }
}

#[inline(always)]
unsafe fn at64(p: *const u64) -> &'static AtomicU64 {
    unsafe { &*(p as *const AtomicU64) }
}

#[inline(always)]
unsafe fn at32(p: *const c_int) -> &'static AtomicI32 {
    unsafe { &*(p as *const AtomicI32) }
}

#[inline(always)]
pub fn jit_key(rip: u64, mode32: c_int) -> u64 {
    if mode32 != 0 {
        rip | JIT_KEY_M32 as u64
    } else {
        rip
    }
}

#[inline(always)]
pub fn jit_key_rip(key: u64) -> u64 {
    key & !(JIT_KEY_M32 as u64)
}

#[inline(always)]
pub fn jit_key_mode32(key: u64) -> c_int {
    (key >> 63) as c_int
}

#[inline(always)]
pub unsafe fn blk_rip(b: *const JitBlock) -> u64 {
    jit_key_rip(unsafe { (*b).key })
}

#[inline(always)]
pub unsafe fn blk_mode32(b: *const JitBlock) -> c_int {
    jit_key_mode32(unsafe { (*b).key })
}

#[inline(always)]
pub unsafe fn blk_insn_full(b: *const JitBlock, i: c_int) -> *const X86Insn {
    unsafe {
        if !(*b).insns.is_null() {
            return (*b).insns.add(i as usize);
        }
        let k = (*(*b).iref.add(i as usize)).keep;
        if k != 0 {
            (*b).kept.add(k as usize - 1)
        } else {
            core::ptr::null()
        }
    }
}

#[inline(always)]
pub unsafe fn tc_note(at: *const u32, kind: c_int, form: c_int, arg: u64) {
    unsafe {
        if g_tc_on == 0 || g_tc_entry.is_null() {
            return;
        }
        if g_tc_nrel >= TC_RELOC_MAX as c_int {
            g_tc_bad = 1;
            return;
        }
        let n = g_tc_nrel as usize;
        g_tc_rel[n].off = at.offset_from(g_tc_entry) as u32;
        g_tc_rel[n].kind = kind as u8;
        g_tc_rel[n].form = form as u8;
        g_tc_rel[n].arg = arg;
        g_tc_nrel += 1;
    }
}

#[inline(always)]
pub unsafe fn tc_imm64(b: *mut A64Buf, rd: c_int, kind: c_int, arg: u64, value: u64) {
    unsafe {
        if g_tc_on == 0 {
            a64_mov_imm64(b, rd, value);
            return;
        }
        tc_note((*b).p, kind, 0, arg);
        a64_movz(b, rd, value as u16, 0);
        a64_movk(b, rd, (value >> 16) as u16, 1);
        a64_movk(b, rd, (value >> 32) as u16, 2);
        a64_movk(b, rd, (value >> 48) as u16, 3);
    }
}

#[inline(always)]
pub unsafe fn tc_noload_has(key: u64) -> c_int {
    unsafe {
        if g_tc_noload_n == 0 {
            return 0;
        }
        let mut i = ((key.wrapping_mul(0x9e3779b97f4a7c15) >> 20) as usize) & (g_tc_noload_cap - 1);
        while *g_tc_noload.add(i) != 0 {
            if *g_tc_noload.add(i) == key {
                return 1;
            }
            i = (i + 1) & (g_tc_noload_cap - 1);
        }
        0
    }
}

#[inline(always)]
pub unsafe fn ras_cell_register(cell: *mut *mut c_void) {
    unsafe {
        if g_n_ras_cells == g_cap_ras_cells {
            let ncap = if g_cap_ras_cells != 0 { g_cap_ras_cells * 2 } else { 1024 };
            let nv = libc::realloc(
                g_ras_cells as *mut c_void,
                ncap * core::mem::size_of::<*mut *mut c_void>(),
            ) as *mut *mut *mut c_void;
            if nv.is_null() {
                return;
            }
            g_ras_cells = nv;
            g_cap_ras_cells = ncap;
        }
        *g_ras_cells.add(g_n_ras_cells) = cell;
        g_n_ras_cells += 1;
    }
}

#[inline(always)]
pub unsafe fn flip_find(rip: u64, insert: c_int) -> c_int {
    unsafe {
        let mut h = ((rip.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as c_uint) & (FLIP_N - 1);
        for _ in 0..64 {
            let e = &mut g_flip[h as usize];
            if e.rip == rip {
                return h as c_int;
            }
            if e.rip == 0 {
                if insert == 0 {
                    return -1;
                }
                e.rip = rip;
                e.state = FLIP_NONE as u8;
                return h as c_int;
            }
            h = (h + 1) & (FLIP_N - 1);
        }
        -1
    }
}

#[inline(always)]
pub unsafe fn flip_state(rip: u64) -> c_int {
    unsafe {
        let i = flip_find(rip, 0);
        if i < 0 { FLIP_NONE as c_int } else { g_flip[i as usize].state as c_int }
    }
}

#[inline(always)]
pub unsafe fn cp_marked(key: u64) -> c_int {
    unsafe {
        if key == 0 {
            return 0;
        }
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 46) as c_uint) & (CP_MARK_SIZE - 1);
        for _ in 0..CP_MARK_SIZE {
            let v = g_cp_marks[i as usize];
            if v == key {
                return 1;
            }
            if v == 0 {
                return 0;
            }
            i = (i + 1) & (CP_MARK_SIZE - 1);
        }
        0
    }
}

#[inline(always)]
pub unsafe fn cp_mark(key: u64) {
    unsafe {
        if key == 0 {
            return;
        }
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 46) as c_uint) & (CP_MARK_SIZE - 1);
        for _ in 0..CP_MARK_SIZE {
            let v = g_cp_marks[i as usize];
            if v == key {
                return;
            }
            if v == 0 {
                g_cp_marks[i as usize] = key;
                return;
            }
            i = (i + 1) & (CP_MARK_SIZE - 1);
        }
    }
}

#[inline(always)]
pub unsafe fn mem_guard_needed() -> c_int {
    unsafe { (ocerz_low_base != 0 || g_cp_guard != 0) as c_int }
}

#[inline(always)]
pub unsafe fn jl_release() {
    unsafe {
        if g_jl_log > 0 {
            at32(&raw mut g_jl_phase).store(0, Ordering::Relaxed);
            at64(&raw mut g_jl_owner).store(0, Ordering::Relaxed);
            at64(&raw mut g_jl_since).store(0, Ordering::Relaxed);
        }
        jl_held -= 1;
        libc::pthread_mutex_unlock(&raw mut jit_lock as *mut libc::pthread_mutex_t);
        ocerz_critical_depth -= 1;
    }
}

#[inline(always)]
pub unsafe fn al_marked(key: u64) -> c_int {
    unsafe {
        if g_al_all != 0 {
            return 1;
        }
        if key == 0 {
            return 0;
        }
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 46) as c_uint) & (AL_MARK_SIZE - 1);
        for _ in 0..AL_MARK_SIZE {
            let v = g_al_marks[i as usize];
            if v == key {
                return 1;
            }
            if v == 0 {
                return 0;
            }
            i = (i + 1) & (AL_MARK_SIZE - 1);
        }
        0
    }
}

#[inline(always)]
pub unsafe fn al_mark(key: u64) {
    unsafe {
        if g_al_all != 0 || key == 0 {
            return;
        }
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 46) as c_uint) & (AL_MARK_SIZE - 1);
        for _ in 0..AL_MARK_SIZE {
            let v = g_al_marks[i as usize];
            if v == key {
                return;
            }
            if v == 0 {
                g_al_marks[i as usize] = key;
                g_al_n += 1;
                return;
            }
            i = (i + 1) & (AL_MARK_SIZE - 1);
        }
        g_al_all = 1;
    }
}

#[inline(always)]
pub unsafe fn stack_plain_access_ok() -> c_int {
    unsafe { (g_plain_mem != 0 || stack_plain_ok() != 0) as c_int }
}

#[inline(always)]
pub unsafe fn mem_plain_access_ok(m: *const X86Operand) -> c_int {
    unsafe {
        if g_plain_mem != 0 {
            return 1;
        }
        (stack_plain_ok() != 0 && (*m).base == OCERZ_RSP as u8 && (*m).riprel == 0) as c_int
    }
}

#[inline(always)]
pub unsafe fn mem_fast_forms_ok() -> c_int {
    unsafe { (jgb_usable() != 0 && mem_guard_needed() == 0) as c_int }
}

#[inline(always)]
pub unsafe fn stack_guard_needed() -> c_int {
    unsafe { (ocerz_low_base != 0) as c_int }
}

#[inline(always)]
pub unsafe fn stack_plain_now() -> c_int {
    unsafe { (ocerz_low_base != 0 && stack_plain_ok() != 0) as c_int }
}

#[inline(always)]
pub unsafe fn hoist_signature() -> u64 {
    unsafe {
        if g_mem_hoist_greg < 0 {
            return 0;
        }
        1u64 | (((g_mem_hoist_greg & 0xff) as u64) << 8)
            | ((((g_mem_hoist_greg2 + 1) & 0xff) as u64) << 16)
            | ((((g_mem_hoist_greg3 + 1) & 0xff) as u64) << 24)
            | ((((g_mem_hoist_aux_index + 1) & 0xff) as u64) << 32)
            | (((g_mem_hoist_aux_scale & 3) as u64) << 40)
            | ((((g_mem_hoist_aux_disp + 0x8000) as u32 & 0xffff) as u64) << 44)
    }
}

#[inline(always)]
pub unsafe fn jgb_usable() -> c_int {
    unsafe { (ocerz_low_base == 0) as c_int }
}

#[inline(always)]
pub unsafe fn emit_reload_jgb(b: *mut A64Buf) {
    unsafe {
        if jgb_usable() != 0 {
            a64_mov_imm64(b, JGB as c_int, ocerz_guest_base);
        } else if g_lowstack != 0 {
            emit_stack_delta(b);
        } else if g_m32low != 0 {
            a64_mov_imm64(b, JGB as c_int, ocerz_low_base);
        }
    }
}

#[inline(always)]
pub unsafe fn g_xlat_mode32_fwd() -> c_int {
    unsafe { g_xlat_mode32 }
}

#[inline(always)]
pub unsafe fn rsp_is_ptr() -> c_int {
    unsafe {
        (g_pin_class == 2 || (g_pin_class == 3 && g_xlat_mode32_fwd() == 0 && rsp_ptr3() != 0))
            as c_int
    }
}

#[inline(always)]
pub unsafe fn ps_retsite_counter(rip: u64) -> *mut u64 {
    unsafe {
        let mut i = ((rip.wrapping_mul(0x9E3779B97F4A7C15) >> 48) as c_uint) & (PS_RETSITE_N - 1);
        for _ in 0..64 {
            if ps_retsite[i as usize].rip == rip {
                return &raw mut ps_retsite[i as usize].n;
            }
            if ps_retsite[i as usize].rip == 0 {
                ps_retsite[i as usize].rip = rip;
                return &raw mut ps_retsite[i as usize].n;
            }
            i = (i + 1) & (PS_RETSITE_N - 1);
        }
        &raw mut ps_retsite[0].n
    }
}

#[inline(always)]
pub fn hash_key(mut key: u64) -> c_uint {
    key ^= key >> 33;
    key = key.wrapping_mul(0xff51afd7ed558ccd);
    key ^= key >> 29;
    (key & JIT_HASH_MASK as u64) as c_uint
}

#[inline(always)]
pub unsafe fn call_body_successor(rip: u64) -> c_int {
    unsafe {
        let term = decoded_terminator(rip);
        (term == OCERZ_OP_CALL as c_uint || term == OCERZ_OP_RET as c_uint) as c_int
    }
}

#[inline(always)]
pub unsafe fn xlive_succ_live_d(jit: *mut OcerzJit, rip: u64, depth: c_int) -> u64 {
    unsafe {
        let t = if jit.is_null() {
            core::ptr::null_mut()
        } else {
            cache_lookup(jit, rip, g_xlat_mode32)
        };
        if g_xlive_log < 0 {
            g_xlive_log = if libc::getenv(b"OCERZ_XLIVELOG\0".as_ptr() as *const _).is_null() {
                0
            } else {
                1
            };
        }
        if !t.is_null() && (*t).code.is_some() && g_xlive_log != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: XLIVE cached rip=%#llx entry_live=%#x n_insns=%d\n".as_ptr(),
                rip as libc::c_ulonglong,
                (*t).entry_live as c_uint,
                (*t).n_insns,
            );
        }
        if !t.is_null() && (*t).code.is_some() && g_tc_rec == 0 {
            (*t).entry_live as u64
        } else {
            xlive_decode_entry_d(rip, depth)
        }
    }
}

#[inline(always)]
pub unsafe fn xlive_succ_live(jit: *mut OcerzJit, rip: u64) -> u64 {
    unsafe { xlive_succ_live_d(jit, rip, 0) }
}

#[inline(always)]
pub unsafe fn probe_wanted(jcc_rip: u64, ft_rip: u64) -> c_int {
    unsafe {
        if flip_disabled() != 0 {
            return 0;
        }
        if g_n_probes >= PROBE_MAX as c_int || flip_state(jcc_rip) != FLIP_NONE as c_int {
            g_tc_learned = 1;
            return 0;
        }
        if xlive_succ_live(g_xlat_jit, ft_rip) != 0 {
            return 0;
        }
        g_n_probes += 1;
        1
    }
}

#[inline(always)]
pub unsafe fn emit_frame_sp_reset(b: *mut A64Buf) {
    unsafe {
        if g_pin_class == 3 && host_ras_enabled() != 0 {
            a64_ldr(b, 8, 15, 20, JIT_FP_OFF);
            a64_add_imm(b, 1, 31, 15, 0);
        }
    }
}

#[inline(always)]
pub unsafe fn stack_fast() -> c_int {
    unsafe { ((jgb_usable() != 0 && stack_guard_needed() == 0) || low_stack_fast() != 0) as c_int }
}

#[inline(always)]
pub unsafe fn emit_zf_sf(b: *mut A64Buf, m: u64) {
    unsafe {
        if m & OCERZ_ZF != 0 {
            a64_cset(b, JTT as c_int, A64_EQ as c_int);
            a64_lsl_imm(b, 0, JTT as c_int, JTT as c_int, 6);
            a64_orr_reg(b, 1, JTF as c_int, JTF as c_int, JTT as c_int, 0);
        }
        if m & OCERZ_SF != 0 {
            a64_cset(b, JTT as c_int, A64_MI as c_int);
            a64_lsl_imm(b, 0, JTT as c_int, JTT as c_int, 7);
            a64_orr_reg(b, 1, JTF as c_int, JTF as c_int, JTT as c_int, 0);
        }
    }
}

#[inline(always)]
pub unsafe fn emit_commit_flags(b: *mut A64Buf, clear_mask: u64) {
    unsafe {
        a64_ldr(b, 8, JTT as c_int, 20, RF_OFF);
        a64_mov_imm64(b, JTU as c_int, !clear_mask);
        a64_and_reg(b, 1, JTT as c_int, JTT as c_int, JTU as c_int, 0);
        a64_orr_reg(b, 1, JTT as c_int, JTT as c_int, JTF as c_int, 0);
        a64_str(b, 8, JTT as c_int, 20, RF_OFF);
    }
}

#[inline(always)]
pub unsafe fn emit_defer_flags(b: *mut A64Buf, ccop: u32, src_reg: c_int, dst_reg: c_int) {
    unsafe {
        a64_stp_off(b, src_reg, dst_reg, 20, CC_SRC_OFF as i32);
        a64_mov_imm64(b, JTT as c_int, ccop as u64);
        a64_str(b, 4, JTT as c_int, 20, CC_OP_OFF);
    }
}

#[inline(always)]
pub unsafe fn pin_slot(greg: c_uint) -> c_int {
    unsafe {
        if !g_pin.is_null() && greg < 16 {
            *g_pin.add(greg as usize) as c_int
        } else {
            -1
        }
    }
}

#[inline(always)]
pub fn pin_hreg(slot: c_int) -> c_int {
    if slot < 8 {
        return 21 + slot;
    }
    if slot < 14 {
        return 3 + (slot - 8);
    }
    1 + (slot - 14)
}

#[inline(always)]
pub unsafe fn body_edge_pin_class() -> c_int {
    unsafe {
        if g_pin_class != 0 {
            return g_pin_class;
        }
        if g_n_pinned == 0 {
            0
        } else {
            -1
        }
    }
}

#[inline(always)]
pub unsafe fn emit_gpr_rd(b: *mut A64Buf, sf: c_int, dst: c_int, greg: c_uint) {
    unsafe {
        let s = pin_slot(greg);
        if s >= 0 && rsp_is_ptr() != 0 && greg == OCERZ_RSP as c_uint {
            if jgb_usable() != 0 {
                a64_sub_reg(b, 1, dst, pin_hreg(s), JGB as c_int, 0);
            } else if ocerz_guest_base == 0 {
                a64_mov_reg(b, 1, dst, pin_hreg(s));
            } else {
                a64_mov_imm64(b, dst, ocerz_guest_base);
                a64_sub_reg(b, 1, dst, pin_hreg(s), dst, 0);
            }
            if sf == 0 {
                a64_mov_reg(b, 0, dst, dst);
            }
        } else if s >= 0 {
            a64_mov_reg(b, sf, dst, pin_hreg(s));
        } else {
            a64_ldr(b, if sf != 0 { 8 } else { 4 }, dst, 20, GPR_OFF(greg));
        }
    }
}

#[inline(always)]
pub unsafe fn emit_gpr_wr(b: *mut A64Buf, src: c_int, greg: c_uint) {
    unsafe {
        let s = pin_slot(greg);
        if s >= 0 && rsp_is_ptr() != 0 && greg == OCERZ_RSP as c_uint {
            if jgb_usable() != 0 {
                a64_add_reg(b, 1, pin_hreg(s), src, JGB as c_int, 0);
            } else if ocerz_guest_base == 0 {
                a64_mov_reg(b, 1, pin_hreg(s), src);
            } else {
                let tmp = if src == JTU as c_int { JTA } else { JTU } as c_int;
                a64_mov_imm64(b, tmp, ocerz_guest_base);
                a64_add_reg(b, 1, pin_hreg(s), src, tmp, 0);
            }
        } else if s >= 0 {
            a64_mov_reg(b, 1, pin_hreg(s), src);
        } else {
            a64_str(b, 8, src, 20, GPR_OFF(greg));
        }
    }
}

#[inline(always)]
pub unsafe fn emit_spill_pinned(b: *mut A64Buf) {
    unsafe {
        for i in 0..g_n_pinned {
            if rsp_is_ptr() != 0 && *g_pin_hold.add(i as usize) == OCERZ_RSP as u8 {
                a64_mov_imm64(b, JTA as c_int, ocerz_guest_base);
                a64_sub_reg(b, 1, JTA as c_int, pin_hreg(i), JTA as c_int, 0);
                a64_str(b, 8, JTA as c_int, 20, GPR_OFF(OCERZ_RSP as c_uint));
            } else {
                a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(*g_pin_hold.add(i as usize) as c_uint));
            }
        }
    }
}

#[inline(always)]
pub unsafe fn emit_spill_pinned_callersaved(b: *mut A64Buf) {
    unsafe {
        for i in 8..g_n_pinned {
            a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(*g_pin_hold.add(i as usize) as c_uint));
        }
    }
}

#[inline(always)]
pub unsafe fn emit_fill_pinned_callersaved(b: *mut A64Buf) {
    unsafe {
        for i in 8..g_n_pinned {
            a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(*g_pin_hold.add(i as usize) as c_uint));
        }
    }
}

#[inline(always)]
pub unsafe fn emit_fill_pinned(b: *mut A64Buf) {
    unsafe {
        for i in 0..g_n_pinned {
            a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(*g_pin_hold.add(i as usize) as c_uint));
        }
        if rsp_is_ptr() != 0 {
            let s = pin_slot(OCERZ_RSP as c_uint);
            debug_assert!(s >= 0);
            a64_mov_imm64(b, JTA as c_int, ocerz_guest_base);
            a64_add_reg(b, 1, pin_hreg(s), pin_hreg(s), JTA as c_int, 0);
        }
    }
}

#[inline(always)]
pub unsafe fn pin_saved_count() -> c_int {
    unsafe { if g_n_pinned < 8 { g_n_pinned } else { 8 } }
}

#[inline(always)]
pub unsafe fn emit_pin_epilogue_restore(b: *mut A64Buf) {
    unsafe {
        if g_pin_class == 2 {
            a64_ldp_post(b, JRET_GUEST as c_int, JRET_HOST as c_int, 31, 16);
        }
        let ns = pin_saved_count();
        let last = if ns & 1 != 0 { ns - 1 } else { ns - 2 };
        let mut i = last;
        while i >= 0 {
            a64_ldp_post(b, 21 + i, 21 + i + 1, 31, 16);
            i -= 2;
        }
        let mut d = 14;
        while d >= 8 {
            a64_ldp_d_post(b, d, d + 1, 31, 16);
            d -= 2;
        }
    }
}

#[inline(always)]
pub unsafe fn mem_native_store_ok() -> c_int {
    unsafe { (ocerz_watch_addr == 0 && ocerz_watch_val == 0) as c_int }
}

#[inline(always)]
pub unsafe fn ea_fold() -> u64 {
    unsafe {
        if ocerz_low_base != 0 {
            0
        } else {
            ocerz_guest_base
        }
    }
}

#[inline(always)]
pub unsafe fn emit_add_const(b: *mut A64Buf, reg: c_int, c: u64) {
    unsafe {
        if c != 0 {
            a64_mov_imm64(b, JTU as c_int, c);
            a64_add_reg(b, 1, reg, reg, JTU as c_int, 0);
        }
    }
}

#[inline(always)]
pub unsafe fn emit_stack_delta_into(b: *mut A64Buf, rd: c_int) {
    unsafe {
        let hs = pin_hreg(pin_slot(OCERZ_RSP as c_uint));
        a64_lsr_imm(b, 1, rd, hs, 32);
        a64_sub_imm(b, 1, rd, rd, (OCERZ_LOW_LIMIT >> 32) as u32);
        a64_asr_imm(b, 1, rd, rd, 63);
        let _ = a64_try_and_imm(b, 1, rd, rd, ocerz_low_base);
    }
}

#[inline(always)]
pub unsafe fn emit_stack_delta(b: *mut A64Buf) {
    unsafe {
        emit_stack_delta_into(b, JGB as c_int);
    }
}

#[inline(always)]
pub unsafe fn emit_stack_delta_check(b: *mut A64Buf) {
    unsafe {
        emit_stack_delta_into(b, JTT as c_int);
        a64_eor_reg(b, 1, JTT as c_int, JTT as c_int, JGB as c_int, 0);
        let ok = a64_label(b);
        a64_cbz(b, 1, JTT as c_int, 0);
        a64_emit32(b, 0xd4200000 | (0x5d0 << 5));
        a64_patch_cbz(ok, a64_label(b));
    }
}

#[inline(always)]
pub unsafe fn patch_guard_skip(skip: *mut u32, target: *mut u32) {
    unsafe {
        if !skip.is_null() {
            a64_patch_b(skip, target);
        }
    }
}

#[inline(always)]
pub unsafe fn undo_save_hook(b: *mut A64Buf, size: c_int, vd: c_int) {
    unsafe {
        if g_undo_want_slot < 0 || size != g_undo_want_size {
            return;
        }
        a64_v_mov(b, g_undo_vreg[g_undo_want_slot as usize] as c_int, vd);
        g_undo_want_slot = -1;
        g_undo_saved = 1;
    }
}

#[inline(always)]
pub unsafe fn emit_v_ld_at(
    b: *mut A64Buf,
    size: c_int,
    vd: c_int,
    ra: c_int,
    disp: i32,
    plain: c_int,
) {
    unsafe {
        emit_v_ld_at_(b, size, vd, ra, disp, plain);
        undo_save_hook(b, size, vd);
    }
}

#[inline(always)]
pub unsafe fn lowstack_disp_ea(
    b: *mut A64Buf,
    insn: *const X86Insn,
    m: *const X86Operand,
    size: c_int,
    unscaled_ok: c_int,
) -> c_int {
    unsafe {
        if lowstack_disp_ok(insn, m, size, unscaled_ok) == 0 {
            return 0;
        }
        if ea_cache_has_base(b, m) == 0 {
            a64_add_reg(b, 1, JTA as c_int, pin_hreg(pin_slot(OCERZ_RSP as c_uint)), JGB as c_int, 0);
        }
        ea_cache_set_full(b, OCERZ_RSP as c_uint, OCERZ_REG_NONE, 0);
        1
    }
}

#[inline(always)]
pub unsafe fn ea_cache_reset() {
    unsafe {
        g_ea_cache.valid = 0;
    }
}

#[inline(always)]
pub fn a64_word_may_write_reg(w: u32, r: c_uint) -> c_int {
    if (w & 0x1f) == r {
        return 1;
    }
    if (w & 0x3a000000) == 0x28000000 && (w & 0x00400000) != 0 {
        if ((w >> 10) & 0x1f) == r {
            return 1;
        }
    }
    if (w & 0x3f000000) == 0x08000000 && ((w >> 10) & 0x1f) == r {
        return 1;
    }
    0
}

#[inline(always)]
pub unsafe fn ea_cache_has_base(b: *const A64Buf, op: *const X86Operand) -> c_int {
    unsafe {
        if ea_cache_usable(b) == 0 {
            return 0;
        }
        (g_ea_cache.base == (*op).base as c_uint
            && (*op).base != OCERZ_REG_NONE as u8
            && g_ea_cache.index == OCERZ_REG_NONE) as c_int
    }
}

#[inline(always)]
pub unsafe fn ea_cache_set_full(b: *const A64Buf, base: c_uint, index: c_uint, scale: c_int) {
    unsafe {
        g_ea_cache.valid = 1;
        g_ea_cache.base = base;
        g_ea_cache.index = index;
        g_ea_cache.scale = scale & 3;
        g_ea_cache.seq = g_callout_seq;
        g_ea_cache.after = (*b).p;
    }
}

#[inline(always)]
pub unsafe fn ea_cache_step(in_: *const X86Insn, prev: *const X86Insn) {
    unsafe {
        if g_ea_cache.valid == 0 {
            return;
        }
        if g_ea_cache.base != OCERZ_REG_NONE
            && (insn_may_write_gpr(in_, g_ea_cache.base) != 0
                || (!prev.is_null() && insn_may_write_gpr(prev, g_ea_cache.base) != 0))
        {
            g_ea_cache.valid = 0;
            return;
        }
        if g_ea_cache.index != OCERZ_REG_NONE
            && (insn_may_write_gpr(in_, g_ea_cache.index) != 0
                || (!prev.is_null() && insn_may_write_gpr(prev, g_ea_cache.index) != 0))
        {
            g_ea_cache.valid = 0;
        }
    }
}

#[inline(always)]
pub unsafe fn emit_mem_ea_plain(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    size: c_int,
    ra_out: *mut c_int,
    disp_out: *mut u32,
) -> c_int {
    unsafe { emit_mem_ea_plain_ex(b, insn, op, size, ra_out, disp_out, 0) }
}

#[inline(always)]
pub unsafe fn m32_stack_low() -> c_int {
    unsafe { (ocerz_low_base != 0 && low_guard_fast_ok() != 0) as c_int }
}

#[inline(always)]
pub unsafe fn m32_stack_base_ok() -> c_int {
    unsafe {
        if stack_inline_enabled() == 0
            || g_pin_class == 2
            || pin_slot(OCERZ_RSP as c_uint) < 0
            || stack_plain_access_ok() == 0
        {
            return 0;
        }
        if m32_stack_low() != 0 {
            return 1;
        }
        (jgb_usable() != 0 && mem_guard_needed() == 0 && stack_guard_needed() == 0) as c_int
    }
}

#[inline(always)]
pub unsafe fn m32_stack_st(b: *mut A64Buf, rv: c_int, wa: c_int) {
    unsafe {
        if m32_stack_low() == 0 || g_m32low != 0 {
            a64_str_regoff_uxtw(b, 4, rv, JGB as c_int, wa);
            return;
        }
        a64_mov_reg(b, 0, JTU as c_int, wa);
        let _ = a64_try_orr_imm(b, 1, JTU as c_int, JTU as c_int, ocerz_low_base);
        a64_str(b, 4, rv, JTU as c_int, 0);
    }
}

#[inline(always)]
pub unsafe fn m32_stack_ld(b: *mut A64Buf, rd: c_int, wa: c_int) {
    unsafe {
        if m32_stack_low() == 0 || g_m32low != 0 {
            a64_ldr_regoff_uxtw(b, 4, rd, JGB as c_int, wa);
            return;
        }
        a64_mov_reg(b, 0, JTU as c_int, wa);
        let _ = a64_try_orr_imm(b, 1, JTU as c_int, JTU as c_int, ocerz_low_base);
        a64_ldr(b, 4, rd, JTU as c_int, 0);
    }
}

#[inline(always)]
pub unsafe fn m32_stack_ok(insn: *const X86Insn) -> c_int {
    unsafe { ((*insn).seg == OCERZ_SEG_NONE as u8 && m32_stack_base_ok() != 0) as c_int }
}

#[inline(always)]
pub unsafe fn emit_cc_predicate(b: *mut A64Buf, cc: c_uint) {
    unsafe {
        emit_cc_predicate_ex(b, cc, 0);
    }
}

#[inline(always)]
pub fn xmm_vreg(xr: c_uint) -> c_int {
    16 + xr as c_int
}

#[inline(always)]
pub unsafe fn xmm_is_pinned(xr: c_uint) -> c_int {
    unsafe { (((g_xmm_pinned as u32) >> xr) & 1) as c_int }
}

#[inline(always)]
pub unsafe fn emit_xmm_pin_load_all(b: *mut A64Buf) {
    unsafe {
        for r in 0..16u32 {
            if xmm_is_pinned(r) != 0 {
                a64_ldr_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
            }
        }
        emit_pk_consts_load(b);
    }
}

#[inline(always)]
pub unsafe fn emit_xmm_pin_spill_all(b: *mut A64Buf) {
    unsafe {
        for r in 0..16u32 {
            if xmm_is_pinned(r) != 0 {
                a64_str_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
            }
        }
    }
}

#[inline(always)]
pub unsafe fn emit_sse_mem_ld_gpr(b: *mut A64Buf, size: c_int, rd: c_int) {
    unsafe {
        if g_sse_mem_plain != 0 {
            emit_gpr_ld_at(b, size, rd, g_sse_mem_ra, g_sse_mem_disp as i32, g_sse_mem_plainacc);
        } else {
            emit_guest_load_ordered(b, size, rd, JTA as c_int, JTU as c_int);
        }
    }
}

#[inline(always)]
pub unsafe fn emit_sse_mem_ld(b: *mut A64Buf, size: c_int, vd: c_int) {
    unsafe {
        emit_v_ld_at(b, size, vd, g_sse_mem_ra, g_sse_mem_disp as i32, g_sse_mem_plainacc);
    }
}

#[inline(always)]
pub unsafe fn emit_sse_mem_st(b: *mut A64Buf, size: c_int, vs: c_int) {
    unsafe {
        emit_v_st_at(b, size, vs, g_sse_mem_ra, g_sse_mem_disp as i32, g_sse_mem_plainacc);
    }
}

#[inline(always)]
pub unsafe fn emit_sse_mem_st_gpr(b: *mut A64Buf, size: c_int, rv: c_int) {
    unsafe {
        if g_sse_mem_plain != 0 {
            emit_gpr_st_at(b, size, rv, g_sse_mem_ra, g_sse_mem_disp as i32, g_sse_mem_plainacc);
        } else {
            emit_guest_store_ordered(b, size, rv, JTA as c_int, JTU as c_int);
        }
    }
}

#[inline(always)]
pub unsafe fn emit_mem_load_any(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    size: c_int,
    rd: c_int,
) -> c_int {
    unsafe {
        if emit_mem_load_plain(b, insn, op, size, rd) != 0 {
            return 1;
        }
        let mut skip: *mut u32 = core::ptr::null_mut();
        if emit_sse_mem_addr(b, insn, op, size, core::ptr::null_mut(), core::ptr::null_mut(), &mut skip)
            == 0
        {
            return 0;
        }
        emit_sse_mem_ld_gpr(b, size, rd);
        patch_guard_skip(skip, a64_label(b));
        1
    }
}

#[inline(always)]
pub unsafe fn scalar_cvt_follows(xreg: c_uint, dbl: c_int) -> c_int {
    unsafe {
        if dbl == 0
            || g_cur_insns.is_null()
            || g_cur_insn_idx < 0
            || g_cur_insn_idx + 1 >= g_cur_insns_n
        {
            return 0;
        }
        if g_n_nanool + 2 > NANOOL_MAX as c_int || unsafe_nocheckbr() != 0 {
            return 0;
        }
        let c = g_cur_insns.add((g_cur_insn_idx + 1) as usize);
        if (*c).vex != 0 || (*g_cur_insns.add(g_cur_insn_idx as usize)).vex != 0 {
            return 0;
        }
        if (*c).op != OCERZ_OP_CVTTSD2SI as u16 || (*c).nops != 2 {
            return 0;
        }
        let d = &(*c).ops[0];
        let sr = &(*c).ops[1];
        if sr.kind != OCERZ_OPK_XMM as u8 || sr.reg as c_uint != xreg || xmm_is_pinned(xreg) == 0 {
            return 0;
        }
        if d.kind != OCERZ_OPK_REG as u8 || d.high8 != 0 || (d.size != 4 && d.size != 8) {
            return 0;
        }
        if rsp_is_ptr() != 0 && d.reg == OCERZ_RSP as u8 {
            return 0;
        }
        1
    }
}

#[inline(always)]
pub unsafe fn scalar_pend_flush(b: *mut A64Buf) {
    unsafe {
        if g_scpend.valid == 0 {
            return;
        }
        g_scpend.valid = 0;
        g_scalar_merge_next = 0;
        emit_nan_fix_scalar2(b, g_scpend.dbl, g_scpend.vr, g_scpend.va, g_scpend.vb);
    }
}

#[inline(always)]
pub unsafe fn emit_pk_consts_load(_b: *mut A64Buf) {
}

#[inline(always)]
pub unsafe fn fpb_undo_clear(i: c_int) {
    unsafe {
        g_fpb_undo[i as usize] = 0;
        g_fpb_undo_size[i as usize] = 0;
        g_fpb_undo_done[i as usize] = 0;
        g_fpb_undo_ld[i as usize] = 0;
        g_fpb_undo_ldsz[i as usize] = 0;
        g_fpb_undo_ldst[i as usize] = -1;
        g_fpb_undo_from[i as usize] = -1;
    }
}

#[inline(always)]
pub unsafe fn lane_reserve() -> c_int {
    unsafe {
        let mut lane: c_int = -1;
        let mut k = L0_NLANES as c_int - 1;
        while k >= 0 {
            if g_lane_used & (1u16 << k) == 0 {
                lane = k;
                break;
            }
            k -= 1;
        }
        if lane < 0 {
            return -1;
        }
        if g_l0_fixed == 0 {
            if lane != g_l0_nlanes - 1 || g_l0_nlanes <= 4 {
                return -1;
            }
            g_l0_nlanes -= 1;
        }
        g_lane_used |= 1u16 << lane;
        4 + lane
    }
}

#[inline(always)]
pub unsafe fn fpb_scan(insns: *const X86Insn, n: c_int, bat: *mut i8) {
    unsafe {
        if fpb_v1() != 0 || g_xlat_mode32 != 0 {
            g_fpb_exit_mask = 0;
            g_fpb_exit_batch = -1;
            for i in 0..n {
                g_fpb_stchk[i as usize] = 0;
                g_fpb_stlane[i as usize] = 0;
                fpb_undo_clear(i);
            }
            g_fpb_v1_active = 1;
            fpb_scan_v1(insns, n, bat);
            g_fpb_v1_active = 0;
            return;
        }
        fpb_scan_v2(insns, n, bat);
    }
}

#[inline(always)]
pub unsafe fn fpb_replay_prelude(
    b: *mut A64Buf,
    fb: *const FpBatch,
    l0: *const i8,
    l0_dbl: *const u8,
) {
    unsafe {
        for r in 0..16u32 {
            if (*fb).ckpt_emit & (1u16 << r) != 0 {
                a64_ldr_v(b, 16, xmm_vreg(r), 20, FPCKPT_OFF + r * 16);
            }
        }
        for r in 0..16usize {
            if ((*fb).dirty_open & !(*fb).written & !(*fb).ckpt_emit & (1u16 << r)) != 0
                && *l0.add(r) >= 0
            {
                if *l0_dbl.add(r) != 0 {
                    a64_ins_d_d(b, xmm_vreg(r as c_uint), 0, *l0.add(r) as c_int, 0);
                } else {
                    a64_ins_s_s(b, xmm_vreg(r as c_uint), 0, *l0.add(r) as c_int, 0);
                }
            }
        }
    }
}

#[inline(always)]
pub unsafe fn fpb_site_emit(b: *mut A64Buf, end: c_int, va: c_int, vb: c_int, dbl: c_int) {
    unsafe {
        if g_n_fpb_sites >= FPB_SITES_MAX as c_int {
            return;
        }
        let st = &mut g_fpb_sites[g_n_fpb_sites as usize];
        g_n_fpb_sites += 1;
        st.batch = g_fpb_open;
        st.end = end;
        st.site = a64_label(b);
        a64_bcond(b, A64_VS as c_int, 0);
        st.back = a64_label(b);
        for r in 0..16usize {
            st.l0[r] = g_l0[r];
            st.l0_dbl[r] = g_l0_dbl[r];
        }
        st.fcmp_a = va as i8;
        st.fcmp_b = vb as i8;
        st.fcmp_dbl = dbl as u8;
        st.keep_jt = 0;
    }
}

#[inline(always)]
pub unsafe fn fpb_det_here(idx: c_int) -> c_int {
    unsafe {
        (g_fpb_open >= 0
            && !g_fpb_of.is_null()
            && idx >= 0
            && g_fpb_det[idx as usize] != 0
            && *g_fpb_of.add(idx as usize) as c_int == g_fpb_open) as c_int
    }
}

#[inline(always)]
pub unsafe fn yc_flush_all(b: *mut A64Buf) {
    unsafe {
        for r in 0..16u32 {
            if (g_yc_dirty & (1u16 << r)) != 0 && g_yc[r as usize] >= 0 {
                a64_str_v(b, 16, g_yc[r as usize] as c_int, 20, YMMH_OFF + r * 16);
            }
        }
        g_yc_dirty = 0;
    }
}

#[inline(always)]
pub unsafe fn yc_flush_from(b: *mut A64Buf, dirty: u16) {
    unsafe {
        for r in 0..16u32 {
            if (dirty & (1u16 << r)) != 0 && g_yc[r as usize] >= 0 {
                a64_str_v(b, 16, g_yc[r as usize] as c_int, 20, YMMH_OFF + r * 16);
            }
        }
    }
}

#[inline(always)]
pub unsafe fn yc_reload_all(b: *mut A64Buf) {
    unsafe {
        for r in 0..16u32 {
            if g_yc[r as usize] >= 0 {
                a64_ldr_v(b, 16, g_yc[r as usize] as c_int, 20, YMMH_OFF + r * 16);
            }
        }
    }
}

#[inline(always)]
pub unsafe fn l0_reset() {
    unsafe {
        for i in 0..16usize {
            g_l0[i] = -1;
        }
        for i in 0..L0_NLANES as usize {
            g_l0_owners[i] = 0;
        }
        g_l0_dirty = 0;
    }
}

#[inline(always)]
pub unsafe fn l0_flush_reg(b: *mut A64Buf, r: c_uint) {
    unsafe {
        if g_l0_dirty & (1u16 << r) == 0 {
            return;
        }
        g_l0_dirty &= !(1u16 << r);
        if g_l0[r as usize] < 0 {
            return;
        }
        if g_l0_dbl[r as usize] != 0 {
            a64_ins_d_d(b, xmm_vreg(r), 0, g_l0[r as usize] as c_int, 0);
        } else {
            a64_ins_s_s(b, xmm_vreg(r), 0, g_l0[r as usize] as c_int, 0);
        }
    }
}

#[inline(always)]
pub unsafe fn l0_flush_all(b: *mut A64Buf) {
    unsafe {
        while g_l0_dirty != 0 {
            l0_flush_reg(b, g_l0_dirty.trailing_zeros());
        }
        yc_flush_all(b);
    }
}

#[inline(always)]
pub unsafe fn l0_defer_take(vs: c_int, xr: c_uint, size: c_int) -> c_int {
    unsafe {
        if g_xlat_mode32 != 0 {
            return 0;
        }
        if !(l0_defer() != 0
            && l0_enabled() != 0
            && vs >= 4
            && vs < 4 + L0_NLANES as c_int
            && g_l0[xr as usize] as c_int == vs
            && g_l0_dbl[xr as usize] == (size == 8) as u8)
        {
            return 0;
        }
        g_l0_dirty |= 1u16 << xr;
        1
    }
}

#[inline(always)]
pub unsafe fn l0_inval(r: c_uint) {
    unsafe {
        if r < 16 && g_l0[r as usize] >= 0 {
            g_l0_owners[g_l0[r as usize] as usize - 4] &= !(1u16 << r);
            g_l0[r as usize] = -1;
        }
        g_l0_dirty &= !(1u16 << r);
    }
}

#[inline(always)]
pub unsafe fn l0_fixed_map() {
    unsafe {
        g_l0_dirty = 0;
        for r in 0..16usize {
            if g_l0_fixed_lane[r] >= 0 {
                g_l0[r] = g_l0_fixed_lane[r];
                g_l0_dbl[r] = g_l0_fixed_dbl[r];
                g_l0_owners[g_l0_fixed_lane[r] as usize - 4] = 1u16 << r;
                g_lane_used |= 1u16 << (g_l0_fixed_lane[r] as u32 - 4);
                if g_l0_fixed_dirty & (1u16 << r) != 0 {
                    g_l0_dirty |= 1u16 << r;
                }
            }
        }
    }
}

#[inline(always)]
pub unsafe fn l0_src2(b: *mut A64Buf, r: c_uint, dbl: c_int) -> c_int {
    unsafe {
        if l0_enabled() != 0 && g_l0[r as usize] >= 0 && g_l0_dbl[r as usize] == dbl as u8 {
            return g_l0[r as usize] as c_int;
        }
        l0_flush_reg(b, r);
        xmm_vreg(r)
    }
}

#[inline(always)]
pub unsafe fn l0_src(b: *mut A64Buf, r: c_uint, dbl: c_int) -> c_int {
    unsafe { l0_src2(b, r, dbl) }
}

#[inline(always)]
pub unsafe fn fpb_emit_undo_save(
    b: *mut A64Buf,
    insn: *const X86Insn,
    i: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) {
    unsafe {
        let size = g_fpb_undo_size[i as usize] as c_int;
        let vr = g_undo_vreg[g_fpb_undo[i as usize] as usize - 1] as c_int;
        if emit_plain_mem_fast(b, insn, &(*insn).ops[0], size, vr, 0, 1) != 0 {
            return;
        }
        let mut skip: *mut u32 = core::ptr::null_mut();
        if emit_sse_mem_addr(b, insn, &(*insn).ops[0], size, exit_sites, n_exits, &mut skip) == 0 {
            g_fpb_undo[i as usize] = 0;
            return;
        }
        emit_sse_mem_ld(b, size, vr);
        patch_guard_skip(skip, a64_label(b));
    }
}

#[inline(always)]
pub unsafe fn fpb_emit_undo_restore(
    b: *mut A64Buf,
    insns: *const X86Insn,
    first: c_int,
    end: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) {
    unsafe {
        let mut m = end;
        while m >= first {
            if g_fpb_undo[m as usize] == 0 {
                m -= 1;
                continue;
            }
            let size = g_fpb_undo_size[m as usize] as c_int;
            let vr = g_undo_vreg[g_fpb_undo[m as usize] as usize - 1] as c_int;
            if emit_plain_mem_fast(b, insns.add(m as usize), &(*insns.add(m as usize)).ops[0], size, vr, 1, 1)
                != 0
            {
                m -= 1;
                continue;
            }
            let mut skip: *mut u32 = core::ptr::null_mut();
            if emit_sse_mem_addr(
                b,
                insns.add(m as usize),
                &(*insns.add(m as usize)).ops[0],
                size,
                exit_sites,
                n_exits,
                &mut skip,
            ) == 0
            {
                m -= 1;
                continue;
            }
            emit_sse_mem_st(b, size, vr);
            patch_guard_skip(skip, a64_label(b));
            m -= 1;
        }
    }
}

#[inline(always)]
pub unsafe fn fpb_emit_exit_check(b: *mut A64Buf) {
    unsafe {
        if g_fpb_exit_batch < 0 || g_fpb_exit_mask == 0 {
            return;
        }
        for r in 0..16u32 {
            if g_fpb_exit_mask & (1u16 << r) != 0 {
                l0_flush_reg(b, r);
            }
        }
        let before = g_n_fpb_sites;
        fpb_emit_regs_check(
            b,
            g_fpb_exit_mask,
            g_fpb_exit_batch,
            g_fpb_exit_end,
            core::ptr::addr_of!(g_l0) as *const i8,
            core::ptr::addr_of!(g_l0_dbl) as *const u8,
        );
        if g_n_fpb_sites > before {
            g_fpb_sites[g_n_fpb_sites as usize - 1].keep_jt = 1;
        }
        g_fpb_exit_mask = 0;
    }
}

#[inline(always)]
pub unsafe fn l0_share(dst: c_uint, src: c_uint) {
    unsafe {
        l0_inval(dst);
        if l0_enabled() != 0 && g_l0[src as usize] >= 0 {
            g_l0[dst as usize] = g_l0[src as usize];
            g_l0_dbl[dst as usize] = g_l0_dbl[src as usize];
            g_l0_owners[g_l0[src as usize] as usize - 4] |= 1u16 << dst;
            if g_l0_dirty & (1u16 << src) != 0 {
                g_l0_dirty |= 1u16 << dst;
            }
        }
    }
}

#[inline(always)]
pub unsafe fn l0_fixed_backedge(b: *mut A64Buf) {
    unsafe {
        if g_l0_fixed != 0 {
            l0_fixed_restore(b);
        }
    }
}

#[inline(always)]
pub unsafe fn l0_fixed_fallthrough(b: *mut A64Buf) {
    unsafe {
        if g_l0_fixed != 0 {
            l0_flush_all(b);
        }
    }
}

#[inline(always)]
pub unsafe fn x87_reset() {
    unsafe {
        g_x87_live = 0;
        g_x87_delta = 0;
        g_n_x87_run = 0;
        g_x87_cur = -1;
        g_n_x87_site = 0;
        g_n_x87_frag = 0;
        g_x87_frag_open = 0;
        g_x87_nzcv = -1;
    }
}

#[inline(always)]
pub unsafe fn x87_inline_ok(insn: *const X86Insn) -> c_int {
    unsafe { (x87_run_flags(insn) != 0) as c_int }
}

#[inline(always)]
pub unsafe fn emit_prof_count(b: *mut A64Buf, pf: *mut JitProf, off: u32) {
    unsafe {
        tc_imm64(
            b,
            JT1 as c_int,
            TCR_PROF as c_int,
            (pf as *const u8).offset_from((*g_cur_blk).prof as *const u8) as u64,
            pf as usize as u64,
        );
        a64_ldr(b, 4, JT2 as c_int, JT1 as c_int, off);
        a64_add_imm(b, 0, JT2 as c_int, JT2 as c_int, 1);
        a64_str(b, 4, JT2 as c_int, JT1 as c_int, off);
    }
}

#[inline(always)]
pub unsafe fn emit_side_tag(b: *mut A64Buf, cpu_reg: c_int) {
    unsafe {
        if g_tag_blk.is_null() {
            return;
        }
        tc_imm64(b, JT0 as c_int, TCR_BLK as c_int, 0, g_tag_blk as usize as u64);
        a64_str(b, 8, JT0 as c_int, cpu_reg, SIDE_BLK_OFF);
        a64_mov_imm64(b, JT0 as c_int, g_tag_idx as u64);
        a64_str(b, 4, JT0 as c_int, cpu_reg, SIDE_IDX_OFF);
    }
}

#[inline(always)]
pub unsafe fn side_stub_has_work(k: c_int) -> c_int {
    unsafe {
        if g_side[k as usize].rec != 0 {
            return 1;
        }
        if g_side[k as usize].fpb >= 0 && g_side[k as usize].fpb_chk != 0 {
            return 1;
        }
        for r in 0..16u32 {
            if (g_side[k as usize].l0_dirty & (1u16 << r)) != 0 && g_side[k as usize].l0[r as usize] >= 0 {
                return 1;
            }
        }
        0
    }
}

#[inline(always)]
pub unsafe fn can_fuse_incdec_jcc(producer: *const X86Insn, jcc: *const X86Insn) -> c_int {
    unsafe {
        if g_no_jccfuse != 0
            || g_no_regflags != 0
            || g_no_chain != 0
            || g_no_jcclink != 0
            || (*jcc).op != OCERZ_OP_JCC as u16
            || (*jcc).ops[0].kind != OCERZ_OPK_IMM as u8
            || ((*jcc).cc != OCERZ_CC_E as u8 && (*jcc).cc != OCERZ_CC_NE as u8)
            || ((*producer).op != OCERZ_OP_INC as u16 && (*producer).op != OCERZ_OP_DEC as u16)
        {
            return 0;
        }
        let d = &(*producer).ops[0];
        (d.kind == OCERZ_OPK_REG as u8 && d.high8 == 0 && (d.size == 4 || d.size == 8)) as c_int
    }
}

#[inline(always)]
pub unsafe fn ras_body_only() -> c_int {
    unsafe {
        (fullpin_enabled() != 0
            && g_no_regflags == 0
            && stack_plain_access_ok() != 0
            && stack_fast() != 0
            && g_no_chain == 0
            && g_no_ras == 0) as c_int
    }
}

#[inline(always)]
pub unsafe fn ras_entry_for(blk: *const JitBlock) -> *mut c_void {
    unsafe {
        if blk.is_null() || (*blk).code.is_none() {
            return core::ptr::null_mut();
        }
        if ras_body_only() != 0 {
            return if (*blk).pin_class == 3 && !(*blk).body_code.is_null() {
                (*blk).body_code as *mut c_void
            } else {
                core::ptr::null_mut()
            };
        }
        if (*blk).pin_class == 3 && !(*blk).body_code.is_null() {
            return ((*blk).body_code as usize | 1) as *mut c_void;
        }
        (*blk).code.unwrap() as usize as *mut c_void
    }
}

#[inline(always)]
pub unsafe fn emit_static_chain_tail(
    b: *mut A64Buf,
    target_rip: u64,
    poll: c_int,
    body_edge: c_int,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
) -> *mut u32 {
    unsafe {
        if body_edge != 0 {
            return emit_body_chain_tail(b, target_rip, poll, epilogue_sites, n_epi);
        }
        a64_mov_imm64(b, JT0 as c_int, target_rip);
        a64_str(b, 8, JT0 as c_int, 20, RIP_OFF);
        emit_chain_tail(b, poll)
    }
}

#[inline(always)]
pub unsafe fn mark_has(m: *mut MarkSet, key: u64) -> c_int {
    unsafe {
        if (*m).full != 0 {
            return 1;
        }
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as c_uint) & (LOWHOIST_N - 1);
        for _ in 0..LOWHOIST_N {
            let v = at64(&raw const (*m).off[i as usize]).load(Ordering::Relaxed);
            if v == key {
                return 1;
            }
            if v == 0 {
                return 0;
            }
            i = (i + 1) & (LOWHOIST_N - 1);
        }
        0
    }
}

#[inline(always)]
pub unsafe fn mark_add(m: *mut MarkSet, key: u64) {
    unsafe {
        let mut i = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as c_uint) & (LOWHOIST_N - 1);
        for _ in 0..LOWHOIST_N {
            let slot = &(*m).off[i as usize];
            let v = at64(slot).load(Ordering::Relaxed);
            if v == key {
                return;
            }
            if v == 0
                && at64(slot)
                    .compare_exchange_weak(v, key, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                return;
            }
            i = (i + 1) & (LOWHOIST_N - 1);
        }
        (*m).full = 1;
    }
}

#[inline(always)]
pub unsafe fn lowhoist_mark(key: u64) {
    unsafe {
        mark_add(&raw mut g_lowhoist_marks, key);
    }
}

#[inline(always)]
pub unsafe fn emit_l0_flush_from(
    b: *mut A64Buf,
    l0: *const i8,
    l0_dbl: *const u8,
    dirty: u16,
) {
    unsafe {
        for r in 0..16usize {
            if (dirty & (1u16 << r)) != 0 && *l0.add(r) >= 0 {
                if *l0_dbl.add(r) != 0 {
                    a64_ins_d_d(b, xmm_vreg(r as c_uint), 0, *l0.add(r) as c_int, 0);
                } else {
                    a64_ins_s_s(b, xmm_vreg(r as c_uint), 0, *l0.add(r) as c_int, 0);
                }
            }
        }
    }
}

#[inline(always)]
pub unsafe fn emit_l0_reload_from(b: *mut A64Buf, l0: *const i8, l0_dbl: *const u8) {
    unsafe {
        for r in 0..16usize {
            if *l0.add(r) >= 0 {
                if *l0_dbl.add(r) != 0 {
                    a64_fmov_d_d(b, *l0.add(r) as c_int, xmm_vreg(r as c_uint));
                } else {
                    a64_fmov_s_s(b, *l0.add(r) as c_int, xmm_vreg(r as c_uint));
                }
            }
        }
    }
}

#[inline(always)]
pub unsafe fn emit_slowcall_keep_lanes(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) {
    unsafe {
        let dirty = g_l0_dirty;
        emit_slowcall(b, insn, exit_sites, n_exits);
        emit_l0_reload_from(b, core::ptr::addr_of!(g_l0) as *const i8, core::ptr::addr_of!(g_l0_dbl) as *const u8);
        g_l0_dirty = dirty;
    }
}

#[inline(always)]
pub unsafe fn patch_any_branch(site: *mut u32, target: *mut u32) {
    unsafe {
        let w = *site;
        if (w & 0x7e000000) == 0x34000000 {
            a64_patch_cbz(site, target);
        } else if (w & 0x7e000000) == 0x36000000 {
            a64_patch_tbz(site, target);
        } else if (w & 0xff000010) == 0x54000000 {
            a64_patch_bcond(site, target);
        } else {
            a64_patch_b(site, target);
        }
    }
}

#[inline(always)]
pub unsafe fn emit_misaligned_pieces_st(
    b: *mut A64Buf,
    psize: c_int,
    n: c_int,
    rv: c_int,
    ra: c_int,
    disp: i32,
    s1: c_int,
) {
    unsafe {
        a64_stlur(b, psize, rv, ra, disp);
        for i in 1..n {
            a64_lsr_imm(b, 1, s1, rv, (i * psize * 8) as c_int);
            a64_stlur(b, psize, s1, ra, disp + i * psize);
        }
    }
}

#[inline(always)]
pub unsafe fn tc_log_init() {
    unsafe {
        if g_tc_log >= 0 {
            return;
        }
        let lp = libc::getenv(b"OCERZ_TCACHE_LOG\0".as_ptr() as *const _);
        g_tc_lf = if !lp.is_null() && libc::strcmp(lp, c"1".as_ptr()) != 0 {
            libc::fopen(lp, c"a".as_ptr()) as *mut FILE
        } else {
            core::ptr::null_mut()
        };
        if !g_tc_lf.is_null() {
            libc::setvbuf(g_tc_lf as *mut libc::FILE, core::ptr::null_mut(), libc::_IOLBF, 0);
        }
        if g_tc_lf.is_null() {
            g_tc_lf = crate::log::stderr() as *mut FILE;
        }
        g_tc_log = if lp.is_null() { 0 } else { 1 };
        if g_tc_log != 0 {
            extern "C" fn summary_trampoline() {
                unsafe { tc_summary() }
            }
            libc::atexit(summary_trampoline);
        }
    }
}

#[inline(always)]
pub unsafe fn tc_usable(jit: *const OcerzJit) -> c_int {
    unsafe {
        (ocerz_mode != OCERZ_MODE_NATIVE as c_int
            && ocerz_jitstat <= 0
            && ocerz_perfstat <= 0
            && (*jit).stop_requested == 0) as c_int
    }
}

#[inline(always)]
pub unsafe fn tc_keepable(rip: u64) -> c_int {
    unsafe {
        if ocerz_guest_base != 0 {
            return 0;
        }
        (ocerz_low_base != 0 || ocerz_cache_region(ocerz_g2h(rip) as usize) != 0) as c_int
    }
}

#[inline(always)]
pub unsafe fn tc_key(rip: u64, mode32: c_int) -> u64 {
    unsafe {
        rip | (if mode32 != 0 { OCERZ_TC_KEY_M32 as u64 } else { 0 })
            | (if g_plain_mem != 0 { OCERZ_TC_KEY_PLAIN } else { 0 })
    }
}

#[inline(always)]
pub fn psc_col(key: u64) -> c_uint {
    (key >> 2) as c_uint & (PSC_N - 1)
}

#[inline(always)]
pub unsafe fn steplog(cpu: *const OcerzCPU) {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"STEP %#llx".as_ptr(),
            (*cpu).rip as libc::c_ulonglong,
        );
        for i in 0..16usize {
            libc::fprintf(
                crate::log::stderr(),
                c" %llx".as_ptr(),
                (*cpu).gpr[i] as libc::c_ulonglong,
            );
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
}
