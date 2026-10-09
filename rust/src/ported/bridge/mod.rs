//! Calling the host's own arm64 functions on the guest's behalf.  Rust port of
//! src/bridge.c.
//!
//! A virtual system library's exports are stubs that trap (see vdylib.h).  This
//! is what happens after the trap: the export is looked up, its arguments are
//! read out of the guest's x86 register state, converted where a conversion is
//! needed, handed to the real arm64 function in the host library, and the result
//! is put back where x86 code expects to find it.

pub(crate) mod common;
mod host;
mod lookup;
mod specials;
mod thunk;
mod zones;
