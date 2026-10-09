//! Maps the x86_64 dyld shared cache and resolves symbols out of it.
//!
//! ---- where the cache is ----
//! macOS 27 keeps the x86_64 cache in the Rosetta cryptex,
//! /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld, and macOS 26
//! keeps it in the OS cryptex, /System/Volumes/Preboot/Cryptexes/OS/System/
//! Library/dyld. The Rosetta location is tried first and the OS one only when
//! the Rosetta one holds no cache, so a macOS 27 machine maps exactly what it
//! did before and a macOS 26 machine runs cache mode at all; looking only in the
//! Rosetta cryptex had made every dynamic program on macOS 26 stop with "cannot
//! map shared cache". The main file and its .NN subcaches always come from the
//! same directory, and ocerz_cache_dir names the one in use.
//!
//! ---- lazy rebasing ----
//! The v2 slide info stores every pointer as offset|delta-chain bits, so the
//! DATA and DATA_CONST regions - about 500 MB - need unpacking even at slide 0.
//! Doing that eagerly touched and copy-on-wrote every page at process start,
//! ~200 ms of it. Instead those regions are mapped PROT_NONE and each 16 KB
//! host page is unpacked on its first touch, from the SIGSEGV handler, then
//! given its final protection. A page is unpacked into a private scratch
//! mapping and installed atomically: unpacking in place after an mprotect(RW)
//! let every other thread read the raw pointer chains mid-unpack, and
//! libswiftCore in steam.exe dereferenced one of those half-baked pointers.
//! Kept PROT_NONE until the remap, a concurrent reader simply faults, waits on
//! the lock and retries against the finished page. A page this thread already
//! retried once is not a lazy-unpack fault, so an alignment or protection fault
//! on an unpacked page still reaches the real handler. The unpack now holds its
//! lock across a whole page rebuild, so fork takes that lock like the other
//! emulator locks or the child inherits one nobody releases.
//!
//! ---- patched pages ----
//! Some engines make a libsystem page writable to patch it in place, so every
//! subcache mapping is recorded (which also tells an address in the cache apart
//! from a wild one) along with write-watch state for the pages a guest has
//! mprotect'ed writable. A lazily-slid page must be unpacked before such an
//! mprotect, because once it is accessible the fault that would have rebased it
//! never comes. PROT_EXEC is always dropped: guest code is never executed by
//! the host. When code has been translated out of a patched page, write is
//! taken back off it so the next patch faults instead of going unseen.
//!
//! ---- symbol resolution ----
//! Every import not satisfied by its own declared dependency falls back to a
//! walk of all ~3000 cache images, and Wine's loaders resolve the same libsystem
//! symbols for every module they map, so the answers are memoized - the cache's
//! export tries do not change at runtime.
//!
//! Two-level namespace resolution looks up a symbol in the SPECIFIC dylib the
//! binary named, following re-exports, rather than taking the flat walk: a
//! binary that links /usr/lib/libcrypto.46.dylib (LibreSSL 3.3.6) must bind
//! OpenSSL_version there even though the cache also carries libcrypto.44 (2.8.3)
//! exporting the same name, and the flat walk bound it to whichever image came
//! first, so openssl reported the wrong version. The path-to-header lookup goes
//! through the index below because resolving every import of a dependency
//! would otherwise rescan all ~3600 images. A dylib lookup follows
//! LC_REEXPORT_DYLIB as well as re-exports in the trie, because an umbrella
//! such as libSystem answers for its members only through those load commands.
//!
//! All of that runs on an index built once per process: install path to image
//! and header to image in open-addressed tables, and per image, filled on first
//! use, its export trie and its linked images in ordinal order with their kind.
//! The path memo it replaced was 512 direct-mapped slots, so under Wine, which
//! resolves thousands of OpenGL and AppKit names through dlsym, colliding paths
//! fell back to a strcmp over every image and a webhelper spent a quarter of
//! its startup in resolve_in_dylib, strcmp and strlen; a dlsym miss on a
//! framework handle cost 2 ms against Rosetta's 30 us. A breadth-first search
//! from one image walks the index with a visited bitmap and memoizes its answer
//! per image and name. Two searches share it: ocerz_cache_resolve_from_image
//! follows every link, upward ones included, and ocerz_cache_dlsym_image skips
//! upward links the way dlsym does (OCERZ_DLSYM_UPWARD follows them there too).
//!
//! A weak-coalescing bind (ordinal -3) asks whether any image already defines
//! the name, and dyld answers it only from images that define weak symbols,
//! MH_WEAK_DEFINES. Walking the whole cache instead cost Electron Framework
//! about 2,560 misses of 1.8 ms each, 4.6 s per process, because its weak names
//! are its own C++ and no system library has them. Those binds walk the ~300
//! weak-defining images only, and after 128 of them a 1 MB bloom filter of
//! every name those images and their re-exports export turns a miss into one
//! probe. The names are hashed along the trie edges rather than rebuilt, which
//! keeps the build near 40 ms, and a program with a handful of weak binds never
//! pays it. The filter is built privately and published with a release store,
//! because readers test it without the lock.
//!
//! The image list holds each library once, under its install name; every other
//! path for it, /usr/lib/libz.dylib for libz.1.dylib or libgcc_s.1.dylib for
//! libSystem, is only in the cache's dylibs trie, at header offset 0x108 in the
//! public dyld_cache_format.h, which maps a path to an image index. A path
//! missing from the image list is looked up there, which is how 6,677 of the
//! 10,595 paths in the macOS 27 Intel cache are reached.

