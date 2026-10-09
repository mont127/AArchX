//! libSystem calls a generic crossing cannot make, answered by ocerz: the
//! variadic file and IPC calls, memory mapping, non-local jumps and processes.
//!
//! This is the Rust port of src/sysbridge.c, split into private submodules
//! along the C file's own sections: `files` and `fcntl` hold the variadic
//! calls whose optional argument is fixed, `int128` the compiler-rt 128-bit
//! arithmetic, `fenv` the floating-point environment, `mem` mmap and the
//! mach_vm calls, `jmp` setjmp/longjmp, `proc` fork/exec/spawn/system/popen,
//! `threads` the pthread keys and thread entry points, and `misc` the rest.

mod common;
mod fenv;
mod fcntl;
mod files;
mod int128;
mod jmp;
mod mem;
mod misc;
mod proc;
mod threads;
