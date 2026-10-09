//! The on-disk translation cache's store.
//!
//! `ocerz/tcache.h` states the interface; what follows is how records are kept.
//!
//! ---- where ----
//! Records live in tc-<fingerprint> under $HOME/Library/Caches/ocerz, or under
//! OCERZ_TCACHE_DIR when that is set.  The fingerprint is a hash of everything
//! that changes what a translation looks like without changing the guest bytes
//! it came from: this ocerz binary's LC_UUID, the memory mode and the host
//! bases it maps the guest at, the machine model, and every OCERZ_ variable in
//! the environment apart from the few that only steer logging, the loader or
//! this cache.  A different build or a different knob therefore opens a
//! different directory, and nothing inside one directory needs a version check
//! of its own.  Opening a store touches its directory.  Directories of other
//! fingerprints are removed once they have not been touched for a day, and the
//! least recently touched go sooner while they hold more than 2 GB together, so
//! rebuilding ocerz does not pile up stores; one touched within the last hour
//! is never removed, because a Wine session uses several fingerprints at once.
//!
//! ---- one index, many data files ----
//! The directory holds one index, shared by every process that uses it, and
//! one data file per process that has written anything, d-<n>.td.  The index
//! is a header and an open-addressed table of (key, location) slots that every
//! process maps shared and changes with atomic operations alone: a location
//! names a data file and an offset in it, and a key is claimed with a
//! compare-and-swap before its location is stored.  A process appends to its
//! own data file only, and publishes a record's location only after the record
//! is written, so a reader that finds a location finds the whole record behind
//! it; a record written again, because the guest bytes under it changed, gets
//! a new location in the same slot, and the newest one wins.  What one process
//! translates is therefore loaded, not translated again, by every process that
//! starts after it, in the same session as well as in the next one.
//!
//! A record is kept LZ4-compressed, which makes it about 1.65 times smaller for
//! 1.6 us when it is written and 0.3 us when it is read, and carries a checksum
//! of what is stored, checked on every load, so a record torn by a crash is
//! refused rather than run.  The index is created under a temporary name and
//! linked into place, so no process ever maps half an index.
//!
//! Nothing is ever taken out of a store, so it only fills.  Writing stops, and
//! a note is printed once, when the data files add up to OCERZ_TCACHE_MAX_MB
//! (4096 by default) or the table runs out of room; reading goes on.  Every
//! process holds a shared flock on the directory's users file for as long as it
//! lives, and one that finds nobody else there (an exclusive flock succeeds)
//! and the store full removes it and starts it again, so a full store costs one
//! cold start rather than every translation from then on.
//!
//! The disk comes first.  The cache never writes while its volume has less than
//! OCERZ_TCACHE_MIN_FREE_MB free (10240 by default), checked before every
//! append, and a process takes at most half of what was free above that floor
//! when it opened the store.  A lone process that finds the volume already
//! under the floor removes the store, and the prune then removes every other
//! store that is more than an hour old.  A tester's Mac with a few GB free
//! filled up under a Steam session, stopped being able to swap, and panicked
//! on the watchdog; with the floor the cache gives up its speed instead.
//!
//! ---- in a process ----
//! A record put is copied into a 256 KB buffer and nothing more, so storing
//! costs the translator almost nothing; a full buffer goes to a writer thread,
//! which compresses its records, appends them to the data file and publishes
//! them.  When the process exits, execs or leaves through the non-main-thread
//! exit path, the last buffer is handed over too and the writer is given up to
//! three seconds to finish; a process killed outright loses what was still
//! queued, never half a record, since nothing is published before it is
//! written.  A data file is mapped when a location first points into it and
//! mapped again, larger, when a location points past what is mapped.  A
//! forked child keeps its parent's mappings but not its parent's data file: it
//! drops the buffer and opens a data file of its own on its first write.

use core::ffi::{c_char, c_int};
use core::sync::atomic::AtomicI32;

mod io;
mod store;
mod writer;