use core::ffi::c_char;

pub(crate) use crate::ffi::OcerzCache;

pub(crate) const MH_MAGIC_64: u32 = 0xfeed_facf;
pub(crate) const MH_WEAK_DEFINES: u32 = 0x8000;
pub(crate) const LC_SEGMENT_64: u32 = 0x19;
pub(crate) const LC_DYLD_INFO: u32 = 0x22;
pub(crate) const LC_DYLD_INFO_ONLY: u32 = 0x8000_0022;
pub(crate) const LC_DYLD_EXPORTS_TRIE: u32 = 0x8000_0033;
pub(crate) const LC_LOAD_DYLIB: u32 = 0x0c;
pub(crate) const LC_LOAD_WEAK_DYLIB: u32 = 0x8000_0018;
pub(crate) const LC_REEXPORT_DYLIB: u32 = 0x8000_001f;
pub(crate) const LC_LOAD_UPWARD_DYLIB: u32 = 0x8000_0023;
pub(crate) const DYLIB_USE_MARKER: u32 = 0x1a74_1800;
pub(crate) const DYLIB_USE_UPWARD: u32 = 0x04;
pub(crate) const DYLIB_USE_COMMAND_SIZE: u32 = 28;

pub(crate) fn is_dylib_cmd(cmd: u32) -> bool {
    cmd == LC_LOAD_DYLIB
        || cmd == LC_LOAD_WEAK_DYLIB
        || cmd == LC_REEXPORT_DYLIB
        || cmd == LC_LOAD_UPWARD_DYLIB
}

pub(crate) unsafe fn rd32(p: *const u8) -> u32 {
    unsafe { p.cast::<u32>().read_unaligned() }
}

pub(crate) unsafe fn rd64(p: *const u8) -> u64 {
    unsafe { p.cast::<u64>().read_unaligned() }
}

pub(crate) unsafe fn uleb(pp: *mut *const u8, end: *const u8) -> u64 {
    unsafe {
        let mut v = 0u64;
        let mut shift = 0u32;
        let mut p = *pp;
        while p < end {
            let b = *p;
            p = p.add(1);
            v |= ((b & 0x7f) as u64).wrapping_shl(shift);
            if b & 0x80 == 0 {
                break;
            }
            shift = shift.wrapping_add(7);
        }
        *pp = p;
        v
    }
}

pub(crate) mod index;
pub(crate) mod map;
pub(crate) mod resolve;
pub(crate) mod weak;
