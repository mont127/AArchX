//! Objective-C classes, categories and protocols an x86 guest defines, made the
//! host's native arm64 runtime's own.  Rust port of src/objcclass.c.
//!
//! In native mode no x86 libobjc exists, so nothing reads a guest image's class
//! lists the way the translated runtime does in cache mode.  A class the guest
//! compiled is a class_t in the guest's __DATA, and every reference the guest
//! holds to it - a class reference, a superclass reference for [super ...], a
//! GOT entry another guest image bound - is that class_t's address.  Native code
//! has to call the guest's methods too: NSSet asks a guest object for -hash, a
//! view asks for -drawRect:, the runtime itself sends +initialize and calls
//! .cxx_destruct.  ocerz_objcbridge_define_image makes each class, category and
//! protocol of one image known to the native runtime, after the loader has bound
//! the image's fixups and canonicalized its selectors and before any guest code
//! runs, and ocerz_objcbridge_run_loads calls the +load methods that definition
//! queued.

mod common;
mod define;
mod read;
