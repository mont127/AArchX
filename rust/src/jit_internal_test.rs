//! Test-only `#[no_mangle]` wrappers over `jit_internal.rs`, one per helper
//! that tests/unit/test_jit_inline_rs.c diffs against the C `static inline`
//! in include/ocerz/jit_internal.h. Each is the C signature of its helper
//! (or a trivial adaptation: reference parameters become out-pointers).
//! Nothing in the translator calls these; the unit test does.
#![allow(clippy::missing_safety_doc)]

use crate::ffi::*;
use crate::jit_internal as j;
use core::ffi::c_int;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_jit_key(rip: u64, mode32: c_int) -> u64 {
    j::jit_key(rip, mode32)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_jit_key_rip(key: u64) -> u64 {
    j::jit_key_rip(key)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_jit_key_mode32(key: u64) -> c_int {
    j::jit_key_mode32(key)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_hash_key(key: u64) -> u32 {
    j::hash_key(key)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_psc_col(key: u64) -> u32 {
    j::psc_col(key)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_tc_key(rip: u64, mode32: c_int) -> u64 {
    unsafe { j::tc_key(rip, mode32) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_a64_word_may_write_reg(w: u32, r: u32) -> c_int {
    j::a64_word_may_write_reg(w, r)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_pin_hreg(slot: c_int) -> c_int {
    j::pin_hreg(slot)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_pin_slot(greg: u32) -> c_int {
    unsafe { j::pin_slot(greg) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_body_edge_pin_class() -> c_int {
    unsafe { j::body_edge_pin_class() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_xmm_vreg(xr: u32) -> c_int {
    j::xmm_vreg(xr)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_xmm_is_pinned(xr: u32) -> c_int {
    unsafe { j::xmm_is_pinned(xr) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_mark_has(m: *mut MarkSet, key: u64) -> c_int {
    unsafe { j::mark_has(m, key) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_mark_add(m: *mut MarkSet, key: u64) {
    unsafe { j::mark_add(m, key) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_patch_guard_skip(skip: *mut u32, target: *mut u32) {
    unsafe { j::patch_guard_skip(skip, target) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_rs_t_patch_any_branch(site: *mut u32, target: *mut u32) {
    unsafe { j::patch_any_branch(site, target) }
}
