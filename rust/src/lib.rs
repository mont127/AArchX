#![feature(thread_local)]
#![allow(non_snake_case, non_camel_case_types, non_upper_case_globals)]

pub mod ffi {
    include!(concat!(env!("OUT_DIR"), "/ffi.rs"));
}

pub mod inline;
pub mod jit_internal;
#[allow(dead_code)]
pub mod jit_internal_test;
pub mod log;

pub mod ported {
    include!(concat!(env!("OUT_DIR"), "/ported_mods.rs"));
}
