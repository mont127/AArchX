//! ---- blocks and the cache ----
//! Blocks are keyed by jit_key(rip, mode32): the same guest address in 32- and
//! 64-bit mode is not the same code and must never share a cache entry, a
//! commpage mark or an invalidation mark.  A published block keeps 16 bytes per
//! instruction (rip, len, op) instead of the 96-byte X86Insn; only the few
//! instructions it still executes through the interpreter (slow calls) or
//! inspects in full (fault-flag producers) are copied into blk->kept.  Dropping
//! the decoded array matters: a GUI Wine process carried ~870 bytes of X86Insn
//! per block across 215k blocks, 180 MB of it.
//!
//! ---- invalidation ----
//! Every guest mmap/mprotect/munmap asks the JIT to drop code in a range.  A
//! global min/max cannot answer that under Wine, where the live set spans PE
//! images at 32-bit addresses and shared-cache dylibs at 0x7ff8_0000_0000, so a
//! region map (one slot per 4 MB, a bitmap of the 64 KB granules in it) answers
//! in constant time; it may say "maybe" after code is gone but never "no" while
//! it is present.  When it says "maybe", the blocks to look at come from a list
//! per 64 KB granule kept beside the granule counts, not from a walk over every
//! live block: Unity's Mono JIT writes code all through R.E.P.O.'s startup with
//! about 390,000 blocks live, and that walk plus rebuilding the live array on
//! every retirement were 30% of the game process; a retired block now leaves
//! the live array by swapping with the last one (OCERZ_INV_SCAN=1 walks every
//! block again).  Only the overlapping blocks are retired - dropping the whole
//! cache per flip made CEF startup a full retranslation storm.  Their code stays
//! allocated on a retired list, so a thread still inside runs to its next exit.
//! A 64 KB region whose translations keep being invalidated (a JS engine
//! W^X-flipping its code space) is run interpreted after a few hits, but not
//! permanently: module-load fixups also retire blocks a few times and then
//! never again, and a permanent blacklist left the hottest DLL code interpreting
//! forever, so a region quiet for CHURN_QUIET_NS is re-probed.
//!
//! Retired code is never reused in place, so code that keeps being regenerated
//! fills the arena: R.E.P.O.'s Mono JIT retranslates its way through the whole
//! 1 GB within two minutes, after which every new block used to run interpreted.
//! The game also crashed in Mono's metadata code in every run that filled the
//! arena while still loading, and a 512 KB arena reproduces the same kind of
//! damage in tests/dynamic/dlopen_cryptex.c whenever a translation overflows and
//! is followed by more translating; why an overflowing translation leaves the
//! translator in that state is not understood yet, so the arena is no longer
//! allowed to overflow.  When less than an eighth of it, at most 8 MB, is left,
//! the next translation miss flushes it instead: every block is retired, every
//! table that points into the arena (chains, return-address cells and slots,
//! site caches, veneer pools, the dispatch stubs, the code index) is emptied,
//! and translation starts again at the front.  The old code is neither unchained
//! nor pushed through the instruction cache, since nothing runs it again;
//! doing both over a full arena held every thread for a third of a second.  The
//! same flush runs when the return-address slots run out, which retranslation
//! also does, because only a flush gives them back.  No thread may still be
//! running translated code when that happens.  Each thread counts the
//! translated frames it is in and how many of them are parked in a syscall made
//! through the slow path, ocerz_jit_exec_one; any other slow-path call returns
//! soon enough to leave by a stop site, and parking every call cost R.E.P.O.'s
//! worker threads a tenth to a quarter of their busy time in atomics and
//! thread-local lookups.  The flusher bumps a generation, patches every stop
//! site so a running thread leaves at its next block edge (one cache line
//! synchronised per patched word), and waits until every thread's two counts are
//! equal; a thread that reaches the dispatcher meanwhile runs its next
//! instruction in the interpreter rather than waiting, so a collector that has
//! frozen a thread can still get round to resuming it.  A parked call that
//! returns into a newer generation goes back to its run loop rather than into
//! the arena: the slow path has already written the guest state to the cpu, so
//! that is the exit the block itself would have taken, and the call's result is
//! handed over with it.  A thread that cannot leave within 500 ms (one stopped
//! by thread_suspend while it spins, say) makes the flush give up and restore
//! the stop-site words it changed, and the next attempt waits two seconds; a
//! thread that is itself inside translated code never flushes.
//! OCERZ_NO_JIT_FLUSH=1 keeps the old behaviour.
//!
//! ---- faults and fork ----
//! A fault inside a block reconstructs the guest state from the host registers:
//! a push whose store faulted has already decremented rsp in its host register
//! (+8 repairs it), elided return-address slots are written back so the
//! interpreter can resume mid-frame, and the XMM pins are recovered from the
//! signal frame's NEON state because the memory copy is stale.  A fork child
//! inherits the parent's MAP_JIT arena, whose pages fault when executed, so the
//! child abandons the arena (rather than freeing it - the fork may have caught
//! the allocator mid-update) and builds a fresh one on its next step.
//!
//!
//! b, and bl: a chained direct call is a bl into its callee
//!
//! An entry's rip word carries its column's retire generation above the address
//!     (rip ^ gen), so retiring a block bumps its column's vm->psc_gen instead of
//!     searching every table: a canonical target only matches an entry filled in
//!     its column's current generation.  (A non-canonical one, which would fault on
//!     x86, could meet an older generation's entry.)
//!
//! A PSC entry for a block sits in the column its own rip selects, bits 2-6,
//!     because the indirect tail stores the rip it looked up beside that block's
//!     body.  Retiring bumps those columns' generations after the blocks have left
//!     the hash table, so a miss that refills from the table never tags a retired
//!     body with a current generation.  A generation that wraps clears its column.

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use crate::ffi::*;
use crate::inline::{ocerz_cc_pack, ocerz_trunc};
use crate::jit_internal::*;
use libc::{
    CLOCK_UPTIME_RAW, KERN_SUCCESS, PTHREAD_ONCE_INIT, TH_STATE_HALTED, TH_STATE_RUNNING,
    TH_STATE_STOPPED, TH_STATE_UNINTERRUPTIBLE, TH_STATE_WAITING, THREAD_BASIC_INFO,
    THREAD_BASIC_INFO_COUNT, nanosleep, pthread_create, pthread_detach, pthread_key_create,
    pthread_key_t, pthread_once, pthread_once_t, pthread_setspecific, pthread_t, sched_yield,
    thread_act_t, thread_basic_info_data_t, thread_info, timespec, usleep,
};

macro_rules! env_on {
    ($name:literal) => {{
        static mut ON: c_int = -1;
        unsafe {
            if ON < 0 {
                ON = if !libc::getenv(concat!($name, "\0").as_ptr().cast()).is_null() {
                    1
                } else {
                    0
                };
            }
            ON
        }
    }};
}

unsafe extern "C" {
    fn getpagesize() -> c_int;
    fn pthread_mutex_trylock(lock: *mut pthread_mutex_t) -> c_int;
    fn pthread_mutex_unlock(lock: *mut pthread_mutex_t) -> c_int;
    fn pthread_mutex_init(lock: *mut pthread_mutex_t, attr: *const c_void) -> c_int;
    fn pthread_jit_write_protect_np(enabled: c_int);
    fn sys_icache_invalidate(start: *mut c_void, len: usize);
    fn backtrace(buffer: *mut *mut c_void, size: c_int) -> c_int;
    fn backtrace_symbols_fd(buffer: *const *mut c_void, size: c_int, fd: c_int);
    fn pthread_threadid_np(thread: *mut c_void, tid: *mut u64) -> c_int;
    fn clock_gettime_nsec_np(clock_id: u32) -> u64;
}

unsafe fn jit_code_bytes() -> usize {
    unsafe {
        let kb = libc::getenv(b"OCERZ_JIT_CODE_KB\0".as_ptr().cast());
        if !kb.is_null() {
            let v = libc::strtoul(kb, core::ptr::null_mut(), 0) as usize;
            if v != 0 {
                let pg = getpagesize() as usize;
                let bytes = v.wrapping_shl(10).wrapping_add(pg - 1);
                return bytes - bytes % pg;
            }
        }
        let e = libc::getenv(b"OCERZ_JIT_CODE_MB\0".as_ptr().cast());
        let mb = if e.is_null() {
            0
        } else {
            libc::strtoul(e, core::ptr::null_mut(), 0) as usize
        };
        if mb != 0 {
            mb.wrapping_shl(20)
        } else {
            JIT_CODE_BYTES_DEFAULT as usize
        }
    }
}

#[inline]
unsafe fn blk_insn_rip(b: *const JitBlock, i: c_int) -> u64 {
    unsafe {
        if !(*b).insns.is_null() {
            (*(*b).insns.offset(i as isize)).rip
        } else {
            (*(*b).iref.offset(i as isize)).rip
        }
    }
}

#[inline]
unsafe fn blk_insn_len(b: *const JitBlock, i: c_int) -> c_uint {
    unsafe {
        if !(*b).insns.is_null() {
            (*(*b).insns.offset(i as isize)).len as c_uint
        } else {
            (*(*b).iref.offset(i as isize)).len as c_uint
        }
    }
}

#[inline]
unsafe fn blk_insn_op(b: *const JitBlock, i: c_int) -> c_uint {
    unsafe {
        if !(*b).insns.is_null() {
            (*(*b).insns.offset(i as isize)).op as c_uint
        } else {
            (*(*b).iref.offset(i as isize)).op as c_uint
        }
    }
}

static mut g_no_fault_recipes: c_int = 0;
static mut g_psc_pool: *mut JitPscEnt = core::ptr::null_mut();
static mut g_psc_cap: usize = 0;
static mut g_psc_used: usize = 0;
static mut g_psc_tables: *mut *mut JitPscEnt = core::ptr::null_mut();
static mut g_cap_psc_tables: usize = 0;
static mut g_n_psc_tables: usize = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn psc_alloc() -> *mut JitPscEnt {
    unsafe {
        if g_psc_used.wrapping_add(PSC_N as usize) > g_psc_cap {
            let bytes = 1usize << 22;
            let p = libc::mmap(
                core::ptr::null_mut(),
                bytes,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_ANON | libc::MAP_PRIVATE,
                -1,
                0,
            );
            if p == libc::MAP_FAILED {
                return core::ptr::null_mut();
            }
            g_psc_pool = p.cast();
            g_psc_used = 0;
            g_psc_cap = bytes / core::mem::size_of::<JitPscEnt>();
        }
        let t = g_psc_pool.add(g_psc_used);
        g_psc_used = g_psc_used.wrapping_add(PSC_N as usize);
        for k in 0..PSC_N as usize {
            (*t.add(k)).rip = PSC_EMPTY_RIP as u64;
        }
        if g_n_psc_tables == g_cap_psc_tables {
            let ncap = if g_cap_psc_tables != 0 {
                g_cap_psc_tables.wrapping_mul(2)
            } else {
                1024
            };
            let nv = libc::realloc(
                g_psc_tables.cast(),
                ncap.wrapping_mul(core::mem::size_of::<*mut JitPscEnt>()),
            )
            .cast::<*mut JitPscEnt>();
            if !nv.is_null() {
                g_psc_tables = nv;
                g_cap_psc_tables = ncap;
            }
        }
        if g_n_psc_tables < g_cap_psc_tables {
            *g_psc_tables.add(g_n_psc_tables) = t;
            g_n_psc_tables = g_n_psc_tables.wrapping_add(1);
        }
        t
    }
}

unsafe fn psc_clear_all() {
    unsafe {
        for i in 0..g_n_psc_tables {
            let table = *g_psc_tables.add(i);
            for k in 0..PSC_N as usize {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!((*table.add(k)).rip))
                    .store(PSC_EMPTY_RIP as u64, Ordering::Release);
                AtomicPtr::<c_void>::from_ptr(core::ptr::addr_of_mut!((*table.add(k)).body))
                    .store(core::ptr::null_mut(), Ordering::Release);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub static mut g_ras_cells: *mut *mut *mut c_void = core::ptr::null_mut();

#[unsafe(no_mangle)]
pub static mut g_cap_ras_cells: usize = 0;

#[unsafe(no_mangle)]
pub static mut g_n_ras_cells: usize = 0;

static mut g_churn_suppress: c_int = 0;

#[unsafe(no_mangle)]
pub static mut jit_lock: pthread_mutex_t = pthread_mutex_t {
    __sig: 0x32AAABA7,
    __opaque: [0; 56],
};

#[unsafe(no_mangle)]
pub static mut g_jl_log: c_int = -1;

static mut g_jl_acq: u64 = 0;
static mut g_jl_waits: u64 = 0;
static mut g_jl_xlat_null: u64 = 0;
static mut g_jl_rip: u64 = 0;

#[unsafe(no_mangle)]
pub static mut g_jl_owner: u64 = 0;

#[unsafe(no_mangle)]
pub static mut g_jl_since: u64 = 0;

#[unsafe(no_mangle)]
pub static mut g_jl_phase: c_int = 0;

#[unsafe(no_mangle)]
#[thread_local]
pub static mut jl_held: c_int = 0;

static mut g_jl_owner_cpu: *mut OcerzCPU = core::ptr::null_mut();

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_lock_held_self() -> c_int {
    unsafe { jl_held }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_lock_depth_ptr() -> *const c_int {
    core::ptr::addr_of!(ocerz_critical_depth)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_lock_owner_cpu() -> *mut OcerzCPU {
    unsafe { core::ptr::addr_of!(g_jl_owner_cpu).read_volatile() }
}

unsafe fn jl_recursive_warn(where_: *const c_char) {
    static mut ONCE: c_int = 0;
    unsafe {
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(ONCE)).fetch_add(1, Ordering::Relaxed) > 4 {
            return;
        }
        let mut bt = [core::ptr::null_mut::<c_void>(); 24];
        let nb = backtrace(bt.as_mut_ptr(), 24);
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: JITLOCK-RECURSIVE[%d] at %s tid=%llu depth=%d phase=%d rip=%#llx\n\0"
                .as_ptr()
                .cast(),
            libc::getpid(),
            where_,
            jl_tid(),
            jl_held,
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).load(Ordering::Relaxed),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_rip)).load(Ordering::Relaxed),
        );
        backtrace_symbols_fd(bt.as_ptr(), nb, 2);
    }
}

unsafe fn jl_tid() -> u64 {
    let mut t = 0;
    unsafe {
        pthread_threadid_np(core::ptr::null_mut(), &mut t);
    }
    t
}

unsafe fn jl_dump(tag: *const c_char, wait_ns: u64) {
    unsafe {
        let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        let since =
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_since)).load(Ordering::Relaxed);
        let w = core::ptr::addr_of!(jit_lock).cast::<u32>();
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: JITLOCK-%s[%d] tid=%llu wait_ms=%.1f owner=%llu held_ms=%.1f phase=%d rip=%#llx acq=%llu waits=%llu xlat_null=%llu mtx=%08x %08x %08x %08x %08x %08x\n\0"
                .as_ptr()
                .cast(),
            tag,
            libc::getpid(),
            jl_tid(),
            wait_ns as f64 / 1e6,
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_owner)).load(Ordering::Relaxed),
            if since != 0 {
                (now.wrapping_sub(since)) as f64 / 1e6
            } else {
                0.0
            },
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).load(Ordering::Relaxed),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_rip)).load(Ordering::Relaxed),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_acq)).load(Ordering::Relaxed),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_waits)).load(Ordering::Relaxed),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_xlat_null)).load(Ordering::Relaxed),
            *w,
            *w.add(1),
            *w.add(2),
            *w.add(3),
            *w.add(4),
            *w.add(5),
        );
        let oc = core::ptr::addr_of!(g_jl_owner_cpu).read_volatile();
        if !oc.is_null() && (*oc).host_kport != 0 {
            let mut bi = core::mem::zeroed::<thread_basic_info_data_t>();
            let mut bn = THREAD_BASIC_INFO_COUNT as u32;
            if thread_info(
                (*oc).host_kport as thread_act_t,
                THREAD_BASIC_INFO as u32,
                (&mut bi as *mut thread_basic_info_data_t).cast(),
                &mut bn,
            ) == KERN_SUCCESS
            {
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: JITLOCK-OWNER-STATE[%d] run_state=%d(%s) suspend_count=%d flags=%#x cpu_usage=%d sleep_time=%d\n\0"
                        .as_ptr()
                        .cast(),
                    libc::getpid(),
                    bi.run_state,
                    if bi.run_state == TH_STATE_RUNNING {
                        b"RUNNING\0".as_ptr().cast::<c_char>()
                    } else if bi.run_state == TH_STATE_STOPPED {
                        b"STOPPED\0".as_ptr().cast::<c_char>()
                    } else if bi.run_state == TH_STATE_WAITING {
                        b"WAITING\0".as_ptr().cast::<c_char>()
                    } else if bi.run_state == TH_STATE_UNINTERRUPTIBLE {
                        b"UNINTERRUPTIBLE\0".as_ptr().cast::<c_char>()
                    } else if bi.run_state == TH_STATE_HALTED {
                        b"HALTED\0".as_ptr().cast::<c_char>()
                    } else {
                        b"?\0".as_ptr().cast::<c_char>()
                    },
                    bi.suspend_count,
                    bi.flags,
                    bi.cpu_usage,
                    bi.sleep_time,
                );
            } else {
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: JITLOCK-OWNER-STATE[%d] thread_info failed kport=%#x\n\0"
                        .as_ptr()
                        .cast(),
                    libc::getpid(),
                    (*oc).host_kport,
                );
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jl_acquire(site: c_int) {
    unsafe {
        ocerz_critical_depth += 1;
        if g_jl_log < 0 {
            g_jl_log = if !libc::getenv(b"OCERZ_JITLOCKLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if jl_held != 0 {
            jl_recursive_warn(b"jl_acquire\0".as_ptr().cast());
        }
        if pthread_mutex_trylock(core::ptr::addr_of_mut!(jit_lock)) != 0 {
            let t0 = if g_jl_log > 0 {
                clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            } else {
                0
            };
            if g_jl_log > 0 {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_waits))
                    .fetch_add(1, Ordering::Relaxed);
            }
            let mut s: u64 = 0;
            loop {
                if s < 64 {
                    sched_yield();
                } else {
                    let ts = timespec {
                        tv_sec: 0,
                        tv_nsec: if s < 512 { 200_000 } else { 2_000_000 },
                    };
                    nanosleep(&ts, core::ptr::null_mut());
                }
                if pthread_mutex_trylock(core::ptr::addr_of_mut!(jit_lock)) == 0 {
                    break;
                }
                if g_jl_log > 0 {
                    let w = clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0);
                    if w > 3_000_000_000 && s % 2000 == 0 {
                        jl_dump(b"WAIT\0".as_ptr().cast(), w);
                    }
                }
                s = s.wrapping_add(1);
            }
        }
        if g_jl_log > 0 {
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_acq)).fetch_add(1, Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_owner))
                .store(jl_tid(), Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_since))
                .store(clock_gettime_nsec_np(CLOCK_UPTIME_RAW), Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_rip)).store(0, Ordering::Relaxed);
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).store(site, Ordering::Relaxed);
        }
        jl_held += 1;
    }
}

static mut g_xlp_log: c_int = -1;
static mut g_xlp: [u64; XLP_SIZE as usize] = [0; XLP_SIZE as usize];

unsafe fn xlatpage_note(rip: u64) {
    unsafe {
        if g_xlp_log < 0 {
            g_xlp_log = if !libc::getenv(b"OCERZ_XLATPAGES\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if g_xlp_log <= 0 {
            return;
        }
        let page = rip & !0xfff;
        if page == 0 {
            return;
        }
        let mut i =
            (page.wrapping_mul(0x9E3779B97F4A7C15) >> 47) as usize & (XLP_SIZE as usize - 1);
        let xlp = core::ptr::addr_of_mut!(g_xlp).cast::<u64>();
        for _ in 0..8 {
            if *xlp.add(i) == page {
                return;
            }
            if *xlp.add(i) == 0 {
                *xlp.add(i) = page;
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: XLATPAGE[%d] %#llx\n\0".as_ptr().cast(),
                    libc::getpid(),
                    page,
                );
                return;
            }
            i = i.wrapping_add(1) & (XLP_SIZE as usize - 1);
        }
    }
}

unsafe fn jl_lock_step(rip: u64) {
    unsafe {
        ocerz_critical_depth += 1;
        if g_jl_log < 0 {
            g_jl_log = if !libc::getenv(b"OCERZ_JITLOCKLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if jl_held != 0 {
            jl_recursive_warn(b"jl_lock_step\0".as_ptr().cast());
        }
        if pthread_mutex_trylock(core::ptr::addr_of_mut!(jit_lock)) != 0 {
            let t0 = if g_jl_log > 0 {
                clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            } else {
                0
            };
            if g_jl_log > 0 {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_waits))
                    .fetch_add(1, Ordering::Relaxed);
            }
            let mut s = 0u64;
            loop {
                if s < 64 {
                    sched_yield();
                } else {
                    let ts = timespec {
                        tv_sec: 0,
                        tv_nsec: if s < 512 { 200_000 } else { 2_000_000 },
                    };
                    nanosleep(&ts, core::ptr::null_mut());
                }
                if pthread_mutex_trylock(core::ptr::addr_of_mut!(jit_lock)) == 0 {
                    break;
                }
                if g_jl_log > 0 {
                    let w = clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0);
                    if w > 3_000_000_000 && s % 2000 == 0 {
                        jl_dump(b"WAIT\0".as_ptr().cast(), w);
                    }
                }
                s = s.wrapping_add(1);
            }
        }
        if g_jl_log > 0 {
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_acq)).fetch_add(1, Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_owner))
                .store(jl_tid(), Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_since))
                .store(clock_gettime_nsec_np(CLOCK_UPTIME_RAW), Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_rip)).store(rip, Ordering::Relaxed);
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).store(1, Ordering::Relaxed);
        }
        jl_held += 1;
    }
}

