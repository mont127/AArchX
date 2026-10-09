#![allow(unsafe_op_in_unsafe_fn, non_camel_case_types, non_snake_case, dead_code, unused_mut, unused_assignments, unused_variables)]
//! ---- bridged calls ----
//! In native mode a guest call into a system library lands on a synthesized
//! stub, `mov r11d, <export id>` then `jmp qword [rip + slot]`, whose slot holds
//! the one trap address every export shares (vdylib.h).  Taken as an ordinary
//! indirect jump, that address has no translation: the block leaves, the run
//! loop finds the rip in the trap window, the interpreter's trap check hands it
//! to the bridge, and the return address comes back in through the dispatcher
//! with the host shadow stack abandoned, so the caller's own ret leaves once
//! more.  When a block ends in that pair and the slot holds the trap address as
//! the block is translated, the jump becomes a call to ocerz_vdylib_fastcall made
//! from inside the block, followed by the ret the export's own code would have
//! ended with.
//!
//! The emitted code loads the slot again and compares it with the trap address,
//! so a guest that rewrote its slot takes the plain indirect jump emitted right
//! after.  It also measures the host stack below the frame base and takes the
//! plain jump past 64 KB, because the native call now runs underneath the shadow
//! of every guest call still open, where the trap path ran it from the top of
//! the run loop.  Then every guest register is spilled, the helper performs the
//! crossing the trap would have performed, the registers are filled again, and a
//! zero answer runs the ret: the shadow's return address is compared with the
//! rip the crossing left, and a match returns into the caller's continuation
//! with a real ret, which keeps the return predictor aligned.  Any other answer
//! is the step code plus one and leaves through the epilogue.  The helper
//! answers zero only when the crossing returned the way a function does - rip
//! equal to the word just popped, rsp eight or sixteen higher - with no exit,
//! interrupt, suspension or interp_once pending and no translation retired
//! while it ran, since a retired continuation may be translated from bytes the
//! crossing just rewrote.  A delivered signal fails the first test.
//!
//! A host-stack RAS entry lives only until the next trip out of translated
//! code, because every exit resets sp to the frame base, and a return whose
//! entry was lost that way used to leave the frame through the epilogue: the
//! dispatcher, the prologue, and the reset discarding every remaining entry, so
//! the whole call chain then missed on its way up.  C++ engine code makes a call
//! every ten instructions and takes a trip every few hundred thousand blocks,
//! which put Adobe AIR's main thread at 36 million misses a second, one return
//! in seven.  A miss now stays in the body: the popped return address goes to
//! the same per-site cache and inline hash probe an indirect call uses, and only
//! an address no block answers to leaves the frame.  A pop that finds the
//! sentinel pair pushes it back first, since the saved frame registers sit right
//! above it.  Brawlhalla's translated throughput rose by a third; the kernels the
//! host RAS was built for (memcpy, str, leafcall, icall) measure the same.
//! OCERZ_PERFSTAT splits misses into an empty stack, a null entry and a
//! mismatched address and names the ret sites that miss most.  On R.E.P.O.'s
//! menu 183.6 of 184.1 million misses were the empty stack, which costs a cache
//! probe rather than a trip out.
//!
//! A chain is a b patched into the exiting block, and a b reaches 128 MB.  The
//! arena is 1 GB and filled front to back, so a caller translated late chains
//! to a callee translated early only if fewer than 128 MB of code came between
//! them; past that the patch was dropped and the edge left through the epilogue
//! on every execution.  That is the shape of a game: the engine's runtime
//! helpers are translated in the first minute and the script code that calls
//! them keeps being generated afterwards, so its hottest edges were exactly the
//! ones dropped.  A 256 KB veneer pool is carved out of the arena every 64 MB,
//! and a patch that does not reach is pointed at a veneer in the nearest pool
//! instead: ldr x15 from the literal that follows, br x15, which reaches
//! anything.  x15 is the computed-address temp, never live across a block edge.
//! Brawlhalla's main thread fell from a saturated core to 71%, while doing more
//! work per second than before.
//!
//! The same distance limit applies to an alignment hotpatch, which puts a
//! branch at the faulting access and a stub at the end of the arena.  When the
//! end is out of reach the stub goes into the nearest veneer pool instead, so
//! the patch never falls back to retranslating the block.  A store whose data
//! register is 31 is patched as well: in store and shift encodings that is the
//! zero register, and it is how a guest store of an immediate zero comes out.
//! Refusing both sent Discord's V8 through 5,391 retranslations in its first
//! ten seconds, most of them the same few stores; it now takes none.
//!
//! A first commpage or alignment fault in a block retires only that block, and
//! a block starting at the faulting instruction, through the predecessor lists
//! the branch-flip retire already trusts.  A range invalidation scans every
//! live block and every chain edge, about 140,000 blocks at Electron's startup,
//! and one per fault made retiring the largest cost on Discord's main thread.
//! A repeat fault still takes the range invalidation, which feeds the churn
//! accounting, and OCERZ_FAULT_INV_RANGE=1 restores it for the first fault too.
//!
//! OCERZ_FPS=1 counts frames in cache mode: the block that begins at the Intel
//! cache's CGLFlushDrawable gets four instructions at its head that increment a
//! counter, and a thread prints the rate to stderr once a second.  The guest
//! sees nothing of it.  It is how a game's frame rate under ocerz is measured
//! when Steam's overlay counter, which relies on dyld interposing, is not there.
//!
//! OCERZ_TRIPSTAT=1 counts the times translated code leaves to the dispatcher,
//! which ocerz_jit_step sees once each, and samples one in 64 of their guest
//! destinations; every ten seconds a thread prints trips per second and the
//! twenty commonest destinations with the state of the block at each.  At
//! Brawlhalla's menu it showed 6.6 million trips a second, ten destinations
//! making 92% of them.  It costs one atomic increment per trip.
//!
//! A memory access whose address is not a known stack slot pays a commpage
//! guard before it - the commpage lives elsewhere on the host, so the address
//! is compared against that range and redirected if it falls inside - and, in
//! ordered mode, a granule test before the ldapr or stlr, since those fault on
//! an access that crosses a granule.  For a rip-relative or absolute operand the
//! address is a constant of the translation, so both questions are answered
//! when the block is built: the guard is skipped or becomes the plain
//! redirect, and an aligned constant address goes straight to the ordered
//! load or store.  A global load or store costs four host words instead of
//! fourteen in plain mode and eighteen in ordered mode; it is the most common
//! form there is in compiled code.  The known-address note is set by the guard
//! and consumed by the very next ordered access, and building any other
//! effective address clears it, so it can never describe an address other than
//! the one just guarded.
//!
//! Eleven libSystem exports do not cross at all.  strlen, strnlen, strcmp,
//! strncmp, memcmp, bcmp, strchr, memchr, memcpy, memmove and memset are called
//! constantly, do very little, and cost several times their own work to reach
//! through a crossing, so src/leaf.s holds arm64 versions that read rdi, rsi and
//! rdx from the registers full pinning keeps them in and leave rax in its own
//! (ocerz_vdylib_leaf names the routine for an export id).  The block checks its
//! slot as before, branches to the routine with nothing spilled, pops the return
//! address and runs the same ret.  A routine that writes guest memory is only
//! used for lengths up to the limit the lookup gives, may decline in x9, and has
//! the retire count read before and after it, a change sending the return through
//! the dispatcher for the reason the helper's own check gives; any of the three
//! falls through to the ordinary fast call emitted next.  A fault inside a
//! routine is attributed to the branch that reached it (ocerz_leaf_site), so the
//! guest sees a fault at the stub with its registers exact, where a fault inside
//! a crossing ends the process.  The routines are reached through a copy made
//! at the start of the code arena, so the branch is a direct one.
//!
//! Cache mode uses the same routines from the other side.  There the guest's
//! strlen is Apple's x86 code, and when a block starts at the exported entry of
//! one of ten of those functions (ocerz_dyldapi_leaf_entry) the same call and
//! ret are emitted ahead of its first instruction, with the translation of the
//! x86 code following as what runs when the routine declines.  No length limit
//! applies, since what it would fall back to is slower at every length.  A
//! fault inside a routine is not delivered from there: src/vm.c makes the
//! routine decline, and the x86 code takes the same fault with every detail
//! right.  A block with a branch back to its own start is left alone, since
//! its lanes may be live where the call would leave.
//! OCERZ_NO_LEAF_INPLACE=1 turns the routines off in both modes, and so do
//! OCERZ_BRIDGESTAT and OCERZ_BRIDGELOG, whose counts and lines come from the
//! crossing.
//!
//! XMM registers are spilled by contract rather than wholesale.  System V makes
//! every xmm register volatile across a call, so for an ordinary export only the
//! argument registers its signature reads are stored first and only the result
//! registers are loaded afterwards; the rest keep whatever the host left in
//! their pinned registers, which an x86 callee was equally free to leave.
//! __tlv_bootstrap and ___chkstk_darwin promise every register back, and so
//! does an export whose record gives no usable signature, and those store and
//! load all sixteen.  A crossing that leaves loads all sixteen from the cpu,
//! where a signal frame may have replaced them.  OCERZ_NO_BRIDGE_FASTCALL=1
//! keeps the trap path everywhere.  fork and vfork keep it always
//! (ocerz_vdylib_trap_only): the fork child starts without the parent's
//! translations, and a crossing made from inside a block would return into code
//! the child no longer has, so their stubs leave the block and trap from the
//! dispatcher, the way the fork syscall does.
//!
//!
//! The Wine layout's slot is rsp - 8 plus the stack delta in x0, as push's; rsp moves once the store is done.
//!
//! call $+5 is how 32-bit code reads eip; its pop never meets a ret.
//!
//! The return address goes through the same per-site cache and hash probe
//! an indirect jump does, keyed as a 32-bit block, instead of out to the
//! dispatcher on every return; first the host shadow, when the call that
//! pushed it went through m32_ras_push.
//!
//! The sentinel pair sits on the frame's saved registers: put it back.
//!
//! The interpreter side of an x87 run: instructions idx..last, stopping at the first that does not step.
//!
//! A 32-bit call and its ret through the host shadow stack, as 64-bit ones go:
//! the call pushes {return address tagged JIT_KEY_M32, host continuation} and
//! bl's (or, indirect, blr's) into the callee's body, and the ret compares the
//! address it popped with the shadow's and returns with a real ret, which the
//! return predictor has seen coming.  The tag keeps a 64-bit ret from taking a
//! 32-bit entry and the reverse.  A mismatch, a stale entry or an empty shadow
//! takes the per-site cache as before.  OCERZ_NO_M32_RAS=1 turns it off.
//!
//! Pushes the shadow pair for a 32-bit call returning to retaddr; the adr is patched to the continuation.
//!
//! Set for one emit_indirect_tail whose target register holds a 32-bit block's key.
//!
//! A 32-bit indirect call or jump: the target, a register or a dword in memory,
//! is zero-extended into JT1 and keyed as a 32-bit block, a call pushes its
//! return address as emit_call_ret32 does, and both leave through the per-site
//! cache and hash probe.  32-bit Windows code calls every import as call dword
//! [IAT], so each of those went out to the dispatcher before.  The target is
//! read before anything moves, so a fault there leaves the instruction unstarted.
//!
//! `ocerz_jit_exec_one` and `ocerz_jit_exec_one_at` live in the C shim. Rust
//! cannot capture `__builtin_return_address(0)`, which the fault and park
//! paths use to identify their caller.

use crate::ffi::*;
use crate::jit_internal::*;
use libc::*;
use core::sync::atomic::{AtomicU64, Ordering};

unsafe extern "C" {
    fn __assert_rtn(function: *const ::core::ffi::c_char, file: *const ::core::ffi::c_char, line: ::core::ffi::c_int, expression: *const ::core::ffi::c_char) -> !;
    fn clock_gettime_nsec_np(clock_id: clockid_t) -> uint64_t;
    fn sys_icache_invalidate(start: *const ::core::ffi::c_void, len: size_t);
}


type uint8_t = u8;
type uint16_t = u16;
type uint32_t = u32;
type uint64_t = u64;
type int8_t = i8;
type int16_t = i16;
type int32_t = i32;
type int64_t = i64;
type uintptr_t = usize;
type size_t = usize;
pub type C2RustUnnamed = ::core::ffi::c_int;
pub type C2RustUnnamed_0 = ::core::ffi::c_uint;
pub type C2RustUnnamed_1 = ::core::ffi::c_uint;
pub type C2RustUnnamed_7 = ::core::ffi::c_uint;
pub type C2RustUnnamed_8 = ::core::ffi::c_uint;
pub type C2RustUnnamed_9 = ::core::ffi::c_uint;
pub type C2RustUnnamed_10 = ::core::ffi::c_uint;
pub type C2RustUnnamed_11 = ::core::ffi::c_uint;
pub type C2RustUnnamed_12 = ::core::ffi::c_uint;
#[derive(Copy, Clone)]
#[repr(C)]
pub struct C2RustUnnamed_13 {
    pub sites: [*mut uint32_t; 3],
    pub nsites: ::core::ffi::c_int,
    pub insn: *const X86Insn,
    pub back: *mut uint32_t,
    pub pre: uint32_t,
    pub l0: [int8_t; 16],
    pub l0_dbl: [uint8_t; 16],
    pub l0_dirty: uint16_t,
    pub yc_dirty: uint16_t,
}
pub const OCERZ_EUNSUP: C2RustUnnamed = -7;
pub const OCERZ_R11: C2RustUnnamed_0 = 11;
pub const OCERZ_RDI: C2RustUnnamed_0 = 7;
pub const OCERZ_RSI: C2RustUnnamed_0 = 6;
pub const OCERZ_RSP: C2RustUnnamed_0 = 4;
pub const OCERZ_RDX: C2RustUnnamed_0 = 2;
pub const OCERZ_RAX: C2RustUnnamed_0 = 0;
pub const OCERZ_SEG_NONE: C2RustUnnamed_1 = 0;
pub const OCERZ_OPK_IMM: OcerzOpKind = 5;
pub const OCERZ_OPK_MEM: OcerzOpKind = 4;
pub const OCERZ_OPK_REG: OcerzOpKind = 1;
pub const OCERZ_OP_COUNT: OcerzOp = 559;
pub const OCERZ_OP_SYSCALL: OcerzOp = 84;
pub const OCERZ_OP_RET: OcerzOp = 60;
pub const OCERZ_OP_CALL: OcerzOp = 59;
pub const OCERZ_OP_JMP: OcerzOp = 53;
pub const OCERZ_OP_TEST: OcerzOp = 26;
pub const OCERZ_OP_POP: OcerzOp = 9;
pub const OCERZ_OP_PUSH: OcerzOp = 8;
pub const OCERZ_OP_MOVSXD: OcerzOp = 4;
pub const OCERZ_OP_MOV: OcerzOp = 1;
pub const OCERZ_STEP_PROFILE: OcerzStep = 4;
pub const OCERZ_STEP_OK: OcerzStep = 0;
pub const A64_HI: C2RustUnnamed_7 = 8;
pub const A64_CC: C2RustUnnamed_7 = 3;
pub const A64_CS: C2RustUnnamed_7 = 2;
pub const A64_NE: C2RustUnnamed_7 = 1;
pub const OCERZ_MODE_NATIVE: C2RustUnnamed_8 = 1;
pub const OCERZ_MODE_CACHE: C2RustUnnamed_8 = 0;
pub const _CLOCK_UPTIME_RAW: clockid_t = 8;
pub const EDGE_BODY: C2RustUnnamed_9 = 2;
pub const EDGE_XBLOCK: C2RustUnnamed_9 = 0;
pub const TCR_PSC: C2RustUnnamed_10 = 10;
pub const TCR_RASSLOT: C2RustUnnamed_10 = 9;
pub const TCR_INSN: C2RustUnnamed_10 = 7;
pub const TCR_BLK: C2RustUnnamed_10 = 6;
pub const TCR_LEAF: C2RustUnnamed_10 = 4;
pub const TCR_BUCKETS: C2RustUnnamed_10 = 3;
pub const TCR_SYM: C2RustUnnamed_10 = 1;
pub const TCS_EXEC_RUN_AT: C2RustUnnamed_11 = 6;
pub const TCS_RETIRE_COUNT: C2RustUnnamed_11 = 5;
pub const TCS_EXEC_ONE_AT: C2RustUnnamed_11 = 3;
pub const TCS_EXEC_ONE: C2RustUnnamed_11 = 2;
pub const TCS_RAS_PUSH: C2RustUnnamed_11 = 1;
pub const JTA: C2RustUnnamed_12 = 15;
pub const JTU: C2RustUnnamed_12 = 14;
pub const JTT: C2RustUnnamed_12 = 13;
pub const JTF: C2RustUnnamed_12 = 12;
pub const JT2: C2RustUnnamed_12 = 11;
pub const JT1: C2RustUnnamed_12 = 10;
pub const JT0: C2RustUnnamed_12 = 9;
pub const __DARWIN_NULL: *mut ::core::ffi::c_void = ::core::ptr::null_mut::<
    ::core::ffi::c_void,
>();
pub const OCERZ_RAS_SIZE: ::core::ffi::c_int = 256 as ::core::ffi::c_int;
pub const OCERZ_COMMPAGE_LO: ::core::ffi::c_ulonglong = 0x7fffffe00000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_COMMPAGE_HI: ::core::ffi::c_ulonglong = 0x7fffffe04000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_LOW_LIMIT: ::core::ffi::c_ulonglong = 0x300000000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_NULL_LIMIT: ::core::ffi::c_ulonglong = 0x10000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_TOP_LO: ::core::ffi::c_ulonglong = 0x7ffffe000000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_TOP_HI: ::core::ffi::c_ulonglong = 0x7fffffe00000
    as ::core::ffi::c_ulonglong;



pub const A64_ZR: ::core::ffi::c_int = 31 as ::core::ffi::c_int;
pub const OCERZ_DYLDAPI_LO: ::core::ffi::c_ulonglong = 0xdda00000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_DYLDAPI_HI: ::core::ffi::c_ulonglong = 0xdda10000
    as ::core::ffi::c_ulonglong;
pub const OCERZ_BRIDGE_OFF: ::core::ffi::c_int = 0x8000 as ::core::ffi::c_int;
pub const JIT_HASH_BITS: ::core::ffi::c_int = 20 as ::core::ffi::c_int;
pub const JIT_HASH_SIZE: ::core::ffi::c_uint = (1 as ::core::ffi::c_uint)
    << JIT_HASH_BITS;
pub const JIT_HASH_MASK: ::core::ffi::c_uint = JIT_HASH_SIZE
    .wrapping_sub(1 as ::core::ffi::c_uint);
pub const JIT_MAX_BLOCK_INSNS: ::core::ffi::c_int = 256 as ::core::ffi::c_int;
pub const JIT_KEY_M32: ::core::ffi::c_ulonglong = (1 as ::core::ffi::c_ulonglong)
    << 63 as ::core::ffi::c_int;
pub const TC_RELOC_MAX: ::core::ffi::c_int = 1024 as ::core::ffi::c_int;
pub const RASLIT_MAX: ::core::ffi::c_int = 96 as ::core::ffi::c_int;
pub const JGB: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
pub const PS_RETSITE_N: ::core::ffi::c_int = 65536 as ::core::ffi::c_int;







pub const JRET_GUEST: ::core::ffi::c_int = 27 as ::core::ffi::c_int;
pub const JRET_HOST: ::core::ffi::c_int = 28 as ::core::ffi::c_int;



