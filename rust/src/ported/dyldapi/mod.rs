//! The Rust port of the dyld API surface that libdyld's trampolines dispatch through.
//!
//! The part that carries the weight is the dependency closure: the set of images
//! handed to libobjc's map_images in the initial batch. An image left out of it
//! is one whose categories never attach to the classes other images own, which
//! surfaces as an unrecognized selector a long way from the cause. The closure
//! used to be walked through a separate fixed 1024-entry queue, which capped it
//! well below the 3609 images this cache holds and, once full, dropped a
//! dependency for good - and it filled early, because a dylib every image links
//! was pushed once per dependent. Safari died in SafariMain on -[NSBundle
//! safari_version] that way; the guest test for it is that Safari reaches its
//! first window. Now the closure array is itself the work list, deduped through
//! an open-addressed set of the same mach_headers so appending stays O(1), each
//! image is appended and therefore walked exactly once, and the only bound left
//! is the cache's own size (Safari's closure is about 1500 of 3609).
//!
//! The cache has already merged every image's categories into the method lists
//! of the classes they extend, and libobjc exposes them once the owning image is
//! marked loaded, so startup images never report their category lists. An image
//! mapped after startup does report them, because the classes it extends may be
//! realized by then with copied method lists that only a category list can reach.
//! OCERZ_PRELOAD_OBJC still moves named cache images' closures into the initial
//! batch, and "@cat" does that for every image that defines categories.
//!
//! A callback registered for bulk image loads is kept, not just driven once: it
//! hears every image already listed when it registers, and then each batch a
//! dlopen adds, before that batch's initializers for a disk image and as soon as
//! a cache image joins the list. libxpc is the one that registers, and it is how
//! a framework's embedded XPC services become reachable: driven only at startup,
//! every framework a program dlopened later had services launchd answered with
//! "No such process", so a Wine process could not reach MTLCompilerService to
//! compile a shader or the accessibility service behind AppKit's text input, and
//! steam.exe retried the view bridge a thousand times a second. A disk image's
//! path is handed over as well as a cache image's. OCERZ_NO_BULK_NOTIFY=1
//! leaves later batches unannounced.
//!
//! libsystem_platform's string and memory routines get two kinds of special
//! treatment from the translator, and this file is where it learns which code
//! they are. The image's function starts are read once, and the names
//! memmove, strlen and the rest are resolved from libSystem, as a program binds
//! them, keeping the answers that land in libsystem_platform's text. Resolving
//! them in libsystem_platform itself only worked while it had an upward link to
//! libSystem: macOS 26.7's has none and exports __platform_memmove and the
//! like, which libsystem_c re-exports under the plain names, so every lookup
//! missed, the translator answered none of them in place and none was
//! translated with plain accesses. Some of those exports
//! are the routine itself; others are a six-byte jump through a pointer the
//! library fills in with the variant it chose for the processor, and for those
//! the pointer is read when the question is asked, never the stub's enclosing
//! function, which is eight kilobytes of unrelated stubs. ocerz_dyldapi_memfn
//! answers whether an address lies inside one of those routines, which are
//! translated with plain accesses even where ordered accesses are required:
//! they move bytes at arbitrary alignments, nearly half of their eight-byte
//! accesses straddle a sixteen-byte boundary at random offsets, and each one
//! that does costs a barrier, which made a copy four and a half times slower
//! than Rosetta's. OCERZ_NO_MEMFN_PLAIN=1 turns that off.
//! ocerz_dyldapi_leaf_entry answers, for an address that is exactly the
//! exported entry of one of ten of them, the routine in src/leaf.s that does
//! the same work with the guest's registers in place, and whether it writes;
//! the translator puts a call to it at the head of that block, ahead of the
//! x86 code it keeps translating for the cases the routine declines.
//!
//! An API ocerz does not implement answers zero in RAX and zero in RDX. RDX
//! matters because a dyld API may return a pair, and leaving the guest's own
//! RDX in place hands the caller a count it never computed. _dyld_get_lib_msg_send
//! _offsets, slot +0x450, is exactly that shape: it answers a table and a count.
//! With RDX left alone the count was whatever happened to be in the register,
//! and a process died much later in os_unfair_lock_recursive_abort inside class
//! realization; answering an empty table with a count of zero ends that. The
//! slot is answered explicitly as well, since ocerz has no such table to offer.
//!
//! What each slot is, is known by running code, never by reading Apple's.
//! tools/dyldslots.sh calls every name the SDK's libdyld.tbd exports, with
//! zero arguments, under OCERZ_DYLDAPI_TRACE=1, which prints each arrival in
//! this table with its arguments and caller, so a public name is paired with
//! the slot its call reaches first; OCERZ_DLSYMLOG=1 prints every dlsym with its
//! handle, result and caller. A slot no export reaches first is named by its
//! caller in a traced run, through dladdr: +0x358 is called from
//! _dyld_objc_register_callbacks. tools/dyldslots.sh --check, part of the
//! dynamic tests, fails for any slot answered here that has no such witness,
//! and for any change in the pinned pairs in tools/dyldslots.pinned.
//!
//! _NSGetExecutablePath behaves as dyld's does: a buffer the path fits in gets
//! the path and a size left exactly as the caller set it, and only a buffer too
//! small has the size rewritten, to the length the path needs, with -1 returned.
//! The slot used to write the length back on success as well, which an arm64
//! build of the same program never sees; the app_bundle native case compares the
//! two.
//!
//! Which image holds an address - asked by _dyld_find_unwind_sections once for
//! every frame an exception unwinds through, and by the image-containing-address
//! slots - is answered from a sorted table of every mapped segment of the
//! closure and the cache, searched by bisection, with a running maximum of the
//! segment ends so overlapping ranges are still found. It used to walk the load
//! commands of each closure image and then of all 3609 cache images per call.
//! The table is rebuilt when the closure changes, and a segment with no
//! protection at all, __PAGEZERO, is left out of it.

