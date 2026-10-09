//! Guest memory: the mapping tables, the 4 KB-guest-in-16 KB-host slot
//! machinery, and the host protections that follow from them.
//!
//! A host page is shared by up to four guest pages, so its protection is the
//! union of its slots' - minus write while the page is ARMED, which means
//! translations were made from code in it.  Arming is how self-modifying code is
//! caught: a later store (a guest JIT rewriting its own code, as V8 does with
//! plain stores into RWX pages) faults, the translations are dropped, and the
//! page is unarmed before the new bytes run.  Only code that itself sits in a
//! guest-writable slot can be rewritten by a store - code in an RX slot changes
//! only through mprotect/mmap, which invalidate on their own - so a page is
//! armed only when a writable slot overlaps the translated bytes, which leaves
//! Wine's PE images untouched where a .text tail shares a host page with a
//! writable section.
//!
//! The reverse direction matters just as much: a kernel copyout that meets a
//! read-only page does not fault, it fails, and a mach reply is destroyed with
//! it.  So the syscall layer unarms every armed page overlapping a buffer the
//! kernel is about to write, and retries once after unarming when a syscall
//! comes back EFAULT with armed pages about.  A counter of currently-armed pages
//! lets that whole check be skipped at zero.
//!
//! Wine's PE loader re-commits over live pages constantly, and the general path
//! costs a memset plus two mprotects per 16 KB page, so a run of whole pages
//! that all have to end up zeroed is replaced with one fresh anonymous mapping.
//! Pages shared with another process are excluded, and so is one specific
//! sibling slot: Wine's syscall-dispatcher slot at 0x7ffe1000, next to
//! KUSER_SHARED_DATA, is physically the shared file every process of the prefix
//! maps.  Zero-filling it for this process zeroes it for all of them, and until
//! this process stores its pointer a few instructions later every syscall
//! elsewhere calls NULL - Steam's CEF children died that way (2026-09-06).  The
//! value is identical in every process, so the slot is left alone.
//!
//! That holds only while ntdll.so loads at the same address everywhere, so a
//! Wine process reserves its identity arena at the fixed base
//! OCERZ_WINE_ARENA_BASE, falling back to the usual placement only if that
//! range is taken.  With the arena wherever the host put it, each process
//! stored a dispatcher address the others had never mapped, and whichever
//! process wrote the shared slot last sent every other one's syscalls into
//! unmapped memory - Wine's notepad died that way while wineboot was still
//! starting services.
//!
//! A guest mmap(MAP_FIXED) that comes back ENOMEM is indistinguishable from real
//! memory pressure inside the guest, and Chromium's allocator treats a refused
//! 4 KB commit as out-of-memory, so OCERZ_MAPFAILLOG names which test in the
//! fixed-mapping path refused instead of leaving it to guesswork.
//!
//! ocerz_mem_overlaps answers whether any part of a range lies in memory these
//! tables describe.  In cache mode every page a guest can name is here, but in
//! native mode the guest's heap is the host's, so a guest that unmaps or
//! protects a buffer native malloc or a native Mach call handed it names memory
//! no table here has ever seen, and native mode passes such a range to the host
//! kernel instead of refusing it.
//!
//! The general mapping path walks the range a 4 KB slot at a time, several
//! times over, and V8 reserves and releases gigabytes at startup, so 1 GB cost
//! 1.8 ms where Rosetta pays nothing.  A mapping onto slots that are all free,
//! starting on a host page, with no shared or armed page in its reach and its
//! guard-only pages uncommitted, ends in exactly the state one host mmap of the
//! data pages gives - zeroed, at the final protection, committed - so it takes
//! that mmap and fills the slot array directly.  A replacing mapping over
//! nothing qualifies too, which is every mapping at an address hint, since that
//! region was created a moment before.  Unmapping releases slots in runs of one
//! owner, and a fully free unshared range is made inaccessible with one mmap.
//! 1 GB reserve and release costs 0.45 ms that way.
//!
//! The low shadow puts the guest's first 12 GB at another host address, so a
//! host pointer below 12 GB that a service or the kernel hands the guest names
//! the guest's own memory at that number, and has to be aliased into the
//! shadow before the guest can read it (see syscall.c).  The identity arena
//! starts at 12 GB, so the kernel placed its mappings in the free host space
//! below it, where Wine and Unity keep their memory too: IOSurface's records
//! landed on numbers the guest already used, where they cannot be aliased,
//! and R.E.P.O. failed to start in three launches of five, on wild pointers
//! in Wine's fault path; with that space reserved, eight launches of ten
//! reached the menu.  A low-shadow process therefore
//! reserves every free host range between 4 GB and 12 GB, at startup and again
//! after each alias, and the kernel maps above the arena instead, where guest
//! and host addresses are the same (OCERZ_NO_LOW_HOLE_FILL=1 turns it off).
//!
//! In native mode the guest calls the host's own libraries, so host pointers
//! below 12 GB reach it all the time: an object from the first malloc region at
//! 4 GB, a structure on the main thread's stack at 5.7 GB, a constant in the
//! shared cache between 6 and 11 GB.  Translated code reads every address below
//! 12 GB through the shadow, so ocerz_mem_pin_host_low remaps each readable host
//! region there into the shadow at the same offset, where the guest reads and
//! writes the host's own pages, and pins it: a fixed mapping, a claim or a
//! shared overlay that touches a pinned range is refused, and an unmap or a
//! protection change leaves the pinned part alone.  It runs once the main image
//! is mapped, and leaves out any page the guest already holds; that is where an
//! x86 executable's fixed segments win over the host, as Wine's loader at 8 GB
//! does over the shared cache.  Ranges the guest never maps stay the host's.
//! The free host space left below 12 GB is then reserved again, as the hole
//! fill above does at startup, so a host allocation made later cannot land
//! where the guest would see the shadow instead of it.  In a process that pins,
//! a value below 64 KB is nobody's memory and crosses unchanged (ocerz_g2h).
//! OCERZ_PINLOG=1 prints each range as it is pinned, OCERZ_NO_LOW_PIN=1 turns
//! pinning off.

use core::ffi::{c_char, c_int, c_uint, c_ulong, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::{
    AtomicI32, AtomicI64, AtomicPtr, AtomicU8, AtomicU32, AtomicU64, Ordering, fence,
};

use crate::ffi;
use crate::ported::globals::{ocerz_critical_depth, ocerz_mode};
use crate::{ocerz_fatal, ocerz_log};

unsafe extern "C" {
    fn ocerz_current_guest_rip() -> u64;
    fn ocerz_current_guest_rsp() -> u64;
}

type MachVmAddress = u64;
type MachVmSize = u64;
type KernReturn = c_int;
type Natural = c_uint;
type MachMsgTypeNumber = c_uint;
type MachPort = c_uint;
type VmProt = c_int;

const KERN_SUCCESS: KernReturn = 0;
const VM_PROT_READ: VmProt = 1;
const VM_FLAGS_FIXED: c_int = 0;
const VM_FLAGS_OVERWRITE: c_int = 0x4000;
const VM_INHERIT_DEFAULT: c_int = 1;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const MACH_PORT_NULL: MachPort = 0;
const VM_REGION_SUBMAP_SHORT_INFO_COUNT_64: MachMsgTypeNumber = 12;
const VM_REGION_BASIC_INFO_COUNT_64: MachMsgTypeNumber = 9;

#[repr(C)]
struct VmRegionSubmapShortInfo64 {
    protection: VmProt,
    max_protection: VmProt,
    inheritance: c_uint,
    offset: u32,
    _pad0: u32,
    user_tag: c_uint,
    ref_count: c_uint,
    shadow_depth: u16,
    external_pager: u8,
    share_mode: u8,
    is_submap: c_uint,
    behavior: c_int,
    object_id: u32,
    user_wired_count: u16,
    flags: u16,
}
const _: () = assert!(size_of::<VmRegionSubmapShortInfo64>() == 48);
const _: () = assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, protection) == 0);
const _: () = assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, max_protection) == 4);
const _: () = assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, offset) == 12);
const _: () = assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, flags) == 46);

#[repr(C)]
struct VmRegionBasicInfo64 {
    protection: VmProt,
    max_protection: VmProt,
    inheritance: c_uint,
    shared: c_uint,
    reserved: c_uint,
    offset: u32,
    _pad1: u32,
    behavior: c_int,
    user_wired_count: u16,
    _pad0: u16,
}
const _: () = assert!(size_of::<VmRegionBasicInfo64>() == 36);
const _: () = assert!(core::mem::offset_of!(VmRegionBasicInfo64, protection) == 0);
const _: () = assert!(core::mem::offset_of!(VmRegionBasicInfo64, max_protection) == 4);
const _: () = assert!(core::mem::offset_of!(VmRegionBasicInfo64, offset) == 20);
const _: () = assert!(core::mem::offset_of!(VmRegionBasicInfo64, behavior) == 28);
const _: () = assert!(core::mem::offset_of!(VmRegionBasicInfo64, user_wired_count) == 32);

unsafe extern "C" {
    static mut mach_task_self_: MachPort;
    fn mach_vm_allocate(
        target: MachPort,
        address: *mut MachVmAddress,
        size: MachVmSize,
        flags: c_int,
    ) -> KernReturn;
    fn mach_vm_deallocate(target: MachPort, address: MachVmAddress, size: MachVmSize)
    -> KernReturn;
    fn mach_vm_region_recurse(
        target: MachPort,
        address: *mut MachVmAddress,
        size: *mut MachVmSize,
        nesting_depth: *mut Natural,
        info: *mut c_void,
        infoCnt: *mut MachMsgTypeNumber,
    ) -> KernReturn;
    fn mach_vm_remap(
        target: MachPort,
        target_address: *mut MachVmAddress,
        size: MachVmSize,
        mask: MachVmAddress,
        flags: c_int,
        src_task: MachPort,
        src_address: MachVmAddress,
        copy: c_int,
        cur_protection: *mut VmProt,
        max_protection: *mut VmProt,
        inheritance: c_int,
    ) -> KernReturn;
    fn mach_vm_region(
        target: MachPort,
        address: *mut MachVmAddress,
        size: *mut MachVmSize,
        flavor: c_int,
        info: *mut c_void,
        infoCnt: *mut MachMsgTypeNumber,
        object_name: *mut MachPort,
    ) -> KernReturn;
}

#[inline(always)]
unsafe fn mach_task_self() -> MachPort {
    unsafe { mach_task_self_ }
}

const OCERZ_GUEST_PAGE: u64 = 0x1000;
const OCERZ_HOST_PAGE: u64 = 0x4000;
const OCERZ_COMMPAGE_LO: u64 = 0x00007fffffe00000;
const OCERZ_COMMPAGE_HI: u64 = 0x00007fffffe04000;
const OCERZ_LOW_LIMIT: u64 = 0x0000000300000000;
const OCERZ_NULL_LIMIT: u64 = 0x0000000000010000;
const OCERZ_TOP_LO: u64 = 0x00007ffffe000000;
const OCERZ_TOP_HI: u64 = 0x00007fffffe00000;
const OCERZ_WINE_ARENA_BASE: u64 = 0x7a0000000000;

const PROT_NONE: c_int = 0;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const PROT_EXEC: c_int = 4;

static mut MAP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe extern "C" {
    fn ocerz_jit_lock_held_self() -> c_int;
}

