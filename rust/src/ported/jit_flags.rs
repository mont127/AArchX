//! ---- flags ----
//! Flags are deferred: an instruction records {kind, size, dst, src} and the
//! flags are materialized only if something reads them. On top of that sit
//! three fusions - NZCV forwarded from an adjacent producer across
//! NZCV-transparent gap instructions, value-based conditions taken straight
//! from a result register (cmp #0, or no compare at all for cbz/cbnz), and the
//! comis fusion, where a jcc/setcc/cmov re-derives its condition by redoing the
//! fcmp. Liveness reaches past the block: an exit's flags are dead if the
//! successor, decoded up to three blocks deep, overwrites them before reading
//! them. That lookahead was a fifth of translation time, because every block
//! branching to an untranslated successor decoded it again, so answers are
//! memoized by rip and depth, and an entry counts only while no translation
//! has been retired since and the successor's first sixteen bytes are
//! unchanged.
//! A static liveness pass and the emitters share the same predicates so they
//! cannot disagree about what is live. The legality rules here are written in
//! blood: a gap may not write a register the producer read (a byte compare
//! through an index register the next instruction overwrites made the
//! NZCV-forwarding fallback re-load with the new index, and a loop spun
//! forever), and a gap's emission must touch only pinned registers (`cmp byte
//! [rdi+0x210],0 ; lea r15,[rsp+0x290] ; jne` branched on rsp and made Steam's
//! CEF browser copy an unengaged optional, 2026-09-06), so a gap is emitted
//! into a scratch buffer first and refused there rather than asserting after
//! the compare is already out.
//!
//! No ABI passes arithmetic flags across a return, but clang's outliner does:
//! a helper that ends in a compare and a ret hands its flags to a caller that
//! branches right after the call. Treating every ret as killing the flags
//! painted every Wine window black (2026-09-05). A pure flag producer
//! reaching a ret keeps its flags live; a tail whose last flag writer is
//! arithmetic (xor eax,eax; ret) returns a value, and keeps the dead seam.
//!
//! ---- control flow ----
//! A block may run past a FORWARD conditional branch, continuing inline and
//! putting the taken side in an out-of-line chain stub (a superblock). When
//! the compiler laid the rare path out inline the hot path is the taken edge
//! instead, and the loop fragments into a chain of blocks; such a branch is
//! probed - both sides count in the arena - and a clearly hotter taken side
//! retires the block, which retranslates with the jcc rewritten as its
//! complement. The first window of a loop is often unlike its steady state,
//! so a verdict counts only when the next window repeats it, and the fourth
//! window decides regardless. OCERZ_FLIP_JCC names branches to invert by hand.
//!
//! Edges are chained block to block, and a conditional branch may be
//! retargeted straight at its successor - but only when nothing the successor
//! needs sits between the branch and the chain tail. A stub that replays
//! lane-0 flushes, an FP-batch check or the producer's flag record must stay
//! on the path: a scalar SSE result left in lane-0 scratch and a side exit
//! chained past the flush produced a singular view transform in Cocoa, and a
//! stale flag record handed to a successor (`cmp ebp,0xb ; jbe L` with
//! `L: ja`) failed every SQLite open in libcef.
//!
//! A guest CALL pushes its return address and also pushes {retaddr, host
//! continuation} onto a host-stack shadow and a return-address stack, then
//! `bl`s into the callee body, so the hardware return predictor matches the
//! RAS and a guest RET is a plain ret. 32-bit calls and rets do the same,
//! their shadow entries tagged JIT_KEY_M32 (m32_ras_ok). Indirect jmp/call go
//! through a per-site direct-mapped cache of 32 {rip, body} pairs (16-aligned,
//! so the lookup's ldp is single-copy atomic) before falling into an inlined
//! hash probe and finally C. In 32-bit code a ret, an indirect call and an
//! indirect jmp take the same cache, their target keyed with JIT_KEY_M32 as a
//! 32-bit block's is, where they all left for the dispatcher before; under
//! WoW64 a vtable-call loop went from 1321 to 464 ms and qsort from 634 to
//! 163 ms. Small straight-line callees ending in a plain ret are spliced into
//! the caller: the call becomes a push, the ret a compare against the known
//! return address, and a mismatch leaves at the ret's rip for the dispatcher
//! to run the real one. Where a frame is pure register work the push's slot
//! is provably never read, so the push becomes a bare rsp -= 8 and matched
//! push/pop pairs become register renames - the loop-carried store-to-load
//! chain of call-dense code. The renames borrow x16, x17 and x30 when no
//! memory base is hoisted into them, and every C call-out clobbers all three,
//! so a renamed pop whose push was followed by a call-out reads the slot the
//! push still wrote instead. That happens whenever an instruction between
//! them falls back to C, and in the low shadow window it happened to every
//! spliced call nested inside another, because the inline call and ret need
//! identity addressing there and go through the slow path: a leaf-call loop
//! built at 0x200000000 summed garbage that changed with every run.
//!
//! In that window the stack may live below 12 GB, where guest and host
//! addresses differ, so every stack fast path used to be off there, and every
//! spliced call and ret went out to C: xbench's leafcall took 2.75 s against
//! 0.16 s outside the window. None of the return-address stack depends on the
//! memory mode, only the two guest-stack accesses do, so with the stack
//! pointer pinned as a pointer and a zero guest base (low_stack_fast), a call
//! stores its return address and a ret loads it through the same translation
//! any other low-window access takes, and the rest - the host shadow push, bl
//! into the callee's body, the compare and the plain ret - is what it is
//! everywhere else. A spliced call's pushes and rets do the same, and its
//! elided pushes and matched rets never touched memory to begin with. leafcall
//! went to 0.35 s and icall from 0.74 to 0.44 (0.39 outside the window).
//! OCERZ_NO_LOW_RAS and OCERZ_NO_LOW_SPLICE turn the two halves off.
//!
//! A setcc or cmovcc reads the forwarded NZCV, never the producer's
//! registers, so a sibling consumer in the gap may write one of them
//! (cmp [rcx],eax ; setg al ; setl dl). A jcc can re-derive its condition
//! from them, so it keeps the rule.
//!
//! after ands, as after test, C and V are clear: be/a are e/ne, l/ge/le/g
//! read N and Z alone
//!
//! A loop's first phase can differ from the rest (an array initialised one
//! way, then settled), and every window can fall in it. So a kept branch
//! goes on counting its taken side, tripping at 2^WATCH_BIT, when
//! flip_side_hit probes it again from scratch, FLIP_REARMS times at most.
//!
//! A cmp/test whose first operand is memory, as emit_rmw_mem emits it inline
//! with NZCV set.
//!
//! A load of a stack slot into a pinned register, which the Wine layout emits
//! as a plain load off rsp + x0 (lowstack_disp_ea): flag-free and touching
//! only JTA, so it may sit between a compare and its fused jcc. It can fault,
//! so emit_cmp_test_jcc writes the compare's flags out before it.
//!
//! Rust cannot soundly model sigsetjmp's returns-twice ABI. The eight guarded
//! decode regions therefore call back through `jit_flags_shim.c`, which keeps
//! each jump buffer and setjmp frame in C while the decode callback itself
//! remains Rust.