unsafe fn jl_unlock_step() {
    unsafe {
        if g_jl_log > 0 {
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).store(3, Ordering::Relaxed);
        }
        jl_held -= 1;
        pthread_mutex_unlock(core::ptr::addr_of_mut!(jit_lock));
        ocerz_critical_depth -= 1;
        if g_jl_log > 0 {
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase)).store(0, Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_owner)).store(0, Ordering::Relaxed);
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_since)).store(0, Ordering::Relaxed);
        }
    }
}

#[unsafe(no_mangle)]
pub static mut ps_align_patches: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_jit_retire_ns: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ps_chain_far: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ps_chain_ok: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ps_ras_noslot: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ps_chain_veneer: u64 = 0;

static mut g_jit_thr: *mut JitThr = core::ptr::null_mut();

#[thread_local]
static mut t_jit_thr: *mut JitThr = core::ptr::null_mut();

static mut g_jit_thr_key: pthread_key_t = 0;
static mut g_jit_thr_once: pthread_once_t = PTHREAD_ONCE_INIT;
static mut g_flush_gen: u64 = 0;
static mut g_flush_req: c_int = 0;
static mut g_flush_mark: u64 = u64::MAX;
static mut g_flush_retry_ns: u64 = 0;

unsafe extern "C" fn jit_thr_release(p: *mut c_void) {
    unsafe {
        let t = p.cast::<JitThr>();
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).store(0, Ordering::SeqCst);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked)).store(0, Ordering::SeqCst);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).dead)).store(1, Ordering::SeqCst);
    }
}

unsafe extern "C" fn jit_thr_key_init() {
    unsafe {
        pthread_key_create(
            core::ptr::addr_of_mut!(g_jit_thr_key),
            Some(jit_thr_release),
        );
    }
}

unsafe fn jit_thr() -> *mut JitThr {
    unsafe {
        let mut t = t_jit_thr;
        if !t.is_null() {
            return t;
        }
        t = AtomicPtr::<JitThr>::from_ptr(core::ptr::addr_of_mut!(g_jit_thr))
            .load(Ordering::Acquire);
        while !t.is_null() {
            let mut one = 1;
            if AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).dead))
                .compare_exchange(one, 0, Ordering::SeqCst, Ordering::Relaxed)
                .is_ok()
            {
                break;
            }
            t = (*t).next;
        }
        if t.is_null() {
            t = libc::calloc(1, core::mem::size_of::<JitThr>()).cast();
            if t.is_null() {
                libc::abort();
            }
            let ptr = AtomicPtr::<JitThr>::from_ptr(core::ptr::addr_of_mut!(g_jit_thr));
            let mut h = ptr.load(Ordering::Relaxed);
            loop {
                (*t).next = h;
                match ptr.compare_exchange(h, t, Ordering::Release, Ordering::Relaxed) {
                    Ok(_) => break,
                    Err(actual) => h = actual,
                }
            }
        }
        pthread_once(
            core::ptr::addr_of_mut!(g_jit_thr_once),
            Some(jit_thr_key_init),
        );
        pthread_setspecific(g_jit_thr_key, t.cast());
        t_jit_thr = t;
        t
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_thread_mark() -> u64 {
    unsafe {
        let t = jit_thr();
        ((AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).load(Ordering::Relaxed) as u32
            as u64)
            << 32)
            | (AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked)).load(Ordering::Relaxed)
                as u32 as u64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_thread_restore(mark: u64) {
    unsafe {
        let t = jit_thr();
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked))
            .store(mark as u32 as c_int, Ordering::SeqCst);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames))
            .store((mark >> 32) as c_int, Ordering::SeqCst);
    }
}

#[inline(never)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_exec_one_parked(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    insn: *const X86Insn,
    ret: *const c_void,
) -> c_int {
    unsafe {
        if ((*insn).op as c_uint != OCERZ_OP_SYSCALL && (*insn).op as c_uint != OCERZ_OP_INT)
            || ocerz_jit_pc_in_arena(vm, ret) == 0
        {
            return jit_exec_one(vm, cpu, insn);
        }
        let t = jit_thr();
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked)).fetch_add(1, Ordering::SeqCst);
        let generation =
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).load(Ordering::SeqCst);
        let r = jit_exec_one(vm, cpu, insn);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked)).fetch_sub(1, Ordering::SeqCst);
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).load(Ordering::SeqCst) != 0
            || AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).load(Ordering::SeqCst)
                != generation
        {
            ocerz_vm_jit_escape(r);
        }
        r
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn cache_lookup(
    jit: *mut OcerzJit,
    rip: u64,
    mode32: c_int,
) -> *mut JitBlock {
    unsafe {
        let key = jit_key(rip, mode32);
        let mut b = AtomicPtr::<JitBlock>::from_ptr(
            core::ptr::addr_of_mut!((*jit).buckets)
                .cast::<*mut JitBlock>()
                .add(hash_key(key) as usize),
        )
        .load(Ordering::Acquire);
        while !b.is_null() {
            if (*b).key == key {
                return b;
            }
            b = (*b).hnext;
        }
        static mut WL: c_int = -1;
        if WL < 0 {
            WL = if !libc::getenv(b"OCERZ_WILDLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if WL != 0 && (rip >= 0x800000000000 || rip < 0x10000) {
            unsafe extern "C" {
                fn ocerz_vm_riphist(out: *mut u64, max: c_uint) -> c_uint;
                fn ocerz_current_dbg_ind_src() -> u64;
                fn ocerz_current_guest_gpr(reg: c_int) -> u64;
            }
            let mut h = [0u64; 8];
            let n = ocerz_vm_riphist(h.as_mut_ptr(), 8);
            static mut RN: [*const c_char; 16] = [
                b"rax\0".as_ptr().cast(),
                b"rcx\0".as_ptr().cast(),
                b"rdx\0".as_ptr().cast(),
                b"rbx\0".as_ptr().cast(),
                b"rsp\0".as_ptr().cast(),
                b"rbp\0".as_ptr().cast(),
                b"rsi\0".as_ptr().cast(),
                b"rdi\0".as_ptr().cast(),
                b"r8\0".as_ptr().cast(),
                b"r9\0".as_ptr().cast(),
                b"r10\0".as_ptr().cast(),
                b"r11\0".as_ptr().cast(),
                b"r12\0".as_ptr().cast(),
                b"r13\0".as_ptr().cast(),
                b"r14\0".as_ptr().cast(),
                b"r15\0".as_ptr().cast(),
            ];
            let rn = core::ptr::addr_of_mut!(RN).cast::<*const c_char>();
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: WILD-LOOKUP[%d] target=%#llx ind_src=%#llx riphist:\0"
                    .as_ptr()
                    .cast(),
                libc::getpid(),
                rip,
                ocerz_current_dbg_ind_src(),
            );
            for k in 0..n as usize {
                libc::fprintf(
                    crate::log::stderr(),
                    b" %#llx\0".as_ptr().cast(),
                    *h.as_ptr().add(k),
                );
            }
            libc::fprintf(
                crate::log::stderr(),
                b"\n  WILD-GPR[%d]\0".as_ptr().cast(),
                libc::getpid(),
            );
            for g in 0..16 {
                libc::fprintf(
                    crate::log::stderr(),
                    b" %s=%#llx\0".as_ptr().cast(),
                    *rn.add(g),
                    ocerz_current_guest_gpr(g as c_int),
                );
            }
            libc::fprintf(crate::log::stderr(), b"\n\0".as_ptr().cast());
            libc::fflush(crate::log::stderr());
        }
        core::ptr::null_mut()
    }
}

#[inline]
fn invmap_slot(tag: u64) -> usize {
    let h = tag.wrapping_mul(0x9e3779b97f4a7c15);
    ((h >> 40) & (INVMAP_SLOTS as u64 - 1)) as usize
}

#[inline]
fn invmap_mask(region: u64, lo: u64, hi: u64) -> u64 {
    let rlo = region << INVMAP_RSHIFT;
    let rhi = rlo.wrapping_add(1u64 << INVMAP_RSHIFT);
    let a = lo.max(rlo);
    let b = hi.min(rhi);
    let g0 = (a.wrapping_sub(rlo) >> INVMAP_GSHIFT) as u32;
    let g1 = (b.wrapping_sub(1).wrapping_sub(rlo) >> INVMAP_GSHIFT) as u32;
    if g1.wrapping_sub(g0) >= 63 {
        u64::MAX
    } else {
        ((1u64 << g1.wrapping_sub(g0).wrapping_add(1)) - 1) << g0
    }
}

unsafe fn invmap_add(jit: *mut OcerzJit, lo: u64, hi: u64) {
    unsafe {
        let mut r = lo >> INVMAP_RSHIFT;
        while r <= hi.wrapping_sub(1) >> INVMAP_RSHIFT {
            let tag = r.wrapping_add(1);
            let m = invmap_mask(r, lo, hi);
            let mut i = invmap_slot(tag);
            let mut tomb = -1isize;
            let mut done = false;
            for _ in 0..INVMAP_PROBE {
                let slot = core::ptr::addr_of_mut!((*jit).invmap)
                    .cast::<InvSlot>()
                    .add(i);
                if (*slot).tag == INVMAP_TOMB {
                    if tomb < 0 {
                        tomb = i as isize;
                    }
                } else if (*slot).tag == 0 {
                    if tomb >= 0 {
                        i = tomb as usize;
                    }
                    (*core::ptr::addr_of_mut!((*jit).invmap)
                        .cast::<InvSlot>()
                        .add(i))
                    .tag = tag;
                }
                let slot = core::ptr::addr_of_mut!((*jit).invmap)
                    .cast::<InvSlot>()
                    .add(i);
                if (*slot).tag == tag {
                    (*slot).bits |= m;
                    done = true;
                    break;
                }
                i = i.wrapping_add(1) & (INVMAP_SLOTS as usize - 1);
            }
            if !done && tomb >= 0 {
                let slot = core::ptr::addr_of_mut!((*jit).invmap)
                    .cast::<InvSlot>()
                    .add(tomb as usize);
                (*slot).tag = tag;
                (*slot).bits = m;
            } else if !done {
                (*jit).invmap_full = 1;
            }
            r = r.wrapping_add(1);
        }
    }
}

unsafe fn invmap_clear_range(jit: *mut OcerzJit, lo: u64, hi: u64) {
    unsafe {
        let g = 1u64 << INVMAP_GSHIFT;
        let clo = lo.wrapping_add(g - 1) & !(g - 1);
        let chi = hi & !(g - 1);
        if clo >= chi || (*jit).invmap_full != 0 {
            return;
        }
        if (chi.wrapping_sub(1) >> INVMAP_RSHIFT).wrapping_sub(clo >> INVMAP_RSHIFT)
            >= INVMAP_MAX_SPAN as u64
        {
            return;
        }
        let mut r = clo >> INVMAP_RSHIFT;
        while r <= chi.wrapping_sub(1) >> INVMAP_RSHIFT {
            let tag = r.wrapping_add(1);
            let m = invmap_mask(r, clo, chi);
            let mut i = invmap_slot(tag);
            for _ in 0..INVMAP_PROBE {
                let slot = core::ptr::addr_of_mut!((*jit).invmap)
                    .cast::<InvSlot>()
                    .add(i);
                if (*slot).tag == 0 {
                    break;
                }
                if (*slot).tag == tag {
                    (*slot).bits &= !m;
                    if (*slot).bits == 0 {
                        (*slot).tag = INVMAP_TOMB;
                    }
                    break;
                }
                i = i.wrapping_add(1) & (INVMAP_SLOTS as usize - 1);
            }
            r = r.wrapping_add(1);
        }
    }
}

unsafe fn invmap_may_hold(jit: *const OcerzJit, lo: u64, hi: u64) -> c_int {
    unsafe {
        let r0 = lo >> INVMAP_RSHIFT;
        let r1 = hi.wrapping_sub(1) >> INVMAP_RSHIFT;
        if env_on!("OCERZ_NO_INVMAP") != 0
            || (*jit).invmap_full != 0
            || r1.wrapping_sub(r0) >= INVMAP_MAX_SPAN as u64
        {
            return 1;
        }
        let mut r = r0;
        while r <= r1 {
            let tag = r.wrapping_add(1);
            let m = invmap_mask(r, lo, hi);
            let mut i = invmap_slot(tag);
            for _ in 0..INVMAP_PROBE {
                let slot = core::ptr::addr_of!((*jit).invmap).cast::<InvSlot>().add(i);
                if (*slot).tag == 0 {
                    break;
                }
                if (*slot).tag == tag {
                    if (*slot).bits & m != 0 {
                        return 1;
                    }
                    break;
                }
                i = i.wrapping_add(1) & (INVMAP_SLOTS as usize - 1);
            }
            r = r.wrapping_add(1);
        }
        0
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct GranEnt {
    page: u64,
    count: i32,
}

static mut g_gran: [GranEnt; GRAN_SLOTS as usize] =
    [GranEnt { page: 0, count: 0 }; GRAN_SLOTS as usize];

static mut g_gran4: [GranEnt; GRAN4_SLOTS as usize] =
    [GranEnt { page: 0, count: 0 }; GRAN4_SLOTS as usize];

static mut g_gran_degenerate: c_int = 0;

unsafe fn gran4_bump(row: u64, d: c_int) {
    unsafe {
        let mut i =
            (row.wrapping_mul(0x9E3779B97F4A7C15) >> 50) as usize & (GRAN4_SLOTS as usize - 1);
        let gran4 = core::ptr::addr_of_mut!(g_gran4).cast::<GranEnt>();
        for _ in 0..32 {
            let ent = gran4.add(i);
            if (*ent).page == row || ((*ent).page == 0 && (*ent).count == 0) {
                (*ent).page = row;
                (*ent).count = (*ent).count.wrapping_add(d);
                return;
            }
            i = i.wrapping_add(1) & (GRAN4_SLOTS as usize - 1);
        }
        g_gran_degenerate = 1;
    }
}

unsafe fn gran4_count(row: u64) -> c_int {
    unsafe {
        let mut i =
            (row.wrapping_mul(0x9E3779B97F4A7C15) >> 50) as usize & (GRAN4_SLOTS as usize - 1);
        let gran4 = core::ptr::addr_of_mut!(g_gran4).cast::<GranEnt>();
        for _ in 0..32 {
            let ent = gran4.add(i);
            if (*ent).page == row {
                return (*ent).count;
            }
            if (*ent).page == 0 && (*ent).count == 0 {
                return 0;
            }
            i = i.wrapping_add(1) & (GRAN4_SLOTS as usize - 1);
        }
        1
    }
}

unsafe fn gran_bump(rip: u64, d: c_int) {
    unsafe {
        let page = rip >> INVMAP_GSHIFT;
        gran4_bump(page >> 6, d);
        let mut i =
            (page.wrapping_mul(0x9E3779B97F4A7C15) >> 48) as usize & (GRAN_SLOTS as usize - 1);
        let gran = core::ptr::addr_of_mut!(g_gran).cast::<GranEnt>();
        for _ in 0..32 {
            let ent = gran.add(i);
            if (*ent).page == page || ((*ent).page == 0 && (*ent).count == 0) {
                (*ent).page = page;
                (*ent).count = (*ent).count.wrapping_add(d);
                return;
            }
            i = i.wrapping_add(1) & (GRAN_SLOTS as usize - 1);
        }
        g_gran_degenerate = 1;
    }
}

unsafe fn gran_fine_any(p0: u64, p1: u64) -> c_int {
    unsafe {
        let mut p = p0;
        let gran = core::ptr::addr_of_mut!(g_gran).cast::<GranEnt>();
        while p <= p1 {
            let mut i =
                (p.wrapping_mul(0x9E3779B97F4A7C15) >> 48) as usize & (GRAN_SLOTS as usize - 1);
            for _ in 0..32 {
                let ent = gran.add(i);
                if (*ent).page == p {
                    if (*ent).count > 0 {
                        return 1;
                    }
                    break;
                }
                if (*ent).page == 0 && (*ent).count == 0 {
                    break;
                }
                i = i.wrapping_add(1) & (GRAN_SLOTS as usize - 1);
            }
            p = p.wrapping_add(1);
        }
        0
    }
}

unsafe fn gran_any(lo: u64, hi: u64) -> c_int {
    unsafe {
        if g_gran_degenerate != 0 {
            return 1;
        }
        let p0 = lo >> INVMAP_GSHIFT;
        let p1 = hi.wrapping_sub(1) >> INVMAP_GSHIFT;
        if p1.wrapping_sub(p0) < 64 {
            return gran_fine_any(p0, p1);
        }
        let mut r = p0 >> 6;
        while r <= p1 >> 6 {
            if gran4_count(r) > 0 {
                let f0 = (r << 6).max(p0);
                let f1 = (r << 6).wrapping_add(63).min(p1);
                if gran_fine_any(f0, f1) != 0 {
                    return 1;
                }
            }
            r = r.wrapping_add(1);
        }
        0
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct GblkEnt {
    page: u64,
    n: u32,
    cap: u32,
    v: *mut *mut JitBlock,
}

static mut g_gblk: [GblkEnt; GBLK_SLOTS as usize] = [GblkEnt {
    page: 0,
    n: 0,
    cap: 0,
    v: core::ptr::null_mut(),
}; GBLK_SLOTS as usize];

static mut g_gblk_off: c_int = -1;

unsafe fn gblk_slot(page: u64, create: c_int) -> c_int {
    unsafe {
        let mut i =
            (page.wrapping_mul(0x9E3779B97F4A7C15) >> 48) as usize & (GBLK_SLOTS as usize - 1);
        let gblk = core::ptr::addr_of_mut!(g_gblk).cast::<GblkEnt>();
        for _ in 0..64 {
            let ent = gblk.add(i);
            if (*ent).page == page.wrapping_add(1) {
                return i as c_int;
            }
            if (*ent).page == 0 {
                if create == 0 {
                    return -1;
                }
                (*ent).page = page.wrapping_add(1);
                return i as c_int;
            }
            i = i.wrapping_add(1) & (GBLK_SLOTS as usize - 1);
        }
        if create != 0 {
            -2
        } else {
            -1
        }
    }
}

unsafe fn gblk_note(page: u64, b: *mut JitBlock, d: c_int) {
    unsafe {
        if g_gblk_off != 0 {
            return;
        }
        let s = gblk_slot(page, (d > 0) as c_int);
        if s < 0 {
            if s == -2 || d > 0 {
                g_gblk_off = 1;
            }
            return;
        }
        let entry = core::ptr::addr_of_mut!(g_gblk)
            .cast::<GblkEnt>()
            .add(s as usize);
        if d > 0 {
            if (*entry).n == (*entry).cap {
                let nc = if (*entry).cap != 0 {
                    (*entry).cap.wrapping_mul(2)
                } else {
                    8
                };
                let nv = libc::realloc(
                    (*entry).v.cast(),
                    (nc as usize).wrapping_mul(core::mem::size_of::<*mut JitBlock>()),
                )
                .cast::<*mut JitBlock>();
                if nv.is_null() {
                    g_gblk_off = 1;
                    return;
                }
                (*entry).v = nv;
                (*entry).cap = nc;
            }
            *(*entry).v.add((*entry).n as usize) = b;
            (*entry).n = (*entry).n.wrapping_add(1);
            return;
        }
        for j in 0..(*entry).n {
            if *(*entry).v.add(j as usize) == b {
                (*entry).n = (*entry).n.wrapping_sub(1);
                *(*entry).v.add(j as usize) = *(*entry).v.add((*entry).n as usize);
                return;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn gran_block(b: *mut JitBlock, d: c_int) {
    unsafe {
        if (*b).n_insns <= 0 {
            return;
        }
        if g_gblk_off < 0 {
            g_gblk_off = if !libc::getenv(b"OCERZ_INV_SCAN\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        let mut lo = blk_insn_rip(b, 0);
        let mut hi = lo.wrapping_add(blk_insn_len(b, 0) as u64);
        for i in 1..=(*b).n_insns {
            if i < (*b).n_insns && blk_insn_rip(b, i) == hi {
                hi = hi.wrapping_add(blk_insn_len(b, i) as u64);
                continue;
            }
            let mut p = lo >> INVMAP_GSHIFT;
            while p <= hi.wrapping_sub(1) >> INVMAP_GSHIFT {
                gran_bump(p << INVMAP_GSHIFT, d);
                gblk_note(p, b, d);
                p = p.wrapping_add(1);
            }
            if i < (*b).n_insns {
                lo = blk_insn_rip(b, i);
                hi = lo.wrapping_add(blk_insn_len(b, i) as u64);
            }
        }
    }
}

unsafe fn gran_clear_all() {
    unsafe {
        core::ptr::write_bytes(
            core::ptr::addr_of_mut!(g_gran).cast::<u8>(),
            0,
            core::mem::size_of::<[GranEnt; GRAN_SLOTS as usize]>(),
        );
        core::ptr::write_bytes(
            core::ptr::addr_of_mut!(g_gran4).cast::<u8>(),
            0,
            core::mem::size_of::<[GranEnt; GRAN4_SLOTS as usize]>(),
        );
        g_gran_degenerate = 0;
        let gblk = core::ptr::addr_of_mut!(g_gblk).cast::<GblkEnt>();
        for i in 0..GBLK_SLOTS as usize {
            libc::free((*gblk.add(i)).v.cast());
        }
        core::ptr::write_bytes(
            core::ptr::addr_of_mut!(g_gblk).cast::<u8>(),
            0,
            core::mem::size_of::<[GblkEnt; GBLK_SLOTS as usize]>(),
        );
        g_gblk_off = if !libc::getenv(b"OCERZ_INV_SCAN\0".as_ptr().cast()).is_null() {
            1
        } else {
            0
        };
    }
}

unsafe fn shrink_edges(b: *mut JitBlock) {
    unsafe {
        let n = if (*b).n_edges != 0 {
            (*b).n_edges as usize
        } else {
            1
        };
        if !(*b).edges.is_null() && n < JIT_MAX_EDGES as usize {
            let p = libc::realloc(
                (*b).edges.cast(),
                n * core::mem::size_of::<JitBlock__bindgen_ty_2>(),
            );
            if !p.is_null() {
                (*b).edges = p.cast();
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn cache_insert(jit: *mut OcerzJit, b: *mut JitBlock) {
    unsafe {
        shrink_edges(b);
        let h = hash_key((*b).key);
        (*b).hnext = *core::ptr::addr_of!((*jit).buckets)
            .cast::<*mut JitBlock>()
            .add(h as usize);
        AtomicPtr::<JitBlock>::from_ptr(
            core::ptr::addr_of_mut!((*jit).buckets)
                .cast::<*mut JitBlock>()
                .add(h as usize),
        )
        .store(b, Ordering::Release);
        if (*jit).n_live == (*jit).cap_live {
            let nc = if (*jit).cap_live != 0 {
                (*jit).cap_live.wrapping_mul(2)
            } else {
                4096
            };
            let nl = libc::realloc(
                (*jit).live.cast(),
                nc.wrapping_mul(core::mem::size_of::<*mut JitBlock>()),
            )
            .cast::<*mut JitBlock>();
            if !nl.is_null() {
                (*jit).live = nl;
                (*jit).cap_live = nc;
            }
        }
        if (*jit).n_live < (*jit).cap_live {
            (*b).live_idx = (*jit).n_live;
            *(*jit).live.add((*jit).n_live) = b;
            (*jit).n_live = (*jit).n_live.wrapping_add(1);
        }
        gran_block(b, 1);
        if (*b).n_insns > 0 {
            let mut lo = blk_insn_rip(b, 0);
            let mut hi = lo.wrapping_add(blk_insn_len(b, 0) as u64);
            for i in 1..=(*b).n_insns {
                if i < (*b).n_insns && blk_insn_rip(b, i) == hi {
                    hi = hi.wrapping_add(blk_insn_len(b, i) as u64);
                    continue;
                }
                if (*jit).code_hi == 0 {
                    (*jit).code_lo = lo;
                    (*jit).code_hi = hi;
                } else {
                    if lo < (*jit).code_lo {
                        (*jit).code_lo = lo;
                    }
                    if hi > (*jit).code_hi {
                        (*jit).code_hi = hi;
                    }
                }
                invmap_add(jit, lo, hi);
                ocerz_cache_arm_exec(lo, hi);
                ocerz_mem_arm_exec(lo, hi);
                if i < (*b).n_insns {
                    lo = blk_insn_rip(b, i);
                    hi = lo.wrapping_add(blk_insn_len(b, i) as u64);
                }
            }
        }
    }
}

static mut g_flush_want: c_int = 0;

unsafe fn jit_table_full(what: *const c_char) {
    unsafe {
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_want)).swap(1, Ordering::Relaxed)
            == 0
        {
            static mut SAID: c_int = 0;
            if SAID == 0 || !libc::getenv(b"OCERZ_FLUSHLOG\0".as_ptr().cast()).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: note: JIT %s are used up; the arena is flushed at the next translation\n\0"
                        .as_ptr()
                        .cast(),
                    what,
                );
            }
            SAID = SAID.wrapping_add(1);
        }
    }
}

static mut js_hits: u64 = 0;
static mut js_misses: u64 = 0;
static mut js_steps: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_xlat_fail: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_xlat_ok: u64 = 0;

static mut js_xlat: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_fail_alloc: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_fail_decode0: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_fail_overflow: u64 = 0;

#[unsafe(no_mangle)]
pub static mut js_decoded_insns: u64 = 0;

static mut js_t0: u64 = 0;
static mut js_ftab: [JsFail; JS_FTAB as usize] = [JsFail {
    rip: 0,
    n: 0,
    reason: 0,
    nins: 0,
    bytes: [0; 8],
}; JS_FTAB as usize];
static mut js_ftab_full: c_uint = 0;
static mut js_ftab_used: c_uint = 0;

unsafe extern "C" {
    fn jit_cache_copy8_recover(dst: *mut c_void, src: *const c_void);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_note_fail(rip: u64, reason: c_uint, nins: c_int) {
    unsafe {
        let mut x = rip;
        x ^= x >> 33;
        x = x.wrapping_mul(0xff51afd7ed558ccd);
        x ^= x >> 29;
        let h = (x & (JS_FTAB as u64 - 1)) as usize;
        for i in 0..8usize {
            let k = (h + i) & (JS_FTAB as usize - 1);
            let ent = core::ptr::addr_of_mut!(js_ftab).cast::<JsFail>().add(k);
            if (*ent).n != 0 && (*ent).rip == rip {
                (*ent).n = (*ent).n.wrapping_add(1);
                return;
            }
            if (*ent).n == 0 {
                (*ent).rip = rip;
                (*ent).n = 1;
                (*ent).reason = reason;
                (*ent).nins = nins;
                let c = ocerz_g2h(rip).cast::<u8>();
                jit_cache_copy8_recover(
                    core::ptr::addr_of_mut!((*ent).bytes).cast(),
                    c.cast_const().cast(),
                );
                js_ftab_used = js_ftab_used.wrapping_add(1);
                return;
            }
        }
        js_ftab_full = js_ftab_full.wrapping_add(1);
    }
}

unsafe extern "C" fn js_cmp(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = (*(a.cast::<JsFail>())).n;
        let y = (*(b.cast::<JsFail>())).n;
        if x < y {
            1
        } else if x > y {
            -1
        } else {
            0
        }
    }
}

unsafe fn js_report(jit: *mut OcerzJit, tag: *const c_char, with_ftab: c_int) {
    unsafe {
        let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        let sec = now.wrapping_sub(js_t0) as f64 / 1e9;
        let st = AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_steps)).load(Ordering::SeqCst);
        let hi = AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_hits)).load(Ordering::SeqCst);
        let mi = AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_misses)).load(Ordering::SeqCst);
        let xf = AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_xlat_fail)).load(Ordering::SeqCst);
        let xo = AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_xlat_ok)).load(Ordering::SeqCst);
        let used = (*jit).code_cur as usize - (*jit).code_base as usize;
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: JITSTAT[%d] %s t=%.1fs steps=%llu hits=%llu (%.4f%%) misses=%llu (%.0f lock/s)\n\
ocerz: JITSTAT[%d]   translate: calls=%llu ok=%llu fail=%llu (decode0=%llu overflow=%llu alloc=%llu)\n\
ocerz: JITSTAT[%d]   decoded_insns_on_miss=%llu  code_used=%zu/%zu bytes (%.1f%%) EXHAUSTED=%d  failtab_used=%u full=%u\n\0"
                .as_ptr()
                .cast(),
            libc::getpid(),
            tag,
            sec,
            st,
            hi,
            if st != 0 { 100.0 * hi as f64 / st as f64 } else { 0.0 },
            mi,
            if sec > 0.0 { mi as f64 / sec } else { 0.0 },
            libc::getpid(),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_xlat)).load(Ordering::SeqCst),
            xo,
            xf,
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_fail_decode0))
                .load(Ordering::SeqCst),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_fail_overflow))
                .load(Ordering::SeqCst),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_fail_alloc)).load(Ordering::SeqCst),
            libc::getpid(),
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_decoded_insns))
                .load(Ordering::SeqCst),
            used,
            (*jit).code_bytes,
            100.0 * used as f64 / (*jit).code_bytes as f64,
            (((*jit).code_end as usize).wrapping_sub((*jit).code_cur as usize) < 4096) as c_int,
            js_ftab_used,
            js_ftab_full,
        );
        if with_ftab == 0 {
            return;
        }
        static mut SNAP: [JsFail; JS_FTAB as usize] = [JsFail {
            rip: 0,
            n: 0,
            reason: 0,
            nins: 0,
            bytes: [0; 8],
        }; JS_FTAB as usize];
        core::ptr::copy_nonoverlapping(
            core::ptr::addr_of!(js_ftab).cast::<JsFail>(),
            core::ptr::addr_of_mut!(SNAP).cast::<JsFail>(),
            JS_FTAB as usize,
        );
        libc::qsort(
            core::ptr::addr_of_mut!(SNAP).cast(),
            JS_FTAB as usize,
            core::mem::size_of::<JsFail>(),
            Some(js_cmp),
        );
        for i in 0..25usize {
            let f = core::ptr::addr_of!(SNAP).cast::<JsFail>().add(i);
            if (*f).n == 0 {
                break;
            }
            let reason = if (*f).reason == JSR_DECODE0 {
                b"decode-fail\0".as_ptr()
            } else if (*f).reason == JSR_OVERFLOW {
                b"code-overflow\0".as_ptr()
            } else {
                b"alloc-fail\0".as_ptr()
            };
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: JITSTAT[%d]   FAILRIP #%2d %#18llx retries=%-10llu reason=%s nins=%d bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n\0"
                    .as_ptr()
                    .cast(),
                libc::getpid(),
                i as c_int,
                (*f).rip,
                (*f).n,
                reason,
                (*f).nins,
                (*f).bytes[0] as c_uint,
                (*f).bytes[1] as c_uint,
                (*f).bytes[2] as c_uint,
                (*f).bytes[3] as c_uint,
                (*f).bytes[4] as c_uint,
                (*f).bytes[5] as c_uint,
                (*f).bytes[6] as c_uint,
                (*f).bytes[7] as c_uint,
            );
        }
    }
}