use crate::ffi::OcerzCache;
use core::ffi::c_int;

mod closure;
mod dispatch;
mod hostmem;
mod macho;
mod memfn;
mod objc;

pub(crate) const DYLDAPI_VTABLE_SIZE: u64 = 0x2000;
pub(crate) const DYLDAPI_NOOP_OFF: u64 = 0x2000;

pub(crate) static mut g_apis_global: u64 = 0;
pub(crate) static mut g_cache: *mut OcerzCache = core::ptr::null_mut();
pub(crate) static mut g_headeropt_rw: u64 = 0;
pub(crate) static mut g_headeropt_ro: u64 = 0;
pub(crate) static mut g_sel_pool: u64 = 0;
pub(crate) static mut g_selopt: u64 = 0;
pub(crate) static mut g_clsopt: u64 = 0;
pub(crate) static mut g_protoopt: u64 = 0;
pub(crate) static mut g_block_scratch: u64 = 0;
pub(crate) static mut g_objc_make_mutable: u64 = 0;
pub(crate) static mut g_cache_start: u64 = 0;
pub(crate) static mut g_cache_size: u64 = 0;
pub(crate) static mut g_closure_mh: *mut u64 = core::ptr::null_mut();
pub(crate) static mut g_closure_n: c_int = 0;
pub(crate) static mut g_closure_cap: c_int = 0;
pub(crate) static mut g_closure_hash: *mut u64 = core::ptr::null_mut();
pub(crate) static mut g_closure_hash_mask: u32 = 0;
pub(crate) static mut g_objc_mapped_cb: u64 = 0;
pub(crate) static mut g_objc_init_cb: u64 = 0;
pub(crate) static mut g_objc_init_info: u64 = 0;
pub(crate) static mut g_objc_dlopen_mapped: *mut u64 = core::ptr::null_mut();
pub(crate) static mut g_objc_dlopen_mapped_n: c_int = 0;
pub(crate) static mut g_main_bv_platform: u32 = 0;
pub(crate) static mut g_main_bv_minos: u32 = 0;
pub(crate) static mut g_main_bv_sdk: u32 = 0;

#[unsafe(no_mangle)]
pub static mut g_main_path: u64 = 0;
