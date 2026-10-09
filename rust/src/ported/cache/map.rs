//! Shared-cache mapping, slide rebasing, lazy page faults, and patch watches.

use core::ffi::{c_char, c_int, c_uint, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU8, Ordering};

use crate::ffi::{self, OcerzCache};
use crate::inline::VM_INHERIT_DEFAULT;

use super::{rd32, rd64};

const LAZY_MAX: c_int = 16;
const CMAP_MAX: c_int = 128;
const WATCH_ARMED: u8 = 1;
const WATCH_WRITABLE: u8 = 2;
const HOST_PAGE_SIZE: u64 = 0x4000;

const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_PROT_EXECUTE: c_int = 4;
const VM_FLAGS_FIXED: c_int = 0x0000_0000;
const VM_FLAGS_OVERWRITE: c_int = 0x0000_4000;
const KERN_SUCCESS: c_int = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct LazyRegion {
    addr: u64,
    size: u64,
    page_size: u32,
    si: *const u8,
    cache_base: u64,
    final_prot: c_int,
    done: *mut u8,
    fd: c_int,
    foff: u64,
}

static mut G_LAZY: [LazyRegion; LAZY_MAX as usize] = [LazyRegion {
    addr: 0,
    size: 0,
    page_size: 0,
    si: ptr::null(),
    cache_base: 0,
    final_prot: 0,
    done: ptr::null_mut(),
    fd: 0,
    foff: 0,
}; LAZY_MAX as usize];
static mut G_N_LAZY: c_int = 0;
static G_LAZY_LOCK: AtomicI32 = AtomicI32::new(0);

#[thread_local]
static mut LAST_RETRY: usize = 0;
static mut NO_REMAP: c_int = -1;
static mut EAGER: c_int = -1;

#[repr(C)]
struct CMap {
    addr: u64,
    size: u64,
    watch: AtomicPtr<u8>,
}

static mut G_CMAP: [CMap; CMAP_MAX as usize] = [const {
    CMap {
        addr: 0,
        size: 0,
        watch: AtomicPtr::new(ptr::null_mut()),
    }
}; CMAP_MAX as usize];
static mut G_N_CMAP: c_int = 0;
static mut G_CMAP_LO: u64 = u64::MAX;
static mut G_CMAP_HI: u64 = 0;
static G_ANY_WATCH: AtomicI32 = AtomicI32::new(0);

static mut G_NAMED_CACHE: *const OcerzCache = ptr::null();
static mut SUBCACHE_HDR: [u8; 0x400] = [0; 0x400];

type MachPort = u32;
type MachVmAddress = u64;
type MachVmSize = u64;
type VmProt = c_int;
type KernReturn = c_int;

unsafe extern "C" {
    static mut mach_task_self_: MachPort;
    fn mach_vm_remap(
        target_task: MachPort,
        target_address: *mut MachVmAddress,
        size: MachVmSize,
        mask: MachVmAddress,
        flags: c_int,
        src_task: MachPort,
        src_address: MachVmAddress,
        copy: c_int,
        cur_protection: *mut VmProt,
        max_protection: *mut VmProt,
        inheritance: libc::vm_inherit_t,
    ) -> KernReturn;
}

#[inline]
unsafe fn mach_task_self() -> MachPort {
    unsafe { ptr::addr_of!(mach_task_self_).read() }
}

unsafe fn rebase_chain_v2(
    page_base: u64,
    page_end: u64,
    start4: u16,
    cache_base: u64,
    delta_mask: u64,
    delta_shift: c_int,
) {
    unsafe {
        let value_mask = !delta_mask;
        let mut cur = page_base.wrapping_add((start4 as u64).wrapping_mul(4));
        loop {
            if cur < page_base || cur.wrapping_add(8) > page_end {
                break;
            }
            let loc = cur as usize as *mut u64;
            let raw = loc.read_unaligned();
            let mut value = raw & value_mask;
            if value != 0 {
                value = value.wrapping_add(cache_base);
            }
            let delta = (raw & delta_mask).wrapping_shr(delta_shift as u32);
            loc.write_unaligned(value);
            if delta == 0 {
                break;
            }
            cur = cur.wrapping_add(delta);
        }
    }
}