pub const BRIDGE_FAST_DEPTH_MAX_K: ::core::ffi::c_int = 16 as ::core::ffi::c_int;

pub const OOLSLOW_MAX: ::core::ffi::c_int = 32 as ::core::ffi::c_int;


























































#[unsafe(no_mangle)]
pub static mut g_no_ras: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_ea_plain: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_pe_real: [JitBlock_JitPushElide; 48] = [JitBlock_JitPushElide {
    ci: 0,
    rj: 0,
    ra: 0,
}; 48];
#[unsafe(no_mangle)]
pub static mut g_n_pe_real: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_promo_real: [JitPromo; 48] = [JitPromo { pi: 0, qi: 0, hreg: 0 }; 48];
#[unsafe(no_mangle)]
pub static mut g_n_promo_real: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_pe_insns: *const X86Insn = ::core::ptr::null::<X86Insn>();
#[unsafe(no_mangle)]
pub static mut g_callout_seq: ::core::ffi::c_ulonglong = 0;
#[unsafe(no_mangle)]
pub static mut g_no_chain: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_chain_target: uint64_t = 0;
#[unsafe(no_mangle)]
pub static mut g_chain_epi: *mut uint32_t = ::core::ptr::null::<uint32_t>()
    as *mut uint32_t;
#[unsafe(no_mangle)]
pub static mut g_chain_keeps_jgb: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_const_lit(
    mut b: *mut A64Buf,
    mut rd: ::core::ffi::c_int,
    mut v: uint64_t,
) {
    let mut parts: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while k < 4 as ::core::ffi::c_int {
        parts
            += (v >> 16 as ::core::ffi::c_int * k & 0xffff as uint64_t != 0 as uint64_t)
                as ::core::ffi::c_int;
        k += 1;
    }
    static mut off: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if off < 0 as ::core::ffi::c_int {
        off = if !libc::getenv(
                b"OCERZ_NO_CONST_LIT\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    if parts < 3 as ::core::ffi::c_int || g_n_raslit >= RASLIT_MAX || off != 0 {
        a64_mov_imm64(b, rd, v);
        return;
    }
    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).site = a64_label(b);
    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).retaddr = v;
    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).kind = 1 as ::core::ffi::c_int;
    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).tcr = 0 as ::core::ffi::c_int;
    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).rt = rd;
    g_n_raslit += 1;
    a64_emit32(b, 0x58000000 as uint32_t | (rd & 31 as ::core::ffi::c_int) as uint32_t);
}
#[unsafe(no_mangle)]
pub static mut g_body_entry: *mut uint32_t = ::core::ptr::null::<uint32_t>()
    as *mut uint32_t;
