//! Process-wide definitions that no single module should own.
//!
//! The guest argv tail kept here for the exit diagnostics is defined in this
//! file rather than in main.c so that the unit tests, which link CORE_OBJS
//! without main.o, still resolve it.

use core::ffi::c_int;

#[unsafe(no_mangle)]
pub static mut ocerz_verbose: c_int = 0;

#[unsafe(no_mangle)]
#[thread_local]
pub static mut ocerz_critical_depth: c_int = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_mode: c_int = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_cmdline_summary: [core::ffi::c_char; 256] = [0; 256];