static mut g_chain_patched: [*mut u32; CHAIN_BATCH_MAX as usize] =
    [core::ptr::null_mut(); CHAIN_BATCH_MAX as usize];
static mut g_chain_npatched: c_int = 0;
static mut g_chain_batching: c_int = 0;

unsafe fn chain_batch_begin() {
    unsafe {
        g_chain_npatched = 0;
        g_chain_batching = 1;
        pthread_jit_write_protect_np(0);
    }
}

unsafe fn chain_batch_end() {
    unsafe {
        pthread_jit_write_protect_np(1);
        for i in 0..g_chain_npatched {
            let site = *core::ptr::addr_of!(g_chain_patched)
                .cast::<*mut u32>()
                .add(i as usize);
            sys_icache_invalidate(site.cast(), 4);
        }
        g_chain_npatched = 0;
        g_chain_batching = 0;
    }
}

unsafe fn chain_cond_short(cond_site: *mut u32, dst: *mut c_void) {
    unsafe {
        if cond_site.is_null() || dst.is_null() {
            return;
        }
        chaincheck(b"chain_cond_short\0".as_ptr().cast(), dst);
        if !g_xlat_jit.is_null() && (*g_xlat_jit).stop_requested != 0 {
            return;
        }
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).load(Ordering::SeqCst) != 0 {
            return;
        }
        let w = *cond_site;
        let off = (dst as isize).wrapping_sub(cond_site as isize) / 4;
        let nw;
        if (w & 0xff000010) == 0x54000000 || (w & 0x7e000000) == 0x34000000 {
            if off < -(1 << 18) || off >= 1 << 18 {
                return;
            }
            nw = (w & !(0x7ffff << 5)) | (((off as u32) & 0x7ffff) << 5);
        } else if (w & 0x7e000000) == 0x36000000 {
            if off < -(1 << 13) || off >= 1 << 13 {
                return;
            }
            nw = (w & !(0x3fff << 5)) | (((off as u32) & 0x3fff) << 5);
        } else {
            return;
        }
        if g_chain_batching != 0 {
            *cond_site = nw;
            if g_chain_npatched < CHAIN_BATCH_MAX as c_int {
                *core::ptr::addr_of_mut!(g_chain_patched)
                    .cast::<*mut u32>()
                    .add(g_chain_npatched as usize) = cond_site;
                g_chain_npatched += 1;
            } else {
                sys_icache_invalidate(cond_site.cast(), 4);
            }
        } else {
            pthread_jit_write_protect_np(0);
            *cond_site = nw;
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(cond_site.cast(), 4);
        }
    }
}

unsafe fn chaincheck(what: *const c_char, dst: *const c_void) {
    static mut EN: c_int = -1;
    unsafe {
        if EN < 0 {
            EN = if !libc::getenv(b"OCERZ_CHAINCHECK\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if EN == 0 || dst.is_null() || g_xlat_jit.is_null() {
            return;
        }
        let d = dst as usize;
        if d < (*g_xlat_jit).code_base as usize || d >= (*g_xlat_jit).code_cur as usize {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: CHAINCHECK[%d] %s target %p OUTSIDE arena [%p,%p)\n\0"
                    .as_ptr()
                    .cast(),
                libc::getpid(),
                what,
                dst,
                (*g_xlat_jit).code_base,
                (*g_xlat_jit).code_cur,
            );
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn veneer_pool_check(jit: *mut OcerzJit) {
    unsafe {
        let veneer_pool = core::ptr::addr_of_mut!((*jit).veneer_pool).cast::<*mut u32>();
        let veneer_used = core::ptr::addr_of_mut!((*jit).veneer_used).cast::<u32>();
        if (*jit).veneer_next_mark.is_null() {
            (*jit).veneer_next_mark = (*jit).code_base.cast();
        }
        while (*jit).veneer_n < 16 && (*jit).code_cur as usize >= (*jit).veneer_next_mark as usize {
            if ((*jit).code_end as usize).wrapping_sub((*jit).code_cur as usize)
                < VENEER_POOL_BYTES as usize + 65536
            {
                return;
            }
            *veneer_pool.add((*jit).veneer_n as usize) = (*jit).code_cur;
            *veneer_used.add((*jit).veneer_n as usize) = 0;
            (*jit).veneer_n = (*jit).veneer_n.wrapping_add(1);
            (*jit).code_cur = ((*jit).code_cur as *mut u8)
                .add(VENEER_POOL_BYTES as usize)
                .cast();
            (*jit).veneer_next_mark = (*jit).veneer_next_mark.add(VENEER_WINDOW_BYTES as usize);
        }
    }
}

unsafe fn veneer_make(
    jit: *mut OcerzJit,
    site: *const u32,
    dst: *const c_void,
    batching: c_int,
) -> *mut u32 {
    unsafe {
        let mut best = -1isize;
        let mut best_d = VENEER_REACH as isize;
        let veneer_pool = core::ptr::addr_of_mut!((*jit).veneer_pool).cast::<*mut u32>();
        let veneer_used = core::ptr::addr_of_mut!((*jit).veneer_used).cast::<u32>();
        for i in 0..(*jit).veneer_n {
            let used = *veneer_used.add(i as usize);
            if used.wrapping_add(1).wrapping_mul(VENEER_BYTES as u32) as usize
                > VENEER_POOL_BYTES as usize
            {
                continue;
            }
            let mut d = ((*veneer_pool.add(i as usize)) as isize).wrapping_sub(site as isize);
            if d < 0 {
                d = d.wrapping_neg();
            }
            if d < best_d {
                best_d = d;
                best = i as isize;
            }
        }
        if best < 0 {
            return core::ptr::null_mut();
        }
        let used = *veneer_used.add(best as usize);
        let v = (*veneer_pool.add(best as usize) as *mut u8)
            .add(used.wrapping_mul(VENEER_BYTES as u32) as usize)
            .cast::<u32>();
        let target = dst as u64;
        if batching == 0 {
            pthread_jit_write_protect_np(0);
        }
        *v = 0x58000040 | (JTA as u32);
        *v.add(1) = 0xd61f0000 | ((JTA as u32) << 5);
        core::ptr::copy_nonoverlapping(
            core::ptr::addr_of!(target).cast::<u8>(),
            v.add(2).cast::<u8>(),
            8,
        );
        if batching == 0 {
            pthread_jit_write_protect_np(1);
        }
        sys_icache_invalidate(v.cast(), VENEER_BYTES as usize);
        *veneer_used.add(best as usize) = used.wrapping_add(1);
        v
    }
}

unsafe fn chain_activate(patch_b: *mut u32, dst: *mut c_void) {
    unsafe {
        if patch_b.is_null() || dst.is_null() {
            return;
        }
        chaincheck(b"chain_activate\0".as_ptr().cast(), dst);
        if !g_xlat_jit.is_null() && (*g_xlat_jit).stop_requested != 0 {
            return;
        }
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).load(Ordering::SeqCst) != 0 {
            return;
        }
        let mut ok;
        let mut veneered = 0;
        if g_chain_batching != 0 {
            ok = a64_try_patch_b(patch_b, dst.cast());
            if ok == 0 && !g_xlat_jit.is_null() {
                let v = veneer_make(g_xlat_jit, patch_b, dst, 1);
                if !v.is_null() {
                    ok = a64_try_patch_b(patch_b, v);
                    veneered = (ok != 0) as c_int;
                }
            }
            if g_chain_npatched < CHAIN_BATCH_MAX as c_int {
                *core::ptr::addr_of_mut!(g_chain_patched)
                    .cast::<*mut u32>()
                    .add(g_chain_npatched as usize) = patch_b;
                g_chain_npatched += 1;
            } else {
                sys_icache_invalidate(patch_b.cast(), 4);
            }
        } else {
            pthread_jit_write_protect_np(0);
            ok = a64_try_patch_b(patch_b, dst.cast());
            pthread_jit_write_protect_np(1);
            if ok == 0 && !g_xlat_jit.is_null() {
                let v = veneer_make(g_xlat_jit, patch_b, dst, 0);
                if !v.is_null() {
                    pthread_jit_write_protect_np(0);
                    ok = a64_try_patch_b(patch_b, v);
                    pthread_jit_write_protect_np(1);
                    veneered = (ok != 0) as c_int;
                }
            }
            sys_icache_invalidate(patch_b.cast(), 4);
        }
        if ocerz_perfstat > 0 {
            if veneered != 0 {
                ps_chain_veneer = ps_chain_veneer.wrapping_add(1);
            } else if ok != 0 {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(ps_chain_ok))
                    .fetch_add(1, Ordering::SeqCst);
            } else {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(ps_chain_far))
                    .fetch_add(1, Ordering::SeqCst);
            }
        }
    }
}

static mut g_pending: [*mut PendingChain; PEND_SIZE as usize] =
    [core::ptr::null_mut(); PEND_SIZE as usize];

