//! Logging that mirrors OCERZ_LOG/OCERZ_TRACE/OCERZ_FATAL from types.h.
//! The macros take a C format string literal plus arguments and forward them
//! to libc::fprintf on stderr, so any printf conversion the C side uses works
//! here too. They NUL-terminate each format string before passing it to C.
#![allow(dead_code)]

unsafe extern "C" {
    static __stderrp: *mut libc::FILE;
}

#[inline(always)]
pub unsafe fn stderr() -> *mut libc::FILE {
    unsafe { __stderrp }
}

#[macro_export]
macro_rules! ocerz_log {
    ($fmt:literal $(, $arg:expr)*) => {
        unsafe {
            if $crate::ffi::ocerz_verbose >= 1 {
                ::libc::fprintf($crate::log::stderr(),
                    concat!("ocerz: ", $fmt, "\0").as_ptr() as *const ::core::ffi::c_char $(, $arg)*);
            }
        }
    };
}

#[macro_export]
macro_rules! ocerz_trace {
    ($fmt:literal $(, $arg:expr)*) => {
        unsafe {
            if $crate::ffi::ocerz_verbose >= 2 {
                ::libc::fprintf($crate::log::stderr(),
                    concat!("ocerz: ", $fmt, "\0").as_ptr() as *const ::core::ffi::c_char $(, $arg)*);
            }
        }
    };
}

#[macro_export]
macro_rules! ocerz_fatal {
    ($fmt:literal $(, $arg:expr)*) => {
        unsafe {
            ::libc::fprintf($crate::log::stderr(),
                concat!("ocerz: fatal: ", $fmt, "\0").as_ptr() as *const ::core::ffi::c_char $(, $arg)*);
        }
    };
}