unsafe fn map_lock_acquire() {
    unsafe {
        ocerz_critical_depth += 1;
        static mut LG: c_int = -1;
        if LG < 0 {
            LG = if libc::getenv(c"OCERZ_JITLOCKLOG".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if LG != 0 && ocerz_jit_lock_held_self() != 0 {
            static mut ONCE: c_int = 0;
            if ONCE < 6 {
                ONCE += 1;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: LOCKORDER[%d] map_lock wanted while holding jit_lock (ABBA risk)\n"
                        .as_ptr(),
                    libc::getpid() as c_int,
                );
            }
        }
        libc::pthread_mutex_lock(&raw mut MAP_LOCK);
    }
}

unsafe fn map_lock_release() {
    unsafe {
        libc::pthread_mutex_unlock(&raw mut MAP_LOCK);
        ocerz_critical_depth -= 1;
    }
}

static mut G_INITGATE_M: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_INITGATE_CV: libc::pthread_cond_t = libc::PTHREAD_COND_INITIALIZER;
static mut G_INIT_RELEASED: c_int = 1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_arm() {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_INITGATE_M);
        ptr::write_volatile(
            &raw mut G_INIT_RELEASED,
            if libc::getenv(c"WINEARCH".as_ptr()).is_null() {
                1
            } else {
                0
            },
        );
        libc::pthread_mutex_unlock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_release() {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_INITGATE_M);
        if ptr::read_volatile(&raw const G_INIT_RELEASED) == 0 {
            ptr::write_volatile(&raw mut G_INIT_RELEASED, 1);
            libc::pthread_cond_broadcast(&raw mut G_INITGATE_CV);
            if !libc::getenv(c"OCERZ_THRLOG".as_ptr()).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: INITGATE released pid=%d\n".as_ptr(),
                    libc::getpid() as c_int,
                );
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_wait() {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_INITGATE_M);
        while ptr::read_volatile(&raw const G_INIT_RELEASED) == 0 {
            libc::pthread_cond_wait(&raw mut G_INITGATE_CV, &raw mut G_INITGATE_M);
        }
        libc::pthread_mutex_unlock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_prefork() {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_postfork_parent() {
    unsafe {
        libc::pthread_mutex_unlock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_init_gate_postfork_child() {
    unsafe {
        ptr::write_volatile(&raw mut G_INIT_RELEASED, 1);
        libc::pthread_mutex_unlock(&raw mut G_INITGATE_M);
    }
}

#[unsafe(no_mangle)]
pub static mut ocerz_guest_base: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_arena_lo: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_arena_hi: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_low_base: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_top_base: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_commpage: *mut u8 = ptr::null_mut();

static mut BUMP_NEXT: u64 = 0;
static mut ALLOC_FLOOR: u64 = 0;

#[inline(always)]
fn align_up(value: u64, align: u64) -> u64 {
    (value + (align - 1)) & !(align - 1)
}

const MEM_SLOT_OWNER_MASK: u32 = 0x00ffffff;
const MEM_SLOT_PROT_SHIFT: u32 = 24;
const MEM_SLOT_PROT_MASK: u32 = 0x07000000;
const MEM_SLOT_GUARD: u32 = 0x80000000;

const MEM_SHARED_SLOT_MASK: u8 = 0x0f;
const MEM_SHARED_PADDED: u8 = 0x40;
const MEM_SHARED_PHYSICAL: u8 = 0x80;

#[repr(C)]
struct MemOwner {
    guard_lo: u64,
    guard_hi: u64,
    live_slots: u32,
    next_free: u32,
    region: u16,
    active: u8,
}

#[repr(C)]
struct MemRegion {
    glo: u64,
    ghi: u64,
    bm: *mut u8,
    shared: *mut u8,
    armed: *mut u8,
    slots: *mut u32,
}

const MEM_REGION_MAX: usize = 128;
static mut REGIONS: [MemRegion; MEM_REGION_MAX] = {
    const EMPTY: MemRegion = MemRegion {
        glo: 0,
        ghi: 0,
        bm: ptr::null_mut(),
        shared: ptr::null_mut(),
        armed: ptr::null_mut(),
        slots: ptr::null_mut(),
    };
    [EMPTY; MEM_REGION_MAX]
};
static G_ARMED_LIVE: AtomicI64 = AtomicI64::new(0);
static REGION_N: AtomicI32 = AtomicI32::new(0);

static mut OWNERS: *mut MemOwner = ptr::null_mut();
static mut OWNER_N: u32 = 0;
static mut OWNER_CAP: u32 = 0;
static mut OWNER_FREE: u32 = 0;

#[inline(always)]
unsafe fn ocerz_pinned_page(gaddr: u64) -> c_int {
    unsafe {
        let pm = ocerz_pin_map;
        if !pm.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*pm.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
        {
            1
        } else {
            0
        }
    }
}

#[inline(always)]
unsafe fn ocerz_g2h(gaddr: u64) -> *mut c_void {
    unsafe {
        let cp = ocerz_commpage;
        if !cp.is_null() && gaddr >= OCERZ_COMMPAGE_LO && gaddr < OCERZ_COMMPAGE_HI {
            return cp.add((gaddr - OCERZ_COMMPAGE_LO) as usize) as *mut c_void;
        }
        if ocerz_low_base != 0 {
            if gaddr < OCERZ_LOW_LIMIT {
                if (gaddr < OCERZ_NULL_LIMIT && !ocerz_pin_map.is_null())
                    || ocerz_pinned_page(gaddr) != 0
                {
                    return gaddr as *mut c_void;
                }
                return gaddr.wrapping_add(ocerz_low_base) as *mut c_void;
            }
            if gaddr.wrapping_sub(OCERZ_TOP_LO) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return gaddr
                    .wrapping_sub(OCERZ_TOP_LO)
                    .wrapping_add(ocerz_top_base) as *mut c_void;
            }
        }
        gaddr.wrapping_add(ocerz_guest_base) as *mut c_void
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_commpage_init() {
    unsafe {
        if !ocerz_commpage.is_null() {
            return;
        }
        let cp = libc::calloc(1, 0x4000) as *mut u8;
        if cp.is_null() {
            return;
        }
        static TMPL: [u8; 0x70] = [
            0x63, 0x6f, 0x6d, 0x6d, 0x70, 0x61, 0x67, 0x65, 0x20, 0x36, 0x34, 0x2d, 0x62, 0x69,
            0x74, 0x00, 0xaf, 0x1f, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x0e, 0x00, 0xaf, 0x1f, 0x0c, 0x00, 0x00, 0x00, 0x40, 0x00, 0xad, 0xdb,
            0xba, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00, 0x0c, 0x0c, 0x0c, 0x03,
            0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xec, 0x5e, 0x3b, 0x57, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        ];
        ptr::copy_nonoverlapping(TMPL.as_ptr(), cp, TMPL.len());
        ocerz_commpage = cp;
    }
}

#[inline(always)]
fn round_down(v: u64) -> u64 {
    v & !(OCERZ_HOST_PAGE - 1)
}

#[inline(always)]
fn round_up(v: u64) -> u64 {
    (v + OCERZ_HOST_PAGE - 1) & !(OCERZ_HOST_PAGE - 1)
}

#[inline(always)]
fn guest_round_down(v: u64) -> u64 {
    v & !(OCERZ_GUEST_PAGE - 1)
}

#[inline(always)]
fn guest_round_up(v: u64) -> u64 {
    (v + OCERZ_GUEST_PAGE - 1) & !(OCERZ_GUEST_PAGE - 1)
}

#[inline(always)]
fn host_prot(prot: c_int) -> c_int {
    let mut p = 0;
    if prot & PROT_READ != 0 {
        p |= PROT_READ;
    }
    if prot & PROT_WRITE != 0 {
        p |= PROT_READ | PROT_WRITE;
    }
    if prot & PROT_EXEC != 0 {
        p |= PROT_READ;
    }
    p
}

#[inline(always)]
unsafe fn page_host_prot(r: *const MemRegion, i: usize, guest_prot: c_int) -> c_int {
    unsafe {
        let mut p = host_prot(guest_prot);
        if !(*r).armed.is_null() && *(*r).armed.add(i) != 0 {
            p &= !PROT_WRITE;
        }
        p
    }
}

unsafe fn region_for_range(lo: u64, hi: u64) -> *mut MemRegion {
    unsafe {
        if hi <= lo {
            return ptr::null_mut();
        }
        let n = REGION_N.load(Ordering::Acquire);
        for k in 0..n {
            let r = ((&raw mut REGIONS) as *mut MemRegion).add(k as usize);
            if lo >= (*r).glo && hi <= (*r).ghi {
                return r;
            }
        }
        ptr::null_mut()
    }
}

#[inline(always)]
unsafe fn pg_index(r: *const MemRegion, gaddr: u64) -> usize {
    unsafe { ((round_down(gaddr) - (*r).glo) / OCERZ_HOST_PAGE) as usize }
}

#[inline(always)]
unsafe fn slot_index(r: *const MemRegion, gaddr: u64) -> usize {
    unsafe { ((guest_round_down(gaddr) - (*r).glo) / OCERZ_GUEST_PAGE) as usize }
}

#[inline(always)]
unsafe fn slot_load(r: *const MemRegion, i: usize) -> u32 {
    unsafe {
        if (*r).slots.is_null() {
            0
        } else {
            AtomicU32::from_ptr((*r).slots.add(i)).load(Ordering::Acquire)
        }
    }
}

#[inline(always)]
unsafe fn slot_store(r: *const MemRegion, i: usize, value: u32) {
    unsafe {
        if !(*r).slots.is_null() {
            AtomicU32::from_ptr((*r).slots.add(i)).store(value, Ordering::Release);
        }
    }
}

#[inline(always)]
fn slot_owner(state: u32) -> u32 {
    state & MEM_SLOT_OWNER_MASK
}

#[inline(always)]
fn slot_is_data(state: u32) -> bool {
    slot_owner(state) != 0 && (state & MEM_SLOT_GUARD) == 0
}

#[inline(always)]
fn slot_data_state(owner: u32, prot: c_int) -> u32 {
    owner | (((prot & 7) as u32) << MEM_SLOT_PROT_SHIFT)
}

#[inline(always)]
unsafe fn bit_test(r: *const MemRegion, i: usize) -> c_int {
    unsafe {
        if !(*r).bm.is_null()
            && (AtomicU8::from_ptr((*r).bm.add(i >> 3)).load(Ordering::Acquire) & (1u8 << (i & 7)))
                != 0
        {
            1
        } else {
            0
        }
    }
}

#[inline(always)]
unsafe fn bit_set(r: *const MemRegion, i: usize) {
    unsafe {
        if !(*r).bm.is_null() {
            AtomicU8::from_ptr((*r).bm.add(i >> 3)).fetch_or(1u8 << (i & 7), Ordering::Release);
        }
    }
}

#[inline(always)]
unsafe fn bit_clr(r: *const MemRegion, i: usize) {
    unsafe {
        if !(*r).bm.is_null() {
            AtomicU8::from_ptr((*r).bm.add(i >> 3)).fetch_and(!(1u8 << (i & 7)), Ordering::Release);
        }
    }
}

#[inline(always)]
unsafe fn shared_load(r: *const MemRegion, i: usize) -> u8 {
    unsafe {
        if (*r).shared.is_null() {
            0
        } else {
            *(*r).shared.add(i)
        }
    }
}

#[inline(always)]
unsafe fn shared_store(r: *const MemRegion, i: usize, value: u8) {
    unsafe {
        if !(*r).shared.is_null() {
            *(*r).shared.add(i) = value;
        }
    }
}

#[inline(always)]
fn shared_slot_bit(gaddr: u64) -> u8 {
    (1u64 << ((gaddr & (OCERZ_HOST_PAGE - 1)) / OCERZ_GUEST_PAGE)) as u8
}

unsafe fn region_add(glo: u64, ghi: u64) -> *mut MemRegion {
    unsafe {
        if REGION_N.load(Ordering::Relaxed) as usize >= MEM_REGION_MAX {
            return ptr::null_mut();
        }
        let region_n = REGION_N.load(Ordering::Relaxed);
        let npages = (ghi - glo) / OCERZ_HOST_PAGE;
        let nslots = (ghi - glo) / OCERZ_GUEST_PAGE;
        let bm = libc::calloc(1, ((npages + 7) / 8) as usize) as *mut u8;
        let shared = libc::calloc(npages as usize, size_of::<u8>()) as *mut u8;
        let armed = libc::calloc(npages as usize, size_of::<u8>()) as *mut u8;
        let slots = libc::calloc(nslots as usize, size_of::<u32>()) as *mut u32;
        if bm.is_null() || shared.is_null() || armed.is_null() || slots.is_null() {
            libc::free(bm as *mut c_void);
            libc::free(shared as *mut c_void);
            libc::free(armed as *mut c_void);
            libc::free(slots as *mut c_void);
            return ptr::null_mut();
        }
        let rp = ((&raw mut REGIONS) as *mut MemRegion).add(region_n as usize);
        (*rp).glo = glo;
        (*rp).ghi = ghi;
        (*rp).bm = bm;
        (*rp).shared = shared;
        (*rp).armed = armed;
        (*rp).slots = slots;
        let result = rp;
        REGION_N.store(region_n + 1, Ordering::Release);
        result
    }
}

unsafe fn guest_range(gaddr: u64, len: u64, lo: *mut u64, hi: *mut u64) -> c_int {
    unsafe {
        if len == 0 || gaddr > u64::MAX - len {
            return 0;
        }
        let end = gaddr + len;
        if end > u64::MAX - (OCERZ_GUEST_PAGE - 1) {
            return 0;
        }
        *lo = guest_round_down(gaddr);
        *hi = guest_round_up(end);
        (*lo < *hi) as c_int
    }
}

unsafe fn allocation_guard_end(data_hi: u64, guard_hi: *mut u64) -> c_int {
    unsafe {
        if data_hi > u64::MAX - (OCERZ_HOST_PAGE - 1) {
            return 0;
        }
        let aligned = round_up(data_hi);
        if aligned > u64::MAX - OCERZ_HOST_PAGE {
            return 0;
        }
        *guard_hi = aligned + OCERZ_HOST_PAGE;
        1
    }
}

unsafe fn map_refuse(site: c_int, lo: u64, hi: u64, rc: c_int) -> c_int {
    unsafe {
        static mut LG: c_int = -1;
        if LG < 0 {
            LG = if libc::getenv(c"OCERZ_MAPFAILLOG".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if LG != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MAPFAIL[%d] site=%d range=[%#llx,%#llx) rc=%d\n".as_ptr(),
                libc::getpid() as c_int,
                site,
                lo as c_ulonglong,
                hi as c_ulonglong,
                rc,
            );
        }
        rc
    }
}

const PIN_MAX: usize = 256;
static mut G_PIN: [PinRange; PIN_MAX] = {
    const EMPTY: PinRange = PinRange { lo: 0, hi: 0 };
    [EMPTY; PIN_MAX]
};
static mut G_PIN_N: c_int = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_pin_map: *mut u8 = ptr::null_mut();

#[repr(C)]
#[derive(Copy, Clone)]
struct PinRange {
    lo: u64,
    hi: u64,
}

unsafe fn pinned_overlap(lo: u64, hi: u64) -> c_int {
    unsafe {
        for i in 0..G_PIN_N {
            let p = ((&raw const G_PIN) as *const PinRange).add(i as usize);
            if lo < (*p).hi && hi > (*p).lo {
                return 1;
            }
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_pinned(gaddr: u64, len: u64) -> c_int {
    unsafe { (G_PIN_N != 0 && len != 0 && pinned_overlap(gaddr, gaddr + len) != 0) as c_int }
}

/// Calls fn on each part of [lo, hi) outside the pinned ranges, in order, and
/// answers the first failure.
unsafe fn for_unpinned(
    mut lo: u64,
    hi: u64,
    f: unsafe fn(u64, u64, c_int) -> c_int,
    arg: c_int,
) -> c_int {
    unsafe {
        while lo < hi {
            let mut end = hi;
            let mut skip = 0u64;
            for i in 0..G_PIN_N {
                let p = &*((&raw const G_PIN) as *const PinRange).add(i as usize);
                if p.hi <= lo || p.lo >= hi {
                    continue;
                }
                if p.lo <= lo {
                    if p.hi > skip {
                        skip = p.hi;
                    }
                } else if p.lo < end {
                    end = p.lo;
                }
            }
            if skip != 0 {
                lo = skip;
                continue;
            }
            let rc = f(lo, end, arg);
            if rc != ffi::OCERZ_OK {
                return rc;
            }
            lo = end;
        }
        ffi::OCERZ_OK
    }
}

unsafe fn owner_create_locked(
    r: *const MemRegion,
    live_slots: u32,
    guard_lo: u64,
    guard_hi: u64,
) -> u32 {
    unsafe {
        let id: u32;
        if OWNER_FREE != 0 {
            id = OWNER_FREE;
            OWNER_FREE = (*OWNERS.add((id - 1) as usize)).next_free;
        } else {
            if OWNER_N == MEM_SLOT_OWNER_MASK {
                return 0;
            }
            if OWNER_N == OWNER_CAP {
                let mut new_cap = if OWNER_CAP != 0 { OWNER_CAP * 2 } else { 256 };
                if new_cap < OWNER_CAP || new_cap > MEM_SLOT_OWNER_MASK {
                    new_cap = MEM_SLOT_OWNER_MASK;
                }
                let grown = libc::realloc(
                    OWNERS as *mut c_void,
                    new_cap as usize * size_of::<MemOwner>(),
                ) as *mut MemOwner;
                if grown.is_null() {
                    return 0;
                }
                ptr::write_bytes(
                    grown.add(OWNER_CAP as usize),
                    0,
                    (new_cap - OWNER_CAP) as usize,
                );
                OWNERS = grown;
                OWNER_CAP = new_cap;
            }
            OWNER_N += 1;
            id = OWNER_N;
        }

        let owner = &mut *OWNERS.add((id - 1) as usize);
        owner.guard_lo = guard_lo;
        owner.guard_hi = guard_hi;
        owner.live_slots = live_slots;
        owner.next_free = 0;
        owner.region = ((r as usize - (&raw mut REGIONS) as usize) / size_of::<MemRegion>()) as u16;
        owner.active = 1;
        id
    }
}

unsafe fn owner_cancel_locked(id: u32) {
    unsafe {
        if id == 0 || id > OWNER_N || (*OWNERS.add((id - 1) as usize)).active == 0 {
            return;
        }
        (*OWNERS.add((id - 1) as usize)).active = 0;
        (*OWNERS.add((id - 1) as usize)).next_free = OWNER_FREE;
        OWNER_FREE = id;
    }
}

unsafe fn affected_include(lo: u64, hi: u64, affected_lo: *mut u64, affected_hi: *mut u64) {
    unsafe {
        if lo >= hi {
            return;
        }
        if *affected_lo > lo {
            *affected_lo = lo;
        }
        if *affected_hi < hi {
            *affected_hi = hi;
        }
    }
}

unsafe fn owner_retire_locked(id: u32, affected_lo: *mut u64, affected_hi: *mut u64) {
    unsafe {
        if id == 0 || id > OWNER_N {
            return;
        }
        let owner = &mut *OWNERS.add((id - 1) as usize);
        if owner.active == 0 {
            return;
        }
        let r = ((&raw mut REGIONS) as *mut MemRegion).add(owner.region as usize);
        let mut p = owner.guard_lo;
        while p < owner.guard_hi {
            let i = slot_index(r, p);
            let state = slot_load(r, i);
            if slot_owner(state) == id && (state & MEM_SLOT_GUARD) != 0 {
                slot_store(r, i, 0);
            }
            p += OCERZ_GUEST_PAGE;
        }
        affected_include(owner.guard_lo, owner.guard_hi, affected_lo, affected_hi);
        owner.active = 0;
        owner.next_free = OWNER_FREE;
        OWNER_FREE = id;
    }
}

unsafe fn release_slot_locked(
    r: *mut MemRegion,
    gaddr: u64,
    affected_lo: *mut u64,
    affected_hi: *mut u64,
) {
    unsafe {
        let i = slot_index(r, gaddr);
        let state = slot_load(r, i);
        let id = slot_owner(state);
        if id == 0 || (state & MEM_SLOT_GUARD) != 0 {
            return;
        }
        slot_store(r, i, 0);
        let page_i = pg_index(r, gaddr);
        let shared = shared_load(r, page_i);
        if shared & shared_slot_bit(gaddr) != 0 {
            shared_store(r, page_i, shared & !shared_slot_bit(gaddr));
        }
        affected_include(gaddr, gaddr + OCERZ_GUEST_PAGE, affected_lo, affected_hi);
        if id <= OWNER_N {
            let owner = &mut *OWNERS.add((id - 1) as usize);
            if owner.active != 0 && owner.live_slots != 0 {
                owner.live_slots -= 1;
                if owner.live_slots == 0 {
                    owner_retire_locked(id, affected_lo, affected_hi);
                }
            }
        }
    }
}

unsafe fn release_owner_run_locked(id: u32, n: u64, affected_lo: *mut u64, affected_hi: *mut u64) {
    unsafe {
        if id == 0 || id > OWNER_N {
            return;
        }
        let owner = &mut *OWNERS.add((id - 1) as usize);
        if owner.active == 0 || owner.live_slots == 0 {
            return;
        }
        owner.live_slots = if n >= owner.live_slots as u64 {
            0
        } else {
            owner.live_slots - n as u32
        };
        if owner.live_slots == 0 {
            owner_retire_locked(id, affected_lo, affected_hi);
        }
    }
}

unsafe fn release_range_locked(
    r: *mut MemRegion,
    lo: u64,
    hi: u64,
    affected_lo: *mut u64,
    affected_hi: *mut u64,
) {
    unsafe {
        if (*r).slots.is_null() {
            let mut p = lo;
            while p < hi {
                release_slot_locked(r, p, affected_lo, affected_hi);
                p += OCERZ_GUEST_PAGE;
            }
            return;
        }
        let slots = (*r).slots.add(slot_index(r, lo));
        let n = (hi - lo) / OCERZ_GUEST_PAGE;
        let mut run_id: u32 = 0;
        let mut run_n: u64 = 0;
        let mut first = u64::MAX;
        let mut last: u64 = 0;
        for k in 0..n {
            let state = *slots.add(k as usize);
            let id = slot_owner(state);
            if id == 0 || (state & MEM_SLOT_GUARD) != 0 {
                continue;
            }
            let p = lo + k * OCERZ_GUEST_PAGE;
            if !(*r).shared.is_null() {
                let page_i = pg_index(r, p);
                let shared = *(*r).shared.add(page_i);
                if shared & shared_slot_bit(p) != 0 {
                    shared_store(r, page_i, shared & !shared_slot_bit(p));
                }
            }
            *slots.add(k as usize) = 0;
            if first == u64::MAX {
                first = p;
            }
            last = p + OCERZ_GUEST_PAGE;
            if id != run_id {
                release_owner_run_locked(run_id, run_n, affected_lo, affected_hi);
                run_id = id;
                run_n = 0;
            }
            run_n += 1;
        }
        fence(Ordering::Release);
        if first < last {
            affected_include(first, last, affected_lo, affected_hi);
        }
        release_owner_run_locked(run_id, run_n, affected_lo, affected_hi);
    }
}

unsafe fn slots_are_free(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        if (*r).slots.is_null() || hi <= lo {
            return 1;
        }
        let s = (*r).slots.add(slot_index(r, lo));
        let n = ((guest_round_up(hi) - guest_round_down(lo)) / OCERZ_GUEST_PAGE) as usize;
        let mut any: u32 = 0;
        for k in 0..n {
            any |= *s.add(k);
        }
        (any == 0) as c_int
    }
}

unsafe fn host_pages_plain_locked(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        let i0 = pg_index(r, lo);
        let i1 = pg_index(r, hi);
        let mut any: u8 = 0;
        if !(*r).shared.is_null() {
            for i in i0..i1 {
                any |= *(*r).shared.add(i);
            }
        }
        if !(*r).armed.is_null() {
            for i in i0..i1 {
                any |= *(*r).armed.add(i);
            }
        }
        (any == 0) as c_int
    }
}

unsafe fn bits_set_range(r: *const MemRegion, i0: usize, i1: usize) {
    unsafe {
        if (*r).bm.is_null() {
            return;
        }
        let mut i = i0;
        while i < i1 && (i & 7) != 0 {
            bit_set(r, i);
            i += 1;
        }
        while i + 8 <= i1 {
            AtomicU8::from_ptr((*r).bm.add(i >> 3)).store(0xff, Ordering::Release);
            i += 8;
        }
        while i < i1 {
            bit_set(r, i);
            i += 1;
        }
    }
}

unsafe fn bits_clr_range(r: *const MemRegion, i0: usize, i1: usize) {
    unsafe {
        if (*r).bm.is_null() {
            return;
        }
        let mut i = i0;
        while i < i1 && (i & 7) != 0 {
            bit_clr(r, i);
            i += 1;
        }
        while i + 8 <= i1 {
            AtomicU8::from_ptr((*r).bm.add(i >> 3)).store(0, Ordering::Release);
            i += 8;
        }
        while i < i1 {
            bit_clr(r, i);
            i += 1;
        }
    }
}

unsafe fn install_fresh_locked(
    r: *mut MemRegion,
    lo: u64,
    hi: u64,
    guard_hi: u64,
    prot: c_int,
    owner_out: *mut u32,
) -> c_int {
    unsafe {
        let dhi = round_up(hi);
        if lo != round_down(lo)
            || guard_hi != round_down(guard_hi)
            || dhi > guard_hi
            || host_pages_plain_locked(r, lo, guard_hi) == 0
        {
            return -1;
        }
        let mut p = dhi;
        while p < guard_hi {
            if bit_test(r, pg_index(r, p)) != 0 {
                return -1;
            }
            p += OCERZ_HOST_PAGE;
        }
        let nslots = (hi - lo) / OCERZ_GUEST_PAGE;
        let owner = owner_create_locked(r, nslots as u32, hi, guard_hi);
        if owner == 0 {
            return -1;
        }
        let hp = ocerz_g2h(lo);
        if libc::mmap(
            hp,
            (dhi - lo) as usize,
            host_prot(prot),
            libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
            -1,
            0,
        ) != hp
        {
            owner_cancel_locked(owner);
            return -1;
        }
        bits_set_range(r, pg_index(r, lo), pg_index(r, dhi));
        if !(*r).slots.is_null() {
            let s = (*r).slots.add(slot_index(r, lo));
            let v = slot_data_state(owner, prot);
            for k in 0..nslots {
                *s.add(k as usize) = v;
            }
            let ng = (guard_hi - hi) / OCERZ_GUEST_PAGE;
            for k in 0..ng {
                *s.add((nslots + k) as usize) = owner | MEM_SLOT_GUARD;
            }
            fence(Ordering::Release);
        }
        if !owner_out.is_null() {
            *owner_out = owner;
        }
        0
    }
}

unsafe fn claim_slots_locked(
    r: *mut MemRegion,
    lo: u64,
    hi: u64,
    guard_hi: u64,
    prot: c_int,
    affected_lo: *mut u64,
    affected_hi: *mut u64,
) -> u32 {
    unsafe {
        if slots_are_free(r, lo, guard_hi) == 0 {
            return 0;
        }
        let nslots64 = (hi - lo) / OCERZ_GUEST_PAGE;
        if nslots64 == 0 || nslots64 > u32::MAX as u64 {
            return 0;
        }
        let id = owner_create_locked(r, nslots64 as u32, hi, guard_hi);
        if id == 0 {
            return 0;
        }
        let mut p = lo;
        while p < hi {
            slot_store(r, slot_index(r, p), slot_data_state(id, prot));
            p += OCERZ_GUEST_PAGE;
        }
        let mut p = hi;
        while p < guard_hi {
            slot_store(r, slot_index(r, p), id | MEM_SLOT_GUARD);
            p += OCERZ_GUEST_PAGE;
        }
        affected_include(lo, guard_hi, affected_lo, affected_hi);
        id
    }
}

unsafe fn host_make_writable(hp: *mut c_void) -> c_int {
    unsafe {
        if libc::mprotect(hp, OCERZ_HOST_PAGE as usize, PROT_READ | PROT_WRITE) == 0 {
            return 0;
        }
        if libc::mmap(
            hp,
            OCERZ_HOST_PAGE as usize,
            PROT_READ | PROT_WRITE,
            libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
            -1,
            0,
        ) == hp
        {
            0
        } else {
            -1
        }
    }
}

unsafe fn memlog(op: *const c_char, gaddr: u64, len: u64, prot: c_int) {
    unsafe {
        static mut INIT: c_int = 0;
        static mut PROBE: u64 = 0;
        static mut LOWLOG: u64 = 0;
        if INIT == 0 {
            INIT = 1;
            let e = libc::getenv(c"OCERZ_MEMLOG".as_ptr());
            PROBE = if e.is_null() {
                0
            } else {
                libc::strtoull(e, ptr::null_mut(), 0)
            };
            let l = libc::getenv(c"OCERZ_LOWLOG".as_ptr());
            LOWLOG = if l.is_null() {
                0
            } else {
                libc::strtoull(l, ptr::null_mut(), 0)
            };
        }
        if PROBE == 0 && LOWLOG == 0 {
            return;
        }
        let lo = round_down(gaddr);
        let hi = round_up(gaddr + len);
        let ppg = round_down(PROBE);
        let hit_probe = PROBE != 0 && ppg >= lo && ppg < hi;
        let hit_low = LOWLOG != 0 && lo < LOWLOG && hi > 0x100000;
        if !hit_probe && !hit_low {
            return;
        }
        let rip = ocerz_current_guest_rip();
        let mut ibase: u64 = 0;
        let iname = if rip != 0 {
            ffi::ocerz_dyld_name_for_addr(rip, &mut ibase)
        } else {
            ptr::null()
        };

        let sane = !iname.is_null() && (rip - ibase) < 0x8000000;
        let cmp = if hit_probe { PROBE } else { gaddr };
        libc::fprintf(crate::log::stderr(),
            c"ocerz: MEMLOG[%d/%04lx] %-7s gaddr=%#llx len=%#llx prot=%d rip=%#llx (%s+%#llx) -> probe(%#llx)committed=%d\n".as_ptr(),
            libc::getpid() as c_int,
            ((libc::pthread_self() as usize as u64) >> 8) as c_ulong & 0xffff,
            op, gaddr as c_ulonglong, len as c_ulonglong, prot,
            rip as c_ulonglong,
            if sane { iname } else { c"<shared-cache>".as_ptr() },
            (if sane { rip - ibase } else { 0 }) as c_ulonglong,
            cmp as c_ulonglong, ocerz_addr_committed(cmp));

        if *op == b'u' as c_char && !sane {
            let sp = ocerz_current_guest_rsp();
            let mut shown = 0;
            let mut i = 0;
            while sp != 0 && i < 160 && shown < 6 {
                let a = sp + i * 8;
                if ocerz_addr_readable(a) == 0 {
                    break;
                }
                let v = *(ocerz_g2h(a) as *const u64);
                if v < 0x300000000 {
                    i += 1;
                    continue;
                }
                let mut b: u64 = 0;
                let n = ffi::ocerz_dyld_name_for_addr(v, &mut b);
                if !n.is_null() && (v - b) < 0x8000000 {
                    let bn = libc::strrchr(n, b'/' as c_int);
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz:   stk[+%#x]=%#llx (%s+%#llx)\n".as_ptr(),
                        (i * 8) as c_uint,
                        v as c_ulonglong,
                        if !bn.is_null() {
                            bn.add(1) as *const c_char
                        } else {
                            n
                        },
                        (v - b) as c_ulonglong,
                    );
                    shown += 1;
                }
                i += 1;
            }
        }
    }
}

unsafe fn ocerz_no_batch_vm() -> c_int {
    unsafe {
        static mut V: c_int = -1;
        if V < 0 {
            V = if libc::getenv(c"OCERZ_NO_BATCH_VM".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        V
    }
}

unsafe fn commit_range(
    r: *const MemRegion,
    lo: u64,
    hi: u64,
    hprot: c_int,
    zlo: u64,
    zhi: u64,
) -> c_int {
    unsafe {
        let mut needs_overlap_zero = false;
        if zlo < zhi {
            let mut p = lo;
            while p < hi {
                if bit_test(r, pg_index(r, p)) != 0 {
                    needs_overlap_zero = true;
                    break;
                }
                p += OCERZ_HOST_PAGE;
            }
        }
        if ocerz_no_batch_vm() == 0
            && !needs_overlap_zero
            && hi > lo
            && libc::mprotect(ocerz_g2h(lo), (hi - lo) as usize, hprot) == 0
        {
            let mut p = lo;
            while p < hi {
                bit_set(r, pg_index(r, p));
                if !(*r).armed.is_null() && *(*r).armed.add(pg_index(r, p)) != 0 {
                    libc::mprotect(ocerz_g2h(p), OCERZ_HOST_PAGE as usize, hprot & !PROT_WRITE);
                }
                p += OCERZ_HOST_PAGE;
            }
            return ffi::OCERZ_OK;
        }
        let blo = round_up(if zlo > lo { zlo } else { lo });
        let mut bhi = round_down(if zhi < hi { zhi } else { hi });
        if ocerz_no_batch_vm() != 0 {
            bhi = blo;
        }
        let mut p = blo;
        while p < bhi {
            if shared_load(r, pg_index(r, p)) & MEM_SHARED_PHYSICAL != 0 {
                bhi = blo;
                break;
            }
            p += OCERZ_HOST_PAGE;
        }
        if blo < bhi {
            let bp = ocerz_g2h(blo);
            if libc::mmap(
                bp,
                (bhi - blo) as usize,
                hprot,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) != bp
            {
                return ffi::OCERZ_ENOMEM;
            }
            let mut p = blo;
            while p < bhi {
                bit_set(r, pg_index(r, p));
                if !(*r).armed.is_null() && *(*r).armed.add(pg_index(r, p)) != 0 {
                    *(*r).armed.add(pg_index(r, p)) = 0;
                    G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                }
                p += OCERZ_HOST_PAGE;
            }
        }

        let mut p = lo;
        while p < hi {
            if p >= blo && p < bhi {
                p += OCERZ_HOST_PAGE;
                continue;
            }
            let i = pg_index(r, p);
            let hp = ocerz_g2h(p);
            let committed = bit_test(r, i) != 0;
            let physical = (shared_load(r, i) & MEM_SHARED_PHYSICAL) != 0;
            let mlo = if p > zlo { p } else { zlo };
            let mhi = if p + OCERZ_HOST_PAGE < zhi {
                p + OCERZ_HOST_PAGE
            } else {
                zhi
            };
            let padded = (shared_load(r, i) & MEM_SHARED_PADDED) != 0;
            if committed && mlo < mhi && !padded {
                let writable_failed = if physical {
                    libc::mprotect(hp, OCERZ_HOST_PAGE as usize, PROT_READ | PROT_WRITE) != 0
                } else {
                    host_make_writable(hp) != 0
                };
                if writable_failed {
                    return ffi::OCERZ_ENOMEM;
                }
                ptr::write_bytes(ocerz_g2h(mlo) as *mut u8, 0, (mhi - mlo) as usize);
            }
            if libc::mprotect(hp, OCERZ_HOST_PAGE as usize, hprot) != 0 {
                if physical
                    || libc::mmap(
                        hp,
                        OCERZ_HOST_PAGE as usize,
                        hprot,
                        libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                        -1,
                        0,
                    ) != hp
                {
                    return ffi::OCERZ_ENOMEM;
                }
                if !(*r).armed.is_null() && *(*r).armed.add(i) != 0 {
                    *(*r).armed.add(i) = 0;
                    G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                }
            } else if !(*r).armed.is_null() && *(*r).armed.add(i) != 0 {
                libc::mprotect(hp, OCERZ_HOST_PAGE as usize, hprot & !PROT_WRITE);
            }
            if !committed {
                bit_set(r, i);
            }
            p += OCERZ_HOST_PAGE;
        }
        ffi::OCERZ_OK
    }
}

unsafe fn host_page_guest_prot(r: *const MemRegion, page: u64, has_data: *mut c_int) -> c_int {
    unsafe {
        let mut prot = 0;
        *has_data = 0;
        let mut p = page;
        while p < page + OCERZ_HOST_PAGE {
            let state = slot_load(r, slot_index(r, p));
            if slot_is_data(state) {
                *has_data = 1;
                prot |= ((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_int;
            }
            p += OCERZ_GUEST_PAGE;
        }
        prot
    }
}

unsafe fn sync_host_page_locked(r: *const MemRegion, page: u64) -> c_int {
    unsafe {
        let i = pg_index(r, page);
        let mut has_data: c_int = 0;
        let prot = page_host_prot(r, i, host_page_guest_prot(r, page, &mut has_data));
        if has_data == 0 && !(*r).armed.is_null() && *(*r).armed.add(i) != 0 {
            *(*r).armed.add(i) = 0;
            G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
        }
        let hp = ocerz_g2h(page);
        let shared = shared_load(r, i);
        if (shared & MEM_SHARED_PHYSICAL) != 0 && (shared & MEM_SHARED_SLOT_MASK) == 0 {
            if libc::mmap(
                hp,
                OCERZ_HOST_PAGE as usize,
                prot,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) != hp
            {
                return ffi::OCERZ_ENOMEM;
            }
            shared_store(r, i, 0);
            if has_data != 0 {
                bit_set(r, i);
            } else {
                bit_clr(r, i);
            }
            return ffi::OCERZ_OK;
        }
        if has_data == 0 {
            if bit_test(r, i) == 0 {
                return ffi::OCERZ_OK;
            }
            if libc::mmap(
                hp,
                OCERZ_HOST_PAGE as usize,
                PROT_NONE,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) != hp
            {
                return ffi::OCERZ_ENOMEM;
            }
            bit_clr(r, i);
            return ffi::OCERZ_OK;
        }
        if libc::mprotect(hp, OCERZ_HOST_PAGE as usize, prot) != 0 {
            if shared & MEM_SHARED_PHYSICAL != 0 {
                return ffi::OCERZ_ENOMEM;
            }
            if libc::mmap(
                hp,
                OCERZ_HOST_PAGE as usize,
                prot,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) != hp
            {
                return ffi::OCERZ_ENOMEM;
            }
        }
        bit_set(r, i);
        ffi::OCERZ_OK
    }
}

unsafe fn sync_host_gap_locked(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        if hi - lo > OCERZ_HOST_PAGE
            && libc::mmap(
                ocerz_g2h(lo),
                (hi - lo) as usize,
                PROT_NONE,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) == ocerz_g2h(lo)
        {
            let mut q = lo;
            while q < hi {
                let i = pg_index(r, q);
                if !(*r).armed.is_null() && *(*r).armed.add(i) != 0 {
                    *(*r).armed.add(i) = 0;
                    G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                }
                bit_clr(r, i);
                q += OCERZ_HOST_PAGE;
            }
            return ffi::OCERZ_OK;
        }
        let mut rc = ffi::OCERZ_OK;
        let mut q = lo;
        while q < hi {
            if sync_host_page_locked(r, q) != ffi::OCERZ_OK {
                rc = ffi::OCERZ_ENOMEM;
            }
            q += OCERZ_HOST_PAGE;
        }
        rc
    }
}

unsafe fn sync_host_range_locked(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        if lo >= hi {
            return ffi::OCERZ_OK;
        }
        let lo = round_down(lo);
        let hi = round_up(hi);
        if hi - lo > 2 * OCERZ_HOST_PAGE
            && slots_are_free(r, lo, hi) != 0
            && host_pages_plain_locked(r, lo, hi) != 0
            && libc::mmap(
                ocerz_g2h(lo),
                (hi - lo) as usize,
                PROT_NONE,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) == ocerz_g2h(lo)
        {
            bits_clr_range(r, pg_index(r, lo), pg_index(r, hi));
            return ffi::OCERZ_OK;
        }
        let mut rc = ffi::OCERZ_OK;
        let mut p = lo;
        while p < hi {
            let shared = shared_load(r, pg_index(r, p));
            if (shared & MEM_SHARED_PHYSICAL) != 0
                && (shared & MEM_SHARED_SLOT_MASK) == 0
                && sync_host_page_locked(r, p) != ffi::OCERZ_OK
            {
                rc = ffi::OCERZ_ENOMEM;
            }
            p += OCERZ_HOST_PAGE;
        }
        let mut run_lo: u64 = 0;
        let mut run_prot: c_int = 0;
        let mut have_run = false;
        let mut gap_lo: u64 = 0;
        let mut have_gap = false;
        let mut p = lo;
        while p <= hi {
            let mut has_data: c_int = 0;
            let mut prot: c_int = 0;
            if p < hi {
                prot = page_host_prot(r, pg_index(r, p), host_page_guest_prot(r, p, &mut has_data));
            }
            let in_gap = p < hi && has_data == 0 && bit_test(r, pg_index(r, p)) != 0;
            if have_gap && !in_gap {
                if sync_host_gap_locked(r, gap_lo, p) != ffi::OCERZ_OK {
                    rc = ffi::OCERZ_ENOMEM;
                }
                have_gap = false;
            }
            if have_run && (p == hi || has_data == 0 || prot != run_prot) {
                if libc::mprotect(ocerz_g2h(run_lo), (p - run_lo) as usize, run_prot) == 0 {
                    let mut q = run_lo;
                    while q < p {
                        bit_set(r, pg_index(r, q));
                        q += OCERZ_HOST_PAGE;
                    }
                } else {
                    let mut q = run_lo;
                    while q < p {
                        if sync_host_page_locked(r, q) != ffi::OCERZ_OK {
                            rc = ffi::OCERZ_ENOMEM;
                        }
                        q += OCERZ_HOST_PAGE;
                    }
                }
                have_run = false;
            }
            if p == hi {
                break;
            }
            if has_data != 0 {
                if !have_run {
                    run_lo = p;
                    run_prot = prot;
                    have_run = true;
                }
                p += OCERZ_HOST_PAGE;
                continue;
            }
            if in_gap && !have_gap {
                gap_lo = p;
                have_gap = true;
            }
            p += OCERZ_HOST_PAGE;
        }
        rc
    }
}

unsafe fn shared_replacement_allowed_locked(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        let mut page = round_down(lo);
        while page < round_up(hi) {
            let shared = shared_load(r, pg_index(r, page));
            if shared & MEM_SHARED_PHYSICAL == 0 {
                page += OCERZ_HOST_PAGE;
                continue;
            }
            if lo <= page && hi >= page + OCERZ_HOST_PAGE {
                page += OCERZ_HOST_PAGE;
                continue;
            }
            let first = if lo > page { lo } else { page };
            let end = if hi < page + OCERZ_HOST_PAGE {
                hi
            } else {
                page + OCERZ_HOST_PAGE
            };
            let mut replaced: u8 = 0;
            let mut p = first;
            while p < end {
                replaced |= shared_slot_bit(p);
                p += OCERZ_GUEST_PAGE;
            }
            if shared & MEM_SHARED_PADDED != 0 {
                if shared & replaced != 0 {
                    return 0;
                }
                page += OCERZ_HOST_PAGE;
                continue;
            }
            if (shared & MEM_SHARED_SLOT_MASK & !replaced) != 0 {
                return 0;
            }
            page += OCERZ_HOST_PAGE;
        }
        1
    }
}

unsafe fn preserves_padded_page_locked(r: *const MemRegion, page: u64, lo: u64, hi: u64) -> c_int {
    unsafe {
        let shared = shared_load(r, pg_index(r, page));
        (((shared & (MEM_SHARED_PHYSICAL | MEM_SHARED_PADDED))
            == (MEM_SHARED_PHYSICAL | MEM_SHARED_PADDED))
            && (lo > page || hi < page + OCERZ_HOST_PAGE)) as c_int
    }
}

unsafe fn update_padded_slots_locked(r: *const MemRegion, lo: u64, hi: u64, add: c_int) {
    unsafe {
        let mut page = round_down(lo);
        while page < round_up(hi) {
            if preserves_padded_page_locked(r, page, lo, hi) != 0 {
                let i = pg_index(r, page);
                let shared = shared_load(r, i);
                let first = if lo > page { lo } else { page };
                let end = if hi < page + OCERZ_HOST_PAGE {
                    hi
                } else {
                    page + OCERZ_HOST_PAGE
                };
                let mut mask: u8 = 0;
                let mut p = first;
                while p < end {
                    mask |= shared_slot_bit(p);
                    p += OCERZ_GUEST_PAGE;
                }
                shared_store(
                    r,
                    i,
                    if add != 0 {
                        shared | mask
                    } else {
                        shared & !mask
                    },
                );
            }
            page += OCERZ_HOST_PAGE;
        }
    }
}

unsafe fn detach_replaced_shared_pages_locked(r: *const MemRegion, lo: u64, hi: u64) -> c_int {
    unsafe {
        let mut page = round_down(lo);
        while page < round_up(hi) {
            let i = pg_index(r, page);
            if (shared_load(r, i) & MEM_SHARED_PHYSICAL) != 0
                && preserves_padded_page_locked(r, page, lo, hi) == 0
            {
                let hp = ocerz_g2h(page);
                if libc::mmap(
                    hp,
                    OCERZ_HOST_PAGE as usize,
                    PROT_READ | PROT_WRITE,
                    libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                    -1,
                    0,
                ) != hp
                {
                    return ffi::OCERZ_ENOMEM;
                }
                shared_store(r, i, 0);
                bit_set(r, i);
            }
            page += OCERZ_HOST_PAGE;
        }
        ffi::OCERZ_OK
    }
}

unsafe fn install_mapping_locked(
    r: *mut MemRegion,
    lo: u64,
    hi: u64,
    guard_hi: u64,
    prot: c_int,
    replace: c_int,
    zero_lo: u64,
    zero_hi: u64,
    owner_out: *mut u32,
) -> c_int {
    unsafe {
        if r.is_null() || lo < (*r).glo || lo >= hi || guard_hi < hi || guard_hi > (*r).ghi {
            return map_refuse(3, lo, hi, ffi::OCERZ_ENOMEM);
        }
        let nslots64 = (hi - lo) / OCERZ_GUEST_PAGE;
        if nslots64 == 0 || nslots64 > u32::MAX as u64 {
            return map_refuse(4, lo, hi, ffi::OCERZ_ENOMEM);
        }
        if (replace == 0 && slots_are_free(r, lo, guard_hi) == 0)
            || (replace != 0 && guard_hi > hi && slots_are_free(r, hi, guard_hi) == 0)
        {
            return map_refuse(5, lo, hi, ffi::OCERZ_ENOMEM);
        }
        if shared_replacement_allowed_locked(r, lo, hi) == 0 {
            return map_refuse(6, lo, hi, ffi::OCERZ_EUNSUP);
        }
        if (replace == 0 || slots_are_free(r, lo, guard_hi) != 0)
            && install_fresh_locked(r, lo, hi, guard_hi, prot, owner_out) == 0
        {
            return ffi::OCERZ_OK;
        }

        let mut old_states: *mut u32 = ptr::null_mut();
        if replace != 0 {
            old_states = libc::malloc(nslots64 as usize * size_of::<u32>()) as *mut u32;
            if old_states.is_null() {
                return map_refuse(7, lo, hi, ffi::OCERZ_ENOMEM);
            }
            for slot in 0..nslots64 {
                *old_states.add(slot as usize) =
                    slot_load(r, slot_index(r, lo + slot * OCERZ_GUEST_PAGE));
            }
        }

        let owner = owner_create_locked(r, nslots64 as u32, hi, guard_hi);
        if owner == 0 {
            libc::free(old_states as *mut c_void);
            return map_refuse(8, lo, hi, ffi::OCERZ_ENOMEM);
        }

        let mut rc = detach_replaced_shared_pages_locked(r, lo, hi);
        if rc != ffi::OCERZ_OK {
            owner_cancel_locked(owner);
            libc::free(old_states as *mut c_void);
            return map_refuse(9, lo, hi, rc);
        }

        let mut prepare_prot = host_prot(prot);
        let mut page = round_down(lo);
        while page < round_up(hi) {
            let mut has_data: c_int = 0;
            prepare_prot |= host_prot(host_page_guest_prot(r, page, &mut has_data));
            page += OCERZ_HOST_PAGE;
        }
        rc = commit_range(
            r,
            round_down(lo),
            round_up(hi),
            prepare_prot,
            zero_lo,
            zero_hi,
        );
        if rc != ffi::OCERZ_OK {
            owner_cancel_locked(owner);
            sync_host_range_locked(r, lo, hi);
            libc::free(old_states as *mut c_void);
            return map_refuse(10, lo, hi, rc);
        }

        let mut affected_lo = u64::MAX;
        let mut affected_hi: u64 = 0;
        affected_include(lo, guard_hi, &mut affected_lo, &mut affected_hi);
        for slot in 0..nslots64 {
            let p = lo + slot * OCERZ_GUEST_PAGE;
            if replace != 0 && slot_is_data(*old_states.add(slot as usize)) {
                let old_owner = slot_owner(*old_states.add(slot as usize));
                if old_owner <= OWNER_N
                    && (*OWNERS.add((old_owner - 1) as usize)).active != 0
                    && (*OWNERS.add((old_owner - 1) as usize)).live_slots != 0
                {
                    (*OWNERS.add((old_owner - 1) as usize)).live_slots -= 1;
                }
            }
            slot_store(r, slot_index(r, p), slot_data_state(owner, prot));
        }
        let mut p = hi;
        while p < guard_hi {
            slot_store(r, slot_index(r, p), owner | MEM_SLOT_GUARD);
            p += OCERZ_GUEST_PAGE;
        }

        update_padded_slots_locked(r, lo, hi, 1);

        rc = sync_host_range_locked(r, affected_lo, affected_hi);
        if rc != ffi::OCERZ_OK {
            update_padded_slots_locked(r, lo, hi, 0);
            let mut p = hi;
            while p < guard_hi {
                let i = slot_index(r, p);
                if slot_owner(slot_load(r, i)) == owner {
                    slot_store(r, i, 0);
                }
                p += OCERZ_GUEST_PAGE;
            }
            for slot in 0..nslots64 {
                let p = lo + slot * OCERZ_GUEST_PAGE;
                if replace != 0 {
                    slot_store(r, slot_index(r, p), *old_states.add(slot as usize));
                    if slot_is_data(*old_states.add(slot as usize)) {
                        let old_owner = slot_owner(*old_states.add(slot as usize));
                        if old_owner <= OWNER_N
                            && (*OWNERS.add((old_owner - 1) as usize)).active != 0
                        {
                            (*OWNERS.add((old_owner - 1) as usize)).live_slots += 1;
                        }
                    }
                } else {
                    slot_store(r, slot_index(r, p), 0);
                }
            }
            owner_cancel_locked(owner);
            sync_host_range_locked(r, affected_lo, affected_hi);
            libc::free(old_states as *mut c_void);
            return map_refuse(11, lo, hi, rc);
        }

        if replace != 0 {
            for slot in 0..nslots64 {
                let old_owner = slot_owner(*old_states.add(slot as usize));
                if slot_is_data(*old_states.add(slot as usize))
                    && old_owner <= OWNER_N
                    && (*OWNERS.add((old_owner - 1) as usize)).active != 0
                    && (*OWNERS.add((old_owner - 1) as usize)).live_slots == 0
                {
                    owner_retire_locked(old_owner, &mut affected_lo, &mut affected_hi);
                }
            }
            sync_host_range_locked(r, affected_lo, affected_hi);
        }
        libc::free(old_states as *mut c_void);
        if !owner_out.is_null() {
            *owner_out = owner;
        }
        ffi::OCERZ_OK
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_commit_fault_page(gaddr: u64) -> c_int {
    unsafe {
        let p = gaddr & !(OCERZ_HOST_PAGE - 1);
        let r = region_for_range(p, p + OCERZ_HOST_PAGE);
        if r.is_null() {
            return 0;
        }
        let state = slot_load(r, slot_index(r, gaddr));
        if !slot_is_data(state) {
            return 0;
        }
        let i = pg_index(r, p);
        if bit_test(r, i) != 0 {
            return 1;
        }
        let hp = ocerz_g2h(p);
        bit_set(r, i);
        if libc::mprotect(hp, OCERZ_HOST_PAGE as usize, PROT_READ | PROT_WRITE) != 0
            && libc::mmap(
                hp,
                OCERZ_HOST_PAGE as usize,
                PROT_READ | PROT_WRITE,
                libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                -1,
                0,
            ) != hp
        {
            bit_clr(r, i);
            return 0;
        }
        if slot_load(r, slot_index(r, gaddr)) != state {
            let mut has_data: c_int = 0;
            let prot = host_prot(host_page_guest_prot(r, p, &mut has_data));
            if has_data == 0 {
                libc::mmap(
                    hp,
                    OCERZ_HOST_PAGE as usize,
                    PROT_NONE,
                    libc::MAP_ANON | libc::MAP_PRIVATE | libc::MAP_FIXED,
                    -1,
                    0,
                );
                bit_clr(r, i);
            } else {
                libc::mprotect(hp, OCERZ_HOST_PAGE as usize, prot);
                bit_set(r, i);
            }
            return 0;
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_init(lo: u64, hi: u64) -> c_int {
    unsafe {
        let len = (hi - lo) as usize;
        let p = libc::mmap(
            ptr::null_mut(),
            len,
            PROT_NONE,
            libc::MAP_PRIVATE | libc::MAP_ANON,
            -1,
            0,
        );
        if p == libc::MAP_FAILED {
            ocerz_fatal!("could not reserve a %#zx-byte guest arena\n", len);
            return ffi::OCERZ_ENOMEM;
        }
        let host_base = p as u64;
        ocerz_guest_base = host_base - lo;
        ocerz_arena_lo = lo;
        ocerz_arena_hi = hi;
        ALLOC_FLOOR = lo + (len / 4) as u64;
        BUMP_NEXT = ALLOC_FLOOR;
        if region_add(lo, hi).is_null() {
            return ffi::OCERZ_ENOMEM;
        }
        ocerz_log!(
            "guest arena [%#llx, %#llx) -> host %#llx, guest_base %#llx\n",
            lo as c_ulonglong,
            hi as c_ulonglong,
            host_base as c_ulonglong,
            ocerz_guest_base as c_ulonglong
        );
        ocerz_init_gate_arm();
        ffi::OCERZ_OK
    }
}

#[unsafe(no_mangle)]
pub static mut ocerz_wine_process: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_init_identity(size: u64) -> c_int {
    unsafe {
        let mut p = libc::MAP_FAILED;
        if ocerz_wine_process != 0 {
            p = libc::mmap(
                OCERZ_WINE_ARENA_BASE as *mut c_void,
                size as usize,
                PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            );
            if p != libc::MAP_FAILED && p as u64 != OCERZ_WINE_ARENA_BASE {
                libc::munmap(p, size as usize);
                p = libc::MAP_FAILED;
            }
        }
        if p == libc::MAP_FAILED {
            p = libc::mmap(
                OCERZ_LOW_LIMIT as *mut c_void,
                size as usize,
                PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            );
        }
        if p == libc::MAP_FAILED {
            ocerz_fatal!(
                "could not reserve a %#llx-byte identity arena\n",
                size as c_ulonglong
            );
            return ffi::OCERZ_ENOMEM;
        }
        let lo = p as u64;
        if lo < OCERZ_LOW_LIMIT {
            ocerz_fatal!(
                "identity arena landed at %#llx, below the low-shadow limit\n",
                lo as c_ulonglong
            );
            return ffi::OCERZ_ENOMEM;
        }
        ocerz_guest_base = 0;
        ocerz_arena_lo = lo;
        ocerz_arena_hi = lo + size;

        let mut loader_reserve = size / 4;
        if loader_reserve > (1u64 << 30) {
            loader_reserve = 1u64 << 30;
        }
        ALLOC_FLOOR = lo + loader_reserve;
        BUMP_NEXT = ALLOC_FLOOR;
        if region_add(lo, lo + size).is_null() {
            return ffi::OCERZ_ENOMEM;
        }
        ocerz_log!(
            "identity guest arena [%#llx, %#llx), bump %#llx, guest_base 0\n",
            lo as c_ulonglong,
            ocerz_arena_hi as c_ulonglong,
            BUMP_NEXT as c_ulonglong
        );
        ocerz_init_gate_arm();
        ffi::OCERZ_OK
    }
}

unsafe fn find_free_span_locked(mut start: u64, mut end: u64, glen: u64, align: u64) -> u64 {
    unsafe {
        if start < ALLOC_FLOOR {
            start = ALLOC_FLOOR;
        }
        if end > ocerz_arena_hi {
            end = ocerz_arena_hi;
        }
        if glen == 0 || glen > u64::MAX - OCERZ_HOST_PAGE {
            return 0;
        }
        if start >= end {
            return 0;
        }

        let r = region_for_range(ALLOC_FLOOR, ocerz_arena_hi);
        if r.is_null() {
            return 0;
        }

        if start > u64::MAX - (align - 1) {
            return 0;
        }
        let mut candidate = align_up(start, align);
        while candidate < end {
            if glen > end - candidate {
                break;
            }
            let data_hi = candidate + glen;
            let mut guard_hi: u64 = 0;
            if allocation_guard_end(data_hi, &mut guard_hi) == 0 || guard_hi > end {
                break;
            }

            let mut occupied = 0u64;
            let mut p = candidate;
            while p < guard_hi {
                if slot_load(r, slot_index(r, p)) != 0 {
                    occupied = p;
                    break;
                }
                p += OCERZ_GUEST_PAGE;
            }
            if occupied == 0 {
                return candidate;
            }

            if occupied > u64::MAX - OCERZ_GUEST_PAGE - (align - 1) {
                break;
            }
            candidate = align_up(occupied + OCERZ_GUEST_PAGE, align);
        }
        0
    }
}

unsafe fn find_anywhere_locked(glen: u64, align: u64) -> u64 {
    unsafe {
        let mut start = BUMP_NEXT;
        if start < ALLOC_FLOOR || start >= ocerz_arena_hi {
            start = ALLOC_FLOOR;
        }

        let mut gaddr = find_free_span_locked(start, ocerz_arena_hi, glen, align);
        if gaddr == 0 && start > ALLOC_FLOOR {
            gaddr = find_free_span_locked(ALLOC_FLOOR, start, glen, align);
        }
        gaddr
    }
}

static RESVLOG_FLAG: AtomicI32 = AtomicI32::new(-1);
static RESVLOG_N: AtomicU64 = AtomicU64::new(0);

unsafe fn reserve_host_fixed(base: u64, size: u64) -> u64 {
    unsafe {
        let mut addr: MachVmAddress = base;
        let kr = mach_vm_allocate(mach_task_self(), &mut addr, size, VM_FLAGS_FIXED);
        if kr != KERN_SUCCESS {
            let mut rlog = RESVLOG_FLAG.load(Ordering::Relaxed);
            if rlog < 0 {
                rlog = if libc::getenv(c"OCERZ_RESVLOG".as_ptr()).is_null() {
                    0
                } else {
                    1
                };
                RESVLOG_FLAG.store(rlog, Ordering::Relaxed);
            }
            let c = RESVLOG_N.fetch_add(1, Ordering::Relaxed) + 1;
            if rlog != 0 && ((c & 0x3ff) == 0 || c < 8) {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: RESV-FAIL n=%llu base=%#llx size=%#llx kr=%d\n".as_ptr(),
                    c,
                    base as c_ulonglong,
                    size as c_ulonglong,
                    kr as c_int,
                );
            }
            return 0;
        }
        if libc::mprotect(addr as usize as *mut c_void, size as usize, PROT_NONE) != 0 {
            mach_vm_deallocate(mach_task_self(), addr, size);
            return 0;
        }
        addr
    }
}

static mut LOW_HOLE_FILL_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_low_fill_host_holes() {
    unsafe {
        static mut OFF: c_int = -1;
        if OFF < 0 {
            OFF = if libc::getenv(c"OCERZ_NO_LOW_HOLE_FILL".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if OFF != 0 || ocerz_low_base == 0 {
            return;
        }
        libc::pthread_mutex_lock(&raw mut LOW_HOLE_FILL_LOCK);
        let mut a: u64 = 0x100000000;
        let mut filled: u64 = 0;
        while a < OCERZ_LOW_LIMIT {
            let mut addr: MachVmAddress = a;
            let mut size: MachVmSize = 0;
            let mut depth: Natural = 0;
            let mut info = VmRegionSubmapShortInfo64 {
                protection: 0,
                max_protection: 0,
                inheritance: 0,
                offset: 0,
                _pad0: 0,
                user_tag: 0,
                ref_count: 0,
                shadow_depth: 0,
                external_pager: 0,
                share_mode: 0,
                is_submap: 0,
                behavior: 0,
                object_id: 0,
                user_wired_count: 0,
                flags: 0,
            };
            let mut cnt: MachMsgTypeNumber = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            let kr = mach_vm_region_recurse(
                mach_task_self(),
                &mut addr,
                &mut size,
                &mut depth,
                &mut info as *mut _ as *mut c_void,
                &mut cnt,
            );
            let next = if kr == KERN_SUCCESS && addr < OCERZ_LOW_LIMIT {
                addr
            } else {
                OCERZ_LOW_LIMIT
            };
            if next > a && reserve_host_fixed(a, next - a) == a {
                filled += next - a;
            }
            if kr != KERN_SUCCESS || addr + size <= a {
                break;
            }
            a = addr + size;
        }
        libc::pthread_mutex_unlock(&raw mut LOW_HOLE_FILL_LOCK);
        if filled != 0 {
            ocerz_log!(
                "low shadow: reserved %#llx bytes of free host space below %#llx\n",
                filled as c_ulonglong,
                OCERZ_LOW_LIMIT as c_ulonglong
            );
        }
    }
}

unsafe fn pin_add(lo: u64, hi: u64) {
    unsafe {
        static mut MAP: [u8; (OCERZ_LOW_LIMIT >> 17) as usize] =
            [0; (OCERZ_LOW_LIMIT >> 17) as usize];
        let mut p = lo & !(OCERZ_HOST_PAGE - 1);
        while p < hi {
            *((&raw mut MAP) as *mut u8).add((p >> 17) as usize) |= 1u8 << ((p >> 14) & 7);
            p += OCERZ_HOST_PAGE;
        }
        AtomicPtr::from_ptr(&raw mut ocerz_pin_map)
            .store((&raw mut MAP) as *mut u8, Ordering::Release);
        let gp = (&raw mut G_PIN) as *mut PinRange;
        if G_PIN_N != 0 && (*gp.add((G_PIN_N - 1) as usize)).hi == lo {
            (*gp.add((G_PIN_N - 1) as usize)).hi = hi;
        } else if (G_PIN_N as usize) < PIN_MAX {
            (*gp.add(G_PIN_N as usize)).lo = lo;
            (*gp.add(G_PIN_N as usize)).hi = hi;
            G_PIN_N += 1;
        } else {
            (*gp.add((G_PIN_N - 1) as usize)).hi = hi;
        }
    }
}

unsafe fn pin_alias(lo: u64, hi: u64) -> u64 {
    unsafe {
        let mut dst: MachVmAddress = ocerz_low_base + lo;
        let mut cur: VmProt = 0;
        let mut max: VmProt = 0;
        let kr = mach_vm_remap(
            mach_task_self(),
            &mut dst,
            hi - lo,
            0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
            mach_task_self(),
            lo,
            0,
            &mut cur,
            &mut max,
            VM_INHERIT_DEFAULT,
        );
        if !libc::getenv(c"OCERZ_PINLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: PIN[%d] %#llx-%#llx kr=%d\n".as_ptr(),
                libc::getpid() as c_int,
                lo as c_ulonglong,
                hi as c_ulonglong,
                kr,
            );
        }
        if kr != KERN_SUCCESS || dst != ocerz_low_base + lo {
            return 0;
        }
        pin_add(lo, hi);
        hi - lo
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_range_in_use(gaddr: u64, len: u64) -> c_int {
    unsafe {
        if len == 0 || gaddr > u64::MAX - len {
            return 1;
        }
        if G_PIN_N != 0 && pinned_overlap(gaddr, gaddr + len) != 0 {
            return 1;
        }
        let r = region_for_range(guest_round_down(gaddr), guest_round_up(gaddr + len));
        if r.is_null() {
            return 0;
        }
        let mut p = guest_round_down(gaddr);
        while p < gaddr + len {
            if slot_owner(slot_load(r, slot_index(r, p))) != 0 {
                return 1;
            }
            p += OCERZ_GUEST_PAGE;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_unpinned_parts(
    lo: u64,
    hi: u64,
    out: *mut u64,
    max: c_int,
) -> c_int {
    unsafe {
        const GRAN: u64 = 0x10000;
        let mut n: c_int = 0;
        let mut at = lo;
        while at < hi && n < max {
            let mut next = hi;
            let mut resume = 0u64;
            for i in 0..G_PIN_N {
                let p = &*((&raw const G_PIN) as *const PinRange).add(i as usize);
                if p.hi <= at || p.lo >= hi {
                    continue;
                }
                if p.lo <= at {
                    if p.hi > resume {
                        resume = p.hi;
                    }
                } else if p.lo < next {
                    next = p.lo;
                }
            }
            if resume != 0 {
                at = (resume + GRAN - 1) & !(GRAN - 1);
                continue;
            }
            let end = if next == hi { hi } else { next & !(GRAN - 1) };
            if end > at + GRAN {
                *out.add((2 * n) as usize) = at;
                *out.add((2 * n + 1) as usize) = end - at;
                n += 1;
            }
            at = if next == hi { hi } else { next };
        }
        n
    }
}

/* A fork gives the child its own copy-on-write copy of each pinned host range
and of its alias in the shadow, and the two copies no longer share pages:
Wine's process launcher builds argv in a forked child, through the alias, in
a malloc block of the first heap region, and the host's execv read the
other copy, all zeros.  The child aliases its own ranges again before any
guest code runs (ocerz_fork_child). */
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_pin_refresh() {
    unsafe {
        let mut failed = 0;
        for i in 0..G_PIN_N {
            let pin = &*((&raw const G_PIN) as *const PinRange).add(i as usize);
            let mut dst: MachVmAddress = ocerz_low_base + pin.lo;
            let mut cur: VmProt = 0;
            let mut max: VmProt = 0;
            if mach_vm_remap(
                mach_task_self(),
                &mut dst,
                pin.hi - pin.lo,
                0,
                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(),
                pin.lo,
                0,
                &mut cur,
                &mut max,
                VM_INHERIT_DEFAULT,
            ) != KERN_SUCCESS
                || dst != ocerz_low_base + pin.lo
            {
                failed += 1;
            }
        }
        if failed != 0 {
            ocerz_log!(
                "native Wine: %d of %d pinned ranges could not be aliased again after fork\n",
                failed,
                G_PIN_N
            );
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_host_low_readable(lo: u64, hi: u64) -> c_int {
    unsafe {
        let mut a: MachVmAddress = lo;
        while a < hi {
            let mut rlo: MachVmAddress = a;
            let mut size: MachVmSize = 0;
            let mut depth: Natural = 999;
            let mut info = VmRegionSubmapShortInfo64 {
                protection: 0,
                max_protection: 0,
                inheritance: 0,
                offset: 0,
                _pad0: 0,
                user_tag: 0,
                ref_count: 0,
                shadow_depth: 0,
                external_pager: 0,
                share_mode: 0,
                is_submap: 0,
                behavior: 0,
                object_id: 0,
                user_wired_count: 0,
                flags: 0,
            };
            let mut cnt: MachMsgTypeNumber = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            if mach_vm_region_recurse(
                mach_task_self(),
                &mut rlo,
                &mut size,
                &mut depth,
                &mut info as *mut _ as *mut c_void,
                &mut cnt,
            ) != KERN_SUCCESS
                || rlo >= hi
                || size == 0
            {
                return 0;
            }
            if info.protection & VM_PROT_READ != 0 {
                return 1;
            }
            a = rlo + size;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_pin_host_low() {
    unsafe {
        if ocerz_mode != ffi::OCERZ_MODE_NATIVE as c_int
            || ocerz_low_base == 0
            || G_PIN_N != 0
            || !libc::getenv(c"OCERZ_NO_LOW_PIN".as_ptr()).is_null()
        {
            return;
        }
        map_lock_acquire();
        let r = region_for_range(0, OCERZ_LOW_LIMIT);
        let mut pinned: u64 = 0;
        let mut lost: u64 = 0;
        let mut failed: c_int = 0;
        let mut a: MachVmAddress = 0x100000000;
        while a < OCERZ_LOW_LIMIT {
            let mut rlo: MachVmAddress = a;
            let mut size: MachVmSize = 0;
            let mut depth: Natural = 999;
            let mut info = VmRegionSubmapShortInfo64 {
                protection: 0,
                max_protection: 0,
                inheritance: 0,
                offset: 0,
                _pad0: 0,
                user_tag: 0,
                ref_count: 0,
                shadow_depth: 0,
                external_pager: 0,
                share_mode: 0,
                is_submap: 0,
                behavior: 0,
                object_id: 0,
                user_wired_count: 0,
                flags: 0,
            };
            let mut cnt: MachMsgTypeNumber = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            if mach_vm_region_recurse(
                mach_task_self(),
                &mut rlo,
                &mut size,
                &mut depth,
                &mut info as *mut _ as *mut c_void,
                &mut cnt,
            ) != KERN_SUCCESS
                || rlo >= OCERZ_LOW_LIMIT
                || size == 0
            {
                break;
            }
            let rhi = if rlo + size > OCERZ_LOW_LIMIT {
                OCERZ_LOW_LIMIT
            } else {
                rlo + size
            };
            a = rhi;
            if info.protection & VM_PROT_READ == 0 {
                continue;
            }
            let mut p = rlo;
            while p < rhi {
                let mut q = p;
                let mut held = false;
                let mut g = p;
                while !r.is_null() && g < p + OCERZ_HOST_PAGE {
                    held |= slot_owner(slot_load(r, slot_index(r, g))) != 0;
                    g += OCERZ_GUEST_PAGE;
                }
                while q < rhi {
                    let mut h = false;
                    let mut g = q;
                    while !r.is_null() && g < q + OCERZ_HOST_PAGE {
                        h |= slot_owner(slot_load(r, slot_index(r, g))) != 0;
                        g += OCERZ_GUEST_PAGE;
                    }
                    if h != held {
                        break;
                    }
                    q += OCERZ_HOST_PAGE;
                }
                if held {
                    lost += q - p;
                } else {
                    let got = pin_alias(p, q);
                    pinned += got;
                    failed += (got == 0) as c_int;
                }
                p = q;
            }
        }
        map_lock_release();
        ocerz_low_fill_host_holes();
        ocerz_log!(
            "native Wine: %#llx bytes of host memory below %#llx aliased into the shadow and pinned in %d ranges; %#llx bytes left to guest segments, %d remaps refused\n",
            pinned as c_ulonglong,
            OCERZ_LOW_LIMIT as c_ulonglong,
            G_PIN_N,
            lost as c_ulonglong,
            failed
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_init_low_shadow() -> c_int {
    unsafe {
        static CANDIDATES: [u64; 4] = [0x8000000000, 0x10000000000, 0x500000000, 0x600000000000];
        let topsz = OCERZ_TOP_HI - OCERZ_TOP_LO;
        let blocksz = OCERZ_LOW_LIMIT + topsz;
        map_lock_acquire();
        if ocerz_low_base != 0 {
            map_lock_release();
            return ffi::OCERZ_OK;
        }
        let mut base: u64 = 0;
        let env = libc::getenv(c"OCERZ_LOWBASE".as_ptr());
        if !env.is_null() && *env != 0 {
            let want = libc::strtoull(env, ptr::null_mut(), 0);
            base = reserve_host_fixed(want, blocksz);
            if base != want {
                map_lock_release();
                ocerz_fatal!(
                    "cannot reserve inherited low-shadow base %#llx\n",
                    want as c_ulonglong
                );
                return ffi::OCERZ_ENOMEM;
            }
        } else {
            let mut k = 0;
            while k < CANDIDATES.len() && base == 0 {
                base = reserve_host_fixed(*CANDIDATES.get_unchecked(k), blocksz);
                k += 1;
            }
            if base == 0 {
                map_lock_release();
                ocerz_fatal!(
                    "no host base accepts the %#llx-byte shadow block\n",
                    blocksz as c_ulonglong
                );
                return ffi::OCERZ_ENOMEM;
            }
            let mut buf = [0 as c_char; 24];
            libc::snprintf(
                buf.as_mut_ptr(),
                buf.len(),
                c"%#llx".as_ptr(),
                base as c_ulonglong,
            );
            libc::setenv(c"OCERZ_LOWBASE".as_ptr(), buf.as_ptr(), 1);
        }
        if region_add(0, OCERZ_LOW_LIMIT).is_null()
            || region_add(OCERZ_TOP_LO, OCERZ_TOP_HI).is_null()
        {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        ocerz_top_base = base + OCERZ_LOW_LIMIT;
        ocerz_low_base = base;
        map_lock_release();
        ocerz_low_fill_host_holes();
        ocerz_log!(
            "low shadow window guest [0, %#llx) -> host %#llx; top strip [%#llx, %#llx) -> host %#llx\n",
            OCERZ_LOW_LIMIT as c_ulonglong,
            base as c_ulonglong,
            OCERZ_TOP_LO as c_ulonglong,
            OCERZ_TOP_HI as c_ulonglong,
            ocerz_top_base as c_ulonglong
        );
        ffi::OCERZ_OK
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_register_range(glo: u64, ghi: u64) -> c_int {
    unsafe {
        let lo = round_down(glo);
        let hi = round_up(ghi);
        map_lock_acquire();
        if !region_for_range(lo, hi).is_null() {
            map_lock_release();
            return ffi::OCERZ_OK;
        }
        if lo < OCERZ_LOW_LIMIT || reserve_host_fixed(lo, hi - lo) != lo {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let ok = !region_add(lo, hi).is_null();
        if !ok {
            mach_vm_deallocate(mach_task_self(), lo, hi - lo);
        }
        map_lock_release();
        if ok {
            ocerz_log!(
                "registered identity guest range [%#llx, %#llx)\n",
                lo as c_ulonglong,
                hi as c_ulonglong
            );
        }
        if ok { ffi::OCERZ_OK } else { ffi::OCERZ_ENOMEM }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_vm_region(
    addr: *mut u64,
    size: *mut u64,
    prot: *mut c_uint,
    max_prot: *mut c_uint,
) -> c_int {
    unsafe {
        let mut query = guest_round_down(*addr);
        const TAIL_END: u64 = 0x1000000000000;
        map_lock_acquire();
        for _guard in 0..=MEM_REGION_MAX {
            let mut cls: *const MemRegion = ptr::null();
            let mut next: *const MemRegion = ptr::null();
            let n = REGION_N.load(Ordering::Relaxed);
            for k in 0..n {
                let rk = ((&raw const REGIONS) as *const MemRegion).add(k as usize);
                if cls.is_null() && query >= (*rk).glo && query < (*rk).ghi {
                    cls = rk;
                } else if (*rk).glo > query && (next.is_null() || (*rk).glo < (*next).glo) {
                    next = rk;
                }
            }
            if !cls.is_null() {
                let mut pos = query;
                let mut state = slot_load(cls, slot_index(cls, pos));
                if !slot_is_data(state) {
                    loop {
                        pos += OCERZ_GUEST_PAGE;
                        if pos >= (*cls).ghi {
                            break;
                        }
                        state = slot_load(cls, slot_index(cls, pos));
                        if slot_is_data(state) {
                            break;
                        }
                    }
                }
                if pos < (*cls).ghi && slot_is_data(state) {
                    let run_prot = ((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_uint;
                    let mut base = pos;
                    if pos == query {
                        while base > (*cls).glo {
                            let before = slot_load(cls, slot_index(cls, base - OCERZ_GUEST_PAGE));
                            if !slot_is_data(before)
                                || ((before & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_uint
                                    != run_prot
                            {
                                break;
                            }
                            base -= OCERZ_GUEST_PAGE;
                        }
                    }
                    let mut end = pos + OCERZ_GUEST_PAGE;
                    while end < (*cls).ghi {
                        let after = slot_load(cls, slot_index(cls, end));
                        if !slot_is_data(after)
                            || ((after & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_uint
                                != run_prot
                        {
                            break;
                        }
                        end += OCERZ_GUEST_PAGE;
                    }
                    *addr = base;
                    *size = end - base;
                    *prot = run_prot;
                    *max_prot = (PROT_READ | PROT_WRITE | PROT_EXEC) as c_uint;
                    map_lock_release();
                    return 1;
                }
                query = (*cls).ghi;
                continue;
            }
            if !next.is_null() && query < (*next).glo {
                query = (*next).glo;
                continue;
            }
            break;
        }
        map_lock_release();
        *addr = query;
        *size = if query < TAIL_END {
            TAIL_END - query
        } else {
            OCERZ_HOST_PAGE
        };
        *prot = 0;
        *max_prot = 0;
        1
    }
}

unsafe fn map_fixed_locked(gaddr: u64, len: u64, prot: c_int, zero_overlap: c_int) -> c_int {
    unsafe {
        let mut lo: u64 = 0;
        let mut hi: u64 = 0;
        if guest_range(gaddr, len, &mut lo, &mut hi) == 0 {
            return map_refuse(1, gaddr, gaddr + len, ffi::OCERZ_ENOMEM);
        }
        if G_PIN_N != 0 && pinned_overlap(round_down(lo), round_up(hi)) != 0 {
            return map_refuse(12, lo, hi, ffi::OCERZ_ENOMEM);
        }
        let r = region_for_range(round_down(lo), round_up(hi));
        if r.is_null() {
            return map_refuse(2, lo, hi, ffi::OCERZ_ENOMEM);
        }
        install_mapping_locked(
            r,
            lo,
            hi,
            hi,
            prot,
            1,
            if zero_overlap != 0 { gaddr } else { 0 },
            if zero_overlap != 0 { gaddr + len } else { 0 },
            ptr::null_mut(),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_fixed(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe {
        map_lock_acquire();
        let rc = map_fixed_locked(gaddr, len, prot, 1);
        map_lock_release();
        memlog(
            if prot == 0 {
                c"reserve".as_ptr()
            } else {
                c"commit".as_ptr()
            },
            gaddr,
            len,
            prot,
        );
        rc
    }
}

unsafe fn map_shared_overlay(
    gaddr: u64,
    len: u64,
    prot: c_int,
    fd: c_int,
    off: u64,
    padded: c_int,
    op: *const c_char,
) -> c_int {
    unsafe {
        let mut data_lo: u64 = 0;
        let mut data_hi: u64 = 0;
        if guest_range(gaddr, len, &mut data_lo, &mut data_hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        if gaddr != data_lo || (fd >= 0 && (off & (OCERZ_GUEST_PAGE - 1)) != 0) {
            return ffi::OCERZ_EUNSUP;
        }
        let lo = round_down(data_lo);
        let hi = round_up(data_hi);
        let mut map_off: u64 = 0;
        if G_PIN_N != 0 && pinned_overlap(lo, hi) != 0 {
            return ffi::OCERZ_ENOMEM;
        }
        if fd >= 0 {
            let prefix = data_lo - lo;
            if off < prefix {
                return ffi::OCERZ_EUNSUP;
            }
            map_off = off - prefix;
            if (map_off & (OCERZ_HOST_PAGE - 1)) != 0 || map_off > i64::MAX as u64 {
                return ffi::OCERZ_EUNSUP;
            }
        }
        map_lock_acquire();
        let r = region_for_range(lo, hi);
        if r.is_null() {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let mut p = data_lo;
        while p < data_hi {
            if !slot_is_data(slot_load(r, slot_index(r, p))) {
                map_lock_release();
                return ffi::OCERZ_ENOMEM;
            }
            p += OCERZ_GUEST_PAGE;
        }
        let mut page = lo;
        while page < hi {
            if shared_load(r, pg_index(r, page)) & MEM_SHARED_PHYSICAL != 0 {
                map_lock_release();
                return ffi::OCERZ_EUNSUP;
            }
            let mut p = page;
            while p < page + OCERZ_HOST_PAGE {
                if !(p >= data_lo && p < data_hi) {
                    let state = slot_load(r, slot_index(r, p));
                    let sibling_prot =
                        ((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_int;
                    if slot_is_data(state) && sibling_prot != PROT_NONE {
                        map_lock_release();
                        return ffi::OCERZ_EUNSUP;
                    }
                }
                p += OCERZ_GUEST_PAGE;
            }
            page += OCERZ_HOST_PAGE;
        }
        if padded != 0 {
            let fd_flags = libc::fcntl(fd, libc::F_GETFL);
            let mut st: libc::stat = core::mem::zeroed();
            let map_len = hi - lo;
            if fd < 0
                || fd_flags < 0
                || (fd_flags & libc::O_ACCMODE) == libc::O_RDONLY
                || map_off > i64::MAX as u64 - map_len
                || libc::fstat(fd, &mut st) != 0
                || st.st_size < 0
            {
                map_lock_release();
                return ffi::OCERZ_EUNSUP;
            }
            let need = map_off + map_len;
            if (st.st_size as u64) < need && libc::ftruncate(fd, need as libc::off_t) != 0 {
                map_lock_release();
                return ffi::OCERZ_EUNSUP;
            }
        }
        let want = ocerz_g2h(lo);
        let flags = libc::MAP_SHARED | libc::MAP_FIXED | if fd < 0 { libc::MAP_ANON } else { 0 };
        let got = libc::mmap(
            want,
            (hi - lo) as usize,
            host_prot(prot),
            flags,
            fd,
            map_off as libc::off_t,
        );
        if got == libc::MAP_FAILED || got != want {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let mut page = lo;
        while page < hi {
            let mut mask: u8 = 0;
            let first = if page > data_lo { page } else { data_lo };
            let end = if page + OCERZ_HOST_PAGE < data_hi {
                page + OCERZ_HOST_PAGE
            } else {
                data_hi
            };
            let mut p = first;
            while p < end {
                mask |= shared_slot_bit(p);
                p += OCERZ_GUEST_PAGE;
            }
            shared_store(
                r,
                pg_index(r, page),
                MEM_SHARED_PHYSICAL | (if padded != 0 { MEM_SHARED_PADDED } else { 0 }) | mask,
            );
            bit_set(r, pg_index(r, page));
            page += OCERZ_HOST_PAGE;
        }
        let mut p = data_lo;
        while p < data_hi {
            let i = slot_index(r, p);
            let state = slot_load(r, i);
            slot_store(r, i, slot_data_state(slot_owner(state), prot));
            p += OCERZ_GUEST_PAGE;
        }
        let rc = sync_host_range_locked(r, lo, hi);
        map_lock_release();
        memlog(op, gaddr, len, prot);
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_shared_anon(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe { map_shared_overlay(gaddr, len, prot, -1, 0, 0, c"shared-anon".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_shared_file(
    gaddr: u64,
    len: u64,
    prot: c_int,
    fd: c_int,
    off: u64,
) -> c_int {
    unsafe { map_shared_overlay(gaddr, len, prot, fd, off, 0, c"shared-file".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_shared_file_padded(
    gaddr: u64,
    len: u64,
    prot: c_int,
    fd: c_int,
    off: u64,
) -> c_int {
    unsafe { map_shared_overlay(gaddr, len, prot, fd, off, 1, c"shared-file-padded".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_anywhere(len: u64, prot: c_int) -> u64 {
    unsafe {
        if len == 0 || len > u64::MAX - (OCERZ_GUEST_PAGE - 1) {
            return 0;
        }
        let glen = guest_round_up(len);
        map_lock_acquire();
        let gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
        if gaddr == 0 {
            if !libc::getenv(c"OCERZ_OOMLOG".as_ptr()).is_null() {
                libc::fprintf(crate::log::stderr(),
                    c"ocerz: MAPOOM[%d] len=%#llx bump_next=%#llx arena=[%#llx,%#llx) free_above_bump=%#llx\n".as_ptr(),
                    libc::getpid() as c_int, len as c_ulonglong,
                    BUMP_NEXT as c_ulonglong,
                    ocerz_arena_lo as c_ulonglong, ocerz_arena_hi as c_ulonglong,
                    ocerz_arena_hi.wrapping_sub(BUMP_NEXT) as c_ulonglong);
            }
            map_lock_release();
            return 0;
        }
        let data_hi = gaddr + glen;
        let mut guard_hi: u64 = 0;
        let rc = if allocation_guard_end(data_hi, &mut guard_hi) != 0 {
            install_mapping_locked(
                region_for_range(gaddr, guard_hi),
                gaddr,
                data_hi,
                guard_hi,
                prot,
                0,
                gaddr,
                data_hi,
                ptr::null_mut(),
            )
        } else {
            ffi::OCERZ_ENOMEM
        };
        if rc == ffi::OCERZ_OK {
            BUMP_NEXT = guard_hi;
        }
        map_lock_release();
        if rc == ffi::OCERZ_OK { gaddr } else { 0 }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_anywhere_aligned(len: u64, prot: c_int, align: u64) -> u64 {
    unsafe {
        let mut align = align;
        if align < OCERZ_HOST_PAGE {
            align = OCERZ_HOST_PAGE;
        }
        if (align & (align - 1)) != 0 || len == 0 || len > u64::MAX - (OCERZ_GUEST_PAGE - 1) {
            return 0;
        }
        let glen = guest_round_up(len);
        map_lock_acquire();
        let gaddr = find_anywhere_locked(glen, align);
        if gaddr == 0 {
            map_lock_release();
            return 0;
        }
        let data_hi = gaddr + glen;
        let mut guard_hi: u64 = 0;
        let rc = if allocation_guard_end(data_hi, &mut guard_hi) != 0 {
            install_mapping_locked(
                region_for_range(gaddr, guard_hi),
                gaddr,
                data_hi,
                guard_hi,
                prot,
                0,
                gaddr,
                data_hi,
                ptr::null_mut(),
            )
        } else {
            ffi::OCERZ_ENOMEM
        };
        if rc == ffi::OCERZ_OK {
            BUMP_NEXT = guard_hi;
        }
        map_lock_release();
        if rc == ffi::OCERZ_OK { gaddr } else { 0 }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_prefork() {
    unsafe {
        map_lock_acquire();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_postfork() {
    unsafe {
        map_lock_release();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_hint(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe {
        if len == 0 || gaddr > u64::MAX - len || gaddr + len > u64::MAX - (OCERZ_HOST_PAGE - 1) {
            return ffi::OCERZ_ENOMEM;
        }
        let lo = gaddr & !(OCERZ_HOST_PAGE - 1);
        let hi = round_up(gaddr + len);
        if lo == 0 || hi <= lo || lo < OCERZ_LOW_LIMIT {
            return ffi::OCERZ_ENOMEM;
        }
        map_lock_acquire();
        if !region_for_range(lo, hi).is_null() {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        if reserve_host_fixed(lo, hi - lo) != lo {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        if region_add(lo, hi).is_null() {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let rc = map_fixed_locked(lo, hi - lo, prot, 0);
        map_lock_release();
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_claim_fixed(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe {
        let mut lo: u64 = 0;
        let mut hi: u64 = 0;
        if guest_range(gaddr, len, &mut lo, &mut hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        let mut guard_hi: u64 = 0;
        if allocation_guard_end(hi, &mut guard_hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        map_lock_acquire();
        let r = region_for_range(round_down(lo), guard_hi);
        if r.is_null() || lo < ALLOC_FLOOR || guard_hi > ocerz_arena_hi {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let rc = install_mapping_locked(r, lo, hi, guard_hi, prot, 0, lo, hi, ptr::null_mut());
        if rc == ffi::OCERZ_OK && lo == BUMP_NEXT {
            BUMP_NEXT = guard_hi;
        }
        map_lock_release();
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_donate(len: u64) -> u64 {
    unsafe {
        if len == 0 || len > u64::MAX - (OCERZ_HOST_PAGE - 1) {
            return 0;
        }
        let glen = round_up(len);
        map_lock_acquire();
        let gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
        if gaddr == 0 {
            map_lock_release();
            return 0;
        }
        let data_hi = gaddr + glen;
        let mut guard_hi: u64 = 0;
        let mut r: *mut MemRegion = ptr::null_mut();
        let mut affected_lo = u64::MAX;
        let mut affected_hi: u64 = 0;
        let mut owner: u32 = 0;
        if allocation_guard_end(data_hi, &mut guard_hi) != 0 {
            r = region_for_range(gaddr, guard_hi);
            if !r.is_null() {
                owner = claim_slots_locked(
                    r,
                    gaddr,
                    data_hi,
                    guard_hi,
                    PROT_READ | PROT_WRITE,
                    &mut affected_lo,
                    &mut affected_hi,
                );
            }
        }
        if owner == 0 {
            map_lock_release();
            return 0;
        }
        BUMP_NEXT = guard_hi;
        let mut p = gaddr;
        while p < data_hi {
            bit_set(r, pg_index(r, p));
            p += OCERZ_HOST_PAGE;
        }
        map_lock_release();
        gaddr
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_map_claim_region(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe {
        let mut lo: u64 = 0;
        let mut hi: u64 = 0;
        if guest_range(gaddr, len, &mut lo, &mut hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        if G_PIN_N != 0 && pinned_overlap(round_down(lo), round_up(hi)) != 0 {
            return ffi::OCERZ_ENOMEM;
        }
        map_lock_acquire();
        let r = region_for_range(round_down(lo), round_up(hi));
        if r.is_null() || ((*r).glo == ocerz_arena_lo && (*r).ghi == ocerz_arena_hi) {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let rc = install_mapping_locked(r, lo, hi, hi, prot, 0, lo, hi, ptr::null_mut());
        map_lock_release();
        rc
    }
}

unsafe fn protect_part(lo: u64, hi: u64, prot: c_int) -> c_int {
    unsafe { ocerz_protect(lo, hi - lo, prot) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_protect(gaddr: u64, len: u64, prot: c_int) -> c_int {
    unsafe {
        let mut lo: u64 = 0;
        let mut hi: u64 = 0;
        if guest_range(gaddr, len, &mut lo, &mut hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        if G_PIN_N != 0 && pinned_overlap(round_down(lo), round_up(hi)) != 0 {
            return for_unpinned(lo, hi, protect_part, prot);
        }
        map_lock_acquire();
        let r = region_for_range(round_down(lo), round_up(hi));
        let mut rc = if r.is_null() {
            ffi::OCERZ_ENOMEM
        } else {
            ffi::OCERZ_OK
        };
        let mut p = lo;
        while rc == ffi::OCERZ_OK && p < hi {
            let state = slot_load(r, slot_index(r, p));
            if !slot_is_data(state) {
                rc = ffi::OCERZ_ENOMEM;
            }
            let shared = shared_load(r, pg_index(r, p));
            if (shared & MEM_SHARED_PHYSICAL) != 0
                && (shared & shared_slot_bit(p)) == 0
                && prot != PROT_NONE
            {
                rc = ffi::OCERZ_EUNSUP;
            }
            p += OCERZ_GUEST_PAGE;
        }
        if rc == ffi::OCERZ_OK {
            let mut p = lo;
            while p < hi {
                let i = slot_index(r, p);
                let state = slot_load(r, i);
                slot_store(r, i, slot_data_state(slot_owner(state), prot));
                p += OCERZ_GUEST_PAGE;
            }
            rc = sync_host_range_locked(r, lo, hi);
        }
        map_lock_release();
        memlog(
            if host_prot(prot) == (PROT_READ | PROT_WRITE) {
                c"prot-rw".as_ptr()
            } else {
                c"prot-ro".as_ptr()
            },
            gaddr,
            len,
            prot,
        );
        rc
    }
}

unsafe fn unmap_part(lo: u64, hi: u64, _unused: c_int) -> c_int {
    unsafe { ocerz_unmap(lo, hi - lo) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_unmap(gaddr: u64, len: u64) -> c_int {
    unsafe {
        let mut lo: u64 = 0;
        let mut hi: u64 = 0;
        if guest_range(gaddr, len, &mut lo, &mut hi) == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        if G_PIN_N != 0 && pinned_overlap(round_down(lo), round_up(hi)) != 0 {
            return for_unpinned(lo, hi, unmap_part, 0);
        }
        map_lock_acquire();
        let r = region_for_range(round_down(lo), round_up(hi));
        if r.is_null() {
            map_lock_release();
            return ffi::OCERZ_ENOMEM;
        }
        let mut affected_lo = u64::MAX;
        let mut affected_hi: u64 = 0;
        release_range_locked(r, lo, hi, &mut affected_lo, &mut affected_hi);
        let rc = sync_host_range_locked(r, affected_lo, affected_hi);
        map_lock_release();
        memlog(c"unmap".as_ptr(), gaddr, len, 0);

        if gaddr <= 0x10000 && hi >= 0x100000000 {
            ocerz_init_gate_release();
        }
        rc
    }
}

static mut G_ARMSTAT_ARMED: c_ulong = 0;
static mut G_ARMSTAT_FAULTS: c_ulong = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_armed_any() -> c_int {
    (G_ARMED_LIVE.load(Ordering::Relaxed) > 0) as c_int
}

extern "C" fn armstat_dump() {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: ARMSTAT[%d] armed=%lu write-faults=%lu\n".as_ptr(),
            libc::getpid() as c_int,
            G_ARMSTAT_ARMED,
            G_ARMSTAT_FAULTS,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_arm_exec(lo: u64, hi: u64) -> c_int {
    unsafe {
        static mut DIS: c_int = -1;
        if DIS < 0 {
            DIS = if libc::getenv(c"OCERZ_NO_ARM_EXEC".as_ptr()).is_null() {
                0
            } else {
                1
            };
            if !libc::getenv(c"OCERZ_ARMSTAT".as_ptr()).is_null() {
                libc::atexit(armstat_dump);
            }
        }
        if DIS != 0 || hi <= lo {
            return 0;
        }
        let plo = round_down(lo);
        let phi = round_up(hi);
        map_lock_acquire();
        let r = region_for_range(plo, phi);
        let mut n: c_int = 0;
        if !r.is_null() && !(*r).armed.is_null() {
            let mut page = plo;
            while page < phi {
                let i = pg_index(r, page);
                if *(*r).armed.add(i) != 0 {
                    page += OCERZ_HOST_PAGE;
                    continue;
                }
                if bit_test(r, i) == 0 {
                    page += OCERZ_HOST_PAGE;
                    continue;
                }
                if shared_load(r, i) & MEM_SHARED_PHYSICAL != 0 {
                    page += OCERZ_HOST_PAGE;
                    continue;
                }
                let mut code_writable = false;
                let slo = if lo > page { lo } else { page };
                let shi = if hi < page + OCERZ_HOST_PAGE {
                    hi
                } else {
                    page + OCERZ_HOST_PAGE
                };
                let mut p = slo & !(OCERZ_GUEST_PAGE - 1);
                while p < shi {
                    let st = slot_load(r, slot_index(r, p));
                    if slot_is_data(st)
                        && (((st & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_int
                            & PROT_WRITE)
                            != 0
                    {
                        code_writable = true;
                        break;
                    }
                    p += OCERZ_GUEST_PAGE;
                }
                if !code_writable {
                    page += OCERZ_HOST_PAGE;
                    continue;
                }
                let mut has_data: c_int = 0;
                let gp = host_page_guest_prot(r, page, &mut has_data);
                if has_data == 0 {
                    page += OCERZ_HOST_PAGE;
                    continue;
                }
                *(*r).armed.add(i) = 1;
                if libc::mprotect(
                    ocerz_g2h(page),
                    OCERZ_HOST_PAGE as usize,
                    host_prot(gp) & !PROT_WRITE,
                ) != 0
                {
                    *(*r).armed.add(i) = 0;
                } else {
                    n += 1;
                    G_ARMSTAT_ARMED += 1;
                    G_ARMED_LIVE.fetch_add(1, Ordering::Relaxed);
                }
                page += OCERZ_HOST_PAGE;
            }
        }
        map_lock_release();
        n
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_disarm_range(
    lo: u64,
    hi: u64,
    pages: *mut u64,
    max: c_int,
) -> c_int {
    unsafe {
        if hi <= lo || G_ARMED_LIVE.load(Ordering::Relaxed) <= 0 {
            return 0;
        }
        let plo = round_down(lo);
        let phi = round_up(hi);
        let mut n: c_int = 0;
        map_lock_acquire();
        let r = region_for_range(plo, phi);
        if !r.is_null() && !(*r).armed.is_null() {
            let mut page = plo;
            while page < phi && n < max {
                let i = pg_index(r, page);
                if *(*r).armed.add(i) != 0 {
                    *(*r).armed.add(i) = 0;
                    G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                    let mut has_data: c_int = 0;
                    let gp = host_page_guest_prot(r, page, &mut has_data);
                    if has_data != 0 {
                        libc::mprotect(ocerz_g2h(page), OCERZ_HOST_PAGE as usize, host_prot(gp));
                    }
                    *pages.add(n as usize) = page;
                    n += 1;
                }
                page += OCERZ_HOST_PAGE;
            }
        }
        map_lock_release();
        n
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_disarm_all(pages: *mut u64, max: c_int) -> c_int {
    unsafe {
        let mut n: c_int = 0;
        map_lock_acquire();
        let rn = REGION_N.load(Ordering::Relaxed);
        let mut ri = 0;
        while ri < rn && n < max {
            let r = ((&raw mut REGIONS) as *mut MemRegion).add(ri as usize);
            if !(*r).armed.is_null() {
                let np = (((*r).ghi - (*r).glo) / OCERZ_HOST_PAGE) as usize;
                let mut i = 0;
                while i < np && n < max {
                    if *(*r).armed.add(i) != 0 {
                        *(*r).armed.add(i) = 0;
                        G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                        let page = (*r).glo + i as u64 * OCERZ_HOST_PAGE;
                        let mut has_data: c_int = 0;
                        let gp = host_page_guest_prot(r, page, &mut has_data);
                        if has_data != 0 {
                            libc::mprotect(
                                ocerz_g2h(page),
                                OCERZ_HOST_PAGE as usize,
                                host_prot(gp),
                            );
                        }
                        *pages.add(n as usize) = page;
                        n += 1;
                    }
                    i += 1;
                }
            }
            ri += 1;
        }
        map_lock_release();
        n
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_exec_write_fault(gaddr: u64) -> c_int {
    unsafe {
        if gaddr == u64::MAX {
            return 0;
        }
        let page = round_down(gaddr);
        map_lock_acquire();
        let r = region_for_range(page, page + OCERZ_HOST_PAGE);
        let mut hit = 0;
        if !r.is_null() && !(*r).armed.is_null() {
            let i = pg_index(r, page);
            let mut has_data: c_int = 0;
            let gp = host_page_guest_prot(r, page, &mut has_data);
            if *(*r).armed.add(i) != 0 {
                *(*r).armed.add(i) = 0;
                G_ARMED_LIVE.fetch_sub(1, Ordering::Relaxed);
                G_ARMSTAT_FAULTS += 1;
                if has_data != 0
                    && libc::mprotect(ocerz_g2h(page), OCERZ_HOST_PAGE as usize, host_prot(gp)) == 0
                {
                    hit = 1;
                }
            } else if has_data != 0
                && bit_test(r, i) != 0
                && (gp & PROT_WRITE) != 0
                && (shared_load(r, i) & MEM_SHARED_PHYSICAL) == 0
            {
                hit = 2;
            }
        }
        map_lock_release();
        hit
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_addr_committed(gaddr: u64) -> c_int {
    unsafe {
        if gaddr == u64::MAX {
            return -1;
        }
        let r = region_for_range(round_down(gaddr), round_up(gaddr + 1));
        if r.is_null() {
            return -1;
        }
        if slot_is_data(slot_load(r, slot_index(r, gaddr))) {
            1
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_addr_prot(gaddr: u64) -> c_int {
    unsafe {
        if gaddr == u64::MAX {
            return -1;
        }
        let r = region_for_range(round_down(gaddr), round_up(gaddr + 1));
        if r.is_null() {
            return -1;
        }
        let state = slot_load(r, slot_index(r, gaddr));
        if !slot_is_data(state) {
            return -1;
        }
        ((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_addr_readable(gaddr: u64) -> c_int {
    unsafe {
        let prot = ocerz_addr_prot(gaddr);
        (prot >= 0 && (prot & PROT_READ) != 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_mem_overlaps(gaddr: u64, len: u64) -> c_int {
    unsafe {
        if len == 0 || gaddr > u64::MAX - len {
            return 0;
        }
        let lo = round_down(gaddr);
        let hi = gaddr + len;
        let n = REGION_N.load(Ordering::Acquire);
        for k in 0..n {
            let rk = ((&raw const REGIONS) as *const MemRegion).add(k as usize);
            if lo < (*rk).ghi && hi > (*rk).glo {
                return 1;
            }
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_host_region_prot(
    gaddr: u64,
    base: *mut u64,
    size: *mut u64,
) -> c_uint {
    unsafe {
        let mut a: MachVmAddress = ocerz_g2h(gaddr) as MachVmAddress;
        let mut sz: MachVmSize = 0;
        let mut info = VmRegionBasicInfo64 {
            protection: 0,
            max_protection: 0,
            inheritance: 0,
            shared: 0,
            reserved: 0,
            offset: 0,
            _pad1: 0,
            behavior: 0,
            user_wired_count: 0,
            _pad0: 0,
        };
        let mut cnt: MachMsgTypeNumber = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj: MachPort = MACH_PORT_NULL;
        let kr = mach_vm_region(
            mach_task_self(),
            &mut a,
            &mut sz,
            VM_REGION_BASIC_INFO_64,
            &mut info as *mut _ as *mut c_void,
            &mut cnt,
            &mut obj,
        );
        if kr != KERN_SUCCESS {
            return !0u32;
        }
        if !base.is_null() {
            *base = a;
        }
        if !size.is_null() {
            *size = sz;
        }
        ((info.protection & 0xff) as c_uint) | (((info.max_protection & 0xff) as c_uint) << 8)
    }
}