unsafe fn rebase_page_v2(page_base: u64, page_size: u64, pg: u32, cache_base: u64, si: *const u8) {
    unsafe {
        let ps_off = rd32(si.add(8));
        let ps_cnt = rd32(si.add(12));
        let pe_off = rd32(si.add(16));
        let pe_cnt = rd32(si.add(20));
        let delta_mask = rd64(si.add(24));
        let delta_shift = delta_mask.trailing_zeros() as c_int - 2;
        let page_starts = si.add(ps_off as usize);
        let page_extras = si.add(pe_off as usize);
        if pg >= ps_cnt {
            return;
        }
        let start_off = pg.wrapping_mul(2) as usize;
        let start = (page_starts.add(start_off).read() as u16)
            | ((page_starts.add(start_off + 1).read() as u16) << 8);
        if start == 0x4000 {
            return;
        }
        let page_end = page_base.wrapping_add(page_size);
        if start & 0x8000 != 0 {
            let mut idx = (start & 0x3fff) as u32;
            while idx < pe_cnt {
                let off = idx.wrapping_mul(2) as usize;
                let e = (page_extras.add(off).read() as u16)
                    | ((page_extras.add(off + 1).read() as u16) << 8);
                rebase_chain_v2(
                    page_base,
                    page_end,
                    e & 0x3fff,
                    cache_base,
                    delta_mask,
                    delta_shift,
                );
                if e & 0x8000 != 0 {
                    break;
                }
                idx = idx.wrapping_add(1);
            }
        } else {
            rebase_chain_v2(
                page_base,
                page_end,
                start,
                cache_base,
                delta_mask,
                delta_shift,
            );
        }
    }
}