#[unsafe(no_mangle)]
pub static mut g_push_fix: [uint32_t; 256] = [0; 256];
#[unsafe(no_mangle)]
pub static mut g_n_push_fix: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_push_entry: *const uint32_t = ::core::ptr::null::<uint32_t>();
#[unsafe(no_mangle)]
pub static mut g_stop_extra: [JitState_g_stop_extra; 6] = [JitState_g_stop_extra {
    site: ::core::ptr::null::<uint32_t>() as *mut uint32_t,
    target: ::core::ptr::null::<uint32_t>() as *mut uint32_t,
}; 6];
#[unsafe(no_mangle)]
pub static mut g_n_stop_extra: ::core::ffi::c_int = 0;
unsafe extern "C" fn stop_extra_add(mut site: *mut uint32_t, mut target: *mut uint32_t) {
    if g_n_stop_extra < 6 as ::core::ffi::c_int {
        (*(&raw mut g_stop_extra)).get_unchecked_mut(g_n_stop_extra as usize).site = site;
        (*(&raw mut g_stop_extra)).get_unchecked_mut(g_n_stop_extra as usize).target = target;
        g_n_stop_extra += 1;
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn stop_retarget(
    mut insn: uint32_t,
    mut site: *const uint32_t,
    mut target: *const uint32_t,
) -> uint32_t {
    let mut off: int32_t = target.offset_from(site) as ::core::ffi::c_long as int32_t;
    if insn & 0xfc000000 as uint32_t == 0x14000000 as uint32_t {
        return 0x14000000 as uint32_t | off as uint32_t & 0x3ffffff as uint32_t;
    }
    if insn & 0xff000010 as uint32_t == 0x54000000 as uint32_t {
        return insn & 0xff00001f as uint32_t
            | (off as uint32_t & 0x7ffff as uint32_t) << 5 as ::core::ffi::c_int;
    }
    if insn & 0x7e000000 as uint32_t == 0x34000000 as uint32_t {
        return insn & 0xff00001f as uint32_t
            | (off as uint32_t & 0x7ffff as uint32_t) << 5 as ::core::ffi::c_int;
    }
    if insn & 0x7e000000 as uint32_t == 0x36000000 as uint32_t {
        return insn & 0xfff8001f as uint32_t
            | (off as uint32_t & 0x3fff as uint32_t) << 5 as ::core::ffi::c_int;
    }
    return 0x14000000 as uint32_t | off as uint32_t & 0x3ffffff as uint32_t;
}
#[unsafe(no_mangle)]
pub static mut g_call_edge: [JitState_g_call_edge; 2] = [JitState_g_call_edge {
    target_rip: 0,
    patch_b: ::core::ptr::null::<uint32_t>() as *mut uint32_t,
    kind: 0,
    pin_class: 0,
}; 2];
#[unsafe(no_mangle)]
pub static mut g_n_call_edges: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut ps_shapes: [[[::core::ffi::c_char; 96]; 3]; OCERZ_OP_COUNT as usize] = [
    [
        [
            0 as ::core::ffi::c_int as ::core::ffi::c_char,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ],
        [0; 96],
        [0; 96],
    ],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
    [[0; 96]; 3],
];
unsafe extern "C" fn ps_note_shape(mut insn: *const X86Insn) {
    let mut o: ::core::ffi::c_uint = (*insn).op as ::core::ffi::c_uint;
    if o >= OCERZ_OP_COUNT as ::core::ffi::c_int as ::core::ffi::c_uint {
        return;
    }
    let mut buf: [::core::ffi::c_char; 96] = [0; 96];
    ocerz_format_insn(
        insn,
        &raw mut buf as *mut ::core::ffi::c_char,
        ::core::mem::size_of::<[::core::ffi::c_char; 96]>() as size_t,
    );
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < 3 as ::core::ffi::c_int {
        if *(*(&raw mut ps_shapes)).get_unchecked_mut(o as usize).get_unchecked_mut(i as usize).get_unchecked_mut(0 as ::core::ffi::c_int as usize)
            as ::core::ffi::c_int == 0 as ::core::ffi::c_int
        {
            snprintf(
                &raw mut *(&raw mut *(&raw mut ps_shapes
                    as *mut [[::core::ffi::c_char; 96]; 3])
                    .offset(o as isize) as *mut [::core::ffi::c_char; 96])
                    .offset(i as isize) as *mut ::core::ffi::c_char,
                ::core::mem::size_of::<[::core::ffi::c_char; 96]>() as size_t,
                b"%s\0" as *const u8 as *const ::core::ffi::c_char,
                &raw mut buf as *mut ::core::ffi::c_char,
            );
            return;
        }
        if strcmp(
            &raw mut *(&raw mut *(&raw mut ps_shapes
                as *mut [[::core::ffi::c_char; 96]; 3])
                .offset(o as isize) as *mut [::core::ffi::c_char; 96])
                .offset(i as isize) as *mut ::core::ffi::c_char,
            &raw mut buf as *mut ::core::ffi::c_char,
        ) == 0 as ::core::ffi::c_int
        {
            return;
        }
        i += 1;
    }
}
#[unsafe(no_mangle)]
pub static ps_ops: [AtomicU64; OCERZ_OP_COUNT as usize] = [const { AtomicU64::new(0) }; OCERZ_OP_COUNT as usize];
#[unsafe(no_mangle)]
pub static ps_slow_insns: AtomicU64 = AtomicU64::new(0);
#[unsafe(no_mangle)]
pub static ps_shape: [[AtomicU64; 2]; 9] = [const { [const { AtomicU64::new(0) }; 2] }; 9];

#[unsafe(no_mangle)]
pub static mut ps_ras_miss: ::core::ffi::c_ulonglong = 0;
#[unsafe(no_mangle)]
pub static mut ps_ras_null: ::core::ffi::c_ulonglong = 0;
#[unsafe(no_mangle)]
pub static mut ps_ras_sentinel: ::core::ffi::c_ulonglong = 0;
#[unsafe(no_mangle)]
pub static mut ps_ras_stale: ::core::ffi::c_ulonglong = 0;
#[inline(never)]
#[cold]
unsafe extern "C" fn jit_trace_one(mut insn: *const X86Insn) {
    let mut buf: [::core::ffi::c_char; 128] = [0; 128];
    ocerz_format_insn(
        insn,
        &raw mut buf as *mut ::core::ffi::c_char,
        ::core::mem::size_of::<[::core::ffi::c_char; 128]>() as size_t,
    );
    fprintf(
        crate::log::stderr(),
        b"ocerz: %#llx: %s\n\0" as *const u8 as *const ::core::ffi::c_char,
        (*insn).rip as ::core::ffi::c_ulonglong,
        &raw mut buf as *mut ::core::ffi::c_char,
    );
}
#[inline(never)]
#[cold]
unsafe extern "C" fn jit_perfstat_one(insn: *const X86Insn) {
    ps_slow_insns.fetch_add(1, Ordering::SeqCst);
    let op = (*insn).op as u32;
    if (op as usize) < OCERZ_OP_COUNT as usize {
        ps_ops.get_unchecked(op as usize).fetch_add(1, Ordering::SeqCst);
        if ps_ops.get_unchecked(op as usize).load(Ordering::SeqCst) & 0xff == 1 {
            ps_note_shape(insn);
        }
    }
    if op == OCERZ_OP_PUSH as u32 || op == OCERZ_OP_POP as u32 {
        let operand = *(*insn).ops.get_unchecked(0);
        let simple = (*insn).opsize == 8
            && (*insn).seg as u32 == OCERZ_SEG_NONE
            && ((operand.kind as u32 == OCERZ_OPK_REG && operand.high8 == 0)
                || operand.kind as u32 == OCERZ_OPK_IMM);
        let shape = if op == OCERZ_OP_PUSH as u32 { 0 } else { 1 };
        ps_shape.get_unchecked(shape).get_unchecked(if simple { 0 } else { 1 }).fetch_add(1, Ordering::SeqCst);
    } else if op == OCERZ_OP_TEST as u32 || op == OCERZ_OP_MOVSXD as u32 {
        let operand = *(*insn).ops.get_unchecked(0);
        let simple = (operand.size == 4 || operand.size == 8)
            && operand.kind as u32 == OCERZ_OPK_REG
            && operand.high8 == 0;
        let shape = if op == OCERZ_OP_TEST as u32 { 2 } else { 3 };
        ps_shape.get_unchecked(shape).get_unchecked(if simple { 0 } else { 1 }).fetch_add(1, Ordering::SeqCst);
    } else if op == OCERZ_OP_CALL as u32 || op == OCERZ_OP_RET as u32 {
        if op == OCERZ_OP_CALL as u32 {
            ps_shape.get_unchecked(4).get_unchecked(if (*insn).ops.get_unchecked(0).kind as u32 == OCERZ_OPK_IMM { 0 } else { 1 })
                .fetch_add(1, Ordering::SeqCst);
        } else {
            ps_shape.get_unchecked(5).get_unchecked(if (*insn).nops == 0 { 0 } else { 1 })
                .fetch_add(1, Ordering::SeqCst);
        }
    } else if op == OCERZ_OP_JMP as u32 {
        let direct = (*insn).ops.get_unchecked(0).kind as u32 == OCERZ_OPK_IMM;
        ps_shape.get_unchecked(6).get_unchecked(if direct { 0 } else { 1 }).fetch_add(1, Ordering::SeqCst);
        if !direct {
            let is_reg = (*insn).ops.get_unchecked(0).kind as u32 == OCERZ_OPK_REG;
            ps_shape.get_unchecked(7).get_unchecked(if is_reg { 0 } else { 1 }).fetch_add(1, Ordering::SeqCst);
            if !is_reg {
                let plain = (*insn).seg as u32 == OCERZ_SEG_NONE && (*insn).addrsize != 4;
                ps_shape.get_unchecked(8).get_unchecked(if plain { 0 } else { 1 }).fetch_add(1, Ordering::SeqCst);
            }
        }
    }
}
#[unsafe(no_mangle)]
#[thread_local]
pub static mut ocerz_jit_exec_state: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_exec_one(
    mut vm: *mut OcerzVM,
    mut cpu: *mut OcerzCPU,
    mut insn: *const X86Insn,
) -> ::core::ffi::c_int {
    if ((*insn).op as ::core::ffi::c_int == OCERZ_OP_SYSCALL as ::core::ffi::c_int
        && (*insn).mode32 == 0
        && *(*cpu).gpr.get_unchecked(OCERZ_RAX as ::core::ffi::c_int as usize)
            == (2 as uint64_t) << 24 as ::core::ffi::c_int | 2 as uint64_t)
        as ::core::ffi::c_int as ::core::ffi::c_long != 0
    {
        (*cpu).cur_rip = (*insn).rip;
        (*cpu).rip = (*insn).rip;
        return OCERZ_EUNSUP as ::core::ffi::c_int;
    }
    if (ocerz_perfstat > 0 as ::core::ffi::c_int) as ::core::ffi::c_int
        as ::core::ffi::c_long != 0
    {
        jit_perfstat_one(insn);
    }
    (*vm).insn_count = (*vm).insn_count.wrapping_add(1);
    (*cpu).cur_rip = (*insn).rip;
    let mut next: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t);
    let mut m32mask: uint64_t = if (*insn).mode32 as ::core::ffi::c_int != 0 {
        0xffffffff as uint64_t
    } else {
        !(0 as uint64_t)
    };
    (*cpu).rip = next & m32mask;
    if ((*vm).trace != 0 as ::core::ffi::c_int) as ::core::ffi::c_int
        as ::core::ffi::c_long != 0
    {
        jit_trace_one(insn);
    }
    let mut prev: ::core::ffi::c_int = ocerz_jit_exec_state;
    if prev == 0 {
        ocerz_jit_exec_state = 1 as ::core::ffi::c_int;
    }
    let mut prev_op: uint64_t = (*cpu).slow_op;
    let mut form: uint64_t = (*insn).op as uint64_t
        | (((*insn).vex as ::core::ffi::c_int & 3 as ::core::ffi::c_int) as uint64_t)
            << 16 as ::core::ffi::c_int
        | (((*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            & 7 as ::core::ffi::c_int) as uint64_t) << 18 as ::core::ffi::c_int
        | (((*insn).ops.get_unchecked(1 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            & 7 as ::core::ffi::c_int) as uint64_t) << 21 as ::core::ffi::c_int
        | (((*insn).ops.get_unchecked(2 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            & 7 as ::core::ffi::c_int) as uint64_t) << 24 as ::core::ffi::c_int
        | ((*insn).opsize as uint64_t) << 32 as ::core::ffi::c_int;
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < (*insn).nops as ::core::ffi::c_int && i < 3 as ::core::ffi::c_int {
        if (*insn).ops.get_unchecked(i as usize).kind as ::core::ffi::c_int
            == OCERZ_OPK_IMM as ::core::ffi::c_int
        {
            form = (form as ::core::ffi::c_ulonglong
                | (((*insn).ops.get_unchecked(i as usize).imm & 0xff as uint64_t)
                    << 40 as ::core::ffi::c_int
                    | (1 as uint64_t) << 48 as ::core::ffi::c_int)
                    as ::core::ffi::c_ulonglong) as uint64_t;
        }
        i += 1;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_SYSCALL as ::core::ffi::c_int {
        form = (*insn).op as uint64_t
            | (*(*cpu).gpr.get_unchecked(OCERZ_RAX as ::core::ffi::c_int as usize) as uint32_t
                as uint64_t) << 32 as ::core::ffi::c_int;
    }
    (*cpu).slow_op = form;
    let mut r: ::core::ffi::c_int = ocerz_interp_exec(vm, cpu, insn);
    (*cpu).slow_op = prev_op;
    (*cpu).rip = ((*cpu).rip as ::core::ffi::c_ulonglong
        & if (*cpu).mode32 as ::core::ffi::c_int != 0 {
            0xffffffff as ::core::ffi::c_ulonglong
        } else {
            !(0 as ::core::ffi::c_ulonglong)
        }) as uint64_t;
    ocerz_jit_exec_state = prev;
    return r;
}
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn ocerz_jit_exec_run_at(
    mut vm: *mut OcerzVM,
    mut cpu: *mut OcerzCPU,
    mut b: *const JitBlock,
    mut idx: uint64_t,
    mut last: uint64_t,
) -> ::core::ffi::c_int {
    loop {
        let mut r: ::core::ffi::c_int = jit_exec_one(
            vm,
            cpu,
            blk_insn_full(b, idx as ::core::ffi::c_int),
        );
        if r != OCERZ_STEP_OK as ::core::ffi::c_int || idx >= last {
            return r;
        }
        idx = idx.wrapping_add(1);
    };
}
#[unsafe(no_mangle)]
pub static mut g_no_compact: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn host_ras_enabled() -> ::core::ffi::c_int {
    static mut en: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if en < 0 as ::core::ffi::c_int {
        en = if !libc::getenv(
                b"OCERZ_NO_HOST_RAS\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return en;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_push_pinned(
    mut b: *mut A64Buf,
    mut hs: ::core::ffi::c_int,
    mut rv: ::core::ffi::c_int,
) {
    if (stack_identity() != 0 || rsp_is_ptr() != 0) && rv != hs {
        a64_str_pre64(b, rv, hs, -(8 as ::core::ffi::c_int));
        return;
    }
    if !g_push_entry.is_null() && g_n_push_fix < JIT_MAX_BLOCK_INSNS && rv != hs {
        a64_sub_imm(b, 1 as ::core::ffi::c_int, hs, hs, 8 as uint32_t);
        let fresh0 = g_n_push_fix;
        g_n_push_fix = g_n_push_fix + 1;
        *(*(&raw mut g_push_fix)).get_unchecked_mut(fresh0 as usize) = a64_label(b).offset_from(g_push_entry)
            as ::core::ffi::c_long as uint32_t;
        a64_str_regoff(b, 8 as ::core::ffi::c_int, rv, JGB, hs, 0 as ::core::ffi::c_int);
        return;
    }
    a64_sub_imm(
        b,
        1 as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        hs,
        8 as uint32_t,
    );
    a64_str_regoff(
        b,
        8 as ::core::ffi::c_int,
        rv,
        JGB,
        JTA as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_reg(b, 1 as ::core::ffi::c_int, hs, JTA as ::core::ffi::c_int);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn low_stack_fast() -> ::core::ffi::c_int {
    static mut off: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if off < 0 as ::core::ffi::c_int {
        off = if !libc::getenv(
                b"OCERZ_NO_LOW_RAS\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    return (off == 0 && ocerz_low_base != 0 as uint64_t
        && ocerz_guest_base == 0 as uint64_t && stack_plain_access_ok() != 0
        && rsp_is_ptr() != 0) as ::core::ffi::c_int;
}
unsafe extern "C" fn emit_stack_push64(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut hs: ::core::ffi::c_int,
    mut rv: ::core::ffi::c_int,
) {
    if stack_guard_needed() == 0 {
        emit_push_pinned(b, hs, rv);
        return;
    }
    if g_lowstack != 0 && stack_plain_access_ok() != 0 && rv != JTA as ::core::ffi::c_int
        && ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_LOWSTACK_CALLRET\0" as *const u8
                        as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) == 0
    {
        a64_sub_imm(
            b,
            1 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            hs,
            8 as uint32_t,
        );
        a64_str_regoff(
            b,
            8 as ::core::ffi::c_int,
            rv,
            JTA as ::core::ffi::c_int,
            JGB,
            0 as ::core::ffi::c_int,
        );
        a64_mov_reg(b, 1 as ::core::ffi::c_int, hs, JTA as ::core::ffi::c_int);
        return;
    }
    a64_sub_imm(
        b,
        1 as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        hs,
        8 as uint32_t,
    );
    let mut skip: *mut uint32_t = emit_commpage_guard(
        b,
        insn,
        JTA as ::core::ffi::c_int,
        ::core::ptr::null_mut::<*mut uint32_t>(),
        ::core::ptr::null_mut::<::core::ffi::c_int>(),
    );
    g_ea_plain = stack_plain_now();
    emit_guest_store_ordered(
        b,
        8 as ::core::ffi::c_int,
        rv,
        JTA as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
    );
    if !skip.is_null() {
        a64_patch_b(skip, a64_label(b));
    }
    a64_sub_imm(b, 1 as ::core::ffi::c_int, hs, hs, 8 as uint32_t);
}
unsafe extern "C" fn emit_stack_pop64(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut hs: ::core::ffi::c_int,
    mut rd: ::core::ffi::c_int,
) {
    if stack_guard_needed() == 0 {
        if stack_identity() != 0 || rsp_is_ptr() != 0 {
            a64_ldr_post64(b, rd, hs, 8 as ::core::ffi::c_int);
        } else {
            a64_ldr_regoff(
                b,
                8 as ::core::ffi::c_int,
                rd,
                JGB,
                hs,
                0 as ::core::ffi::c_int,
            );
            a64_add_imm(b, 1 as ::core::ffi::c_int, hs, hs, 8 as uint32_t);
        }
        return;
    }
    if g_lowstack != 0 && stack_plain_access_ok() != 0 && rd != hs
        && ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_LOWSTACK_CALLRET\0" as *const u8
                        as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) == 0
    {
        a64_ldr_regoff(b, 8 as ::core::ffi::c_int, rd, hs, JGB, 0 as ::core::ffi::c_int);
        a64_add_imm(b, 1 as ::core::ffi::c_int, hs, hs, 8 as uint32_t);
        return;
    }
    a64_mov_reg(b, 1 as ::core::ffi::c_int, JTA as ::core::ffi::c_int, hs);
    let mut skip: *mut uint32_t = emit_commpage_guard(
        b,
        insn,
        JTA as ::core::ffi::c_int,
        ::core::ptr::null_mut::<*mut uint32_t>(),
        ::core::ptr::null_mut::<::core::ffi::c_int>(),
    );
    g_ea_plain = stack_plain_now();
    emit_guest_load_ordered(
        b,
        8 as ::core::ffi::c_int,
        rd,
        JTA as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
    );
    if !skip.is_null() {
        a64_patch_b(skip, a64_label(b));
    }
    a64_add_imm(b, 1 as ::core::ffi::c_int, hs, hs, 8 as uint32_t);
}
unsafe extern "C" fn a64_and_imm_or_mov(
    mut b: *mut A64Buf,
    mut sf: ::core::ffi::c_int,
    mut rd: ::core::ffi::c_int,
    mut rn: ::core::ffi::c_int,
    mut imm: uint64_t,
) {
    if a64_try_and_imm(b, sf, rd, rn, imm) == 0 {
        a64_mov_imm64(b, JTT as ::core::ffi::c_int, imm);
        a64_and_reg(b, sf, rd, rn, JTT as ::core::ffi::c_int, 0 as ::core::ffi::c_int);
    }
}
#[unsafe(no_mangle)]
pub static mut g_slow_run_last: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
unsafe extern "C" fn jmp_inline_enabled() -> ::core::ffi::c_int {
    static mut en: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if en < 0 as ::core::ffi::c_int {
        en = if !libc::getenv(
                b"OCERZ_NO_INLINE_JMP\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return en;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_jmp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut epilogue_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).op as ::core::ffi::c_int != OCERZ_OP_JMP as ::core::ffi::c_int
        || jmp_inline_enabled() == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
        != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut target: uint64_t = (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).imm;
    if g_no_chain == 0 && !g_loop_entry.is_null() && target == g_self_rip {
        l0_fixed_backedge(b);
        g_stop_patch = a64_label(b);
        a64_b(
            b,
            g_loop_entry.offset_from(g_stop_patch) as ::core::ffi::c_long as int32_t,
        );
        l0_fixed_fallthrough(b);
        fpb_emit_exit_check(b);
        g_stop_target = a64_label(b);
        a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            RIP_OFF,
        );
        a64_mov_imm64(
            b,
            0 as ::core::ffi::c_int,
            OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
        );
        let ref mut fresh1 = *epilogue_sites.offset(*n_epi as isize);
        *fresh1 = a64_label(b);
        a64_b(b, 0 as int32_t);
        *n_epi += 1;
        return 1 as ::core::ffi::c_int;
    }
    if g_no_chain == 0 {
        let mut poll: ::core::ffi::c_int = (target <= g_self_rip) as ::core::ffi::c_int;
        let mut edge_class: ::core::ffi::c_int = body_edge_pin_class();
        let mut body_edge: ::core::ffi::c_int = (edge_class >= 0 as ::core::ffi::c_int)
            as ::core::ffi::c_int;
        let mut pb: *mut uint32_t = emit_static_chain_tail(
            b,
            target,
            poll,
            body_edge,
            epilogue_sites,
            n_epi,
        );
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = target;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = (if body_edge != 0 {
            EDGE_BODY as ::core::ffi::c_int
        } else {
            EDGE_XBLOCK as ::core::ffi::c_int
        }) as uint8_t;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = (if body_edge != 0 {
            edge_class as uint8_t as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        }) as uint8_t;
        g_n_jcc_edges = 1 as ::core::ffi::c_int;
        return 1 as ::core::ffi::c_int;
    }
    a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    let ref mut fresh2 = *epilogue_sites.offset(*n_epi as isize);
    *fresh2 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    return 1 as ::core::ffi::c_int;
}
unsafe extern "C" fn callret_inline_enabled() -> ::core::ffi::c_int {
    static mut en: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if en < 0 as ::core::ffi::c_int {
        en = if !libc::getenv(
                b"OCERZ_NO_INLINE_CALLRET\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return en;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ras_push(
    mut vm: *mut OcerzVM,
    mut cpu: *mut OcerzCPU,
    mut retaddr: uint64_t,
) {
    let mut t: uint32_t = (*cpu).ras_top;
    if ras_body_only() != 0 {
        let mut blk: *mut JitBlock = cache_lookup(
            (*vm).jit as *mut OcerzJit,
            retaddr,
            (*cpu).mode32 as ::core::ffi::c_int,
        );
        (*cpu)
            .ras.get_unchecked_mut(
                (t & (OCERZ_RAS_SIZE - 1 as ::core::ffi::c_int) as uint32_t) as usize,
            )
            .guest_rip = retaddr;
        (*cpu)
            .ras.get_unchecked_mut(
                (t & (OCERZ_RAS_SIZE - 1 as ::core::ffi::c_int) as uint32_t) as usize,
            )
            .host_entry = ras_entry_for(blk);
        (*cpu).ras_top = t.wrapping_add(1 as uint32_t);
        return;
    }
    if t >= OCERZ_RAS_SIZE as uint32_t {
        return;
    }
    let mut blk_0: *mut JitBlock = cache_lookup(
        (*vm).jit as *mut OcerzJit,
        retaddr,
        (*cpu).mode32 as ::core::ffi::c_int,
    );
    (*cpu).ras.get_unchecked_mut(t as usize).guest_rip = retaddr;
    (*cpu).ras.get_unchecked_mut(t as usize).host_entry = ras_entry_for(blk_0);
    (*cpu).ras_top = t.wrapping_add(1 as uint32_t);
}
#[unsafe(no_mangle)]
pub static mut g_fps_frames: uint64_t = 0;
static mut g_fps_start: uint64_t = 0;
unsafe extern "C" fn fps_report(
    mut arg: *mut ::core::ffi::c_void,
) -> *mut ::core::ffi::c_void {
    let mut last: uint64_t = AtomicU64::from_ptr(&raw mut g_fps_frames).load(Ordering::Relaxed);
    let mut t0: uint64_t = clock_gettime_nsec_np(_CLOCK_UPTIME_RAW) as uint64_t;
    g_fps_start = t0;
    loop {
        usleep(1000000 as useconds_t);
        let mut now: uint64_t = AtomicU64::from_ptr(&raw mut g_fps_frames).load(Ordering::Relaxed);
        let mut t1: uint64_t = clock_gettime_nsec_np(_CLOCK_UPTIME_RAW) as uint64_t;
        if now != last {
            fprintf(
                crate::log::stderr(),
                b"ocerz: FPS[%d] %.1f t=%.1f\n\0" as *const u8
                    as *const ::core::ffi::c_char,
                getpid() as ::core::ffi::c_int,
                now.wrapping_sub(last) as ::core::ffi::c_double * 1e9f64
                    / t1.wrapping_sub(t0) as ::core::ffi::c_double,
                t1.wrapping_sub(g_fps_start) as ::core::ffi::c_double / 1e9f64,
            );
        }
        last = now;
        t0 = t1;
    };
}
extern "C" fn fps_report_entry(arg: *mut ::core::ffi::c_void) -> *mut ::core::ffi::c_void {
    unsafe { fps_report(arg) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fps_watch(mut rip: uint64_t) -> ::core::ffi::c_int {
    static mut state: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    static mut target: uint64_t = 0;
    if state < 0 as ::core::ffi::c_int {
        state = 0 as ::core::ffi::c_int;
        if !libc::getenv(b"OCERZ_FPS\0" as *const u8 as *const ::core::ffi::c_char).is_null()
            && ocerz_mode == OCERZ_MODE_CACHE as ::core::ffi::c_int
        {
            target = ocerz_dyld_resolve_guest_sym(
                b"_CGLFlushDrawable\0" as *const u8 as *const ::core::ffi::c_char,
            );
            let mut t: pthread_t = 0;
            if target != 0
                && pthread_create(
                    &raw mut t,
                    ::core::ptr::null::<pthread_attr_t>(),
                    fps_report_entry as extern "C" fn(*mut ::core::ffi::c_void) -> *mut ::core::ffi::c_void,
                    NULL,
                ) == 0 as ::core::ffi::c_int
            {
                pthread_detach(t);
                state = 1 as ::core::ffi::c_int;
            }
        }
    }
    return (state == 1 as ::core::ffi::c_int && rip == target) as ::core::ffi::c_int;
}
static mut g_ind_treg: ::core::ffi::c_int = JT1 as ::core::ffi::c_int;
unsafe extern "C" fn patch_local_adr(
    mut at: *mut uint32_t,
    mut target: *mut uint32_t,
    mut rd: ::core::ffi::c_int,
) {
    let mut off: ptrdiff_t = (target as *mut ::core::ffi::c_char)
        .offset_from(at as *mut ::core::ffi::c_char) as ptrdiff_t;
    if !(off >= -((1 as ::core::ffi::c_int) << 20 as ::core::ffi::c_int) as ptrdiff_t
        && off < ((1 as ::core::ffi::c_int) << 20 as ::core::ffi::c_int) as ptrdiff_t)
    {
        return;
    }
    let mut imm: uint32_t = (off as uint64_t & 0x1fffff as uint64_t) as uint32_t;
    *at = 0x10000000 as uint32_t | (imm & 3 as uint32_t) << 29 as ::core::ffi::c_int
        | (imm >> 2 as ::core::ffi::c_int & 0x7ffff as uint32_t)
            << 5 as ::core::ffi::c_int | (rd & 31 as ::core::ffi::c_int) as uint32_t;
}
unsafe extern "C" fn emit_step_epilogue_branch(
    mut b: *mut A64Buf,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) {
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    let ref mut fresh8 = *epi_sites.offset(*n_epi as isize);
    *fresh8 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
}
unsafe extern "C" fn emit_call_region_call(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if g_pin_class != 2 as ::core::ffi::c_int || g_no_chain != 0
        || g_body_entry.is_null()
        || (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int || mem_native_store_ok() == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut retaddr: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t);
    let mut target: uint64_t = (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).imm;
    let mut rs: ::core::ffi::c_int = pin_slot(
        OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
    );
    if !(rs >= 0 as ::core::ffi::c_int) as ::core::ffi::c_int as ::core::ffi::c_long != 0
    {
        __assert_rtn(
            b"emit_call_region_call\0" as *const u8 as *const ::core::ffi::c_char,
            b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
            672 as ::core::ffi::c_int,
            b"rs >= 0\0" as *const u8 as *const ::core::ffi::c_char,
        );
    } else {};
    a64_mov_imm64(b, JRET_GUEST, retaddr);
    let mut skip: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if stack_plain_access_ok() != 0 && stack_guard_needed() == 0 {
        a64_str_pre64(b, JRET_GUEST, pin_hreg(rs), -(8 as ::core::ffi::c_int));
    } else {
        a64_sub_imm(
            b,
            1 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            pin_hreg(rs),
            8 as uint32_t,
        );
        skip = emit_commpage_guard(
            b,
            insn,
            JTA as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
        g_ea_plain = stack_plain_now();
        emit_guest_store_ordered(
            b,
            8 as ::core::ffi::c_int,
            JRET_GUEST,
            JTA as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
        );
        a64_sub_imm(
            b,
            1 as ::core::ffi::c_int,
            pin_hreg(rs),
            pin_hreg(rs),
            8 as uint32_t,
        );
    }
    patch_guard_skip(skip, a64_label(b));
    emit_xmm_pin_spill_all(b);
    let mut adr: *mut uint32_t = a64_label(b);
    a64_emit32(b, 0x10000000 as uint32_t | JRET_HOST as uint32_t);
    let mut callee_patch: *mut uint32_t = a64_label(b);
    a64_b(b, 0 as int32_t);
    let mut host_cont: *mut uint32_t = a64_label(b);
    patch_local_adr(adr, host_cont, JRET_HOST);
    emit_xmm_pin_load_all(b);
    let mut return_patch: *mut uint32_t = emit_body_chain_tail(
        b,
        retaddr,
        0 as ::core::ffi::c_int,
        epi_sites,
        n_epi,
    );
    let mut callee_fallback: *mut uint32_t = a64_label(b);
    a64_patch_b(callee_patch, callee_fallback);
    if target <= g_self_rip {
        if !g_stop_patch.is_null() as ::core::ffi::c_int as ::core::ffi::c_long != 0 {
            __assert_rtn(
                b"emit_call_region_call\0" as *const u8 as *const ::core::ffi::c_char,
                b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
                703 as ::core::ffi::c_int,
                b"!g_stop_patch\0" as *const u8 as *const ::core::ffi::c_char,
            );
        } else {};
        g_stop_patch = callee_patch;
        g_stop_target = callee_fallback;
    }
    a64_add_imm(
        b,
        1 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        29 as ::core::ffi::c_int,
        0 as uint32_t,
    );
    a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    emit_step_epilogue_branch(b, epi_sites, n_epi);
    (*(&raw mut g_call_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = target;
    (*(&raw mut g_call_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = callee_patch;
    (*(&raw mut g_call_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = EDGE_BODY as ::core::ffi::c_int
        as uint8_t;
    (*(&raw mut g_call_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = 2 as uint8_t;
    (*(&raw mut g_call_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).target_rip = retaddr;
    (*(&raw mut g_call_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).patch_b = return_patch;
    (*(&raw mut g_call_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).kind = EDGE_BODY as ::core::ffi::c_int
        as uint8_t;
    (*(&raw mut g_call_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).pin_class = 2 as uint8_t;
    g_n_call_edges = 2 as ::core::ffi::c_int;
    return 1 as ::core::ffi::c_int;
}
unsafe extern "C" fn emit_call_region_ret(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if g_pin_class != 2 as ::core::ffi::c_int || g_no_chain != 0
        || g_body_entry.is_null()
        || (*insn).nops as ::core::ffi::c_int != 0 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut rs: ::core::ffi::c_int = pin_slot(
        OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
    );
    if !(rs >= 0 as ::core::ffi::c_int) as ::core::ffi::c_int as ::core::ffi::c_long != 0
    {
        __assert_rtn(
            b"emit_call_region_ret\0" as *const u8 as *const ::core::ffi::c_char,
            b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
            732 as ::core::ffi::c_int,
            b"rs >= 0\0" as *const u8 as *const ::core::ffi::c_char,
        );
    } else {};
    let mut skip: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if stack_plain_access_ok() != 0 && stack_guard_needed() == 0 {
        a64_ldr_post64(
            b,
            JT1 as ::core::ffi::c_int,
            pin_hreg(rs),
            8 as ::core::ffi::c_int,
        );
    } else {
        skip = emit_commpage_guard(b, insn, pin_hreg(rs), exit_sites, n_exits);
        g_ea_plain = stack_plain_now();
        emit_guest_load_ordered(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            pin_hreg(rs),
            JTU as ::core::ffi::c_int,
        );
        a64_add_imm(
            b,
            1 as ::core::ffi::c_int,
            pin_hreg(rs),
            pin_hreg(rs),
            8 as uint32_t,
        );
    }
    let mut check: *mut uint32_t = a64_label(b);
    let mut no_cache: *mut uint32_t = a64_label(b);
    a64_cbz(b, 1 as ::core::ffi::c_int, JRET_HOST, 0 as int32_t);
    a64_subs_reg(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JRET_GUEST,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let mut mismatch: *mut uint32_t = a64_label(b);
    a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
    emit_xmm_pin_spill_all(b);
    a64_br(b, JRET_HOST);
    let mut fallback: *mut uint32_t = a64_label(b);
    a64_patch_cbz(no_cache, fallback);
    a64_patch_bcond(mismatch, fallback);
    a64_add_imm(
        b,
        1 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        29 as ::core::ffi::c_int,
        0 as uint32_t,
    );
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    emit_step_epilogue_branch(b, epi_sites, n_epi);
    let mut slow: *mut uint32_t = a64_label(b);
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    let mut to_check: *mut uint32_t = a64_label(b);
    a64_b(b, 0 as int32_t);
    a64_patch_b(to_check, check);
    patch_guard_skip(skip, slow);
    return 1 as ::core::ffi::c_int;
}
unsafe extern "C" fn m32_ras_ok(mut insn: *const X86Insn) -> ::core::ffi::c_int {
    static mut no_blret: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if no_blret < 0 as ::core::ffi::c_int {
        no_blret = if !libc::getenv(
                b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    return (g_pin_class == 3 as ::core::ffi::c_int && fullpin_enabled() != 0
        && g_no_regflags == 0 && g_no_chain == 0 && g_no_ras == 0
        && host_ras_enabled() != 0 && no_blret == 0
        && ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_M32_RAS\0" as *const u8 as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) == 0 && m32_stack_ok(insn) != 0) as ::core::ffi::c_int;
}
unsafe extern "C" fn m32_ras_push(
    mut b: *mut A64Buf,
    mut retaddr: uint64_t,
) -> *mut uint32_t {
    a64_mov_imm64(b, JT2 as ::core::ffi::c_int, retaddr | JIT_KEY_M32 as uint64_t);
    let mut adr_site: *mut uint32_t = a64_label(b);
    a64_emit32(b, 0x10000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t);
    a64_stp_pre(
        b,
        JT2 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        -(16 as ::core::ffi::c_int),
    );
    return adr_site;
}
unsafe extern "C" fn emit_call_ret32(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut size: ::core::ffi::c_int = if (*insn).opsize as ::core::ffi::c_int != 0 {
        (*insn).opsize as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    if size != 4 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if m32_stack_ok(insn) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut hs: ::core::ffi::c_int = pin_hreg(
        pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
    );
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int {
        if (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        if mem_native_store_ok() == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut retaddr: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t)
            as uint32_t as uint64_t;
        let mut target: uint64_t = (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).imm;
        if target != retaddr && m32_ras_ok(insn) != 0 {
            let mut adr_site: *mut uint32_t = m32_ras_push(b, retaddr);
            a64_mov_imm64(b, JT1 as ::core::ffi::c_int, retaddr);
            a64_sub_imm(
                b,
                0 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                hs,
                4 as uint32_t,
            );
            m32_stack_st(b, JT1 as ::core::ffi::c_int, JTA as ::core::ffi::c_int);
            a64_mov_reg(b, 0 as ::core::ffi::c_int, hs, JTA as ::core::ffi::c_int);
            let mut pb_callee: *mut uint32_t = a64_label(b);
            a64_emit32(b, 0x94000000 as uint32_t);
            let mut cont: *mut uint32_t = a64_label(b);
            patch_local_adr(adr_site, cont, JT0 as ::core::ffi::c_int);
            let mut pb_ret: *mut uint32_t = emit_body_chain_tail(
                b,
                retaddr,
                0 as ::core::ffi::c_int,
                epi_sites,
                n_epi,
            );
            let mut callee_fb: *mut uint32_t = a64_label(b);
            *pb_callee = 0x94000000 as uint32_t
                | callee_fb.offset_from(pb_callee) as ::core::ffi::c_long as uint32_t
                    & 0x3ffffff as uint32_t;
            a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
            a64_mov_imm64(
                b,
                0 as ::core::ffi::c_int,
                OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
            );
            let ref mut fresh9 = *epi_sites.offset(*n_epi as isize);
            *fresh9 = a64_label(b);
            a64_b(b, 0 as int32_t);
            *n_epi += 1;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = target;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb_callee;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
                uint32_t,
            >();
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = EDGE_BODY
                as ::core::ffi::c_int as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).target_rip = retaddr;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).patch_b = pb_ret;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
                uint32_t,
            >();
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).kind = EDGE_BODY
                as ::core::ffi::c_int as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
            g_n_jcc_edges = 2 as ::core::ffi::c_int;
            return 1 as ::core::ffi::c_int;
        }
        a64_mov_imm64(b, JT1 as ::core::ffi::c_int, retaddr);
        a64_sub_imm(
            b,
            0 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            hs,
            4 as uint32_t,
        );
        m32_stack_st(b, JT1 as ::core::ffi::c_int, JTA as ::core::ffi::c_int);
        a64_mov_reg(b, 0 as ::core::ffi::c_int, hs, JTA as ::core::ffi::c_int);
        if g_no_chain != 0 {
            a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
            a64_mov_imm64(
                b,
                0 as ::core::ffi::c_int,
                OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
            );
            let ref mut fresh10 = *epi_sites.offset(*n_epi as isize);
            *fresh10 = a64_label(b);
            a64_b(b, 0 as int32_t);
            *n_epi += 1;
            return 1 as ::core::ffi::c_int;
        }
        let mut edge_class: ::core::ffi::c_int = body_edge_pin_class();
        let mut body_edge: ::core::ffi::c_int = (edge_class >= 0 as ::core::ffi::c_int)
            as ::core::ffi::c_int;
        let mut pb: *mut uint32_t = emit_static_chain_tail(
            b,
            target,
            (target <= g_self_rip) as ::core::ffi::c_int,
            body_edge,
            epi_sites,
            n_epi,
        );
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = target;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
            uint32_t,
        >();
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = (if body_edge != 0 {
            EDGE_BODY as ::core::ffi::c_int
        } else {
            EDGE_XBLOCK as ::core::ffi::c_int
        }) as uint8_t;
        (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = (if body_edge != 0 {
            edge_class as uint8_t as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        }) as uint8_t;
        g_n_jcc_edges = 1 as ::core::ffi::c_int;
        return 1 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_RET as ::core::ffi::c_int {
        let mut pop: uint32_t = 4 as uint32_t;
        if (*insn).nops as ::core::ffi::c_int == 1 as ::core::ffi::c_int {
            if (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
                != OCERZ_OPK_IMM as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            pop = pop
                .wrapping_add(
                    ((*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).imm
                        & 0xffff as uint64_t) as uint32_t,
                );
        }
        if pop > 4095 as uint32_t {
            return 0 as ::core::ffi::c_int;
        }
        m32_stack_ld(b, JT1 as ::core::ffi::c_int, hs);
        a64_add_imm(b, 0 as ::core::ffi::c_int, hs, hs, pop);
        if g_no_chain == 0
            && ({
                static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
                if on_ < 0 as ::core::ffi::c_int {
                    on_ = (libc::getenv(
                        b"OCERZ_NO_M32_RET_TAIL\0" as *const u8
                            as *const ::core::ffi::c_char,
                    ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
                }
                on_
            }) == 0
        {
            a64_try_orr_imm(
                b,
                1 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JIT_KEY_M32 as uint64_t,
            );
            if m32_ras_ok(insn) != 0 {
                a64_ldp_post(
                    b,
                    JTF as ::core::ffi::c_int,
                    30 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    16 as ::core::ffi::c_int,
                );
                a64_subs_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    A64_ZR,
                    JTF as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
                let mut stale: *mut uint32_t = a64_label(b);
                a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
                let mut null: *mut uint32_t = a64_label(b);
                a64_cbz(
                    b,
                    1 as ::core::ffi::c_int,
                    30 as ::core::ffi::c_int,
                    0 as int32_t,
                );
                if xmm_global_enabled() == 0 {
                    emit_xmm_pin_spill_all(b);
                }
                a64_ret(b);
                a64_patch_bcond(stale, a64_label(b));
                a64_patch_cbz(null, a64_label(b));
                let mut keep: *mut uint32_t = a64_label(b);
                a64_cbnz(
                    b,
                    1 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    0 as int32_t,
                );
                a64_sub_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    16 as uint32_t,
                );
                a64_patch_cbz(keep, a64_label(b));
                if xmm_global_enabled() == 0 {
                    emit_xmm_pin_spill_all(b);
                }
            }
            g_ind_treg = JT1 as ::core::ffi::c_int;
            g_ind_m32 = 1 as ::core::ffi::c_int;
            emit_indirect_tail(b, epi_sites, n_epi);
            return 1 as ::core::ffi::c_int;
        }
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            RIP_OFF,
        );
        a64_mov_imm64(
            b,
            0 as ::core::ffi::c_int,
            OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
        );
        let ref mut fresh11 = *epi_sites.offset(*n_epi as isize);
        *fresh11 = a64_label(b);
        a64_b(b, 0 as int32_t);
        *n_epi += 1;
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_call_ret(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut gbase: uint64_t = ocerz_guest_base;
    if callret_inline_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).mode32 != 0 {
        return emit_call_ret32(b, insn, epi_sites, n_epi);
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int
        && emit_call_region_call(b, insn, exit_sites, n_exits, epi_sites, n_epi) != 0
    {
        return 1 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_RET as ::core::ffi::c_int
        && emit_call_region_ret(b, insn, exit_sites, n_exits, epi_sites, n_epi) != 0
    {
        return 1 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int {
        if (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        if mem_native_store_ok() == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut retaddr: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t);
        let mut target: uint64_t = (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).imm;
        g_chain_target = target;
        emit_const_lit(b, JT1 as ::core::ffi::c_int, retaddr);
        let mut fast3: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
            && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
                >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
            && stack_fast() != 0 && g_no_chain == 0) as ::core::ffi::c_int;
        static mut no_blret: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        if no_blret < 0 as ::core::ffi::c_int {
            no_blret = if !libc::getenv(
                    b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
                )
                .is_null()
            {
                1 as ::core::ffi::c_int
            } else {
                0 as ::core::ffi::c_int
            };
        }
        if fast3 != 0 && ras_body_only() != 0 && g_no_ras == 0 && no_blret == 0 {
            let mut hs: ::core::ffi::c_int = pin_hreg(
                pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
            );
            let mut adr_site: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
            if host_ras_enabled() != 0 {
                adr_site = a64_label(b);
                a64_emit32(
                    b,
                    0x10000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t,
                );
                a64_stp_pre(
                    b,
                    JT1 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    -(16 as ::core::ffi::c_int),
                );
                emit_stack_push64(b, insn, hs, JT1 as ::core::ffi::c_int);
            } else {
                emit_stack_push64(b, insn, hs, JT1 as ::core::ffi::c_int);
                a64_ldr(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
                adr_site = a64_label(b);
                a64_emit32(
                    b,
                    0x10000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t,
                );
                a64_and_imm_or_mov(
                    b,
                    0 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    (OCERZ_RAS_SIZE - 1 as ::core::ffi::c_int) as uint64_t,
                );
                a64_add_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    4 as ::core::ffi::c_int,
                );
                if RAS_OFF <= 504 as uint32_t {
                    a64_stp_off(
                        b,
                        JT1 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        RAS_OFF as ::core::ffi::c_int,
                    );
                } else {
                    a64_str(
                        b,
                        8 as ::core::ffi::c_int,
                        JT1 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        RAS_OFF,
                    );
                    a64_str(
                        b,
                        8 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        RAS_OFF.wrapping_add(8 as uint32_t),
                    );
                }
                a64_add_imm(
                    b,
                    0 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
            }
            let mut pb_callee: *mut uint32_t = a64_label(b);
            a64_emit32(b, 0x94000000 as uint32_t);
            let mut cont: *mut uint32_t = a64_label(b);
            patch_local_adr(adr_site, cont, JT0 as ::core::ffi::c_int);
            let mut pb_ret: *mut uint32_t = emit_body_chain_tail(
                b,
                retaddr,
                0 as ::core::ffi::c_int,
                epi_sites,
                n_epi,
            );
            let mut callee_fb: *mut uint32_t = a64_label(b);
            *pb_callee = 0x94000000 as uint32_t
                | callee_fb.offset_from(pb_callee) as ::core::ffi::c_long as uint32_t
                    & 0x3ffffff as uint32_t;
            a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
            a64_mov_imm64(
                b,
                0 as ::core::ffi::c_int,
                OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
            );
            let ref mut fresh4 = *epi_sites.offset(*n_epi as isize);
            *fresh4 = a64_label(b);
            a64_b(b, 0 as int32_t);
            *n_epi += 1;
            g_chain_target = 0 as uint64_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = target;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb_callee;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
                uint32_t,
            >();
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = EDGE_BODY
                as ::core::ffi::c_int as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).target_rip = retaddr;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).patch_b = pb_ret;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
                uint32_t,
            >();
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).kind = EDGE_BODY
                as ::core::ffi::c_int as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(1 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
            g_n_jcc_edges = 2 as ::core::ffi::c_int;
            return 1 as ::core::ffi::c_int;
        }
        if fast3 != 0 {
            let mut hs_0: ::core::ffi::c_int = pin_hreg(
                pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
            );
            emit_stack_push64(b, insn, hs_0, JT1 as ::core::ffi::c_int);
        } else {
            emit_gpr_rd(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
            );
            a64_sub_imm(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                8 as uint32_t,
            );
            emit_add_const(b, JTA as ::core::ffi::c_int, ea_fold());
            let mut skip: *mut uint32_t = emit_commpage_guard(
                b,
                insn,
                JTA as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            );
            emit_add_const(b, JTA as ::core::ffi::c_int, gbase.wrapping_sub(ea_fold()));
            g_ea_plain = stack_plain_now();
            emit_guest_store_ordered(
                b,
                8 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
            );
            a64_sub_imm(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                8 as uint32_t,
            );
            emit_gpr_wr(
                b,
                JT0 as ::core::ffi::c_int,
                OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
            );
            a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target);
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
            patch_guard_skip(skip, a64_label(b));
        }
        if g_no_ras == 0 {
            let mut slot: *mut *mut ::core::ffi::c_void = ::core::ptr::null_mut::<
                *mut ::core::ffi::c_void,
            >();
            let mut lit: ::core::ffi::c_int = (fast3 != 0 && g_n_raslit < RASLIT_MAX)
                as ::core::ffi::c_int;
            if lit == 0 {
                slot = ras_slot_alloc();
            }
            if !slot.is_null() || lit != 0 {
                if !slot.is_null() {
                    let mut rb: *mut JitBlock = cache_lookup(
                        g_xlat_jit,
                        retaddr,
                        g_xlat_mode32,
                    );
                    if !rb.is_null() && (*rb).code.is_some() {
                        *slot = ras_entry_for(rb);
                    } else {
                        pending_add_ras(jit_key(retaddr, g_xlat_mode32), slot);
                    }
                }
                a64_ldr(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
                a64_subs_imm(
                    b,
                    0 as ::core::ffi::c_int,
                    A64_ZR,
                    JT2 as ::core::ffi::c_int,
                    OCERZ_RAS_SIZE as uint32_t,
                );
                let mut full: *mut uint32_t = a64_label(b);
                a64_bcond(b, A64_CS as ::core::ffi::c_int, 0 as int32_t);
                if fast3 == 0 {
                    a64_mov_imm64(b, JT1 as ::core::ffi::c_int, retaddr);
                }
                if lit != 0 {
                    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).site = a64_label(b);
                    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).retaddr = retaddr;
                    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).kind = 0 as ::core::ffi::c_int;
                    (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).rt = JT0 as ::core::ffi::c_int;
                    g_n_raslit += 1;
                    a64_emit32(
                        b,
                        0x58000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t,
                    );
                } else {
                    tc_imm64(
                        b,
                        JTA as ::core::ffi::c_int,
                        TCR_RASSLOT as ::core::ffi::c_int,
                        retaddr,
                        slot as uintptr_t as uint64_t,
                    );
                    a64_ldr(
                        b,
                        8 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        0 as uint32_t,
                    );
                }
                a64_add_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    4 as ::core::ffi::c_int,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF.wrapping_add(8 as uint32_t),
                );
                a64_add_imm(
                    b,
                    0 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
                a64_patch_bcond(full, a64_label(b));
            } else {
                emit_xmm_pin_spill_all(b);
                emit_spill_pinned_callersaved(b);
                a64_mov_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                    19 as ::core::ffi::c_int,
                );
                a64_mov_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    1 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                );
                a64_mov_imm64(b, 2 as ::core::ffi::c_int, retaddr);
                tc_imm64(
                    b,
                    16 as ::core::ffi::c_int,
                    TCR_SYM as ::core::ffi::c_int,
                    TCS_RAS_PUSH as ::core::ffi::c_int as uint64_t,
                    ::core::mem::transmute::<
                        Option<
                            unsafe extern "C" fn(
                                *mut OcerzVM,
                                *mut OcerzCPU,
                                uint64_t,
                            ) -> (),
                        >,
                        uintptr_t,
                    >(
                        Some(
                            ocerz_ras_push
                                as unsafe extern "C" fn(
                                    *mut OcerzVM,
                                    *mut OcerzCPU,
                                    uint64_t,
                                ) -> (),
                        ),
                    ) as uint64_t,
                );
                a64_blr(b, 16 as ::core::ffi::c_int);
                emit_fill_pinned_callersaved(b);
                emit_reload_jgb(b);
                emit_xmm_pin_load_all(b);
            }
        }
    } else if (*insn).op as ::core::ffi::c_int == OCERZ_OP_RET as ::core::ffi::c_int {
        if (*insn).nops as ::core::ffi::c_int != 0 as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        let mut fast3_0: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
            && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
                >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
            && stack_fast() != 0) as ::core::ffi::c_int;
        if fast3_0 != 0 {
            let mut hs_1: ::core::ffi::c_int = pin_hreg(
                pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
            );
            emit_stack_pop64(b, insn, hs_1, JT1 as ::core::ffi::c_int);
            if ras_body_only() == 0 {
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RIP_OFF,
                );
            }
        } else {
            emit_gpr_rd(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
            );
            a64_mov_reg(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            );
            emit_add_const(b, JTA as ::core::ffi::c_int, ea_fold());
            let mut skip_0: *mut uint32_t = emit_commpage_guard(
                b,
                insn,
                JTA as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            );
            emit_add_const(b, JTA as ::core::ffi::c_int, gbase.wrapping_sub(ea_fold()));
            g_ea_plain = stack_plain_now();
            emit_guest_load_ordered(
                b,
                8 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
            );
            a64_add_imm(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                8 as uint32_t,
            );
            emit_gpr_wr(
                b,
                JT0 as ::core::ffi::c_int,
                OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
            );
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
            patch_guard_skip(skip_0, a64_label(b));
        }
        if g_no_ras == 0 {
            let mut ras_empty: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
            let mut ras_stale: [*mut uint32_t; 3] = [::core::ptr::null_mut::<
                uint32_t,
            >(); 3];
            let mut nst: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
            let mut hostras: ::core::ffi::c_int = (fast3_0 != 0 && ras_body_only() != 0
                && host_ras_enabled() != 0) as ::core::ffi::c_int;
            if hostras == 0 {
                a64_ldr(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
            }
            if !(hostras != 0) {
                if fast3_0 != 0 && ras_body_only() != 0 {
                    a64_sub_imm(
                        b,
                        0 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        1 as uint32_t,
                    );
                    a64_and_imm_or_mov(
                        b,
                        0 as ::core::ffi::c_int,
                        JTU as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        (OCERZ_RAS_SIZE - 1 as ::core::ffi::c_int) as uint64_t,
                    );
                    a64_add_reg(
                        b,
                        1 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        20 as ::core::ffi::c_int,
                        JTU as ::core::ffi::c_int,
                        4 as ::core::ffi::c_int,
                    );
                } else {
                    ras_empty = a64_label(b);
                    a64_cbz(
                        b,
                        0 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        0 as int32_t,
                    );
                    a64_sub_imm(
                        b,
                        0 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        1 as uint32_t,
                    );
                    a64_add_reg(
                        b,
                        1 as ::core::ffi::c_int,
                        JTA as ::core::ffi::c_int,
                        20 as ::core::ffi::c_int,
                        JT2 as ::core::ffi::c_int,
                        4 as ::core::ffi::c_int,
                    );
                }
            }
            static mut no_blret_r: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if no_blret_r < 0 as ::core::ffi::c_int {
                no_blret_r = if !libc::getenv(
                        b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
                    )
                    .is_null()
                {
                    1 as ::core::ffi::c_int
                } else {
                    0 as ::core::ffi::c_int
                };
            }
            let mut use_ret: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
                && fast3_0 != 0 && ras_body_only() != 0 && no_blret_r == 0)
                as ::core::ffi::c_int;
            let mut host_reg: ::core::ffi::c_int = if use_ret != 0 {
                30 as ::core::ffi::c_int
            } else {
                JT0 as ::core::ffi::c_int
            };
            if hostras != 0 {
                a64_ldp_post(
                    b,
                    JTF as ::core::ffi::c_int,
                    host_reg,
                    31 as ::core::ffi::c_int,
                    16 as ::core::ffi::c_int,
                );
            } else if RAS_OFF <= 504 as uint32_t {
                a64_ldp_off(
                    b,
                    JTF as ::core::ffi::c_int,
                    host_reg,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF as ::core::ffi::c_int,
                );
            } else {
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    host_reg,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF.wrapping_add(8 as uint32_t),
                );
            }
            a64_subs_reg(
                b,
                1 as ::core::ffi::c_int,
                A64_ZR,
                JTF as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            *ras_stale.get_unchecked_mut(nst as usize) = a64_label(b);
            a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
            nst += 1;
            *ras_stale.get_unchecked_mut(nst as usize) = a64_label(b);
            a64_cbz(b, 1 as ::core::ffi::c_int, host_reg, 0 as int32_t);
            nst += 1;
            if hostras == 0 {
                a64_str(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
            }
            let mut not_body: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
            if g_pin_class == 3 as ::core::ffi::c_int && fast3_0 != 0
                && ras_body_only() != 0
            {
                if xmm_global_enabled() == 0 {
                    emit_xmm_pin_spill_all(b);
                }
                if use_ret != 0 {
                    a64_ret(b);
                } else {
                    a64_br(b, JT0 as ::core::ffi::c_int);
                }
                not_body = ::core::ptr::null_mut::<uint32_t>();
            } else if g_pin_class == 3 as ::core::ffi::c_int {
                not_body = a64_label(b);
                a64_tbz(
                    b,
                    JT0 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                    0 as int32_t,
                );
                if a64_try_and_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    !(1 as uint64_t),
                ) == 0
                {
                    a64_mov_imm64(b, JTU as ::core::ffi::c_int, 1 as uint64_t);
                    a64_bic_reg(
                        b,
                        1 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JTU as ::core::ffi::c_int,
                        0 as ::core::ffi::c_int,
                    );
                }
                if xmm_global_enabled() == 0 {
                    emit_xmm_pin_spill_all(b);
                }
                a64_br(b, JT0 as ::core::ffi::c_int);
                a64_patch_tbz(not_body, a64_label(b));
                if a64_try_and_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    !(1 as uint64_t),
                ) == 0
                {
                    a64_mov_imm64(b, JTU as ::core::ffi::c_int, 1 as uint64_t);
                    a64_bic_reg(
                        b,
                        1 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JT0 as ::core::ffi::c_int,
                        JTU as ::core::ffi::c_int,
                        0 as ::core::ffi::c_int,
                    );
                }
            } else {
                *ras_stale.get_unchecked_mut(nst as usize) = a64_label(b);
                a64_tbnz(
                    b,
                    JT0 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                    0 as int32_t,
                );
                nst += 1;
            }
            emit_xmm_pin_spill_all(b);
            emit_spill_pinned(b);
            a64_mov_reg(
                b,
                1 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
                19 as ::core::ffi::c_int,
            );
            a64_mov_reg(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
            );
            emit_frame_sp_reset(b);
            emit_pin_epilogue_restore(b);
            a64_ldp_post(
                b,
                19 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                31 as ::core::ffi::c_int,
                16 as ::core::ffi::c_int,
            );
            a64_ldp_post(
                b,
                29 as ::core::ffi::c_int,
                30 as ::core::ffi::c_int,
                31 as ::core::ffi::c_int,
                16 as ::core::ffi::c_int,
            );
            a64_br(b, JT0 as ::core::ffi::c_int);
            let mut null_pop: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
            if ocerz_perfstat > 0 as ::core::ffi::c_int {
                null_pop = a64_label(b);
                a64_mov_imm64(
                    b,
                    JTA as ::core::ffi::c_int,
                    &raw mut ps_ras_null as uintptr_t as uint64_t,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_add_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
            }
            let mut miss_pop: *mut uint32_t = a64_label(b);
            if hostras == 0 {
                a64_str(
                    b,
                    4 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RAS_TOP_OFF,
                );
            }
            if fast3_0 != 0 && ras_body_only() != 0 {
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RIP_OFF,
                );
            }
            if ocerz_perfstat > 0 as ::core::ffi::c_int {
                g_tc_bad = 1 as ::core::ffi::c_int;
                a64_mov_imm64(
                    b,
                    JTA as ::core::ffi::c_int,
                    &raw mut ps_ras_stale as uintptr_t as uint64_t,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_add_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_mov_imm64(
                    b,
                    JTA as ::core::ffi::c_int,
                    ps_retsite_counter((*insn).rip) as uintptr_t as uint64_t,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_add_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                let mut nz: *mut uint32_t = a64_label(b);
                a64_cbnz(
                    b,
                    1 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    0 as int32_t,
                );
                a64_mov_imm64(
                    b,
                    JTA as ::core::ffi::c_int,
                    &raw mut ps_ras_sentinel as uintptr_t as uint64_t,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_add_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_patch_cbz(nz, a64_label(b));
            }
            let mut skip_rip: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
            if fast3_0 != 0 && ras_body_only() != 0 {
                skip_rip = a64_label(b);
                a64_b(b, 0 as int32_t);
            }
            let mut miss: *mut uint32_t = a64_label(b);
            if !ras_empty.is_null() {
                a64_patch_cbz(ras_empty, miss);
            }
            if fast3_0 != 0 && ras_body_only() != 0 {
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    RIP_OFF,
                );
                a64_patch_b(skip_rip, a64_label(b));
            }
            if ocerz_perfstat > 0 as ::core::ffi::c_int {
                g_tc_bad = 1 as ::core::ffi::c_int;
                a64_mov_imm64(
                    b,
                    JTA as ::core::ffi::c_int,
                    &raw mut ps_ras_miss as uintptr_t as uint64_t,
                );
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
                a64_add_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    1 as uint32_t,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JTU as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    0 as uint32_t,
                );
            }
            let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
            while i < nst {
                if **ras_stale.get_unchecked(i as usize) & 0x7f000000 as uint32_t
                    == 0x36000000 as uint32_t
                    || **ras_stale.get_unchecked(i as usize) & 0x7f000000 as uint32_t
                        == 0x37000000 as uint32_t
                {
                    a64_patch_tbz(*ras_stale.get_unchecked(i as usize), miss_pop);
                } else if **ras_stale.get_unchecked(i as usize) & 0xff000010 as uint32_t
                    == 0x54000000 as uint32_t
                {
                    a64_patch_bcond(*ras_stale.get_unchecked(i as usize), miss_pop);
                } else {
                    a64_patch_cbz(
                        *ras_stale.get_unchecked(i as usize),
                        if !null_pop.is_null() { null_pop } else { miss_pop },
                    );
                }
                i += 1;
            }
            if hostras != 0 {
                let mut keep: *mut uint32_t = a64_label(b);
                a64_cbnz(
                    b,
                    1 as ::core::ffi::c_int,
                    JTF as ::core::ffi::c_int,
                    0 as int32_t,
                );
                a64_sub_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    31 as ::core::ffi::c_int,
                    16 as uint32_t,
                );
                a64_patch_cbz(keep, a64_label(b));
                if xmm_global_enabled() == 0 {
                    emit_xmm_pin_spill_all(b);
                }
                g_ind_call_cont = ::core::ptr::null_mut::<*mut uint32_t>();
                g_ind_treg = JT1 as ::core::ffi::c_int;
                emit_indirect_tail(b, epi_sites, n_epi);
                return 1 as ::core::ffi::c_int;
            }
        }
    } else {
        return 0 as ::core::ffi::c_int
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int
        && g_pin_class == 3 as ::core::ffi::c_int
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
        && jgb_usable() != 0 && stack_guard_needed() == 0 && g_no_chain == 0
    {
        g_chain_keeps_jgb = 1 as ::core::ffi::c_int;
    } else {
        a64_mov_imm64(
            b,
            0 as ::core::ffi::c_int,
            OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
        );
    }
    let ref mut fresh5 = *epi_sites.offset(*n_epi as isize);
    *fresh5 = a64_label(b);
    g_chain_epi = *epi_sites.offset(*n_epi as isize);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_dispatch_stub(
    mut jit: *mut OcerzJit,
    mut mode32: ::core::ffi::c_int,
) {
    veneer_pool_check(jit);
    let mut b: A64Buf = A64Buf {
        start: (*jit).code_cur,
        p: (*jit).code_cur,
        end: (*jit).code_end,
        overflow: 0 as ::core::ffi::c_int,
        sink: 0 as uint32_t,
    };
    let mut entry: *mut uint32_t = b.p;
    let mut to_ret: [*mut uint32_t; 12] = [::core::ptr::null_mut::<uint32_t>(); 12];
    let mut nr: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    a64_ldr(
        &raw mut b,
        4 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        INT_OFF,
    );
    let fresh12 = nr;
    nr = nr + 1;
    *to_ret.get_unchecked_mut(fresh12 as usize) = a64_label(&raw mut b);
    a64_cbnz(
        &raw mut b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as int32_t,
    );
    a64_ldr(
        &raw mut b,
        4 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        5192 as ::core::ffi::c_ulong as uint32_t,
    );
    let fresh13 = nr;
    nr = nr + 1;
    *to_ret.get_unchecked_mut(fresh13 as usize) = a64_label(&raw mut b);
    a64_cbnz(
        &raw mut b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as int32_t,
    );
    a64_ldr(
        &raw mut b,
        4 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        9408 as ::core::ffi::c_ulong as uint32_t,
    );
    let fresh14 = nr;
    nr = nr + 1;
    *to_ret.get_unchecked_mut(fresh14 as usize) = a64_label(&raw mut b);
    a64_cbnz(
        &raw mut b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as int32_t,
    );
    a64_ldr(
        &raw mut b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        RIP_OFF,
    );
    a64_mov_imm64(&raw mut b, JTU as ::core::ffi::c_int, OCERZ_DYLDAPI_LO as uint64_t);
    a64_sub_reg(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(
        &raw mut b,
        JTU as ::core::ffi::c_int,
        (OCERZ_DYLDAPI_HI as uint64_t).wrapping_sub(OCERZ_DYLDAPI_LO as uint64_t),
    );
    a64_subs_reg(
        &raw mut b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let fresh15 = nr;
    nr = nr + 1;
    *to_ret.get_unchecked_mut(fresh15 as usize) = a64_label(&raw mut b);
    a64_bcond(&raw mut b, A64_CC as ::core::ffi::c_int, 0 as int32_t);
    if mode32 != 0 {
        let mut ok: ::core::ffi::c_int = a64_try_orr_imm(
            &raw mut b,
            1 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            JIT_KEY_M32 as uint64_t,
        );
        if ok == 0
        {
            __assert_rtn(
                b"emit_dispatch_stub\0" as *const u8 as *const ::core::ffi::c_char,
                b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
                1239 as ::core::ffi::c_int,
                b"ok && \"JIT_KEY_M32 must encode as a logical immediate\"\0"
                    as *const u8 as *const ::core::ffi::c_char,
            );
        } else {};
    }
    a64_lsr_imm(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        33 as ::core::ffi::c_int,
    );
    a64_eor_reg(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(&raw mut b, JTU as ::core::ffi::c_int, 0xff51afd7ed558ccd as uint64_t);
    a64_mul(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
    );
    a64_lsr_imm(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        29 as ::core::ffi::c_int,
    );
    a64_eor_reg(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(&raw mut b, JTU as ::core::ffi::c_int, JIT_HASH_MASK as uint64_t);
    a64_and_reg(
        &raw mut b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(
        &raw mut b,
        JTA as ::core::ffi::c_int,
        &raw mut (*jit).buckets as *mut *mut JitBlock as uintptr_t as uint64_t,
    );
    a64_ldr_regoff(
        &raw mut b,
        8 as ::core::ffi::c_int,
        JTF as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    );
    let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while k < 6 as ::core::ffi::c_int {
        let fresh16 = nr;
        nr = nr + 1;
        *to_ret.get_unchecked_mut(fresh16 as usize) = a64_label(&raw mut b);
        a64_cbz(
            &raw mut b,
            1 as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            0 as int32_t,
        );
        a64_ldr(
            &raw mut b,
            8 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            0 as ::core::ffi::c_ulong as uint32_t,
        );
        a64_sub_reg(
            &raw mut b,
            1 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        let mut nxt: *mut uint32_t = a64_label(&raw mut b);
        a64_cbnz(
            &raw mut b,
            1 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            0 as int32_t,
        );
        a64_ldr(
            &raw mut b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            8 as ::core::ffi::c_ulong as uint32_t,
        );
        let mut nocode: *mut uint32_t = a64_label(&raw mut b);
        a64_cbz(
            &raw mut b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            0 as int32_t,
        );
        a64_br(&raw mut b, JT0 as ::core::ffi::c_int);
        let mut cont: *mut uint32_t = a64_label(&raw mut b);
        a64_patch_cbz(nxt, cont);
        a64_patch_cbz(nocode, cont);
        a64_ldr(
            &raw mut b,
            8 as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            80 as ::core::ffi::c_ulong as uint32_t,
        );
        k += 1;
    }
    let mut retl: *mut uint32_t = a64_label(&raw mut b);
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < nr {
        let mut w: uint32_t = **to_ret.get_unchecked(i as usize);
        if w & 0xff000010 as uint32_t == 0x54000000 as uint32_t {
            a64_patch_bcond(*to_ret.get_unchecked(i as usize), retl);
        } else {
            a64_patch_cbz(*to_ret.get_unchecked(i as usize), retl);
        }
        i += 1;
    }
    a64_mov_imm64(
        &raw mut b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    a64_ret(&raw mut b);
    if b.overflow == 0 {
        if mode32 != 0 {
            (*jit).dispatch_stub32 = entry;
        } else {
            (*jit).dispatch_stub = entry;
        }
        (*jit).code_cur = b.p;
        sys_icache_invalidate(
            entry as *mut ::core::ffi::c_void,
            (b.p as *mut uint8_t).offset_from(entry as *mut uint8_t)
                as ::core::ffi::c_long as size_t,
        );
    }
}
unsafe extern "C" fn emit_indirect_leave_br(
    mut b: *mut A64Buf,
    mut code_reg: ::core::ffi::c_int,
) {
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        19 as ::core::ffi::c_int,
    );
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
    );
    emit_frame_sp_reset(b);
    emit_pin_epilogue_restore(b);
    a64_ldp_post(
        b,
        19 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    a64_ldp_post(
        b,
        29 as ::core::ffi::c_int,
        30 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    a64_br(b, code_reg);
}
static mut g_ind_call_cont: *mut *mut uint32_t = ::core::ptr::null::<*mut uint32_t>()
    as *mut *mut uint32_t;
static mut g_ind_call_tocont: *mut uint32_t = ::core::ptr::null::<uint32_t>()
    as *mut uint32_t;
static mut g_ind_m32: ::core::ffi::c_int = 0;
static mut g_dbg_ind_src: uint64_t = 0;
unsafe extern "C" fn emit_indirect_tail(
    mut b: *mut A64Buf,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) {
    static mut dbg: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if dbg < 0 as ::core::ffi::c_int {
        dbg = if !libc::getenv(b"OCERZ_WILDLOG\0" as *const u8 as *const ::core::ffi::c_char)
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    if dbg != 0 && g_dbg_ind_src != 0 {
        a64_mov_imm64(b, JTU as ::core::ffi::c_int, g_dbg_ind_src);
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            5608 as ::core::ffi::c_ulong as uint32_t,
        );
    }
    let mut to_blr: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    let mut psc: *mut JitPscEnt = ::core::ptr::null_mut::<JitPscEnt>();
    let mut treg: ::core::ffi::c_int = g_ind_treg;
    let mut m32: ::core::ffi::c_int = g_ind_m32;
    g_ind_treg = JT1 as ::core::ffi::c_int;
    g_ind_m32 = 0 as ::core::ffi::c_int;
    if g_pin_class == 3 as ::core::ffi::c_int && g_n_raslit < RASLIT_MAX
        && ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_PSC\0" as *const u8 as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) == 0
    {
        psc = psc_alloc();
    }
    let mut psc_miss: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if !psc.is_null() {
        (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).site = a64_label(b);
        (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).retaddr = psc as uintptr_t as uint64_t;
        (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).kind = 1 as ::core::ffi::c_int;
        (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).tcr = TCR_PSC as ::core::ffi::c_int;
        (*(&raw mut g_raslit)).get_unchecked_mut(g_n_raslit as usize).rt = JT2 as ::core::ffi::c_int;
        g_n_raslit += 1;
        a64_emit32(b, 0x58000000 as uint32_t | JT2 as ::core::ffi::c_int as uint32_t);
        a64_ubfx(
            b,
            1 as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            treg,
            2 as ::core::ffi::c_int,
            5 as ::core::ffi::c_int,
        );
        a64_add_reg(
            b,
            1 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            4 as ::core::ffi::c_int,
        );
        a64_ldr_regoff(
            b,
            8 as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            19 as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
        );
        a64_ldp_off(
            b,
            JTU as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        a64_eor_reg(
            b,
            1 as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            treg,
            0 as ::core::ffi::c_int,
        );
        a64_subs_reg(
            b,
            1 as ::core::ffi::c_int,
            A64_ZR,
            JTU as ::core::ffi::c_int,
            JTT as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        psc_miss = a64_label(b);
        a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
        let mut intr: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
        let mut stop_site_ok: ::core::ffi::c_int = (g_n_stop_extra
            < 6 as ::core::ffi::c_int) as ::core::ffi::c_int;
        if stop_site_ok == 0 {
            a64_ldr(
                b,
                4 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                INT_OFF,
            );
            intr = a64_label(b);
            a64_cbnz(
                b,
                0 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                0 as int32_t,
            );
        }
        let mut br_site: *mut uint32_t = a64_label(b);
        if !g_ind_call_cont.is_null() {
            to_blr = a64_label(b);
            a64_blr(b, JT0 as ::core::ffi::c_int);
            *g_ind_call_cont = a64_label(b);
            g_ind_call_tocont = a64_label(b);
            a64_b(b, 0 as int32_t);
        } else {
            a64_br(b, JT0 as ::core::ffi::c_int);
        }
        let mut stop_lbl: *mut uint32_t = a64_label(b);
        if !intr.is_null() {
            a64_patch_cbz(intr, stop_lbl);
        }
        if stop_site_ok != 0 {
            stop_extra_add(br_site, stop_lbl);
        }
        if m32 != 0 {
            a64_try_and_imm(
                b,
                1 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                treg,
                !(JIT_KEY_M32 as uint64_t),
            );
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RIP_OFF,
            );
        } else {
            a64_str(b, 8 as ::core::ffi::c_int, treg, 20 as ::core::ffi::c_int, RIP_OFF);
        }
        a64_mov_imm64(
            b,
            0 as ::core::ffi::c_int,
            OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
        );
        let ref mut fresh6 = *epi_sites.offset(*n_epi as isize);
        *fresh6 = a64_label(b);
        a64_b(b, 0 as int32_t);
        *n_epi += 1;
        a64_patch_bcond(psc_miss, a64_label(b));
        a64_ubfx(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            treg,
            2 as ::core::ffi::c_int,
            5 as ::core::ffi::c_int,
        );
        a64_add_reg(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            19 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            3 as ::core::ffi::c_int,
        );
        a64_ldar(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
        );
    }
    if treg != JT1 as ::core::ffi::c_int {
        a64_mov_reg(b, 1 as ::core::ffi::c_int, JT1 as ::core::ffi::c_int, treg);
    }
    if m32 != 0 {
        a64_try_and_imm(
            b,
            1 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            !(JIT_KEY_M32 as uint64_t),
        );
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            RIP_OFF,
        );
    } else {
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            RIP_OFF,
        );
    }
    a64_lsr_imm(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        33 as ::core::ffi::c_int,
    );
    a64_eor_reg(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(b, JTU as ::core::ffi::c_int, 0xff51afd7ed558ccd as uint64_t);
    a64_mul(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
    );
    a64_lsr_imm(
        b,
        1 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        29 as ::core::ffi::c_int,
    );
    a64_eor_reg(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_mov_imm64(b, JTU as ::core::ffi::c_int, JIT_HASH_MASK as uint64_t);
    a64_and_reg(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    tc_imm64(
        b,
        JTA as ::core::ffi::c_int,
        TCR_BUCKETS as ::core::ffi::c_int,
        0 as uint64_t,
        &raw mut (*g_xlat_jit).buckets as *mut *mut JitBlock as uintptr_t as uint64_t,
    );
    a64_ldr_regoff(
        b,
        8 as ::core::ffi::c_int,
        JTF as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    );
    let mut loop_0: *mut uint32_t = a64_label(b);
    let mut to_nofind: *mut uint32_t = a64_label(b);
    a64_cbz(b, 1 as ::core::ffi::c_int, JTF as ::core::ffi::c_int, 0 as int32_t);
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        JTF as ::core::ffi::c_int,
        0 as ::core::ffi::c_ulong as uint32_t,
    );
    a64_sub_reg(
        b,
        1 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let mut found: *mut uint32_t = a64_label(b);
    a64_cbz(b, 1 as ::core::ffi::c_int, JTU as ::core::ffi::c_int, 0 as int32_t);
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JTF as ::core::ffi::c_int,
        JTF as ::core::ffi::c_int,
        80 as ::core::ffi::c_ulong as uint32_t,
    );
    let mut here: *mut uint32_t = a64_label(b);
    a64_b(b, loop_0.offset_from(here) as ::core::ffi::c_long as int32_t);
    a64_patch_cbz(found, a64_label(b));
    let mut to_full: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if g_pin_class == 1 as ::core::ffi::c_int || g_pin_class == 3 as ::core::ffi::c_int {
        a64_ldr(
            b,
            1 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            369 as ::core::ffi::c_ulong as uint32_t,
        );
        a64_sub_imm(
            b,
            0 as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
            g_pin_class as uint32_t,
        );
        to_full = a64_label(b);
        a64_cbnz(b, 0 as ::core::ffi::c_int, JTU as ::core::ffi::c_int, 0 as int32_t);
        let mut nobody: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
        if !psc.is_null() {
            a64_ldr(
                b,
                8 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                16 as ::core::ffi::c_ulong as uint32_t,
            );
            nobody = a64_label(b);
            a64_cbz(b, 1 as ::core::ffi::c_int, JTA as ::core::ffi::c_int, 0 as int32_t);
            a64_eor_reg(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            a64_stp_off(
                b,
                JT0 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            a64_mov_reg(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
            );
        } else {
            a64_ldr(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                16 as ::core::ffi::c_ulong as uint32_t,
            );
            nobody = a64_label(b);
            a64_cbz(b, 1 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, 0 as int32_t);
        }
        let mut intr_0: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
        let mut stop_site_ok2: ::core::ffi::c_int = (g_n_stop_extra
            < 6 as ::core::ffi::c_int) as ::core::ffi::c_int;
        if stop_site_ok2 == 0 {
            a64_ldr(
                b,
                4 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                INT_OFF,
            );
            intr_0 = a64_label(b);
            a64_cbnz(
                b,
                0 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                0 as int32_t,
            );
        }
        if xmm_global_enabled() == 0 {
            emit_xmm_pin_spill_all(b);
        }
        let mut br_site2: *mut uint32_t = a64_label(b);
        if !to_blr.is_null() {
            let mut here_0: *mut uint32_t = a64_label(b);
            a64_b(b, to_blr.offset_from(here_0) as ::core::ffi::c_long as int32_t);
        } else if !g_ind_call_cont.is_null() {
            to_blr = a64_label(b);
            a64_blr(b, JT0 as ::core::ffi::c_int);
            *g_ind_call_cont = a64_label(b);
            g_ind_call_tocont = a64_label(b);
            a64_b(b, 0 as int32_t);
        } else {
            a64_br(b, JT0 as ::core::ffi::c_int);
        }
        let mut stop_lbl2: *mut uint32_t = a64_label(b);
        a64_patch_cbz(nobody, stop_lbl2);
        if !intr_0.is_null() {
            a64_patch_cbz(intr_0, stop_lbl2);
        }
        if stop_site_ok2 != 0 && to_blr.is_null() {
            stop_extra_add(br_site2, stop_lbl2);
        } else if stop_site_ok2 != 0 && !to_blr.is_null() && br_site2 != to_blr {
            stop_extra_add(br_site2, stop_lbl2);
        }
        let mut to_epi: *mut uint32_t = a64_label(b);
        a64_b(b, 0 as int32_t);
        a64_patch_cbz(to_full, a64_label(b));
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            8 as ::core::ffi::c_ulong as uint32_t,
        );
        let mut nocode: *mut uint32_t = a64_label(b);
        a64_cbz(b, 1 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, 0 as int32_t);
        emit_indirect_leave_br(b, JT0 as ::core::ffi::c_int);
        a64_patch_cbz(nocode, a64_label(b));
        a64_patch_b(to_epi, a64_label(b));
    } else {
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JTF as ::core::ffi::c_int,
            8 as ::core::ffi::c_ulong as uint32_t,
        );
        let mut nocode_0: *mut uint32_t = a64_label(b);
        a64_cbz(b, 1 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, 0 as int32_t);
        emit_indirect_leave_br(b, JT0 as ::core::ffi::c_int);
        a64_patch_cbz(nocode_0, a64_label(b));
    }
    a64_patch_cbz(to_nofind, a64_label(b));
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    let ref mut fresh7 = *epi_sites.offset(*n_epi as isize);
    *fresh7 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
}
unsafe extern "C" fn emit_branch_target(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut o: *const X86Operand,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    g_ind_treg = JT1 as ::core::ffi::c_int;
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
        if (*o).high8 as ::core::ffi::c_int != 0
            || (*o).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        if rsp_is_ptr() != 0
            && (*o).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        if pin_slot((*o).reg as ::core::ffi::c_uint) >= 0 as ::core::ffi::c_int
            && (*o).reg as ::core::ffi::c_int != OCERZ_RSP as ::core::ffi::c_int
            && ({
                static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
                if on_ < 0 as ::core::ffi::c_int {
                    on_ = (libc::getenv(
                        b"OCERZ_NO_IND_TREG\0" as *const u8 as *const ::core::ffi::c_char,
                    ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
                }
                on_
            }) == 0
        {
            g_ind_treg = pin_hreg(pin_slot((*o).reg as ::core::ffi::c_uint));
            return 1 as ::core::ffi::c_int;
        }
        emit_gpr_rd(
            b,
            1 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            (*o).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if (*o).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        if emit_plain_mem_fast(
            b,
            insn,
            o,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        if emit_mem_ea(b, insn, o, JTA as ::core::ffi::c_int) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut skip: *mut uint32_t = emit_commpage_guard(
            b,
            insn,
            JTA as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
        emit_add_const(
            b,
            JTA as ::core::ffi::c_int,
            ocerz_guest_base.wrapping_sub(ea_fold()),
        );
        emit_guest_load_ordered(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
        );
        patch_guard_skip(skip, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe extern "C" fn emit_indirect32(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut o: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize) as *const X86Operand;
    if (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
        || g_no_chain != 0 || (*o).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
        || ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_INLINE_INDIRECT\0" as *const u8
                        as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) != 0
        || ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (libc::getenv(
                    b"OCERZ_NO_M32_RET_TAIL\0" as *const u8 as *const ::core::ffi::c_char,
                ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
            }
            on_
        }) != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int
        && (m32_stack_ok(insn) == 0 || mem_native_store_ok() == 0)
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
        let mut s: ::core::ffi::c_int = pin_slot((*o).reg as ::core::ffi::c_uint);
        if (*o).high8 as ::core::ffi::c_int != 0 || s < 0 as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        a64_mov_reg(b, 0 as ::core::ffi::c_int, JT1 as ::core::ffi::c_int, pin_hreg(s));
    } else if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_mem_ea(b, insn, o, JTA as ::core::ffi::c_int) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut skip: *mut uint32_t = emit_commpage_guard(
            b,
            insn,
            JTA as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
        emit_add_const(
            b,
            JTA as ::core::ffi::c_int,
            ocerz_guest_base.wrapping_sub(ea_fold()),
        );
        emit_guest_load_ordered(
            b,
            4 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            JTU as ::core::ffi::c_int,
        );
        patch_guard_skip(skip, a64_label(b));
    } else {
        return 0 as ::core::ffi::c_int
    }
    let mut retaddr: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t)
        as uint32_t as uint64_t;
    let mut adr_site: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CALL as ::core::ffi::c_int {
        let mut hs: ::core::ffi::c_int = pin_hreg(
            pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
        );
        if m32_ras_ok(insn) != 0 {
            adr_site = m32_ras_push(b, retaddr);
        }
        a64_mov_imm64(b, JT0 as ::core::ffi::c_int, retaddr);
        a64_sub_imm(
            b,
            0 as ::core::ffi::c_int,
            JTA as ::core::ffi::c_int,
            hs,
            4 as uint32_t,
        );
        m32_stack_st(b, JT0 as ::core::ffi::c_int, JTA as ::core::ffi::c_int);
        a64_mov_reg(b, 0 as ::core::ffi::c_int, hs, JTA as ::core::ffi::c_int);
    }
    a64_try_orr_imm(
        b,
        1 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        JIT_KEY_M32 as uint64_t,
    );
    g_ind_treg = JT1 as ::core::ffi::c_int;
    g_ind_m32 = 1 as ::core::ffi::c_int;
    if adr_site.is_null() {
        emit_indirect_tail(b, epi_sites, n_epi);
        return 1 as ::core::ffi::c_int;
    }
    let mut cont: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    g_ind_call_cont = &raw mut cont;
    g_ind_call_tocont = ::core::ptr::null_mut::<uint32_t>();
    emit_indirect_tail(b, epi_sites, n_epi);
    let mut to_cont: *mut uint32_t = g_ind_call_tocont;
    g_ind_call_cont = ::core::ptr::null_mut::<*mut uint32_t>();
    if cont.is_null() || to_cont.is_null()
    {
        __assert_rtn(
            b"emit_indirect32\0" as *const u8 as *const ::core::ffi::c_char,
            b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
            1531 as ::core::ffi::c_int,
            b"cont && to_cont && \"32-bit indirect call: no continuation site\"\0"
                as *const u8 as *const ::core::ffi::c_char,
        );
    } else {};
    patch_local_adr(adr_site, cont, JT0 as ::core::ffi::c_int);
    a64_patch_b(to_cont, a64_label(b));
    let mut pb_ret: *mut uint32_t = emit_body_chain_tail(
        b,
        retaddr,
        0 as ::core::ffi::c_int,
        epi_sites,
        n_epi,
    );
    (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = retaddr;
    (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb_ret;
    (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
        uint32_t,
    >();
    (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = EDGE_BODY as ::core::ffi::c_int
        as uint8_t;
    (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
    g_n_jcc_edges = 1 as ::core::ffi::c_int;
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_indirect_jmp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).op as ::core::ffi::c_int != OCERZ_OP_JMP as ::core::ffi::c_int
        || (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            == OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).mode32 != 0 {
        return emit_indirect32(b, insn, exit_sites, n_exits, epi_sites, n_epi);
    }
    if ({
        static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        if on_ < 0 as ::core::ffi::c_int {
            on_ = (libc::getenv(
                b"OCERZ_NO_INLINE_INDIRECT\0" as *const u8 as *const ::core::ffi::c_char,
            ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
        }
        on_
    }) != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if ({
        static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        if on_ < 0 as ::core::ffi::c_int {
            on_ = (libc::getenv(
                b"OCERZ_EXP_MAT_IND\0" as *const u8 as *const ::core::ffi::c_char,
            ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
        }
        on_
    }) != 0
    {
        emit_materialize(b);
    }
    if emit_branch_target(
        b,
        insn,
        (&raw const (*insn).ops as *const X86Operand)
            .offset(0 as ::core::ffi::c_int as isize) as *const X86Operand,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    emit_indirect_tail(b, epi_sites, n_epi);
    return 1 as ::core::ffi::c_int;
}
unsafe extern "C" fn bridge_fastcall_enabled() -> ::core::ffi::c_int {
    static mut en: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if en < 0 as ::core::ffi::c_int {
        en = (ocerz_mode == OCERZ_MODE_NATIVE as ::core::ffi::c_int
            && libc::getenv(
                    b"OCERZ_NO_BRIDGE_FASTCALL\0" as *const u8
                        as *const ::core::ffi::c_char,
                )
                .is_null()) as ::core::ffi::c_int;
    }
    return en;
}
unsafe extern "C" fn leaf_inplace_enabled() -> ::core::ffi::c_int {
    static mut en: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if en < 0 as ::core::ffi::c_int {
        en = (libc::getenv(
                b"OCERZ_NO_LEAF_INPLACE\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
            && libc::getenv(b"OCERZ_BRIDGESTAT\0" as *const u8 as *const ::core::ffi::c_char)
                .is_null()
            && libc::getenv(b"OCERZ_BRIDGELOG\0" as *const u8 as *const ::core::ffi::c_char)
                .is_null()) as ::core::ffi::c_int;
    }
    return en;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_leaf_call_ret(
    mut b: *mut A64Buf,
    mut leaf: *const ::core::ffi::c_void,
    mut writes: ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> *mut uint32_t {
    if writes != 0 {
        tc_imm64(
            b,
            JT0 as ::core::ffi::c_int,
            TCR_SYM as ::core::ffi::c_int,
            TCS_RETIRE_COUNT as ::core::ffi::c_int as uint64_t,
            &raw mut ocerz_jit_retire_count as uintptr_t as uint64_t,
        );
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            0 as uint32_t,
        );
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            LEAF_EPOCH_OFF,
        );
    }
    let mut leaf_at: *const ::core::ffi::c_char = if !g_xlat_jit.is_null()
        && !(*g_xlat_jit).leaf_near.is_null()
    {
        (*g_xlat_jit)
            .leaf_near
            .offset(
                (leaf as *const ::core::ffi::c_char)
                    .offset_from(&raw const ocerz_leaf_lo as *const ::core::ffi::c_char)
                    as ::core::ffi::c_long as isize,
            )
    } else {
        leaf as *const ::core::ffi::c_char
    };
    let mut leaf_words: int64_t = (leaf_at
        .offset_from((*b).p as *const ::core::ffi::c_char) as ::core::ffi::c_long
        / 4 as ::core::ffi::c_long) as int64_t;
    if g_tc_on != 0 {
        tc_imm64(
            b,
            16 as ::core::ffi::c_int,
            TCR_LEAF as ::core::ffi::c_int,
            (leaf as *const ::core::ffi::c_char)
                .offset_from(&raw const ocerz_leaf_lo as *const ::core::ffi::c_char)
                as ::core::ffi::c_long as uint64_t,
            leaf_at as uintptr_t as uint64_t,
        );
        a64_blr(b, 16 as ::core::ffi::c_int);
    } else if leaf_words
        > -((1 as ::core::ffi::c_int) << 25 as ::core::ffi::c_int) as int64_t
        && leaf_words
            < ((1 as ::core::ffi::c_int) << 25 as ::core::ffi::c_int) as int64_t
    {
        a64_emit32(
            b,
            0x94000000 as uint32_t | leaf_words as uint32_t & 0x3ffffff as uint32_t,
        );
    } else {
        a64_mov_imm64(b, 16 as ::core::ffi::c_int, leaf_at as uintptr_t as uint64_t);
        a64_blr(b, 16 as ::core::ffi::c_int);
    }
    g_callout_seq = g_callout_seq.wrapping_add(1);
    let mut declined: *mut uint32_t = a64_label(b);
    a64_cbnz(b, 1 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, 0 as int32_t);
    emit_reload_mem_base(b);
    a64_ldr_post64(
        b,
        JT1 as ::core::ffi::c_int,
        pin_hreg(pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)),
        8 as ::core::ffi::c_int,
    );
    let mut retired: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if writes != 0 {
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            LEAF_EPOCH_OFF,
        );
        tc_imm64(
            b,
            JT0 as ::core::ffi::c_int,
            TCR_SYM as ::core::ffi::c_int,
            TCS_RETIRE_COUNT as ::core::ffi::c_int as uint64_t,
            &raw mut ocerz_jit_retire_count as uintptr_t as uint64_t,
        );
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            0 as uint32_t,
        );
        a64_subs_reg(
            b,
            1 as ::core::ffi::c_int,
            A64_ZR,
            JT0 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        retired = a64_label(b);
        a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
    }
    a64_ldp_post(
        b,
        JTF as ::core::ffi::c_int,
        30 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    a64_subs_reg(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JTF as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let mut miss_ne: *mut uint32_t = a64_label(b);
    a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
    let mut miss_z: *mut uint32_t = a64_label(b);
    a64_cbz(b, 1 as ::core::ffi::c_int, 30 as ::core::ffi::c_int, 0 as int32_t);
    if xmm_global_enabled() == 0 {
        emit_xmm_pin_spill_all(b);
    }
    a64_ret(b);
    let mut miss: *mut uint32_t = a64_label(b);
    a64_patch_bcond(miss_ne, miss);
    a64_patch_cbz(miss_z, miss);
    if !retired.is_null() {
        a64_patch_bcond(retired, miss);
    }
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    let ref mut fresh17 = *epi_sites.offset(*n_epi as isize);
    *fresh17 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    return declined;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn leaf_layout_ok() -> ::core::ffi::c_int {
    static mut no_blret: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if no_blret < 0 as ::core::ffi::c_int {
        no_blret = if !libc::getenv(
                b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    let mut fast3: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
        && jgb_usable() != 0 && stack_guard_needed() == 0) as ::core::ffi::c_int;
    return (fast3 != 0 && ras_body_only() != 0 && host_ras_enabled() != 0
        && no_blret == 0 && g_xlat_mode32 == 0 && leaf_inplace_enabled() != 0
        && ocerz_guest_base == 0 as uint64_t
        && !(ocerz_low_base != 0
            && ocerz_mode == OCERZ_MODE_NATIVE as ::core::ffi::c_int)
        && pin_slot(OCERZ_RAX as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
        && pin_slot(OCERZ_RDI as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
        && pin_slot(OCERZ_RSI as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
        && pin_slot(OCERZ_RDX as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
        && pin_hreg(pin_slot(OCERZ_RAX as ::core::ffi::c_int as ::core::ffi::c_uint))
            == 21 as ::core::ffi::c_int
        && pin_hreg(pin_slot(OCERZ_RDI as ::core::ffi::c_int as ::core::ffi::c_uint))
            == 28 as ::core::ffi::c_int
        && pin_hreg(pin_slot(OCERZ_RSI as ::core::ffi::c_int as ::core::ffi::c_uint))
            == 27 as ::core::ffi::c_int
        && pin_hreg(pin_slot(OCERZ_RDX as ::core::ffi::c_int as ::core::ffi::c_uint))
            == 23 as ::core::ffi::c_int) as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_bridge_fastcall(
    mut b: *mut A64Buf,
    mut insns: *const X86Insn,
    mut i: ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) {
    let mut insn: *const X86Insn = insns.offset(i as isize) as *const X86Insn;
    if bridge_fastcall_enabled() == 0 || g_xlat_mode32 != 0
        || i < 1 as ::core::ffi::c_int
    {
        return;
    }
    if (*insn).op as ::core::ffi::c_int != OCERZ_OP_JMP as ::core::ffi::c_int
        || (*insn).nops as ::core::ffi::c_int != 1 as ::core::ffi::c_int
        || (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
    {
        return;
    }
    let mut o: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize) as *const X86Operand;
    if (*o).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
        || (*o).riprel == 0 || (*o).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
    {
        return;
    }
    let mut mv: *const X86Insn = insns.offset((i - 1 as ::core::ffi::c_int) as isize)
        as *const X86Insn;
    if (*mv).op as ::core::ffi::c_int != OCERZ_OP_MOV as ::core::ffi::c_int
        || (*mv).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*mv).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
        || (*mv).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*mv).ops.get_unchecked(0 as ::core::ffi::c_int as usize).reg as ::core::ffi::c_int
            != OCERZ_R11 as ::core::ffi::c_int
        || (*mv).ops.get_unchecked(0 as ::core::ffi::c_int as usize).size as ::core::ffi::c_int
            != 4 as ::core::ffi::c_int
        || (*mv).ops.get_unchecked(0 as ::core::ffi::c_int as usize).high8 as ::core::ffi::c_int != 0
        || (*mv).ops.get_unchecked(1 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return;
    }
    let trap: uint64_t = (OCERZ_DYLDAPI_LO as uint64_t)
        .wrapping_add(OCERZ_BRIDGE_OFF as uint64_t);
    let mut slot: uint64_t = (*o).disp as uint64_t;
    if ocerz_addr_readable(slot) == 0
        || ocerz_addr_readable(slot.wrapping_add(7 as uint64_t)) == 0
        || ocerz_ld(slot, 8 as ::core::ffi::c_int) != trap
    {
        return;
    }
    let mut id: uint64_t = (*mv).ops.get_unchecked(1 as ::core::ffi::c_int as usize).imm as uint32_t
        as uint64_t;
    if ocerz_vdylib_export_name(
        id,
        ::core::ptr::null_mut::<*const ::core::ffi::c_char>(),
        ::core::ptr::null_mut::<*const ::core::ffi::c_char>(),
    ) == 0 || ocerz_vdylib_trap_only(id) != 0
    {
        return;
    }
    let mut xin: uint16_t = 0xffff as uint16_t;
    let mut xout: uint16_t = 0xffff as uint16_t;
    if xmm_global_enabled() == 0
        || ocerz_vdylib_xmm_contract(id, &raw mut xin, &raw mut xout) == 0
    {
        xout = 0xffff as uint16_t;
        xin = xout;
    }
    static mut no_blret: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if no_blret < 0 as ::core::ffi::c_int {
        no_blret = if !libc::getenv(
                b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    let mut fast3: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
        && jgb_usable() != 0 && stack_guard_needed() == 0) as ::core::ffi::c_int;
    if fast3 == 0 || ras_body_only() == 0 || host_ras_enabled() == 0 || no_blret != 0 {
        return;
    }
    let mut leaf_limit: uint64_t = 0 as uint64_t;
    let mut leaf: *const ::core::ffi::c_void = if leaf_layout_ok() != 0 {
        ocerz_vdylib_leaf(id, &raw mut leaf_limit)
    } else {
        ::core::ptr::null::<::core::ffi::c_void>()
    };
    l0_flush_all(b);
    if !leaf.is_null() {
        let mut leaf_sym: *const ::core::ffi::c_char = ::core::ptr::null::<
            ::core::ffi::c_char,
        >();
        if ocerz_verbose >= 1 as ::core::ffi::c_int
            && ocerz_vdylib_export_name(
                id,
                ::core::ptr::null_mut::<*const ::core::ffi::c_char>(),
                &raw mut leaf_sym,
            ) != 0
        {
            if ocerz_verbose >= 1 as ::core::ffi::c_int {
                fprintf(
                    crate::log::stderr(),
                    b"ocerz: jit: %s is answered in place at %#llx by %p from %p\n\0"
                        as *const u8 as *const ::core::ffi::c_char,
                    leaf_sym,
                    (*insn).rip as ::core::ffi::c_ulonglong,
                    leaf,
                    (*b).p as *mut ::core::ffi::c_void,
                );
            }
        }
        let mut leaf_out: [*mut uint32_t; 3] = [::core::ptr::null_mut::<uint32_t>(); 3];
        let mut leaf_out_cb: [uint8_t; 3] = [
            0 as ::core::ffi::c_int as uint8_t,
            0 as ::core::ffi::c_int as uint8_t,
            0 as ::core::ffi::c_int as uint8_t,
        ];
        let mut n_leaf_out: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        a64_mov_imm64(b, JT1 as ::core::ffi::c_int, slot.wrapping_add(ocerz_guest_base));
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            0 as uint32_t,
        );
        a64_mov_imm64(b, JT2 as ::core::ffi::c_int, trap);
        a64_subs_reg(
            b,
            1 as ::core::ffi::c_int,
            A64_ZR,
            JT1 as ::core::ffi::c_int,
            JT2 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        let fresh18 = n_leaf_out;
        n_leaf_out = n_leaf_out + 1;
        *leaf_out.get_unchecked_mut(fresh18 as usize) = a64_label(b);
        a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
        if leaf_limit != 0 {
            a64_mov_imm64(b, JT0 as ::core::ffi::c_int, leaf_limit);
            a64_subs_reg(
                b,
                1 as ::core::ffi::c_int,
                A64_ZR,
                23 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            let fresh19 = n_leaf_out;
            n_leaf_out = n_leaf_out + 1;
            *leaf_out.get_unchecked_mut(fresh19 as usize) = a64_label(b);
            a64_bcond(b, A64_HI as ::core::ffi::c_int, 0 as int32_t);
        }
        *leaf_out_cb.get_unchecked_mut(n_leaf_out as usize) = 1 as uint8_t;
        let fresh20 = n_leaf_out;
        n_leaf_out = n_leaf_out + 1;
        *leaf_out.get_unchecked_mut(fresh20 as usize) = emit_leaf_call_ret(
            b,
            leaf,
            (leaf_limit != 0 as uint64_t) as ::core::ffi::c_int,
            epi_sites,
            n_epi,
        );
        let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while k < n_leaf_out {
            if *leaf_out_cb.get_unchecked(k as usize) != 0 {
                a64_patch_cbz(*leaf_out.get_unchecked(k as usize), a64_label(b));
            } else {
                a64_patch_bcond(*leaf_out.get_unchecked(k as usize), a64_label(b));
            }
            k += 1;
        }
    }
    a64_mov_imm64(b, JT1 as ::core::ffi::c_int, slot.wrapping_add(ocerz_guest_base));
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as uint32_t,
    );
    a64_mov_imm64(b, JT2 as ::core::ffi::c_int, trap);
    a64_subs_reg(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JT1 as ::core::ffi::c_int,
        JT2 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let mut to_plain: *mut uint32_t = a64_label(b);
    a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        JIT_FP_OFF,
    );
    a64_add_imm(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        0 as uint32_t,
    );
    a64_sub_reg(
        b,
        1 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_subs_imm_sh12(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JT0 as ::core::ffi::c_int,
        BRIDGE_FAST_DEPTH_MAX_K as uint32_t,
    );
    let mut to_deep: *mut uint32_t = a64_label(b);
    a64_bcond(b, A64_HI as ::core::ffi::c_int, 0 as int32_t);
    g_ymmh_zero = 0 as uint16_t;
    let mut r: ::core::ffi::c_uint = 0 as ::core::ffi::c_uint;
    while r < 16 as ::core::ffi::c_uint {
        if xmm_is_pinned(r) != 0
            && xin as ::core::ffi::c_int >> r & 1 as ::core::ffi::c_int != 0
        {
            a64_str_v(
                b,
                16 as ::core::ffi::c_int,
                xmm_vreg(r),
                20 as ::core::ffi::c_int,
                XMM_BASE_OFF.wrapping_add((r as uint32_t).wrapping_mul(16 as uint32_t)),
            );
        }
        r = r.wrapping_add(1);
    }
    emit_spill_pinned(b);
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        19 as ::core::ffi::c_int,
    );
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
    );
    g_tc_bad = 1 as ::core::ffi::c_int;
    a64_mov_imm64(
        b,
        16 as ::core::ffi::c_int,
        ::core::mem::transmute::<
            Option<
                unsafe extern "C" fn(*mut OcerzVM, *mut OcerzCPU) -> ::core::ffi::c_int,
            >,
            uintptr_t,
        >(
            Some(
                ocerz_vdylib_fastcall
                    as unsafe extern "C" fn(
                        *mut OcerzVM,
                        *mut OcerzCPU,
                    ) -> ::core::ffi::c_int,
            ),
        ) as uint64_t,
    );
    a64_blr(b, 16 as ::core::ffi::c_int);
    g_callout_seq = g_callout_seq.wrapping_add(1);
    a64_mov_reg(
        b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    emit_fill_pinned(b);
    let mut to_leave: *mut uint32_t = a64_label(b);
    a64_cbnz(b, 0 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, 0 as int32_t);
    let mut r_0: ::core::ffi::c_uint = 0 as ::core::ffi::c_uint;
    while r_0 < 16 as ::core::ffi::c_uint {
        if xmm_is_pinned(r_0) != 0
            && xout as ::core::ffi::c_int >> r_0 & 1 as ::core::ffi::c_int != 0
        {
            a64_ldr_v(
                b,
                16 as ::core::ffi::c_int,
                xmm_vreg(r_0),
                20 as ::core::ffi::c_int,
                XMM_BASE_OFF.wrapping_add((r_0 as uint32_t).wrapping_mul(16 as uint32_t)),
            );
        }
        r_0 = r_0.wrapping_add(1);
    }
    emit_pk_consts_load(b);
    yc_reload_all(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    a64_ldp_post(
        b,
        JTF as ::core::ffi::c_int,
        30 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    a64_subs_reg(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR,
        JTF as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    let mut miss_ne: *mut uint32_t = a64_label(b);
    a64_bcond(b, A64_NE as ::core::ffi::c_int, 0 as int32_t);
    let mut miss_z: *mut uint32_t = a64_label(b);
    a64_cbz(b, 1 as ::core::ffi::c_int, 30 as ::core::ffi::c_int, 0 as int32_t);
    if xmm_global_enabled() == 0 {
        emit_xmm_pin_spill_all(b);
    }
    a64_ret(b);
    let mut miss: *mut uint32_t = a64_label(b);
    a64_patch_bcond(miss_ne, miss);
    a64_patch_cbz(miss_z, miss);
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    let ref mut fresh21 = *epi_sites.offset(*n_epi as isize);
    *fresh21 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    a64_patch_cbz(to_leave, a64_label(b));
    emit_xmm_pin_load_all(b);
    yc_reload_all(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    a64_sub_imm(
        b,
        0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        1 as uint32_t,
    );
    let ref mut fresh22 = *epi_sites.offset(*n_epi as isize);
    *fresh22 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    let mut plain: *mut uint32_t = a64_label(b);
    a64_patch_bcond(to_plain, plain);
    a64_patch_bcond(to_deep, plain);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_indirect_call(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
    mut epi_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    g_dbg_ind_src = (*insn).rip;
    if (*insn).op as ::core::ffi::c_int != OCERZ_OP_CALL as ::core::ffi::c_int
        || (*insn).ops.get_unchecked(0 as ::core::ffi::c_int as usize).kind as ::core::ffi::c_int
            == OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).mode32 != 0 {
        return emit_indirect32(b, insn, exit_sites, n_exits, epi_sites, n_epi);
    }
    if ({
        static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        if on_ < 0 as ::core::ffi::c_int {
            on_ = (libc::getenv(
                b"OCERZ_NO_INLINE_INDIRECT\0" as *const u8 as *const ::core::ffi::c_char,
            ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
        }
        on_
    }) != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
        || mem_native_store_ok() == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if ({
        static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        if on_ < 0 as ::core::ffi::c_int {
            on_ = (libc::getenv(
                b"OCERZ_EXP_MAT_IND\0" as *const u8 as *const ::core::ffi::c_char,
            ) != NULL as *mut ::core::ffi::c_char) as ::core::ffi::c_int;
        }
        on_
    }) != 0
    {
        emit_materialize(b);
    }
    if emit_branch_target(
        b,
        insn,
        (&raw const (*insn).ops as *const X86Operand)
            .offset(0 as ::core::ffi::c_int as isize) as *const X86Operand,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut retaddr: uint64_t = (*insn).rip.wrapping_add((*insn).len as uint64_t);
    static mut no_blret_i: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if no_blret_i < 0 as ::core::ffi::c_int {
        no_blret_i = if !libc::getenv(
                b"OCERZ_NO_BLRET\0" as *const u8 as *const ::core::ffi::c_char,
            )
            .is_null()
        {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    let mut fast3: ::core::ffi::c_int = (g_pin_class == 3 as ::core::ffi::c_int
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int && stack_plain_access_ok() != 0
        && stack_fast() != 0 && g_no_chain == 0 && g_no_ras == 0 && ras_body_only() != 0
        && no_blret_i == 0) as ::core::ffi::c_int;
    if fast3 != 0 {
        let mut hs: ::core::ffi::c_int = pin_hreg(
            pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
        );
        emit_const_lit(b, JT2 as ::core::ffi::c_int, retaddr);
        let mut adr_site: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
        if host_ras_enabled() != 0 {
            adr_site = a64_label(b);
            a64_emit32(
                b,
                0x10000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t,
            );
            a64_stp_pre(
                b,
                JT2 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                31 as ::core::ffi::c_int,
                -(16 as ::core::ffi::c_int),
            );
            emit_stack_push64(b, insn, hs, JT2 as ::core::ffi::c_int);
        } else {
            emit_stack_push64(b, insn, hs, JT2 as ::core::ffi::c_int);
            a64_ldr(
                b,
                4 as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RAS_TOP_OFF,
            );
            adr_site = a64_label(b);
            a64_emit32(
                b,
                0x10000000 as uint32_t | JT0 as ::core::ffi::c_int as uint32_t,
            );
            a64_and_imm_or_mov(
                b,
                0 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                (OCERZ_RAS_SIZE - 1 as ::core::ffi::c_int) as uint64_t,
            );
            a64_add_reg(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                JTU as ::core::ffi::c_int,
                4 as ::core::ffi::c_int,
            );
            if RAS_OFF <= 504 as uint32_t {
                a64_stp_off(
                    b,
                    JT2 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF as ::core::ffi::c_int,
                );
            } else {
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF,
                );
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    JTA as ::core::ffi::c_int,
                    RAS_OFF.wrapping_add(8 as uint32_t),
                );
            }
            a64_add_imm(
                b,
                0 as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                1 as uint32_t,
            );
            a64_str(
                b,
                4 as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RAS_TOP_OFF,
            );
        }
        let mut cont: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
        g_ind_call_cont = &raw mut cont;
        g_ind_call_tocont = ::core::ptr::null_mut::<uint32_t>();
        emit_indirect_tail(b, epi_sites, n_epi);
        let mut to_cont: *mut uint32_t = g_ind_call_tocont;
        g_ind_call_cont = ::core::ptr::null_mut::<*mut uint32_t>();
        if !cont.is_null() && !to_cont.is_null() {
            patch_local_adr(adr_site, cont, JT0 as ::core::ffi::c_int);
            a64_patch_b(to_cont, a64_label(b));
            let mut pb_ret: *mut uint32_t = emit_body_chain_tail(
                b,
                retaddr,
                0 as ::core::ffi::c_int,
                epi_sites,
                n_epi,
            );
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).target_rip = retaddr;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).patch_b = pb_ret;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).cond_site = ::core::ptr::null_mut::<
                uint32_t,
            >();
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).kind = EDGE_BODY
                as ::core::ffi::c_int as uint8_t;
            (*(&raw mut g_jcc_edge)).get_unchecked_mut(0 as ::core::ffi::c_int as usize).pin_class = 3 as uint8_t;
            g_n_jcc_edges = 1 as ::core::ffi::c_int;
        } else {

                __assert_rtn(
                    b"emit_indirect_call\0" as *const u8 as *const ::core::ffi::c_char,
                    b"jit_control.c\0" as *const u8 as *const ::core::ffi::c_char,
                    1830 as ::core::ffi::c_int,
                    b"0 && \"indirect call: no continuation site\"\0" as *const u8
                        as *const ::core::ffi::c_char,
                );
        }
        return 1 as ::core::ffi::c_int;
    }
    a64_mov_imm64(b, JT2 as ::core::ffi::c_int, retaddr);
    emit_gpr_rd(
        b,
        1 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
    );
    a64_sub_imm(
        b,
        1 as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        8 as uint32_t,
    );
    emit_add_const(b, JTA as ::core::ffi::c_int, ea_fold());
    let mut skip: *mut uint32_t = emit_commpage_guard(
        b,
        insn,
        JTA as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    emit_add_const(
        b,
        JTA as ::core::ffi::c_int,
        ocerz_guest_base.wrapping_sub(ea_fold()),
    );
    g_ea_plain = stack_plain_now();
    emit_guest_store_ordered(
        b,
        8 as ::core::ffi::c_int,
        JT2 as ::core::ffi::c_int,
        JTA as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
    );
    a64_sub_imm(
        b,
        1 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        8 as uint32_t,
    );
    emit_gpr_wr(
        b,
        JT0 as ::core::ffi::c_int,
        OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint,
    );
    patch_guard_skip(skip, a64_label(b));
    if g_no_ras == 0 {
        let mut rslot: *mut *mut ::core::ffi::c_void = ras_slot_alloc();
        if !rslot.is_null() {
            let mut rb: *mut JitBlock = cache_lookup(g_xlat_jit, retaddr, g_xlat_mode32);
            if !rb.is_null() && (*rb).code.is_some() {
                *rslot = ras_entry_for(rb);
            } else {
                pending_add_ras(jit_key(retaddr, g_xlat_mode32), rslot);
            }
            a64_ldr(
                b,
                4 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RAS_TOP_OFF,
            );
            a64_subs_imm(
                b,
                0 as ::core::ffi::c_int,
                A64_ZR,
                JT2 as ::core::ffi::c_int,
                OCERZ_RAS_SIZE as uint32_t,
            );
            let mut full: *mut uint32_t = a64_label(b);
            a64_bcond(b, A64_CS as ::core::ffi::c_int, 0 as int32_t);
            a64_mov_imm64(b, JTF as ::core::ffi::c_int, retaddr);
            tc_imm64(
                b,
                JTA as ::core::ffi::c_int,
                TCR_RASSLOT as ::core::ffi::c_int,
                retaddr,
                rslot as uintptr_t as uint64_t,
            );
            a64_ldr(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                0 as uint32_t,
            );
            a64_lsl_imm(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                4 as ::core::ffi::c_int,
            );
            a64_add_reg(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JTF as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                RAS_OFF,
            );
            a64_str(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                RAS_OFF.wrapping_add(8 as uint32_t),
            );
            a64_add_imm(
                b,
                0 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                1 as uint32_t,
            );
            a64_str(
                b,
                4 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                RAS_TOP_OFF,
            );
            a64_patch_bcond(full, a64_label(b));
        }
    }
    emit_indirect_tail(b, epi_sites, n_epi);
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_slowcall(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
) {
    if !g_pe_insns.is_null() && g_n_pe_real != 0
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
    {
        let mut hsp: ::core::ffi::c_int = pin_hreg(
            pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
        );
        let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i < g_n_pe_real {
            let mut p: *const JitBlock_JitPushElide = (&raw mut g_pe_real as *mut JitBlock_JitPushElide)
                .offset(i as isize) as *mut JitBlock_JitPushElide;
            if !(g_cur_insn_idx as int32_t <= (*p).ci
                || g_cur_insn_idx as int32_t >= (*p).rj)
            {
                let mut delta: int64_t = 0 as int64_t;
                let mut m: ::core::ffi::c_int = (*p).ci as ::core::ffi::c_int
                    + 1 as ::core::ffi::c_int;
                while m < g_cur_insn_idx {
                    let mut op2: ::core::ffi::c_uint = (*g_pe_insns.offset(m as isize))
                        .op as ::core::ffi::c_uint;
                    if op2 == OCERZ_OP_PUSH as ::core::ffi::c_int as ::core::ffi::c_uint
                        || op2
                            == OCERZ_OP_CALL as ::core::ffi::c_int as ::core::ffi::c_uint
                    {
                        delta -= 8 as int64_t;
                    } else if op2
                        == OCERZ_OP_POP as ::core::ffi::c_int as ::core::ffi::c_uint
                        || op2
                            == OCERZ_OP_RET as ::core::ffi::c_int as ::core::ffi::c_uint
                    {
                        delta += 8 as int64_t;
                    }
                    m += 1;
                }
                a64_mov_imm64(b, JT1 as ::core::ffi::c_int, (*p).ra);
                a64_str(
                    b,
                    8 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    hsp,
                    -delta as uint32_t,
                );
            }
            i += 1;
        }
    }
    l0_flush_all(b);
    emit_materialize(b);
    g_ymmh_zero = 0 as uint16_t;
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        19 as ::core::ffi::c_int,
    );
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
    );
    let mut base: *const X86Insn = if !g_cur_blk.is_null() {
        (*g_cur_blk).insns
    } else {
        ::core::ptr::null_mut::<X86Insn>()
    };
    let mut idx: ptrdiff_t = if !base.is_null() && insn >= base
        && insn < base.offset((*g_cur_blk).n_insns as isize)
    {
        insn.offset_from(base) as ptrdiff_t
    } else {
        -(1 as ::core::ffi::c_int) as ptrdiff_t
    };
    if idx >= 0 as ptrdiff_t && !g_keep.is_null() && idx < g_keep_n as ptrdiff_t
        && g_slow_run_last as ptrdiff_t >= idx && g_slow_run_last < g_keep_n
    {
        let mut k: ::core::ffi::c_int = idx as ::core::ffi::c_int;
        while k <= g_slow_run_last {
            *g_keep.offset(k as isize) = 1 as uint8_t;
            k += 1;
        }
        tc_imm64(
            b,
            2 as ::core::ffi::c_int,
            TCR_BLK as ::core::ffi::c_int,
            0 as uint64_t,
            g_cur_blk as uintptr_t as uint64_t,
        );
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            3 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            JIT_SCRATCH_OFF,
        );
        a64_mov_imm64(b, 4 as ::core::ffi::c_int, g_slow_run_last as uint64_t);
        tc_imm64(
            b,
            16 as ::core::ffi::c_int,
            TCR_SYM as ::core::ffi::c_int,
            TCS_EXEC_RUN_AT as ::core::ffi::c_int as uint64_t,
            ::core::mem::transmute::<
                Option<
                    unsafe extern "C" fn(
                        *mut OcerzVM,
                        *mut OcerzCPU,
                        *const JitBlock,
                        uint64_t,
                        uint64_t,
                    ) -> ::core::ffi::c_int,
                >,
                uintptr_t,
            >(
                Some(
                    ocerz_jit_exec_run_at
                        as unsafe extern "C" fn(
                            *mut OcerzVM,
                            *mut OcerzCPU,
                            *const JitBlock,
                            uint64_t,
                            uint64_t,
                        ) -> ::core::ffi::c_int,
                ),
            ) as uint64_t,
        );
    } else if idx >= 0 as ptrdiff_t && !g_keep.is_null() && idx < g_keep_n as ptrdiff_t {
        *g_keep.offset(idx as isize) = 1 as uint8_t;
        tc_imm64(
            b,
            2 as ::core::ffi::c_int,
            TCR_BLK as ::core::ffi::c_int,
            0 as uint64_t,
            g_cur_blk as uintptr_t as uint64_t,
        );
        a64_mov_imm64(b, 3 as ::core::ffi::c_int, idx as uint64_t);
        tc_imm64(
            b,
            16 as ::core::ffi::c_int,
            TCR_SYM as ::core::ffi::c_int,
            TCS_EXEC_ONE_AT as ::core::ffi::c_int as uint64_t,
            ::core::mem::transmute::<
                Option<
                    unsafe extern "C" fn(
                        *mut OcerzVM,
                        *mut OcerzCPU,
                        *const JitBlock,
                        uint64_t,
                    ) -> ::core::ffi::c_int,
                >,
                uintptr_t,
            >(
                Some(
                    ocerz_jit_exec_one_at
                        as unsafe extern "C" fn(
                            *mut OcerzVM,
                            *mut OcerzCPU,
                            *const JitBlock,
                            uint64_t,
                        ) -> ::core::ffi::c_int,
                ),
            ) as uint64_t,
        );
    } else {
        g_no_compact = 1 as ::core::ffi::c_int;
        if idx >= 0 as ptrdiff_t {
            tc_imm64(
                b,
                2 as ::core::ffi::c_int,
                TCR_INSN as ::core::ffi::c_int,
                idx as uint64_t,
                insn as uintptr_t as uint64_t,
            );
        } else {
            g_tc_bad = 1 as ::core::ffi::c_int;
            a64_mov_imm64(b, 2 as ::core::ffi::c_int, insn as uintptr_t as uint64_t);
        }
        tc_imm64(
            b,
            16 as ::core::ffi::c_int,
            TCR_SYM as ::core::ffi::c_int,
            TCS_EXEC_ONE as ::core::ffi::c_int as uint64_t,
            ::core::mem::transmute::<
                Option<
                    unsafe extern "C" fn(
                        *mut OcerzVM,
                        *mut OcerzCPU,
                        *const X86Insn,
                    ) -> ::core::ffi::c_int,
                >,
                uintptr_t,
            >(
                Some(
                    ocerz_jit_exec_one
                        as unsafe extern "C" fn(
                            *mut OcerzVM,
                            *mut OcerzCPU,
                            *const X86Insn,
                        ) -> ::core::ffi::c_int,
                ),
            ) as uint64_t,
        );
    }
    a64_blr(b, 16 as ::core::ffi::c_int);
    g_callout_seq = g_callout_seq.wrapping_add(1);
    emit_fill_pinned(b);
    emit_xmm_pin_load_all(b);
    yc_reload_all(b);
    let ref mut fresh23 = *exit_sites.offset(*n_exits as isize);
    *fresh23 = a64_label(b);
    a64_cbnz(b, 0 as ::core::ffi::c_int, 0 as ::core::ffi::c_int, 0 as int32_t);
    *n_exits += 1;
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    if !g_pe_insns.is_null() && g_n_promo_real != 0
        && pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint)
            >= 0 as ::core::ffi::c_int
    {
        let mut hsp_0: ::core::ffi::c_int = pin_hreg(
            pin_slot(OCERZ_RSP as ::core::ffi::c_int as ::core::ffi::c_uint),
        );
        let mut i_0: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i_0 < g_n_promo_real {
            let mut p_0: *const JitPromo = (&raw mut g_promo_real as *mut JitPromo)
                .offset(i_0 as isize) as *mut JitPromo;
            if !(g_cur_insn_idx as int32_t <= (*p_0).pi
                || g_cur_insn_idx as int32_t >= (*p_0).qi)
            {
                let mut delta_0: int64_t = 0 as int64_t;
                let mut m_0: ::core::ffi::c_int = (*p_0).pi as ::core::ffi::c_int
                    + 1 as ::core::ffi::c_int;
                while m_0 < g_cur_insn_idx {
                    let mut op2_0: ::core::ffi::c_uint = (*g_pe_insns
                        .offset(m_0 as isize))
                        .op as ::core::ffi::c_uint;
                    if op2_0
                        == OCERZ_OP_PUSH as ::core::ffi::c_int as ::core::ffi::c_uint
                        || op2_0
                            == OCERZ_OP_CALL as ::core::ffi::c_int as ::core::ffi::c_uint
                    {
                        delta_0 -= 8 as int64_t;
                    } else if op2_0
                        == OCERZ_OP_POP as ::core::ffi::c_int as ::core::ffi::c_uint
                        || op2_0
                            == OCERZ_OP_RET as ::core::ffi::c_int as ::core::ffi::c_uint
                    {
                        delta_0 += 8 as int64_t;
                    }
                    m_0 += 1;
                }
                a64_ldr(
                    b,
                    8 as ::core::ffi::c_int,
                    (*p_0).hreg as ::core::ffi::c_int,
                    hsp_0,
                    -delta_0 as uint32_t,
                );
            }
            i_0 += 1;
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_body_chain_tail(
    mut b: *mut A64Buf,
    mut target_rip: uint64_t,
    mut poll: ::core::ffi::c_int,
    mut epilogue_sites: *mut *mut uint32_t,
    mut n_epi: *mut ::core::ffi::c_int,
) -> *mut uint32_t {
    let mut patch_stop: ::core::ffi::c_int = (poll != 0 && g_stop_patch.is_null())
        as ::core::ffi::c_int;
    let mut extra_stop: ::core::ffi::c_int = (poll != 0 && patch_stop == 0
        && g_n_stop_extra < 6 as ::core::ffi::c_int) as ::core::ffi::c_int;
    let mut intr: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if xmm_global_enabled() == 0 {
        emit_xmm_pin_spill_all(b);
    }
    if poll != 0 && patch_stop == 0 && extra_stop == 0 {
        a64_ldr(
            b,
            4 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            INT_OFF,
        );
        intr = a64_label(b);
        a64_cbnz(b, 0 as ::core::ffi::c_int, JT1 as ::core::ffi::c_int, 0 as int32_t);
    }
    let mut patch_b: *mut uint32_t = a64_label(b);
    a64_b(b, 0 as int32_t);
    let mut fallback: *mut uint32_t = a64_label(b);
    a64_patch_b(patch_b, fallback);
    if !intr.is_null() {
        a64_patch_cbz(intr, fallback);
    }
    if patch_stop != 0 {
        g_stop_patch = patch_b;
        g_stop_target = fallback;
    } else if extra_stop != 0 {
        stop_extra_add(patch_b, fallback);
    }
    if g_pin_class == 2 as ::core::ffi::c_int {
        a64_add_imm(
            b,
            1 as ::core::ffi::c_int,
            31 as ::core::ffi::c_int,
            29 as ::core::ffi::c_int,
            0 as uint32_t,
        );
    }
    emit_side_tag(b, 20 as ::core::ffi::c_int);
    a64_mov_imm64(b, JT0 as ::core::ffi::c_int, target_rip);
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RIP_OFF,
    );
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        (if !g_tag_blk.is_null() {
            OCERZ_STEP_PROFILE as ::core::ffi::c_int
        } else {
            OCERZ_STEP_OK as ::core::ffi::c_int
        }) as uint64_t,
    );
    let ref mut fresh3 = *epilogue_sites.offset(*n_epi as isize);
    *fresh3 = a64_label(b);
    a64_b(b, 0 as int32_t);
    *n_epi += 1;
    return patch_b;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_chain_tail(
    mut b: *mut A64Buf,
    mut poll: ::core::ffi::c_int,
) -> *mut uint32_t {
    let mut intr: *mut uint32_t = ::core::ptr::null_mut::<uint32_t>();
    if poll != 0 {
        a64_ldr(
            b,
            4 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            INT_OFF,
        );
    }
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        19 as ::core::ffi::c_int,
    );
    a64_mov_reg(
        b,
        1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
    );
    emit_frame_sp_reset(b);
    emit_pin_epilogue_restore(b);
    a64_ldp_post(
        b,
        19 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    a64_ldp_post(
        b,
        29 as ::core::ffi::c_int,
        30 as ::core::ffi::c_int,
        31 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    if poll != 0 {
        intr = a64_label(b);
        a64_cbnz(b, 0 as ::core::ffi::c_int, JT1 as ::core::ffi::c_int, 0 as int32_t);
    }
    let mut patch_b: *mut uint32_t = a64_label(b);
    a64_b(b, 0 as int32_t);
    let mut fallback: *mut uint32_t = a64_label(b);
    a64_patch_b(patch_b, fallback);
    if poll != 0 {
        a64_patch_cbz(intr, fallback);
    }
    emit_side_tag(b, 1 as ::core::ffi::c_int);
    a64_mov_imm64(
        b,
        0 as ::core::ffi::c_int,
        OCERZ_STEP_OK as ::core::ffi::c_int as uint64_t,
    );
    a64_ret(b);
    return patch_b;
}
static mut g_oolslow: [C2RustUnnamed_13; 32] = [C2RustUnnamed_13 {
    sites: [::core::ptr::null::<uint32_t>() as *mut uint32_t; 3],
    nsites: 0,
    insn: ::core::ptr::null::<X86Insn>(),
    back: ::core::ptr::null::<uint32_t>() as *mut uint32_t,
    pre: 0,
    l0: [0; 16],
    l0_dbl: [0; 16],
    l0_dirty: 0,
    yc_dirty: 0,
}; 32];
#[unsafe(no_mangle)]
pub static mut g_n_oolslow: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn oolslow_add(
    mut insn: *const X86Insn,
    mut sites: *mut *mut uint32_t,
    mut nsites: ::core::ffi::c_int,
    mut back: *mut uint32_t,
) -> ::core::ffi::c_int {
    if g_n_oolslow >= OOLSLOW_MAX || nsites > 3 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    ea_cache_reset();
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < nsites {
        *(*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).sites.get_unchecked_mut(i as usize) = *sites.offset(i as isize);
        i += 1;
    }
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).nsites = nsites;
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).insn = insn;
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).back = back;
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).pre = g_oolslow_pre;
    let mut r: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while r < 16 as ::core::ffi::c_int {
        *(*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).l0.get_unchecked_mut(r as usize) = *(*(&raw mut g_l0)).get_unchecked_mut(r as usize);
        *(*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).l0_dbl.get_unchecked_mut(r as usize) = *(*(&raw mut g_l0_dbl)).get_unchecked_mut(r as usize);
        r += 1;
    }
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).l0_dirty = g_l0_dirty;
    (*(&raw mut g_oolslow)).get_unchecked_mut(g_n_oolslow as usize).yc_dirty = g_yc_dirty;
    g_oolslow_pre = 0 as uint32_t;
    g_n_oolslow += 1;
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_oolslow_arms(
    mut b: *mut A64Buf,
    mut exit_sites: *mut *mut uint32_t,
    mut n_exits: *mut ::core::ffi::c_int,
) {
    let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while k < g_n_oolslow {
        let mut sane: ::core::ffi::c_int = ((*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).back
            >= g_push_entry as *mut uint32_t && (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).back < (*b).p)
            as ::core::ffi::c_int;
        let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while sane != 0 && i < (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).nsites {
            sane = (*(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).sites.get_unchecked_mut(i as usize)
                >= g_push_entry as *mut uint32_t
                && *(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).sites.get_unchecked_mut(i as usize) < (*b).p)
                as ::core::ffi::c_int;
            i += 1;
        }
        if sane == 0 {
            static mut nwarn: ::core::ffi::c_int = 0;
            let fresh24 = nwarn;
            nwarn = nwarn + 1;
            if fresh24 < 40 as ::core::ffi::c_int {
                let mut tb: [::core::ffi::c_char; 128] = ::core::mem::transmute::<
                    [u8; 128],
                    [::core::ffi::c_char; 128],
                >(
                    *b"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
                );
                if !(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).insn.is_null() {
                    ocerz_format_insn(
                        (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).insn,
                        &raw mut tb as *mut ::core::ffi::c_char,
                        ::core::mem::size_of::<[::core::ffi::c_char; 128]>() as size_t,
                    );
                }
                fprintf(
                    crate::log::stderr(),
                    b"ocerz: warning: dropping stale oolslow arm (site/back outside the current block) block=%#llx insn=%#llx back=%+ld site0=%+ld end=%ld nsites=%d %s\n\0"
                        as *const u8 as *const ::core::ffi::c_char,
                    g_self_rip as ::core::ffi::c_ulonglong,
                    (if !(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).insn.is_null() {
                        (*(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).insn).rip
                    } else {
                        0 as uint64_t
                    }) as ::core::ffi::c_ulonglong,
                    (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).back.offset_from(g_push_entry)
                        as ::core::ffi::c_long,
                    if (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).nsites != 0 {
                        (*(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).sites
                            .get_unchecked_mut(0 as ::core::ffi::c_int as usize))
                            .offset_from(g_push_entry) as ::core::ffi::c_long
                    } else {
                        0 as ::core::ffi::c_long
                    },
                    (*b).p.offset_from(g_push_entry) as ::core::ffi::c_long,
                    (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).nsites,
                    &raw mut tb as *mut ::core::ffi::c_char,
                );
            }
        } else {
            let mut lbl: *mut uint32_t = a64_label(b);
            let mut i_0: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
            while i_0 < (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).nsites {
                patch_any_branch(*(*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).sites.get_unchecked_mut(i_0 as usize), lbl);
                i_0 += 1;
            }
            if (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).pre != 0 {
                a64_emit32(b, (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).pre);
            }
            emit_l0_flush_from(
                b,
                &raw mut (*(&raw mut g_oolslow as *mut C2RustUnnamed_13)
                    .offset(k as isize))
                    .l0 as *mut int8_t,
                &raw mut (*(&raw mut g_oolslow as *mut C2RustUnnamed_13)
                    .offset(k as isize))
                    .l0_dbl as *mut uint8_t,
                (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).l0_dirty,
            );
            yc_flush_from(b, (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).yc_dirty);
            emit_slowcall(b, (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).insn, exit_sites, n_exits);
            emit_l0_reload_from(
                b,
                &raw mut (*(&raw mut g_oolslow as *mut C2RustUnnamed_13)
                    .offset(k as isize))
                    .l0 as *mut int8_t,
                &raw mut (*(&raw mut g_oolslow as *mut C2RustUnnamed_13)
                    .offset(k as isize))
                    .l0_dbl as *mut uint8_t,
            );
            let mut here: *mut uint32_t = a64_label(b);
            a64_b(
                b,
                (*(&raw mut g_oolslow)).get_unchecked_mut(k as usize).back.offset_from(here) as ::core::ffi::c_long
                    as int32_t,
            );
        }
        k += 1;
    }
    g_n_oolslow = 0 as ::core::ffi::c_int;
}
pub const __ATOMIC_RELAXED: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
pub const __ATOMIC_ACQUIRE: ::core::ffi::c_int = 2 as ::core::ffi::c_int;
pub const NULL: *mut ::core::ffi::c_void = __DARWIN_NULL;
