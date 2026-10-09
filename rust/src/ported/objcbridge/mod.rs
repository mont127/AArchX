//! Objective-C message sends and formatted output, crossed from an x86 guest
//! into the host's native arm64 runtime.  Rust port of src/objcbridge.c.
//!
//! ---- one runtime, the host's ----
//! In native mode no x86 libobjc exists.  A guest's classes are the host's
//! own: _OBJC_CLASS_$_NSString binds to the native class object, a constant
//! string's isa to the native constant-string class, and an object any native
//! method hands back is an address the guest can hold because native mode runs
//! in the identity map.  What the guest cannot do is send a message, because
//! objc_msgSend is a trampoline into whatever method implementation the
//! selector finds, and the arguments of that implementation are in x86
//! registers.  So every send is a crossing whose signature is not known until
//! the send arrives, and the runtime that is about to run the method is asked
//! for it.  A class the guest itself defines is made the native runtime's own
//! before any guest code runs (src/objcclass.c), so a send to a guest object
//! is the same crossing, and the native objc_msgSend it ends in reaches the
//! guest's method through a callback slot.
//!
//! ---- what every crossing here shares ----
//! A send raises a bridge frame before it touches the receiver, naming the
//! export, and once the method is known names the selector and the notation,
//! so a fault inside a native method is reported against the message that ran
//! it.  The frame is lowered after the call returns and before pending guest
//! signals are delivered, which every crossing does last.  A refusal is a line
//! on stderr and OCERZ_BRIDGE_UNIMPL_EXIT, never a call made under a guessed
//! signature.  OCERZ_OBJCLOG prints each send's class, selector and notation
//! as it crosses.

pub mod common;
pub mod encode;
pub mod send;
pub mod imp;
pub mod veneer;
pub mod eh;
pub mod hooks;