unsafe fn pred_add(target: *mut JitBlock, src: *mut JitBlock, e: c_int) {
    unsafe {
        if target.is_null() || src.is_null() || target == src {
            return;
        }
        if (*target).n_preds != 0
            && (*(*target)
                .preds
                .add((*target).n_preds.wrapping_sub(1) as usize))
            .pb == src
            && (*(*target)
                .preds
                .add((*target).n_preds.wrapping_sub(1) as usize))
            .e as c_int
                == e
        {
            return;
        }
        if (*target).n_preds == (*target).cap_preds {
            let cap = if (*target).cap_preds != 0 {
                (*target).cap_preds.wrapping_mul(2)
            } else {
                4
            };
            let np = libc::realloc(
                (*target).preds.cast(),
                (cap as usize).wrapping_mul(core::mem::size_of::<JitBlock__bindgen_ty_3>()),
            )
            .cast::<JitBlock__bindgen_ty_3>();
            if np.is_null() {
                return;
            }
            (*target).preds = np;
            (*target).cap_preds = cap;
        }
        let pred = (*target).preds.add((*target).n_preds as usize);
        (*pred).pb = src;
        (*pred).e = e as u8;
        (*target).n_preds = (*target).n_preds.wrapping_add(1);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn pending_add(
    target_key: u64,
    patch_b: *mut u32,
    kind: u8,
    pin_class: u8,
    cond_site: *mut u32,
    src_sig: u64,
    src: *mut JitBlock,
    edge: c_int,
) {
    unsafe {
        let e = libc::malloc(core::mem::size_of::<PendingChain>()).cast::<PendingChain>();
        if e.is_null() {
            return;
        }
        let h = (hash_key(target_key) as usize) & (PEND_MASK as usize);
        (*e).target_key = target_key;
        (*e).patch_b = patch_b;
        (*e).cond_site = cond_site;
        (*e).ras_slot = core::ptr::null_mut();
        (*e).src = src;
        (*e).edge = edge as u8;
        (*e).src_sig = src_sig;
        (*e).kind = kind;
        (*e).pin_class = pin_class;
        let pending = core::ptr::addr_of_mut!(g_pending).cast::<*mut PendingChain>();
        (*e).next = *pending.add(h);
        *pending.add(h) = e;
    }
}

#[unsafe(no_mangle)]
pub static mut g_ras_slots: *mut *mut c_void = core::ptr::null_mut();

#[unsafe(no_mangle)]
pub static mut g_ras_slot_n: c_uint = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ras_slot_alloc() -> *mut *mut c_void {
    unsafe {
        if g_ras_slots.is_null() {
            let p = libc::mmap(
                core::ptr::null_mut(),
                (RAS_SLOT_CAP as usize).wrapping_mul(core::mem::size_of::<*mut c_void>()),
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_ANON | libc::MAP_PRIVATE,
                -1,
                0,
            );
            if p == libc::MAP_FAILED {
                return core::ptr::null_mut();
            }
            g_ras_slots = p.cast();
        }
        if g_ras_slot_n >= RAS_SLOT_CAP {
            ps_ras_noslot = ps_ras_noslot.wrapping_add(1);
            jit_table_full(b"return-address slots\0".as_ptr().cast());
            return core::ptr::null_mut();
        }
        let s = g_ras_slots.add(g_ras_slot_n as usize);
        g_ras_slot_n = g_ras_slot_n.wrapping_add(1);
        *s = core::ptr::null_mut();
        s
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn pending_add_ras(target_key: u64, ras_slot: *mut *mut c_void) {
    unsafe {
        let e = libc::malloc(core::mem::size_of::<PendingChain>()).cast::<PendingChain>();
        if e.is_null() {
            return;
        }
        let h = (hash_key(target_key) as usize) & (PEND_MASK as usize);
        (*e).target_key = target_key;
        (*e).patch_b = core::ptr::null_mut();
        (*e).cond_site = core::ptr::null_mut();
        (*e).ras_slot = ras_slot;
        (*e).src = core::ptr::null_mut();
        (*e).edge = 0;
        (*e).kind = EDGE_XBLOCK as u8;
        (*e).pin_class = 0;
        let pending = core::ptr::addr_of_mut!(g_pending).cast::<*mut PendingChain>();
        (*e).next = *pending.add(h);
        *pending.add(h) = e;
    }
}

unsafe fn body_entry_for(t: *const JitBlock, src_sig: u64) -> *mut c_void {
    static mut DIS: c_int = -1;
    unsafe {
        if DIS < 0 {
            DIS = if !libc::getenv(b"OCERZ_NO_HOIST_HANDOFF\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if DIS == 0
            && src_sig != 0
            && (*t).hoist_sig == src_sig
            && !(*t).body_noreload.is_null()
            && ((src_sig >> 24) & 0xff) == 0
        {
            (*t).body_noreload.cast()
        } else {
            (*t).body_code.cast()
        }
    }
}

unsafe fn pending_drain(key: u64, target: *mut JitBlock) {
    unsafe {
        let h = (hash_key(key) as usize) & (PEND_MASK as usize);
        let mut pp = core::ptr::addr_of_mut!(g_pending)
            .cast::<*mut PendingChain>()
            .add(h);
        while !(*pp).is_null() {
            let e = *pp;
            if (*e).target_key == key {
                if !(*e).ras_slot.is_null() {
                    chaincheck(b"ras_slot\0".as_ptr().cast(), ras_entry_for(target));
                    AtomicPtr::<c_void>::from_ptr((*e).ras_slot)
                        .store(ras_entry_for(target), Ordering::Release);
                } else if (*e).kind as c_uint == EDGE_BODY {
                    let compatible = if (*e).pin_class != 0 {
                        (*target).pin_class == (*e).pin_class
                    } else {
                        (*target).pin_class == 0 && (*target).n_pinned == 0
                    };
                    if compatible && !(*target).body_code.is_null() {
                        let dst = body_entry_for(target, (*e).src_sig);
                        chain_activate((*e).patch_b, dst);
                        chain_cond_short((*e).cond_site, dst);
                        pred_add(target, (*e).src, (*e).edge as c_int);
                    }
                } else {
                    chain_activate(
                        (*e).patch_b,
                        (*target).code.unwrap_unchecked() as *const () as *mut c_void,
                    );
                    pred_add(target, (*e).src, (*e).edge as c_int);
                }
                *pp = (*e).next;
                libc::free(e.cast());
            } else {
                pp = core::ptr::addr_of_mut!((*e).next);
            }
        }
    }
}

unsafe fn fault_recipe_native_mov(insn: *const X86Insn) -> c_int {
    unsafe {
        if g_defer == 0
            || mem_guard_needed() != 0
            || (*insn).op as c_uint != OCERZ_OP_MOV
            || (*insn).nops != 2
            || (*insn).seg as c_uint != OCERZ_SEG_NONE
            || (*insn).addrsize == 4
        {
            return 0;
        }
        let d = core::ptr::addr_of!((*insn).ops).cast::<X86Operand>();
        let s = d.add(1);
        if (*d).kind as c_uint == OCERZ_OPK_MEM && (*s).kind as c_uint == OCERZ_OPK_REG {
            return ((*s).high8 == 0
                && ((*s).size == 4 || (*s).size == 8)
                && mem_native_store_ok() != 0) as c_int;
        }
        if (*d).kind as c_uint == OCERZ_OPK_REG && (*s).kind as c_uint == OCERZ_OPK_MEM {
            return ((*d).high8 == 0 && ((*d).size == 4 || (*d).size == 8)) as c_int;
        }
        0
    }
}

unsafe fn fault_recipe_add_shape(insn: *const X86Insn) -> c_int {
    unsafe {
        if (*insn).op as c_uint != OCERZ_OP_ADD || (*insn).lock != 0 || (*insn).nops != 2 {
            return 0;
        }
        let d = core::ptr::addr_of!((*insn).ops).cast::<X86Operand>();
        let s = d.add(1);
        if (*d).kind as c_uint != OCERZ_OPK_REG
            || (*d).high8 != 0
            || ((*d).size != 4 && (*d).size != 8)
            || (*s).size != (*d).size
        {
            return 0;
        }
        if (*s).kind as c_uint == OCERZ_OPK_IMM {
            return 1;
        }
        ((*s).kind as c_uint == OCERZ_OPK_REG && (*s).high8 == 0 && (*s).reg != (*d).reg) as c_int
    }
}

unsafe fn fault_recipe_logic_shape(insn: *const X86Insn) -> c_int {
    unsafe {
        if ((*insn).op as c_uint != OCERZ_OP_AND
            && (*insn).op as c_uint != OCERZ_OP_OR
            && (*insn).op as c_uint != OCERZ_OP_XOR)
            || (*insn).lock != 0
            || (*insn).nops != 2
        {
            return 0;
        }
        let d = core::ptr::addr_of!((*insn).ops).cast::<X86Operand>();
        ((*d).kind as c_uint == OCERZ_OPK_REG
            && (*d).high8 == 0
            && ((*d).size == 4 || (*d).size == 8)) as c_int
    }
}

unsafe fn fault_recipe_matching_inc(add: *const X86Insn, inc: *const X86Insn) -> c_int {
    unsafe {
        let d = core::ptr::addr_of!((*add).ops).cast::<X86Operand>();
        let inc_d = core::ptr::addr_of!((*inc).ops).cast::<X86Operand>();
        ((*inc).op as c_uint == OCERZ_OP_INC
            && (*inc).lock == 0
            && (*inc).nops == 1
            && (*inc_d).kind as c_uint == OCERZ_OPK_REG
            && (*inc_d).high8 == 0
            && (*inc_d).reg == (*d).reg
            && (*inc_d).size == (*d).size) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn build_fault_flag_recipes(
    insns: *const X86Insn,
    n: c_int,
    recipes: *mut JitFaultFlagRecipe,
) -> c_int {
    unsafe {
        libc::memset(
            recipes.cast(),
            0,
            (n as usize).wrapping_mul(core::mem::size_of::<JitFaultFlagRecipe>()),
        );
        if g_no_fault_recipes != 0 {
            return 0;
        }
        let mut found = 0;
        for i in 0..n {
            let insn = insns.offset(i as isize);
            if fault_recipe_native_mov(insn) == 0 {
                continue;
            }
            let mut r = JitFaultFlagRecipe {
                kind: JFF_NONE as u8,
                producer: 0,
            };
            if i >= 2
                && fault_recipe_add_shape(insns.offset((i - 2) as isize)) != 0
                && fault_recipe_matching_inc(
                    insns.offset((i - 2) as isize),
                    insns.offset((i - 1) as isize),
                ) != 0
            {
                r.kind = JFF_ADD_INC_RESULT_SRC as u8;
                r.producer = (i - 2) as u8;
            } else if i >= 1 && fault_recipe_logic_shape(insns.offset((i - 1) as isize)) != 0 {
                r.kind = JFF_LOGIC_RESULT as u8;
                r.producer = (i - 1) as u8;
            } else if i >= 1 && fault_recipe_add_shape(insns.offset((i - 1) as isize)) != 0 {
                r.kind = JFF_ADD_RESULT_SRC as u8;
                r.producer = (i - 1) as u8;
            }
            if r.kind as c_uint != JFF_NONE {
                *recipes.offset(i as isize) = r;
                found += 1;
            }
        }
        found
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn code_index_append_locked(
    jit: *mut OcerzJit,
    block: *mut JitBlock,
) -> c_int {
    unsafe {
        let ci_ptr = core::ptr::addr_of_mut!((*jit).ci);
        let index = AtomicPtr::<JitCodeIndex>::from_ptr(ci_ptr).load(Ordering::Relaxed);
        let count = if !index.is_null() {
            AtomicUsize::from_ptr(core::ptr::addr_of_mut!((*index).count)).load(Ordering::Relaxed)
        } else {
            0
        };
        if index.is_null() || count == (*index).capacity {
            let capacity = if !index.is_null() {
                (*index).capacity.wrapping_mul(2)
            } else {
                4096
            };
            if (!index.is_null() && capacity < (*index).capacity)
                || capacity
                    > (usize::MAX - core::mem::size_of::<JitCodeIndex>())
                        / core::mem::size_of::<*mut JitBlock>()
            {
                return 0;
            }
            let next = libc::malloc(
                core::mem::size_of::<JitCodeIndex>()
                    .wrapping_add(capacity.wrapping_mul(core::mem::size_of::<*mut JitBlock>())),
            )
            .cast::<JitCodeIndex>();
            if next.is_null() {
                return 0;
            }
            (*next).older = index;
            (*next).capacity = capacity;
            (*next).count = 0;
            let next_blocks = (next as *mut u8)
                .add(core::mem::size_of::<JitCodeIndex>())
                .cast::<*mut JitBlock>();
            if count != 0 {
                let old_blocks = (index as *const u8)
                    .add(core::mem::size_of::<JitCodeIndex>())
                    .cast::<*mut JitBlock>();
                core::ptr::copy_nonoverlapping(old_blocks, next_blocks, count);
            }
            *next_blocks.add(count) = block;
            AtomicUsize::from_ptr(core::ptr::addr_of_mut!((*next).count))
                .store(count.wrapping_add(1), Ordering::Release);
            AtomicPtr::from_ptr(ci_ptr).store(next, Ordering::Release);
            return 1;
        }
        let blocks = (index as *mut u8)
            .add(core::mem::size_of::<JitCodeIndex>())
            .cast::<*mut JitBlock>();
        *blocks.add(count) = block;
        AtomicUsize::from_ptr(core::ptr::addr_of_mut!((*index).count))
            .store(count.wrapping_add(1), Ordering::Release);
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn compact_block(blk: *mut JitBlock) {
    unsafe {
        let n = (*blk).n_insns;
        if (*blk).code.is_none()
            || (*blk).insns.is_null()
            || g_no_compact != 0
            || g_cur_blk != blk
            || g_keep_n != n
            || env_on!("OCERZ_NO_COMPACT") != 0
        {
            return;
        }
        if !(*blk).fault_flags.is_null() {
            for i in 0..n {
                let r = (*blk).fault_flags.offset(i as isize);
                if (*r).kind as c_uint == JFF_NONE {
                    continue;
                }
                if ((*r).producer as c_int) < n {
                    *g_keep.add((*r).producer as usize) = 1;
                }
                if (*r).kind as c_uint == JFF_ADD_INC_RESULT_SRC && ((*r).producer as c_int + 1) < n
                {
                    *g_keep.add((*r).producer as usize + 1) = 1;
                }
            }
        }
        let mut nk = 0;
        for i in 0..n {
            nk += (*g_keep.add(i as usize) != 0) as c_int;
        }
        if nk > 65535 {
            return;
        }
        let refs = libc::malloc((n as usize).wrapping_mul(core::mem::size_of::<JitInsnRef>()))
            .cast::<JitInsnRef>();
        let kept = if nk != 0 {
            libc::malloc((nk as usize).wrapping_mul(core::mem::size_of::<X86Insn>()))
                .cast::<X86Insn>()
        } else {
            core::ptr::null_mut()
        };
        if refs.is_null() || (nk != 0 && kept.is_null()) {
            libc::free(refs.cast());
            libc::free(kept.cast());
            return;
        }
        let mut k = 0;
        for i in 0..n {
            let input = (*blk).insns.offset(i as isize);
            let output = refs.add(i as usize);
            (*output).rip = (*input).rip;
            (*output).op = (*input).op as u16;
            (*output).len = (*input).len as u8;
            (*output).flags = 0;
            (*output).pad = 0;
            (*output).keep = 0;
            if *g_keep.add(i as usize) != 0 {
                *kept.add(k as usize) = *input;
                (*output).keep = (k + 1) as u16;
                k += 1;
            }
        }
        (*blk).iref = refs;
        (*blk).kept = kept;
        (*blk).n_kept = nk as u16;
        libc::free((*blk).insns.cast());
        (*blk).insns = core::ptr::null_mut();
        g_pe_insns = core::ptr::null_mut();
        g_cur_insns = core::ptr::null_mut();
        g_cur_insns_n = 0;
        g_cur_blk = core::ptr::null_mut();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn blk_chain_install(jit: *mut OcerzJit, blk: *mut JitBlock) {
    unsafe {
        if g_no_chain != 0 {
            return;
        }
        chain_batch_begin();
        for i in 0..(*blk).n_edges as usize {
            let edge = (*blk).edges.add(i);
            let cs = if (*edge).probing != 0 {
                core::ptr::null_mut()
            } else {
                (*edge).cond_site
            };
            let t = cache_lookup(jit, (*edge).target_rip, blk_mode32(blk));
            if !t.is_null() && (*t).code.is_some() {
                let mut dst = (*t).code.unwrap_unchecked() as *const () as *mut c_void;
                if (*edge).kind as c_uint == EDGE_BODY {
                    let compatible = if (*edge).pin_class != 0 {
                        (*t).pin_class == (*edge).pin_class
                    } else {
                        (*t).pin_class == 0 && (*t).n_pinned == 0
                    };
                    if !compatible || (*t).body_code.is_null() {
                        dst = core::ptr::null_mut();
                    } else {
                        dst = body_entry_for(t, (*blk).hoist_sig);
                    }
                }
                if !dst.is_null() {
                    chain_activate((*edge).patch_b, dst);
                    chain_cond_short(cs, dst);
                    pred_add(t, blk, i as c_int);
                }
            } else {
                pending_add(
                    jit_key((*edge).target_rip, blk_mode32(blk)),
                    (*edge).patch_b,
                    (*edge).kind as u8,
                    (*edge).pin_class,
                    cs,
                    (*blk).hoist_sig,
                    blk,
                    i as c_int,
                );
            }
        }
        pending_drain((*blk).key, blk);
        chain_batch_end();
    }
}

#[inline]
unsafe fn block_code_ptr(b: *const JitBlock) -> *const u32 {
    unsafe {
        match (*b).code {
            Some(code) => code as *const () as *const u32,
            None => core::ptr::null(),
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_pc_in_arena(
    vm: *const OcerzVM,
    host_pc: *const c_void,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        if jit.is_null() {
            return 0;
        }
        let pc = host_pc as usize;
        (pc >= (*jit).code_base as usize && pc < (*jit).code_end as usize) as c_int
    }
}

unsafe fn fault_block(jit: *const OcerzJit, pc: *const u32) -> *const JitBlock {
    unsafe {
        if jit.is_null()
            || (pc as usize) < (*jit).code_base as usize
            || (pc as usize) >= (*jit).code_end as usize
        {
            return core::ptr::null();
        }
        let index = AtomicPtr::<JitCodeIndex>::from_ptr(core::ptr::addr_of!((*jit).ci).cast_mut())
            .load(Ordering::Acquire);
        if index.is_null() {
            return core::ptr::null();
        }
        let n =
            AtomicUsize::from_ptr(core::ptr::addr_of_mut!((*index).count)).load(Ordering::Acquire);
        if n == 0 {
            return core::ptr::null();
        }
        let blocks = (index as *const u8)
            .add(core::mem::size_of::<JitCodeIndex>())
            .cast::<*const JitBlock>();
        let mut lo = 0usize;
        let mut hi = n;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let block = *blocks.add(mid);
            if (block_code_ptr(block) as usize) <= pc as usize {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo == 0 {
            return core::ptr::null();
        }
        let b = *blocks.add(lo - 1);
        let base = block_code_ptr(b);
        if base.is_null() || (pc as usize) >= base.add((*b).code_words as usize) as usize {
            return core::ptr::null();
        }
        b
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_guest_gprs_at(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    host_x: *const u64,
    cpu: *const OcerzCPU,
    out: *mut u64,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let b = fault_block(jit, host_pc.cast());
        if b.is_null() || host_x.is_null() {
            return 0;
        }
        let in_callout = *host_x.add(1) == cpu as u64;
        for i in 0..(*b).n_pinned {
            let hr = pin_hreg(i as c_int);
            if in_callout && (hr == 1 || hr == 2) {
                continue;
            }
            let mut value = *host_x.add(hr as usize);
            let guest_reg = *core::ptr::addr_of!((*b).host_holds)
                .cast::<u8>()
                .add(i as usize);
            if ((*b).pin_class == 2
                || ((*b).pin_class == 3
                    && (*b).n_insns > 0
                    && blk_mode32(b) == 0
                    && rsp_ptr3() != 0))
                && guest_reg as c_int == OCERZ_RSP as c_int
            {
                value = value.wrapping_sub(ocerz_guest_base);
            }
            *out.add(guest_reg as usize) = value;
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_recover_regs(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    host_x: *const u64,
    cpu: *mut OcerzCPU,
) {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let b = fault_block(jit, host_pc.cast());
        if b.is_null() || host_x.is_null() || cpu.is_null() {
            return;
        }
        for i in 0..(*b).n_pinned {
            let hr = pin_hreg(i as c_int);
            let mut value = *host_x.add(hr as usize);
            let guest_reg = *core::ptr::addr_of!((*b).host_holds)
                .cast::<u8>()
                .add(i as usize);
            if ((*b).pin_class == 2
                || ((*b).pin_class == 3
                    && (*b).n_insns > 0
                    && blk_mode32(b) == 0
                    && rsp_ptr3() != 0))
                && guest_reg as c_int == OCERZ_RSP as c_int
            {
                value = value.wrapping_sub(ocerz_guest_base);
            }
            *core::ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(guest_reg as usize) = value;
        }
        let code = block_code_ptr(b);
        if (*b).n_push_fix != 0 && !code.is_null() {
            let off = ((host_pc as usize).wrapping_sub(code as usize) / 4) as u32;
            for i in 0..(*b).n_push_fix {
                if *(*b).push_fix.add(i as usize) == off {
                    let rsp = core::ptr::addr_of_mut!((*cpu).gpr)
                        .cast::<u64>()
                        .add(OCERZ_RSP as usize);
                    *rsp = (*rsp).wrapping_add(8);
                    break;
                }
            }
        }
        if (*b).n_pushelide != 0
            && !(*b).pushelide.is_null()
            && (!(*b).insns.is_null() || !(*b).iref.is_null())
        {
            let k = fault_insn_index(b, host_pc.cast());
            for i in 0..(*b).n_pushelide {
                if k <= 0 {
                    break;
                }
                let p = (*b).pushelide.add(i as usize);
                if k <= (*p).ci as c_int || k >= (*p).rj as c_int {
                    continue;
                }
                let mut delta = 0i64;
                let mut m = (*p).ci as c_int + 1;
                while m < k {
                    let op2 = blk_insn_op(b, m);
                    if op2 == OCERZ_OP_PUSH || op2 == OCERZ_OP_CALL {
                        delta = delta.wrapping_sub(8);
                    } else if op2 == OCERZ_OP_POP || op2 == OCERZ_OP_RET {
                        delta = delta.wrapping_add(8);
                    }
                    m += 1;
                }
                let rsp = *core::ptr::addr_of!((*cpu).gpr)
                    .cast::<u64>()
                    .add(OCERZ_RSP as usize);
                let slot = rsp.wrapping_add(delta.wrapping_neg() as u64);
                *((slot.wrapping_add(ocerz_guest_base)) as *mut u64) = (*p).ra;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_recover_xmm(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    host_v: *const c_void,
    cpu: *mut OcerzCPU,
) {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let b = fault_block(jit, host_pc.cast());
        if b.is_null() || host_v.is_null() || cpu.is_null() || (*b).xmm_pinned == 0 {
            return;
        }
        let v = host_v.cast::<u8>();
        for r in 0..16usize {
            if ((*b).xmm_pinned >> r) & 1 != 0 {
                libc::memcpy(
                    core::ptr::addr_of_mut!((*cpu).xmm)
                        .cast::<u8>()
                        .add(r * 16)
                        .cast(),
                    v.add((16 + r) * 16).cast(),
                    16,
                );
            }
        }
        let code = block_code_ptr(b);
        let off = ((host_pc as usize).wrapping_sub(code as usize) / 4) as u32;
        let mut rec = core::ptr::null::<JitLaneRec>();
        for k in 0..(*b).n_lanerec {
            let candidate = (*b).lanerec.add(k as usize);
            if (*candidate).off > off {
                break;
            }
            rec = candidate;
        }
        if rec.is_null() {
            return;
        }
        for r in 0..16usize {
            let l0 = *core::ptr::addr_of!((*rec).l0).cast::<u8>().add(r);
            let yc = *core::ptr::addr_of!((*rec).yc).cast::<u8>().add(r);
            if l0 != 0xff && ((*rec).dirty >> r) & 1 != 0 {
                libc::memcpy(
                    core::ptr::addr_of_mut!((*cpu).xmm)
                        .cast::<u8>()
                        .add(r * 16)
                        .cast(),
                    v.add((4 + (l0 & 15) as usize) * 16).cast(),
                    if (l0 & 0x10) != 0 { 8 } else { 4 },
                );
            }
            if yc != 0xff {
                libc::memcpy(
                    core::ptr::addr_of_mut!((*cpu).ymmh)
                        .cast::<u8>()
                        .add(r * 16)
                        .cast(),
                    v.add((4 + yc as usize) * 16).cast(),
                    16,
                );
            }
        }
    }
}

unsafe fn fault_insn_index(b: *const JitBlock, pc: *const u32) -> c_int {
    unsafe {
        if b.is_null() || (*b).insn_off.is_null() {
            return -1;
        }
        let base = block_code_ptr(b);
        let off = ((pc as usize).wrapping_sub(base as usize) / 4) as u32;
        for k in 0..(*b).n_oslow {
            let oslow = (*b).oslow.add(k as usize);
            if off >= (*oslow).lo && off < (*oslow).hi {
                return (*oslow).idx;
            }
        }
        let mut lo = 0;
        let mut hi = (*b).n_insns;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if *(*b).insn_off.add(mid as usize) <= off {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo - 1
    }
}

unsafe fn fault_recipe_rhs(
    cpu: *const OcerzCPU,
    op: *const X86Operand,
    size: c_int,
    out: *mut u64,
) -> c_int {
    unsafe {
        if (*op).size as c_int != size {
            return 0;
        }
        if (*op).kind as c_uint == OCERZ_OPK_REG && (*op).high8 == 0 {
            *out = ocerz_trunc(
                *core::ptr::addr_of!((*cpu).gpr)
                    .cast::<u64>()
                    .add((*op).reg as usize),
                size,
            );
            return 1;
        }
        if (*op).kind as c_uint == OCERZ_OPK_IMM {
            *out = ocerz_trunc((*op).imm, size);
            return 1;
        }
        0
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ChurnEnt {
    page: u64,
    hits: u32,
    last_ns: u64,
    refused: u64,
}

static mut g_churn: [ChurnEnt; CHURN_SLOTS as usize] = [ChurnEnt {
    page: 0,
    hits: 0,
    last_ns: 0,
    refused: 0,
}; CHURN_SLOTS as usize];

#[thread_local]
static mut g_inv_caller: u64 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct InvSrcEnt {
    caller: u64,
    page: u64,
    bumps: u64,
    retires: u64,
}

static mut g_invsrc: [InvSrcEnt; 256] = [InvSrcEnt {
    caller: 0,
    page: 0,
    bumps: 0,
    retires: 0,
}; 256];

unsafe fn invsrc_note(page: u64, is_retire: c_int) {
    static mut LG: c_int = -1;
    unsafe {
        if LG < 0 {
            LG = if !libc::getenv(b"OCERZ_INVSRC\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if LG <= 0 {
            return;
        }
        let c = g_inv_caller;
        let mut i = (c.wrapping_mul(0x9E3779B97F4A7C15) >> 56) as usize;
        let invsrc = core::ptr::addr_of_mut!(g_invsrc).cast::<InvSrcEnt>();
        for _ in 0..256 {
            let ent = invsrc.add(i);
            if (*ent).caller == c || (*ent).caller == 0 {
                (*ent).caller = c;
                if is_retire != 0 {
                    (*ent).retires = (*ent).retires.wrapping_add(1);
                } else {
                    (*ent).bumps = (*ent).bumps.wrapping_add(1);
                    (*ent).page = page << 16;
                }
                break;
            }
            i = i.wrapping_add(1) & 255;
        }
        static mut TOT: u64 = 0;
        if is_retire != 0 {
            return;
        }
        TOT = TOT.wrapping_add(1);
        if TOT & 0x3ff != 0 {
            return;
        }
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: INVSRC[%d] bumps=%llu |\0".as_ptr().cast(),
            libc::getpid(),
            TOT,
        );
        let mut used = [0u8; 256];
        let used_ptr = used.as_mut_ptr();
        for _ in 0..6 {
            let mut best = -1isize;
            let mut bv = 0u64;
            for k in 0..256 {
                let ent = invsrc.add(k);
                if *used_ptr.add(k) == 0 && (*ent).bumps > bv {
                    bv = (*ent).bumps;
                    best = k as isize;
                }
            }
            if best < 0 {
                break;
            }
            *used_ptr.add(best as usize) = 1;
            let ent = invsrc.add(best as usize);
            libc::fprintf(
                crate::log::stderr(),
                b" rel=%lld:b=%llu,r=%llu,pg=%#llx\0".as_ptr().cast(),
                ((*ent).caller as i64).wrapping_sub(ocerz_jit_step as *const () as u64 as i64),
                (*ent).bumps,
                (*ent).retires,
                (*ent).page,
            );
        }
        libc::fprintf(crate::log::stderr(), b"\n\0".as_ptr().cast());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn churn_note_refusal(_rip: u64) {
    static mut BLOG: c_int = -1;
    static mut TOT: u64 = 0;
    unsafe {
        if BLOG < 0 {
            BLOG = if !libc::getenv(b"OCERZ_BLACKLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if BLOG <= 0 {
            return;
        }
        TOT = TOT.wrapping_add(1);
        if TOT & 0xffff != 0 {
            return;
        }
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: BLACKLOG[%d] refusals=%lluk blacklisted_pages=\0"
                .as_ptr()
                .cast(),
            libc::getpid(),
            TOT >> 10,
        );
        let mut np: c_uint = 0;
        let churn = core::ptr::addr_of_mut!(g_churn).cast::<ChurnEnt>();
        for k in 0..CHURN_SLOTS as usize {
            let ent = churn.add(k);
            if (*ent).page != 0 && (*ent).hits >= CHURN_LIMIT {
                np = np.wrapping_add(1);
            }
        }
        libc::fprintf(crate::log::stderr(), b"%u top:\0".as_ptr().cast(), np);
        for _ in 0..5 {
            let mut best = CHURN_SLOTS as usize;
            let mut bv = 0u64;
            for k in 0..CHURN_SLOTS as usize {
                let ent = churn.add(k);
                if (*ent).refused > bv {
                    bv = (*ent).refused;
                    best = k;
                }
            }
            if best == CHURN_SLOTS as usize {
                break;
            }
            let ent = churn.add(best);
            libc::fprintf(
                crate::log::stderr(),
                b" %#llx=%lluk\0".as_ptr().cast(),
                (*ent).page << 16,
                bv >> 10,
            );
            (*ent).refused = 0;
        }
        libc::fprintf(crate::log::stderr(), b"\n\0".as_ptr().cast());
    }
}

unsafe fn churn_bump(rip: u64) {
    unsafe {
        if g_churn_suppress != 0 {
            return;
        }
        invsrc_note(rip >> 16, 0);
        let page = rip >> 16;
        let mut i =
            (page.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as usize & (CHURN_SLOTS as usize - 1);
        let churn = core::ptr::addr_of_mut!(g_churn).cast::<ChurnEnt>();
        for _ in 0..8 {
            let ent = churn.add(i);
            if (*ent).page == page {
                (*ent).hits = (*ent).hits.wrapping_add(1);
                (*ent).last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if (*ent).hits == CHURN_LIMIT
                    && !libc::getenv(b"OCERZ_CHURNLOG\0".as_ptr().cast()).is_null()
                {
                    libc::fprintf(
                        crate::log::stderr(),
                        b"ocerz: CHURN[%d] blacklist page=%#llx\n\0".as_ptr().cast(),
                        libc::getpid(),
                        page << 16,
                    );
                }
                return;
            }
            if (*ent).page == 0 {
                (*ent).page = page;
                (*ent).hits = 1;
                (*ent).last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                return;
            }
            i = i.wrapping_add(1) & (CHURN_SLOTS as usize - 1);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn churn_blacklisted(rip: u64) -> c_int {
    unsafe {
        let page = rip >> 16;
        let mut i =
            (page.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as usize & (CHURN_SLOTS as usize - 1);
        let churn = core::ptr::addr_of_mut!(g_churn).cast::<ChurnEnt>();
        for _ in 0..8 {
            let ent = churn.add(i);
            if (*ent).page == page {
                if (*ent).hits < CHURN_LIMIT {
                    return 0;
                }
                let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if now.wrapping_sub((*ent).last_ns) > CHURN_QUIET_NS as u64 {
                    (*ent).hits = CHURN_LIMIT - 1;
                    (*ent).last_ns = now;
                    (*ent).refused = 0;
                    if !libc::getenv(b"OCERZ_CHURNLOG\0".as_ptr().cast()).is_null() {
                        libc::fprintf(
                            crate::log::stderr(),
                            b"ocerz: CHURN[%d] reprieve page=%#llx\n\0".as_ptr().cast(),
                            libc::getpid(),
                            page << 16,
                        );
                    }
                    return 0;
                }
                (*ent).refused = (*ent).refused.wrapping_add(1);
                if (*ent).refused > CHURN_REFUSE_MAX as u64 {
                    (*ent).hits = CHURN_LIMIT - 1;
                    (*ent).last_ns = now;
                    (*ent).refused = 0;
                    if !libc::getenv(b"OCERZ_CHURNLOG\0".as_ptr().cast()).is_null() {
                        libc::fprintf(
                            crate::log::stderr(),
                            b"ocerz: CHURN[%d] cost-reprieve page=%#llx\n\0"
                                .as_ptr()
                                .cast(),
                            libc::getpid(),
                            page << 16,
                        );
                    }
                    return 0;
                }
                return 1;
            }
            if (*ent).page == 0 {
                return 0;
            }
            i = i.wrapping_add(1) & (CHURN_SLOTS as usize - 1);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn branch_word_target(site: *mut u32, w: u32) -> *mut u32 {
    let off: i64;
    if w & 0x7c000000 == 0x14000000 {
        off = ((w << 6) as i32 as i64 >> 6) * 4;
        return ((site as usize).wrapping_add(off as usize)) as *mut u32;
    }
    if w & 0xff000010 == 0x54000000 || w & 0x7e000000 == 0x34000000 {
        off = (((w >> 5) & 0x7ffff) << 13) as i32 as i64 >> 13;
        return ((site as usize).wrapping_add(off.wrapping_mul(4) as usize)) as *mut u32;
    }
    core::ptr::null_mut()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_pair(host_pc: *const c_void) -> c_int {
    unsafe {
        let w = *host_pc.cast::<u32>();
        ((w & 0xffc00000) == 0xad400000 || (w & 0xffc00000) == 0xad000000) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_rip(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    out_rip: *mut u64,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let pc = host_pc.cast::<u32>();
        let b = fault_block(jit, pc);
        if b.is_null() || (*b).insn_off.is_null() {
            return 0;
        }
        let i = fault_insn_index(b, pc);
        if i < 0 {
            return 0;
        }
        *out_rip = blk_insn_rip(b, i);
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_info(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    out: *mut OcerzJitFaultInfo,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let pc = host_pc.cast::<u32>();
        let b = fault_block(jit, pc);
        if b.is_null() || out.is_null() {
            return 0;
        }
        libc::memset(out.cast(), 0, core::mem::size_of::<OcerzJitFaultInfo>());
        (*out).block_rip = blk_rip(b);
        (*out).host_word = (pc as usize)
            .wrapping_sub(block_code_ptr(b) as usize)
            .wrapping_div(4) as u32;
        (*out).insn_index = fault_insn_index(b, pc);
        if (*out).insn_index >= 0 {
            (*out).insn_rip = blk_insn_rip(b, (*out).insn_index);
        }
        (*out).n_pinned = (*b).n_pinned;
        (*out).pin_class = (*b).pin_class;
        libc::memcpy(
            core::ptr::addr_of_mut!((*out).host_holds).cast(),
            core::ptr::addr_of!((*b).host_holds).cast(),
            core::mem::size_of::<[u8; 8]>(),
        );
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_code_range(
    vm: *mut OcerzVM,
    lo: *mut *const u32,
    hi: *mut *const u32,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null_mut()
        } else {
            (*vm).jit
        };
        if jit.is_null() {
            return 0;
        }
        *lo = (*jit).code_base;
        *hi = (*jit).code_cur;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_owner_pid(vm: *mut OcerzVM) -> c_int {
    unsafe {
        if !vm.is_null() && !(*vm).jit.is_null() {
            (*(*vm).jit).owner_pid
        } else {
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_forget(vm: *mut OcerzVM) {
    unsafe {
        pthread_mutex_init(core::ptr::addr_of_mut!(jit_lock), core::ptr::null());
        g_xlat_jit = core::ptr::null_mut();
        g_n_ras_cells = 0;
        g_ras_slot_n = 0;
        libc::memset(
            core::ptr::addr_of_mut!(g_pending).cast(),
            0,
            core::mem::size_of::<[*mut PendingChain; PEND_SIZE as usize]>(),
        );
        if !vm.is_null() {
            (*vm).jit = core::ptr::null_mut();
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_create(vm: *mut OcerzVM) -> *mut OcerzJit {
    unsafe {
        let jit = libc::calloc(1, core::mem::size_of::<OcerzJit>()).cast::<OcerzJit>();
        if jit.is_null() {
            return core::ptr::null_mut();
        }
        (*jit).vm = vm;
        (*jit).plain_mem = ((*vm).jit_ordered_required == 0
            && ((*vm).jit_plain_mem != 0
                || !libc::getenv(b"OCERZ_PLAIN_MEM\0".as_ptr().cast()).is_null()))
            as c_int;
        let bytes = jit_code_bytes();
        #[cfg(target_os = "macos")]
        const MAP_JIT_LOCAL: c_int = 0x800;
        #[cfg(not(target_os = "macos"))]
        const MAP_JIT_LOCAL: c_int = 0;
        let p = libc::mmap(
            core::ptr::null_mut(),
            bytes,
            libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
            libc::MAP_PRIVATE | libc::MAP_ANON | MAP_JIT_LOCAL,
            -1,
            0,
        );
        if p == libc::MAP_FAILED {
            crate::ocerz_log!("JIT unavailable (MAP_JIT failed); using interpreter\n");
            libc::free(jit.cast());
            return core::ptr::null_mut();
        }
        (*jit).owner_pid = libc::getpid();
        (*jit).code_base = p.cast();
        (*jit).code_cur = p.cast();
        let leaf_hi = core::ptr::addr_of!(ocerz_leaf_hi) as usize;
        let leaf_lo = core::ptr::addr_of!(ocerz_leaf_lo) as usize;
        let leaf_bytes = leaf_hi.wrapping_sub(leaf_lo);
        if leaf_bytes != 0 && leaf_bytes.wrapping_add(64) < bytes {
            pthread_jit_write_protect_np(0);
            libc::memcpy(p, leaf_lo as *const c_void, leaf_bytes);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(p, leaf_bytes);
            (*jit).leaf_near = p.cast();
            (*jit).code_cur = (p as *mut u8)
                .add(leaf_bytes.wrapping_add(63) & !63usize)
                .cast();
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_leaf_near_hi)).store(
                (p as usize).wrapping_add(leaf_bytes) as u64,
                Ordering::Release,
            );
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_leaf_near_lo))
                .store(p as usize as u64, Ordering::Release);
        }
        (*jit).code_start = (*jit).code_cur;
        (*jit).code_end = (p as *mut u8).add(bytes).cast();
        (*jit).code_bytes = bytes;
        crate::ocerz_log!(
            "JIT code arena %zu MB reserved at [%p,%p)\n",
            bytes >> 20,
            p,
            (p as *mut u8).add(bytes)
        );
        jit
    }
}

unsafe fn block_destroy(b: *mut JitBlock) {
    unsafe {
        libc::free((*b).insn_off.cast());
        libc::free((*b).oslow.cast());
        libc::free((*b).lanerec.cast());
        libc::free((*b).fault_flags.cast());
        libc::free((*b).push_fix.cast());
        libc::free((*b).pushelide.cast());
        libc::free((*b).insns.cast());
        libc::free((*b).iref.cast());
        libc::free((*b).kept.cast());
        libc::free((*b).edges.cast());
        libc::free((*b).prof.cast());
        libc::free((*b).preds.cast());
        libc::free(b.cast());
    }
}

unsafe fn block_list_destroy(mut b: *mut JitBlock) {
    unsafe {
        while !b.is_null() {
            let next = (*b).hnext;
            block_destroy(b);
            b = next;
        }
    }
}

unsafe fn retired_list_destroy(mut b: *mut JitBlock) {
    unsafe {
        while !b.is_null() {
            let next = (*b).retired_next;
            block_destroy(b);
            b = next;
        }
    }
}

unsafe fn code_index_destroy(mut index: *mut JitCodeIndex) {
    unsafe {
        while !index.is_null() {
            let older = (*index).older;
            libc::free(index.cast());
            index = older;
        }
    }
}

unsafe fn pending_clear() {
    unsafe {
        for i in 0..PEND_SIZE as usize {
            let mut e = *core::ptr::addr_of!(g_pending)
                .cast::<*mut PendingChain>()
                .add(i);
            while !e.is_null() {
                let next = (*e).next;
                libc::free(e.cast());
                e = next;
            }
            *core::ptr::addr_of_mut!(g_pending)
                .cast::<*mut PendingChain>()
                .add(i) = core::ptr::null_mut();
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_destroy(jit: *mut OcerzJit) {
    unsafe {
        if jit.is_null() {
            return;
        }
        if ocerz_perfstat > 0 {
            ps_report(jit);
        }
        for i in 0..JIT_HASH_SIZE as usize {
            block_list_destroy(
                *core::ptr::addr_of!((*jit).buckets)
                    .cast::<*mut JitBlock>()
                    .add(i),
            );
        }
        retired_list_destroy((*jit).retired);
        pending_clear();
        let index = AtomicPtr::<JitCodeIndex>::from_ptr(core::ptr::addr_of_mut!((*jit).ci))
            .load(Ordering::Relaxed);
        code_index_destroy(index);
        libc::munmap((*jit).code_base.cast(), (*jit).code_bytes);
        libc::free(jit.cast());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_blocks(jit: *const OcerzJit) -> u64 {
    unsafe {
        if jit.is_null() {
            0
        } else {
            (*jit).blocks_translated
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_prof_stats(
    vm: *const OcerzVM,
    translated: *mut u64,
    live: *mut u64,
    retires: *mut u64,
    flips: *mut u64,
) {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        *translated = if jit.is_null() {
            0
        } else {
            (*jit).blocks_translated
        };
        *live = if jit.is_null() {
            0
        } else {
            (*jit).n_live as u64
        };
        *retires = AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_jit_retire_count))
            .load(Ordering::Relaxed);
        *flips = g_flip_n_retire;
    }
}

#[unsafe(no_mangle)]
pub static mut ocerz_jit_retire_count: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_leaf_near_hi: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_leaf_near_lo: u64 = 0;

static mut g_retire_sweep: c_uint = 0;

unsafe fn invalidate_all_locked(jit: *mut OcerzJit) {
    unsafe {
        let mut patched = 0;
        AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_jit_retire_count))
            .fetch_add(1, Ordering::Release);
        pthread_jit_write_protect_np(0);
        patched |= force_stop_sites_writable(jit);
        for i in 0..g_n_ras_cells {
            AtomicPtr::<c_void>::from_ptr(*g_ras_cells.add(i as usize))
                .store(core::ptr::null_mut(), Ordering::Release);
        }
        for k in 0..(*jit).n_live {
            let b = *(*jit).live.add(k);
            for i in 0..(*b).n_edges {
                let edge = (*b).edges.add(i as usize);
                let at = (*edge).patch_b;
                let fallback = (*edge).fallback_insn;
                if !at.is_null() && fallback != 0 && *at != fallback {
                    AtomicU32::from_ptr(at).store(fallback, Ordering::Release);
                    patched = 1;
                }
                let cs = (*edge).cond_site;
                if !cs.is_null() && (*edge).cond_orig != 0 && *cs != (*edge).cond_orig {
                    AtomicU32::from_ptr(cs).store((*edge).cond_orig, Ordering::Release);
                    patched = 1;
                }
            }
        }
        pthread_jit_write_protect_np(1);
        if patched != 0 {
            sys_icache_invalidate(
                (*jit).code_base.cast(),
                ((*jit).code_cur as usize).wrapping_sub((*jit).code_base as usize),
            );
        }
        for k in 0..(*jit).n_live {
            let b = *(*jit).live.add(k);
            AtomicPtr::<JitBlock>::from_ptr(
                (*jit).buckets.as_mut_ptr().add(hash_key((*b).key) as usize),
            )
            .store(core::ptr::null_mut(), Ordering::Release);
        }
        for k in 0..(*jit).n_live {
            let b = *(*jit).live.add(k);
            (*b).retired_next = (*jit).retired;
            (*jit).retired = b;
        }
        (*jit).n_live = 0;
        (*jit).code_lo = 0;
        (*jit).code_hi = 0;
        libc::memset(
            core::ptr::addr_of_mut!((*jit).invmap).cast(),
            0,
            core::mem::size_of_val(&(*jit).invmap),
        );
        (*jit).invmap_full = 0;
        gran_clear_all();
        pending_clear();
        for i in 0..g_ras_slot_n {
            AtomicPtr::<c_void>::from_ptr(core::ptr::addr_of_mut!(*g_ras_slots.add(i as usize)))
                .store(core::ptr::null_mut(), Ordering::Release);
        }
        psc_clear_all();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_invalidate_all(vm: *mut OcerzVM) {
    unsafe {
        if vm.is_null() {
            return;
        }
        let jit = (*vm).jit;
        if !jit.is_null() {
            jl_acquire(line!() as c_int);
            invalidate_all_locked(jit);
            jl_release();
        }
        ocerz_vm_purge_jit_ras(vm);
    }
}

unsafe fn ranges_overlap(a: u64, alen: u64, b: u64, blen: u64) -> c_int {
    if alen == 0 || blen == 0 {
        0
    } else if a <= b {
        (b.wrapping_sub(a) < alen) as c_int
    } else {
        (a.wrapping_sub(b) < blen) as c_int
    }
}

unsafe fn invmap_check_reject(jit: *const OcerzJit, addr: u64, len: u64) {
    unsafe {
        for k in 0..(*jit).n_live {
            let b = *(*jit).live.add(k);
            for i in 0..(*b).n_insns {
                if ranges_overlap(addr, len, blk_insn_rip(b, i), blk_insn_len(b, i) as u64) == 0 {
                    continue;
                }
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: INVMAP MISS[%d] addr=%#llx len=%#llx block=%#llx\n\0"
                        .as_ptr()
                        .cast(),
                    libc::getpid(),
                    addr,
                    len,
                    blk_rip(b),
                );
                libc::abort();
            }
        }
    }
}

unsafe fn stopcheck(b: *const JitBlock, site: *const u32, insn: u32, what: *const c_char) {
    unsafe {
        if env_on!("OCERZ_STOPCHECK") == 0 || site.is_null() {
            return;
        }
        let off = if insn & 0xfc000000 == 0x14000000 {
            ((insn << 6) as i32 as i64 >> 6) * 4
        } else if insn & 0xff000010 == 0x54000000 || insn & 0x7e000000 == 0x34000000 {
            ((insn << 8) as i32 as i64 >> 13) * 4
        } else if insn & 0x7e000000 == 0x36000000 {
            ((insn << 13) as i32 as i64 >> 18) * 4
        } else {
            0
        };
        let tgt = (site as usize).wrapping_add(off as usize) as *const u32;
        let lo = block_code_ptr(b);
        let hi = if lo.is_null() {
            core::ptr::null()
        } else {
            lo.add((*b).code_words as usize)
        };
        if lo.is_null() || (tgt as usize) < lo as usize || (tgt as usize) >= hi as usize {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: STOPCHECK[%d] %s rip=%#llx site=%p insn=%08x tgt=%p block=[%p,%p)\n\0"
                    .as_ptr()
                    .cast(),
                libc::getpid(),
                what,
                blk_rip(b),
                site,
                insn,
                tgt,
                lo,
                hi,
            );
        }
    }
}

unsafe fn force_stop_sites_writable(jit: *mut OcerzJit) -> c_int {
    unsafe {
        let mut patched = 0;
        let mut b = (*jit).stop_blocks;
        while !b.is_null() {
            stopcheck(
                b,
                (*b).stop_patch,
                (*b).stop_insn,
                b"stop\0".as_ptr().cast(),
            );
            for i in 0..(*b).n_stop_extra {
                let extra = (*b).stop_extra.as_mut_ptr().add(i as usize);
                stopcheck(b, (*extra).site, (*extra).insn, b"extra\0".as_ptr().cast());
            }
            if env_on!("OCERZ_STOPLOG") != 0 {
                let current = if (*b).stop_patch.is_null() {
                    0
                } else {
                    *(*b).stop_patch
                };
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: STOPSITE rip=%#llx patch=%p cur=%08x stop_insn=%08x\n\0"
                        .as_ptr()
                        .cast(),
                    blk_rip(b),
                    (*b).stop_patch,
                    current,
                    (*b).stop_insn,
                );
            }
            if !(*b).stop_patch.is_null()
                && (*b).stop_insn != 0
                && *(*b).stop_patch != (*b).stop_insn
            {
                AtomicU32::from_ptr((*b).stop_patch).store((*b).stop_insn, Ordering::Release);
                patched = 1;
            }
            for i in 0..(*b).n_stop_extra {
                let extra = (*b).stop_extra.as_mut_ptr().add(i as usize);
                if *(*extra).site != (*extra).insn {
                    AtomicU32::from_ptr((*extra).site).store((*extra).insn, Ordering::Release);
                    patched = 1;
                }
            }
            for i in 0..(*b).n_edges {
                let edge = (*b).edges.add(i as usize);
                let cs = (*edge).cond_site;
                let mut is_stop = (*edge).patch_b == (*b).stop_patch;
                for k in 0..(*b).n_stop_extra {
                    if is_stop {
                        break;
                    }
                    is_stop =
                        (*edge).patch_b == (*(*b).stop_extra.as_mut_ptr().add(k as usize)).site;
                }
                if !cs.is_null() && (*edge).cond_orig != 0 && *cs != (*edge).cond_orig && is_stop {
                    AtomicU32::from_ptr(cs).store((*edge).cond_orig, Ordering::Release);
                    patched = 1;
                }
            }
            b = (*b).stop_next;
        }
        patched
    }
}

#[repr(C)]
struct StopUndo {
    site: *mut u32,
    was: u32,
    now: u32,
}

unsafe fn stop_sites_sync(u: *const StopUndo, n: usize) {
    unsafe {
        for i in 0..n {
            core::arch::asm!(
                "dc cvau, {site}",
                site = in(reg) (*u.add(i)).site,
                options(preserves_flags)
            );
        }
        core::arch::asm!("dsb ish", options(preserves_flags));
        for i in 0..n {
            core::arch::asm!(
                "ic ivau, {site}",
                site = in(reg) (*u.add(i)).site,
                options(preserves_flags)
            );
        }
        core::arch::asm!("dsb ish", "isb", options(preserves_flags));
    }
}

unsafe fn jit_space_low(jit: *const OcerzJit) -> c_int {
    unsafe {
        let total = ((*jit).code_end as usize).wrapping_sub((*jit).code_start as usize);
        let margin = core::cmp::min(total / 8, 8usize << 20);
        (((*jit).code_end as usize).wrapping_sub((*jit).code_cur as usize) < margin) as c_int
    }
}

unsafe fn stop_set(
    u: *mut *mut StopUndo,
    n: *mut usize,
    cap: *mut usize,
    site: *mut u32,
    insn: u32,
) {
    unsafe {
        if site.is_null() || insn == 0 || *site == insn {
            return;
        }
        if *n == *cap {
            *cap = if *cap != 0 {
                (*cap).wrapping_mul(2)
            } else {
                256
            };
            let nu = libc::realloc(
                (*u).cast(),
                (*cap).wrapping_mul(core::mem::size_of::<StopUndo>()),
            )
            .cast::<StopUndo>();
            if nu.is_null() {
                libc::abort();
            }
            *u = nu;
        }
        *(*u).add(*n) = StopUndo {
            site,
            was: *site,
            now: insn,
        };
        *n = (*n).wrapping_add(1);
        AtomicU32::from_ptr(site).store(insn, Ordering::Release);
    }
}

unsafe fn stop_sites_force_undoable(jit: *mut OcerzJit, n_out: *mut usize) -> *mut StopUndo {
    unsafe {
        let mut n = 0usize;
        let mut cap = 0usize;
        let mut u = core::ptr::null_mut::<StopUndo>();
        pthread_jit_write_protect_np(0);
        let mut b = (*jit).stop_blocks;
        while !b.is_null() {
            stop_set(&mut u, &mut n, &mut cap, (*b).stop_patch, (*b).stop_insn);
            for i in 0..(*b).n_stop_extra {
                let extra = (*b).stop_extra.as_mut_ptr().add(i as usize);
                stop_set(&mut u, &mut n, &mut cap, (*extra).site, (*extra).insn);
            }
            for i in 0..(*b).n_edges {
                let edge = (*b).edges.add(i as usize);
                let mut is_stop = (*edge).patch_b == (*b).stop_patch;
                for k in 0..(*b).n_stop_extra {
                    if is_stop {
                        break;
                    }
                    is_stop =
                        (*edge).patch_b == (*(*b).stop_extra.as_mut_ptr().add(k as usize)).site;
                }
                if is_stop {
                    stop_set(
                        &mut u,
                        &mut n,
                        &mut cap,
                        (*edge).cond_site,
                        (*edge).cond_orig,
                    );
                }
            }
            b = (*b).stop_next;
        }
        pthread_jit_write_protect_np(1);
        stop_sites_sync(u, n);
        *n_out = n;
        u
    }
}

unsafe fn stop_sites_undo(jit: *mut OcerzJit, u: *const StopUndo, n: usize) {
    unsafe {
        if n == 0 {
            return;
        }
        pthread_jit_write_protect_np(0);
        let mut i = n;
        while i != 0 {
            i = i.wrapping_sub(1);
            if *(*u.add(i)).site == (*u.add(i)).now {
                AtomicU32::from_ptr((*u.add(i)).site).store((*u.add(i)).was, Ordering::Release);
            }
        }
        pthread_jit_write_protect_np(1);
        stop_sites_sync(u, n);
        let _ = jit;
    }
}

#[repr(C)]
struct HitSet {
    hits: *mut *mut JitBlock,
    n: usize,
}

unsafe fn ptr_in_block_code(b: *const JitBlock, p: *const u32) -> c_int {
    unsafe {
        let lo = block_code_ptr(b);
        (!lo.is_null()
            && (p as usize) >= lo as usize
            && (p as usize) < lo.add((*b).code_words as usize) as usize) as c_int
    }
}

unsafe fn ptr_in_hits(hits: *const *mut JitBlock, n_hits: usize, p: *const u32) -> c_int {
    unsafe {
        if p.is_null() {
            return 0;
        }
        let mut lo = 0usize;
        let mut hi = n_hits;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if (block_code_ptr(*hits.add(mid)) as usize) <= p as usize {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        (lo > 0 && ptr_in_block_code(*hits.add(lo - 1), p) != 0) as c_int
    }
}

unsafe extern "C" fn hit_code_cmp(pa: *const c_void, pb: *const c_void) -> c_int {
    unsafe {
        let a = *pa.cast::<*mut JitBlock>();
        let b = *pb.cast::<*mut JitBlock>();
        if (block_code_ptr(a) as usize) < block_code_ptr(b) as usize {
            -1
        } else {
            ((block_code_ptr(a) as usize) > block_code_ptr(b) as usize) as c_int
        }
    }
}

unsafe fn retire_unlink_hits(jit: *mut OcerzJit, hits: *const *mut JitBlock, n_hits: usize) {
    unsafe {
        for m in 0..n_hits {
            let b = *hits.add(m);
            let mut idx = (*b).live_idx;
            if idx >= (*jit).n_live || *(*jit).live.add(idx) != b {
                idx = 0;
                while idx < (*jit).n_live && *(*jit).live.add(idx) != b {
                    idx = idx.wrapping_add(1);
                }
                if idx == (*jit).n_live {
                    continue;
                }
            }
            *(*jit).live.add(idx) = *(*jit).live.add((*jit).n_live.wrapping_sub(1));
            (*(*(*jit).live.add(idx))).live_idx = idx;
            (*jit).n_live = (*jit).n_live.wrapping_sub(1);
            let h = hash_key((*b).key);
            let mut pp = core::ptr::addr_of_mut!((*jit).buckets)
                .cast::<*mut JitBlock>()
                .add(h as usize);
            while !(*pp).is_null() && *pp != b {
                pp = core::ptr::addr_of_mut!((**pp).hnext);
            }
            if *pp == b {
                AtomicPtr::<JitBlock>::from_ptr(pp).store((*b).hnext, Ordering::Release);
            }
            gran_block(b, -1);
            tc_noload_add((*b).key);
            (*b).retired_next = (*jit).retired;
            (*jit).retired = b;
        }
    }
}

unsafe extern "C" fn ras_entry_in_hits(entry: *const c_void, arg: *mut c_void) -> c_int {
    unsafe {
        let set = arg.cast::<HitSet>();
        let p = ((entry as usize) & !3usize) as *const u32;
        let hits = (*set).hits;
        let n = (*set).n;
        let mut lo = 0usize;
        let mut hi = n;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if (block_code_ptr(*hits.add(mid)) as usize) <= p as usize {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo == 0 {
            return 0;
        }
        ptr_in_block_code(*hits.add(lo - 1), p)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn psc_retire_cols(vm: *mut OcerzVM, cols: u32) {
    unsafe {
        for k in 0..PSC_N as usize {
            if cols >> k & 1 == 0 {
                continue;
            }
            let generation = AtomicU64::from_ptr((*vm).psc_gen.as_mut_ptr().add(k))
                .load(Ordering::Relaxed)
                .wrapping_add(1u64 << PSC_GEN_SHIFT);
            if generation == 0 {
                for i in 0..g_n_psc_tables {
                    let table = *g_psc_tables.add(i);
                    AtomicU64::from_ptr(core::ptr::addr_of_mut!((*table.add(k)).rip))
                        .store(PSC_EMPTY_RIP as u64, Ordering::Release);
                    AtomicPtr::<c_void>::from_ptr(core::ptr::addr_of_mut!((*table.add(k)).body))
                        .store(core::ptr::null_mut(), Ordering::Release);
                }
            }
            AtomicU64::from_ptr((*vm).psc_gen.as_mut_ptr().add(k))
                .store(generation, Ordering::Release);
        }
    }
}

unsafe fn unchain_edge(sblk: *mut JitBlock, i: c_int, hits: *const *mut JitBlock, n_hits: usize) {
    unsafe {
        let edge = (*sblk).edges.add(i as usize);
        let at = (*edge).patch_b;
        let fallback = (*edge).fallback_insn;
        if !at.is_null()
            && fallback != 0
            && *at != fallback
            && ptr_in_hits(hits, n_hits, branch_word_target(at, *at)) != 0
        {
            AtomicU32::from_ptr(at).store(fallback, Ordering::Release);
            sys_icache_invalidate(at.cast(), 4);
        }
        let cs = (*edge).cond_site;
        if !cs.is_null()
            && (*edge).cond_orig != 0
            && *cs != (*edge).cond_orig
            && ptr_in_hits(hits, n_hits, branch_word_target(cs, *cs)) != 0
        {
            AtomicU32::from_ptr(cs).store((*edge).cond_orig, Ordering::Release);
            sys_icache_invalidate(cs.cast(), 4);
        }
    }
}

unsafe fn retire_hit_blocks_locked(
    vm: *mut OcerzVM,
    jit: *mut OcerzJit,
    hits: *mut *mut JitBlock,
    n_hits: usize,
) {
    unsafe {
        AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_jit_retire_count))
            .fetch_add(1, Ordering::Release);
        invsrc_note(0, 1);
        let mut any_code = 0;
        for m in 0..n_hits {
            let b = *hits.add(m);
            if (*b).code.is_some() {
                any_code = 1;
                churn_bump(blk_insn_rip(b, 0));
            }
        }
        if any_code == 0 {
            retire_unlink_hits(jit, hits, n_hits);
            return;
        }
        libc::qsort(
            hits.cast(),
            n_hits,
            core::mem::size_of::<*mut JitBlock>(),
            Some(hit_code_cmp),
        );
        static mut FULL_SCAN: c_int = -1;
        if FULL_SCAN < 0 {
            FULL_SCAN = if !libc::getenv(b"OCERZ_RETIRE_SCAN\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        let set = HitSet { hits, n: n_hits };
        pthread_jit_write_protect_np(0);
        for i in 0..g_n_ras_cells {
            let cell = *g_ras_cells.add(i as usize);
            let e = AtomicPtr::<c_void>::from_ptr(cell).load(Ordering::Relaxed);
            if !e.is_null() && ras_entry_in_hits(e, (&set as *const HitSet).cast_mut().cast()) != 0
            {
                AtomicPtr::<c_void>::from_ptr(cell).store(core::ptr::null_mut(), Ordering::Release);
            }
        }
        for m in 0..n_hits {
            let b = *hits.add(m);
            if !(*b).stop_patch.is_null()
                && (*b).stop_insn != 0
                && *(*b).stop_patch != (*b).stop_insn
            {
                AtomicU32::from_ptr((*b).stop_patch).store((*b).stop_insn, Ordering::Release);
                sys_icache_invalidate((*b).stop_patch.cast(), 4);
            }
            for i in 0..(*b).n_stop_extra {
                let extra = (*b).stop_extra.as_mut_ptr().add(i as usize);
                if *(*extra).site != (*extra).insn {
                    AtomicU32::from_ptr((*extra).site).store((*extra).insn, Ordering::Release);
                    sys_icache_invalidate((*extra).site.cast(), 4);
                }
            }
            for i in 0..(*b).n_edges {
                let edge = (*b).edges.add(i as usize);
                let cs = (*edge).cond_site;
                let mut is_stop = (*edge).patch_b == (*b).stop_patch;
                for q in 0..(*b).n_stop_extra {
                    if is_stop {
                        break;
                    }
                    is_stop =
                        (*edge).patch_b == (*(*b).stop_extra.as_mut_ptr().add(q as usize)).site;
                }
                if !cs.is_null() && (*edge).cond_orig != 0 && *cs != (*edge).cond_orig && is_stop {
                    AtomicU32::from_ptr(cs).store((*edge).cond_orig, Ordering::Release);
                    sys_icache_invalidate(cs.cast(), 4);
                }
            }
        }
        if FULL_SCAN != 0 {
            for k in 0..(*jit).n_live {
                let sblk = *(*jit).live.add(k);
                if (*sblk).inv_hit != 0 {
                    continue;
                }
                for i in 0..(*sblk).n_edges {
                    unchain_edge(sblk, i as c_int, hits, n_hits);
                }
            }
        } else {
            for m in 0..n_hits {
                let t = *hits.add(m);
                for q in 0..(*t).n_preds {
                    let pred = (*t).preds.add(q as usize);
                    let sblk = (*pred).pb;
                    let i = (*pred).e;
                    if (*sblk).inv_hit != 0 || i >= (*sblk).n_edges {
                        continue;
                    }
                    unchain_edge(sblk, i as c_int, hits, n_hits);
                }
                (*t).n_preds = 0;
            }
        }
        pthread_jit_write_protect_np(1);
        retire_unlink_hits(jit, hits, n_hits);
        for i in 0..g_ras_slot_n {
            let slot = core::ptr::addr_of_mut!(*g_ras_slots.add(i as usize));
            let e = AtomicPtr::<c_void>::from_ptr(slot).load(Ordering::Relaxed);
            if !e.is_null() && ras_entry_in_hits(e, (&set as *const HitSet).cast_mut().cast()) != 0
            {
                AtomicPtr::<c_void>::from_ptr(slot).store(core::ptr::null_mut(), Ordering::Release);
            }
        }
        g_retire_sweep = g_retire_sweep.wrapping_add(1);
        let mut cols = 0u32;
        for m in 0..n_hits {
            let b = *hits.add(m);
            if (*b).code.is_some() {
                cols |= 1u32 << psc_col((*b).key);
            }
        }
        psc_retire_cols(vm, cols);
    }
}

#[inline(never)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_invalidate_range(vm: *mut OcerzVM, mut addr: u64, mut len: u64) {
    unsafe {
        g_inv_caller = core::intrinsics::return_address() as usize as u64;
        if vm.is_null() || len == 0 || (*vm).jit.is_null() {
            return;
        }
        let jit = (*vm).jit;
        let mut invalidated = 0;
        let t0 = if ocerz_jit_time_xlat != 0 {
            clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
        } else {
            0
        };
        jl_acquire(line!() as c_int);
        if (*jit).code_hi == 0
            || ranges_overlap(
                addr,
                len,
                (*jit).code_lo,
                (*jit).code_hi.wrapping_sub((*jit).code_lo),
            ) == 0
        {
            if env_on!("OCERZ_INVMAP_CHECK") != 0 {
                invmap_check_reject(jit, addr, len);
            }
            jl_release();
            return;
        }
        if addr < (*jit).code_lo {
            len = len.wrapping_sub((*jit).code_lo.wrapping_sub(addr));
            addr = (*jit).code_lo;
        }
        if addr.wrapping_add(len) > (*jit).code_hi {
            len = (*jit).code_hi.wrapping_sub(addr);
        }
        let mut xlo = addr;
        let mut xlen = len;
        let g = 1u64 << INVMAP_GSHIFT;
        let alo = addr & !(g - 1);
        let ahi = addr.wrapping_add(len).wrapping_add(g - 1) & !(g - 1);
        addr = alo;
        len = ahi.wrapping_sub(alo);
        static mut WHOLE: c_int = -1;
        if WHOLE < 0 {
            WHOLE = if !libc::getenv(b"OCERZ_INV_GRANULE\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if WHOLE != 0 {
            xlo = addr;
            xlen = len;
        }
        let mut hits = core::ptr::null_mut::<*mut JitBlock>();
        let mut n_hit = 0usize;
        {
            static mut IVLOG: c_int = -1;
            static IVN: AtomicU64 = AtomicU64::new(0);
            if IVLOG < 0 {
                IVLOG = if !libc::getenv(b"OCERZ_INVLOG\0".as_ptr().cast()).is_null() {
                    1
                } else {
                    0
                };
            }
            if IVLOG != 0 {
                let n = IVN.fetch_add(1, Ordering::SeqCst).wrapping_add(1);
                if n & 0xfff == 0 {
                    libc::fprintf(
                        crate::log::stderr(),
                        b"ocerz: INVQ[%d] n=%llu addr=%#llx len=%#llx nlive=%zu\n\0"
                            .as_ptr()
                            .cast(),
                        libc::getpid(),
                        n,
                        addr,
                        len,
                        (*jit).n_live,
                    );
                }
            }
        }
        if invmap_may_hold(jit, addr, addr.wrapping_add(len)) == 0
            || gran_any(addr, addr.wrapping_add(len)) == 0
        {
            if env_on!("OCERZ_INVMAP_CHECK") != 0 {
                invmap_check_reject(jit, addr, len);
            }
            jl_release();
            return;
        }
        {
            static mut NOPREC: c_int = -1;
            if NOPREC < 0 {
                NOPREC = if !libc::getenv(b"OCERZ_INV_ALL\0".as_ptr().cast()).is_null() {
                    1
                } else {
                    0
                };
            }
            let mut cap = 0usize;
            let g0 = addr >> INVMAP_GSHIFT;
            let g1 = addr.wrapping_add(len) >> INVMAP_GSHIFT;
            let listed = g_gblk_off == 0 && g1.wrapping_sub(g0) <= 256;
            let n_cand = if listed { 0 } else { (*jit).n_live };
            let mut gp = g0;
            let mut gj = 0u32;
            let mut k = 0usize;
            loop {
                let b;
                if listed {
                    let mut gs = -1;
                    while gp < g1 {
                        gs = gblk_slot(gp, 0);
                        if gs >= 0
                            && gj
                                < (*core::ptr::addr_of_mut!(g_gblk)
                                    .cast::<GblkEnt>()
                                    .add(gs as usize))
                                .n
                        {
                            break;
                        }
                        gp = gp.wrapping_add(1);
                        gj = 0;
                    }
                    if gp >= g1 {
                        break;
                    }
                    let slot = *core::ptr::addr_of!(g_gblk)
                        .cast::<GblkEnt>()
                        .add(gs as usize);
                    b = *slot.v.add(gj as usize);
                    gj = gj.wrapping_add(1);
                    if (*b).inv_hit != 0 {
                        continue;
                    }
                } else {
                    if k >= n_cand {
                        break;
                    }
                    b = *(*jit).live.add(k);
                    (*b).inv_hit = 0;
                }
                let blo = blk_insn_rip(b, 0);
                let last = (*b).n_insns - 1;
                let bhi = blk_insn_rip(b, last).wrapping_add(blk_insn_len(b, last) as u64);
                if ranges_overlap(xlo, xlen, blo, bhi.wrapping_sub(blo)) == 0 {
                    k = k.wrapping_add(1);
                    continue;
                }
                for i in 0..(*b).n_insns {
                    if ranges_overlap(xlo, xlen, blk_insn_rip(b, i), blk_insn_len(b, i) as u64) != 0
                    {
                        (*b).inv_hit = 1;
                        if n_hit == cap {
                            cap = if cap != 0 { cap.wrapping_mul(2) } else { 64 };
                            let nh = libc::realloc(
                                hits.cast(),
                                cap.wrapping_mul(core::mem::size_of::<*mut JitBlock>()),
                            )
                            .cast::<*mut JitBlock>();
                            if nh.is_null() {
                                libc::free(hits.cast());
                                hits = core::ptr::null_mut();
                                n_hit = usize::MAX;
                                break;
                            }
                            hits = nh;
                        }
                        *hits.add(n_hit) = b;
                        n_hit = n_hit.wrapping_add(1);
                        break;
                    }
                }
                if n_hit == usize::MAX {
                    break;
                }
                k = k.wrapping_add(1);
            }
            if n_hit == usize::MAX {
                invalidated = 2;
                invalidate_all_locked(jit);
            } else if n_hit > 0 {
                invalidated = 1;
                if NOPREC != 0 {
                    invalidated = 2;
                    invalidate_all_locked(jit);
                } else {
                    retire_hit_blocks_locked(vm, jit, hits, n_hit);
                }
            }
            if n_hit != usize::MAX {
                let mut g = addr;
                let gsz = 1u64 << INVMAP_GSHIFT;
                while g < addr.wrapping_add(len) {
                    if gran_any(g, g.wrapping_add(gsz)) == 0 {
                        invmap_clear_range(jit, g, g.wrapping_add(gsz));
                    }
                    g = g.wrapping_add(gsz);
                }
            }
        }
        jl_release();
        if invalidated == 1 && g_retire_sweep & 255 != 0 {
            let set = HitSet { hits, n: n_hit };
            ocerz_vm_purge_jit_ras_if(
                vm,
                Some(ras_entry_in_hits),
                (&set as *const HitSet).cast_mut().cast(),
            );
        } else if invalidated != 0 {
            ocerz_vm_purge_jit_ras(vm);
        }
        if n_hit != usize::MAX {
            libc::free(hits.cast());
        }
        if invalidated != 0 && t0 != 0 {
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_jit_retire_ns)).fetch_add(
                clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0),
                Ordering::Relaxed,
            );
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_hotpatch_align(
    vm: *mut OcerzVM,
    host_pc: *const c_void,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null_mut()
        } else {
            (*vm).jit
        };
        let site = host_pc.cast_mut().cast::<u32>();
        if jit.is_null() || ocerz_jit_pc_in_arena(vm, host_pc) == 0 {
            return 0;
        }
        let w = *site;
        if w & 0xfc00_0000 == 0x1400_0000 {
            return 2;
        }
        let opc = (w >> 22) & 3;
        let is_acc = w & 0x3f20_0c00 == 0x1900_0000;
        let is_lds = is_acc && opc >= 2;
        let lds_sf = (opc == 2) as c_int;
        let is_ld = is_acc && opc != 0;
        let is_st = is_acc && opc == 0;
        if !is_ld && !is_st {
            return 0;
        }
        let size = 1 << (w >> 30);
        if size < 2 || (is_lds && size == 8) {
            return 0;
        }
        let mut imm9 = ((w >> 12) & 0x1ff) as i32;
        if imm9 & 0x100 != 0 {
            imm9 -= 0x200;
        }
        let rn = ((w >> 5) & 31) as c_int;
        let rt = (w & 31) as c_int;
        if rn == 31 || (rt == 31 && !is_st) {
            return 0;
        }
        let mut pair = false;
        if is_st && size == 8 && rt == JT0 as c_int && imm9 <= 247 {
            let w2 = *site.add(1);
            let want =
                (w & !(0x1ff << 12) & !0x1f) | (((imm9 + 8) as u32 & 0x1ff) << 12) | JTU as u32;
            pair = w2 == want;
        }
        let cand = [JTF as c_int, JTT as c_int, JTU as c_int];
        let mut sc = [0; 2];
        let sc_ptr = sc.as_mut_ptr();
        let mut n = 0usize;
        for c in cand {
            if n < 2 && c != rt && c != rn && !(pair && c == JTU as c_int) {
                *sc_ptr.add(n) = c;
                n += 1;
            }
        }
        let ta = *sc_ptr;
        let s1 = *sc_ptr.add(1);
        let blk = fault_block(jit, site);
        let use_dmb = !blk.is_null() && (*blk).ordered_loads != 0;
        jl_acquire(line!() as c_int);
        let mut rc = 0;
        if ((*jit).code_end as usize).wrapping_sub((*jit).code_cur as usize) > 128 {
            pthread_jit_write_protect_np(0);
            veneer_pool_check(jit);
            let mut pool = -1;
            let mut start = (*jit).code_cur;
            let mut lim = (*jit).code_end;
            let reach = (start as isize).wrapping_sub(site as isize);
            if reach > VENEER_REACH as isize || reach < -(VENEER_REACH as isize) {
                let mut best_d = VENEER_REACH as isize;
                let veneer_pool = core::ptr::addr_of_mut!((*jit).veneer_pool).cast::<*mut u32>();
                let veneer_used = core::ptr::addr_of_mut!((*jit).veneer_used).cast::<u32>();
                for i in 0..(*jit).veneer_n {
                    let used = *veneer_used.add(i as usize);
                    if used
                        .wrapping_mul(VENEER_BYTES as u32)
                        .wrapping_add(ALIGN_ARM_BYTES as u32)
                        > VENEER_POOL_BYTES as u32
                    {
                        continue;
                    }
                    let d = ((*veneer_pool.add(i as usize)) as isize)
                        .wrapping_sub(site as isize)
                        .abs();
                    if d < best_d {
                        best_d = d;
                        pool = i as c_int;
                    }
                }
                if pool >= 0 {
                    let used = *veneer_used.add(pool as usize);
                    start = (*veneer_pool.add(pool as usize) as *mut u8)
                        .add(used.wrapping_mul(VENEER_BYTES as u32) as usize)
                        .cast();
                    lim = start.add((ALIGN_ARM_BYTES / 4) as usize);
                }
            }
            let mut b = A64Buf {
                start,
                p: start,
                end: lim,
                overflow: 0,
                sink: 0,
            };
            let arm = b.p;
            a64_stp_pre(&mut b, ta, s1, 31, -16);
            if imm9 > 0 {
                a64_add_imm(&mut b, 1, ta, rn, imm9 as u32);
            } else if imm9 < 0 {
                a64_sub_imm(&mut b, 1, ta, rn, imm9.wrapping_neg() as u32);
            } else {
                a64_mov_reg(&mut b, 1, ta, rn);
            }
            let bne;
            if pair {
                a64_try_and_imm(&mut b, 1, s1, ta, 7);
                bne = a64_label(&mut b);
                a64_cbnz(&mut b, 1, s1, 0);
            } else {
                a64_add_imm(&mut b, 1, s1, ta, (size - 1) as u32);
                a64_eor_reg(&mut b, 1, s1, s1, ta, 0);
                bne = a64_label(&mut b);
                a64_tbnz(&mut b, s1, 4, 0);
            }
            if is_lds {
                a64_ldapurs(&mut b, size as c_int, lds_sf, rt, ta, 0);
            } else if is_ld {
                a64_ldapur(&mut b, size as c_int, rt, ta, 0);
            } else {
                a64_stlur(&mut b, size as c_int, rt, ta, 0);
                if pair {
                    a64_stlur(&mut b, 8, JTU as c_int, ta, 8);
                }
            }
            a64_ldp_post(&mut b, ta, s1, 31, 16);
            let back1 = a64_label(&mut b);
            a64_b(&mut b, 0);
            if pair {
                a64_patch_cbz(bne, a64_label(&mut b));
            } else {
                a64_patch_tbz(bne, a64_label(&mut b));
            }
            if is_ld {
                if !is_lds {
                    a64_ldr(&mut b, size as c_int, rt, ta, 0);
                } else if size == 2 {
                    a64_ldrsh(&mut b, lds_sf, rt, ta, 0);
                } else {
                    a64_ldrsw(&mut b, rt, ta, 0);
                }
                a64_dmb_ishld(&mut b);
            } else if use_dmb {
                a64_dmb_ish(&mut b);
                a64_str(&mut b, size as c_int, rt, ta, 0);
                if pair {
                    a64_str(&mut b, 8, JTU as c_int, ta, 8);
                }
            } else if size == 8 {
                let tob0 = a64_label(&mut b);
                a64_tbnz(&mut b, ta, 0, 0);
                let tob1 = a64_label(&mut b);
                a64_tbnz(&mut b, ta, 1, 0);
                emit_misaligned_pieces_st(&mut b, 4, 2, rt, ta, 0, s1);
                if pair {
                    emit_misaligned_pieces_st(&mut b, 4, 2, JTU as c_int, ta, 8, s1);
                }
                let tod = a64_label(&mut b);
                a64_b(&mut b, 0);
                a64_patch_tbz(tob0, a64_label(&mut b));
                a64_patch_tbz(tob1, a64_label(&mut b));
                emit_misaligned_pieces_st(&mut b, 1, 8, rt, ta, 0, s1);
                if pair {
                    emit_misaligned_pieces_st(&mut b, 1, 8, JTU as c_int, ta, 8, s1);
                }
                a64_patch_b(tod, a64_label(&mut b));
            } else {
                emit_misaligned_pieces_st(&mut b, 1, size as c_int, rt, ta, 0, s1);
            }
            a64_ldp_post(&mut b, ta, s1, 31, 16);
            let back2 = a64_label(&mut b);
            a64_b(&mut b, 0);
            let back = site.add(if pair { 2 } else { 1 });
            let mut ok = b.overflow == 0
                && a64_try_patch_b(back1, back) != 0
                && a64_try_patch_b(back2, back) != 0;
            if ok {
                let saved = *site;
                *site = 0x1400_0000;
                if a64_try_patch_b(site, arm) != 0 {
                    if pool >= 0 {
                        let used = (b.p as usize)
                            .wrapping_sub(arm as usize)
                            .wrapping_add((VENEER_BYTES - 1) as usize)
                            / VENEER_BYTES as usize;
                        let veneer_used = core::ptr::addr_of_mut!((*jit).veneer_used).cast::<u32>();
                        let entry = veneer_used.add(pool as usize);
                        *entry = (*entry).wrapping_add(used as u32);
                    } else {
                        (*jit).code_cur = b.p;
                    }
                    sys_icache_invalidate(arm.cast(), (b.p as usize).wrapping_sub(arm as usize));
                    sys_icache_invalidate(site.cast(), 4);
                    rc = 1;
                } else {
                    *site = saved;
                }
            }
            pthread_jit_write_protect_np(1);
        }
        jl_release();
        if rc == 1 && ocerz_perfstat > 0 {
            ps_align_patches = ps_align_patches.wrapping_add(1);
        }
        if rc == 1 && env_on!("OCERZ_ALPATCHLOG") != 0 {
            let imm26 = ((*site).wrapping_shl(6) as i32) >> 6;
            let arm = (site as *mut u8)
                .offset((imm26 as isize).wrapping_mul(4))
                .cast::<u32>();
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: ALPATCH site=%p w=%08x size=%d rt=%d rn=%d imm=%d pair=%d dmb=%d ta=%d s1=%d arm=%p arm:\0"
                    .as_ptr()
                    .cast(),
                site,
                w,
                size,
                rt,
                rn,
                imm9,
                pair as c_int,
                use_dmb as c_int,
                ta,
                s1,
                arm,
            );
            for i in 0..40isize {
                let cur = arm.offset(i);
                if cur as usize >= (*jit).code_cur as usize {
                    break;
                }
                let instr = *cur;
                libc::fprintf(crate::log::stderr(), b" %08x\0".as_ptr().cast(), instr);
                if instr & 0xfc00_0000 == 0x1400_0000 {
                    let off = (instr.wrapping_shl(6) as i32 >> 6) as isize;
                    libc::fprintf(
                        crate::log::stderr(),
                        b"(->%p)\0".as_ptr().cast(),
                        cur.offset(off),
                    );
                }
            }
            libc::fprintf(crate::log::stderr(), b"\n\0".as_ptr().cast());
        }
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_request_stop(vm: *mut OcerzVM) {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null_mut()
        } else {
            (*vm).jit
        };
        if jit.is_null() {
            return;
        }
        jl_acquire(line!() as c_int);
        (*jit).stop_requested = 1;
        pthread_jit_write_protect_np(0);
        let patched = force_stop_sites_writable(jit);
        pthread_jit_write_protect_np(1);
        if patched != 0 {
            sys_icache_invalidate(
                (*jit).code_base.cast(),
                ((*jit).code_cur as usize).wrapping_sub((*jit).code_base as usize),
            );
        }
        jl_release();
    }
}

#[inline(never)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_require_ordered(vm: *mut OcerzVM) {
    unsafe {
        if vm.is_null() {
            return;
        }
        if env_on!("OCERZ_ORDERLOG") != 0 && (*vm).jit_ordered_required == 0 {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: ORDERED memory required from here (caller %p)\n\0"
                    .as_ptr()
                    .cast(),
                core::intrinsics::return_address(),
            );
            let mut bt = [core::ptr::null_mut::<c_void>(); 8];
            let n = backtrace(bt.as_mut_ptr(), 8);
            backtrace_symbols_fd(bt.as_ptr(), n, 2);
        }
        (*vm).jit_ordered_required = 1;
        (*vm).jit_plain_mem = 0;
        let jit = (*vm).jit;
        if jit.is_null() {
            ocerz_vm_purge_jit_ras(vm);
            return;
        }
        jl_acquire(line!() as c_int);
        if (*jit).plain_mem != 0 {
            (*jit).plain_mem = 0;
            g_plain_mem = 0;
            invalidate_all_locked(jit);
        }
        jl_release();
        ocerz_vm_purge_jit_ras(vm);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_prefork() {
    unsafe {
        jl_acquire(line!() as c_int);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_postfork() {
    unsafe {
        jl_release();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_postfork_child() {
    unsafe {
        let self_thr = t_jit_thr;
        let mut t = AtomicPtr::<JitThr>::from_ptr(core::ptr::addr_of_mut!(g_jit_thr))
            .load(Ordering::Acquire);
        while !t.is_null() {
            if t != self_thr {
                AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames))
                    .store(0, Ordering::Relaxed);
                AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked))
                    .store(0, Ordering::Relaxed);
                AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).dead)).store(1, Ordering::Relaxed);
            }
            t = (*t).next;
        }
        AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).store(0, Ordering::SeqCst);
        g_flush_mark = u64::MAX;
        g_flush_retry_ns = 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn chain_edge_now(jit: *mut OcerzJit, blk: *mut JitBlock, e: c_int) {
    unsafe {
        let edge = (*blk).edges.add(e as usize);
        let t = cache_lookup(jit, (*edge).target_rip, blk_mode32(blk));
        if !t.is_null() && (*t).code.is_some() {
            let mut dst = block_code_ptr(t).cast_mut().cast::<c_void>();
            if (*edge).kind as c_uint == EDGE_BODY {
                let compatible = if (*edge).pin_class != 0 {
                    (*t).pin_class == (*edge).pin_class
                } else {
                    (*t).pin_class == 0 && (*t).n_pinned == 0
                };
                dst = if compatible && !(*t).body_code.is_null() {
                    body_entry_for(t, (*blk).hoist_sig)
                } else {
                    core::ptr::null_mut()
                };
            }
            if !dst.is_null() {
                chain_activate((*edge).patch_b, dst);
                if (*edge).kind as c_uint == EDGE_BODY {
                    chain_cond_short((*edge).cond_site, dst);
                }
                pred_add(t, blk, e);
                return;
            }
        }
        pending_add(
            jit_key((*edge).target_rip, blk_mode32(blk)),
            (*edge).patch_b,
            (*edge).kind as u8,
            (*edge).pin_class,
            (*edge).cond_site,
            (*blk).hoist_sig,
            blk,
            e,
        );
    }
}

unsafe fn code_index_reset_locked(jit: *mut OcerzJit) {
    unsafe {
        let old = AtomicPtr::<JitCodeIndex>::from_ptr(core::ptr::addr_of_mut!((*jit).ci))
            .load(Ordering::Relaxed);
        let next = libc::malloc(
            core::mem::size_of::<JitCodeIndex>() + 4096 * core::mem::size_of::<*mut JitBlock>(),
        )
        .cast::<JitCodeIndex>();
        if next.is_null() {
            libc::abort();
        }
        (*next).older = old;
        (*next).capacity = 4096;
        (*next).count = 0;
        AtomicPtr::<JitCodeIndex>::from_ptr(core::ptr::addr_of_mut!((*jit).ci))
            .store(next, Ordering::Release);
    }
}

unsafe fn jit_arena_reset_locked(jit: *mut OcerzJit) {
    unsafe {
        AtomicU64::from_ptr(core::ptr::addr_of_mut!(ocerz_jit_retire_count))
            .fetch_add(1, Ordering::Release);
        for k in 0..(*jit).n_live {
            let b = *(*jit).live.add(k);
            AtomicPtr::<JitBlock>::from_ptr(
                (*jit).buckets.as_mut_ptr().add(hash_key((*b).key) as usize),
            )
            .store(core::ptr::null_mut(), Ordering::Release);
            (*b).retired_next = (*jit).retired;
            (*jit).retired = b;
        }
        (*jit).n_live = 0;
        (*jit).code_lo = 0;
        (*jit).code_hi = 0;
        libc::memset(
            core::ptr::addr_of_mut!((*jit).invmap).cast(),
            0,
            core::mem::size_of_val(&(*jit).invmap),
        );
        (*jit).invmap_full = 0;
        gran_clear_all();
        pending_clear();
        AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_want)).store(0, Ordering::Relaxed);
        g_n_ras_cells = 0;
        g_ras_slot_n = 0;
        g_n_psc_tables = 0;
        g_psc_used = 0;
        (*jit).stop_blocks = core::ptr::null_mut();
        (*jit).dispatch_stub = core::ptr::null_mut();
        (*jit).dispatch_stub32 = core::ptr::null_mut();
        (*jit).veneer_n = 0;
        (*jit).veneer_next_mark = core::ptr::null_mut();
        libc::memset(
            core::ptr::addr_of_mut!((*jit).veneer_pool).cast(),
            0,
            core::mem::size_of_val(&(*jit).veneer_pool),
        );
        libc::memset(
            core::ptr::addr_of_mut!((*jit).veneer_used).cast(),
            0,
            core::mem::size_of_val(&(*jit).veneer_used),
        );
        code_index_reset_locked(jit);
        (*jit).code_cur = (*jit).code_start;
        (*jit).code_full = 0;
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct TripEnt {
    rip: u64,
    n: u64,
}

static mut g_trip_tab: [TripEnt; TRIP_SLOTS as usize] =
    [TripEnt { rip: 0, n: 0 }; TRIP_SLOTS as usize];
static mut g_trip_count: u64 = 0;
static mut g_trip_jit: *mut OcerzJit = core::ptr::null_mut();

extern "C" fn trip_report(arg: *mut c_void) -> *mut c_void {
    unsafe {
        let trip_tab = core::ptr::addr_of_mut!(g_trip_tab).cast::<TripEnt>();
        let mut last = 0u64;
        loop {
            usleep(10_000_000);
            let now =
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_trip_count)).load(Ordering::Relaxed);
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: TRIPSTAT[%d] trips/s=%.0f\n\0".as_ptr().cast(),
                libc::getpid(),
                (now.wrapping_sub(last) as f64) / 10.0,
            );
            last = now;
            let mut top = [0; 20];
            let top_ptr = top.as_mut_ptr();
            let mut nt = 0usize;
            for _ in 0..20 {
                let mut best = -1;
                for i in 0..TRIP_SLOTS as usize {
                    let mut used = 0;
                    for j in 0..nt {
                        used |= (*top_ptr.add(j) == i as c_int) as c_int;
                    }
                    if used == 0
                        && (*trip_tab.add(i)).n != 0
                        && (best < 0 || (*trip_tab.add(i)).n > (*trip_tab.add(best as usize)).n)
                    {
                        best = i as c_int;
                    }
                }
                if best < 0 {
                    break;
                }
                *top_ptr.add(nt) = best;
                nt += 1;
            }
            let mut sampled = 0u64;
            for i in 0..TRIP_SLOTS as usize {
                sampled = sampled.wrapping_add((*trip_tab.add(i)).n);
            }
            for j in 0..nt {
                let ent = trip_tab.add(*top_ptr.add(j) as usize);
                let rip = (*ent).rip;
                let tb = if !g_trip_jit.is_null() {
                    cache_lookup(g_trip_jit, rip, 0)
                } else {
                    core::ptr::null_mut()
                };
                libc::fprintf(
                    crate::log::stderr(),
                    b"ocerz: TRIPSTAT[%d]   %#llx %.1f%% blk=%d code=%d body=%d pin_class=%d n_pinned=%d insns=%d preds=%u execs=%llu\n\0"
                        .as_ptr()
                        .cast(),
                    libc::getpid(),
                    rip,
                    100.0 * (*ent).n as f64
                        / if sampled != 0 { sampled as f64 } else { 1.0 },
                    (!tb.is_null()) as c_int,
                    (!tb.is_null() && (*tb).code.is_some()) as c_int,
                    (!tb.is_null() && !(*tb).body_code.is_null()) as c_int,
                    if tb.is_null() { -1 } else { (*tb).pin_class as c_int },
                    if tb.is_null() { -1 } else { (*tb).n_pinned as c_int },
                    if tb.is_null() { -1 } else { (*tb).n_insns },
                    if tb.is_null() { 0 } else { (*tb).n_preds },
                    if tb.is_null() { 0 } else { (*tb).exec_count },
                );
            }
            libc::memset(
                core::ptr::addr_of_mut!(g_trip_tab).cast(),
                0,
                core::mem::size_of::<[TripEnt; TRIP_SLOTS as usize]>(),
            );
        }
        #[allow(unreachable_code)]
        arg
    }
}

unsafe fn trip_note(rip: u64) {
    static mut STATE: c_int = -1;
    unsafe {
        if STATE < 0 {
            STATE = 0;
            if !libc::getenv(b"OCERZ_TRIPSTAT\0".as_ptr().cast()).is_null() {
                let mut thread = core::mem::zeroed::<pthread_t>();
                if pthread_create(
                    &mut thread,
                    core::ptr::null(),
                    trip_report,
                    core::ptr::null_mut(),
                ) == 0
                {
                    pthread_detach(thread);
                    STATE = 1;
                }
            }
        }
        if STATE != 1 {
            return;
        }
        let c = AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_trip_count))
            .fetch_add(1, Ordering::Relaxed)
            .wrapping_add(1);
        if c & 63 != 0 {
            return;
        }
        let mut h =
            (rip.wrapping_mul(0x9E3779B97F4A7C15) >> 52) as usize & (TRIP_SLOTS as usize - 1);
        let trip_tab = core::ptr::addr_of_mut!(g_trip_tab).cast::<TripEnt>();
        for _ in 0..8 {
            let ent = trip_tab.add(h);
            if (*ent).rip == rip || (*ent).n == 0 {
                (*ent).rip = rip;
                (*ent).n = (*ent).n.wrapping_add(1);
                return;
            }
            h = h.wrapping_add(1) & (TRIP_SLOTS as usize - 1);
        }
    }
}

static mut g_flush_fail: u64 = 0;
static mut g_flush_n: u64 = 0;

#[unsafe(no_mangle)]
pub static mut ocerz_perfstat: c_int = -1;

static mut g_ps_atexit_jit: *mut OcerzJit = core::ptr::null_mut();

extern "C" fn ps_report_atexit() {
    unsafe {
        if !g_ps_atexit_jit.is_null() {
            ps_report(g_ps_atexit_jit);
        }
    }
}

unsafe fn jit_flush(vm: *mut OcerzVM, jit: *mut OcerzJit) -> c_int {
    static mut OFF: c_int = -1;
    static mut LOG: c_int = -1;
    unsafe {
        if OFF < 0 {
            OFF = if !libc::getenv(b"OCERZ_NO_JIT_FLUSH\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if OFF != 0 {
            return 0;
        }
        let t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if t0
            < AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_retry_ns)).load(Ordering::Relaxed)
        {
            return 0;
        }
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req))
            .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return 0;
        }
        let self_thr = jit_thr();
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!((*self_thr).frames)).load(Ordering::SeqCst)
            != AtomicI32::from_ptr(core::ptr::addr_of_mut!((*self_thr).parked))
                .load(Ordering::SeqCst)
        {
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).store(0, Ordering::SeqCst);
            return 0;
        }
        AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).fetch_add(1, Ordering::SeqCst);
        jl_acquire(line!() as c_int);
        let full = (*jit).code_full != 0
            || jit_space_low(jit) != 0
            || AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_want)).load(Ordering::Relaxed)
                != 0;
        if full && (*jit).blocks_translated == g_flush_mark {
            jl_release();
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen))
                .fetch_add(1, Ordering::SeqCst);
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).store(0, Ordering::SeqCst);
            return 0;
        }
        let mut n_undo = 0usize;
        let undo = if full {
            stop_sites_force_undoable(jit, &mut n_undo)
        } else {
            core::ptr::null_mut()
        };
        jl_release();
        if !full {
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen))
                .fetch_add(1, Ordering::SeqCst);
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).store(0, Ordering::SeqCst);
            return 1;
        }
        let mut ok = 1;
        let deadline = t0.wrapping_add(500_000_000);
        if LOG < 0 {
            LOG = if !libc::getenv(b"OCERZ_FLUSHLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        let mut next_report = t0.wrapping_add(50_000_000);
        while ok != 0 {
            let mut busy = 0;
            let mut t = AtomicPtr::<JitThr>::from_ptr(core::ptr::addr_of_mut!(g_jit_thr))
                .load(Ordering::Acquire);
            while !t.is_null() && busy == 0 {
                if t != self_thr
                    && AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames))
                        .load(Ordering::SeqCst)
                        != AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked))
                            .load(Ordering::SeqCst)
                {
                    busy = 1;
                }
                t = (*t).next;
            }
            if busy == 0 {
                break;
            }
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if LOG != 0 && now > next_report {
                next_report = next_report.wrapping_add(50_000_000);
                let mut t = AtomicPtr::<JitThr>::from_ptr(core::ptr::addr_of_mut!(g_jit_thr))
                    .load(Ordering::Acquire);
                while !t.is_null() {
                    if t != self_thr
                        && AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames))
                            .load(Ordering::SeqCst)
                            != AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).parked))
                                .load(Ordering::SeqCst)
                    {
                        let cpu = (*t).cpu;
                        libc::fprintf(
                            crate::log::stderr(),
                            b"ocerz: JIT flush waiting on cpu#%u frames=%d parked=%d last block %#llx\n\0"
                                .as_ptr()
                                .cast(),
                            if cpu.is_null() { 0 } else { (*cpu).cpu_number },
                            (*t).frames,
                            (*t).parked,
                            if cpu.is_null() { 0 } else { (*cpu).rip },
                        );
                    }
                    t = (*t).next;
                }
            }
            if now > deadline {
                ok = 0;
                g_flush_fail = g_flush_fail.wrapping_add(1);
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_retry_ns))
                    .store(t0.wrapping_add(2_000_000_000), Ordering::Relaxed);
                break;
            }
            let ts = timespec {
                tv_sec: 0,
                tv_nsec: 20_000,
            };
            nanosleep(&ts, core::ptr::null_mut());
        }
        jl_acquire(line!() as c_int);
        if ok == 0 {
            stop_sites_undo(jit, undo, n_undo);
        }
        libc::free(undo.cast());
        jl_release();
        if ok != 0 {
            jl_acquire(line!() as c_int);
            jit_arena_reset_locked(jit);
            g_flush_mark = (*jit).blocks_translated;
            jl_release();
            ocerz_vm_purge_jit_refs(vm);
            g_flush_n = g_flush_n.wrapping_add(1);
        }
        if LOG != 0 || (ok == 0 && g_flush_fail == 1) || (ok != 0 && g_flush_n == 1) {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: JIT arena %s (flush #%llu, %llu failed, %llu us)\n\0"
                    .as_ptr()
                    .cast(),
                if ok != 0 {
                    b"flushed\0".as_ptr().cast::<c_char>()
                } else {
                    b"flush gave up waiting for threads to leave translated code\0"
                        .as_ptr()
                        .cast::<c_char>()
                },
                g_flush_n,
                g_flush_fail,
                clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0) / 1000,
            );
        }
        AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).fetch_add(1, Ordering::SeqCst);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).store(0, Ordering::SeqCst);
        ok
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_step(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        g_trip_jit = (*vm).jit;
        trip_note((*cpu).rip);
        if (*cpu).rip.wrapping_sub(OCERZ_DYLDAPI_LO as u64)
            < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO) as u64
        {
            return OCERZ_EUNSUP as c_int;
        }
        static mut STEPLOG: c_int = -1;
        if STEPLOG < 0 {
            STEPLOG = if !libc::getenv(b"OCERZ_STEPLOG\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        if STEPLOG != 0 {
            steplog(cpu);
        }
        let jit = (*vm).jit;
        if !(*cpu).side_blk.is_null() {
            flip_side_hit(vm, jit, cpu);
        }
        if ocerz_jitstat < 0 {
            jl_acquire(line!() as c_int);
            if ocerz_jitstat < 0 {
                js_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                ps_t0 = js_t0;
                ocerz_perfstat = if !libc::getenv(b"OCERZ_PERFSTAT\0".as_ptr().cast()).is_null() {
                    1
                } else {
                    0
                };
                ocerz_jitstat = if !libc::getenv(b"OCERZ_JITSTAT\0".as_ptr().cast()).is_null() {
                    1
                } else {
                    0
                };
                if ocerz_perfstat > 0 {
                    g_ps_atexit_jit = jit;
                    libc::atexit(ps_report_atexit);
                }
                g_flaglive_log =
                    (!libc::getenv(b"OCERZ_FLAGLIVE\0".as_ptr().cast()).is_null()) as c_int;
                g_no_lazyflags =
                    (!libc::getenv(b"OCERZ_NO_LAZYFLAGS\0".as_ptr().cast()).is_null()) as c_int;
                g_no_ras = (!libc::getenv(b"OCERZ_NO_RAS\0".as_ptr().cast()).is_null()) as c_int;
                g_no_ldapr =
                    (!libc::getenv(b"OCERZ_NO_LDAPR\0".as_ptr().cast()).is_null()) as c_int;
                g_no_oolslow =
                    (!libc::getenv(b"OCERZ_NO_OOLSLOW\0".as_ptr().cast()).is_null()) as c_int;
                g_no_regflags =
                    (!libc::getenv(b"OCERZ_NO_REGFLAGS\0".as_ptr().cast()).is_null()) as c_int;
                g_no_chain =
                    (!libc::getenv(b"OCERZ_NO_CHAIN\0".as_ptr().cast()).is_null()) as c_int;
                g_no_jcclink =
                    (!libc::getenv(b"OCERZ_NO_JCCLINK\0".as_ptr().cast()).is_null()) as c_int;
                g_no_xlive =
                    (!libc::getenv(b"OCERZ_NO_XLIVE\0".as_ptr().cast()).is_null()) as c_int;
                g_no_jccfuse =
                    (!libc::getenv(b"OCERZ_NO_JCCFUSE\0".as_ptr().cast()).is_null()) as c_int;
                g_no_addincfuse =
                    (!libc::getenv(b"OCERZ_NO_ADDINCFUSE\0".as_ptr().cast()).is_null()) as c_int;
                g_no_fault_recipes =
                    (!libc::getenv(b"OCERZ_NO_FAULT_RECIPES\0".as_ptr().cast()).is_null()) as c_int;
                g_plain_mem = (*jit).plain_mem;
            }
            jl_release();
        }
        if ocerz_jitstat > 0 {
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_steps)).fetch_add(1, Ordering::SeqCst);
        }
        if ocerz_perfstat > 0 {
            ps_steps = ps_steps.wrapping_add(1);
            let s = ps_steps;
            if s & 0xfffff == 0 {
                static PS_NEXT: AtomicU64 = AtomicU64::new(0);
                let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                let exp = PS_NEXT.load(Ordering::Relaxed);
                if now >= exp
                    && PS_NEXT
                        .compare_exchange(
                            exp,
                            now.wrapping_add(15_000_000_000),
                            Ordering::Relaxed,
                            Ordering::Relaxed,
                        )
                        .is_ok()
                {
                    ps_report(jit);
                }
            }
        }
        if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).load(Ordering::SeqCst) != 0 {
            return OCERZ_EUNSUP as c_int;
        }
        let generation =
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).load(Ordering::SeqCst);
        let mut b = cache_lookup(jit, (*cpu).rip, (*cpu).mode32 as c_int);
        if b.is_null()
            && (jit_space_low(jit) != 0
                || AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_want))
                    .load(Ordering::Relaxed)
                    != 0)
            && jit_flush(vm, jit) != 0
        {
            return OCERZ_STEP_OK as c_int;
        }
        if b.is_null() {
            jl_lock_step((*cpu).rip);
            core::ptr::addr_of_mut!(g_jl_owner_cpu).write_volatile(cpu);
            if ocerz_perfstat > 0 {
                ps_misses = ps_misses.wrapping_add(1);
            }
            if ocerz_jitstat > 0 {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_misses))
                    .fetch_add(1, Ordering::SeqCst);
                let misses =
                    AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_misses)).load(Ordering::SeqCst);
                if misses & 0x3ff == 0 {
                    static mut NEXT_S: u64 = 0;
                    static mut NEXT_F: u64 = 0;
                    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    if now >= NEXT_F {
                        NEXT_F = now.wrapping_add(60_000_000_000);
                        NEXT_S = now.wrapping_add(10_000_000_000);
                        js_report(jit, b"FULL\0".as_ptr().cast(), 1);
                    } else if now >= NEXT_S {
                        NEXT_S = now.wrapping_add(10_000_000_000);
                        js_report(jit, b"tick\0".as_ptr().cast(), 0);
                    }
                }
            }
            b = cache_lookup(jit, (*cpu).rip, (*cpu).mode32 as c_int);
            if b.is_null() {
                if ocerz_jitstat > 0 {
                    AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_xlat))
                        .fetch_add(1, Ordering::SeqCst);
                }
                g_plain_mem = ((*jit).plain_mem != 0
                    || ((*cpu).mode32 == 0 && ocerz_dyldapi_memfn((*cpu).rip) != 0))
                    as c_int;
                if g_jl_log > 0 {
                    AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_jl_phase))
                        .store(2, Ordering::Relaxed);
                }
                g_xlat_ftop = ((*cpu).ftop & 7) as c_int;
                b = translate(jit, (*cpu).rip, (*cpu).mode32 as c_int);
                g_xlat_ftop = -1;
                if !b.is_null() {
                    xlatpage_note((*cpu).rip);
                }
                if g_jl_log > 0 && b.is_null() {
                    AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_jl_xlat_null))
                        .fetch_add(1, Ordering::Relaxed);
                }
            }
            core::ptr::addr_of_mut!(g_jl_owner_cpu).write_volatile(core::ptr::null_mut());
            jl_unlock_step();
        } else {
            if ocerz_jitstat > 0 {
                AtomicU64::from_ptr(core::ptr::addr_of_mut!(js_hits))
                    .fetch_add(1, Ordering::SeqCst);
            }
            if ocerz_perfstat > 0 {
                ps_hits = ps_hits.wrapping_add(1);
            }
        }
        if b.is_null() {
            return OCERZ_EUNSUP as c_int;
        }
        if (*b).code.is_none() {
            if t_xlat_overflow != 0 {
                t_xlat_overflow = 0;
                if jit_flush(vm, jit) != 0 {
                    return OCERZ_STEP_OK as c_int;
                }
            }
            return jit_interp_block(vm, cpu, b);
        }
        static mut CODECHECK: c_int = -1;
        if CODECHECK < 0 {
            CODECHECK = if !libc::getenv(b"OCERZ_CODECHECK\0".as_ptr().cast()).is_null() {
                1
            } else {
                0
            };
        }
        let code = block_code_ptr(b);
        if CODECHECK != 0
            && ((code as usize) < (*jit).code_base as usize
                || (code as usize) >= (*jit).code_cur as usize)
        {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: CODECHECK[%d] blk=%p rip=%#llx code=%p OUTSIDE arena [%p,%p) n_insns=%d pin_class=%d n_pinned=%d body=%p noreload=%p hoist_sig=%#llx key_field=%#llx code_words=%u\n\0"
                    .as_ptr()
                    .cast(),
                libc::getpid(),
                b,
                (*cpu).rip,
                code,
                (*jit).code_base,
                (*jit).code_cur,
                (*b).n_insns,
                (*b).pin_class as c_int,
                (*b).n_pinned as c_int,
                (*b).body_code,
                (*b).body_noreload,
                (*b).hoist_sig,
                (*b).key,
                (*b).code_words,
            );
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: CODECHECK[%d] blk words:\0".as_ptr().cast(),
                libc::getpid(),
            );
            for i in 0..24 {
                libc::fprintf(
                    crate::log::stderr(),
                    b" %llx\0".as_ptr().cast(),
                    *((b as *const u64).add(i)),
                );
            }
            libc::fprintf(crate::log::stderr(), b"\n\0".as_ptr().cast());
            return OCERZ_STEP_FATAL as c_int;
        }
        let t = jit_thr();
        let base =
            AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).load(Ordering::Relaxed);
        (*t).cpu = cpu;
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).store(base + 1, Ordering::SeqCst);
        let flush_req =
            AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req)).load(Ordering::SeqCst);
        let flush_gen =
            AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_flush_gen)).load(Ordering::SeqCst);
        if flush_req != 0 || flush_gen != generation {
            AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).store(base, Ordering::SeqCst);
            return if AtomicI32::from_ptr(core::ptr::addr_of_mut!(g_flush_req))
                .load(Ordering::SeqCst)
                != 0
            {
                OCERZ_EUNSUP as c_int
            } else {
                OCERZ_STEP_OK as c_int
            };
        }
        let r = (*b).code.unwrap_unchecked()(vm, cpu);
        AtomicI32::from_ptr(core::ptr::addr_of_mut!((*t).frames)).store(base, Ordering::SeqCst);
        r
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_fault_recover_flags(
    vm: *const OcerzVM,
    host_pc: *const c_void,
    cpu: *mut OcerzCPU,
) {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null()
        } else {
            (*vm).jit
        };
        let pc = host_pc.cast::<u32>();
        let b = fault_block(jit, pc);
        let fi = fault_insn_index(b, pc);
        if cpu.is_null() || fi < 0 || (*b).fault_flags.is_null() {
            return;
        }
        let recipe = *(*b).fault_flags.add(fi as usize);
        if recipe.kind as c_uint == JFF_NONE || recipe.producer as c_int >= (*b).n_insns {
            return;
        }
        let p = blk_insn_full(b, recipe.producer as c_int);
        if p.is_null() {
            return;
        }
        let p_ops = core::ptr::addr_of!((*p).ops).cast::<X86Operand>();
        if (*p).nops < 1
            || (*p_ops).kind as c_uint != OCERZ_OPK_REG
            || (*p_ops).high8 != 0
            || ((*p_ops).size != 4 && (*p_ops).size != 8)
        {
            return;
        }
        let size = (*p_ops).size as c_int;
        let res = ocerz_trunc(
            *core::ptr::addr_of!((*cpu).gpr)
                .cast::<u64>()
                .add((*p_ops).reg as usize),
            size,
        );
        let mut src = 0u64;
        let cc_src;
        let cc_dst;
        let cc_op;
        match recipe.kind as c_uint {
            JFF_LOGIC_RESULT => {
                if (*p).op as c_uint != OCERZ_OP_AND
                    && (*p).op as c_uint != OCERZ_OP_OR
                    && (*p).op as c_uint != OCERZ_OP_XOR
                {
                    return;
                }
                cc_src = res;
                cc_dst = res;
                cc_op = ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0);
            }
            JFF_ADD_RESULT_SRC => {
                if (*p).op as c_uint != OCERZ_OP_ADD
                    || (*p).nops != 2
                    || fault_recipe_rhs(cpu, p_ops.add(1), size, &mut src) == 0
                {
                    return;
                }
                cc_src = ocerz_trunc(res.wrapping_sub(src), size);
                cc_dst = src;
                cc_op = ocerz_cc_pack(OCERZ_CC_ADD, size, 0);
            }
            JFF_ADD_INC_RESULT_SRC => {
                if (*p).op as c_uint != OCERZ_OP_ADD
                    || (*p).nops != 2
                    || recipe.producer as c_int + 1 >= (*b).n_insns
                    || fault_recipe_rhs(cpu, p_ops.add(1), size, &mut src) == 0
                {
                    return;
                }
                let inc = blk_insn_full(b, recipe.producer as c_int + 1);
                if inc.is_null() {
                    return;
                }
                let inc_ops = core::ptr::addr_of!((*inc).ops).cast::<X86Operand>();
                if (*inc).op as c_uint != OCERZ_OP_INC
                    || (*inc).nops != 1
                    || (*inc_ops).kind as c_uint != OCERZ_OPK_REG
                    || (*inc_ops).high8 != 0
                    || (*inc_ops).reg != (*p_ops).reg
                    || (*inc_ops).size as c_int != size
                {
                    return;
                }
                let add_res = ocerz_trunc(res.wrapping_sub(1), size);
                let add_lhs = ocerz_trunc(add_res.wrapping_sub(src), size);
                cc_src = (add_res < add_lhs) as u64;
                cc_dst = res;
                cc_op = ocerz_cc_pack(OCERZ_CC_INC, size, 0);
            }
            _ => return,
        }
        (*cpu).cc_src = cc_src;
        (*cpu).cc_dst = cc_dst;
        (*cpu).cc_op = cc_op;
    }
}

