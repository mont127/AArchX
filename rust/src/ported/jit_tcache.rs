//! ---- kept translations ----
//! Unless OCERZ_TCACHE=off, a translation is written to src/tcache.c's store,
//! and the next process to need the same key loads it instead of translating.
//! Only code whose address is the same from run to run is kept: everything in
//! a Wine process, whose low-shadow layout and fixed image bases make it so,
//! and otherwise only the shared cache, since a main program, a dylib or a JIT
//! loaded at a random address would fill the store with records nobody loads,
//! and nothing at all when the guest's own base is chosen at random.  The
//! code is emitted so that it can move: every value that differs between
//! processes (an ocerz function or global, the commpage delta, the block's own
//! JitBlock, an instruction or profile slot inside it, a RAS slot, a
//! return-address cell, an indirect-call cache table, the leaf routines and the
//! dispatch stub) is loaded by a fixed movz and three movk or sits in a literal
//! cell, and translate records where (TcReloc); a leaf call and the
//! dispatch-stub exit go through a register instead of a direct branch.  A load
//! copies the code to the same address modulo 64, because the literal pool's
//! padding and a loop head's alignment are absolute, and fills every site from
//! the current process, allocating slots, cells and tables afresh.  A record
//! also names every guest byte range the translation read through jit_decode -
//! the block, the callees spliced into it, and the successors whose liveness it
//! relied on, which is why a recording translation decodes a successor itself
//! rather than trusting its entry_live, and why an xlive memo entry keeps the
//! ranges and a hash of the bytes it read, replaying them into the record when
//! it answers - and a hash of those bytes, which a load checks against guest
//! memory first.  The key adds
//! the memory model to jit_key, since a block translates differently once a
//! second thread makes loads ordered.  A translation made because the process
//! learned something (a flipped branch, the guard a commpage or alignment fault
//! asked for) is never answered from the store: a key that carries a mark, or
//! whose block was retired or invalidated, is translated again, and the new
//! record replaces the old.  OCERZ_TCACHE=verify translates everything anyway
//! and compares it with the record it would have loaded, counting a difference
//! in shape as a variant and any other as a bug; OCERZ_TCACHE=roundtrip writes
//! nothing, moves every translation to a fresh address, fills the original with
//! BRK, and reports a PC-relative reference that leaves the block or a
//! host-looking constant that no relocation covers.
//! tc_deps_check's sigsetjmp lives in src/jit_tcache_shim.c.

use core::ffi::{c_int, c_void};
use core::mem::{size_of, transmute, zeroed};
use core::ptr::{self, addr_of, addr_of_mut};
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use crate::ffi;
use crate::jit_internal::{
    blk_mode32, blk_rip, jit_key, ocerz_g2h, ras_cell_register, ras_entry_for, tc_noload_has,
};

const TC_RELOC_MAX: usize = ffi::TC_RELOC_MAX as usize;
const TC_DLOG_MAX: usize = ffi::TC_DLOG_MAX as usize;
const TC_DBYTES_MAX: usize = ffi::TC_DBYTES_MAX as usize;
const TC_OUT_MAX: usize = ffi::TC_OUT_MAX as usize;
const JIT_MAX_BLOCK_INSNS: u32 = ffi::JIT_MAX_BLOCK_INSNS;
const JIT_MAX_EDGES: usize = ffi::JIT_MAX_EDGES as usize;
const SIDE_MAX: usize = ffi::SIDE_MAX as usize;
const RAS_SLOT_CAP: usize = ffi::RAS_SLOT_CAP as usize;
const TC_NONE: u32 = ffi::TC_NONE;
const TC_HASH_SEED: u64 = ffi::TC_HASH_SEED;
const TCF_COMPACT: u8 = ffi::TCF_COMPACT as u8;
const TCF_FAULTF: u8 = ffi::TCF_FAULTF as u8;
const TCF_PROF: u8 = ffi::TCF_PROF as u8;
const TCF_LEARNED: u8 = ffi::TCF_LEARNED as u8;
const OCERZ_COMMPAGE_LO: u64 = ffi::OCERZ_COMMPAGE_LO;

#[unsafe(no_mangle)]
pub static mut g_tc_rel: [ffi::TcReloc; TC_RELOC_MAX] = [ffi::TcReloc {
    off: 0,
    kind: 0,
    form: 0,
    arg: 0,
}; TC_RELOC_MAX];
#[unsafe(no_mangle)]
pub static mut g_tc_nrel: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_on: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_bad: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_entry: *const u32 = ptr::null();
#[unsafe(no_mangle)]
pub static mut g_tc_dlog: [ffi::JitState_g_tc_dlog; TC_DLOG_MAX] = [ffi::JitState_g_tc_dlog {
    pc: 0,
    at: 0,
    len: 0,
}; TC_DLOG_MAX];
#[unsafe(no_mangle)]
pub static mut g_tc_dbytes: [u8; TC_DBYTES_MAX] = [0; TC_DBYTES_MAX];
#[unsafe(no_mangle)]
pub static mut g_tc_nbytes: u32 = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_dbar: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_learned: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_noload: *mut u64 = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_tc_noload_cap: usize = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_noload_n: usize = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_jitstat: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_tc_pool_off: u32 = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_log: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_tc_lf: *mut libc::FILE = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_tc_key: u64 = 0;

static mut g_tc_val: [u64; TC_RELOC_MAX] = [0; TC_RELOC_MAX];
static mut g_tc_mdep: [ffi::TcDep; TC_DLOG_MAX] = [ffi::TcDep { lo: 0, hi: 0 }; TC_DLOG_MAX];
static mut g_tc_mbytes: [u8; TC_DBYTES_MAX] = [0; TC_DBYTES_MAX];
static mut g_tc_out: *mut u8 = ptr::null_mut();
static mut g_tc_opos: usize = 0;

#[inline(always)]
fn tc_val_ptr() -> *mut u64 {
    std::ptr::addr_of_mut!(g_tc_val).cast()
}

#[inline(always)]
fn tc_rel_ptr() -> *mut ffi::TcReloc {
    std::ptr::addr_of_mut!(g_tc_rel).cast()
}

#[inline(always)]
fn tc_dlog_ptr() -> *mut ffi::JitState_g_tc_dlog {
    std::ptr::addr_of_mut!(g_tc_dlog).cast()
}

#[inline(always)]
fn tc_dbytes_ptr() -> *mut u8 {
    std::ptr::addr_of_mut!(g_tc_dbytes).cast()
}

#[inline(always)]
fn tc_mdep_ptr() -> *mut ffi::TcDep {
    std::ptr::addr_of_mut!(g_tc_mdep).cast()
}

#[inline(always)]
fn tc_mbytes_ptr() -> *mut u8 {
    std::ptr::addr_of_mut!(g_tc_mbytes).cast()
}

static g_tc_n_bad: AtomicU64 = AtomicU64::new(0);
static g_tc_n_const: AtomicU64 = AtomicU64::new(0);
static g_tc_n_full: AtomicU64 = AtomicU64::new(0);
static g_tc_n_mism: AtomicU64 = AtomicU64::new(0);
static g_tc_n_ok: AtomicU64 = AtomicU64::new(0);
static g_tc_n_pcrel: AtomicU64 = AtomicU64::new(0);
static g_tc_n_load: AtomicU64 = AtomicU64::new(0);
static g_tc_n_put: AtomicU64 = AtomicU64::new(0);
static g_tc_n_rej: AtomicU64 = AtomicU64::new(0);
static g_tc_n_stale: AtomicU64 = AtomicU64::new(0);
static g_tc_n_vbad: AtomicU64 = AtomicU64::new(0);
static g_tc_n_vok: AtomicU64 = AtomicU64::new(0);
static g_tc_n_vvar: AtomicU64 = AtomicU64::new(0);
static TC_REPORT_N: AtomicI32 = AtomicI32::new(0);
static TC_VERIFY_NREP: AtomicI32 = AtomicI32::new(0);
static mut TC_NO_DISPATCH_STUB: c_int = -1;
static mut TC_SELF_BASE: *const c_void = ptr::null();

#[repr(C)]
struct TcDepsCheckState {
    d: *const ffi::TcDep,
    n: u32,
    h: u64,
    good: c_int,
}

unsafe extern "C" {
    fn ocerz_tc_guard(fn_: unsafe extern "C" fn(*mut c_void), arg: *mut c_void) -> c_int;
    fn pthread_jit_write_protect_np(enabled: c_int);
    fn sys_icache_invalidate(start: *mut c_void, len: usize);
}

#[inline(always)]
unsafe fn tc_copy(dst: *mut c_void, src: *const c_void, n: usize) {
    unsafe {
        if n != 0 {
            ptr::copy_nonoverlapping(src.cast::<u8>(), dst.cast::<u8>(), n);
        }
    }
}

#[inline(always)]
unsafe fn tc_zero(dst: *mut c_void, n: usize) {
    unsafe {
        if n != 0 {
            ptr::write_bytes(dst.cast::<u8>(), 0, n);
        }
    }
}