use core::ffi::{CStr, c_char, c_int, c_uint, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::inline::*;
mod ffi {
    pub use crate::ffi::*;
    pub use crate::inline::OCERZ_CF;
    pub use crate::jit_internal::{
        CC_DST_OFF, CC_OP_OFF, CC_SRC_OFF, FCMP_MEM_OFF, JIT_ARITH_FLAGS, RF_OFF, RIP_OFF,
    };

    pub const JT0: i32 = crate::ffi::JT0 as i32;
    pub const JT1: i32 = crate::ffi::JT1 as i32;
    pub const JT2: i32 = crate::ffi::JT2 as i32;
    pub const JTF: i32 = crate::ffi::JTF as i32;
    pub const JTT: i32 = crate::ffi::JTT as i32;
    pub const JTU: i32 = crate::ffi::JTU as i32;
    pub const JTA: i32 = crate::ffi::JTA as i32;
    pub const A64_ZR: i32 = crate::ffi::A64_ZR as i32;
    pub const A64_AL: i32 = crate::ffi::A64_AL as i32;
    pub const A64_NV: i32 = crate::ffi::A64_NV as i32;
    pub const A64_EQ: i32 = crate::ffi::A64_EQ as i32;
    pub const A64_NE: i32 = crate::ffi::A64_NE as i32;
    pub const A64_CS: i32 = crate::ffi::A64_CS as i32;
    pub const A64_CC: i32 = crate::ffi::A64_CC as i32;
    pub const A64_MI: i32 = crate::ffi::A64_MI as i32;
    pub const A64_PL: i32 = crate::ffi::A64_PL as i32;
    pub const A64_VS: i32 = crate::ffi::A64_VS as i32;
    pub const A64_VC: i32 = crate::ffi::A64_VC as i32;
    pub const A64_HI: i32 = crate::ffi::A64_HI as i32;
    pub const A64_LS: i32 = crate::ffi::A64_LS as i32;
    pub const A64_GE: i32 = crate::ffi::A64_GE as i32;
    pub const A64_LT: i32 = crate::ffi::A64_LT as i32;
    pub const A64_GT: i32 = crate::ffi::A64_GT as i32;
    pub const A64_LE: i32 = crate::ffi::A64_LE as i32;
    pub const EDGE_BODY: u8 = crate::ffi::EDGE_BODY as u8;
    pub const EDGE_XBLOCK: u8 = crate::ffi::EDGE_XBLOCK as u8;
    pub const FLIP_NONE: u8 = crate::ffi::FLIP_NONE as u8;
    pub const FLIP_DECIDED_ORIG: u8 = crate::ffi::FLIP_DECIDED_ORIG as u8;
    pub const FLIP_DECIDED_INV: u8 = crate::ffi::FLIP_DECIDED_INV as u8;
    pub const OCERZ_OPK_IMM: u8 = crate::ffi::OCERZ_OPK_IMM as u8;
    pub const OCERZ_OPK_MEM: u8 = crate::ffi::OCERZ_OPK_MEM as u8;
    pub const OCERZ_OPK_REG: u8 = crate::ffi::OCERZ_OPK_REG as u8;
    pub const OCERZ_OPK_XMM: u8 = crate::ffi::OCERZ_OPK_XMM as u8;
    pub const OCERZ_RSP: u8 = crate::ffi::OCERZ_RSP as u8;
    pub const OCERZ_REG_NONE: u8 = crate::ffi::OCERZ_REG_NONE as u8;
    pub const OCERZ_SEG_NONE: u8 = crate::ffi::OCERZ_SEG_NONE as u8;
    pub const OCERZ_OP_ADC: u16 = crate::ffi::OCERZ_OP_ADC as u16;
    pub const OCERZ_OP_ADD: u16 = crate::ffi::OCERZ_OP_ADD as u16;
    pub const OCERZ_OP_AND: u16 = crate::ffi::OCERZ_OP_AND as u16;
    pub const OCERZ_OP_BSF: u16 = crate::ffi::OCERZ_OP_BSF as u16;
    pub const OCERZ_OP_BSR: u16 = crate::ffi::OCERZ_OP_BSR as u16;
    pub const OCERZ_OP_BT: u16 = crate::ffi::OCERZ_OP_BT as u16;
    pub const OCERZ_OP_BTC: u16 = crate::ffi::OCERZ_OP_BTC as u16;
    pub const OCERZ_OP_BTR: u16 = crate::ffi::OCERZ_OP_BTR as u16;
    pub const OCERZ_OP_BTS: u16 = crate::ffi::OCERZ_OP_BTS as u16;
    pub const OCERZ_OP_CALL: u16 = crate::ffi::OCERZ_OP_CALL as u16;
    pub const OCERZ_OP_CMOVCC: u16 = crate::ffi::OCERZ_OP_CMOVCC as u16;
    pub const OCERZ_OP_CMP: u16 = crate::ffi::OCERZ_OP_CMP as u16;
    pub const OCERZ_OP_COMISD: u16 = crate::ffi::OCERZ_OP_COMISD as u16;
    pub const OCERZ_OP_COMISS: u16 = crate::ffi::OCERZ_OP_COMISS as u16;
    pub const OCERZ_OP_DEC: u16 = crate::ffi::OCERZ_OP_DEC as u16;
    pub const OCERZ_OP_INC: u16 = crate::ffi::OCERZ_OP_INC as u16;
    pub const OCERZ_OP_JCC: u16 = crate::ffi::OCERZ_OP_JCC as u16;
    pub const OCERZ_OP_JMP: u16 = crate::ffi::OCERZ_OP_JMP as u16;
    pub const OCERZ_OP_LEA: u16 = crate::ffi::OCERZ_OP_LEA as u16;
    pub const OCERZ_OP_MOV: u16 = crate::ffi::OCERZ_OP_MOV as u16;
    pub const OCERZ_OP_NEG: u16 = crate::ffi::OCERZ_OP_NEG as u16;
    pub const OCERZ_OP_OR: u16 = crate::ffi::OCERZ_OP_OR as u16;
    pub const OCERZ_OP_RET: u16 = crate::ffi::OCERZ_OP_RET as u16;
    pub const OCERZ_OP_SAR: u16 = crate::ffi::OCERZ_OP_SAR as u16;
    pub const OCERZ_OP_SBB: u16 = crate::ffi::OCERZ_OP_SBB as u16;
    pub const OCERZ_OP_SETCC: u16 = crate::ffi::OCERZ_OP_SETCC as u16;
    pub const OCERZ_OP_SHL: u16 = crate::ffi::OCERZ_OP_SHL as u16;
    pub const OCERZ_OP_SHR: u16 = crate::ffi::OCERZ_OP_SHR as u16;
    pub const OCERZ_OP_SUB: u16 = crate::ffi::OCERZ_OP_SUB as u16;
    pub const OCERZ_OP_TEST: u16 = crate::ffi::OCERZ_OP_TEST as u16;
    pub const OCERZ_OP_UCOMISD: u16 = crate::ffi::OCERZ_OP_UCOMISD as u16;
    pub const OCERZ_OP_UCOMISS: u16 = crate::ffi::OCERZ_OP_UCOMISS as u16;
    pub const OCERZ_OP_XOR: u16 = crate::ffi::OCERZ_OP_XOR as u16;
    pub const TCR_SYM: i32 = crate::ffi::TCR_SYM as i32;
    pub const TCS_FLAGS_MATERIALIZE: u64 = crate::ffi::TCS_FLAGS_MATERIALIZE as u64;
    pub const OCERZ_FL_ALL: u64 = crate::ported::flags_live::OCERZ_FL_ALL;
    #[allow(non_snake_case)]
    pub const fn A64_INV(cond: i32) -> i32 {
        cond ^ 1
    }

    unsafe extern "C" {
        #[link_name = "__stderrp"]
        pub static mut stderr: *mut FILE;
        pub fn fprintf(stream: *mut FILE, fmt: *const core::ffi::c_char, ...) -> i32;
    }
}

mod jit_internal {
    pub use crate::jit_internal::*;
    use core::ffi::{c_int, c_uint};

    pub unsafe fn pin_slot<G: Into<c_uint>>(greg: G) -> c_int {
        unsafe { crate::jit_internal::pin_slot(greg.into()) }
    }
    pub unsafe fn xmm_is_pinned<G: Into<c_uint>>(xr: G) -> c_int {
        unsafe { crate::jit_internal::xmm_is_pinned(xr.into()) }
    }
    pub unsafe fn l0_src2<G: Into<c_uint>>(
        b: *mut crate::ffi::A64Buf,
        r: G,
        dbl: c_int,
    ) -> c_int {
        unsafe { crate::jit_internal::l0_src2(b, r.into(), dbl) }
    }
    pub unsafe fn emit_gpr_rd<G: Into<c_uint>>(
        b: *mut crate::ffi::A64Buf,
        sf: c_int,
        dst: c_int,
        greg: G,
    ) {
        unsafe { crate::jit_internal::emit_gpr_rd(b, sf, dst, greg.into()) }
    }
    pub unsafe fn emit_gpr_wr<G: Into<c_uint>>(
        b: *mut crate::ffi::A64Buf,
        src: c_int,
        greg: G,
    ) {
        unsafe { crate::jit_internal::emit_gpr_wr(b, src, greg.into()) }
    }
}
#[unsafe(no_mangle)]
pub static mut g_flag_producer: *const ffi::X86Insn = ptr::null();
#[unsafe(no_mangle)]
pub static mut g_no_regflags: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_jcc_side_mode: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_jcc_side_need: u64 = 0;
#[unsafe(no_mangle)]
pub static mut g_jcc_side_fall_need: u64 = 0;
#[unsafe(no_mangle)]
pub static mut g_no_jcclink: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_no_xlive: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_no_jccfuse: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_ndlog: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tc_rec: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_self_rip: u64 = 0;
#[unsafe(no_mangle)]
pub static mut g_loop_entry: *mut u32 = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_stop_patch: *mut u32 = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_side: [ffi::JitState_g_side; ffi::SIDE_MAX as usize] =
    [ffi::JitState_g_side {
        site: ptr::null_mut(),
        taken: 0,
        idx: 0,
        stub: ptr::null_mut(),
        patch_b: ptr::null_mut(),
        rec: 0,
        rec_ccop: 0,
        rec_src: 0,
        rec_dst: 0,
        rec_imm_pending: 0,
        rec_imm: 0,
        jcc_rip: 0,
        probe: 0,
        ft_site: ptr::null_mut(),
        ft_rip: 0,
        fpb: 0,
        fpb_chk: 0,
        fpb_end: 0,
        l0: [0; 16],
        l0_dbl: [0; 16],
        l0_dirty: 0,
        yc_dirty: 0,
    }; ffi::SIDE_MAX as usize];
#[unsafe(no_mangle)]
pub static mut g_n_side: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_flip: [ffi::JitState_g_flip; ffi::FLIP_N as usize] =
    [ffi::JitState_g_flip { rip: 0, state: 0 }; ffi::FLIP_N as usize];
#[unsafe(no_mangle)]
pub static mut g_n_probes: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_stop_target: *mut u32 = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_jcc_edge: [ffi::JitState_g_jcc_edge; 2] =
    [ffi::JitState_g_jcc_edge {
        target_rip: 0,
        patch_b: ptr::null_mut(),
        cond_site: ptr::null_mut(),
        kind: 0,
        pin_class: 0,
    }; 2];
#[unsafe(no_mangle)]
pub static mut g_n_jcc_edges: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_xlat_jit: *mut ffi::OcerzJit = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_xlat_mode32: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_xlive_log: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_cc_want_cbz: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_cc_cbz_nz: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_cc_cbz_reg: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_cc_cbz_sf: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_tag_blk: *mut ffi::JitBlock = ptr::null_mut();
#[unsafe(no_mangle)]
pub static mut g_tag_idx: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_flip_n_retire: u64 = 0;
#[unsafe(no_mangle)]
#[thread_local]
pub static mut ocerz_jit_decode_recover: *mut ffi::sigjmp_buf = ptr::null_mut();

#[derive(Copy, Clone)]
struct XliveMemo {
    key: u64,
    live: u64,
    generation: u64,
    dhash: u64,
    head: [u8; 16],
    nd: u8,
    dep: [ffi::XliveDep; 6],
}

static mut G_XLIVE_DBUF: [u8; 64 * 1024] = [0; 64 * 1024];
static mut G_XLIVE_MEMO: [XliveMemo; ffi::XLIVE_MEMO_SLOTS as usize] = [XliveMemo {
    key: 0,
    live: 0,
    generation: 0,
    dhash: 0,
    head: [0; 16],
    nd: 0,
    dep: [ffi::XliveDep { pc: 0, len: 0 }; 6],
}; ffi::XLIVE_MEMO_SLOTS as usize];

static mut ENV_NO_FLIP: c_int = -1;
static mut ENV_NO_SUPERBLOCK: c_int = -1;
static mut ENV_NO_SB_BACK: c_int = -1;
static mut ENV_FLIP_JCC_N: c_int = -1;
static mut ENV_FLIP_JCC_RIPS: [u64; 32] = [0; 32];
static mut ENV_XLIVE_DEPTH: c_int = -1;
static mut ENV_NO_XLIVE_MEMO: c_int = -1;

unsafe extern "C" {
    fn jit_flags_guarded(
        f: unsafe extern "C" fn(*mut c_void),
        arg: *mut c_void,
    ) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn atoi(s: *const c_char) -> c_int;
    fn strtoull(s: *const c_char, end: *mut *mut c_char, base: c_int) -> u64;
    fn getpid() -> c_int;
    fn atexit(f: unsafe extern "C" fn()) -> c_int;
    fn clock_gettime_nsec_np(clock_id: c_int) -> u64;
    fn pthread_jit_write_protect_np(enabled: c_int);
    fn sys_icache_invalidate(start: *const c_void, len: usize);
    fn ocerz_vm_purge_jit_ras(vm: *mut ffi::OcerzVM);
}

const CLOCK_UPTIME_RAW: c_int = 8;
const A64_NOP_WORD: u32 = 0xd503_201f;
const PROBE_BIT: u32 = 10;
const WATCH_BIT: u32 = 16;
const FLIP_REARMS: u8 = 3;

static mut G_FLIP_N_HIT: u64 = 0;
static mut G_FLIP_NS_HIT: u64 = 0;
static mut G_FLIP_NS_RETIRE: u64 = 0;
static mut G_FLIP_ATEXIT_JIT: *mut ffi::OcerzJit = ptr::null_mut();
static mut G_NO_FLIP_REARM: c_int = -1;
static mut G_FLIPLOG: c_int = -1;
static mut G_NO_FLIP_NORETIRE: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn flip_disabled() -> c_int {
    unsafe {
        env_bool(ptr::addr_of_mut!(ENV_NO_FLIP), c"OCERZ_NO_FLIP", 1, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn superblock_enabled() -> c_int {
    unsafe {
        env_bool(ptr::addr_of_mut!(ENV_NO_SUPERBLOCK), c"OCERZ_NO_SUPERBLOCK", 0, 1)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn superblock_back_enabled() -> c_int {
    unsafe {
        env_bool(ptr::addr_of_mut!(ENV_NO_SB_BACK), c"OCERZ_NO_SB_BACK", 0, 1)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jcc_flip_wanted(rip: u64) -> c_int {
    unsafe {
        if ENV_FLIP_JCC_N < 0 {
            ENV_FLIP_JCC_N = 0;
            let mut e = getenv(c"OCERZ_FLIP_JCC".as_ptr());
            while !e.is_null() && *e != 0 && ENV_FLIP_JCC_N < 32 {
                let mut end = ptr::null_mut();
                let value = strtoull(e, &mut end, 0);
                if end == e {
                    break;
                }
                ENV_FLIP_JCC_RIPS[ENV_FLIP_JCC_N as usize] = value;
                ENV_FLIP_JCC_N += 1;
                e = if *end == b',' as c_char { end.add(1) } else { end };
            }
        }
        for i in 0..ENV_FLIP_JCC_N as usize {
            if ENV_FLIP_JCC_RIPS[i] == rip {
                return 1;
            }
        }
        (jit_internal::flip_state(rip) == ffi::FLIP_DECIDED_INV as c_int) as c_int
    }
}

unsafe fn env_bool(cache: *mut c_int, name: &'static CStr, set: c_int, unset: c_int) -> c_int {
    unsafe {
        let current = ptr::read(cache);
        if current >= 0 {
            return current;
        }
        let value = if !getenv(name.as_ptr()).is_null() {
            set
        } else {
            unset
        };
        ptr::write(cache, value);
        value
    }
}

macro_rules! env_on {
    ($name:literal) => {{
        static mut CACHE: c_int = -1;
        unsafe { env_bool(ptr::addr_of_mut!(CACHE), $name, 1, 0) != 0 }
    }};
}

fn xlive_dep_mix(mut h: u64, w: u64) -> u64 {
    h = (h ^ w).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    h ^ (h >> 29)
}

unsafe fn xlive_dep_hash(d: *const ffi::XliveDep, nd: c_int, bytes: *const u8) -> u64 {
    unsafe {
        let mut h = 0x1319_8a2e_0370_7344;
        let mut at = 0usize;
        for i in 0..nd as usize {
            let dep = &*d.add(i);
            h = xlive_dep_mix(xlive_dep_mix(h, dep.pc), dep.len as u64);
            let len = dep.len as usize;
            let mut k = 0usize;
            while k + 8 <= len {
                let mut word = 0u64;
                ptr::copy_nonoverlapping(bytes.add(at + k), (&mut word as *mut u64).cast(), 8);
                h = xlive_dep_mix(h, word);
                k += 8;
            }
            while k < len {
                h = xlive_dep_mix(h, *bytes.add(at + k) as u64);
                k += 1;
            }
            at = at.wrapping_add(len);
        }
        h
    }
}

#[repr(C)]
struct FetchState {
    dep: *const ffi::XliveDep,
    nd: c_int,
    dst: *mut u8,
    room: u32,
    at: u32,
    fits: c_int,
    ok: c_int,
}

unsafe extern "C" fn xlive_deps_fetch_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<FetchState>();
        let mut at = 0u32;
        let mut fits = 1;
        ptr::write_volatile(&mut (*s).at, at);
        ptr::write_volatile(&mut (*s).fits, fits);
        for i in 0..(*s).nd {
            if fits == 0 {
                break;
            }
            let dep = &*(*s).dep.add(i as usize);
            if at.wrapping_add(dep.len) > (*s).room {
                fits = 0;
                ptr::write_volatile(&mut (*s).fits, fits);
                break;
            }
            let src = jit_internal::ocerz_g2h(dep.pc).cast::<u8>();
            ptr::copy_nonoverlapping(src, (*s).dst.add(at as usize), dep.len as usize);
            at = at.wrapping_add(dep.len);
            ptr::write_volatile(&mut (*s).at, at);
        }
        ptr::write_volatile(&mut (*s).ok, fits);
    }
}

unsafe fn xlive_deps_fetch(
    d: *const ffi::XliveDep,
    nd: c_int,
    dst: *mut u8,
    room: u32,
) -> c_int {
    unsafe {
        let mut state = FetchState {
            dep: d,
            nd,
            dst,
            room,
            at: 0,
            fits: 1,
            ok: 0,
        };
        let complete = jit_flags_guarded(
            xlive_deps_fetch_cb,
            (&mut state as *mut FetchState).cast(),
        );
        (complete != 0 && ptr::read_volatile(&state.ok) != 0) as c_int
    }
}

unsafe extern "C" fn cmp_xdep(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = &*a.cast::<ffi::XliveDep>();
        let y = &*b.cast::<ffi::XliveDep>();
        (x.pc > y.pc) as c_int - (x.pc < y.pc) as c_int
    }
}

unsafe fn xlive_deps_since(start: c_int, out: *mut ffi::XliveDep, hash: *mut u64) -> u8 {
    unsafe {
        const MAXE: usize = 512;
        let mut e = MaybeUninit::<[ffi::XliveDep; MAXE]>::uninit();
        let e_ptr = e.as_mut_ptr().cast::<ffi::XliveDep>();
        let n = g_tc_ndlog - start;
        if ffi::g_tc_bad != 0 || n <= 0 || n as usize > MAXE {
            return ffi::XLIVE_NO_DEPS as u8;
        }
        for i in 0..n as usize {
            let src = ptr::addr_of!(ffi::g_tc_dlog).cast::<ffi::JitState_g_tc_dlog>().add((start as usize) + i);
            ptr::write(
                e_ptr.add(i),
                ffi::XliveDep {
                    pc: (*src).pc,
                    len: (*src).len,
                },
            );
        }
        core::slice::from_raw_parts_mut(e_ptr, n as usize)
            .sort_unstable_by_key(|dep| dep.pc);
        let mut nd = 0usize;
        for i in 0..n as usize {
            let dep = *e_ptr.add(i);
            if nd != 0 {
                let prev = &mut *out.add(nd - 1);
                if dep.pc <= prev.pc.wrapping_add(prev.len as u64) {
                    let end = dep.pc.wrapping_add(dep.len as u64);
                    if end > prev.pc.wrapping_add(prev.len as u64) {
                        prev.len = end.wrapping_sub(prev.pc) as u32;
                    }
                    continue;
                }
            }
            if nd == ffi::XLIVE_MEMO_DEPS as usize {
                return ffi::XLIVE_NO_DEPS as u8;
            }
            *out.add(nd) = dep;
            nd += 1;
        }
        if xlive_deps_fetch(
            out,
            nd as c_int,
            ptr::addr_of_mut!(G_XLIVE_DBUF).cast(),
            64 * 1024,
        ) == 0
        {
            return ffi::XLIVE_NO_DEPS as u8;
        }
        *hash = xlive_dep_hash(out, nd as c_int, ptr::addr_of!(G_XLIVE_DBUF).cast());
        nd as u8
    }
}

unsafe fn xlive_deps_replay(d: *const ffi::XliveDep, nd: c_int, want: u64) -> c_int {
    unsafe {
        if g_tc_ndlog + nd > ffi::TC_DLOG_MAX as c_int
            || xlive_deps_fetch(
                d,
                nd,
                ptr::addr_of_mut!(ffi::g_tc_dbytes)
                    .cast::<u8>()
                    .add(ffi::g_tc_nbytes as usize),
                (ffi::TC_DBYTES_MAX as usize - ffi::g_tc_nbytes as usize) as u32,
            ) == 0
            || xlive_dep_hash(
                d,
                nd,
                ptr::addr_of!(ffi::g_tc_dbytes)
                    .cast::<u8>()
                    .add(ffi::g_tc_nbytes as usize),
            ) != want
        {
            return 0;
        }
        for i in 0..nd as usize {
            let dst = ptr::addr_of_mut!(ffi::g_tc_dlog)
                .cast::<ffi::JitState_g_tc_dlog>()
                .add(g_tc_ndlog as usize);
            (*dst).pc = (*d.add(i)).pc;
            (*dst).at = ffi::g_tc_nbytes;
            (*dst).len = (*d.add(i)).len;
            g_tc_ndlog += 1;
            ffi::g_tc_nbytes += (*d.add(i)).len;
        }
        1
    }
}

#[repr(C)]
struct DecodeState {
    rip: u64,
    pc: u64,
    insns: *mut ffi::X86Insn,
    mode32: c_int,
    n: c_int,
}

unsafe extern "C" fn decode_block_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<DecodeState>();
        let mut n = 0;
        let mut pc = (*s).rip;
        ptr::write_volatile(&mut (*s).n, n);
        ptr::write_volatile(&mut (*s).pc, pc);
        while n < ffi::JIT_MAX_BLOCK_INSNS as c_int {
            let insn = (*s).insns.add(n as usize);
            if ffi::jit_decode(pc, insn, (*s).mode32) != ffi::OCERZ_OK {
                break;
            }
            let op = (*insn).op;
            let len = (*insn).len;
            n += 1;
            ptr::write_volatile(&mut (*s).n, n);
            if ffi::is_terminator(op as c_uint) != 0 {
                break;
            }
            pc = pc.wrapping_add(len as u64);
            ptr::write_volatile(&mut (*s).pc, pc);
        }
    }
}

#[repr(C)]
struct HeadState {
    rip: u64,
    head: [u8; 16],
    have_head: c_int,
}

unsafe extern "C" fn copy_head_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<HeadState>();
        ptr::copy_nonoverlapping(
            jit_internal::ocerz_g2h((*s).rip).cast::<u8>(),
            (*s).head.as_mut_ptr(),
            16,
        );
        ptr::write_volatile(&mut (*s).have_head, 1);
    }
}

#[repr(C)]
struct ScanState {
    rip: u64,
    pc: u64,
    insn: MaybeUninit<ffi::X86Insn>,
    mode32: c_int,
    purpose: c_int,
    compatible: c_int,
    term: c_uint,
    rsp_ok: c_int,
}

unsafe extern "C" fn decode_scan_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<ScanState>();
        let mut pc = (*s).rip;
        ptr::write_volatile(&mut (*s).pc, pc);
        ptr::write_volatile(&mut (*s).compatible, 0);
        ptr::write_volatile(&mut (*s).term, 0);
        ptr::write_volatile(&mut (*s).rsp_ok, 1);
        for _ in 0..ffi::JIT_MAX_BLOCK_INSNS {
            let insn = ptr::addr_of_mut!((*s).insn).cast::<ffi::X86Insn>();
            if ffi::jit_decode(pc, insn, (*s).mode32) != ffi::OCERZ_OK {
                break;
            }
            let insn = insn.cast_const();
            let op = (*insn).op;
            let len = (*insn).len;
            if ffi::is_terminator(op as c_uint) != 0 {
                ptr::write_volatile(&mut (*s).term, op as c_uint);
                if (*s).purpose == 0 {
                    ptr::write_volatile(
                        &mut (*s).compatible,
                        (op == ffi::OCERZ_OP_JCC || op == ffi::OCERZ_OP_JMP) as c_int,
                    );
                } else if (*s).purpose == 2 {
                    if op == ffi::OCERZ_OP_CALL || op == ffi::OCERZ_OP_RET {
                        ptr::write_volatile(&mut (*s).compatible, ptr::read_volatile(&(*s).rsp_ok));
                    } else if op == ffi::OCERZ_OP_JCC
                        && ptr::read_volatile(&(*s).rsp_ok) != 0
                        && (*insn).ops[0].kind == ffi::OCERZ_OPK_IMM
                    {
                        let target = (*insn).ops[0].imm;
                        let fall = (*insn).rip.wrapping_add((*insn).len as u64);
                        let compatible = jit_internal::call_body_successor(target) != 0
                            && jit_internal::call_body_successor(
                                fall,
                            ) != 0;
                        ptr::write_volatile(&mut (*s).compatible, compatible as c_int);
                    }
                }
                break;
            }
            if (*s).purpose == 2 {
                for k in 0..(*insn).nops as usize {
                    let o = ptr::addr_of!((*insn).ops[k]);
                    let kind = (*o).kind;
                    let reg = (*o).reg;
                    let base = (*o).base;
                    let index = (*o).index;
                    let rsp = (kind == ffi::OCERZ_OPK_REG && (reg & 15) == ffi::OCERZ_RSP)
                        || (kind == ffi::OCERZ_OPK_MEM
                            && ((base != ffi::OCERZ_REG_NONE && (base & 15) == ffi::OCERZ_RSP)
                                || (index != ffi::OCERZ_REG_NONE
                                    && (index & 15) == ffi::OCERZ_RSP)));
                    if rsp
                        && !(op == ffi::OCERZ_OP_MOV
                            && k == 1
                            && kind == ffi::OCERZ_OPK_REG)
                    {
                        ptr::write_volatile(&mut (*s).rsp_ok, 0);
                        break;
                    }
                }
                if ptr::read_volatile(&(*s).rsp_ok) == 0 {
                    break;
                }
            }
            pc = pc.wrapping_add(len as u64);
            ptr::write_volatile(&mut (*s).pc, pc);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn xlive_decode_entry_d(rip: u64, depth: c_int) -> u64 {
    unsafe {
        if ENV_XLIVE_DEPTH < 0 {
            let e = getenv(c"OCERZ_XLIVE_DEPTH".as_ptr());
            ENV_XLIVE_DEPTH = if e.is_null() { 3 } else { atoi(e) };
        }
        if ENV_NO_XLIVE_MEMO < 0 {
            ENV_NO_XLIVE_MEMO =
                (!getenv(c"OCERZ_NO_XLIVE_MEMO".as_ptr()).is_null()) as c_int;
        }
        if g_xlive_log < 0 {
            g_xlive_log = (!getenv(c"OCERZ_XLIVELOG".as_ptr()).is_null()) as c_int;
        }
        let memo_on = ENV_NO_XLIVE_MEMO == 0;
        let mkey = (rip.wrapping_shl(4)
            | ((depth as u64) << 1)
            | ((g_xlat_mode32 != 0) as u64))
        .wrapping_add(1);
        let mgen = core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(ffi::ocerz_jit_retire_count))
            .load(core::sync::atomic::Ordering::Relaxed);
        let mslot = ((mkey.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> 51)
            & (ffi::XLIVE_MEMO_SLOTS as u64 - 1)) as usize;
        let mut hs = HeadState {
            rip,
            head: [0; 16],
            have_head: 0,
        };
        let dstart = g_tc_ndlog;
        let dbar = ffi::g_tc_dbar;
        ffi::g_tc_dbar = dstart;
        if memo_on {
            let _ = jit_flags_guarded(copy_head_cb, (&mut hs as *mut HeadState).cast());
            if ptr::read_volatile(&hs.have_head) != 0 {
                let memo = ptr::addr_of!(G_XLIVE_MEMO).cast::<XliveMemo>().add(mslot);
                if (*memo).key == mkey
                    && (*memo).generation == mgen
                    && (*memo).head == hs.head
                    && (g_tc_rec == 0
                        || ((*memo).nd != ffi::XLIVE_NO_DEPS as u8
                            && xlive_deps_replay(
                                (*memo).dep.as_ptr(),
                                (*memo).nd as c_int,
                                (*memo).dhash,
                            ) != 0))
                {
                    ffi::g_tc_dbar = dbar;
                    return (*memo).live;
                }
            }
        }
        let mut insns =
            MaybeUninit::<[ffi::X86Insn; ffi::JIT_MAX_BLOCK_INSNS as usize]>::uninit();
        let insn_ptr = insns.as_mut_ptr().cast::<ffi::X86Insn>();
        let mut ds = DecodeState {
            rip,
            pc: rip,
            insns: insn_ptr,
            mode32: g_xlat_mode32,
            n: 0,
        };
        let _ = jit_flags_guarded(decode_block_cb, (&mut ds as *mut DecodeState).cast());
        let n = ptr::read_volatile(&ds.n);
        if n == 0 {
            ffi::g_tc_dbar = dbar;
            return ffi::OCERZ_FL_ALL;
        }
        let mut live = ffi::OCERZ_FL_ALL;
        let last = &*insn_ptr.add((n - 1) as usize);
        if depth < ENV_XLIVE_DEPTH && ffi::is_terminator(last.op as c_uint) != 0 {
            if (last.op == ffi::OCERZ_OP_JMP || last.op == ffi::OCERZ_OP_CALL)
                && last.ops[0].kind == ffi::OCERZ_OPK_IMM
            {
                live = jit_internal::xlive_succ_live_d(
                    g_xlat_jit,
                    last.ops[0].imm,
                    depth + 1,
                );
            } else if last.op == ffi::OCERZ_OP_JCC && last.ops[0].kind == ffi::OCERZ_OPK_IMM {
                live = jit_internal::xlive_succ_live_d(
                    g_xlat_jit,
                    last.ops[0].imm,
                    depth + 1,
                ) | jit_internal::xlive_succ_live_d(
                    g_xlat_jit,
                    last.rip.wrapping_add(last.len as u64),
                    depth + 1,
                );
            }
        }
        for i in (0..n as usize).rev() {
            let mut def = 0u64;
            let mut used = 0u64;
            ffi::ocerz_flags_defuse(insn_ptr.add(i), &mut def, &mut used);
            live = (live & !def) | used;
        }
        if g_xlive_log != 0 {
            ffi::fprintf(
                ffi::stderr,
                c"ocerz: XLIVE rip=%#llx depth=%d n=%d term=%d live=%#llx\n".as_ptr(),
                rip,
                depth,
                n,
                (*insn_ptr.add((n - 1) as usize)).op as c_int,
                live,
            );
        }
        if ptr::read_volatile(&hs.have_head) != 0 {
            let memo = ptr::addr_of_mut!(G_XLIVE_MEMO).cast::<XliveMemo>().add(mslot);
            (*memo).key = mkey;
            (*memo).live = live;
            (*memo).generation = mgen;
            (*memo).head = hs.head;
            (*memo).nd = if g_tc_rec != 0 {
                xlive_deps_since(dstart, (*memo).dep.as_mut_ptr(), &mut (*memo).dhash)
            } else {
                ffi::XLIVE_NO_DEPS as u8
            };
        }
        ffi::g_tc_dbar = dbar;
        live
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn canonical_body_successor(rip: u64) -> c_int {
    unsafe {
        let mut state = ScanState {
            rip,
            pc: rip,
            insn: MaybeUninit::uninit(),
            mode32: g_xlat_mode32,
            purpose: 0,
            compatible: 0,
            term: 0,
            rsp_ok: 1,
        };
        let _ = jit_flags_guarded(decode_scan_cb, (&mut state as *mut ScanState).cast());
        ptr::read_volatile(&state.compatible)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn decoded_terminator(rip: u64) -> c_uint {
    unsafe {
        let mut state = ScanState {
            rip,
            pc: rip,
            insn: MaybeUninit::uninit(),
            mode32: g_xlat_mode32,
            purpose: 1,
            compatible: 0,
            term: 0,
            rsp_ok: 1,
        };
        let _ = jit_flags_guarded(decode_scan_cb, (&mut state as *mut ScanState).cast());
        ptr::read_volatile(&state.term)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn decoded_call_region_entry(rip: u64) -> c_int {
    unsafe {
        let mut state = ScanState {
            rip,
            pc: rip,
            insn: MaybeUninit::uninit(),
            mode32: g_xlat_mode32,
            purpose: 2,
            compatible: 0,
            term: 0,
            rsp_ok: 1,
        };
        let _ = jit_flags_guarded(decode_scan_cb, (&mut state as *mut ScanState).cast());
        ptr::read_volatile(&state.compatible)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_pf(b: *mut ffi::A64Buf, res: c_int) {
    unsafe {
        ffi::a64_uxtb(b, ffi::JTT, res);
        ffi::a64_lsr_imm(b, 0, ffi::JTU, ffi::JTT, 4);
        ffi::a64_eor_reg(b, 0, ffi::JTT, ffi::JTT, ffi::JTU, 0);
        ffi::a64_lsr_imm(b, 0, ffi::JTU, ffi::JTT, 2);
        ffi::a64_eor_reg(b, 0, ffi::JTT, ffi::JTT, ffi::JTU, 0);
        ffi::a64_lsr_imm(b, 0, ffi::JTU, ffi::JTT, 1);
        ffi::a64_eor_reg(b, 0, ffi::JTT, ffi::JTT, ffi::JTU, 0);
        ffi::a64_mov_imm64(b, ffi::JTU, 1);
        ffi::a64_bic_reg(b, 0, ffi::JTT, ffi::JTU, ffi::JTT, 0);
        ffi::a64_lsl_imm(b, 0, ffi::JTT, ffi::JTT, 2);
        ffi::a64_orr_reg(b, 1, ffi::JTF, ffi::JTF, ffi::JTT, 0);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_materialize(b: *mut ffi::A64Buf) {
    unsafe {
        if ffi::g_defer == 0 {
            return;
        }
        ffi::a64_ldr(b, 4, ffi::JT0, 20, ffi::CC_OP_OFF);
        let skip = ffi::a64_label(b);
        ffi::a64_cbz(b, 0, ffi::JT0, 0);
        jit_internal::emit_xmm_pin_spill_all(b);
        jit_internal::emit_spill_pinned_callersaved(b);
        for v in (4..16).step_by(2) {
            if ffi::g_lane_used & (3 << (v - 4)) != 0 {
                ffi::a64_stp_q_pre(b, v as c_int, v as c_int + 1, 31, -32);
            }
        }
        ffi::a64_mov_reg(b, 1, 0, 20);
        jit_internal::tc_imm64(
            b,
            16,
            ffi::TCR_SYM,
            ffi::TCS_FLAGS_MATERIALIZE,
            ffi::ocerz_flags_materialize as usize as u64,
        );
        ffi::a64_blr(b, 16);
        ffi::g_callout_seq = ffi::g_callout_seq.wrapping_add(1);
        for v in (4..16).step_by(2).rev() {
            if ffi::g_lane_used & (3 << (v - 4)) != 0 {
                ffi::a64_ldp_q_post(b, v as c_int, v as c_int + 1, 31, 32);
            }
        }
        jit_internal::emit_fill_pinned_callersaved(b);
        jit_internal::emit_reload_jgb(b);
        ffi::emit_reload_mem_base(b);
        jit_internal::emit_xmm_pin_load_all(b);
        ffi::a64_patch_cbz(skip, ffi::a64_label(b));
    }
}

unsafe fn emit_cc_predicate_rflags(b: *mut ffi::A64Buf, cc: c_uint) {
    unsafe {
        ffi::a64_ldr(b, 8, ffi::JT0, 20, ffi::RF_OFF);
        ffi::a64_ubfx(b, 1, ffi::JT1, ffi::JT0, 0, 1);
        ffi::a64_ubfx(b, 1, ffi::JTA, ffi::JT0, 6, 1);
        ffi::a64_ubfx(b, 1, ffi::JTT, ffi::JT0, 7, 1);
        ffi::a64_ubfx(b, 1, ffi::JTU, ffi::JT0, 11, 1);
        match cc >> 1 {
            0 => ffi::a64_mov_reg(b, 1, ffi::JTF, ffi::JTU),
            1 => ffi::a64_mov_reg(b, 1, ffi::JTF, ffi::JT1),
            2 => ffi::a64_mov_reg(b, 1, ffi::JTF, ffi::JTA),
            3 => ffi::a64_orr_reg(b, 1, ffi::JTF, ffi::JT1, ffi::JTA, 0),
            4 => ffi::a64_mov_reg(b, 1, ffi::JTF, ffi::JTT),
            5 => ffi::a64_ubfx(b, 1, ffi::JTF, ffi::JT0, 2, 1),
            6 => ffi::a64_eor_reg(b, 1, ffi::JTF, ffi::JTT, ffi::JTU, 0),
            _ => {
                ffi::a64_eor_reg(b, 1, ffi::JTF, ffi::JTT, ffi::JTU, 0);
                ffi::a64_orr_reg(b, 1, ffi::JTF, ffi::JTF, ffi::JTA, 0);
            }
        }
        if cc & 1 != 0 {
            ffi::a64_mov_imm64(b, ffi::JTU, 1);
            ffi::a64_eor_reg(b, 1, ffi::JTF, ffi::JTF, ffi::JTU, 0);
        }
    }
}

fn cc_after_subs(cc: c_uint) -> c_int {
    const T: [c_int; 16] = [
        ffi::A64_VS as c_int,
        ffi::A64_VC as c_int,
        ffi::A64_CC as c_int,
        ffi::A64_CS as c_int,
        ffi::A64_EQ as c_int,
        ffi::A64_NE as c_int,
        ffi::A64_LS as c_int,
        ffi::A64_HI as c_int,
        ffi::A64_MI as c_int,
        ffi::A64_PL as c_int,
        -1,
        -1,
        ffi::A64_LT as c_int,
        ffi::A64_GE as c_int,
        ffi::A64_LE as c_int,
        ffi::A64_GT as c_int,
    ];
    if cc < 16 { T[cc as usize] } else { -1 }
}

fn cc_after_ands(cc: c_uint) -> c_int {
    match cc {
        ffi::OCERZ_CC_O | ffi::OCERZ_CC_B => ffi::A64_NV as c_int,
        ffi::OCERZ_CC_NO | ffi::OCERZ_CC_AE => ffi::A64_AL as c_int,
        ffi::OCERZ_CC_E => ffi::A64_EQ as c_int,
        ffi::OCERZ_CC_NE => ffi::A64_NE as c_int,
        ffi::OCERZ_CC_BE => ffi::A64_EQ as c_int,
        ffi::OCERZ_CC_A => ffi::A64_NE as c_int,
        ffi::OCERZ_CC_S => ffi::A64_MI as c_int,
        ffi::OCERZ_CC_NS => ffi::A64_PL as c_int,
        ffi::OCERZ_CC_L => ffi::A64_MI as c_int,
        ffi::OCERZ_CC_GE => ffi::A64_PL as c_int,
        ffi::OCERZ_CC_LE => ffi::A64_LE as c_int,
        ffi::OCERZ_CC_G => ffi::A64_GT as c_int,
        _ => -1,
    }
}

unsafe fn producer_record_kind(p: *const ffi::X86Insn, size: *mut c_int) -> c_uint {
    unsafe {
        if p.is_null() {
            return 0;
        }
        match (*p).op {
            ffi::OCERZ_OP_CMP | ffi::OCERZ_OP_SUB => {
                *size = (*p).ops[0].size as c_int;
                ffi::OCERZ_CC_SUB
            }
            ffi::OCERZ_OP_TEST | ffi::OCERZ_OP_AND | ffi::OCERZ_OP_OR
            | ffi::OCERZ_OP_XOR => {
                *size = (*p).ops[0].size as c_int;
                ffi::OCERZ_CC_LOGIC
            }
            ffi::OCERZ_OP_ADD => {
                *size = (*p).ops[0].size as c_int;
                ffi::OCERZ_CC_ADD
            }
            ffi::OCERZ_OP_SHL | ffi::OCERZ_OP_SHR | ffi::OCERZ_OP_SAR => {
                if (*p).ops[1].kind == ffi::OCERZ_OPK_IMM
                    && (*p).ops[0].kind == ffi::OCERZ_OPK_REG
                    && ((*p).ops[0].size == 4 || (*p).ops[0].size == 8)
                {
                    *size = (*p).ops[0].size as c_int;
                    if (*p).op == ffi::OCERZ_OP_SHL {
                        ffi::OCERZ_CC_SHL
                    } else if (*p).op == ffi::OCERZ_OP_SHR {
                        ffi::OCERZ_CC_SHR
                    } else {
                        ffi::OCERZ_CC_SAR
                    }
                } else {
                    0
                }
            }
            _ => 0,
        }
    }
}

unsafe fn cc_consumer_inline_ok(c: *const ffi::X86Insn) -> c_int {
    unsafe {
        let d = ptr::addr_of!((*c).ops[0]);
        if (*c).mode32 != 0
            && (*c).op != ffi::OCERZ_OP_JCC
            && ffi::m32_inline_ok(c) == 0
        {
            return 0;
        }
        match (*c).op {
            ffi::OCERZ_OP_JCC => 1,
            ffi::OCERZ_OP_SETCC => {
                if env_on!(c"OCERZ_NO_INLINE_SETCC") {
                    return 0;
                }
                ((*d).kind == ffi::OCERZ_OPK_REG
                    && (*d).high8 == 0
                    && (*d).size == 1
                    && !(jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP))
                    as c_int
            }
            ffi::OCERZ_OP_CMOVCC => {
                if env_on!(c"OCERZ_NO_INLINE_CMOV") {
                    return 0;
                }
                let sr = ptr::addr_of!((*c).ops[1]);
                if (*d).kind != ffi::OCERZ_OPK_REG
                    || (*d).high8 != 0
                    || ((*d).size != 4 && (*d).size != 8)
                {
                    return 0;
                }
                if jit_internal::rsp_is_ptr() != 0
                    && ((*d).reg == ffi::OCERZ_RSP
                        || ((*sr).kind == ffi::OCERZ_OPK_REG && (*sr).reg == ffi::OCERZ_RSP))
                {
                    return 0;
                }
                if (*sr).kind == ffi::OCERZ_OPK_REG {
                    (!((*sr).high8 != 0) && (*sr).size == (*d).size) as c_int
                } else {
                    ((*sr).kind == ffi::OCERZ_OPK_MEM
                        && ((*c).addrsize == 8 || (*c).mode32 != 0)) as c_int
                }
            }
            ffi::OCERZ_OP_ADC | ffi::OCERZ_OP_SBB => {
                let sr = ptr::addr_of!((*c).ops[1]);
                if ffi::g_defer == 0
                    || (*d).kind != ffi::OCERZ_OPK_REG
                    || (*d).high8 != 0
                    || ((*d).size != 4 && (*d).size != 8)
                    || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
                {
                    return 0;
                }
                if (*sr).kind == ffi::OCERZ_OPK_REG {
                    ((!((*sr).high8 != 0)) && (*sr).size == (*d).size) as c_int
                } else {
                    ((*sr).kind == ffi::OCERZ_OPK_IMM) as c_int
                }
            }
            _ => 0,
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comis_fuse_producer(insns: *const ffi::X86Insn, ci: c_int) -> c_int {
    unsafe {
        let c = insns.add(ci as usize);
        if cc_consumer_inline_ok(c) == 0 {
            return -1;
        }
        let cc = (*c).cc as c_uint;
        if !(cc == ffi::OCERZ_CC_A
            || cc == ffi::OCERZ_CC_AE
            || cc == ffi::OCERZ_CC_B
            || cc == ffi::OCERZ_CC_BE
            || cc == ffi::OCERZ_CC_P
            || cc == ffi::OCERZ_CC_NP
            || cc == ffi::OCERZ_CC_E
            || cc == ffi::OCERZ_CC_NE)
        {
            return -1;
        }
        let mut pi = -1;
        for k in (0..ci).rev() {
            let mut def = 0u64;
            let mut used = 0u64;
            ffi::ocerz_flags_defuse(insns.add(k as usize), &mut def, &mut used);
            if def & ffi::JIT_ARITH_FLAGS != 0 {
                pi = k;
                break;
            }
        }
        if pi < 0 {
            return -1;
        }
        let p = insns.add(pi as usize);
        if !((*p).op == ffi::OCERZ_OP_UCOMISD
            || (*p).op == ffi::OCERZ_OP_UCOMISS
            || (*p).op == ffi::OCERZ_OP_COMISD
            || (*p).op == ffi::OCERZ_OP_COMISS)
        {
            return -1;
        }
        if (*p).ops[0].kind != ffi::OCERZ_OPK_XMM
            || jit_internal::xmm_is_pinned((*p).ops[0].reg) == 0
        {
            return -1;
        }
        let smem = ((*p).ops[1].kind == ffi::OCERZ_OPK_MEM
            && !env_on!(c"OCERZ_NO_COMIS_MEM_FUSE")) as c_int;
        if smem == 0
            && ((*p).ops[1].kind != ffi::OCERZ_OPK_XMM
                || jit_internal::xmm_is_pinned((*p).ops[1].reg) == 0)
        {
            return -1;
        }
        for k in pi + 1..ci {
            let m = insns.add(k as usize);
            if (*m).nops > 0
                && (*m).ops[0].kind == ffi::OCERZ_OPK_XMM
                && ((*m).ops[0].reg == (*p).ops[0].reg
                    || (smem == 0 && (*m).ops[0].reg == (*p).ops[1].reg))
            {
                return -1;
            }
            let mut mdef = 0u64;
            let mut muse = 0u64;
            ffi::ocerz_flags_defuse_nofault(m, &mut mdef, &mut muse);
            if mdef & ffi::JIT_ARITH_FLAGS == 0 && muse & ffi::JIT_ARITH_FLAGS == ffi::JIT_ARITH_FLAGS {
                return -1;
            }
        }
        pi
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn value_cond_fuse_producer(insns: *const ffi::X86Insn, ci: c_int) -> c_int {
    unsafe {
        static mut DIS: c_int = -1;
        if DIS < 0 {
            DIS = (!getenv(c"OCERZ_NO_VALCC".as_ptr()).is_null()) as c_int;
        }
        if DIS != 0 {
            return -1;
        }
        let c = insns.add(ci as usize);
        if cc_consumer_inline_ok(c) == 0 {
            return -1;
        }
        let cc = (*c).cc as c_uint;
        if !(cc == ffi::OCERZ_CC_E || cc == ffi::OCERZ_CC_NE || cc == ffi::OCERZ_CC_S || cc == ffi::OCERZ_CC_NS) {
            return -1;
        }
        let mut pi = -1;
        for k in (0..ci).rev() {
            let mut def = 0u64;
            let mut used = 0u64;
            ffi::ocerz_flags_defuse(insns.add(k as usize), &mut def, &mut used);
            if def & ffi::JIT_ARITH_FLAGS != 0 {
                pi = k;
                break;
            }
        }
        if pi < 0 {
            return -1;
        }
        let p = insns.add(pi as usize);
        static mut ONLY: c_int = -2;
        if ONLY == -2 {
            let e = getenv(c"OCERZ_VALCC_ONLY".as_ptr());
            ONLY = if e.is_null() { -1 } else { atoi(e) };
        }
        if ONLY >= 0 && (*p).op as c_int != ONLY {
            return -1;
        }
        match (*p).op {
            ffi::OCERZ_OP_ADD | ffi::OCERZ_OP_SUB | ffi::OCERZ_OP_AND | ffi::OCERZ_OP_OR
            | ffi::OCERZ_OP_XOR | ffi::OCERZ_OP_INC | ffi::OCERZ_OP_DEC | ffi::OCERZ_OP_NEG => {}
            ffi::OCERZ_OP_SHL | ffi::OCERZ_OP_SHR | ffi::OCERZ_OP_SAR => {
                if (*p).nops < 2
                    || (*p).ops[1].kind != ffi::OCERZ_OPK_IMM
                    || ((*p).ops[1].imm & if (*p).ops[0].size == 8 { 63 } else { 31 }) == 0
                {
                    return -1;
                }
            }
            _ => return -1,
        }
        let d = ptr::addr_of!((*p).ops[0]);
        if (*d).kind != ffi::OCERZ_OPK_REG
            || (*d).high8 != 0
            || ((*d).size != 4 && (*d).size != 8)
            || jit_internal::pin_slot((*d).reg) < 0
            || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
        {
            return -1;
        }
        for k in pi + 1..ci {
            let m = insns.add(k as usize);
            if ffi::insn_may_write_gpr(m, (*d).reg as c_uint) != 0 {
                return -1;
            }
            let mut mdef = 0u64;
            let mut muse = 0u64;
            ffi::ocerz_flags_defuse_nofault(m, &mut mdef, &mut muse);
            if mdef & ffi::JIT_ARITH_FLAGS == 0 && muse & ffi::JIT_ARITH_FLAGS == ffi::JIT_ARITH_FLAGS {
                return -1;
            }
        }
        pi
    }
}

unsafe fn mem_plain_ok(insn: *const ffi::X86Insn, op: *const ffi::X86Operand) -> c_int {
    unsafe {
        if jit_internal::mem_fast_forms_ok() == 0
            || (*insn).seg != ffi::OCERZ_SEG_NONE
            || (*insn).addrsize != 8
            || (jit_internal::rsp_is_ptr() != 0
                && ((*op).base == ffi::OCERZ_RSP || (*op).index == ffi::OCERZ_RSP))
        {
            return 0;
        }
        if (*op).riprel != 0 {
            return 1;
        }
        if ((*op).base != ffi::OCERZ_REG_NONE && jit_internal::pin_slot((*op).base) < 0)
            || ((*op).index != ffi::OCERZ_REG_NONE && jit_internal::pin_slot((*op).index) < 0)
        {
            return 0;
        }
        1
    }
}

unsafe fn rmw_nzcv_ok(p: *const ffi::X86Insn, m: *const ffi::X86Operand) -> c_int {
    unsafe {
        if env_on!(c"OCERZ_NO_INLINE_RMW")
            || env_on!(c"OCERZ_NO_NZCV_MEMDST")
            || (*p).seg != ffi::OCERZ_SEG_NONE
            || (*p).addrsize != 8
            || (*p).lock != 0
            || jit_internal::mem_native_store_ok() == 0
            || (jit_internal::rsp_is_ptr() != 0 && (*m).index == ffi::OCERZ_RSP)
        {
            return 0;
        }
        1
    }
}

fn nzcv_gap_max() -> c_int {
    static mut V: c_int = -1;
    unsafe {
        if V < 0 {
            let e = getenv(c"OCERZ_NZCV_GAP".as_ptr());
            V = if e.is_null() { ffi::NZCV_GAP_MAX as c_int } else { atoi(e) };
            if V < 0 {
                V = 0;
            }
            if V > ffi::NZCV_GAP_MAX as c_int {
                V = ffi::NZCV_GAP_MAX as c_int;
            }
        }
        V
    }
}

unsafe fn nzcv_producer_candidate(p: *const ffi::X86Insn) -> c_int {
    unsafe {
        match (*p).op {
            ffi::OCERZ_OP_CMP
            | ffi::OCERZ_OP_SUB
            | ffi::OCERZ_OP_ADD
            | ffi::OCERZ_OP_TEST
            | ffi::OCERZ_OP_AND
            | ffi::OCERZ_OP_OR
            | ffi::OCERZ_OP_XOR
            | ffi::OCERZ_OP_BSF
            | ffi::OCERZ_OP_BSR
            | ffi::OCERZ_OP_BT
            | ffi::OCERZ_OP_BTS
            | ffi::OCERZ_OP_BTR
            | ffi::OCERZ_OP_BTC => 1,
            _ => 0,
        }
    }
}

unsafe fn flag_neutral_ok(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        if ffi::g_pin_class != 3 {
            return 0;
        }
        if (*in_).op == ffi::OCERZ_OP_LEA {
            let d = ptr::addr_of!((*in_).ops[0]);
            let m = ptr::addr_of!((*in_).ops[1]);
            if (*d).kind != ffi::OCERZ_OPK_REG
                || (*d).high8 != 0
                || ((*d).size != 4 && (*d).size != 8)
                || (*m).kind != ffi::OCERZ_OPK_MEM
                || (*m).riprel != 0
                || (*in_).addrsize != 8
                || (*in_).seg != ffi::OCERZ_SEG_NONE
                || (*m).base == ffi::OCERZ_REG_NONE
                || jit_internal::pin_slot((*m).base) < 0
            {
                return 0;
            }
            let has_idx = (*m).index != ffi::OCERZ_REG_NONE;
            if has_idx && jit_internal::pin_slot((*m).index) < 0 {
                return 0;
            }
            if jit_internal::rsp_is_ptr() != 0
                && ((*d).reg == ffi::OCERZ_RSP
                    || (*m).base == ffi::OCERZ_RSP
                    || (*m).index == ffi::OCERZ_RSP)
            {
                return 0;
            }
            if jit_internal::pin_slot((*d).reg) < 0 {
                return 0;
            }
            return ((*m).disp >= -4095 && (*m).disp <= 4095
                || (has_idx && (*m).disp == 0)) as c_int;
        }
        if (*in_).op == ffi::OCERZ_OP_MOV {
            let d = ptr::addr_of!((*in_).ops[0]);
            let s = ptr::addr_of!((*in_).ops[1]);
            if (*d).kind != ffi::OCERZ_OPK_REG
                || (*d).high8 != 0
                || ((*d).size != 4 && (*d).size != 8)
                || jit_internal::pin_slot((*d).reg) < 0
                || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
            {
                return 0;
            }
            if (*s).kind == ffi::OCERZ_OPK_REG {
                return ((!((*s).high8 != 0)
                    && (*s).size == (*d).size
                    && jit_internal::pin_slot((*s).reg) >= 0
                    && !(jit_internal::rsp_is_ptr() != 0 && (*s).reg == ffi::OCERZ_RSP)))
                    as c_int;
            }
            return ((*s).kind == ffi::OCERZ_OPK_IMM) as c_int;
        }
        0
    }
}

unsafe fn nzcv_gap_shape(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        if nzcv_gap_max() == 0 {
            return 0;
        }
        if (*in_).op == ffi::OCERZ_OP_CMOVCC {
            let d = ptr::addr_of!((*in_).ops[0]);
            let sr = ptr::addr_of!((*in_).ops[1]);
            return ((*d).kind == ffi::OCERZ_OPK_REG
                && (*d).high8 == 0
                && ((*d).size == 4 || (*d).size == 8)
                && (*sr).kind == ffi::OCERZ_OPK_REG
                && (*sr).high8 == 0
                && (*sr).size == (*d).size
                && jit_internal::pin_slot((*d).reg) >= 0
                && jit_internal::pin_slot((*sr).reg) >= 0
                && ffi::g_pin_class == 3) as c_int;
        }
        if (*in_).op == ffi::OCERZ_OP_SETCC {
            let d = ptr::addr_of!((*in_).ops[0]);
            return ((*d).kind == ffi::OCERZ_OPK_REG
                && (*d).high8 == 0
                && (*d).size == 1
                && ffi::g_pin_class == 3) as c_int;
        }
        flag_neutral_ok(in_)
    }
}

unsafe fn nzcv_gap_ok(insns: *const ffi::X86Insn, m: c_int, k: c_int) -> c_int {
    unsafe {
        let in_ = insns.add(m as usize);
        if (*in_).op == ffi::OCERZ_OP_CMOVCC || (*in_).op == ffi::OCERZ_OP_SETCC {
            if nzcv_gap_shape(in_) == 0 || nzcv_fuse_producer(insns, m) != k {
                return 0;
            }
            let p = insns.add(k as usize);
            let kind = if (*p).op == ffi::OCERZ_OP_CMP || (*p).op == ffi::OCERZ_OP_SUB {
                ffi::OCERZ_CC_SUB
            } else if (*p).op == ffi::OCERZ_OP_ADD {
                ffi::OCERZ_CC_ADD
            } else if (*p).op == ffi::OCERZ_OP_BSF || (*p).op == ffi::OCERZ_OP_BSR {
                ffi::OCERZ_CC_SUB
            } else if (*p).op == ffi::OCERZ_OP_BT
                || (*p).op == ffi::OCERZ_OP_BTS
                || (*p).op == ffi::OCERZ_OP_BTR
                || (*p).op == ffi::OCERZ_OP_BTC
            {
                ffi::NZCV_KIND_BT
            } else {
                ffi::OCERZ_CC_LOGIC
            };
            let dc = nzcv_dc_for(kind, (*in_).cc as c_uint);
            return (dc >= 0 && dc != ffi::A64_AL as c_int && dc != ffi::A64_NV as c_int) as c_int;
        }
        flag_neutral_ok(in_)
    }
}

unsafe fn cc_after_adds(cc: c_uint) -> c_int {
    const T: [c_int; 16] = [
        ffi::A64_VS as c_int,
        ffi::A64_VC as c_int,
        ffi::A64_CS as c_int,
        ffi::A64_CC as c_int,
        ffi::A64_EQ as c_int,
        ffi::A64_NE as c_int,
        -1,
        -1,
        ffi::A64_MI as c_int,
        ffi::A64_PL as c_int,
        -1,
        -1,
        ffi::A64_LT as c_int,
        ffi::A64_GE as c_int,
        ffi::A64_LE as c_int,
        ffi::A64_GT as c_int,
    ];
    if cc < 16 { T[cc as usize] } else { -1 }
}

unsafe fn nzcv_dc_for(kind: c_uint, cc: c_uint) -> c_int {
    if kind == ffi::OCERZ_CC_SUB {
        cc_after_subs(cc)
    } else if kind == ffi::OCERZ_CC_ADD {
        cc_after_adds(cc)
    } else if kind == ffi::NZCV_KIND_BT {
        if cc == ffi::OCERZ_CC_B {
            ffi::A64_NE as c_int
        } else if cc == ffi::OCERZ_CC_AE {
            ffi::A64_EQ as c_int
        } else {
            -1
        }
    } else {
        cc_after_ands(cc)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nzcv_fuse_producer(insns: *const ffi::X86Insn, ci: c_int) -> c_int {
    unsafe {
        if env_on!(c"OCERZ_NO_NZCVFWD")
            || ci < 1
            || ffi::g_defer == 0
            || g_no_regflags != 0
        {
            return -1;
        }
        let c = insns.add(ci as usize);
        if cc_consumer_inline_ok(c) == 0 {
            return -1;
        }
        let cc = if (*c).op == ffi::OCERZ_OP_SETCC || (*c).op == ffi::OCERZ_OP_CMOVCC {
            (*c).cc as c_uint
        } else if (*c).op == ffi::OCERZ_OP_ADC || (*c).op == ffi::OCERZ_OP_SBB {
            ffi::OCERZ_CC_B
        } else if (*c).op == ffi::OCERZ_OP_JCC {
            (*c).cc as c_uint
        } else {
            return -1;
        };
        let mut k = ci - 1;
        while k >= 0
            && ci - 1 - k < ffi::NZCV_GAP_MAX as c_int
            && nzcv_producer_candidate(insns.add(k as usize)) == 0
            && nzcv_gap_shape(insns.add(k as usize)) != 0
        {
            k -= 1;
        }
        if k < 0 || nzcv_producer_candidate(insns.add(k as usize)) == 0 {
            return -1;
        }
        for m in k + 1..ci {
            if nzcv_gap_ok(insns, m, k) == 0 {
                return -1;
            }
            let p = insns.add(k as usize);
            let gap = insns.add(m as usize);
            if ((*c).op == ffi::OCERZ_OP_SETCC || (*c).op == ffi::OCERZ_OP_CMOVCC)
                && ((*gap).op == ffi::OCERZ_OP_SETCC || (*gap).op == ffi::OCERZ_OP_CMOVCC)
                && !env_on!(c"OCERZ_NO_NZCV_SIBLING")
            {
                continue;
            }
            for o in 0..(*p).nops as usize {
                let po = ptr::addr_of!((*p).ops[o]);
                if (*po).kind == ffi::OCERZ_OPK_REG
                    && insn_writes_reg(gap, (*po).reg) != 0
                {
                    return -1;
                }
                if (*po).kind == ffi::OCERZ_OPK_MEM
                    && (((*po).base != ffi::OCERZ_REG_NONE
                        && insn_writes_reg(gap, (*po).base) != 0)
                        || ((*po).index != ffi::OCERZ_REG_NONE
                            && insn_writes_reg(gap, (*po).index) != 0))
                {
                    return -1;
                }
            }
        }
        let p = insns.add(k as usize);
        if (*p).op == ffi::OCERZ_OP_BSF || (*p).op == ffi::OCERZ_OP_BSR {
            if (cc != ffi::OCERZ_CC_E && cc != ffi::OCERZ_CC_NE)
                || (*p).nops != 2
                || (*p).seg != ffi::OCERZ_SEG_NONE
            {
                return -1;
            }
            let bd = ptr::addr_of!((*p).ops[0]);
            let bs = ptr::addr_of!((*p).ops[1]);
            if (*bd).kind != ffi::OCERZ_OPK_REG
                || (*bd).high8 != 0
                || ((*bd).size != 4 && (*bd).size != 8)
                || jit_internal::pin_slot((*bd).reg) < 0
            {
                return -1;
            }
            if (*bs).kind == ffi::OCERZ_OPK_REG {
                if (*bs).high8 != 0
                    || jit_internal::pin_slot((*bs).reg) < 0
                    || (*bs).size != (*bd).size
                {
                    return -1;
                }
            } else if (*bs).kind == ffi::OCERZ_OPK_MEM {
                if mem_plain_ok(p, bs) == 0 || (*bs).size != (*bd).size {
                    return -1;
                }
            } else {
                return -1;
            }
            return if ffi::g_pin_class == 2 { -1 } else { k };
        }
        if (*p).op == ffi::OCERZ_OP_BT
            || (*p).op == ffi::OCERZ_OP_BTS
            || (*p).op == ffi::OCERZ_OP_BTR
            || (*p).op == ffi::OCERZ_OP_BTC
        {
            if (cc != ffi::OCERZ_CC_B && cc != ffi::OCERZ_CC_AE)
                || (*p).nops != 2
                || (*p).seg != ffi::OCERZ_SEG_NONE
                || (*p).addrsize != 8
            {
                return -1;
            }
            let bd = ptr::addr_of!((*p).ops[0]);
            let bo = ptr::addr_of!((*p).ops[1]);
            if (*bd).size != 2 && (*bd).size != 4 && (*bd).size != 8 {
                return -1;
            }
            if (*bd).kind == ffi::OCERZ_OPK_REG {
                if (*bd).high8 != 0
                    || jit_internal::pin_slot((*bd).reg) < 0
                    || (jit_internal::rsp_is_ptr() != 0 && (*bd).reg == ffi::OCERZ_RSP)
                {
                    return -1;
                }
            } else if (*bd).kind != ffi::OCERZ_OPK_MEM || (*p).op != ffi::OCERZ_OP_BT {
                return -1;
            }
            if (*bo).kind == ffi::OCERZ_OPK_REG {
                if (*bo).high8 != 0
                    || jit_internal::pin_slot((*bo).reg) < 0
                    || (jit_internal::rsp_is_ptr() != 0 && (*bo).reg == ffi::OCERZ_RSP)
                {
                    return -1;
                }
            } else if (*bo).kind != ffi::OCERZ_OPK_IMM {
                return -1;
            }
            return k;
        }
        let (kind, valid) = match (*p).op {
            ffi::OCERZ_OP_CMP | ffi::OCERZ_OP_SUB => (ffi::OCERZ_CC_SUB, cc_after_subs(cc) >= 0),
            ffi::OCERZ_OP_ADD => (ffi::OCERZ_CC_ADD, cc_after_adds(cc) >= 0),
            ffi::OCERZ_OP_TEST | ffi::OCERZ_OP_AND | ffi::OCERZ_OP_OR | ffi::OCERZ_OP_XOR => {
                (ffi::OCERZ_CC_LOGIC, cc_after_ands(cc) >= 0)
            }
            _ => return -1,
        };
        let _ = kind;
        if !valid || (*p).nops != 2 || (*p).seg != ffi::OCERZ_SEG_NONE {
            return -1;
        }
        let d = ptr::addr_of!((*p).ops[0]);
        let sr = ptr::addr_of!((*p).ops[1]);
        if (*d).size == 1 || (*d).size == 2 {
            if (*sr).size != (*d).size || (*sr).high8 != 0 {
                return -1;
            }
            if (*d).kind == ffi::OCERZ_OPK_REG {
                if (*d).high8 != 0
                    || jit_internal::pin_slot((*d).reg) < 0
                    || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
                {
                    return -1;
                }
            } else if (*d).kind == ffi::OCERZ_OPK_MEM {
                if (*p).op != ffi::OCERZ_OP_CMP
                    || mem_plain_ok(p, d) == 0
                    || (*sr).kind == ffi::OCERZ_OPK_MEM
                {
                    return -1;
                }
            } else {
                return -1;
            }
            if (*p).op == ffi::OCERZ_OP_TEST {
                return ((*sr).kind == ffi::OCERZ_OPK_IMM
                    && (cc == ffi::OCERZ_CC_E || cc == ffi::OCERZ_CC_NE)) as c_int
                    * k
                    + if (*sr).kind == ffi::OCERZ_OPK_IMM
                        && (cc == ffi::OCERZ_CC_E || cc == ffi::OCERZ_CC_NE)
                    {
                        0
                    } else {
                        -1
                    };
            }
            if (*p).op == ffi::OCERZ_OP_CMP {
                if (*sr).kind == ffi::OCERZ_OPK_REG {
                    if jit_internal::pin_slot((*sr).reg) < 0
                        || (jit_internal::rsp_is_ptr() != 0 && (*sr).reg == ffi::OCERZ_RSP)
                    {
                        return -1;
                    }
                } else if (*sr).kind == ffi::OCERZ_OPK_MEM {
                    if mem_plain_ok(p, sr) == 0 {
                        return -1;
                    }
                } else if (*sr).kind != ffi::OCERZ_OPK_IMM {
                    return -1;
                }
                return if cc == ffi::OCERZ_CC_E
                    || cc == ffi::OCERZ_CC_NE
                    || cc == ffi::OCERZ_CC_B
                    || cc == ffi::OCERZ_CC_AE
                    || cc == ffi::OCERZ_CC_A
                    || cc == ffi::OCERZ_CC_BE
                {
                    k
                } else {
                    -1
                };
            }
            return -1;
        }
        if (*d).kind == ffi::OCERZ_OPK_MEM
            && ((*p).op == ffi::OCERZ_OP_CMP || (*p).op == ffi::OCERZ_OP_TEST)
            && ((*c).op == ffi::OCERZ_OP_SETCC
                || (*c).op == ffi::OCERZ_OP_CMOVCC
                || (*c).op == ffi::OCERZ_OP_ADC
                || (*c).op == ffi::OCERZ_OP_SBB)
            && ((*d).size == 4 || (*d).size == 8)
            && (*sr).size == (*d).size
        {
            if rmw_nzcv_ok(p, d) == 0 {
                return -1;
            }
            if (*sr).kind == ffi::OCERZ_OPK_REG {
                if (*sr).high8 != 0
                    || jit_internal::pin_slot((*sr).reg) < 0
                    || (jit_internal::rsp_is_ptr() != 0 && (*sr).reg == ffi::OCERZ_RSP)
                {
                    return -1;
                }
            } else if (*sr).kind != ffi::OCERZ_OPK_IMM {
                return -1;
            }
            return k;
        }
        if (*d).kind != ffi::OCERZ_OPK_REG
            || (*d).high8 != 0
            || jit_internal::pin_slot((*d).reg) < 0
            || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
            || ((*d).size != 4 && (*d).size != 8)
            || (*sr).size != (*d).size
        {
            return -1;
        }
        if (*sr).kind == ffi::OCERZ_OPK_REG {
            if (*sr).high8 != 0
                || jit_internal::pin_slot((*sr).reg) < 0
                || (jit_internal::rsp_is_ptr() != 0 && (*sr).reg == ffi::OCERZ_RSP)
            {
                return -1;
            }
        } else if (*sr).kind == ffi::OCERZ_OPK_MEM {
            if mem_plain_ok(p, sr) == 0 {
                return -1;
            }
        } else if (*sr).kind != ffi::OCERZ_OPK_IMM {
            return -1;
        }
        k
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cc_predicate_ex(
    b: *mut ffi::A64Buf,
    cc: c_uint,
    want_direct: c_int,
) {
    unsafe {
        ffi::g_cc_direct = -1;
        g_cc_cbz_reg = -1;
        if env_on!(c"OCERZ_CCLOG") {
            let mut text = [0i8; 96];
            if !g_flag_producer.is_null() {
                ffi::ocerz_format_insn(g_flag_producer, text.as_mut_ptr(), text.len());
            }
            ffi::fprintf(
                ffi::stderr,
                c"ocerz: CCPRED cc=%u producer=%s\n".as_ptr(),
                cc,
                if g_flag_producer.is_null() { c"(none)".as_ptr() } else { text.as_ptr() },
            );
        }
        if ffi::g_nzcv_from >= 0
            && !ffi::g_cur_insns.is_null()
            && ffi::g_nzcv_from < ffi::g_cur_insn_idx
            && nzcv_fuse_producer(ffi::g_cur_insns, ffi::g_cur_insn_idx) == ffi::g_nzcv_from
        {
            let c = ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize);
            let want_cc = if (*c).op == ffi::OCERZ_OP_ADC || (*c).op == ffi::OCERZ_OP_SBB {
                ffi::OCERZ_CC_B
            } else {
                (*c).cc as c_uint
            };
            if want_cc == cc {
                let dc = if ffi::g_nzcv_kind == ffi::OCERZ_CC_SUB {
                    cc_after_subs(cc)
                } else if ffi::g_nzcv_kind == ffi::OCERZ_CC_ADD {
                    cc_after_adds(cc)
                } else if ffi::g_nzcv_kind == ffi::NZCV_KIND_BT {
                    if cc == ffi::OCERZ_CC_B { ffi::A64_NE as c_int }
                    else if cc == ffi::OCERZ_CC_AE { ffi::A64_EQ as c_int }
                    else { -1 }
                } else {
                    cc_after_ands(cc)
                };
                if dc == ffi::A64_AL as c_int || dc == ffi::A64_NV as c_int {
                    ffi::a64_mov_imm64(b, ffi::JTF, (dc == ffi::A64_AL as c_int) as u64);
                    ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
                    return;
                }
                if dc >= 0 {
                    if want_direct != 0 {
                        ffi::g_cc_direct = dc;
                        return;
                    }
                    ffi::a64_cset(b, ffi::JTF, dc as c_int);
                    ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
                    return;
                }
            }
        }
        if ffi::g_defer != 0
            && g_no_regflags == 0
            && !ffi::g_cur_insns.is_null()
            && ffi::g_cur_insn_idx >= 0
        {
            let c = ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize);
            if ((*c).op == ffi::OCERZ_OP_JCC
                || (*c).op == ffi::OCERZ_OP_SETCC
                || (*c).op == ffi::OCERZ_OP_CMOVCC)
                && (*c).cc as c_uint == cc
            {
                let pi = value_cond_fuse_producer(ffi::g_cur_insns, ffi::g_cur_insn_idx);
                if pi >= 0 {
                    let d = ptr::addr_of!((*ffi::g_cur_insns.add(pi as usize)).ops[0]);
                    let rd = jit_internal::pin_hreg(jit_internal::pin_slot((*d).reg));
                    if want_direct != 0
                        && g_cc_want_cbz != 0
                        && (cc == ffi::OCERZ_CC_E || cc == ffi::OCERZ_CC_NE)
                    {
                        g_cc_cbz_reg = rd;
                        g_cc_cbz_sf = ((*d).size == 8) as c_int;
                        g_cc_cbz_nz = (cc == ffi::OCERZ_CC_NE) as c_int;
                        ffi::g_cc_direct = if cc == ffi::OCERZ_CC_E {
                            ffi::A64_EQ as c_int
                        } else {
                            ffi::A64_NE as c_int
                        };
                        return;
                    }
                    ffi::a64_subs_imm(b, ((*d).size == 8) as c_int, ffi::A64_ZR, rd, 0);
                    let dc = if cc == ffi::OCERZ_CC_E { ffi::A64_EQ }
                        else if cc == ffi::OCERZ_CC_NE { ffi::A64_NE }
                        else if cc == ffi::OCERZ_CC_S { ffi::A64_MI }
                        else { ffi::A64_PL };
                    if want_direct != 0 {
                        ffi::g_cc_direct = dc as c_int;
                        return;
                    }
                    ffi::a64_cset(b, ffi::JTF, dc);
                    ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
                    return;
                }
            }
        }
        let mut generic = [ptr::null_mut::<u32>(); 8];
        let mut ngen = 0usize;
        let mut done = [ptr::null_mut::<u32>(); 4];
        let mut ndone = 0usize;
        let c_sub = cc_after_subs(cc);
        let c_and = cc_after_ands(cc);
        let mut psize = 0;
        let pkind = producer_record_kind(g_flag_producer, &mut psize);
        let c_add = cc_after_adds(cc);
        let shift_ok = (pkind == ffi::OCERZ_CC_SHL
            || pkind == ffi::OCERZ_CC_SHR
            || pkind == ffi::OCERZ_CC_SAR)
            && (cc == ffi::OCERZ_CC_E || cc == ffi::OCERZ_CC_NE
                || cc == ffi::OCERZ_CC_S || cc == ffi::OCERZ_CC_NS);
        if ffi::g_defer != 0 && shift_ok && !g_flag_producer.is_null() {
            let count = ((*g_flag_producer).ops[1].imm
                & if psize == 8 { 63 } else { 31 }) as c_int;
            let sf = (psize == 8) as c_int;
            ffi::a64_ldr(b, 4, ffi::JT0, 20, ffi::CC_OP_OFF);
            let to_rflags = ffi::a64_label(b);
            ffi::a64_cbz(b, 0, ffi::JT0, 0);
            ffi::a64_ldr(b, 8, ffi::JT1, 20, ffi::CC_SRC_OFF);
            if pkind == ffi::OCERZ_CC_SHL {
                ffi::a64_lsl_imm(b, sf, ffi::JT1, ffi::JT1, count);
            } else if pkind == ffi::OCERZ_CC_SHR {
                ffi::a64_lsr_imm(b, sf, ffi::JT1, ffi::JT1, count);
            } else {
                ffi::a64_asr_imm(b, sf, ffi::JT1, ffi::JT1, count);
            }
            ffi::a64_ands_reg(b, sf, ffi::A64_ZR, ffi::JT1, ffi::JT1, 0);
            let dc = if cc == ffi::OCERZ_CC_E { ffi::A64_EQ }
                else if cc == ffi::OCERZ_CC_NE { ffi::A64_NE }
                else if cc == ffi::OCERZ_CC_S { ffi::A64_MI }
                else { ffi::A64_PL };
            ffi::a64_cset(b, ffi::JTF, dc);
            let ready = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_cbz(to_rflags, ffi::a64_label(b));
            emit_cc_predicate_rflags(b, cc);
            ffi::a64_patch_b(ready, ffi::a64_label(b));
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        if ffi::g_defer != 0 && pkind == ffi::OCERZ_CC_ADD && c_add >= 0
            && (psize == 4 || psize == 8)
        {
            ffi::a64_ldr(b, 4, ffi::JT0, 20, ffi::CC_OP_OFF);
            let to_rflags = ffi::a64_label(b);
            ffi::a64_cbz(b, 0, ffi::JT0, 0);
            ffi::a64_ldr(b, 8, ffi::JT1, 20, ffi::CC_SRC_OFF);
            ffi::a64_ldr(b, 8, ffi::JTA, 20, ffi::CC_DST_OFF);
            ffi::a64_adds_reg(b, (psize == 8) as c_int, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0);
            ffi::a64_cset(b, ffi::JTF, c_add as c_int);
            let ready = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_cbz(to_rflags, ffi::a64_label(b));
            emit_cc_predicate_rflags(b, cc);
            ffi::a64_patch_b(ready, ffi::a64_label(b));
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        let cur_ok = !ffi::g_cur_insns.is_null()
            && ffi::g_cur_insn_idx >= 0
            && ( (*ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize)).op == ffi::OCERZ_OP_JCC
                || (*ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize)).op == ffi::OCERZ_OP_SETCC
                || (*ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize)).op == ffi::OCERZ_OP_CMOVCC)
            && (*ffi::g_cur_insns.add(ffi::g_cur_insn_idx as usize)).cc as c_uint == cc;
        if cur_ok && ffi::sse_enabled() != 0
            && comis_fuse_producer(ffi::g_cur_insns, ffi::g_cur_insn_idx) >= 0
        {
            let pi = comis_fuse_producer(ffi::g_cur_insns, ffi::g_cur_insn_idx);
            g_flag_producer = ffi::g_cur_insns.add(pi as usize);
            let dbl = ((*g_flag_producer).op == ffi::OCERZ_OP_UCOMISD
                || (*g_flag_producer).op == ffi::OCERZ_OP_COMISD) as c_int;
            let va = jit_internal::l0_src2(b, (*g_flag_producer).ops[0].reg, dbl);
            let vb = if (*g_flag_producer).ops[1].kind == ffi::OCERZ_OPK_MEM {
                ffi::a64_ldr_v(b, if dbl != 0 { 8 } else { 4 }, 3, 20, ffi::FCMP_MEM_OFF);
                3
            } else {
                jit_internal::l0_src2(b, (*g_flag_producer).ops[1].reg, dbl)
            };
            ffi::a64_fcmp(b, dbl, va, vb);
            let pidx = g_flag_producer.offset_from(ffi::g_cur_insns) as c_int;
            if jit_internal::fpb_det_here(pidx) != 0 {
                jit_internal::fpb_site_emit(b, pidx, va, vb, dbl);
            }
            if want_direct != 0 {
                let dc = if cc == ffi::OCERZ_CC_A { ffi::A64_GT as c_int }
                    else if cc == ffi::OCERZ_CC_AE { ffi::A64_GE as c_int }
                    else if cc == ffi::OCERZ_CC_B { ffi::A64_LT as c_int }
                    else if cc == ffi::OCERZ_CC_BE { ffi::A64_LE as c_int }
                    else if cc == ffi::OCERZ_CC_P { ffi::A64_VS as c_int }
                    else if cc == ffi::OCERZ_CC_NP { ffi::A64_VC as c_int }
                    else { -1 };
                if dc >= 0 {
                    ffi::g_cc_direct = dc;
                    return;
                }
            }
            if cc == ffi::OCERZ_CC_A { ffi::a64_cset(b, ffi::JTF, ffi::A64_GT); }
            else if cc == ffi::OCERZ_CC_AE { ffi::a64_cset(b, ffi::JTF, ffi::A64_GE); }
            else if cc == ffi::OCERZ_CC_B { ffi::a64_cset(b, ffi::JTF, ffi::A64_LT); }
            else if cc == ffi::OCERZ_CC_BE { ffi::a64_cset(b, ffi::JTF, ffi::A64_LE); }
            else if cc == ffi::OCERZ_CC_P { ffi::a64_cset(b, ffi::JTF, ffi::A64_VS); }
            else if cc == ffi::OCERZ_CC_NP { ffi::a64_cset(b, ffi::JTF, ffi::A64_VC); }
            else if cc == ffi::OCERZ_CC_E {
                ffi::a64_cset(b, ffi::JTF, ffi::A64_EQ);
                ffi::a64_cset(b, ffi::JTU, ffi::A64_VS);
                ffi::a64_orr_reg(b, 1, ffi::JTF, ffi::JTF, ffi::JTU, 0);
            } else {
                ffi::a64_cset(b, ffi::JTF, ffi::A64_NE);
                ffi::a64_cset(b, ffi::JTU, ffi::A64_VC);
                ffi::a64_and_reg(b, 1, ffi::JTF, ffi::JTF, ffi::JTU, 0);
            }
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        if !g_flag_producer.is_null()
            && ((*g_flag_producer).op == ffi::OCERZ_OP_UCOMISD
                || (*g_flag_producer).op == ffi::OCERZ_OP_UCOMISS
                || (*g_flag_producer).op == ffi::OCERZ_OP_COMISD
                || (*g_flag_producer).op == ffi::OCERZ_OP_COMISS)
        {
            emit_cc_predicate_rflags(b, cc);
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        if ffi::g_defer != 0 && pkind != 0
            && cc != ffi::OCERZ_CC_P && cc != ffi::OCERZ_CC_NP
            && (psize == 1 || psize == 2 || psize == 4 || psize == 8)
            && ((pkind == ffi::OCERZ_CC_SUB && c_sub >= 0)
                || (pkind == ffi::OCERZ_CC_LOGIC && c_and >= 0))
        {
            ffi::a64_ldr(b, 4, ffi::JT0, 20, ffi::CC_OP_OFF);
            let to_rf = ffi::a64_label(b);
            ffi::a64_cbz(b, 0, ffi::JT0, 0);
            ffi::a64_ldr(b, 8, ffi::JT1, 20, ffi::CC_SRC_OFF);
            ffi::a64_ldr(b, 8, ffi::JTA, 20, ffi::CC_DST_OFF);
            let sh = if psize < 4 { 32 - 8 * psize } else { 0 };
            if pkind == ffi::OCERZ_CC_SUB {
                if psize == 8 { ffi::a64_subs_reg(b, 1, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0); }
                else if psize == 4 { ffi::a64_subs_reg(b, 0, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0); }
                else {
                    ffi::a64_lsl_imm(b, 0, ffi::JT1, ffi::JT1, sh);
                    ffi::a64_lsl_imm(b, 0, ffi::JTA, ffi::JTA, sh);
                    ffi::a64_subs_reg(b, 0, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0);
                }
                ffi::a64_cset(b, ffi::JTF, c_sub as c_int);
            } else {
                if psize == 8 { ffi::a64_ands_reg(b, 1, ffi::A64_ZR, ffi::JTA, ffi::JTA, 0); }
                else if psize == 4 { ffi::a64_ands_reg(b, 0, ffi::A64_ZR, ffi::JTA, ffi::JTA, 0); }
                else {
                    ffi::a64_lsl_imm(b, 0, ffi::JTA, ffi::JTA, sh);
                    ffi::a64_ands_reg(b, 0, ffi::A64_ZR, ffi::JTA, ffi::JTA, 0);
                }
                if c_and == ffi::A64_AL as c_int { ffi::a64_mov_imm64(b, ffi::JTF, 1); }
                else if c_and == ffi::A64_NV as c_int { ffi::a64_mov_imm64(b, ffi::JTF, 0); }
                else { ffi::a64_cset(b, ffi::JTF, c_and as c_int); }
            }
            let ready = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_cbz(to_rf, ffi::a64_label(b));
            emit_cc_predicate_rflags(b, cc);
            ffi::a64_patch_b(ready, ffi::a64_label(b));
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        if ffi::g_defer != 0 && c_sub >= 0 && c_and >= 0
            && cc != ffi::OCERZ_CC_P && cc != ffi::OCERZ_CC_NP
        {
            ffi::a64_ldr(b, 4, ffi::JT0, 20, ffi::CC_OP_OFF);
            generic[ngen] = ffi::a64_label(b);
            ngen += 1;
            ffi::a64_cbz(b, 0, ffi::JT0, 0);
            ffi::a64_ldr(b, 8, ffi::JT1, 20, ffi::CC_SRC_OFF);
            ffi::a64_ldr(b, 8, ffi::JTA, 20, ffi::CC_DST_OFF);
            ffi::a64_ubfx(b, 0, ffi::JTT, ffi::JT0, 8, 8);
            ffi::a64_ubfx(b, 0, ffi::JTU, ffi::JT0, 0, 8);
            ffi::a64_ubfx(b, 0, ffi::JTF, ffi::JT0, 16, 1);
            generic[ngen] = ffi::a64_label(b);
            ngen += 1;
            ffi::a64_cbnz(b, 0, ffi::JTF, 0);
            ffi::a64_subs_imm(b, 0, ffi::A64_ZR, ffi::JTU, ffi::OCERZ_CC_SUB);
            let not_sub = ffi::a64_label(b);
            ffi::a64_bcond(b, ffi::A64_NE, 0);
            ffi::a64_subs_imm(b, 0, ffi::A64_ZR, ffi::JTT, 8);
            let size8 = ffi::a64_label(b);
            ffi::a64_bcond(b, ffi::A64_EQ, 0);
            ffi::a64_subs_imm(b, 0, ffi::A64_ZR, ffi::JTT, 4);
            let size4 = ffi::a64_label(b);
            ffi::a64_bcond(b, ffi::A64_EQ, 0);
            ffi::a64_mov_imm64(b, ffi::JTF, 32);
            ffi::a64_lsl_imm(b, 0, ffi::JTT, ffi::JTT, 3);
            ffi::a64_sub_reg(b, 0, ffi::JTF, ffi::JTF, ffi::JTT, 0);
            ffi::a64_lslv(b, 0, ffi::JT1, ffi::JT1, ffi::JTF);
            ffi::a64_lslv(b, 0, ffi::JTA, ffi::JTA, ffi::JTF);
            ffi::a64_subs_reg(b, 0, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0);
            done[ndone] = ffi::a64_label(b);
            ndone += 1;
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(size4, ffi::a64_label(b));
            ffi::a64_subs_reg(b, 0, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0);
            done[ndone] = ffi::a64_label(b);
            ndone += 1;
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(size8, ffi::a64_label(b));
            ffi::a64_subs_reg(b, 1, ffi::A64_ZR, ffi::JT1, ffi::JTA, 0);
            done[ndone] = ffi::a64_label(b);
            ndone += 1;
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(not_sub, ffi::a64_label(b));
            ffi::a64_subs_imm(b, 0, ffi::A64_ZR, ffi::JTU, ffi::OCERZ_CC_LOGIC);
            generic[ngen] = ffi::a64_label(b);
            ngen += 1;
            ffi::a64_bcond(b, ffi::A64_NE, 0);
            ffi::a64_subs_imm(b, 0, ffi::A64_ZR, ffi::JTT, 8);
            let logic8 = ffi::a64_label(b);
            ffi::a64_bcond(b, ffi::A64_EQ, 0);
            ffi::a64_mov_imm64(b, ffi::JTF, 32);
            ffi::a64_lsl_imm(b, 0, ffi::JTT, ffi::JTT, 3);
            ffi::a64_sub_reg(b, 0, ffi::JTF, ffi::JTF, ffi::JTT, 0);
            ffi::a64_lslv(b, 0, ffi::JTA, ffi::JTA, ffi::JTF);
            ffi::a64_ands_reg(b, 0, ffi::A64_ZR, ffi::JTA, ffi::JTA, 0);
            if c_and == ffi::A64_AL as c_int {
                ffi::a64_mov_imm64(b, ffi::JTF, 1);
            } else if c_and == ffi::A64_NV as c_int {
                ffi::a64_mov_imm64(b, ffi::JTF, 0);
            } else {
                ffi::a64_cset(b, ffi::JTF, c_and as c_int);
            }
            let lg_done = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(logic8, ffi::a64_label(b));
            ffi::a64_ands_reg(b, 1, ffi::A64_ZR, ffi::JTA, ffi::JTA, 0);
            if c_and == ffi::A64_AL as c_int {
                ffi::a64_mov_imm64(b, ffi::JTF, 1);
            } else if c_and == ffi::A64_NV as c_int {
                ffi::a64_mov_imm64(b, ffi::JTF, 0);
            } else {
                ffi::a64_cset(b, ffi::JTF, c_and as c_int);
            }
            ffi::a64_patch_b(lg_done, ffi::a64_label(b));
            let lg_pred = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            for i in 0..ndone {
                ffi::a64_patch_b(done[i], ffi::a64_label(b));
            }
            ffi::a64_cset(b, ffi::JTF, c_sub as c_int);
            let sub_pred = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            for i in 0..ngen {
                let word = *generic[i];
                if word & 0xff00_0010 == 0x5400_0000 {
                    ffi::a64_patch_bcond(generic[i], ffi::a64_label(b));
                } else {
                    ffi::a64_patch_cbz(generic[i], ffi::a64_label(b));
                }
            }
            emit_materialize(b);
            emit_cc_predicate_rflags(b, cc);
            ffi::a64_patch_b(lg_pred, ffi::a64_label(b));
            ffi::a64_patch_b(sub_pred, ffi::a64_label(b));
            ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
            return;
        }
        emit_materialize(b);
        emit_cc_predicate_rflags(b, cc);
        ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JTF, 0);
    }
}

unsafe fn fused_jcc_cond(producer: *const ffi::X86Insn, jcc: *const ffi::X86Insn) -> c_int {
    unsafe {
        if (*producer).op == ffi::OCERZ_OP_TEST {
            const TEST: [c_int; 16] = [
                -1, -1, -1, -1, ffi::A64_EQ as c_int, ffi::A64_NE as c_int,
                ffi::A64_EQ as c_int, ffi::A64_NE as c_int, ffi::A64_MI as c_int,
                ffi::A64_PL as c_int, -1, -1, ffi::A64_LT as c_int,
                ffi::A64_GE as c_int, ffi::A64_LE as c_int, ffi::A64_GT as c_int,
            ];
            if ((*jcc).cc as c_uint != ffi::OCERZ_CC_E && (*jcc).cc as c_uint != ffi::OCERZ_CC_NE)
                && env_on!(c"OCERZ_NO_TEST_CC")
            {
                return -1;
            }
            return if ((*jcc).cc as c_uint) < 16 { TEST[(*jcc).cc as usize] } else { -1 };
        }
        const CMP: [c_int; 16] = [
            ffi::A64_VS as c_int, ffi::A64_VC as c_int, ffi::A64_CC as c_int,
            ffi::A64_CS as c_int, ffi::A64_EQ as c_int, ffi::A64_NE as c_int,
            ffi::A64_LS as c_int, ffi::A64_HI as c_int, ffi::A64_MI as c_int,
            ffi::A64_PL as c_int, -1, -1, ffi::A64_LT as c_int,
            ffi::A64_GE as c_int, ffi::A64_LE as c_int, ffi::A64_GT as c_int,
        ];
        if ((*jcc).cc as c_uint) < 16 { CMP[(*jcc).cc as usize] } else { -1 }
    }
}

unsafe fn cond_short_site(to_taken: *mut u32, taken_rec: c_int, body_edge: c_int) -> *mut u32 {
    unsafe {
        if taken_rec != 0 || body_edge == 0 || ffi::g_l0_dirty != 0 {
            ptr::null_mut()
        } else {
            to_taken
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn can_fuse_cmp_test_jcc(
    producer: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    block_rip: u64,
) -> c_int {
    unsafe {
        if g_no_jccfuse != 0
            || g_no_regflags != 0
            || ffi::g_no_chain != 0
            || (*jcc).op != ffi::OCERZ_OP_JCC
            || (*jcc).ops[0].kind != ffi::OCERZ_OPK_IMM
            || ((*jcc).ops[0].imm != block_rip && g_no_jcclink != 0)
            || fused_jcc_cond(producer, jcc) < 0
        {
            return 0;
        }
        if (*jcc).ops[0].imm == block_rip
            && (g_no_xlive != 0
                || jit_internal::xlive_succ_live(g_xlat_jit, block_rip) != 0)
        {
            return 0;
        }
        if (*producer).op != ffi::OCERZ_OP_CMP && (*producer).op != ffi::OCERZ_OP_TEST {
            return 0;
        }
        let d = ptr::addr_of!((*producer).ops[0]);
        let s = ptr::addr_of!((*producer).ops[1]);
        if (*s).size != (*d).size
            || ((*d).size != 4 && (*d).size != 8 && (*d).size != 1 && (*d).size != 2)
        {
            return 0;
        }
        let d_mem = (*d).kind == ffi::OCERZ_OPK_MEM;
        let s_mem = (*s).kind == ffi::OCERZ_OPK_MEM;
        if d_mem && s_mem || ((d_mem || s_mem) && (*producer).seg != ffi::OCERZ_SEG_NONE) {
            return 0;
        }
        if !d_mem && ((*d).kind != ffi::OCERZ_OPK_REG || (*d).high8 != 0) {
            return 0;
        }
        if (*s).kind == ffi::OCERZ_OPK_REG {
            (!((*s).high8 != 0)) as c_int
        } else {
            ((*s).kind == ffi::OCERZ_OPK_IMM || s_mem) as c_int
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn side_gap_fuse_ok(insns: *const ffi::X86Insn, i: c_int, n: c_int) -> c_int {
    (unsafe {
        if g_xlat_mode32 != 0 || env_on!(c"OCERZ_NO_SIDEFUSE")
            || i < 0 || i + 2 >= n - 1
        {
            return 0;
        }
        let p = insns.add(i as usize);
        let j = insns.add((i + 2) as usize);
        if (*j).op != ffi::OCERZ_OP_JCC
            || ((*p).op != ffi::OCERZ_OP_CMP && (*p).op != ffi::OCERZ_OP_TEST)
            || ffi::g_defer == 0
            || g_no_jccfuse != 0
            || (*j).ops[0].kind != ffi::OCERZ_OPK_IMM
            || (*j).ops[0].imm == g_self_rip
            || can_fuse_cmp_test_jcc(p, j, g_self_rip) == 0
            || (*p).addrsize != 8
            || jcc_gap_ok(insns.add((i + 1) as usize)) == 0
        {
            return 0;
        }
        ((*p).ops[0].kind != ffi::OCERZ_OPK_REG
            || insn_writes_reg(insns.add((i + 1) as usize), (*p).ops[0].reg) == 0)
            && ((*p).ops[1].kind != ffi::OCERZ_OPK_REG
                || insn_writes_reg(insns.add((i + 1) as usize), (*p).ops[1].reg) == 0)
    }) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn side_fuse_ok(insns: *const ffi::X86Insn, i: c_int, n: c_int) -> c_int {
    unsafe {
        if g_xlat_mode32 != 0 || env_on!(c"OCERZ_NO_SIDEFUSE") || i + 1 >= n - 1 {
            return 0;
        }
        let p = insns.add(i as usize);
        let j = insns.add((i + 1) as usize);
        if (*j).op != ffi::OCERZ_OP_JCC
            || ((*p).op != ffi::OCERZ_OP_CMP && (*p).op != ffi::OCERZ_OP_TEST)
            || ffi::g_defer == 0
            || (*j).ops[0].kind != ffi::OCERZ_OPK_IMM
            || (*j).ops[0].imm == g_self_rip
            || can_fuse_cmp_test_jcc(p, j, g_self_rip) == 0
            || (*p).addrsize != 8
        {
            return 0;
        }
        1
    }
}

unsafe fn insn_writes_reg<R: Into<c_uint>>(in_: *const ffi::X86Insn, reg: R) -> c_int {
    unsafe {
        if (*in_).nops == 0 {
            return 0;
        }
        let d = ptr::addr_of!((*in_).ops[0]);
        ((*d).kind == ffi::OCERZ_OPK_REG && (((*d).reg as c_uint) & 15) == (reg.into() & 15))
            as c_int
    }
}

unsafe fn stack_gap_load_ok(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        if (*in_).op != ffi::OCERZ_OP_MOV || (*in_).nops != 2
            || env_on!(c"OCERZ_NO_STACK_GAP")
        {
            return 0;
        }
        let d = ptr::addr_of!((*in_).ops[0]);
        let s = ptr::addr_of!((*in_).ops[1]);
        if (*d).kind != ffi::OCERZ_OPK_REG || (*d).high8 != 0
            || ((*d).size != 4 && (*d).size != 8)
            || jit_internal::pin_slot((*d).reg) < 0
            || (*d).reg == ffi::OCERZ_RSP
            || (*s).kind != ffi::OCERZ_OPK_MEM || (*s).size != (*d).size
            || jit_internal::mem_plain_access_ok(s) == 0
        {
            return 0;
        }
        ffi::lowstack_disp_ok(in_, s, (*d).size as c_int, 1)
    }
}

unsafe fn jcc_gap_ok(in_: *const ffi::X86Insn) -> c_int {
    unsafe { (flag_neutral_ok(in_) != 0 || stack_gap_load_ok(in_) != 0) as c_int }
}

unsafe fn emit_flag_neutral(b: *mut ffi::A64Buf, in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        match (*in_).op {
            ffi::OCERZ_OP_LEA => ffi::emit_lea(b, in_),
            ffi::OCERZ_OP_MOV => {
                let d = ptr::addr_of!((*in_).ops[0]);
                let s = ptr::addr_of!((*in_).ops[1]);
                if stack_gap_load_ok(in_) != 0 {
                    if jit_internal::lowstack_disp_ea(b, in_, s, (*d).size as c_int, 1) == 0 {
                        return 0;
                    }
                    ffi::emit_gpr_ld_at(
                        b,
                        (*d).size as c_int,
                        jit_internal::pin_hreg(jit_internal::pin_slot((*d).reg)),
                        ffi::JTA,
                        (*s).disp as c_int,
                        1,
                    );
                    return 1;
                }
                if (*d).kind != ffi::OCERZ_OPK_REG || (*d).high8 != 0
                    || ((*d).size != 4 && (*d).size != 8)
                {
                    return 0;
                }
                if (*s).kind == ffi::OCERZ_OPK_REG {
                    if (*s).high8 != 0 || (*s).size != (*d).size {
                        return 0;
                    }
                    let ds = jit_internal::pin_slot((*d).reg);
                    let ss = jit_internal::pin_slot((*s).reg);
                    if ds < 0 || ss < 0
                        || (jit_internal::rsp_is_ptr() != 0
                            && ((*d).reg == ffi::OCERZ_RSP || (*s).reg == ffi::OCERZ_RSP))
                    {
                        return 0;
                    }
                    if ds != ss || (*d).size == 4 {
                        ffi::a64_mov_reg(
                            b,
                            ((*d).size == 8) as c_int,
                            jit_internal::pin_hreg(ds),
                            jit_internal::pin_hreg(ss),
                        );
                    }
                    1
                } else if (*s).kind == ffi::OCERZ_OPK_IMM {
                    let ds = jit_internal::pin_slot((*d).reg);
                    if ds < 0
                        || (jit_internal::rsp_is_ptr() != 0 && (*d).reg == ffi::OCERZ_RSP)
                    {
                        return 0;
                    }
                    let v = if (*d).size == 4 { (*s).imm & 0xffff_ffff } else { (*s).imm };
                    ffi::a64_mov_imm64(b, jit_internal::pin_hreg(ds), v);
                    1
                } else {
                    0
                }
            }
            _ => 0,
        }
    }
}

#[repr(C)]
struct LogicTargetDecode {
    target: MaybeUninit<[ffi::X86Insn; 2]>,
    rip: u64,
    n: c_int,
    pc: u64,
}

unsafe extern "C" fn logic_target_decode_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<LogicTargetDecode>();
        let n = ptr::addr_of_mut!((*s).n);
        let pc = ptr::addr_of_mut!((*s).pc);
        ptr::write_volatile(n, 0);
        ptr::write_volatile(pc, (*s).rip);
        let target = ptr::addr_of_mut!((*s).target).cast::<ffi::X86Insn>();
        while ptr::read_volatile(n) < 2 {
            let i = ptr::read_volatile(n) as usize;
            let at = ptr::read_volatile(pc);
            let out = target.add(i);
            if ffi::jit_decode(at, out, g_xlat_mode32) != ffi::OCERZ_OK {
                break;
            }
            let op = (*out).op;
            let len = (*out).len;
            ptr::write_volatile(n, (i + 1) as c_int);
            ptr::write_volatile(pc, at.wrapping_add(len as u64));
            if ffi::is_terminator(op as c_uint) != 0 {
                break;
            }
        }
    }
}

unsafe fn ifconv_test_bit(
    test: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    bit: *mut c_int,
) -> c_int {
    unsafe {
        if (*test).op != ffi::OCERZ_OP_TEST
            || (*test).lock != 0
            || (*test).nops != 2
            || (*jcc).op != ffi::OCERZ_OP_JCC
            || (*jcc).ops[0].kind != ffi::OCERZ_OPK_IMM
            || ((*jcc).cc as c_uint != ffi::OCERZ_CC_E && (*jcc).cc as c_uint != ffi::OCERZ_CC_NE)
            || (*test).rip.wrapping_add((*test).len as u64) != (*jcc).rip
        {
            return 0;
        }
        let d = ptr::addr_of!((*test).ops[0]);
        let s = ptr::addr_of!((*test).ops[1]);
        if (*d).kind != ffi::OCERZ_OPK_REG
            || (*d).high8 != 0
            || ((*d).size != 4 && (*d).size != 8)
            || (*s).kind != ffi::OCERZ_OPK_IMM
            || (*s).size != (*d).size
        {
            return 0;
        }
        let mask = (*s).imm & if (*d).size == 8 { u64::MAX } else { 0xffff_ffff };
        if mask == 0 || (mask & (mask - 1)) != 0 {
            return 0;
        }
        *bit = mask.trailing_zeros() as c_int;
        1
    }
}

unsafe fn ifconv_direct_latch(
    p: *const ffi::X86Insn,
    loop_rip: u64,
    latch_rip: *mut u64,
    exit_rip: *mut u64,
) -> c_int {
    unsafe {
        let arith = p;
        let latch = p.add(1);
        let jcc = p.add(2);
        let ad = ptr::addr_of!((*arith).ops[0]);
        let ass = ptr::addr_of!((*arith).ops[1]);
        let ld = ptr::addr_of!((*latch).ops[0]);
        if ((*arith).op != ffi::OCERZ_OP_ADD && (*arith).op != ffi::OCERZ_OP_SUB)
            || (*arith).lock != 0
            || (*arith).nops != 2
            || (*ad).kind != ffi::OCERZ_OPK_REG
            || (*ad).high8 != 0
            || ((*ad).size != 4 && (*ad).size != 8)
            || (*ass).kind != ffi::OCERZ_OPK_IMM
            || (*ass).size != (*ad).size
            || ((*latch).op != ffi::OCERZ_OP_INC && (*latch).op != ffi::OCERZ_OP_DEC)
            || (*latch).lock != 0
            || (*latch).nops != 1
            || (*ld).kind != ffi::OCERZ_OPK_REG
            || (*ld).high8 != 0
            || ((*ld).size != 4 && (*ld).size != 8)
            || (*jcc).op != ffi::OCERZ_OP_JCC
            || (*jcc).ops[0].kind != ffi::OCERZ_OPK_IMM
            || ((*jcc).cc as c_uint != ffi::OCERZ_CC_E && (*jcc).cc as c_uint != ffi::OCERZ_CC_NE)
            || (*arith).rip.wrapping_add((*arith).len as u64) != (*latch).rip
            || (*latch).rip.wrapping_add((*latch).len as u64) != (*jcc).rip
        {
            return 0;
        }
        let taken = (*jcc).ops[0].imm;
        let fall = (*jcc).rip.wrapping_add((*jcc).len as u64);
        let taken_self = taken == loop_rip;
        let fall_self = fall == loop_rip;
        if taken_self == fall_self {
            return 0;
        }
        *latch_rip = (*latch).rip;
        *exit_rip = if taken_self { fall } else { taken };
        1
    }
}

unsafe fn ifconv_simple_path(p: *const ffi::X86Insn, latch_rip: u64, acc: c_uint, size: c_uint) -> c_int {
    (unsafe {
        let acc = acc as u8;
        let size = size as u8;
        let d = ptr::addr_of!((*p).ops[0]);
        ((*p).op == ffi::OCERZ_OP_INC || (*p).op == ffi::OCERZ_OP_DEC)
            && (*p).lock == 0
            && (*p).nops == 1
            && (*d).kind == ffi::OCERZ_OPK_REG
            && (*d).high8 == 0
            && (*d).reg == acc
            && (*d).size == size
            && (*p.add(1)).op == ffi::OCERZ_OP_JMP
            && (*p.add(1)).ops[0].kind == ffi::OCERZ_OPK_IMM
            && (*p.add(1)).ops[0].imm == latch_rip
    }) as c_int
}

unsafe fn ifconv_complex_path(p: *const ffi::X86Insn, latch_rip: u64, acc: c_uint, size: c_uint) -> c_int {
    unsafe {
        let acc = acc as u8;
        let size = size as u8;
        let md = ptr::addr_of!((*p).ops[0]);
        let ms = ptr::addr_of!((*p).ops[1]);
        let sd = ptr::addr_of!((*p.add(1)).ops[0]);
        let ss = ptr::addr_of!((*p.add(1)).ops[1]);
        let xd = ptr::addr_of!((*p.add(2)).ops[0]);
        let xs = ptr::addr_of!((*p.add(2)).ops[1]);
        ((*p).op == ffi::OCERZ_OP_MOV
            && (*p).lock == 0
            && (*p).nops == 2
            && (*md).kind == ffi::OCERZ_OPK_REG
            && (*ms).kind == ffi::OCERZ_OPK_REG
            && (*md).high8 == 0
            && (*ms).high8 == 0
            && (*md).reg != acc
            && (*md).size == size
            && (*ms).size == size
            && (*p.add(1)).op == ffi::OCERZ_OP_SHR
            && (*p.add(1)).lock == 0
            && (*p.add(1)).nops == 2
            && (*sd).kind == ffi::OCERZ_OPK_REG
            && (*sd).high8 == 0
            && (*sd).reg == (*md).reg
            && (*sd).size == size
            && (*ss).kind == ffi::OCERZ_OPK_IMM
            && (*p.add(2)).op == ffi::OCERZ_OP_XOR
            && (*p.add(2)).lock == 0
            && (*p.add(2)).nops == 2
            && (*xd).kind == ffi::OCERZ_OPK_REG
            && (*xs).kind == ffi::OCERZ_OPK_REG
            && (*xd).high8 == 0
            && (*xs).high8 == 0
            && (*xd).reg == acc
            && (*xd).size == size
            && (*xs).reg == (*md).reg
            && (*xs).size == size
            && (*p.add(3)).op == ffi::OCERZ_OP_JMP
            && (*p.add(3)).ops[0].kind == ffi::OCERZ_OPK_IMM
            && (*p.add(3)).ops[0].imm == latch_rip) as c_int
    }
}

unsafe fn match_ifconv_diamond(
    test: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    m: *mut ffi::IfConvDiamond,
) -> c_int {
    unsafe {
        ptr::write_bytes(m.cast::<u8>(), 0, core::mem::size_of::<ffi::IfConvDiamond>());
        if ffi::g_defer == 0
            || g_no_jccfuse != 0
            || g_no_regflags != 0
            || ffi::g_no_chain != 0
            || g_no_jcclink != 0
            || ffi::g_pin_class != 1
            || g_loop_entry.is_null()
            || ifconv_test_bit(test, jcc, ptr::addr_of_mut!((*m).first_bit)) == 0
        {
            return 0;
        }
        let first_succ = [
            (*jcc).rip.wrapping_add((*jcc).len as u64),
            (*jcc).ops[0].imm,
        ];
        for direct_taken in 0..=1 {
            let direct_rip = first_succ[direct_taken];
            let nested_rip = first_succ[1 - direct_taken];
            let mut latch_rip = 0u64;
            let mut exit_rip = 0u64;
            if decode_ifconv_block(direct_rip, ptr::addr_of_mut!((*m).direct[0]), 3) != 3
                || ifconv_direct_latch(
                    ptr::addr_of!((*m).direct[0]),
                    g_self_rip,
                    &mut latch_rip,
                    &mut exit_rip,
                ) == 0
                || decode_ifconv_block(nested_rip, ptr::addr_of_mut!((*m).nested[0]), 2) != 2
                || ifconv_test_bit(
                    ptr::addr_of!((*m).nested[0]),
                    ptr::addr_of!((*m).nested[1]),
                    ptr::addr_of_mut!((*m).nested_bit),
                ) == 0
            {
                continue;
            }
            let acc = ptr::addr_of!((*m).direct[0].ops[0]);
            let nested_succ = [
                (*m).nested[1]
                    .rip
                    .wrapping_add((*m).nested[1].len as u64),
                (*m).nested[1].ops[0].imm,
            ];
            for simple_taken in 0..=1 {
                let simple_rip = nested_succ[simple_taken];
                let complex_rip = nested_succ[1 - simple_taken];
                if decode_ifconv_block(simple_rip, ptr::addr_of_mut!((*m).simple[0]), 2) != 2
                    || ifconv_simple_path(
                        ptr::addr_of!((*m).simple[0]),
                        latch_rip,
                        (*acc).reg as c_uint,
                        (*acc).size as c_uint,
                    ) == 0
                    || decode_ifconv_block(complex_rip, ptr::addr_of_mut!((*m).complex[0]), 4) != 4
                    || ifconv_complex_path(
                        ptr::addr_of!((*m).complex[0]),
                        latch_rip,
                        (*acc).reg as c_uint,
                        (*acc).size as c_uint,
                    ) == 0
                {
                    continue;
                }
                let tmp = ptr::addr_of!((*m).complex[0].ops[0]);
                let src = ptr::addr_of!((*m).complex[0].ops[1]);
                let latch = ptr::addr_of!((*m).direct[1].ops[0]);
                let nested_test = ptr::addr_of!((*m).nested[0].ops[0]);
                if jit_internal::pin_slot((*test).ops[0].reg) < 0
                    || jit_internal::pin_slot((*nested_test).reg) < 0
                    || jit_internal::pin_slot((*acc).reg) < 0
                    || jit_internal::pin_slot((*tmp).reg) < 0
                    || jit_internal::pin_slot((*src).reg) < 0
                    || jit_internal::pin_slot((*latch).reg) < 0
                {
                    continue;
                }
                (*m).direct_is_taken = direct_taken as c_int;
                (*m).simple_is_taken = simple_taken as c_int;
                (*m).exit_rip = exit_rip;
                return 1;
            }
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_ifconv_diamond(
    b: *mut ffi::A64Buf,
    test: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    jcc_label: *mut *mut u32,
) -> c_int {
    unsafe {
        if g_xlat_mode32 != 0 {
            return 0;
        }
        let mut m = MaybeUninit::<ffi::IfConvDiamond>::uninit();
        if match_ifconv_diamond(test, jcc, m.as_mut_ptr()) == 0 {
            return 0;
        }
        let m = &*m.as_ptr();
        let arith = ptr::addr_of!(m.direct[0]);
        let latch = ptr::addr_of!(m.direct[1]);
        let latch_jcc = ptr::addr_of!(m.direct[2]);
        let simple = ptr::addr_of!(m.simple[0]);
        let acc = ptr::addr_of!((*arith).ops[0]);
        let tmp = ptr::addr_of!(m.complex[0].ops[0]);
        let src = ptr::addr_of!(m.complex[0].ops[1]);
        let ld = ptr::addr_of!((*latch).ops[0]);
        let sf = ((*acc).size == 8) as c_int;
        let acc_hr = jit_internal::pin_hreg(jit_internal::pin_slot((*acc).reg));
        let tmp_hr = jit_internal::pin_hreg(jit_internal::pin_slot((*tmp).reg));
        let src_hr = jit_internal::pin_hreg(jit_internal::pin_slot((*src).reg));
        let latch_hr = jit_internal::pin_hreg(jit_internal::pin_slot((*ld).reg));
        ffi::a64_ubfx(
            b,
            1,
            ffi::JT0,
            jit_internal::pin_hreg(jit_internal::pin_slot((*test).ops[0].reg)),
            m.first_bit,
            1,
        );
        *jcc_label = ffi::a64_label(b);
        ffi::a64_ubfx(
            b,
            1,
            ffi::JT1,
            jit_internal::pin_hreg(jit_internal::pin_slot(m.nested[0].ops[0].reg)),
            m.nested_bit,
            1,
        );
        let imm = (*arith).ops[1].imm & if sf != 0 { u64::MAX } else { 0xffff_ffff };
        if imm <= 4095 {
            if (*arith).op == ffi::OCERZ_OP_ADD {
                ffi::a64_adds_imm(b, sf, ffi::JT2, acc_hr, imm as c_uint);
            } else {
                ffi::a64_subs_imm(b, sf, ffi::JT2, acc_hr, imm as c_uint);
            }
        } else {
            ffi::a64_mov_imm64(b, ffi::JTT, imm);
            if (*arith).op == ffi::OCERZ_OP_ADD {
                ffi::a64_adds_reg(b, sf, ffi::JT2, acc_hr, ffi::JTT, 0);
            } else {
                ffi::a64_subs_reg(b, sf, ffi::JT2, acc_hr, ffi::JTT, 0);
            }
        }
        ffi::a64_cset(
            b,
            ffi::JTF,
            if (*arith).op == ffi::OCERZ_OP_ADD { ffi::A64_CS } else { ffi::A64_CC },
        );
        if (*simple).op == ffi::OCERZ_OP_INC {
            ffi::a64_add_imm(b, sf, ffi::JTT, acc_hr, 1);
        } else {
            ffi::a64_sub_imm(b, sf, ffi::JTT, acc_hr, 1);
        }
        let shift = (m.complex[1].ops[1].imm & if sf != 0 { 63 } else { 31 }) as c_int;
        ffi::a64_lsr_imm(b, sf, ffi::JTU, src_hr, shift);
        ffi::a64_eor_reg(b, sf, ffi::JTA, acc_hr, ffi::JTU, 0);
        let simple_nonzero = (m.nested[1].cc as c_uint == ffi::OCERZ_CC_NE) == (m.simple_is_taken != 0);
        let simple_cond = if simple_nonzero { ffi::A64_NE } else { ffi::A64_EQ };
        ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JT1, 0);
        ffi::a64_csel(b, sf, ffi::JTA, ffi::JTT, ffi::JTA, simple_cond);
        ffi::a64_csel(b, sf, ffi::JTU, tmp_hr, ffi::JTU, simple_cond);
        let direct_nonzero = ((*jcc).cc as c_uint == ffi::OCERZ_CC_NE) == (m.direct_is_taken != 0);
        let direct_cond = if direct_nonzero { ffi::A64_NE } else { ffi::A64_EQ };
        ffi::a64_subs_imm(b, 1, ffi::A64_ZR, ffi::JT0, 0);
        ffi::a64_csel(b, sf, acc_hr, ffi::JT2, ffi::JTA, direct_cond);
        ffi::a64_csel(b, sf, tmp_hr, tmp_hr, ffi::JTU, direct_cond);
        ffi::a64_csel(b, 1, ffi::JT0, ffi::JTF, ffi::A64_ZR, direct_cond);
        let lsf = ((*ld).size == 8) as c_int;
        if (*latch).op == ffi::OCERZ_OP_INC {
            ffi::a64_adds_imm(b, lsf, latch_hr, latch_hr, 1);
        } else {
            ffi::a64_subs_imm(b, lsf, latch_hr, latch_hr, 1);
        }
        let taken = (*latch_jcc).ops[0].imm;
        let cond = if (*latch_jcc).cc as c_uint == ffi::OCERZ_CC_E {
            ffi::A64_EQ
        } else {
            ffi::A64_NE
        };
        let self_cond = if taken == g_self_rip { cond } else { ffi::A64_INV(cond) };
        jit_internal::l0_fixed_backedge(b);
        g_stop_patch = ffi::a64_label(b);
        ffi::a64_bcond(b, self_cond, (g_loop_entry.offset_from(ffi::a64_label(b))) as c_int);
        jit_internal::l0_fixed_fallthrough(b);
        jit_internal::fpb_emit_exit_check(b);
        let edge_class = jit_internal::body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        let pb = emit_incdec_jcc_arm(
            b,
            latch,
            m.exit_rip,
            (m.exit_rip <= g_self_rip) as c_int,
            body_edge,
            ffi::JT0,
            epilogue_sites,
            n_epi,
            ptr::null_mut(),
        );
        g_jcc_edge[0].target_rip = m.exit_rip;
        g_jcc_edge[0].patch_b = pb;
        g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_n_jcc_edges = 1;
        g_stop_target = ffi::a64_label(b);
        jit_internal::emit_gpr_rd(b, 1, ffi::JT1, (*ld).reg);
        jit_internal::emit_defer_flags(
            b,
            ocerz_cc_pack(
                if (*latch).op == ffi::OCERZ_OP_INC {
                    ffi::OCERZ_CC_INC
                } else {
                    ffi::OCERZ_CC_DEC
                },
                (*ld).size as c_int,
                0,
            ),
            ffi::JT0,
            ffi::JT1,
        );
        ffi::a64_mov_imm64(b, ffi::JT2, g_self_rip);
        ffi::a64_str(b, 8, ffi::JT2, 20, ffi::RIP_OFF);
        emit_materialize(b);
        ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
        *epilogue_sites.add(*n_epi as usize) = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        *n_epi += 1;
        1
    }
}

#[repr(C)]
struct IfconvDecode {
    rip: u64,
    out: *mut ffi::X86Insn,
    cap: c_int,
    n: c_int,
    terminated: c_int,
    pc: u64,
}

unsafe extern "C" fn ifconv_decode_cb(arg: *mut c_void) {
    unsafe {
        let s = arg.cast::<IfconvDecode>();
        let n = ptr::addr_of_mut!((*s).n);
        let terminated = ptr::addr_of_mut!((*s).terminated);
        let pc = ptr::addr_of_mut!((*s).pc);
        ptr::write_volatile(n, 0);
        ptr::write_volatile(terminated, 0);
        ptr::write_volatile(pc, (*s).rip);
        while ptr::read_volatile(n) < (*s).cap {
            let i = ptr::read_volatile(n) as usize;
            let at = ptr::read_volatile(pc);
            let out = (*s).out.add(i);
            if ffi::jit_decode(at, out, g_xlat_mode32) != ffi::OCERZ_OK {
                break;
            }
            let op = (*out).op;
            let len = (*out).len;
            ptr::write_volatile(pc, at.wrapping_add(len as u64));
            ptr::write_volatile(n, (i + 1) as c_int);
            if ffi::is_terminator(op as c_uint) != 0 {
                ptr::write_volatile(terminated, 1);
                break;
            }
        }
    }
}

unsafe fn decode_ifconv_block(rip: u64, out: *mut ffi::X86Insn, cap: c_int) -> c_int {
    unsafe {
        let mut s = IfconvDecode {
            rip,
            out,
            cap,
            n: 0,
            terminated: 0,
            pc: rip,
        };
        if jit_flags_guarded(ifconv_decode_cb, (&mut s as *mut IfconvDecode).cast()) == 0 {
            ptr::write_volatile(ptr::addr_of_mut!(s.n), 0);
            ptr::write_volatile(ptr::addr_of_mut!(s.terminated), 0);
        }
        if ptr::read_volatile(ptr::addr_of!(s.terminated)) != 0 {
            ptr::read_volatile(ptr::addr_of!(s.n))
        } else {
            0
        }
    }
}

unsafe fn emit_incdec_jcc_arm(
    b: *mut ffi::A64Buf,
    producer: *const ffi::X86Insn,
    target: u64,
    poll: c_int,
    body_edge: c_int,
    mut cf_reg: c_int,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    recorded: *mut c_int,
) -> *mut u32 {
    unsafe {
        let live = if g_no_xlive != 0 {
            ffi::OCERZ_FL_ALL as u64
        } else {
            jit_internal::xlive_succ_live(g_xlat_jit, target)
        };
        if !recorded.is_null() {
            *recorded = (live != 0) as c_int;
        }
        if live != 0 {
            let d = ptr::addr_of!((*producer).ops[0]);
            if cf_reg < 0 {
                emit_materialize(b);
                ffi::a64_ldr(b, 8, ffi::JTT, 20, ffi::RF_OFF);
                ffi::a64_ubfx(b, 1, ffi::JT0, ffi::JTT, 0, 1);
                cf_reg = ffi::JT0;
            }
            jit_internal::emit_gpr_rd(b, 1, ffi::JT1, (*d).reg);
            jit_internal::emit_defer_flags(
                b,
                ocerz_cc_pack(
                    if (*producer).op == ffi::OCERZ_OP_INC {
                        ffi::OCERZ_CC_INC
                    } else {
                        ffi::OCERZ_CC_DEC
                    },
                    (*d).size as c_int,
                    0,
                ),
                cf_reg,
                ffi::JT1,
            );
        }
        jit_internal::emit_static_chain_tail(b, target, poll, body_edge, epilogue_sites, n_epi)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_incdec_jcc(
    b: *mut ffi::A64Buf,
    producer: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    jcc_label: *mut *mut u32,
) -> c_int {
    unsafe {
        if jit_internal::can_fuse_incdec_jcc(producer, jcc) == 0 || ffi::g_defer == 0 {
            return 0;
        }
        let d = ptr::addr_of!((*producer).ops[0]);
        let sf = ((*d).size == 8) as c_int;
        let ds = jit_internal::pin_slot((*d).reg);
        let rd = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT2 };
        if ds >= 0 {
            if (*producer).op == ffi::OCERZ_OP_INC {
                ffi::a64_adds_imm(b, sf, rd, rd, 1);
            } else {
                ffi::a64_subs_imm(b, sf, rd, rd, 1);
            }
        } else {
            jit_internal::emit_gpr_rd(b, sf, ffi::JT0, (*d).reg);
            if (*producer).op == ffi::OCERZ_OP_INC {
                ffi::a64_adds_imm(b, sf, ffi::JT2, ffi::JT0, 1);
            } else {
                ffi::a64_subs_imm(b, sf, ffi::JT2, ffi::JT0, 1);
            }
            jit_internal::emit_gpr_wr(b, ffi::JT2, (*d).reg);
        }
        *jcc_label = ffi::a64_label(b);
        let taken_cond = if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { ffi::A64_EQ } else { ffi::A64_NE };
        let taken = (*jcc).ops[0].imm;
        let mut fall = (*jcc).rip.wrapping_add((*jcc).len as u64);
        if (*jcc).mode32 != 0 {
            fall = fall as u32 as u64;
        }
        let edge_class = jit_internal::body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        let to_taken = ffi::a64_label(b);
        ffi::a64_bcond(b, taken_cond, 0);
        let pb_fall = emit_incdec_jcc_arm(
            b, producer, fall, (fall <= g_self_rip) as c_int, body_edge, -1,
            epilogue_sites, n_epi, ptr::null_mut(),
        );
        let taken_label = ffi::a64_label(b);
        ffi::a64_patch_bcond(to_taken, taken_label);
        let mut taken_rec = 0;
        let pb_taken = emit_incdec_jcc_arm(
            b, producer, taken, (taken <= g_self_rip) as c_int, body_edge, -1,
            epilogue_sites, n_epi, &mut taken_rec,
        );
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
        g_jcc_edge[1].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[1].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_n_jcc_edges = 2;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_arith_incdec_jcc(
    b: *mut ffi::A64Buf,
    arith: *const ffi::X86Insn,
    incdec: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    arith_need: u64,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    incdec_label: *mut *mut u32,
    jcc_label: *mut *mut u32,
) -> c_int {
    unsafe {
        if ffi::g_defer == 0 || g_no_jccfuse != 0 || g_no_regflags != 0
            || ffi::g_no_chain != 0 || g_no_jcclink != 0 || arith_need != ffi::OCERZ_CF
            || ((*arith).op != ffi::OCERZ_OP_ADD && (*arith).op != ffi::OCERZ_OP_SUB)
            || ((*incdec).op != ffi::OCERZ_OP_INC && (*incdec).op != ffi::OCERZ_OP_DEC)
            || (*jcc).op != ffi::OCERZ_OP_JCC
            || ((*jcc).cc as c_uint != ffi::OCERZ_CC_E && (*jcc).cc as c_uint != ffi::OCERZ_CC_NE)
            || (*jcc).ops[0].kind != ffi::OCERZ_OPK_IMM
            || (*arith).lock != 0 || (*incdec).lock != 0
            || (*arith).rip.wrapping_add((*arith).len as u64) != (*incdec).rip
            || (*incdec).rip.wrapping_add((*incdec).len as u64) != (*jcc).rip
        {
            return 0;
        }
        let d = ptr::addr_of!((*arith).ops[0]);
        let s = ptr::addr_of!((*arith).ops[1]);
        let id = ptr::addr_of!((*incdec).ops[0]);
        if (*arith).nops != 2 || (*incdec).nops != 1
            || (*d).kind != ffi::OCERZ_OPK_REG || (*id).kind != ffi::OCERZ_OPK_REG
            || (*d).high8 != 0 || (*id).high8 != 0
            || ((*d).size != 4 && (*d).size != 8) || (*id).size != (*d).size
            || (*id).reg == (*d).reg
        {
            return 0;
        }
        if (*s).kind == ffi::OCERZ_OPK_REG {
            if (*s).high8 != 0 || (*s).size != (*d).size {
                return 0;
            }
        } else if (*s).kind != ffi::OCERZ_OPK_IMM || (*s).size != (*d).size {
            return 0;
        }
        let sf = ((*d).size == 8) as c_int;
        let ds = jit_internal::pin_slot((*d).reg);
        let rn = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT2 };
        if ds < 0 {
            jit_internal::emit_gpr_rd(b, sf, rn, (*d).reg);
        }
        let mut emitted = 0;
        let mut rm = ffi::JT1;
        if (*s).kind == ffi::OCERZ_OPK_IMM {
            let mut v = (*s).imm;
            if sf == 0 {
                v &= 0xffff_ffff;
            }
            if v <= 4095 {
                if (*arith).op == ffi::OCERZ_OP_ADD {
                    ffi::a64_adds_imm(b, sf, rn, rn, v as c_uint);
                } else {
                    ffi::a64_subs_imm(b, sf, rn, rn, v as c_uint);
                }
                emitted = 1;
            } else {
                ffi::a64_mov_imm64(b, ffi::JT1, v);
            }
        } else {
            let ss = jit_internal::pin_slot((*s).reg);
            if (*s).reg == (*d).reg {
                rm = rn;
            } else if ss >= 0 {
                rm = jit_internal::pin_hreg(ss);
            } else {
                jit_internal::emit_gpr_rd(b, sf, ffi::JT1, (*s).reg);
            }
        }
        if emitted == 0 {
            if (*arith).op == ffi::OCERZ_OP_ADD {
                ffi::a64_adds_reg(b, sf, rn, rn, rm, 0);
            } else {
                ffi::a64_subs_reg(b, sf, rn, rn, rm, 0);
            }
        }
        if ds < 0 {
            jit_internal::emit_gpr_wr(b, rn, (*d).reg);
        }
        ffi::a64_cset(b, ffi::JT0, if (*arith).op == ffi::OCERZ_OP_ADD { ffi::A64_CS } else { ffi::A64_CC });
        *incdec_label = ffi::a64_label(b);
        let ids = jit_internal::pin_slot((*id).reg);
        let ird = if ids >= 0 { jit_internal::pin_hreg(ids) } else { ffi::JT2 };
        if ids < 0 {
            jit_internal::emit_gpr_rd(b, sf, ird, (*id).reg);
        }
        if (*incdec).op == ffi::OCERZ_OP_INC {
            ffi::a64_adds_imm(b, sf, ird, ird, 1);
        } else {
            ffi::a64_subs_imm(b, sf, ird, ird, 1);
        }
        if ids < 0 {
            jit_internal::emit_gpr_wr(b, ird, (*id).reg);
        }
        *jcc_label = ffi::a64_label(b);
        let taken_cond = if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { ffi::A64_EQ } else { ffi::A64_NE };
        let taken = (*jcc).ops[0].imm;
        let mut fall = (*jcc).rip.wrapping_add((*jcc).len as u64);
        if (*jcc).mode32 != 0 {
            fall = fall as u32 as u64;
        }
        let edge_class = jit_internal::body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        let to_taken = ffi::a64_label(b);
        ffi::a64_bcond(b, taken_cond, 0);
        let pb_fall = emit_incdec_jcc_arm(
            b, incdec, fall, (fall <= g_self_rip) as c_int, body_edge, ffi::JT0,
            epilogue_sites, n_epi, ptr::null_mut(),
        );
        let taken_label = ffi::a64_label(b);
        ffi::a64_patch_bcond(to_taken, taken_label);
        let mut taken_rec = 0;
        let pb_taken = emit_incdec_jcc_arm(
            b, incdec, taken, (taken <= g_self_rip) as c_int, body_edge, ffi::JT0,
            epilogue_sites, n_epi, &mut taken_rec,
        );
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
        g_jcc_edge[1].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[1].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_n_jcc_edges = 2;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_logic_jmp_incdec_jcc(
    b: *mut ffi::A64Buf,
    logic: *const ffi::X86Insn,
    jmp: *const ffi::X86Insn,
    logic_need: u64,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    jmp_label: *mut *mut u32,
) -> c_int {
    unsafe {
        if g_xlat_mode32 != 0 {
            return 0;
        }
        if ffi::g_defer == 0 || g_no_jccfuse != 0 || g_no_regflags != 0
            || ffi::g_no_chain != 0 || g_no_jcclink != 0 || ffi::g_pin_class != 1
            || logic_need != ffi::OCERZ_CF
            || ((*logic).op != ffi::OCERZ_OP_AND && (*logic).op != ffi::OCERZ_OP_OR
                && (*logic).op != ffi::OCERZ_OP_XOR)
            || (*logic).lock != 0 || (*jmp).op != ffi::OCERZ_OP_JMP
            || (*jmp).ops[0].kind != ffi::OCERZ_OPK_IMM
            || (*logic).rip.wrapping_add((*logic).len as u64) != (*jmp).rip
        {
            return 0;
        }
        let mut decoded = LogicTargetDecode {
            target: MaybeUninit::uninit(),
            rip: (*jmp).ops[0].imm,
            n: 0,
            pc: (*jmp).ops[0].imm,
        };
        let ok = jit_flags_guarded(
            logic_target_decode_cb,
            (&mut decoded as *mut LogicTargetDecode).cast(),
        );
        let target = decoded.target.as_ptr().cast::<ffi::X86Insn>();
        if ok == 0 || ptr::read_volatile(ptr::addr_of!(decoded.n)) != 2
            || jit_internal::can_fuse_incdec_jcc(&*target, &*target.add(1)) == 0
        {
            return 0;
        }
        let ld = ptr::addr_of!((*logic).ops[0]);
        let ls = ptr::addr_of!((*logic).ops[1]);
        if (*logic).nops != 2 || (*ld).kind != ffi::OCERZ_OPK_REG || (*ld).high8 != 0
            || ((*ld).size != 4 && (*ld).size != 8) || (*ls).size != (*ld).size
        {
            return 0;
        }
        if (*ls).kind == ffi::OCERZ_OPK_REG {
            if (*ls).high8 != 0 {
                return 0;
            }
        } else if (*ls).kind != ffi::OCERZ_OPK_IMM {
            return 0;
        }
        if ffi::emit_arith(b, logic, 0) == 0 {
            return 0;
        }
        *jmp_label = ffi::a64_label(b);
        let incdec = target;
        let jcc = target.add(1);
        let id = ptr::addr_of!((*incdec).ops[0]);
        let sf = ((*id).size == 8) as c_int;
        let ids = jit_internal::pin_slot((*id).reg);
        let rd = if ids >= 0 { jit_internal::pin_hreg(ids) } else { ffi::JT2 };
        if ids < 0 {
            jit_internal::emit_gpr_rd(b, sf, rd, (*id).reg);
        }
        if (*incdec).op == ffi::OCERZ_OP_INC {
            ffi::a64_adds_imm(b, sf, rd, rd, 1);
        } else {
            ffi::a64_subs_imm(b, sf, rd, rd, 1);
        }
        if ids < 0 {
            jit_internal::emit_gpr_wr(b, rd, (*id).reg);
        }
        let taken_cond = if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { ffi::A64_EQ } else { ffi::A64_NE };
        let taken = (*jcc).ops[0].imm;
        let fall = (*jcc).rip.wrapping_add((*jcc).len as u64);
        let edge_class = jit_internal::body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        jit_internal::l0_flush_all(b);
        let to_taken = ffi::a64_label(b);
        ffi::a64_bcond(b, taken_cond, 0);
        let pb_fall = emit_incdec_jcc_arm(
            b, incdec, fall, (fall <= g_self_rip) as c_int, body_edge, ffi::A64_ZR as c_int,
            epilogue_sites, n_epi, ptr::null_mut(),
        );
        let taken_label = ffi::a64_label(b);
        ffi::a64_patch_bcond(to_taken, taken_label);
        let mut taken_rec = 0;
        let pb_taken = emit_incdec_jcc_arm(
            b, incdec, taken, (taken <= g_self_rip) as c_int, body_edge, ffi::A64_ZR as c_int,
            epilogue_sites, n_epi, &mut taken_rec,
        );
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
        g_jcc_edge[1].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[1].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_n_jcc_edges = 2;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cmp_test_jcc(
    b: *mut ffi::A64Buf,
    producer: *const ffi::X86Insn,
    jcc: *const ffi::X86Insn,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
    jcc_label: *mut *mut u32,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
    gap: *const ffi::X86Insn,
    gap_label: *mut *mut u32,
) -> c_int {
    unsafe {
        if can_fuse_cmp_test_jcc(producer, jcc, g_self_rip) == 0 || ffi::g_defer == 0 {
            return 0;
        }
        let has_gap = !gap.is_null();
        if has_gap {
            let pd = ptr::addr_of!((*producer).ops[0]);
            let ps = ptr::addr_of!((*producer).ops[1]);
            if ((*pd).kind == ffi::OCERZ_OPK_REG && insn_writes_reg(gap, (*pd).reg) != 0)
                || ((*ps).kind == ffi::OCERZ_OPK_REG && insn_writes_reg(gap, (*ps).reg) != 0)
                || jcc_gap_ok(gap) == 0
            {
                return 0;
            }
            let mut tmpw = MaybeUninit::<[u32; 128]>::uninit();
            let tmpw_ptr = tmpw.as_mut_ptr().cast::<u32>();
            let mut tb = ffi::A64Buf {
                start: tmpw_ptr,
                p: tmpw_ptr,
                end: tmpw_ptr.add(128),
                overflow: 0,
                sink: 0,
            };
            let saved = ptr::read(ptr::addr_of!(ffi::g_ea_cache));
            let saved_lits = ffi::g_n_raslit;
            jit_internal::ea_cache_reset();
            let ok = emit_flag_neutral(&mut tb, gap) != 0 && tb.overflow == 0;
            ptr::write(ptr::addr_of_mut!(ffi::g_ea_cache), saved);
            ffi::g_n_raslit = saved_lits;
            if !ok {
                return 0;
            }
        }
        let pre_rec = (has_gap && stack_gap_load_ok(gap) != 0) as c_int;
        let d = ptr::addr_of!((*producer).ops[0]);
        let s = ptr::addr_of!((*producer).ops[1]);
        let sf = ((*d).size == 8) as c_int;
        let taken = (*jcc).ops[0].imm;
        let mut fall = (*jcc).rip.wrapping_add((*jcc).len as u64);
        if (*jcc).mode32 != 0 {
            fall = fall as u32 as u64;
        }
        let self_loop = (taken == g_self_rip) as c_int;
        let mut test_bit = -1;
        let mut test_sf = 0;
        let mut test_rn = -1;
        let mut test_mask = 0u64;
        let mut cbz_rn = -1;
        let mut cbz_sf = 0;
        let mut rec_imm_pending = 0;
        let mut rec_imm = 0u64;
        let cc_is_zero_test = (*jcc).cc as c_uint == ffi::OCERZ_CC_E || (*jcc).cc as c_uint == ffi::OCERZ_CC_NE;
        let mut record_src = ffi::JT2;
        let mut record_dst = ffi::JT2;
        let mut ccop = 0u32;
        let d_mem = ((*d).kind == ffi::OCERZ_OPK_MEM) as c_int;
        let s_mem = ((*s).kind == ffi::OCERZ_OPK_MEM) as c_int;
        let mut d_in_jt0 = 0;
        let mut s_in_jt1 = 0;
        if d_mem != 0 || s_mem != 0 {
            let m = if d_mem != 0 { d } else { s };
            let into = if d_mem != 0 { ffi::JT0 } else { ffi::JT1 };
            if ffi::emit_mem_load_plain(b, producer, m, (*d).size as c_int, into) == 0 {
                if ffi::emit_mem_ea(b, producer, m, ffi::JTA) == 0 {
                    return 0;
                }
                let skip = ffi::emit_commpage_guard(b, producer, ffi::JTA, exit_sites, n_exits);
                jit_internal::emit_add_const(
                    b,
                    ffi::JTA,
                    ffi::ocerz_guest_base.wrapping_sub(jit_internal::ea_fold()),
                );
                ffi::emit_guest_load_ordered(b, (*d).size as c_int, into, ffi::JTA, ffi::JTU);
                jit_internal::patch_guard_skip(skip, ffi::a64_label(b));
            }
            if d_mem != 0 {
                d_in_jt0 = 1;
            } else {
                s_in_jt1 = 1;
            }
        }
        let narrow_direct = ((*d).size == 1 || (*d).size == 2)
            && (*producer).op == ffi::OCERZ_OP_CMP
            && ((*jcc).cc as c_uint == ffi::OCERZ_CC_E
                || (*jcc).cc as c_uint == ffi::OCERZ_CC_NE
                || (*jcc).cc as c_uint == ffi::OCERZ_CC_B
                || (*jcc).cc as c_uint == ffi::OCERZ_CC_AE
                || (*jcc).cc as c_uint == ffi::OCERZ_CC_A
                || (*jcc).cc as c_uint == ffi::OCERZ_CC_BE)
            && (*d).high8 == 0
            && ((*s).kind != ffi::OCERZ_OPK_REG || (*s).high8 == 0);
        if narrow_direct {
            let size = (*d).size as c_int;
            let mask = if size == 1 { 0xff } else { 0xffff };
            let rn: c_int;
            let mut rm: c_int;
            if d_in_jt0 != 0 {
                rn = ffi::JT0;
            } else {
                let ds = jit_internal::pin_slot((*d).reg);
                if ds >= 0 {
                    if size == 1 {
                        ffi::a64_uxtb(b, ffi::JT0, jit_internal::pin_hreg(ds));
                    } else {
                        ffi::a64_uxth(b, ffi::JT0, jit_internal::pin_hreg(ds));
                    }
                } else {
                    jit_internal::emit_gpr_rd(b, 1, ffi::JT0, (*d).reg);
                    if size == 1 { ffi::a64_uxtb(b, ffi::JT0, ffi::JT0); }
                    else { ffi::a64_uxth(b, ffi::JT0, ffi::JT0); }
                }
                rn = ffi::JT0;
            }
            record_src = rn;
            if s_in_jt1 != 0 {
                rm = ffi::JT1;
                ffi::a64_subs_reg(b, 0, ffi::A64_ZR, rn, rm, 0);
            } else if (*s).kind == ffi::OCERZ_OPK_REG {
                let ss = jit_internal::pin_slot((*s).reg);
                if ss >= 0 {
                    if size == 1 { ffi::a64_uxtb(b, ffi::JT1, jit_internal::pin_hreg(ss)); }
                    else { ffi::a64_uxth(b, ffi::JT1, jit_internal::pin_hreg(ss)); }
                } else {
                    jit_internal::emit_gpr_rd(b, 1, ffi::JT1, (*s).reg);
                    if size == 1 { ffi::a64_uxtb(b, ffi::JT1, ffi::JT1); }
                    else { ffi::a64_uxth(b, ffi::JT1, ffi::JT1); }
                }
                rm = ffi::JT1;
                ffi::a64_subs_reg(b, 0, ffi::A64_ZR, rn, rm, 0);
            } else {
                let v = (*s).imm & mask;
                if v == 0 && cc_is_zero_test {
                    cbz_rn = rn;
                    cbz_sf = 0;
                    rm = ffi::A64_ZR;
                } else {
                    ffi::a64_mov_imm64(b, ffi::JT1, v);
                    rm = ffi::JT1;
                    if v <= 4095 {
                        ffi::a64_subs_imm(b, 0, ffi::A64_ZR, rn, v as c_uint);
                    } else {
                        ffi::a64_subs_reg(b, 0, ffi::A64_ZR, rn, rm, 0);
                    }
                }
            }
            record_dst = rm;
            ccop = ocerz_cc_pack(ffi::OCERZ_CC_SUB, size, 0);
        } else if ((*d).size == 1 || (*d).size == 2)
            && (*producer).op == ffi::OCERZ_OP_TEST
            && d_mem == 0
            && (*d).high8 == 0
            && (*s).kind == ffi::OCERZ_OPK_IMM
            && ((*jcc).cc as c_uint == ffi::OCERZ_CC_E || (*jcc).cc as c_uint == ffi::OCERZ_CC_NE)
        {
            let v = (*s).imm & if (*d).size == 1 { 0xff } else { 0xffff };
            let ds = jit_internal::pin_slot((*d).reg);
            let rn = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT0 };
            if ds < 0 { jit_internal::emit_gpr_rd(b, 1, ffi::JT0, (*d).reg); }
            if v != 0 && (v & (v - 1)) == 0 {
                test_bit = v.trailing_zeros() as c_int;
                test_rn = rn;
                test_mask = v;
                test_sf = 0;
            } else if ffi::a64_try_ands_imm(b, 0, ffi::JT2, rn, v) == 0 {
                ffi::a64_mov_imm64(b, ffi::JT1, v);
                ffi::a64_ands_reg(b, 0, ffi::JT2, rn, ffi::JT1, 0);
            }
            record_src = ffi::JT2;
            record_dst = ffi::JT2;
            ccop = ocerz_cc_pack(ffi::OCERZ_CC_LOGIC, (*d).size as c_int, 0);
        } else if ((*d).size == 1 || (*d).size == 2)
            && (*producer).op == ffi::OCERZ_OP_TEST
            && d_mem == 0 && s_mem == 0
            && (*s).kind == ffi::OCERZ_OPK_REG
            && (*d).high8 == 0 && (*s).high8 == 0
            && ((*jcc).cc as c_uint == ffi::OCERZ_CC_E || (*jcc).cc as c_uint == ffi::OCERZ_CC_NE)
        {
            let mask = if (*d).size == 1 { 0xff } else { 0xffff };
            let ds = jit_internal::pin_slot((*d).reg);
            let ss = jit_internal::pin_slot((*s).reg);
            let ra = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT0 };
            let rb = if ss >= 0 { jit_internal::pin_hreg(ss) } else { ffi::JT1 };
            if ds < 0 { jit_internal::emit_gpr_rd(b, 1, ffi::JT0, (*d).reg); }
            if ss < 0 && (*s).reg != (*d).reg {
                jit_internal::emit_gpr_rd(b, 1, ffi::JT1, (*s).reg);
            }
            if (*d).reg == (*s).reg {
                ffi::a64_try_ands_imm(b, 0, ffi::JT2, ra, mask);
            } else {
                ffi::a64_and_reg(b, 0, ffi::JT2, ra, rb, 0);
                ffi::a64_try_ands_imm(b, 0, ffi::JT2, ffi::JT2, mask);
            }
            record_src = ffi::JT2;
            record_dst = ffi::JT2;
            ccop = ocerz_cc_pack(ffi::OCERZ_CC_LOGIC, (*d).size as c_int, 0);
        } else if (*d).size == 1 || (*d).size == 2 {
            let size = (*d).size as c_int;
            let sh = 32 - 8 * size;
            let mask = if size == 1 { 0xff } else { 0xffff };
            if d_in_jt0 == 0 {
                jit_internal::emit_gpr_rd(b, 1, ffi::JT0, (*d).reg);
                if size == 1 { ffi::a64_uxtb(b, ffi::JT0, ffi::JT0); }
                else { ffi::a64_uxth(b, ffi::JT0, ffi::JT0); }
            }
            if s_in_jt1 == 0 {
                if (*s).kind == ffi::OCERZ_OPK_REG {
                    jit_internal::emit_gpr_rd(b, 1, ffi::JT1, (*s).reg);
                    if size == 1 { ffi::a64_uxtb(b, ffi::JT1, ffi::JT1); }
                    else { ffi::a64_uxth(b, ffi::JT1, ffi::JT1); }
                } else {
                    ffi::a64_mov_imm64(b, ffi::JT1, (*s).imm & mask);
                }
            }
            ffi::a64_lsl_imm(b, 0, ffi::JTA, ffi::JT0, sh);
            ffi::a64_lsl_imm(b, 0, ffi::JTU, ffi::JT1, sh);
            if (*producer).op == ffi::OCERZ_OP_CMP {
                ffi::a64_subs_reg(b, 0, ffi::A64_ZR, ffi::JTA, ffi::JTU, 0);
                record_src = ffi::JT0;
                record_dst = ffi::JT1;
                ccop = ocerz_cc_pack(ffi::OCERZ_CC_SUB, size, 0);
            } else {
                ffi::a64_ands_reg(b, 0, ffi::JT2, ffi::JTA, ffi::JTU, 0);
                ffi::a64_lsr_imm(b, 0, ffi::JT2, ffi::JT2, sh);
                record_src = ffi::JT2;
                record_dst = ffi::JT2;
                ccop = ocerz_cc_pack(ffi::OCERZ_CC_LOGIC, size, 0);
            }
        } else if (*producer).op == ffi::OCERZ_OP_CMP {
            let ds = if d_in_jt0 != 0 { -1 } else { jit_internal::pin_slot((*d).reg) };
            record_src = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT0 };
            if ds < 0 && d_in_jt0 == 0 { jit_internal::emit_gpr_rd(b, sf, ffi::JT0, (*d).reg); }
            let mut cmp_done = false;
            if s_in_jt1 != 0 {
                record_dst = ffi::JT1;
            } else if (*s).kind == ffi::OCERZ_OPK_REG && (*s).high8 == 0 {
                let ss = jit_internal::pin_slot((*s).reg);
                record_dst = if ss >= 0 { jit_internal::pin_hreg(ss) } else { ffi::JT1 };
                if ss < 0 { jit_internal::emit_gpr_rd(b, sf, ffi::JT1, (*s).reg); }
            } else if (*s).kind == ffi::OCERZ_OPK_IMM {
                let v = if sf != 0 { (*s).imm } else { (*s).imm & 0xffff_ffff };
                if v == 0 && cc_is_zero_test {
                    cbz_rn = record_src;
                    cbz_sf = sf;
                    record_dst = ffi::A64_ZR;
                } else if v <= 4095 || ((v & 0xfff) == 0 && (v >> 12) <= 4095) {
                    if v <= 4095 {
                        ffi::a64_subs_imm(b, sf, ffi::A64_ZR, record_src, v as c_uint);
                    } else {
                        ffi::a64_subs_imm_sh12(b, sf, ffi::A64_ZR, record_src, (v >> 12) as c_uint);
                    }
                    rec_imm_pending = 1;
                    rec_imm = v;
                    record_dst = ffi::JT1;
                    ccop = ocerz_cc_pack(ffi::OCERZ_CC_SUB, (*d).size as c_int, 0);
                    cmp_done = true;
                } else {
                    ffi::a64_mov_imm64(b, ffi::JT1, v);
                    record_dst = ffi::JT1;
                }
            } else {
                return 0;
            }
            if !cmp_done {
                if cbz_rn < 0 {
                    ffi::a64_subs_reg(b, sf, ffi::A64_ZR, record_src, record_dst, 0);
                }
                ccop = ocerz_cc_pack(ffi::OCERZ_CC_SUB, (*d).size as c_int, 0);
            }
        } else {
            let ds = if d_in_jt0 != 0 { -1 } else { jit_internal::pin_slot((*d).reg) };
            let rn = if ds >= 0 { jit_internal::pin_hreg(ds) } else { ffi::JT0 };
            if ds < 0 && d_in_jt0 == 0 { jit_internal::emit_gpr_rd(b, sf, ffi::JT0, (*d).reg); }
            let mut emitted = 0;
            if s_in_jt1 != 0 {
                ffi::a64_ands_reg(b, sf, ffi::JT2, rn, ffi::JT1, 0);
                emitted = 1;
            } else if (*s).kind == ffi::OCERZ_OPK_IMM {
                let v = if sf != 0 { (*s).imm } else { (*s).imm & 0xffff_ffff };
                if v != 0 && (v & (v - 1)) == 0 && cc_is_zero_test {
                    test_bit = v.trailing_zeros() as c_int;
                    test_rn = rn;
                    test_mask = v;
                    test_sf = sf;
                    emitted = 1;
                } else {
                    emitted = ffi::a64_try_ands_imm(b, sf, ffi::JT2, rn, v);
                    if emitted == 0 {
                        ffi::a64_mov_imm64(b, ffi::JT1, v);
                        ffi::a64_ands_reg(b, sf, ffi::JT2, rn, ffi::JT1, 0);
                        emitted = 1;
                    }
                }
            } else if (*s).kind == ffi::OCERZ_OPK_REG && (*s).high8 == 0 {
                let ss = jit_internal::pin_slot((*s).reg);
                let rm = if ss >= 0 { jit_internal::pin_hreg(ss) } else { ffi::JT1 };
                if ss < 0 { jit_internal::emit_gpr_rd(b, sf, ffi::JT1, (*s).reg); }
                ffi::a64_ands_reg(b, sf, ffi::JT2, rn, rm, 0);
                emitted = 1;
            }
            if emitted == 0 { return 0; }
            ccop = ocerz_cc_pack(ffi::OCERZ_CC_LOGIC, (*d).size as c_int, 0);
        }
        if pre_rec != 0 {
            if (*producer).op == ffi::OCERZ_OP_CMP {
                if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
            } else {
                if test_bit >= 0 {
                    ffi::a64_mov_imm64(b, ffi::JT2, test_mask);
                    ffi::a64_and_reg(b, 1, ffi::JT2, test_rn, ffi::JT2, 0);
                }
                jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
            }
        }
        if has_gap {
            let gl = ffi::a64_label(b);
            if emit_flag_neutral(b, gap) == 0 {
                return 0;
            }
            if !gap_label.is_null() {
                *gap_label = gl;
            }
        }
        *jcc_label = ffi::a64_label(b);
        let taken_cond = fused_jcc_cond(producer, jcc);
        if g_jcc_side_mode != 0 && self_loop == 0 {
            let taken_live = g_no_xlive != 0
                || jit_internal::xlive_succ_live(g_xlat_jit, taken) != 0;
            let need_rec = pre_rec == 0 && (g_jcc_side_need != 0 || taken_live);
            let rec_after = need_rec && !taken_live;
            let nostub = env_on!(c"OCERZ_NO_RECSTUB");
            let rec_stub = !nostub
                && need_rec
                && taken_live
                && g_jcc_side_fall_need == 0
                && (*producer).op == ffi::OCERZ_OP_CMP
                && g_n_side < ffi::SIDE_MAX as c_int
                && record_src >= 0
                && record_dst >= 0
                && (record_src < 9 || record_src > 15)
                && (record_dst < 9
                    || record_dst > 15
                    || (rec_imm_pending != 0 && record_dst == ffi::JT1));
            if need_rec && !rec_after && !rec_stub {
                if (*producer).op == ffi::OCERZ_OP_CMP {
                    if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                    jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
                } else {
                    if test_bit >= 0 {
                        ffi::a64_mov_imm64(b, ffi::JT2, test_mask);
                        ffi::a64_and_reg(b, 1, ffi::JT2, test_rn, ffi::JT2, 0);
                    }
                    jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
                }
            }
            if g_n_side >= ffi::SIDE_MAX as c_int {
                return 0;
            }
            let si = g_n_side as usize;
            g_side[si].site = ffi::a64_label(b);
            g_side[si].taken = taken;
            g_side[si].idx = -1;
            g_side[si].stub = ptr::null_mut();
            g_side[si].patch_b = ptr::null_mut();
            g_side[si].rec = rec_stub as c_int;
            g_side[si].fpb = -1;
            g_side[si].fpb_chk = 0;
            g_side[si].l0_dirty = ffi::g_l0_dirty;
            g_side[si].yc_dirty = ffi::g_yc_dirty;
            for r in 0..16 {
                g_side[si].l0[r] = ffi::g_l0[r];
                g_side[si].l0_dbl[r] = ffi::g_l0_dbl[r];
            }
            g_side[si].jcc_rip = (*jcc).rip;
            g_side[si].ft_rip = (*jcc).rip.wrapping_add((*jcc).len as u64);
            g_side[si].ft_site = ptr::null_mut();
            g_side[si].probe = (!need_rec
                && jit_internal::probe_wanted(
                    (*jcc).rip,
                    (*jcc).rip.wrapping_add((*jcc).len as u64),
                ) != 0) as c_int;
            if rec_stub {
                g_side[si].rec_ccop = ccop;
                g_side[si].rec_src = record_src;
                g_side[si].rec_dst = record_dst;
                g_side[si].rec_imm_pending = rec_imm_pending;
                g_side[si].rec_imm = rec_imm;
            }
            g_n_side += 1;
            if test_bit >= 0 {
                if (*jcc).cc as c_uint == ffi::OCERZ_CC_E {
                    ffi::a64_tbz(b, test_rn, test_bit, 0);
                } else {
                    ffi::a64_tbnz(b, test_rn, test_bit, 0);
                }
            } else if cbz_rn >= 0 {
                if (*jcc).cc as c_uint == ffi::OCERZ_CC_E {
                    ffi::a64_cbz(b, cbz_sf, cbz_rn, 0);
                } else {
                    ffi::a64_cbnz(b, cbz_sf, cbz_rn, 0);
                }
            } else {
                ffi::a64_bcond(b, taken_cond, 0);
            }
            if g_side[(g_n_side - 1) as usize].probe != 0 {
                g_side[(g_n_side - 1) as usize].ft_site = ffi::a64_label(b);
                ffi::a64_b(b, 0);
            }
            if rec_after {
                if (*producer).op == ffi::OCERZ_OP_CMP {
                    if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                    jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
                } else {
                    if test_bit >= 0 {
                        ffi::a64_mov_imm64(b, ffi::JT2, test_mask);
                        ffi::a64_and_reg(b, 1, ffi::JT2, test_rn, ffi::JT2, 0);
                    }
                    jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
                }
            }
            return 1;
        }
        if self_loop == 0 {
            let poll_fall = (fall <= g_self_rip) as c_int;
            let poll_taken = (taken <= g_self_rip) as c_int;
            let edge_class = jit_internal::body_edge_pin_class();
            let body_edge = (edge_class >= 0) as c_int;
            let to_taken = ffi::a64_label(b);
            if test_bit >= 0 {
                if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { ffi::a64_tbz(b, test_rn, test_bit, 0); }
                else { ffi::a64_tbnz(b, test_rn, test_bit, 0); }
            } else if cbz_rn >= 0 {
                if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { ffi::a64_cbz(b, cbz_sf, cbz_rn, 0); }
                else { ffi::a64_cbnz(b, cbz_sf, cbz_rn, 0); }
            } else {
                ffi::a64_bcond(b, taken_cond, 0);
            }
            if pre_rec == 0 && (g_no_xlive != 0 || jit_internal::xlive_succ_live(g_xlat_jit, fall) != 0) {
                if (*producer).op == ffi::OCERZ_OP_CMP {
                    if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                    jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
                } else {
                    if test_bit >= 0 {
                        ffi::a64_mov_imm64(b, ffi::JT2, if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { test_mask } else { 0 });
                    }
                    jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
                }
            }
            let pb_fall = jit_internal::emit_static_chain_tail(b, fall, poll_fall, body_edge, epilogue_sites, n_epi);
            let taken_label = ffi::a64_label(b);
            if test_bit >= 0 { ffi::a64_patch_tbz(to_taken, taken_label); }
            else if cbz_rn >= 0 { ffi::a64_patch_cbz(to_taken, taken_label); }
            else { ffi::a64_patch_bcond(to_taken, taken_label); }
            let taken_rec = pre_rec == 0
                && (g_no_xlive != 0 || jit_internal::xlive_succ_live(g_xlat_jit, taken) != 0);
            if taken_rec {
                if (*producer).op == ffi::OCERZ_OP_CMP {
                    if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                    jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
                } else {
                    if test_bit >= 0 {
                        ffi::a64_mov_imm64(b, ffi::JT2, if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { 0 } else { test_mask });
                    }
                    jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
                }
            }
            let pb_taken = jit_internal::emit_static_chain_tail(b, taken, poll_taken, body_edge, epilogue_sites, n_epi);
            g_jcc_edge[0].target_rip = fall;
            g_jcc_edge[0].patch_b = pb_fall;
            g_jcc_edge[0].cond_site = ptr::null_mut();
            g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_jcc_edge[1].target_rip = taken;
            g_jcc_edge[1].patch_b = pb_taken;
            g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec as c_int, body_edge);
            g_jcc_edge[1].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[1].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_n_jcc_edges = 2;
            return 1;
        }
        if g_loop_entry.is_null() {
            return 0;
        }
        jit_internal::l0_fixed_backedge(b);
        let mut tb_ok = 0;
        if test_bit >= 0 {
            let reach = g_loop_entry.offset_from(ffi::a64_label(b));
            tb_ok = (reach >= -(1 << 13) && reach < (1 << 13)) as c_int;
            if tb_ok == 0 && ffi::a64_try_ands_imm(b, test_sf, ffi::JT2, test_rn, test_mask) == 0 {
                ffi::a64_mov_imm64(b, ffi::JT1, test_mask);
                ffi::a64_ands_reg(b, test_sf, ffi::JT2, test_rn, ffi::JT1, 0);
            }
        }
        g_stop_patch = ffi::a64_label(b);
        if test_bit >= 0 && tb_ok != 0 {
            if (*jcc).cc as c_uint == ffi::OCERZ_CC_E {
                ffi::a64_tbz(b, test_rn, test_bit, g_loop_entry.offset_from(g_stop_patch) as i32);
            } else {
                ffi::a64_tbnz(b, test_rn, test_bit, g_loop_entry.offset_from(g_stop_patch) as i32);
            }
        } else if cbz_rn >= 0 {
            if (*jcc).cc as c_uint == ffi::OCERZ_CC_E {
                ffi::a64_cbz(b, cbz_sf, cbz_rn, g_loop_entry.offset_from(g_stop_patch) as i32);
            } else {
                ffi::a64_cbnz(b, cbz_sf, cbz_rn, g_loop_entry.offset_from(g_stop_patch) as i32);
            }
        } else {
            ffi::a64_bcond(b, taken_cond, g_loop_entry.offset_from(g_stop_patch) as i32);
        }
        jit_internal::l0_fixed_fallthrough(b);
        jit_internal::fpb_emit_exit_check(b);
        if pre_rec == 0 && (g_no_xlive != 0 || jit_internal::xlive_succ_live(g_xlat_jit, fall) != 0) {
            if (*producer).op == ffi::OCERZ_OP_CMP {
                if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
            } else {
                if test_bit >= 0 { ffi::a64_mov_imm64(b, ffi::JT2, if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { test_mask } else { 0 }); }
                jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
            }
        }
        let edge_class = jit_internal::body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        let pb_fall = jit_internal::emit_static_chain_tail(b, fall, (fall <= g_self_rip) as c_int, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
        g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
        g_n_jcc_edges = 1;
        g_stop_target = ffi::a64_label(b);
        if pre_rec == 0 && (g_no_xlive != 0 || jit_internal::xlive_succ_live(g_xlat_jit, taken) != 0) {
            if (*producer).op == ffi::OCERZ_OP_CMP {
                if rec_imm_pending != 0 { ffi::a64_mov_imm64(b, ffi::JT1, rec_imm); }
                jit_internal::emit_defer_flags(b, ccop, record_src, record_dst);
            } else {
                if test_bit >= 0 { ffi::a64_mov_imm64(b, ffi::JT2, if (*jcc).cc as c_uint == ffi::OCERZ_CC_E { 0 } else { test_mask }); }
                jit_internal::emit_defer_flags(b, ccop, ffi::JT2, ffi::JT2);
            }
        }
        ffi::a64_mov_imm64(b, ffi::JT0, taken);
        ffi::a64_str(b, 8, ffi::JT0, 20, ffi::RIP_OFF);
        ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
        *epilogue_sites.add(*n_epi as usize) = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        *n_epi += 1;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_jcc(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    epilogue_sites: *mut *mut u32,
    n_epi: *mut c_int,
) -> c_int {
    unsafe {
        if (*insn).op != ffi::OCERZ_OP_JCC {
            return 0;
        }
        let cc = (*insn).cc as c_uint;
        let taken = (*insn).ops[0].imm;
        let mut fall = (*insn).rip.wrapping_add((*insn).len as u64);
        if (*insn).mode32 != 0 {
            fall = fall as u32 as u64;
        }
        let self_loop = (ffi::g_no_chain == 0
            && !g_loop_entry.is_null()
            && taken == g_self_rip) as c_int;
        let two_way = (self_loop == 0 && ffi::g_no_chain == 0 && g_no_jcclink == 0) as c_int;
        g_cc_want_cbz = two_way;
        emit_cc_predicate_ex(b, cc, (two_way != 0 || self_loop != 0) as c_int);
        g_cc_want_cbz = 0;
        let direct = if two_way != 0 || self_loop != 0 { ffi::g_cc_direct } else { -1 };
        if self_loop != 0 && direct >= 0 {
            jit_internal::l0_fixed_backedge(b);
            g_stop_patch = ffi::a64_label(b);
            ffi::a64_bcond(b, direct, g_loop_entry.offset_from(g_stop_patch) as i32);
            jit_internal::l0_fixed_fallthrough(b);
            jit_internal::fpb_emit_exit_check(b);
            let edge_class = jit_internal::body_edge_pin_class();
            let body_edge = (edge_class >= 0) as c_int;
            let pb_fall = jit_internal::emit_static_chain_tail(b, fall, (fall <= g_self_rip) as c_int, body_edge, epilogue_sites, n_epi);
            g_jcc_edge[0].target_rip = fall;
            g_jcc_edge[0].patch_b = pb_fall;
            g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_n_jcc_edges = 1;
            g_stop_target = ffi::a64_label(b);
            ffi::a64_mov_imm64(b, ffi::JT0, taken);
            ffi::a64_str(b, 8, ffi::JT0, 20, ffi::RIP_OFF);
            emit_materialize(b);
            ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
            *epilogue_sites.add(*n_epi as usize) = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            *n_epi += 1;
            return 1;
        }
        if two_way == 0 {
            ffi::a64_mov_imm64(b, ffi::JT1, fall);
            ffi::a64_mov_imm64(b, ffi::JT2, taken);
            ffi::a64_csel(b, 1, ffi::JT0, ffi::JT2, ffi::JT1, ffi::A64_NE);
            ffi::a64_str(b, 8, ffi::JT0, 20, ffi::RIP_OFF);
        }
        if self_loop != 0 {
            ffi::a64_mov_imm64(b, ffi::JT1, g_self_rip);
            ffi::a64_subs_reg(b, 1, ffi::A64_ZR, ffi::JT0, ffi::JT1, 0);
            jit_internal::l0_fixed_backedge(b);
            g_stop_patch = ffi::a64_label(b);
            ffi::a64_bcond(b, ffi::A64_EQ, g_loop_entry.offset_from(g_stop_patch) as i32);
            jit_internal::l0_fixed_fallthrough(b);
            jit_internal::fpb_emit_exit_check(b);
            let edge_class = jit_internal::body_edge_pin_class();
            let body_edge = (edge_class >= 0) as c_int;
            let pb_fall = jit_internal::emit_static_chain_tail(b, fall, (fall <= g_self_rip) as c_int, body_edge, epilogue_sites, n_epi);
            g_jcc_edge[0].target_rip = fall;
            g_jcc_edge[0].patch_b = pb_fall;
            g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_n_jcc_edges = 1;
            g_stop_target = ffi::a64_label(b);
            ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
            *epilogue_sites.add(*n_epi as usize) = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            *n_epi += 1;
            return 1;
        }
        if two_way != 0 {
            let poll_fall = (fall <= g_self_rip) as c_int;
            let poll_taken = (taken <= g_self_rip) as c_int;
            let edge_class = jit_internal::body_edge_pin_class();
            let body_edge = (edge_class >= 0) as c_int;
            let to_taken = ffi::a64_label(b);
            if g_cc_cbz_reg >= 0 {
                if g_cc_cbz_nz != 0 { ffi::a64_cbnz(b, g_cc_cbz_sf, g_cc_cbz_reg, 0); }
                else { ffi::a64_cbz(b, g_cc_cbz_sf, g_cc_cbz_reg, 0); }
            } else {
                ffi::a64_bcond(b, if direct >= 0 { direct } else { ffi::A64_NE }, 0);
            }
            let pb_fall = jit_internal::emit_static_chain_tail(b, fall, poll_fall, body_edge, epilogue_sites, n_epi);
            let ltaken = ffi::a64_label(b);
            if (*to_taken & 0x7e00_0000) == 0x3400_0000 {
                ffi::a64_patch_cbz(to_taken, ltaken);
            } else {
                ffi::a64_patch_bcond(to_taken, ltaken);
            }
            let pb_taken = jit_internal::emit_static_chain_tail(b, taken, poll_taken, body_edge, epilogue_sites, n_epi);
            g_jcc_edge[0].target_rip = fall;
            g_jcc_edge[0].patch_b = pb_fall;
            g_jcc_edge[0].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[0].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_jcc_edge[1].target_rip = taken;
            g_jcc_edge[1].patch_b = pb_taken;
            g_jcc_edge[1].cond_site = cond_short_site(to_taken, 0, body_edge);
            g_jcc_edge[1].kind = if body_edge != 0 { ffi::EDGE_BODY } else { ffi::EDGE_XBLOCK };
            g_jcc_edge[1].pin_class = if body_edge != 0 { edge_class as u8 } else { 0 };
            g_n_jcc_edges = 2;
            return 1;
        }
        ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
        *epilogue_sites.add(*n_epi as usize) = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        *n_epi += 1;
        1
    }
}

unsafe fn store_code_release(site: *mut u32, value: u32) {
    unsafe { core::sync::atomic::AtomicU32::from_ptr(site).store(value, core::sync::atomic::Ordering::Release) }
}

unsafe fn store_pointer_release(site: *mut *mut c_void, value: *mut c_void) {
    unsafe { core::sync::atomic::AtomicPtr::from_ptr(site).store(value, core::sync::atomic::Ordering::Release) }
}

unsafe extern "C" fn flip_report_atexit() {
    unsafe {
        let jit = G_FLIP_ATEXIT_JIT;
        let translated = if jit.is_null() { 0 } else { (*jit).blocks_translated };
        let live = if jit.is_null() { 0 } else { (*jit).n_live };
        ffi::fprintf(
            ffi::stderr,
            c"ocerz: FLIPSTAT[%d] trips=%llu hit_ms=%.1f retires=%llu retire_ms=%.1f translated=%llu live=%zu probes=%d\n".as_ptr(),
            getpid(),
            G_FLIP_N_HIT,
            G_FLIP_NS_HIT as f64 / 1e6,
            g_flip_n_retire,
            G_FLIP_NS_RETIRE as f64 / 1e6,
            translated,
            live,
            g_n_probes,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn flip_retire_locked(
    vm: *mut ffi::OcerzVM,
    jit: *mut ffi::OcerzJit,
    blk: *mut ffi::JitBlock,
) {
    unsafe {
        let code = (*blk).code.map_or(0, |f| f as usize);
        let lo = code;
        let hi = lo.wrapping_add((*blk).code_words as usize * core::mem::size_of::<u32>());
        let in_block = |p: *const c_void| {
            let p = p as usize;
            p >= lo && p < hi
        };
        let mut idx = (*blk).live_idx;
        if idx >= (*jit).n_live || *(*jit).live.add(idx) != blk {
            idx = (*jit).n_live;
            for k in 0..(*jit).n_live {
                if *(*jit).live.add(k) == blk {
                    idx = k;
                    break;
                }
            }
        }
        if idx == (*jit).n_live {
            return;
        }
        pthread_jit_write_protect_np(0);
        if !(*blk).stop_patch.is_null()
            && (*blk).stop_insn != 0
            && *(*blk).stop_patch != (*blk).stop_insn
        {
            store_code_release((*blk).stop_patch, (*blk).stop_insn);
            sys_icache_invalidate((*blk).stop_patch.cast(), 4);
        }
        for i in 0..(*blk).n_stop_extra as usize {
            let extra = ptr::addr_of_mut!((*blk).stop_extra[i]);
            if *(*extra).site != (*extra).insn {
                store_code_release((*extra).site, (*extra).insn);
                sys_icache_invalidate((*extra).site.cast(), 4);
            }
        }
        for i in 0..(*blk).n_edges as usize {
            let edge = (*blk).edges.add(i);
            let cs = (*edge).cond_site;
            let mut is_stop = (*edge).patch_b == (*blk).stop_patch;
            for q in 0..(*blk).n_stop_extra as usize {
                if !is_stop {
                    is_stop = (*edge).patch_b == (*blk).stop_extra[q].site;
                }
            }
            if !cs.is_null() && (*edge).cond_orig != 0
                && *cs != (*edge).cond_orig && is_stop
            {
                store_code_release(cs, (*edge).cond_orig);
                sys_icache_invalidate(cs.cast(), 4);
            }
        }
        for q in 0..(*blk).n_preds {
            let pred = (*blk).preds.add(q as usize);
            let sblk = (*pred).pb;
            let i = (*pred).e as usize;
            if sblk == blk || i >= (*sblk).n_edges as usize {
                continue;
            }
            let edge = (*sblk).edges.add(i);
            let at = (*edge).patch_b;
            let fallback = (*edge).fallback_insn;
            let mut cut = 0;
            let mut cs = (*edge).cond_site;
            if !at.is_null() && fallback != 0 && *at != fallback
                && in_block(ffi::branch_word_target(at, *at).cast())
            {
                store_code_release(at, fallback);
                sys_icache_invalidate(at.cast(), 4);
                cut = 1;
            }
            if !cs.is_null() && (*edge).cond_orig != 0 && *cs != (*edge).cond_orig
                && in_block(ffi::branch_word_target(cs, *cs).cast())
            {
                store_code_release(cs, (*edge).cond_orig);
                sys_icache_invalidate(cs.cast(), 4);
                cut = 1;
            }
            if cut != 0 {
                ffi::pending_add(
                    jit_internal::jit_key((*edge).target_rip, jit_internal::blk_mode32(sblk)),
                    at,
                    (*edge).kind,
                    (*edge).pin_class,
                    if (*edge).probing != 0 { ptr::null_mut() } else { cs },
                    (*sblk).hoist_sig,
                    sblk,
                    i as c_int,
                );
            }
        }
        (*blk).n_preds = 0;
        for i in 0..ffi::g_n_ras_cells {
            let cell = *ffi::g_ras_cells.add(i);
            let value = *cell;
            if in_block(value) {
                store_pointer_release(cell, ptr::null_mut());
            }
        }
        pthread_jit_write_protect_np(1);
        let h = jit_internal::hash_key((*blk).key);
        let mut pp = ptr::addr_of_mut!((*jit).buckets[h as usize]);
        while !(*pp).is_null() && *pp != blk {
            pp = ptr::addr_of_mut!((**pp).hnext);
        }
        if *pp == blk {
            store_pointer_release(pp.cast(), (*blk).hnext.cast());
        }
        *(*jit).live.add(idx) = *(*jit).live.add((*jit).n_live - 1);
        (**(*jit).live.add(idx)).live_idx = idx;
        (*jit).n_live -= 1;
        ffi::gran_block(blk, -1);
        ffi::tc_noload_add((*blk).key);
        (*blk).retired_next = (*jit).retired;
        (*jit).retired = blk;
        for i in 0..ffi::g_ras_slot_n as usize {
            let slot = ptr::addr_of_mut!(*ffi::g_ras_slots.add(i));
            if in_block(*slot) {
                store_pointer_release(slot, ptr::null_mut());
            }
        }
        ffi::psc_retire_cols(vm, 1u32 << jit_internal::psc_col((*blk).key));
    }
}

unsafe fn flip_retire_block(vm: *mut ffi::OcerzVM, jit: *mut ffi::OcerzJit, blk: *mut ffi::JitBlock) {
    unsafe {
        let t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        g_flip_n_retire = g_flip_n_retire.wrapping_add(1);
        ffi::jl_acquire(2654);
        let live = (*blk).code.is_some();
        if live {
            flip_retire_locked(vm, jit, blk);
        }
        jit_internal::jl_release();
        if live {
            ocerz_vm_purge_jit_ras(vm);
        }
        G_FLIP_NS_RETIRE = G_FLIP_NS_RETIRE.wrapping_add(clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0));
        if live && ffi::ocerz_jit_time_xlat != 0 {
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(ffi::ocerz_jit_retire_ns))
                .fetch_add(clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0), core::sync::atomic::Ordering::Relaxed);
        }
    }
}

unsafe fn flip_decide_locked(blk: *mut ffi::JitBlock, e: c_int, tk: c_int, ft: c_int, logit: c_int) -> c_int {
    unsafe {
        let edge = (*blk).edges.add(e as usize);
        let jcc_rip = (*edge).jcc_rip;
        let mut flip = (tk >= 2 * ft && tk >= (1 << (PROBE_BIT - 1))) as c_int;
        let i = jit_internal::flip_find(jcc_rip, 1);
        if i < 0 {
            flip = 0;
        } else if g_flip[i as usize].state != ffi::FLIP_NONE {
            flip = (g_flip[i as usize].state == ffi::FLIP_DECIDED_INV) as c_int;
        } else {
            g_flip[i as usize].state = if flip != 0 { ffi::FLIP_DECIDED_INV as u8 } else { ffi::FLIP_DECIDED_ORIG as u8 };
        }
        if logit != 0 {
            ffi::fprintf(
                ffi::stderr,
                c"ocerz: FLIP[%d] blk=%#llx jcc=%#llx taken=%d fall=%d -> %s\n".as_ptr(),
                getpid(),
                jit_internal::blk_rip(blk),
                jcc_rip,
                tk,
                ft,
                if flip != 0 { c"invert".as_ptr() } else { c"keep".as_ptr() },
            );
        }
        (*edge).probing = 0;
        if flip == 0 {
            let pf = (*blk).prof.add(((*edge).side - 1) as usize);
            let norearm = ptr::addr_of_mut!(G_NO_FLIP_REARM);
            if *norearm < 0 {
                *norearm = (!getenv(c"OCERZ_NO_FLIP_REARM".as_ptr()).is_null()) as c_int;
            }
            let watch = *norearm == 0 && (*pf).rearms < FLIP_REARMS;
            (*pf).ft_word = *(*pf).ft_site;
            (*pf).tk_word = *(*pf).tk_trip;
            let tk_word = if watch {
                ((*pf).tk_word & !((1u32 << 31) | (0x1fu32 << 19))) | (WATCH_BIT << 19)
            } else {
                A64_NOP_WORD
            };
            if watch {
                (*pf).taken = 0;
                (*edge).probing = 2;
            }
            pthread_jit_write_protect_np(0);
            store_code_release((*pf).ft_site, A64_NOP_WORD);
            store_code_release((*pf).tk_trip, tk_word);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate((*pf).ft_site.cast(), 4);
            sys_icache_invalidate((*pf).tk_trip.cast(), 4);
        }
        flip
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn flip_side_hit(
    vm: *mut ffi::OcerzVM,
    jit: *mut ffi::OcerzJit,
    cpu: *mut ffi::OcerzCPU,
) {
    unsafe {
        let blk = (*cpu).side_blk.cast::<ffi::JitBlock>();
        let k = (*cpu).side_idx;
        (*cpu).side_blk = ptr::null_mut();
        if k == -2 && !blk.is_null() {
            jit_internal::lowhoist_mark((*blk).key);
            flip_retire_block(vm, jit, blk);
            return;
        }
        if k == -3 && !blk.is_null() {
            if !getenv(c"OCERZ_FLIPLOG".as_ptr()).is_null() {
                ffi::fprintf(
                    ffi::stderr,
                    c"ocerz: FLIP[%d] blk=%#llx entered with another x87 TOP -> translate without it\n".as_ptr(),
                    getpid(),
                    jit_internal::blk_rip(blk),
                );
            }
            jit_internal::mark_add(ptr::addr_of_mut!(ffi::g_x87spec_marks), (*blk).key);
            flip_retire_block(vm, jit, blk);
            return;
        }
        if blk.is_null() || (*blk).prof.is_null() || k < 0 || k >= ffi::SIDE_MAX as c_int {
            return;
        }
        let mut e = -1;
        for i in 0..(*blk).n_edges as usize {
            if (*(*blk).edges.add(i)).side as c_int == k + 1 {
                e = i as c_int;
                break;
            }
        }
        if e < 0 || (*(*blk).edges.add(e as usize)).probing == 0 {
            return;
        }
        if G_FLIPLOG < 0 {
            G_FLIPLOG = (!getenv(c"OCERZ_FLIPLOG".as_ptr()).is_null()) as c_int;
            if G_FLIPLOG != 0 {
                G_FLIP_ATEXIT_JIT = jit;
                atexit(flip_report_atexit);
            }
        }
        if (*(*blk).edges.add(e as usize)).probing == 2 {
            let wp = (*blk).prof.add(k as usize);
            ffi::jl_acquire(2730);
            if (*blk).live_idx >= (*jit).n_live
                || *(*jit).live.add((*blk).live_idx) != blk
                || (*(*blk).edges.add(e as usize)).probing != 2
            {
                jit_internal::jl_release();
                return;
            }
            let fi = jit_internal::flip_find((*(*blk).edges.add(e as usize)).jcc_rip, 0);
            if fi >= 0 {
                g_flip[fi as usize].state = ffi::FLIP_NONE as u8;
            }
            (*wp).taken = 0;
            (*wp).ft = 0;
            (*wp).windows = 0;
            (*wp).rearms = (*wp).rearms.wrapping_add(1);
            pthread_jit_write_protect_np(0);
            store_code_release((*wp).ft_site, (*wp).ft_word);
            store_code_release((*wp).tk_trip, (*wp).tk_word);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate((*wp).ft_site.cast(), 4);
            sys_icache_invalidate((*wp).tk_trip.cast(), 4);
            (*(*blk).edges.add(e as usize)).probing = 1;
            jit_internal::jl_release();
            if G_FLIPLOG != 0 {
                ffi::fprintf(
                    ffi::stderr,
                    c"ocerz: FLIP[%d] blk=%#llx jcc=%#llx kept side hot again -> probe (%d)\n".as_ptr(),
                    getpid(),
                    jit_internal::blk_rip(blk),
                    (*(*blk).edges.add(e as usize)).jcc_rip,
                    (*wp).rearms as c_int,
                );
            }
            return;
        }
        let t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        G_FLIP_N_HIT = G_FLIP_N_HIT.wrapping_add(1);
        let pf = (*blk).prof.add(k as usize);
        let tk = (*pf).taken as c_int;
        let ft = (*pf).ft as c_int;
        let verdict = (tk >= 2 * ft && tk >= (1 << (PROBE_BIT - 1))) as c_int;
        ffi::jl_acquire(2759);
        if (*blk).live_idx >= (*jit).n_live || *(*jit).live.add((*blk).live_idx) != blk {
            jit_internal::jl_release();
            return;
        }
        if (*pf).windows == 0 || ((*pf).prev as c_int != verdict && (*pf).windows < 3) {
            (*pf).prev = verdict as u8;
            (*pf).windows = (*pf).windows.wrapping_add(1);
            (*pf).taken = 0;
            (*pf).ft = 0;
            jit_internal::jl_release();
            return;
        }
        let flip = flip_decide_locked(blk, e, tk, ft, G_FLIPLOG);
        if flip == 0 {
            ffi::chain_edge_now(jit, blk, e);
        }
        jit_internal::jl_release();
        G_FLIP_NS_HIT = G_FLIP_NS_HIT.wrapping_add(clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0));
        if G_NO_FLIP_NORETIRE < 0 {
            G_NO_FLIP_NORETIRE =
                (!getenv(c"OCERZ_FLIP_NORETIRE".as_ptr()).is_null()) as c_int;
        }
        if flip != 0 && G_NO_FLIP_NORETIRE == 0 {
            flip_retire_block(vm, jit, blk);
        }
    }
}