unsafe fn rebase_slide_v2(map_addr: u64, map_size: u64, cache_base: u64, si: *const u8) {
    unsafe {
        let page_size = rd32(si.add(4));
        let ps_off = rd32(si.add(8));
        let ps_cnt = rd32(si.add(12));
        let pe_off = rd32(si.add(16));
        let pe_cnt = rd32(si.add(20));
        let delta_mask = rd64(si.add(24));
        let delta_shift = delta_mask.trailing_zeros() as c_int - 2;
        let page_starts = si.add(ps_off as usize);
        let page_extras = si.add(pe_off as usize);
        let mut pg = 0u32;
        while pg < ps_cnt {
            let off = pg.wrapping_mul(2) as usize;
            let start = (page_starts.add(off).read() as u16)
                | ((page_starts.add(off + 1).read() as u16) << 8);
            if start != 0x4000 {
                let page_base = map_addr.wrapping_add((pg as u64).wrapping_mul(page_size as u64));
                if page_base.wrapping_add(page_size as u64) > map_addr.wrapping_add(map_size) {
                    break;
                }
                let page_end = page_base.wrapping_add(page_size as u64);
                if start & 0x8000 != 0 {
                    let mut idx = (start & 0x3fff) as u32;
                    while idx < pe_cnt {
                        let off = idx.wrapping_mul(2) as usize;
                        let e = (page_extras.add(off).read() as u16)
                            | ((page_extras.add(off + 1).read() as u16) << 8);
                        rebase_chain_v2(
                            page_base,
                            page_end,
                            e & 0x3fff,
                            cache_base,
                            delta_mask,
                            delta_shift,
                        );
                        if e & 0x8000 != 0 {
                            break;
                        }
                        idx = idx.wrapping_add(1);
                    }
                } else {
                    rebase_chain_v2(
                        page_base,
                        page_end,
                        start,
                        cache_base,
                        delta_mask,
                        delta_shift,
                    );
                }
            }
            pg = pg.wrapping_add(1);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_lazy_fault(addr: usize) -> c_int {
    unsafe {
        let n_lazy = ptr::addr_of!(G_N_LAZY).read();
        let lazy = ptr::addr_of_mut!(G_LAZY).cast::<LazyRegion>();
        let mut i = 0;
        while i < n_lazy {
            let entry = lazy.add(i as usize);
            let region_addr = (*entry).addr;
            if (addr as u64).wrapping_sub(region_addr) >= (*entry).size {
                i = i.wrapping_add(1);
                continue;
            }
            let hp = HOST_PAGE_SIZE;
            let off = ((addr as u64).wrapping_sub(region_addr)) & !(hp - 1);
            let hidx = (off / hp) as usize;
            let page = region_addr.wrapping_add(off) as usize;
            while G_LAZY_LOCK.swap(1, Ordering::Acquire) != 0 {}
            if (*(*entry).done.add(hidx)) != 0 {
                let again = ptr::addr_of!(LAST_RETRY).read() == page;
                ptr::addr_of_mut!(LAST_RETRY).write(page);
                G_LAZY_LOCK.store(0, Ordering::Release);
                return if again { 0 } else { 1 };
            }
            ptr::addr_of_mut!(LAST_RETRY).write(0);
            let base = region_addr.wrapping_add(off);
            let per = (hp / (*entry).page_size as u64) as u32;
            let mut installed = 0;
            let no_remap = ptr::addr_of_mut!(NO_REMAP);
            if no_remap.read() < 0 {
                no_remap.write(if libc::getenv(c"OCERZ_NO_LAZY_REMAP".as_ptr()).is_null() {
                    0
                } else {
                    1
                });
            }
            let tmp = if (*entry).fd >= 0 && no_remap.read() == 0 {
                libc::mmap(
                    ptr::null_mut(),
                    hp as usize,
                    libc::PROT_READ | libc::PROT_WRITE,
                    libc::MAP_PRIVATE | libc::MAP_ANON,
                    -1,
                    0,
                )
            } else {
                libc::MAP_FAILED
            };
            if tmp != libc::MAP_FAILED {
                let got = libc::pread(
                    (*entry).fd,
                    tmp,
                    hp as usize,
                    (*entry).foff.wrapping_add(off) as libc::off_t,
                );
                if got > 0 {
                    let mut k = 0u32;
                    while k < per {
                        let pb =
                            base.wrapping_add((k as u64).wrapping_mul((*entry).page_size as u64));
                        if pb.wrapping_add((*entry).page_size as u64)
                            > region_addr.wrapping_add((*entry).size)
                        {
                            break;
                        }
                        rebase_page_v2(
                            (tmp as usize as u64)
                                .wrapping_add((k as u64).wrapping_mul((*entry).page_size as u64)),
                            (*entry).page_size as u64,
                            (pb.wrapping_sub(region_addr) / (*entry).page_size as u64) as u32,
                            (*entry).cache_base,
                            (*entry).si,
                        );
                        k = k.wrapping_add(1);
                    }
                    let mut dst = base;
                    let mut curp: VmProt = 0;
                    let mut maxp: VmProt = 0;
                    let kr = mach_vm_remap(
                        mach_task_self(),
                        &mut dst,
                        hp,
                        0,
                        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                        mach_task_self(),
                        tmp as usize as u64,
                        0,
                        &mut curp,
                        &mut maxp,
                        VM_INHERIT_DEFAULT,
                    );
                    if kr == KERN_SUCCESS && dst == base {
                        libc::mprotect(
                            base as usize as *mut c_void,
                            hp as usize,
                            (*entry).final_prot,
                        );
                        installed = 1;
                    }
                    if !libc::getenv(c"OCERZ_LAZYCHECK".as_ptr()).is_null() && kr == KERN_SUCCESS {
                        let chk = libc::mmap(
                            ptr::null_mut(),
                            hp as usize,
                            libc::PROT_READ | libc::PROT_WRITE,
                            libc::MAP_PRIVATE | libc::MAP_ANON,
                            -1,
                            0,
                        );
                        if chk != libc::MAP_FAILED
                            && libc::pread(
                                (*entry).fd,
                                chk,
                                hp as usize,
                                (*entry).foff.wrapping_add(off) as libc::off_t,
                            ) == got
                        {
                            let mut k = 0u32;
                            while k < per {
                                let pb = base.wrapping_add(
                                    (k as u64).wrapping_mul((*entry).page_size as u64),
                                );
                                if pb.wrapping_add((*entry).page_size as u64)
                                    > region_addr.wrapping_add((*entry).size)
                                {
                                    break;
                                }
                                rebase_page_v2(
                                    (chk as usize as u64).wrapping_add(
                                        (k as u64).wrapping_mul((*entry).page_size as u64),
                                    ),
                                    (*entry).page_size as u64,
                                    (pb.wrapping_sub(region_addr) / (*entry).page_size as u64)
                                        as u32,
                                    (*entry).cache_base,
                                    (*entry).si,
                                );
                                k = k.wrapping_add(1);
                            }
                            let rc =
                                libc::memcmp(chk, base as usize as *const c_void, got as usize);
                            libc::fprintf(
                                crate::log::stderr(),
                                c"ocerz: LAZYCHECK[%d] page=%#llx region=%d off=%#llx got=%zd %s\n"
                                    .as_ptr(),
                                libc::getpid(),
                                base as c_ulonglong,
                                i,
                                off as c_ulonglong,
                                got,
                                if rc != 0 {
                                    c"MISMATCH".as_ptr()
                                } else {
                                    c"ok".as_ptr()
                                },
                            );
                        }
                        if chk != libc::MAP_FAILED {
                            libc::munmap(chk, hp as usize);
                        }
                    }
                }
                libc::munmap(tmp, hp as usize);
            }
            if installed == 0 {
                libc::mprotect(
                    base as usize as *mut c_void,
                    hp as usize,
                    libc::PROT_READ | libc::PROT_WRITE,
                );
                let mut k = 0u32;
                while k < per {
                    let pb = base.wrapping_add((k as u64).wrapping_mul((*entry).page_size as u64));
                    if pb.wrapping_add((*entry).page_size as u64)
                        > region_addr.wrapping_add((*entry).size)
                    {
                        break;
                    }
                    rebase_page_v2(
                        pb,
                        (*entry).page_size as u64,
                        (pb.wrapping_sub(region_addr) / (*entry).page_size as u64) as u32,
                        (*entry).cache_base,
                        (*entry).si,
                    );
                    k = k.wrapping_add(1);
                }
                if (*entry).final_prot != (libc::PROT_READ | libc::PROT_WRITE) {
                    libc::mprotect(
                        base as usize as *mut c_void,
                        hp as usize,
                        (*entry).final_prot,
                    );
                }
            }
            (*entry).done.add(hidx).write(1);
            G_LAZY_LOCK.store(0, Ordering::Release);
            return 1;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_prefork() {
    unsafe { while G_LAZY_LOCK.swap(1, Ordering::Acquire) != 0 {} }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_postfork() {
    unsafe {
        G_LAZY_LOCK.store(0, Ordering::Release);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_lazy_region(addr: usize) -> c_int {
    unsafe {
        let n_lazy = ptr::addr_of!(G_N_LAZY).read();
        let lazy = ptr::addr_of!(G_LAZY).cast::<LazyRegion>();
        let mut i = 0;
        while i < n_lazy {
            let entry = lazy.add(i as usize);
            if (addr as u64).wrapping_sub((*entry).addr) < (*entry).size {
                return 1;
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

#[inline]
unsafe fn cmap_find(addr: usize) -> c_int {
    unsafe {
        let lo = ptr::addr_of!(G_CMAP_LO).read();
        let hi = ptr::addr_of!(G_CMAP_HI).read();
        if (addr as u64).wrapping_sub(lo) >= hi.wrapping_sub(lo) {
            return -1;
        }
        let count = ptr::addr_of!(G_N_CMAP).read();
        let cmap = ptr::addr_of!(G_CMAP).cast::<CMap>();
        let mut i = 0;
        while i < count {
            let entry = cmap.add(i as usize);
            if (addr as u64).wrapping_sub((*entry).addr) < (*entry).size {
                return i;
            }
            i = i.wrapping_add(1);
        }
        -1
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_region(addr: usize) -> c_int {
    unsafe { (cmap_find(addr) >= 0) as c_int }
}

unsafe fn watch_slot(addr: usize, alloc: c_int) -> *mut u8 {
    unsafe {
        let i = cmap_find(addr);
        if i < 0 {
            return ptr::null_mut();
        }
        let entry = ptr::addr_of_mut!(G_CMAP).cast::<CMap>().add(i as usize);
        let mut watch = (*entry).watch.load(Ordering::Acquire);
        if watch.is_null() {
            if alloc == 0 {
                return ptr::null_mut();
            }
            let n = ((*entry).size.wrapping_add(HOST_PAGE_SIZE - 1) / HOST_PAGE_SIZE) as usize;
            watch = libc::calloc(n, 1).cast::<u8>();
            if watch.is_null() {
                return ptr::null_mut();
            }
            match (*entry).watch.compare_exchange(
                ptr::null_mut(),
                watch,
                Ordering::Release,
                Ordering::Acquire,
            ) {
                Ok(_) => {}
                Err(had) => {
                    libc::free(watch.cast());
                    watch = had;
                }
            }
        }
        watch.add(((addr as u64).wrapping_sub((*entry).addr) / HOST_PAGE_SIZE) as usize)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_protect(addr: usize, len: u64, prot: c_int) -> c_int {
    unsafe {
        let hp = HOST_PAGE_SIZE;
        let lo = (addr as u64) & !(hp - 1);
        let hi = (addr as u64).wrapping_add(len).wrapping_add(hp - 1) & !(hp - 1);
        if hi <= lo {
            return libc::EINVAL;
        }
        let mut prot = prot & !libc::PROT_EXEC;
        if prot == 0 {
            prot = libc::PROT_READ;
        }
        let mut p = lo;
        while p < hi {
            if ocerz_cache_lazy_region(p as usize) != 0 {
                ocerz_cache_lazy_fault(p as usize);
            }
            p = p.wrapping_add(hp);
        }
        if libc::mprotect(
            lo as usize as *mut c_void,
            hi.wrapping_sub(lo) as usize,
            prot,
        ) != 0
        {
            return *libc::__error();
        }
        if prot & libc::PROT_WRITE != 0 {
            p = lo;
            while p < hi {
                let slot = watch_slot(p as usize, 1);
                if !slot.is_null() {
                    AtomicU8::from_ptr(slot).store(WATCH_WRITABLE, Ordering::Release);
                }
                p = p.wrapping_add(hp);
            }
            G_ANY_WATCH.store(1, Ordering::Release);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_write_fault(addr: usize) -> c_int {
    unsafe {
        if G_ANY_WATCH.load(Ordering::Acquire) == 0 {
            return 0;
        }
        let slot = watch_slot(addr, 0);
        if slot.is_null() || AtomicU8::from_ptr(slot).load(Ordering::Acquire) == 0 {
            return 0;
        }
        let page = (addr as u64) & !(HOST_PAGE_SIZE - 1);
        if libc::mprotect(
            page as usize as *mut c_void,
            HOST_PAGE_SIZE as usize,
            libc::PROT_READ | libc::PROT_WRITE,
        ) != 0
        {
            return 0;
        }
        AtomicU8::from_ptr(slot).store(WATCH_WRITABLE, Ordering::Release);
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_arm_exec(lo: u64, hi: u64) {
    unsafe {
        if G_ANY_WATCH.load(Ordering::Acquire) == 0 {
            return;
        }
        let hp = HOST_PAGE_SIZE;
        if hi <= lo || hi.wrapping_sub(lo) > (1u64 << 20) {
            return;
        }
        let mut p = lo & !(hp - 1);
        while p < hi {
            let slot = watch_slot(p as usize, 0);
            if slot.is_null() || AtomicU8::from_ptr(slot).load(Ordering::Acquire) != WATCH_WRITABLE
            {
                p = p.wrapping_add(hp);
                continue;
            }
            if libc::mprotect(p as usize as *mut c_void, hp as usize, libc::PROT_READ) == 0 {
                AtomicU8::from_ptr(slot).store(WATCH_ARMED, Ordering::Release);
            }
            p = p.wrapping_add(hp);
        }
    }
}

pub(super) unsafe fn map_subcache(
    path: *const c_char,
    is_main: c_int,
    c: *mut OcerzCache,
) -> c_int {
    unsafe { map_subcache_impl(path, is_main, c) }
}

unsafe fn subcache_f2a(hdr: *const u8, rec_off: u32, rec_cnt: u32, foff: u64) -> u64 {
    unsafe {
        let mut i = 0u32;
        while i < rec_cnt {
            let off = rec_off.wrapping_add(i.wrapping_mul(56)) as usize;
            let m = hdr.add(off);
            let a = rd64(m);
            let s = rd64(m.add(8));
            let fo = rd64(m.add(16));
            if foff >= fo && foff < fo.wrapping_add(s) {
                return a.wrapping_add(foff.wrapping_sub(fo));
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

unsafe fn map_subcache_impl(path: *const c_char, is_main: c_int, c: *mut OcerzCache) -> c_int {
    unsafe {
        let fd = libc::open(path, libc::O_RDONLY);
        if fd < 0 {
            return -1;
        }
        let hdr = ptr::addr_of_mut!(SUBCACHE_HDR).cast::<u8>();
        if libc::pread(fd, hdr.cast(), 0x400, 0) != 0x400 {
            libc::close(fd);
            return -1;
        }
        let rec_off = rd32(hdr.add(0x138));
        let rec_cnt = rd32(hdr.add(0x13c));
        if rec_off == 0 || rec_cnt == 0 || rec_cnt > 8 {
            libc::close(fd);
            return -1;
        }
        let mut slide_regions = [[0u64; 6]; 8];
        let mut n_slide = 0usize;
        let mut i = 0u32;
        while i < rec_cnt {
            let m = hdr.add(rec_off.wrapping_add(i.wrapping_mul(56)) as usize);
            let addr = rd64(m);
            let size = rd64(m.add(8));
            let foff = rd64(m.add(16));
            let slide_off = rd64(m.add(24));
            let slide_size = rd64(m.add(32));
            let initp = rd32(m.add(52));
            let cml = libc::getenv(c"OCERZ_CACHEMAPLOG".as_ptr());
            if !cml.is_null() {
                let of = if *cml != 0 {
                    libc::strtoull(cml, ptr::null_mut(), 0) as u64
                } else {
                    0
                };
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: CMAP %s map%u addr=%#llx size=%#llx foff=%#llx slide_off=%#llx slide_size=%#llx initp=%#x %s\n"
                        .as_ptr(),
                    if is_main != 0 {
                        c"main".as_ptr()
                    } else {
                        c"sub".as_ptr()
                    },
                    i as c_uint,
                    addr as c_ulonglong,
                    size as c_ulonglong,
                    foff as c_ulonglong,
                    slide_off as c_ulonglong,
                    slide_size as c_ulonglong,
                    initp,
                    if of != 0 && of >= addr && of < addr.wrapping_add(size) {
                        c"<-- contains address of interest".as_ptr()
                    } else {
                        c"".as_ptr()
                    },
                );
            }
            let mut prot = 0;
            if initp & VM_PROT_READ as u32 != 0 {
                prot |= libc::PROT_READ;
            }
            if initp & VM_PROT_WRITE as u32 != 0 {
                prot |= libc::PROT_READ | libc::PROT_WRITE;
            }
            if initp & VM_PROT_EXECUTE as u32 != 0 {
                prot |= libc::PROT_READ;
            }
            if slide_size != 0 {
                prot |= libc::PROT_READ | libc::PROT_WRITE;
            }
            if prot == 0 {
                prot = libc::PROT_READ;
            }
            let eager = ptr::addr_of_mut!(EAGER);
            if eager.read() < 0 {
                eager.write(if libc::getenv(c"OCERZ_EAGER_SLIDE".as_ptr()).is_null() {
                    0
                } else {
                    1
                });
            }
            let n_lazy = ptr::addr_of!(G_N_LAZY).read();
            let lazy = slide_size != 0 && eager.read() == 0 && n_lazy < LAZY_MAX;
            let map_prot = if lazy { libc::PROT_NONE } else { prot };
            let mapped = libc::mmap(
                addr as usize as *mut c_void,
                size as usize,
                map_prot,
                libc::MAP_PRIVATE | libc::MAP_FIXED,
                fd,
                foff as libc::off_t,
            );
            if mapped != addr as usize as *mut c_void {
                crate::ocerz_fatal!(
                    "cache mapping %u of %s failed (%p want %#llx)\n",
                    i as c_uint,
                    path,
                    mapped,
                    addr as c_ulonglong
                );
                libc::close(fd);
                return -1;
            }
            let n_cmap = ptr::addr_of!(G_N_CMAP).read();
            if n_cmap < CMAP_MAX {
                let entry = ptr::addr_of_mut!(G_CMAP)
                    .cast::<CMap>()
                    .add(n_cmap as usize);
                (*entry).addr = addr;
                (*entry).size = size;
                ptr::addr_of_mut!(G_N_CMAP).write(n_cmap.wrapping_add(1));
                let lo = ptr::addr_of_mut!(G_CMAP_LO);
                if addr < lo.read() {
                    lo.write(addr);
                }
                let hi = ptr::addr_of_mut!(G_CMAP_HI);
                if addr.wrapping_add(size) > hi.read() {
                    hi.write(addr.wrapping_add(size));
                }
            }
            if is_main != 0 && i == 0 {
                (*c).base = addr;
                (*c).hdr = addr as usize as *const u8;
            }
            if slide_size != 0 {
                let row = slide_regions.as_mut_ptr().add(n_slide).cast::<u64>();
                row.add(0).write(addr);
                row.add(1).write(slide_off);
                row.add(2).write(size);
                row.add(3).write(lazy as u64);
                row.add(4).write(if initp & VM_PROT_WRITE as u32 != 0 {
                    (libc::PROT_READ | libc::PROT_WRITE) as u64
                } else {
                    libc::PROT_READ as u64
                });
                row.add(5).write(foff);
                n_slide += 1;
            }
            i = i.wrapping_add(1);
        }
        let cache_base = (*c).base;
        let mut i = 0usize;
        while i < n_slide {
            let row = slide_regions.as_mut_ptr().add(i).cast::<u64>();
            let si_addr = subcache_f2a(hdr, rec_off, rec_cnt, row.add(1).read());
            if si_addr == 0 {
                i += 1;
                continue;
            }
            let si = si_addr as usize as *const u8;
            if rd32(si) != 2 {
                i += 1;
                continue;
            }
            if row.add(3).read() != 0 {
                let addr = row.add(0).read();
                let size = row.add(2).read();
                let hp = HOST_PAGE_SIZE;
                let npages = size.wrapping_add(hp - 1).wrapping_div(hp) as usize;
                let n_lazy = ptr::addr_of!(G_N_LAZY).read();
                let entry = ptr::addr_of_mut!(G_LAZY)
                    .cast::<LazyRegion>()
                    .add(n_lazy as usize);
                (*entry).addr = addr;
                (*entry).size = size;
                (*entry).page_size = rd32(si.add(4));
                (*entry).si = si;
                (*entry).cache_base = cache_base;
                (*entry).final_prot = row.add(4).read() as c_int;
                (*entry).fd = libc::dup(fd);
                (*entry).foff = row.add(5).read();
                (*entry).done = libc::calloc(npages, 1).cast::<u8>();
                if !(*entry).done.is_null() {
                    ptr::addr_of_mut!(G_N_LAZY).write(n_lazy.wrapping_add(1));
                } else {
                    rebase_slide_v2(addr, size, cache_base, si);
                }
            } else {
                rebase_slide_v2(row.add(0).read(), row.add(2).read(), cache_base, si);
            }
            i += 1;
        }
        libc::close(fd);
        0
    }
}

const CACHE_STEM: *const c_char = c"dyld_shared_cache_x86_64".as_ptr();
const CACHE_DIRS: [*const c_char; 2] = [
    c"/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld/".as_ptr(),
    c"/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/".as_ptr(),
];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_dir() -> *const c_char {
    unsafe {
        let mut path = [0 as c_char; 512];
        let dirs = CACHE_DIRS.as_ptr();
        let mut i = 0usize;
        while i < CACHE_DIRS.len() {
            let dir = *dirs.add(i);
            libc::snprintf(
                path.as_mut_ptr(),
                path.len(),
                c"%s%s".as_ptr(),
                dir,
                CACHE_STEM,
            );
            if libc::access(path.as_ptr(), libc::R_OK) == 0 {
                return dir;
            }
            i += 1;
        }
        ptr::null()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_map(c: *mut OcerzCache) -> c_int {
    unsafe {
        ptr::write_bytes(c, 0, 1);
        let dir = ocerz_cache_dir();
        if dir.is_null() {
            crate::ocerz_fatal!(
                "cannot find the x86_64 shared cache in %s or %s\n",
                *CACHE_DIRS.as_ptr(),
                *CACHE_DIRS.as_ptr().add(1)
            );
            return ffi::OCERZ_EIO;
        }
        let mut path = [0 as c_char; 512];
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s%s".as_ptr(),
            dir,
            CACHE_STEM,
        );
        if map_subcache(path.as_ptr(), 1, c) != 0 {
            crate::ocerz_fatal!("cannot map shared cache %s\n", path.as_ptr());
            return ffi::OCERZ_EIO;
        }
        let mut n = 1;
        while n < 16 {
            libc::snprintf(
                path.as_mut_ptr(),
                path.len(),
                c"%s%s.%02d".as_ptr(),
                dir,
                CACHE_STEM,
                n,
            );
            if map_subcache(path.as_ptr(), 0, c) != 0 {
                break;
            }
            n += 1;
        }
        (*c).images_off = rd32((*c).hdr.add(0x1c0));
        (*c).images_cnt = rd32((*c).hdr.add(0x1c4));
        if (*c).images_cnt == 0 || (*c).images_off == 0 {
            crate::ocerz_fatal!(
                "shared cache image table not found (off=%#x cnt=%u)\n",
                (*c).images_off,
                (*c).images_cnt
            );
            return ffi::OCERZ_EFORMAT;
        }
        (*c).mapped = 1;
        ptr::addr_of_mut!(G_NAMED_CACHE).write(c);
        crate::ocerz_log!(
            "shared cache mapped at %#llx, %u images\n",
            (*c).base as c_ulonglong,
            (*c).images_cnt
        );
        ffi::OCERZ_OK
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_name_for_addr(addr: u64, base_out: *mut u64) -> *const c_char {
    unsafe {
        let c = ptr::addr_of!(G_NAMED_CACHE).read();
        if c.is_null() || (*c).mapped == 0 || (*c).images_cnt == 0 {
            return ptr::null();
        }
        let mut best = 0u64;
        let mut best_path = ptr::null();
        let mut i = 0u32;
        while i < (*c).images_cnt {
            let e = (*c)
                .hdr
                .add((*c).images_off as usize)
                .add((i as usize).wrapping_mul(32));
            let a = rd64(e);
            if a <= addr && a > best {
                best = a;
                best_path = (*c).hdr.add(rd32(e.add(0x18)) as usize).cast::<c_char>();
            }
            i = i.wrapping_add(1);
        }
        if best_path.is_null() {
            return ptr::null();
        }
        if !base_out.is_null() {
            base_out.write(best);
        }
        best_path
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_image_addr(
    c: *mut OcerzCache,
    i: u32,
    path_out: *mut *const c_char,
) -> u64 {
    unsafe {
        if i >= (*c).images_cnt {
            return 0;
        }
        let e = (*c)
            .hdr
            .add((*c).images_off as usize)
            .add((i as usize).wrapping_mul(32));
        let addr = rd64(e);
        if !path_out.is_null() {
            let poff = rd32(e.add(0x18));
            path_out.write((*c).hdr.add(poff as usize).cast::<c_char>());
        }
        addr
    }
}