unsafe fn retire_fault_blocks(
    vm: *mut OcerzVM,
    jit: *mut OcerzJit,
    block_rip: u64,
    fault_rip: u64,
    mode32: c_int,
) {
    unsafe {
        jl_acquire(line!() as c_int);
        let b = cache_lookup(jit, block_rip, mode32);
        if !b.is_null() && (*b).code.is_some() {
            flip_retire_locked(vm, jit, b);
        }
        if fault_rip != block_rip {
            let f = cache_lookup(jit, fault_rip, mode32);
            if !f.is_null() && (*f).code.is_some() {
                flip_retire_locked(vm, jit, f);
            }
        }
        jl_release();
        ocerz_vm_purge_jit_ras(vm);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_note_commpage_fault(
    vm: *mut OcerzVM,
    host_pc: *const c_void,
    fault_rip: u64,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null_mut()
        } else {
            (*vm).jit
        };
        let pc = host_pc.cast::<u32>();
        let b = fault_block(jit, pc);
        if b.is_null() {
            return 0;
        }
        let block_rip = blk_rip(b);
        let fresh = cp_marked((*b).key) == 0;
        cp_mark((*b).key);
        cp_mark(jit_key(fault_rip, blk_mode32(b)));
        if env_on!("OCERZ_CP_NOINVAL") != 0 {
            return 1;
        }
        if !fresh
            && cache_lookup(jit, block_rip, blk_mode32(b)) != b.cast_mut()
            && env_on!("OCERZ_REFAULT_INVAL") == 0
        {
            return 1;
        }
        if fresh && env_on!("OCERZ_FAULT_INV_RANGE") == 0 {
            retire_fault_blocks(vm, jit, block_rip, fault_rip, blk_mode32(b));
            return 1;
        }
        let prev = g_churn_suppress;
        if fresh {
            g_churn_suppress = 1;
        }
        ocerz_jit_invalidate_range(vm, block_rip, 1);
        if fault_rip != block_rip {
            ocerz_jit_invalidate_range(vm, fault_rip, 1);
        }
        g_churn_suppress = prev;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jit_note_align_fault(
    vm: *mut OcerzVM,
    host_pc: *const c_void,
    fault_rip: u64,
) -> c_int {
    unsafe {
        let jit = if vm.is_null() {
            core::ptr::null_mut()
        } else {
            (*vm).jit
        };
        let pc = host_pc.cast::<u32>();
        let b = fault_block(jit, pc);
        if b.is_null() || (*jit).plain_mem != 0 {
            return 0;
        }
        let block_rip = blk_rip(b);
        let ik = jit_key(fault_rip, blk_mode32(b));
        let mut fresh = al_marked(ik) == 0;
        if fresh {
            al_mark(ik);
        } else {
            fresh = al_marked((*b).key | AL_BLK_TAG) == 0;
            al_mark((*b).key | AL_BLK_TAG);
        }
        if !fresh
            && cache_lookup(jit, block_rip, blk_mode32(b)) != b.cast_mut()
            && env_on!("OCERZ_REFAULT_INVAL") == 0
        {
            return 1;
        }
        if fresh && env_on!("OCERZ_FAULT_INV_RANGE") == 0 {
            retire_fault_blocks(vm, jit, block_rip, fault_rip, blk_mode32(b));
            return 1;
        }
        let prev = g_churn_suppress;
        if fresh {
            g_churn_suppress = 1;
        }
        ocerz_jit_invalidate_range(vm, block_rip, 1);
        if fault_rip != block_rip {
            ocerz_jit_invalidate_range(vm, fault_rip, 1);
        }
        g_churn_suppress = prev;
        1
    }
}