#[inline(always)]
unsafe fn tc_env_on() -> c_int {
    unsafe {
        if TC_NO_DISPATCH_STUB < 0 {
            TC_NO_DISPATCH_STUB =
                (!libc::getenv(c"OCERZ_NO_DISPATCH_STUB".as_ptr()).is_null()) as c_int;
        }
        TC_NO_DISPATCH_STUB
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_decode(pc: u64, out: *mut ffi::X86Insn, mode32: c_int) -> c_int {
    unsafe {
        let code = ocerz_g2h(pc).cast::<u8>();
        let rc = ffi::ocerz_decode_mode(code, 15, pc, out, mode32);
        if rc == ffi::OCERZ_OK && ffi::g_tc_rec != 0 {
            let last = ffi::g_tc_ndlog - 1;
            if last >= g_tc_dbar
                && (*tc_dlog_ptr().wrapping_offset(last as isize))
                    .pc
                    .wrapping_add((*tc_dlog_ptr().wrapping_offset(last as isize)).len as u64)
                    == pc
                && (*tc_dlog_ptr().wrapping_offset(last as isize))
                    .at
                    .wrapping_add((*tc_dlog_ptr().wrapping_offset(last as isize)).len)
                    == g_tc_nbytes
                && g_tc_nbytes.wrapping_add((*out).len as u32) <= TC_DBYTES_MAX as u32
            {
                tc_copy(
                    tc_dbytes_ptr().wrapping_add(g_tc_nbytes as usize).cast(),
                    code.cast(),
                    (*out).len as usize,
                );
                (*tc_dlog_ptr().wrapping_offset(last as isize)).len += (*out).len as u32;
                g_tc_nbytes += (*out).len as u32;
            } else if ffi::g_tc_ndlog < TC_DLOG_MAX as c_int
                && g_tc_nbytes.wrapping_add((*out).len as u32) <= TC_DBYTES_MAX as u32
            {
                let log = tc_dlog_ptr().wrapping_offset(ffi::g_tc_ndlog as isize);
                (*log).pc = pc;
                (*log).at = g_tc_nbytes;
                (*log).len = (*out).len as u32;
                tc_copy(
                    tc_dbytes_ptr().wrapping_add(g_tc_nbytes as usize).cast(),
                    code.cast(),
                    (*out).len as usize,
                );
                ffi::g_tc_ndlog += 1;
                g_tc_nbytes += (*out).len as u32;
            } else {
                g_tc_bad = 1;
            }
        }
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_noload_add(key: u64) {
    unsafe {
        if key == 0
            || ffi::ocerz_tcache_mode() != ffi::OCERZ_TC_ON as c_int
            || tc_noload_has(key) != 0
        {
            return;
        }
        if g_tc_noload_n.wrapping_add(1).wrapping_mul(2) > g_tc_noload_cap {
            let ncap = if g_tc_noload_cap != 0 {
                g_tc_noload_cap.wrapping_mul(2)
            } else {
                4096
            };
            let nv = libc::calloc(ncap, size_of::<u64>()).cast::<u64>();
            if nv.is_null() {
                return;
            }
            for k in 0..g_tc_noload_cap {
                let v = *g_tc_noload.wrapping_add(k);
                if v == 0 {
                    continue;
                }
                let mut i = (v.wrapping_mul(0x9e3779b97f4a7c15) >> 20) as usize & (ncap - 1);
                while *nv.wrapping_add(i) != 0 {
                    i = (i + 1) & (ncap - 1);
                }
                *nv.wrapping_add(i) = v;
            }
            libc::free(g_tc_noload.cast());
            g_tc_noload = nv;
            g_tc_noload_cap = ncap;
        }
        let mut i = (key.wrapping_mul(0x9e3779b97f4a7c15) >> 20) as usize & (g_tc_noload_cap - 1);
        while *g_tc_noload.wrapping_add(i) != 0 {
            i = (i + 1) & (g_tc_noload_cap - 1);
        }
        *g_tc_noload.wrapping_add(i) = key;
        g_tc_noload_n = g_tc_noload_n.wrapping_add(1);
    }
}

unsafe fn tc_value(
    jit: *const ffi::OcerzJit,
    blk: *const ffi::JitBlock,
    kind: c_int,
    arg: u64,
    ok: *mut c_int,
) -> u64 {
    unsafe {
        *ok = 1;
        if kind == ffi::TCR_SYM as c_int {
            let symbol = arg as c_int;
            if symbol == ffi::TCS_FLAGS_MATERIALIZE as c_int {
                ffi::ocerz_flags_materialize as *const () as usize as u64
            } else if symbol == ffi::TCS_RAS_PUSH as c_int {
                ffi::ocerz_ras_push as *const () as usize as u64
            } else if symbol == ffi::TCS_EXEC_ONE as c_int {
                ffi::ocerz_jit_exec_one as *const () as usize as u64
            } else if symbol == ffi::TCS_EXEC_ONE_AT as c_int {
                ffi::ocerz_jit_exec_one_at as *const () as usize as u64
            } else if symbol == ffi::TCS_JGB_TRAP as c_int {
                ffi::ocerz_jgb_trap as *const () as usize as u64
            } else if symbol == ffi::TCS_RETIRE_COUNT as c_int {
                addr_of!(ffi::ocerz_jit_retire_count) as usize as u64
            } else if symbol == ffi::TCS_EXEC_RUN_AT as c_int {
                ffi::ocerz_jit_exec_run_at as *const () as usize as u64
            } else {
                *ok = 0;
                0
            }
        } else if kind == ffi::TCR_COMMPAGE as c_int {
            if !ffi::ocerz_commpage.is_null() {
                (ffi::ocerz_commpage as usize as u64)
                    .wrapping_sub(OCERZ_COMMPAGE_LO)
                    .wrapping_sub(ffi::ocerz_guest_base)
            } else {
                *ok = 0;
                0
            }
        } else if kind == ffi::TCR_BUCKETS as c_int {
            addr_of!((*jit).buckets) as usize as u64
        } else if kind == ffi::TCR_LEAF as c_int {
            let leaf = if !(*jit).leaf_near.is_null() {
                (*jit).leaf_near.wrapping_offset(arg as isize)
            } else {
                addr_of!(ffi::ocerz_leaf_lo)
                    .cast::<libc::c_char>()
                    .wrapping_offset(arg as isize)
            };
            leaf as usize as u64
        } else if kind == ffi::TCR_DSTUB as c_int {
            let d = if arg != 0 {
                (*jit).dispatch_stub32
            } else {
                (*jit).dispatch_stub
            };
            if !d.is_null() {
                d as usize as u64
            } else {
                *ok = 0;
                0
            }
        } else if kind == ffi::TCR_BLK as c_int {
            blk as usize as u64
        } else if kind == ffi::TCR_INSN as c_int {
            if !(*blk).insns.is_null() && arg < ((*blk).n_insns as i64 as u64) {
                (*blk).insns.wrapping_offset(arg as isize) as usize as u64
            } else {
                *ok = 0;
                0
            }
        } else if kind == ffi::TCR_PROF as c_int {
            if !(*blk).prof.is_null() && arg < (SIDE_MAX * size_of::<ffi::JitProf>()) as u64 {
                ((*blk).prof as usize).wrapping_add(arg as usize) as u64
            } else {
                *ok = 0;
                0
            }
        } else {
            *ok = 0;
            0
        }
    }
}

unsafe fn tc_imm_read(w: *const u32) -> u64 {
    unsafe {
        let mut v = 0u64;
        for k in 0..4 {
            let word = *w.wrapping_add(k);
            v |= (((word >> 5) & 0xffff) as u64) << (16 * k);
        }
        v
    }
}

unsafe fn tc_imm_shape(w: *const u32) -> c_int {
    unsafe {
        if (*w & 0xffe0_0000) != 0xd280_0000 {
            return 0;
        }
        for k in 1..4 {
            let word = *w.wrapping_add(k);
            if (word & 0xffe0_0000) != (0xf280_0000 | ((k as u32) << 21))
                || (word & 31) != (*w & 31)
            {
                return 0;
            }
        }
        1
    }
}

unsafe fn tc_imm_write(w: *mut u32, v: u64) {
    unsafe {
        for k in 0..4 {
            let word = *w.wrapping_add(k);
            *w.wrapping_add(k) = (word & !(0xffff << 5)) | (((v >> (16 * k)) as u32 & 0xffff) << 5);
        }
    }
}

unsafe fn tc_ras_fill(
    jit: *mut ffi::OcerzJit,
    cell: *mut *mut c_void,
    retaddr: u64,
    mode32: c_int,
) {
    unsafe {
        let rb = ffi::cache_lookup(jit, retaddr, mode32);
        if !rb.is_null() && (*rb).code.is_some() {
            *cell = ras_entry_for(rb);
        } else {
            *cell = ptr::null_mut();
            ffi::pending_add_ras(jit_key(retaddr, mode32), cell);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_bind(
    jit: *mut ffi::OcerzJit,
    blk: *mut ffi::JitBlock,
    code: *mut u32,
    rel: *const ffi::TcReloc,
    nrel: c_int,
    fresh: c_int,
) -> c_int {
    unsafe {
        let mode32 = blk_mode32(blk);
        if fresh != 0 {
            for i in 0..nrel {
                let r = rel.wrapping_offset(i as isize);
                if (*r).kind as c_int == ffi::TCR_RASCELL as c_int {
                    continue;
                }
                if (*r).kind as c_int == ffi::TCR_RASSLOT as c_int {
                    let sl = ffi::ras_slot_alloc();
                    if sl.is_null() {
                        return 0;
                    }
                    *tc_val_ptr().wrapping_offset(i as isize) = sl as usize as u64;
                } else if (*r).kind as c_int == ffi::TCR_PSC as c_int {
                    let t = ffi::psc_alloc();
                    if t.is_null() {
                        return 0;
                    }
                    *tc_val_ptr().wrapping_offset(i as isize) = t as usize as u64;
                } else {
                    let mut ok = 0;
                    *tc_val_ptr().wrapping_offset(i as isize) =
                        tc_value(jit, blk, (*r).kind as c_int, (*r).arg, &mut ok);
                    if ok == 0 {
                        return 0;
                    }
                }
            }
        }
        for i in 0..nrel {
            let r = rel.wrapping_offset(i as isize);
            let w = code.wrapping_offset((*r).off as isize);
            if (*r).kind as c_int == ffi::TCR_RASCELL as c_int {
                ras_cell_register(w.cast());
                tc_ras_fill(jit, w.cast(), (*r).arg, mode32);
                continue;
            }
            if fresh == 0 {
                continue;
            }
            let val = *tc_val_ptr().wrapping_offset(i as isize);
            if (*r).kind as c_int == ffi::TCR_RASSLOT as c_int {
                tc_ras_fill(jit, val as usize as *mut *mut c_void, (*r).arg, mode32);
            }
            if (*r).form == 1 {
                w.cast::<u64>().write(val);
            } else {
                tc_imm_write(w, val);
            }
        }
        1
    }
}

unsafe fn tc_pcrel_target(w: *const u32) -> *const u32 {
    unsafe {
        let v = *w;
        let off: i64;
        if (v & 0x7c00_0000) == 0x1400_0000 {
            off = (((v << 6) as i32 >> 6) as i64).wrapping_mul(4);
        } else if (v & 0xff00_0010) == 0x5400_0000
            || (v & 0x7e00_0000) == 0x3400_0000
            || (v & 0x3b00_0000) == 0x1800_0000
        {
            off = (((v << 8) as i32 >> 13) as i64).wrapping_mul(4);
        } else if (v & 0x7e00_0000) == 0x3600_0000 {
            off = (((v << 13) as i32 >> 18) as i64).wrapping_mul(4);
        } else if (v & 0x9f00_0000) == 0x1000_0000 {
            off = ((((v << 8) as i32 >> 13) as i64).wrapping_mul(4)) | ((v >> 29) & 3) as i64;
        } else if (v & 0x9f00_0000) == 0x9000_0000 {
            return ptr::null();
        } else {
            return w;
        }
        w.cast::<u8>().wrapping_offset(off as isize).cast()
    }
}

unsafe fn tc_is_reloc_word(w: u32) -> c_int {
    unsafe {
        for i in 0..g_tc_nrel {
            let r = tc_rel_ptr().wrapping_offset(i as isize);
            let o = (*r).off;
            let len = if (*r).form == 1 { 2u32 } else { 4u32 };
            if w >= o && w < o.wrapping_add(len) {
                return 1;
            }
        }
        0
    }
}

unsafe fn tc_insn_at(blk: *const ffi::JitBlock, w: u32) -> c_int {
    unsafe {
        let mut ii = -1;
        if !(*blk).insn_off.is_null() {
            for k in 0..(*blk).n_insns {
                if *(*blk).insn_off.wrapping_offset(k as isize) <= w {
                    ii = k;
                }
            }
        }
        ii
    }
}

unsafe fn tc_report(blk: *const ffi::JitBlock, what: *const libc::c_char, w: u32, v: u32, x: u64) {
    unsafe {
        if g_tc_log == 0 || TC_REPORT_N.fetch_add(1, Ordering::Relaxed) >= 60 {
            return;
        }
        let ii = tc_insn_at(blk, w);
        let mut tb = [0 as libc::c_char; 128];
        if ii >= 0 && !(*blk).insns.is_null() {
            ffi::ocerz_format_insn(
                (*blk).insns.wrapping_offset(ii as isize),
                tb.as_mut_ptr(),
                tb.len(),
            );
        }
        libc::fprintf(
            g_tc_lf,
            c"ocerz: TCACHE[%d] %s rip=%#llx word=%u v=%08x x=%#llx insn=%d %s\n".as_ptr(),
            libc::getpid() as c_int,
            what,
            blk_rip(blk) as libc::c_ulonglong,
            w as libc::c_uint,
            v as libc::c_uint,
            x as libc::c_ulonglong,
            ii,
            tb.as_ptr(),
        );
    }
}

unsafe fn tc_host_const(jit: *const ffi::OcerzJit, blk: *const ffi::JitBlock, x: u64) -> c_int {
    unsafe {
        if TC_SELF_BASE.is_null() {
            let mut di: libc::Dl_info = zeroed();
            if libc::dladdr(
                ffi::ocerz_jit_exec_one as *const () as usize as *const c_void,
                &mut di,
            ) != 0
            {
                TC_SELF_BASE = di.dli_fbase;
            }
        }
        let p = x as usize;
        if p >= (*jit).code_base as usize && p < (*jit).code_end as usize {
            return 1;
        }
        let blk_start = blk as usize;
        if p >= blk_start && p < blk_start.wrapping_add(size_of::<ffi::JitBlock>()) {
            return 1;
        }
        if !(*blk).insns.is_null()
            && p >= (*blk).insns as usize
            && p < (*blk).insns.wrapping_offset((*blk).n_insns as isize) as usize
        {
            return 1;
        }
        if !(*blk).prof.is_null()
            && p >= (*blk).prof as usize
            && p < (*blk).prof.wrapping_add(SIDE_MAX) as usize
        {
            return 1;
        }
        if !ffi::ocerz_commpage.is_null()
            && x == (ffi::ocerz_commpage as usize as u64)
                .wrapping_sub(OCERZ_COMMPAGE_LO)
                .wrapping_sub(ffi::ocerz_guest_base)
        {
            return 1;
        }
        if !ffi::g_ras_slots.is_null()
            && p >= ffi::g_ras_slots as usize
            && p < ffi::g_ras_slots.wrapping_add(RAS_SLOT_CAP) as usize
        {
            return 1;
        }
        let jit_start = jit as usize;
        if p >= jit_start && p < jit_start.wrapping_add(size_of::<ffi::OcerzJit>()) {
            return 1;
        }
        if (0x1_0000_0000..0x8000_0000_0000).contains(&x) {
            let mut di: libc::Dl_info = zeroed();
            if !TC_SELF_BASE.is_null()
                && libc::dladdr(p as *const c_void, &mut di) != 0
                && di.dli_fbase == TC_SELF_BASE as *mut c_void
            {
                return 1;
            }
        }
        0
    }
}

unsafe fn tc_scan(
    jit: *const ffi::OcerzJit,
    blk: *const ffi::JitBlock,
    code: *const u32,
    words: u32,
) -> c_int {
    unsafe {
        let end = if g_tc_pool_off < words {
            g_tc_pool_off
        } else {
            words
        };
        let mut bad = 0;
        for w in 0..end {
            if tc_is_reloc_word(w) != 0 {
                continue;
            }
            let at = code.wrapping_offset(w as isize);
            let t = tc_pcrel_target(at);
            if t == at {
                continue;
            }
            let code_start = code as usize;
            let code_end = code.wrapping_add(words as usize) as usize;
            if t.is_null() || (t as usize) < code_start || (t as usize) >= code_end {
                let target = if t.is_null() {
                    0
                } else {
                    ((t as usize).wrapping_sub(code_start) as isize / 4) as u64
                };
                tc_report(blk, c"PCREL".as_ptr(), w, *at, target);
                g_tc_n_pcrel.fetch_add(1, Ordering::SeqCst);
                bad = 1;
            }
        }
        for w in 0..end {
            let v = *code.wrapping_offset(w as isize);
            if (v & 0xffe0_0000) != 0xd280_0000 || tc_is_reloc_word(w) != 0 {
                continue;
            }
            let rd = (v & 31) as c_int;
            let mut x = ((v >> 5) & 0xffff) as u64;
            let mut k = w + 1;
            while k < end {
                let next = *code.wrapping_offset(k as isize);
                if (next & 0xff80_0000) != 0xf280_0000 || (next & 31) as c_int != rd {
                    break;
                }
                let hw = ((next >> 21) & 3) as u32;
                x = (x & !(0xffffu64 << (16 * hw)))
                    | ((((next >> 5) & 0xffff) as u64) << (16 * hw));
                k += 1;
            }
            if k > w + 1 && tc_host_const(jit, blk, x) != 0 {
                tc_report(blk, c"HOSTCONST".as_ptr(), w, v, x);
                g_tc_n_const.fetch_add(1, Ordering::SeqCst);
                bad = 1;
            }
        }
        bad
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_summary() {
    unsafe {
        if ffi::ocerz_tcache_mode() == ffi::OCERZ_TC_ROUNDTRIP as c_int {
            libc::fprintf(
                g_tc_lf,
                c"ocerz: TCACHE[%d] roundtrip ok=%llu bad=%llu pcrel=%llu hostconst=%llu mismatch=%llu full=%llu\n"
                    .as_ptr(),
                libc::getpid() as c_int,
                g_tc_n_ok.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_bad.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_pcrel.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_const.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_mism.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_full.load(Ordering::SeqCst) as libc::c_ulonglong,
            );
        } else {
            libc::fprintf(
                g_tc_lf,
                c"ocerz: TCACHE[%d] loaded=%llu saved=%llu stale=%llu rejected=%llu verify_ok=%llu verify_variant=%llu verify_bad=%llu\n"
                    .as_ptr(),
                libc::getpid() as c_int,
                g_tc_n_load.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_put.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_stale.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_rej.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_vok.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_vvar.load(Ordering::SeqCst) as libc::c_ulonglong,
                g_tc_n_vbad.load(Ordering::SeqCst) as libc::c_ulonglong,
            );
        }
        libc::fflush(g_tc_lf);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_tcache_final() {
    unsafe {
        ffi::ocerz_tcache_flush();
        if g_tc_log > 0 {
            tc_summary();
        }
    }
}

#[inline(always)]
fn tc_rebase_pointer(c: *mut u32, entry: *mut u32, p: *mut u32) -> *mut u32 {
    if p.is_null() {
        ptr::null_mut()
    } else {
        let d = (c as usize).wrapping_sub(entry as usize) as isize / 4;
        p.wrapping_offset(d)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_roundtrip(
    jit: *mut ffi::OcerzJit,
    blk: *mut ffi::JitBlock,
    entry: *mut u32,
) -> *mut u32 {
    unsafe {
        let words = (*blk).code_words;
        let mut bad = g_tc_bad;
        if tc_scan(jit, blk, entry, words) != 0 {
            bad = 1;
        }
        for i in 0..g_tc_nrel {
            let r = tc_rel_ptr().wrapping_offset(i as isize);
            if (*r).form != 0 {
                continue;
            }
            if (*r).off.wrapping_add(4) > words
                || tc_imm_shape(entry.wrapping_offset((*r).off as isize)) == 0
            {
                let v = if (*r).off < words {
                    *entry.wrapping_offset((*r).off as isize)
                } else {
                    0
                };
                tc_report(blk, c"SHAPE".as_ptr(), (*r).off, v, (*r).kind as u64);
                g_tc_n_mism.fetch_add(1, Ordering::SeqCst);
                bad = 1;
                continue;
            }
            if (*r).kind as c_int == ffi::TCR_RASSLOT as c_int {
                continue;
            }
            let mut ok = 0;
            let value = tc_value(jit, blk, (*r).kind as c_int, (*r).arg, &mut ok);
            if ok == 0 || value != tc_imm_read(entry.wrapping_offset((*r).off as isize)) {
                tc_report(
                    blk,
                    c"MISMATCH".as_ptr(),
                    (*r).off,
                    *entry.wrapping_offset((*r).off as isize),
                    ((*r).kind as u64) << 56 | (*r).arg,
                );
                g_tc_n_mism.fetch_add(1, Ordering::SeqCst);
                bad = 1;
            }
        }
        if bad != 0 {
            g_tc_n_bad.fetch_add(1, Ordering::SeqCst);
            return ptr::null_mut();
        }
        let p = (*jit).code_cur.cast::<u8>();
        let copy = p.wrapping_add((entry as usize).wrapping_sub(p as usize) & 63);
        if (copy as usize).wrapping_add(words as usize * 4) > (*jit).code_end as usize {
            g_tc_n_full.fetch_add(1, Ordering::SeqCst);
            return ptr::null_mut();
        }
        let c = copy.cast::<u32>();
        pthread_jit_write_protect_np(0);
        tc_copy(c.cast(), entry.cast(), words as usize * 4);
        if tc_bind(jit, blk, c, tc_rel_ptr(), g_tc_nrel, 1) == 0 {
            pthread_jit_write_protect_np(1);
            g_tc_n_bad.fetch_add(1, Ordering::SeqCst);
            return ptr::null_mut();
        }
        for w in 0..words {
            *entry.wrapping_offset(w as isize) = 0xd420_0000 | (0xc0de << 5);
        }
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(entry.cast(), words as usize * 4);
        sys_icache_invalidate(c.cast(), words as usize * 4);
        (*jit).code_cur = c.wrapping_add(words as usize);
        (*blk).code = transmute::<*mut u32, ffi::JitBlockFn>(c);
        (*blk).body_code = tc_rebase_pointer(c, entry, (*blk).body_code);
        (*blk).body_noreload = tc_rebase_pointer(c, entry, (*blk).body_noreload);
        (*blk).stop_patch = tc_rebase_pointer(c, entry, (*blk).stop_patch);
        for i in 0..(*blk).n_stop_extra {
            let p = (*blk).stop_extra.as_mut_ptr().wrapping_offset(i as isize);
            (*p).site = tc_rebase_pointer(c, entry, (*p).site);
        }
        for i in 0..(*blk).n_edges {
            let edge = (*blk).edges.wrapping_offset(i as isize);
            (*edge).patch_b = tc_rebase_pointer(c, entry, (*edge).patch_b);
            (*edge).cond_site = tc_rebase_pointer(c, entry, (*edge).cond_site);
        }
        if !(*blk).prof.is_null() {
            for k in 0..SIDE_MAX {
                let prof = (*blk).prof.wrapping_add(k);
                (*prof).ft_site = tc_rebase_pointer(c, entry, (*prof).ft_site);
                (*prof).tk_trip = tc_rebase_pointer(c, entry, (*prof).tk_trip);
            }
        }
        g_tc_n_ok.fetch_add(1, Ordering::SeqCst);
        c
    }
}

unsafe fn tc_mix(h: u64, w: u64) -> u64 {
    let h = (h ^ w).wrapping_mul(0x9e3779b97f4a7c15);
    h ^ (h >> 29)
}

unsafe fn tc_hash_range(mut h: u64, lo: u64, p: *const u8, mut n: u64) -> u64 {
    unsafe {
        h = tc_mix(tc_mix(h, lo), n);
        let mut at = p;
        while n >= 8 {
            let mut w = 0u64;
            tc_copy((&mut w as *mut u64).cast(), at.cast(), size_of::<u64>());
            h = tc_mix(h, w);
            at = at.wrapping_add(8);
            n -= 8;
        }
        let mut t = 0u64;
        for i in 0..n {
            t |= (*at.wrapping_add(i as usize) as u64) << (8 * i);
        }
        tc_mix(h, t ^ (n << 56))
    }
}

unsafe extern "C" fn cmp_dlog(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = a.cast::<ffi::JitState_g_tc_dlog>();
        let y = b.cast::<ffi::JitState_g_tc_dlog>();
        if (*x).pc != (*y).pc {
            return if (*x).pc < (*y).pc { -1 } else { 1 };
        }
        ((*x).len as c_int).wrapping_sub((*y).len as c_int)
    }
}

unsafe fn tc_deps_build(ndep: *mut u32, hash: *mut u64) -> c_int {
    unsafe {
        if ffi::g_tc_ndlog == 0 {
            return 0;
        }
        libc::qsort(
            tc_dlog_ptr().cast(),
            ffi::g_tc_ndlog as usize,
            size_of::<ffi::JitState_g_tc_dlog>(),
            Some(cmp_dlog),
        );
        let mut nd = 0u32;
        let mut mb = 0u32;
        let mut cur = 0u32;
        for i in 0..ffi::g_tc_ndlog {
            let log = tc_dlog_ptr().wrapping_offset(i as isize);
            let lo = (*log).pc;
            let hi = lo.wrapping_add((*log).len as u64);
            let src = tc_dbytes_ptr().wrapping_add((*log).at as usize);
            if nd != 0 && lo <= (*tc_mdep_ptr().wrapping_offset((nd - 1) as isize)).hi {
                let dep = tc_mdep_ptr().wrapping_offset((nd - 1) as isize);
                let ov = if hi < (*dep).hi { hi } else { (*dep).hi };
                let mut a = lo;
                while a < ov {
                    let old = *tc_mbytes_ptr()
                        .wrapping_add(cur.wrapping_add((a - (*dep).lo) as u32) as usize);
                    let new = *src.wrapping_add((a - lo) as usize);
                    if old != new {
                        return 0;
                    }
                    a += 1;
                }
                if hi > (*dep).hi {
                    let n = (hi - (*dep).hi) as usize;
                    tc_copy(
                        tc_mbytes_ptr().wrapping_add(mb as usize).cast(),
                        src.wrapping_add(((*dep).hi - lo) as usize).cast(),
                        n,
                    );
                    mb = mb.wrapping_add(n as u32);
                    (*dep).hi = hi;
                }
            } else {
                cur = mb;
                let dep = tc_mdep_ptr().wrapping_add(nd as usize);
                (*dep).lo = lo;
                (*dep).hi = hi;
                nd += 1;
                let n = (hi - lo) as usize;
                tc_copy(
                    tc_mbytes_ptr().wrapping_add(mb as usize).cast(),
                    src.cast(),
                    n,
                );
                mb = mb.wrapping_add(n as u32);
            }
        }
        let mut h = TC_HASH_SEED;
        let mut at = 0u32;
        for i in 0..nd {
            let dep = tc_mdep_ptr().wrapping_add(i as usize);
            let len = (*dep).hi - (*dep).lo;
            h = tc_hash_range(h, (*dep).lo, tc_mbytes_ptr().wrapping_add(at as usize), len);
            at = at.wrapping_add(len as u32);
        }
        *ndep = nd;
        *hash = h;
        1
    }
}

unsafe extern "C" fn tc_deps_check_callback(arg: *mut c_void) {
    unsafe {
        let st = arg.cast::<TcDepsCheckState>();
        let mut hh = TC_HASH_SEED;
        let mut good = 1;
        for i in 0..(*st).n {
            let d = (*st).d.wrapping_add(i as usize);
            if (*d).hi <= (*d).lo || (*d).hi - (*d).lo > TC_DBYTES_MAX as u64 {
                good = 0;
                break;
            }
            hh = tc_hash_range(hh, (*d).lo, ocerz_g2h((*d).lo).cast(), (*d).hi - (*d).lo);
        }
        (*st).h = hh;
        (*st).good = good;
    }
}

unsafe fn tc_deps_check(d: *const ffi::TcDep, n: u32, want: u64) -> c_int {
    unsafe {
        let mut st = TcDepsCheckState {
            d,
            n,
            h: 0,
            good: 0,
        };
        let done = ocerz_tc_guard(
            tc_deps_check_callback,
            (&mut st as *mut TcDepsCheckState).cast(),
        );
        (done != 0 && st.good != 0 && st.h == want) as c_int
    }
}

unsafe fn tc_out(p: *const c_void, n: usize) -> c_int {
    unsafe {
        let a = n.wrapping_add(7) & !7usize;
        if g_tc_opos.wrapping_add(a) > TC_OUT_MAX {
            return 0;
        }
        if n != 0 {
            tc_copy(g_tc_out.wrapping_add(g_tc_opos).cast(), p, n);
        }
        let padding = a.wrapping_sub(n);
        if padding != 0 {
            tc_zero(
                g_tc_out.wrapping_add(g_tc_opos.wrapping_add(n)).cast(),
                padding,
            );
        }
        g_tc_opos = g_tc_opos.wrapping_add(a);
        1
    }
}

unsafe fn tc_off(blk: *const ffi::JitBlock, p: *const u32) -> u32 {
    if p.is_null() {
        TC_NONE
    } else {
        unsafe {
            let code = transmute::<ffi::JitBlockFn, *mut u32>((*blk).code);
            let diff = (p as usize).wrapping_sub(code as usize) as isize;
            (diff / 4) as u32
        }
    }
}

unsafe fn tc_take<T>(p: *mut *const u8, end: *const u8, dst: *mut *const T, count: usize) -> c_int {
    unsafe {
        let nb = count.wrapping_mul(size_of::<T>());
        let pos = *p as usize;
        let end = end as usize;
        if pos > end || end.wrapping_sub(pos) < nb {
            return 0;
        }
        *dst = (*p).cast::<T>();
        let aligned = nb.wrapping_add(7) & !7usize;
        *p = (*p).wrapping_add(aligned);
        1
    }
}

unsafe fn tc_parse(h: *const ffi::OcerzTcRecHead, v: *mut ffi::TcView) -> c_int {
    unsafe {
        tc_zero(v.cast(), size_of::<ffi::TcView>());
        if ((*h).size as usize) < size_of::<ffi::TcRec>() {
            return 0;
        }
        let r = h.cast::<ffi::TcRec>();
        let mut p = r.wrapping_add(1).cast::<u8>();
        let end = h.cast::<u8>().wrapping_add((*h).size as usize);
        let words = (*r).code_words;
        let n = (*r).n_insns;
        if words == 0
            || words > (1 << 20)
            || n == 0
            || n > JIT_MAX_BLOCK_INSNS
            || (*r).n_kept > n
            || (*r).n_rel as usize > TC_RELOC_MAX
            || (*r).n_dep == 0
            || (*r).n_dep as usize > TC_DLOG_MAX
            || (*r).n_edges as usize > JIT_MAX_EDGES
            || (*r).n_stop_extra > 6
            || (*r).n_push_fix > 0xffff
            || (*r).n_pushelide > 0xffff
            || (*r).n_oslow > (1 << 16)
            || (*r).n_lanerec > (1 << 16)
            || (*r).entry_mod >= 64
            || ((*r).entry_mod & 3) != 0
        {
            return 0;
        }
        (*v).r = r;
        if tc_take(&mut p, end, addr_of_mut!((*v).code), words as usize) == 0
            || tc_take(&mut p, end, addr_of_mut!((*v).rel), (*r).n_rel as usize) == 0
            || tc_take(&mut p, end, addr_of_mut!((*v).dep), (*r).n_dep as usize) == 0
            || tc_take(&mut p, end, addr_of_mut!((*v).insn_off), n as usize) == 0
        {
            return 0;
        }
        if ((*r).flags & TCF_COMPACT) != 0 {
            if tc_take(&mut p, end, addr_of_mut!((*v).iref), n as usize) == 0
                || tc_take(&mut p, end, addr_of_mut!((*v).insns), (*r).n_kept as usize) == 0
            {
                return 0;
            }
        } else if tc_take(&mut p, end, addr_of_mut!((*v).insns), n as usize) == 0 {
            return 0;
        }
        if ((*r).flags & TCF_FAULTF) != 0
            && tc_take(&mut p, end, addr_of_mut!((*v).ff), n as usize) == 0
        {
            return 0;
        }
        if tc_take(&mut p, end, addr_of_mut!((*v).oslow), (*r).n_oslow as usize) == 0
            || tc_take(
                &mut p,
                end,
                addr_of_mut!((*v).lanerec),
                (*r).n_lanerec as usize,
            ) == 0
            || tc_take(
                &mut p,
                end,
                addr_of_mut!((*v).push_fix),
                (*r).n_push_fix as usize,
            ) == 0
            || tc_take(
                &mut p,
                end,
                addr_of_mut!((*v).pushelide),
                (*r).n_pushelide as usize,
            ) == 0
            || tc_take(
                &mut p,
                end,
                addr_of_mut!((*v).stop),
                (*r).n_stop_extra as usize,
            ) == 0
            || tc_take(&mut p, end, addr_of_mut!((*v).edge), (*r).n_edges as usize) == 0
        {
            return 0;
        }
        if ((*r).flags & TCF_PROF) != 0
            && tc_take(&mut p, end, addr_of_mut!((*v).prof), SIDE_MAX) == 0
        {
            return 0;
        }
        for i in 0..(*r).n_rel {
            let rel = (*v).rel.wrapping_add(i as usize);
            if (*rel)
                .off
                .wrapping_add(if (*rel).form == 1 { 2 } else { 4 })
                > words
                || ((*rel).form == 1
                    && ((*r).entry_mod as usize)
                        .wrapping_add(4usize.wrapping_mul((*rel).off as usize))
                        & 7
                        != 0)
                || ((*rel).form == 0
                    && tc_imm_shape((*v).code.wrapping_add((*rel).off as usize)) == 0)
            {
                return 0;
            }
        }
        for i in 0..n {
            if *(*v).insn_off.wrapping_add(i as usize) >= words {
                return 0;
            }
        }
        if ((*r).body_code != TC_NONE && (*r).body_code >= words)
            || ((*r).body_noreload != TC_NONE && (*r).body_noreload >= words)
            || ((*r).stop_patch != TC_NONE && (*r).stop_patch >= words)
        {
            return 0;
        }
        for i in 0..(*r).n_stop_extra {
            if (*(*v).stop.wrapping_add(i as usize)).off >= words {
                return 0;
            }
        }
        for i in 0..(*r).n_edges {
            let edge = (*v).edge.wrapping_add(i as usize);
            if (*edge).patch_b >= words
                || ((*edge).cond_site != TC_NONE && (*edge).cond_site >= words)
            {
                return 0;
            }
        }
        if !(*v).prof.is_null() {
            for k in 0..SIDE_MAX {
                let prof = (*v).prof.wrapping_add(k);
                if ((*prof).ft_site != TC_NONE && (*prof).ft_site >= words)
                    || ((*prof).tk_trip != TC_NONE && (*prof).tk_trip >= words)
                {
                    return 0;
                }
            }
        }
        1
    }
}

unsafe fn tc_free_blk(blk: *mut ffi::JitBlock) {
    unsafe {
        if blk.is_null() {
            return;
        }
        libc::free((*blk).edges.cast());
        libc::free((*blk).insn_off.cast());
        libc::free((*blk).iref.cast());
        libc::free((*blk).kept.cast());
        libc::free((*blk).insns.cast());
        libc::free((*blk).fault_flags.cast());
        libc::free((*blk).oslow.cast());
        libc::free((*blk).lanerec.cast());
        libc::free((*blk).push_fix.cast());
        libc::free((*blk).pushelide.cast());
        libc::free((*blk).prof.cast());
        libc::free(blk.cast());
    }
}

unsafe fn tc_dup(p: *const c_void, n: usize, fail: *mut c_int) -> *mut c_void {
    unsafe {
        if n == 0 {
            return ptr::null_mut();
        }
        let d = libc::malloc(n);
        if d.is_null() {
            *fail = 1;
            return ptr::null_mut();
        }
        tc_copy(d, p, n);
        d
    }
}

#[inline(always)]
unsafe fn tc_at(c: *mut u32, off: u32) -> *mut u32 {
    if off == TC_NONE {
        ptr::null_mut()
    } else {
        c.wrapping_add(off as usize)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_load(
    jit: *mut ffi::OcerzJit,
    h: *const ffi::OcerzTcRecHead,
    rip: u64,
    mode32: c_int,
) -> *mut ffi::JitBlock {
    unsafe {
        let mut v: ffi::TcView = zeroed();
        if tc_parse(h, &mut v) == 0 {
            g_tc_n_rej.fetch_add(1, Ordering::SeqCst);
            return ptr::null_mut();
        }
        let r = v.r;
        if tc_deps_check(v.dep, (*r).n_dep, (*r).dep_hash) == 0 {
            g_tc_n_stale.fetch_add(1, Ordering::SeqCst);
            return ptr::null_mut();
        }
        let n = (*r).n_insns;
        let words = (*r).code_words;
        let mut fail = 0;
        let blk = libc::calloc(1, size_of::<ffi::JitBlock>()).cast::<ffi::JitBlock>();
        if blk.is_null() {
            return ptr::null_mut();
        }
        (*blk).key = jit_key(rip, mode32);
        (*blk).n_insns = n as c_int;
        (*blk).edges = libc::calloc(JIT_MAX_EDGES, size_of::<ffi::JitBlock__bindgen_ty_2>())
            .cast::<ffi::JitBlock__bindgen_ty_2>();
        if (*blk).edges.is_null() {
            fail = 1;
        }
        (*blk).insn_off =
            tc_dup(v.insn_off.cast(), n as usize * size_of::<u32>(), &mut fail).cast::<u32>();
        if ((*r).flags & TCF_COMPACT) != 0 {
            (*blk).iref = tc_dup(
                v.iref.cast(),
                n as usize * size_of::<ffi::JitInsnRef>(),
                &mut fail,
            )
            .cast::<ffi::JitInsnRef>();
            (*blk).kept = tc_dup(
                v.insns.cast(),
                (*r).n_kept as usize * size_of::<ffi::X86Insn>(),
                &mut fail,
            )
            .cast::<ffi::X86Insn>();
            (*blk).n_kept = (*r).n_kept as u16;
        } else {
            (*blk).insns = tc_dup(
                v.insns.cast(),
                n as usize * size_of::<ffi::X86Insn>(),
                &mut fail,
            )
            .cast::<ffi::X86Insn>();
        }
        if !v.ff.is_null() {
            (*blk).fault_flags = tc_dup(
                v.ff.cast(),
                n as usize * size_of::<ffi::JitFaultFlagRecipe>(),
                &mut fail,
            )
            .cast::<ffi::JitFaultFlagRecipe>();
        }
        (*blk).oslow = tc_dup(
            v.oslow.cast(),
            (*r).n_oslow as usize * size_of::<ffi::JitBlock_JitOslowMap>(),
            &mut fail,
        )
        .cast::<ffi::JitBlock_JitOslowMap>();
        (*blk).n_oslow = if (*blk).oslow.is_null() {
            0
        } else {
            (*r).n_oslow as c_int
        };
        (*blk).lanerec = tc_dup(
            v.lanerec.cast(),
            (*r).n_lanerec as usize * size_of::<ffi::JitLaneRec>(),
            &mut fail,
        )
        .cast::<ffi::JitLaneRec>();
        (*blk).n_lanerec = if (*blk).lanerec.is_null() {
            0
        } else {
            (*r).n_lanerec as c_int
        };
        (*blk).push_fix = tc_dup(
            v.push_fix.cast(),
            (*r).n_push_fix as usize * size_of::<u32>(),
            &mut fail,
        )
        .cast::<u32>();
        (*blk).n_push_fix = if (*blk).push_fix.is_null() {
            0
        } else {
            (*r).n_push_fix as u16
        };
        (*blk).pushelide = tc_dup(
            v.pushelide.cast(),
            (*r).n_pushelide as usize * size_of::<ffi::JitBlock_JitPushElide>(),
            &mut fail,
        )
        .cast::<ffi::JitBlock_JitPushElide>();
        (*blk).n_pushelide = if (*blk).pushelide.is_null() {
            0
        } else {
            (*r).n_pushelide as u16
        };
        if !v.prof.is_null() {
            (*blk).prof = libc::calloc(SIDE_MAX, size_of::<ffi::JitProf>()).cast::<ffi::JitProf>();
            if (*blk).prof.is_null() {
                fail = 1;
            }
        }
        if fail != 0 {
            tc_free_blk(blk);
            return ptr::null_mut();
        }

        pthread_jit_write_protect_np(0);
        if tc_env_on() == 0 {
            if mode32 != 0 {
                if (*jit).dispatch_stub32.is_null() {
                    ffi::emit_dispatch_stub(jit, 1);
                }
            } else if (*jit).dispatch_stub.is_null() {
                ffi::emit_dispatch_stub(jit, 0);
            }
        }
        ffi::veneer_pool_check(jit);
        let pp = (*jit).code_cur.cast::<u8>();
        let c = pp
            .wrapping_add(((*r).entry_mod as usize).wrapping_sub(pp as usize) & 63)
            .cast::<u32>();
        if (*jit).code_full != 0
            || (c as usize).wrapping_add(words as usize * 4) > (*jit).code_end as usize
        {
            pthread_jit_write_protect_np(1);
            tc_free_blk(blk);
            return ptr::null_mut();
        }
        tc_copy(c.cast(), v.code.cast(), words as usize * 4);
        if tc_bind(jit, blk, c, v.rel, (*r).n_rel as c_int, 1) == 0 {
            pthread_jit_write_protect_np(1);
            tc_free_blk(blk);
            return ptr::null_mut();
        }
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(c.cast(), words as usize * 4);
        (*jit).code_cur = c.wrapping_add(words as usize);
        (*blk).code = transmute::<*mut u32, ffi::JitBlockFn>(c);
        (*blk).code_words = words;
        (*blk).body_code = tc_at(c, (*r).body_code);
        (*blk).body_noreload = tc_at(c, (*r).body_noreload);
        (*blk).hoist_sig = (*r).hoist_sig;
        (*blk).ordered_loads = (*r).ordered_loads;
        (*blk).stop_patch = tc_at(c, (*r).stop_patch);
        (*blk).stop_insn = (*r).stop_insn;
        (*blk).n_stop_extra = (*r).n_stop_extra;
        for i in 0..(*r).n_stop_extra {
            let dst = (*blk).stop_extra.as_mut_ptr().wrapping_add(i as usize);
            let src = v.stop.wrapping_add(i as usize);
            (*dst).site = c.wrapping_add((*src).off as usize);
            (*dst).insn = (*src).insn;
        }
        (*blk).n_edges = (*r).n_edges;
        for i in 0..(*r).n_edges {
            let dst = (*blk).edges.wrapping_add(i as usize);
            let src = v.edge.wrapping_add(i as usize);
            (*dst).target_rip = (*src).target_rip;
            (*dst).jcc_rip = (*src).jcc_rip;
            (*dst).patch_b = c.wrapping_add((*src).patch_b as usize);
            (*dst).fallback_insn = (*src).fallback_insn;
            (*dst).cond_site = tc_at(c, (*src).cond_site);
            (*dst).cond_orig = (*src).cond_orig;
            (*dst).kind = (*src).kind;
            (*dst).pin_class = (*src).pin_class;
            (*dst).side = (*src).side;
            (*dst).probing = (*src).probing;
        }
        if !v.prof.is_null() {
            for k in 0..SIDE_MAX {
                let dst = (*blk).prof.wrapping_add(k);
                let src = v.prof.wrapping_add(k);
                (*dst).ft_site = tc_at(c, (*src).ft_site);
                (*dst).tk_trip = tc_at(c, (*src).tk_trip);
            }
        }
        (*blk).n_inlined = (*r).n_inlined;
        (*blk).n_slow = (*r).n_slow;
        (*blk).entry_live = (*r).entry_live;
        (*blk).xmm_pinned = (*r).xmm_pinned;
        (*blk).n_pinned = (*r).n_pinned;
        (*blk).pin_class = (*r).pin_class;
        tc_copy(
            addr_of_mut!((*blk).host_holds).cast(),
            addr_of!((*r).host_holds).cast(),
            size_of::<[u8; 16]>(),
        );
        tc_copy(
            addr_of_mut!((*blk).guest_in_host).cast(),
            addr_of!((*r).guest_in_host).cast(),
            size_of::<[i8; 16]>(),
        );
        if !(*blk).stop_patch.is_null() || (*blk).n_stop_extra != 0 {
            (*blk).stop_next = (*jit).stop_blocks;
            (*jit).stop_blocks = blk;
        }
        if ffi::code_index_append_locked(jit, blk) == 0 {
            (*blk).code = None;
            return ptr::null_mut();
        }
        ffi::cache_insert(jit, blk);
        ffi::blk_chain_install(jit, blk);
        g_tc_n_load.fetch_add(1, Ordering::SeqCst);
        blk
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_verify(
    jit: *mut ffi::OcerzJit,
    blk: *mut ffi::JitBlock,
    h: *const ffi::OcerzTcRecHead,
) {
    unsafe {
        let mut v: ffi::TcView = zeroed();
        if tc_parse(h, &mut v) == 0 {
            g_tc_n_rej.fetch_add(1, Ordering::SeqCst);
            return;
        }
        let r = v.r;
        if tc_deps_check(v.dep, (*r).n_dep, (*r).dep_hash) == 0 {
            g_tc_n_stale.fetch_add(1, Ordering::SeqCst);
            tc_put(jit, blk);
            return;
        }
        let code: *const u32 = match (*blk).code {
            Some(p) => p as *const u32,
            None => ptr::null(),
        };
        let mut why: *const libc::c_char = ptr::null();
        let mut at = 0u32;
        if (*r).entry_mod != (code as usize as u32 & 63) {
            why = c"align".as_ptr();
        } else if (*r).code_words != (*blk).code_words {
            why = c"words".as_ptr();
        } else if (*r).n_rel as c_int != g_tc_nrel {
            why = c"nrel".as_ptr();
        }
        let mut rel_i = 0u32;
        for i in 0..(*r).n_rel {
            if !why.is_null() {
                break;
            }
            let a = v.rel.wrapping_add(i as usize);
            let b = tc_rel_ptr().wrapping_add(i as usize);
            if (*a).off != (*b).off
                || (*a).kind != (*b).kind
                || (*a).form != (*b).form
                || (*a).arg != (*b).arg
            {
                why = c"rel".as_ptr();
                at = (*a).off;
                rel_i = i;
            }
        }
        for w in 0..(*r).code_words {
            if !why.is_null() {
                break;
            }
            if *code.wrapping_add(w as usize) != *v.code.wrapping_add(w as usize)
                && tc_is_reloc_word(w) == 0
            {
                why = c"code".as_ptr();
                at = w;
            }
        }
        if why.is_null()
            && ((*r).n_insns as c_int != (*blk).n_insns
                || (*r).n_edges != (*blk).n_edges
                || (*r).pin_class != (*blk).pin_class
                || (*r).n_pinned != (*blk).n_pinned
                || (*r).entry_live != (*blk).entry_live
                || (*r).xmm_pinned != (*blk).xmm_pinned
                || (*r).hoist_sig != (*blk).hoist_sig
                || (*r).ordered_loads != (*blk).ordered_loads
                || (*r).body_code != tc_off(blk, (*blk).body_code)
                || (*r).body_noreload != tc_off(blk, (*blk).body_noreload)
                || (*r).stop_patch != tc_off(blk, (*blk).stop_patch)
                || (*r).n_stop_extra != (*blk).n_stop_extra)
        {
            why = c"meta".as_ptr();
        }
        for i in 0..(*r).n_insns {
            if !why.is_null() {
                break;
            }
            if *v.insn_off.wrapping_add(i as usize) != *(*blk).insn_off.wrapping_add(i as usize) {
                why = c"insn_off".as_ptr();
                at = i as u32;
            }
        }
        for i in 0..(*r).n_edges {
            if !why.is_null() {
                break;
            }
            let a = v.edge.wrapping_add(i as usize);
            let b = (*blk).edges.wrapping_add(i as usize);
            if (*a).target_rip != (*b).target_rip
                || (*a).patch_b != tc_off(blk, (*b).patch_b)
                || (*a).cond_site != tc_off(blk, (*b).cond_site)
                || (*a).fallback_insn != (*b).fallback_insn
                || (*a).kind != (*b).kind
            {
                why = c"edge".as_ptr();
                at = i as u32;
            }
        }
        if why.is_null() {
            g_tc_n_vok.fetch_add(1, Ordering::SeqCst);
            return;
        }
        if (*r).code_words != (*blk).code_words
            || (*r).n_insns as c_int != (*blk).n_insns
            || ((*r).flags & TCF_LEARNED) != 0
            || g_tc_learned != 0
        {
            g_tc_n_vvar.fetch_add(1, Ordering::SeqCst);
            return;
        }
        g_tc_n_vbad.fetch_add(1, Ordering::SeqCst);
        if g_tc_log == 0 || TC_VERIFY_NREP.fetch_add(1, Ordering::Relaxed) >= 60 {
            return;
        }
        let is_code = libc::strcmp(why, c"code".as_ptr()) == 0;
        let is_rel = libc::strcmp(why, c"rel".as_ptr()) == 0;
        let ii = if is_code || is_rel {
            tc_insn_at(blk, at)
        } else {
            -1
        };
        let irip = if ii >= 0 {
            if !(*blk).insns.is_null() {
                (*(*blk).insns.wrapping_add(ii as usize)).rip
            } else if !(*blk).iref.is_null() {
                (*(*blk).iref.wrapping_add(ii as usize)).rip
            } else {
                0
            }
        } else {
            0
        };
        libc::fprintf(
            g_tc_lf,
            c"ocerz: TCACHE[%d] VERIFY %s rip=%#llx at=%u insn=%d insn_rip=%#llx now=%08x rec=%08x words=%u/%u insns=%d/%u\n"
                .as_ptr(),
            libc::getpid() as c_int,
            why,
            blk_rip(blk) as libc::c_ulonglong,
            at as libc::c_uint,
            ii as c_int,
            irip as libc::c_ulonglong,
            if is_code {
                *code.wrapping_add(at as usize)
            } else {
                0
            } as libc::c_uint,
            if is_code {
                *v.code.wrapping_add(at as usize)
            } else {
                0
            } as libc::c_uint,
            (*blk).code_words as libc::c_uint,
            (*r).code_words as libc::c_uint,
            (*blk).n_insns as c_int,
            (*r).n_insns as libc::c_uint,
        );
        let is_meta = libc::strcmp(why, c"meta".as_ptr()) == 0;
        if is_meta {
            libc::fprintf(
                g_tc_lf,
                c"ocerz: TCACHE[%d]   meta now/rec edges=%u/%u pin_class=%u/%u pinned=%u/%u live=%#x/%#x xmm=%#x/%#x hoist=%#llx/%#llx ordered=%u/%u body=%u/%u noreload=%u/%u stop=%u/%u extra=%u/%u\n"
                    .as_ptr(),
                libc::getpid() as c_int,
                (*blk).n_edges as libc::c_uint,
                (*r).n_edges as libc::c_uint,
                (*blk).pin_class as libc::c_uint,
                (*r).pin_class as libc::c_uint,
                (*blk).n_pinned as libc::c_uint,
                (*r).n_pinned as libc::c_uint,
                (*blk).entry_live as libc::c_uint,
                (*r).entry_live as libc::c_uint,
                (*blk).xmm_pinned as libc::c_uint,
                (*r).xmm_pinned as libc::c_uint,
                (*blk).hoist_sig as libc::c_ulonglong,
                (*r).hoist_sig as libc::c_ulonglong,
                (*blk).ordered_loads as libc::c_uint,
                (*r).ordered_loads as libc::c_uint,
                tc_off(blk, (*blk).body_code) as libc::c_uint,
                (*r).body_code as libc::c_uint,
                tc_off(blk, (*blk).body_noreload) as libc::c_uint,
                (*r).body_noreload as libc::c_uint,
                tc_off(blk, (*blk).stop_patch) as libc::c_uint,
                (*r).stop_patch as libc::c_uint,
                (*blk).n_stop_extra as libc::c_uint,
                (*r).n_stop_extra as libc::c_uint,
            );
        }
        if is_rel {
            let a = tc_rel_ptr().wrapping_add(rel_i as usize);
            let b = v.rel.wrapping_add(rel_i as usize);
            libc::fprintf(
                g_tc_lf,
                c"ocerz: TCACHE[%d]   rel %u now off=%u kind=%u form=%u arg=%#llx rec off=%u kind=%u form=%u arg=%#llx\n"
                    .as_ptr(),
                libc::getpid() as c_int,
                rel_i as libc::c_uint,
                (*a).off as libc::c_uint,
                (*a).kind as libc::c_uint,
                (*a).form as libc::c_uint,
                (*a).arg as libc::c_ulonglong,
                (*b).off as libc::c_uint,
                (*b).kind as libc::c_uint,
                (*b).form as libc::c_uint,
                (*b).arg as libc::c_ulonglong,
            );
        }
        if TC_VERIFY_NREP.load(Ordering::Relaxed) > 4 {
            return;
        }
        for side in 0..2 {
            let (cw, io, nw, ni) = if side != 0 {
                (v.code, v.insn_off, (*r).code_words, (*r).n_insns as c_int)
            } else {
                (
                    code,
                    (*blk).insn_off as *const u32,
                    (*blk).code_words,
                    (*blk).n_insns,
                )
            };
            libc::fprintf(
                g_tc_lf,
                c"ocerz: TCACHE[%d]   %s insn_off:".as_ptr(),
                libc::getpid() as c_int,
                if side != 0 {
                    c"rec".as_ptr()
                } else {
                    c"now".as_ptr()
                },
            );
            for i in 0..ni {
                libc::fprintf(
                    g_tc_lf,
                    c" %u".as_ptr(),
                    *io.wrapping_add(i as usize) as libc::c_uint,
                );
            }
            for w in 0..nw {
                libc::fprintf(
                    g_tc_lf,
                    if w % 8 != 0 {
                        c"%s%08x".as_ptr()
                    } else {
                        c"%s%08x".as_ptr()
                    },
                    if w % 8 != 0 {
                        c" ".as_ptr()
                    } else {
                        c"\nocerz:     ".as_ptr()
                    },
                    *cw.wrapping_add(w as usize) as libc::c_uint,
                );
            }
            libc::fprintf(g_tc_lf, c"\n".as_ptr());
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tc_put(jit: *mut ffi::OcerzJit, blk: *mut ffi::JitBlock) {
    unsafe {
        let _ = jit;
        let n = (*blk).n_insns;
        let compact = (*blk).insns.is_null();
        if (*blk).code.is_none()
            || (*blk).insn_off.is_null()
            || n <= 0
            || (compact && (*blk).iref.is_null())
        {
            return;
        }
        if g_tc_out.is_null() {
            g_tc_out = libc::malloc(TC_OUT_MAX).cast::<u8>();
            if g_tc_out.is_null() {
                return;
            }
        }
        let mut ndep = 0;
        let mut dh = 0;
        if tc_deps_build(&mut ndep, &mut dh) == 0 {
            return;
        }
        let mut r: ffi::TcRec = zeroed();
        r.h.magic = ffi::OCERZ_TC_REC_MAGIC;
        r.h.key = g_tc_key;
        r.dep_hash = dh;
        r.hoist_sig = (*blk).hoist_sig;
        r.code_words = (*blk).code_words;
        let code = transmute::<ffi::JitBlockFn, *mut u32>((*blk).code);
        r.entry_mod = (code as usize & 63) as u32;
        r.n_insns = n as u32;
        r.n_kept = if compact { (*blk).n_kept as u32 } else { 0 };
        r.n_rel = g_tc_nrel as u32;
        r.n_dep = ndep;
        r.n_oslow = if !(*blk).oslow.is_null() {
            (*blk).n_oslow as u32
        } else {
            0
        };
        r.n_lanerec = if !(*blk).lanerec.is_null() {
            (*blk).n_lanerec as u32
        } else {
            0
        };
        r.n_push_fix = if !(*blk).push_fix.is_null() {
            (*blk).n_push_fix as u32
        } else {
            0
        };
        r.n_pushelide = if !(*blk).pushelide.is_null() {
            (*blk).n_pushelide as u32
        } else {
            0
        };
        r.body_code = tc_off(blk, (*blk).body_code);
        r.body_noreload = tc_off(blk, (*blk).body_noreload);
        r.stop_patch = tc_off(blk, (*blk).stop_patch);
        r.stop_insn = (*blk).stop_insn;
        r.n_inlined = (*blk).n_inlined;
        r.n_slow = (*blk).n_slow;
        r.entry_live = (*blk).entry_live;
        r.xmm_pinned = (*blk).xmm_pinned;
        r.n_edges = (*blk).n_edges;
        r.n_stop_extra = (*blk).n_stop_extra;
        r.n_pinned = (*blk).n_pinned;
        r.pin_class = (*blk).pin_class;
        r.ordered_loads = (*blk).ordered_loads;
        r.flags = (if compact { TCF_COMPACT } else { 0 })
            | (if !(*blk).fault_flags.is_null() {
                TCF_FAULTF
            } else {
                0
            })
            | (if !(*blk).prof.is_null() { TCF_PROF } else { 0 })
            | (if g_tc_learned != 0 { TCF_LEARNED } else { 0 });
        tc_copy(
            addr_of_mut!(r.host_holds).cast(),
            addr_of!((*blk).host_holds).cast(),
            size_of::<[u8; 16]>(),
        );
        tc_copy(
            addr_of_mut!(r.guest_in_host).cast(),
            addr_of!((*blk).guest_in_host).cast(),
            size_of::<[i8; 16]>(),
        );

        let mut st: [ffi::TcStop; 6] = [zeroed(); 6];
        for i in 0..(*blk).n_stop_extra.min(6) {
            let stop = (*blk).stop_extra.as_ptr().wrapping_add(i as usize);
            let out = st.as_mut_ptr().wrapping_add(i as usize);
            (*out).off = tc_off(blk, (*stop).site);
            (*out).insn = (*stop).insn;
        }
        let mut ed: [ffi::TcEdge; JIT_MAX_EDGES] = [zeroed(); JIT_MAX_EDGES];
        for i in 0..(*blk).n_edges.min(JIT_MAX_EDGES as u8) {
            let edge = (*blk).edges.wrapping_add(i as usize);
            let out = ed.as_mut_ptr().wrapping_add(i as usize);
            (*out).target_rip = (*edge).target_rip;
            (*out).jcc_rip = (*edge).jcc_rip;
            (*out).patch_b = tc_off(blk, (*edge).patch_b);
            (*out).fallback_insn = (*edge).fallback_insn;
            (*out).cond_site = tc_off(blk, (*edge).cond_site);
            (*out).cond_orig = (*edge).cond_orig;
            (*out).kind = (*edge).kind;
            (*out).pin_class = (*edge).pin_class;
            (*out).side = (*edge).side;
            (*out).probing = (*edge).probing;
        }
        let mut pf: [ffi::TcProf; SIDE_MAX] = [zeroed(); SIDE_MAX];
        if !(*blk).prof.is_null() {
            for k in 0..SIDE_MAX {
                let src = (*blk).prof.wrapping_add(k);
                let out = pf.as_mut_ptr().wrapping_add(k);
                (*out).ft_site = tc_off(blk, (*src).ft_site);
                (*out).tk_trip = tc_off(blk, (*src).tk_trip);
            }
        }
        g_tc_opos = 0;
        let mut ok = tc_out((&r as *const ffi::TcRec).cast(), size_of::<ffi::TcRec>()) != 0
            && tc_out(code.cast(), (*blk).code_words as usize * 4) != 0
            && tc_out(
                tc_rel_ptr().cast(),
                g_tc_nrel as usize * size_of::<ffi::TcReloc>(),
            ) != 0
            && tc_out(
                tc_mdep_ptr().cast(),
                ndep as usize * size_of::<ffi::TcDep>(),
            ) != 0
            && tc_out((*blk).insn_off.cast(), n as usize * size_of::<u32>()) != 0;
        if ok && compact {
            ok = tc_out(
                (*blk).iref.cast(),
                n as usize * size_of::<ffi::JitInsnRef>(),
            ) != 0
                && tc_out(
                    (*blk).kept.cast(),
                    r.n_kept as usize * size_of::<ffi::X86Insn>(),
                ) != 0;
        } else if ok {
            ok = tc_out((*blk).insns.cast(), n as usize * size_of::<ffi::X86Insn>()) != 0;
        }
        if ok && !(*blk).fault_flags.is_null() {
            ok = tc_out(
                (*blk).fault_flags.cast(),
                n as usize * size_of::<ffi::JitFaultFlagRecipe>(),
            ) != 0;
        }
        ok = ok
            && tc_out(
                (*blk).oslow.cast(),
                r.n_oslow as usize * size_of::<ffi::JitBlock_JitOslowMap>(),
            ) != 0
            && tc_out(
                (*blk).lanerec.cast(),
                r.n_lanerec as usize * size_of::<ffi::JitLaneRec>(),
            ) != 0
            && tc_out(
                (*blk).push_fix.cast(),
                r.n_push_fix as usize * size_of::<u32>(),
            ) != 0
            && tc_out(
                (*blk).pushelide.cast(),
                r.n_pushelide as usize * size_of::<ffi::JitBlock_JitPushElide>(),
            ) != 0
            && tc_out(
                st.as_ptr().cast(),
                r.n_stop_extra as usize * size_of::<ffi::TcStop>(),
            ) != 0
            && tc_out(
                ed.as_ptr().cast(),
                r.n_edges as usize * size_of::<ffi::TcEdge>(),
            ) != 0;
        if ok && !(*blk).prof.is_null() {
            ok = tc_out(pf.as_ptr().cast(), size_of::<[ffi::TcProf; SIDE_MAX]>()) != 0;
        }
        if !ok {
            return;
        }
        (*(g_tc_out.cast::<ffi::TcRec>())).h.size = g_tc_opos as u32;
        ffi::ocerz_tcache_put(g_tc_out.cast::<ffi::OcerzTcRecHead>());
        g_tc_n_put.fetch_add(1, Ordering::SeqCst);
    }
}