pub(super) const TC_IDX_MAGIC: u32 = 0x32494354;
pub(super) const TC_DAT_MAGIC: u32 = 0x32444354;
pub(super) const TC_ZREC_MAGIC: u32 = 0x315a4354;
pub(super) const TC_SLOTS: u64 = 1 << 22;
pub(super) const TC_PROBE: c_int = 64;
pub(super) const TC_BUF_BYTES: usize = 256 << 10;
pub(super) const TC_OUT_BYTES: usize = 512 << 10;
pub(super) const TC_QMAX: usize = 4;
pub(super) const TC_REC_MAX: usize = 256 << 10;
pub(super) const TC_LOC_OFF_BITS: u32 = 40;
pub(super) const COMPRESSION_LZ4_RAW: c_int = 0x101;

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct TcIdxHdr {
    pub magic: u32,
    pub version: u32,
    pub fp: u64,
    pub slots: u64,
    pub next_file: u64,
    pub bytes: u64,
    pub pad: [u64; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct TcSlot {
    pub key: u64,
    pub loc: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct TcDatHdr {
    pub magic: u32,
    pub version: u32,
    pub fp: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct TcFile {
    pub p: *const u8,
    pub len: usize,
    pub fd: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct TcStored {
    pub magic: u32,
    pub size: u32,
    pub key: u64,
    pub sum: u64,
    pub raw: u32,
    pub zlen: u32,
}

#[derive(Clone, Copy)]
pub(super) struct QueueItem {
    pub p: *mut u8,
    pub n: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct MachHeader64 {
    pub magic: u32,
    pub cputype: i32,
    pub cpusubtype: i32,
    pub filetype: u32,
    pub ncmds: u32,
    pub sizeofcmds: u32,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct LoadCommand {
    pub cmd: u32,
    pub cmdsize: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct UuidCommand {
    pub cmd: u32,
    pub cmdsize: u32,
    pub uuid: [u8; 16],
}

const _: () = assert!(core::mem::size_of::<TcIdxHdr>() == 64);
const _: () = assert!(core::mem::size_of::<TcSlot>() == 16);
const _: () = assert!(core::mem::size_of::<TcDatHdr>() == 16);
const _: () = assert!(core::mem::size_of::<TcStored>() == 32);
const _: () = assert!(core::mem::size_of::<MachHeader64>() == 32);
const _: () = assert!(core::mem::size_of::<LoadCommand>() == 8);
const _: () = assert!(core::mem::size_of::<UuidCommand>() == 24);

pub(super) static G_MODE: AtomicI32 = AtomicI32::new(-1);
pub(super) static G_FULL: AtomicI32 = AtomicI32::new(0);

pub(super) static mut G_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub(super) static mut G_FP: u64 = 0;
pub(super) static mut G_DIR: [c_char; 1024] = [0; 1024];
pub(super) static mut G_OPENED: c_int = 0;
pub(super) static mut G_HDR: *mut TcIdxHdr = core::ptr::null_mut();
pub(super) static mut G_SLOT: *mut TcSlot = core::ptr::null_mut();
pub(super) static mut G_MASK: u64 = 0;
pub(super) static mut G_FILE: *mut TcFile = core::ptr::null_mut();
pub(super) static mut G_NFILE: u64 = 0;
pub(super) static mut G_CAP_BYTES: u64 = 0;
pub(super) static mut G_FLOOR_BYTES: u64 = 0;
pub(super) static mut G_ROOM_END: u64 = 0;
pub(super) static mut G_DNO: u64 = 0;
pub(super) static mut G_DFD: c_int = -1;
pub(super) static mut G_DOFF: u64 = 0;
pub(super) static mut G_BUF: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_BUF_N: usize = 0;
pub(super) static mut G_LOG: c_int = -1;
pub(super) static mut G_UFD: c_int = -1;
pub(super) static mut G_ZBUF: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_OBUF: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_RBUF: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_ZSCRATCH: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_DSCRATCH: *mut u8 = core::ptr::null_mut();
pub(super) static mut G_QLOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub(super) static mut G_QWORK: libc::pthread_cond_t = libc::PTHREAD_COND_INITIALIZER;
pub(super) static mut G_QDONE: libc::pthread_cond_t = libc::PTHREAD_COND_INITIALIZER;
pub(super) static mut G_Q: [QueueItem; TC_QMAX] = [QueueItem {
    p: core::ptr::null_mut(),
    n: 0,
}; TC_QMAX];
pub(super) static mut G_QN: c_int = 0;
pub(super) static mut G_BUSY: c_int = 0;
pub(super) static mut G_WRITER: c_int = 0;
pub(super) static mut G_NSPARE: c_int = 0;
pub(super) static mut G_SPARE: [*mut u8; TC_QMAX] = [core::ptr::null_mut(); TC_QMAX];
pub(super) static mut G_WRITER_TID: libc::pthread_t =
    unsafe { core::mem::zeroed::<libc::pthread_t>() };

unsafe extern "C" {
    pub(super) fn compression_encode_buffer(
        dst_buffer: *mut u8,
        dst_size: usize,
        src_buffer: *const u8,
        src_size: usize,
        scratch_buffer: *mut core::ffi::c_void,
        algorithm: c_int,
    ) -> usize;
    pub(super) fn compression_decode_buffer(
        dst_buffer: *mut u8,
        dst_size: usize,
        src_buffer: *const u8,
        src_size: usize,
        scratch_buffer: *mut core::ffi::c_void,
        algorithm: c_int,
    ) -> usize;
    pub(super) fn compression_encode_scratch_buffer_size(algorithm: c_int) -> usize;
    pub(super) fn compression_decode_scratch_buffer_size(algorithm: c_int) -> usize;
    pub(super) fn _dyld_get_image_header(image_index: u32) -> *const MachHeader64;
    pub(super) static mut environ: *mut *mut c_char;
}
