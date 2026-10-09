//! Mach trap routing, guest VM operations, region aliasing, and relocation of
//! host VM objects returned through MIG replies.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr;

const VM_REGION_BASIC_INFO_64: c_int = 9;
const VM_REGION_SUBMAP_INFO_64: c_int = 18;
const VM_REGION_SUBMAP_SHORT_INFO_64: c_int = 19;
const VM_FLAGS_FIXED: u32 = 0;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_PROT_EXECUTE: c_int = 4;
const VM_FLAGS_ANYWHERE: u32 = 1;
const VM_FLAGS_OVERWRITE: u32 = 0x4000;
const VM_INHERIT_DEFAULT: c_int = 2;
const VM_MEMORY_IOKIT: u32 = 21;
const CLOCK_UPTIME_RAW: libc::clockid_t = 8;
const OCERZ_SC_UNIVERSE_MAX: usize = 16;
const OCERZ_ALIAS_REG_MAX: usize = 4096;
type MachPort = c_uint;
type MachVmAddress = u64;
type MachVmSize = u64;
type KernReturn = c_int;
type Natural = c_uint;
type MachMsgTypeNumber = c_uint;

#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: c_int,
    shared: c_int,
    reserved: c_int,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}

#[repr(C, packed(4))]
struct VmRegionSubmapInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: c_int,
    offset: u64,
    user_tag: u32,
    pages_resident: u32,
    pages_shared_now_private: u32,
    pages_swapped_out: u32,
    pages_dirtied: u32,
    ref_count: u32,
    shadow_depth: u16,
    external_pager: u8,
    share_mode: u8,
    is_submap: c_int,
    behavior: c_int,
    object_id: u32,
    user_wired_count: u16,
    flags: u16,
    pages_reusable: u32,
    object_id_full: u64,
}

#[repr(C, packed(4))]
struct VmRegionSubmapShortInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: c_int,
    offset: u64,
    user_tag: u32,
    ref_count: u32,
    shadow_depth: u16,
    external_pager: u8,
    share_mode: u8,
    is_submap: c_int,
    behavior: c_int,
    object_id: u32,
    user_wired_count: u16,
    flags: u16,
}

const _: () = {
    assert!(core::mem::size_of::<VmRegionBasicInfo64>() == 36);
    assert!(core::mem::offset_of!(VmRegionBasicInfo64, protection) == 0);
    assert!(core::mem::offset_of!(VmRegionBasicInfo64, offset) == 20);
    assert!(core::mem::offset_of!(VmRegionBasicInfo64, behavior) == 28);
    assert!(core::mem::offset_of!(VmRegionBasicInfo64, user_wired_count) == 32);
    assert!(core::mem::size_of::<VmRegionSubmapInfo64>() == 76);
    assert!(core::mem::offset_of!(VmRegionSubmapInfo64, offset) == 12);
    assert!(core::mem::offset_of!(VmRegionSubmapInfo64, user_tag) == 20);
    assert!(core::mem::offset_of!(VmRegionSubmapInfo64, share_mode) == 47);
    assert!(core::mem::offset_of!(VmRegionSubmapInfo64, object_id) == 56);
    assert!(core::mem::offset_of!(VmRegionSubmapInfo64, object_id_full) == 68);
    assert!(core::mem::size_of::<VmRegionSubmapShortInfo64>() == 48);
    assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, offset) == 12);
    assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, object_id) == 40);
    assert!(core::mem::offset_of!(VmRegionSubmapShortInfo64, flags) == 46);
};

#[derive(Clone, Copy)]
struct ScUniverse {
    uid_key: u64,
    address: u64,
}

#[derive(Clone, Copy)]
struct AliasRegion {
    lo: u64,
    hi: u64,
    object: u32,
}

static mut G_SC_UNIVERSES: [ScUniverse; OCERZ_SC_UNIVERSE_MAX] = [ScUniverse {
    uid_key: 0,
    address: 0,
}; OCERZ_SC_UNIVERSE_MAX];
static mut G_ALIAS_REG: [AliasRegion; OCERZ_ALIAS_REG_MAX] = [AliasRegion {
    lo: 0,
    hi: 0,
    object: 0,
}; OCERZ_ALIAS_REG_MAX];
static mut G_ALIAS_REG_N: u32 = 0;
static mut G_ALIAS_REG_NEXT: u32 = 0;
static mut G_SC_UNIVERSES_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_ALIAS_REG_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe extern "C" {
    static mut mach_task_self_: MachPort;
    fn clock_gettime_nsec_np(clock_id: libc::clockid_t) -> u64;
    fn mach_vm_region(
        target: MachPort,
        address: *mut MachVmAddress,
        size: *mut MachVmSize,
        flavor: c_int,
        info: *mut c_void,
        info_cnt: *mut MachMsgTypeNumber,
        object_name: *mut MachPort,
    ) -> KernReturn;
    fn mach_vm_region_recurse(
        target: MachPort,
        address: *mut MachVmAddress,
        size: *mut MachVmSize,
        nesting_depth: *mut Natural,
        info: *mut c_void,
        info_cnt: *mut MachMsgTypeNumber,
    ) -> KernReturn;
    fn mach_port_deallocate(task: MachPort, name: MachPort) -> KernReturn;
    fn mach_vm_remap(
        target_task: MachPort,
        target_address: *mut MachVmAddress,
        size: MachVmSize,
        mask: MachVmAddress,
        flags: c_int,
        source_task: MachPort,
        source_address: MachVmAddress,
        copy: c_int,
        cur_protection: *mut c_int,
        max_protection: *mut c_int,
        inheritance: c_int,
    ) -> KernReturn;
    fn mach_vm_deallocate(task: MachPort, address: MachVmAddress, size: MachVmSize) -> KernReturn;
    fn mach_vm_allocate(
        task: MachPort,
        address: *mut MachVmAddress,
        size: MachVmSize,
        flags: c_int,
    ) -> KernReturn;
    fn mach_vm_protect(
        task: MachPort,
        address: MachVmAddress,
        size: MachVmSize,
        set_maximum: c_int,
        protection: c_int,
    ) -> KernReturn;
    fn mach_port_type(task: MachPort, name: MachPort, port_type: *mut MachPort) -> KernReturn;
}

#[inline(always)]
unsafe fn mach_task_self() -> MachPort {
    mach_task_self_
}

unsafe fn mach_trap_name(num: c_int) -> *const c_char {
    match num {
        10 => c"_kernelrpc_mach_vm_allocate_trap".as_ptr(),
        11 => c"_kernelrpc_mach_vm_purgable_control_trap".as_ptr(),
        12 => c"_kernelrpc_mach_vm_deallocate_trap".as_ptr(),
        14 => c"_kernelrpc_mach_vm_protect_trap".as_ptr(),
        15 => c"_kernelrpc_mach_vm_map_trap".as_ptr(),
        16 => c"_kernelrpc_mach_port_allocate_trap".as_ptr(),
        18 => c"_kernelrpc_mach_port_deallocate_trap".as_ptr(),
        19 => c"_kernelrpc_mach_port_mod_refs_trap".as_ptr(),
        26 => c"mach_reply_port".as_ptr(),
        40 => c"_kernelrpc_mach_port_get_attributes_trap".as_ptr(),
        27 => c"thread_self_trap".as_ptr(),
        28 => c"task_self_trap".as_ptr(),
        29 => c"host_self_trap".as_ptr(),
        31 => c"mach_msg_trap".as_ptr(),
        33 => c"semaphore_signal_trap".as_ptr(),
        34 => c"semaphore_signal_all_trap".as_ptr(),
        36 => c"semaphore_wait_trap".as_ptr(),
        37 => c"semaphore_wait_signal_trap".as_ptr(),
        38 => c"semaphore_timedwait_trap".as_ptr(),
        43 => c"mach_generate_activity_id".as_ptr(),
        44 => c"task_name_for_pid".as_ptr(),
        45 => c"task_for_pid".as_ptr(),
        46 => c"pid_for_task".as_ptr(),
        47 => c"mach_msg2_trap".as_ptr(),
        50 => c"thread_get_special_reply_port".as_ptr(),
        76 => c"_kernelrpc_mach_port_type_trap".as_ptr(),
        77 => c"_kernelrpc_mach_port_request_notification_trap".as_ptr(),
        59 => c"swtch_pri".as_ptr(),
        60 => c"swtch".as_ptr(),
        61 => c"thread_switch".as_ptr(),
        89 => c"mach_timebase_info_trap".as_ptr(),
        90 => c"mach_wait_until_trap".as_ptr(),
        91 => c"mk_timer_create_trap".as_ptr(),
        92 => c"mk_timer_destroy_trap".as_ptr(),
        93 => c"mk_timer_arm_trap".as_ptr(),
        94 => c"mk_timer_cancel_trap".as_ptr(),
        95 => c"mk_timer_arm_leeway_trap".as_ptr(),
        _ => ptr::null(),
    }
}

unsafe fn ocerz_sc_remember_universe(uid: u32, address: u64) {
    let key = uid as u64 + 1;
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
    let slots = ptr::addr_of_mut!(G_SC_UNIVERSES).cast::<ScUniverse>();
    for i in 0..OCERZ_SC_UNIVERSE_MAX {
        let slot = &mut *slots.add(i);
        if slot.uid_key == key {
            slot.address = address;
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
            return;
        }
    }
    for i in 0..OCERZ_SC_UNIVERSE_MAX {
        let slot = &mut *slots.add(i);
        if slot.uid_key == 0 {
            slot.address = address;
            slot.uid_key = key;
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
            return;
        }
    }
    let slot = &mut *slots.add(uid as usize % OCERZ_SC_UNIVERSE_MAX);
    slot.uid_key = 0;
    slot.address = address;
    slot.uid_key = key;
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
}

unsafe fn ocerz_sc_find_universe(uid: u32) -> u64 {
    let key = uid as u64 + 1;
    let mut address = 0;
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
    let slots = ptr::addr_of!(G_SC_UNIVERSES).cast::<ScUniverse>();
    for i in 0..OCERZ_SC_UNIVERSE_MAX {
        let slot = &*slots.add(i);
        if slot.uid_key == key {
            address = slot.address;
            break;
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SC_UNIVERSES_LOCK));
    address
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_host_region_is_device(addr: u64, prot_out: *mut c_int) -> c_int {
    let mut address = addr;
    let mut size = 0;
    let mut depth = 0;
    let mut info: VmRegionSubmapInfo64 = core::mem::zeroed();
    let mut count = (core::mem::size_of::<VmRegionSubmapInfo64>() / 4) as u32;
    if mach_vm_region_recurse(
        mach_task_self(),
        &mut address,
        &mut size,
        &mut depth,
        (&mut info as *mut VmRegionSubmapInfo64).cast(),
        &mut count,
    ) != 0
        || address > addr
        || addr - address >= size
    {
        return 0;
    }
    let protection = ptr::read_unaligned(ptr::addr_of!(info.protection));
    let share_mode = ptr::read_unaligned(ptr::addr_of!(info.share_mode));
    let user_tag = ptr::read_unaligned(ptr::addr_of!(info.user_tag));
    if !prot_out.is_null() {
        *prot_out = protection;
    }
    let shared = share_mode == 4 || share_mode == 5 || share_mode == 7;
    let device = user_tag == VM_MEMORY_IOKIT || user_tag == 81 || user_tag == 70;
    (shared || device) as c_int * ((protection & VM_PROT_READ != 0) as c_int)
}

unsafe fn host_region_entry(
    address: u64,
    start: *mut u64,
    end: *mut u64,
    object: *mut u32,
    protection: *mut c_int,
) -> bool {
    let mut region = address;
    let mut size = 0;
    let mut depth = 0;
    let mut info: VmRegionSubmapShortInfo64 = core::mem::zeroed();
    let mut count = (core::mem::size_of::<VmRegionSubmapShortInfo64>() / 4) as u32;
    if mach_vm_region_recurse(
        mach_task_self(),
        &mut region,
        &mut size,
        &mut depth,
        (&mut info as *mut VmRegionSubmapShortInfo64).cast(),
        &mut count,
    ) != 0
        || size == 0
    {
        return false;
    }
    *start = region;
    *end = region.wrapping_add(size);
    *object = ptr::read_unaligned(ptr::addr_of!(info.object_id));
    *protection = ptr::read_unaligned(ptr::addr_of!(info.protection));
    true
}

unsafe fn alias_reg_add(lo: u64, hi: u64, object: u32) {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_ALIAS_REG_LOCK));
    let mut n = G_ALIAS_REG_N;
    let entries = ptr::addr_of_mut!(G_ALIAS_REG).cast::<AliasRegion>();
    let mut i = 0;
    while i < n {
        let entry = &mut *entries.add(i as usize);
        if entry.lo >= lo && entry.hi <= hi {
            n -= 1;
            *entry = *entries.add(n as usize);
        } else {
            i += 1;
        }
    }
    G_ALIAS_REG_N = n;
    let index = if n < OCERZ_ALIAS_REG_MAX as u32 {
        G_ALIAS_REG_N += 1;
        n
    } else {
        let next = G_ALIAS_REG_NEXT;
        G_ALIAS_REG_NEXT = next.wrapping_add(1);
        next % OCERZ_ALIAS_REG_MAX as u32
    };
    *entries.add(index as usize) = AliasRegion { lo, hi, object };
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ALIAS_REG_LOCK));
}

unsafe fn alias_reg_find(address: u64, object: u32, any_object: bool) -> bool {
    let mut hit = false;
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_ALIAS_REG_LOCK));
    let entries = ptr::addr_of!(G_ALIAS_REG).cast::<AliasRegion>();
    for i in 0..G_ALIAS_REG_N {
        let entry = &*entries.add(i as usize);
        hit = address >= entry.lo
            && address < entry.hi
            && (any_object || (object != 0 && entry.object == object));
        if hit {
            break;
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ALIAS_REG_LOCK));
    hit
}

unsafe fn alias_page_ours(page: u64) -> bool {
    let mut start = 0;
    let mut end = 0;
    let mut object = 0;
    let mut protection = 0;
    let host = ocerz_g2h(page) as u64;
    alias_reg_find(page, 0, true)
        && host_region_entry(host, &mut start, &mut end, &mut object, &mut protection)
        && start <= host
        && alias_reg_find(page, object, false)
}

unsafe fn alias_page_taken(page: u64) -> bool {
    let mut at = page;
    while at < page.wrapping_add(0x4000) {
        if crate::ffi::ocerz_addr_prot(at) > 0 {
            return !alias_page_ours(page);
        }
        at = at.wrapping_add(crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64);
    }
    false
}

unsafe fn alias_raw_region(vm: *mut OcerzVM, pointer: u64, refresh: bool) -> c_int {
    if pointer == 0 {
        return -1;
    }
    let mut region = pointer;
    let mut size = 0;
    let mut info: VmRegionBasicInfo64 = core::mem::zeroed();
    let mut count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
    let mut object_name = 0;
    let kr = mach_vm_region(
        mach_task_self(),
        &mut region,
        &mut size,
        VM_REGION_BASIC_INFO_64,
        (&mut info as *mut VmRegionBasicInfo64).cast(),
        &mut count,
        &mut object_name,
    );
    if object_name != 0 {
        mach_port_deallocate(mach_task_self(), object_name);
    }
    if kr != 0 || region > pointer || pointer - region >= size || size == 0 {
        return -1;
    }
    if ocerz_g2h(region) as u64 == region {
        return 0;
    }
    if region >= crate::ffi::OCERZ_LOW_LIMIT || size > crate::ffi::OCERZ_LOW_LIMIT - region {
        return -1;
    }
    static mut NO_ALIAS_CLIP: c_int = -1;
    if NO_ALIAS_CLIP < 0 {
        NO_ALIAS_CLIP = (!libc::getenv(c"OCERZ_NO_ALIAS_CLIP".as_ptr()).is_null()) as c_int;
    }
    if NO_ALIAS_CLIP == 0 {
        let mut lo = pointer & !0x3fff;
        let mut hi = lo.wrapping_add(0x4000);
        let rlo = region;
        let rhi = region.wrapping_add(size);
        let win = 0x1000_0000u64;
        if alias_page_taken(lo) {
            if env_set!("OCERZ_MIGTRACE") {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: ALIASCLIP[%d] pointer=%#llx page taken readable=%d\n".as_ptr(),
                    libc::getpid(),
                    pointer as libc::c_ulonglong,
                    crate::ffi::ocerz_addr_readable(pointer),
                );
            }
            return if crate::ffi::ocerz_addr_readable(pointer) != 0 {
                0
            } else {
                -1
            };
        }
        while lo > rlo
            && pointer.wrapping_sub(lo.wrapping_sub(0x4000)) <= win
            && !alias_page_taken(lo.wrapping_sub(0x4000))
        {
            lo = lo.wrapping_sub(0x4000);
        }
        while hi < rhi
            && hi.wrapping_add(0x4000).wrapping_sub(pointer) <= win
            && !alias_page_taken(hi)
        {
            hi = hi.wrapping_add(0x4000);
        }
        if env_set!("OCERZ_MIGTRACE") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: ALIASCLIP[%d] pointer=%#llx region=%#llx+%#llx -> %#llx..%#llx\n".as_ptr(),
                libc::getpid(),
                pointer as libc::c_ulonglong,
                rlo as libc::c_ulonglong,
                rhi.wrapping_sub(rlo) as libc::c_ulonglong,
                lo as libc::c_ulonglong,
                hi as libc::c_ulonglong,
            );
        }
        region = if lo > rlo { lo } else { rlo };
        size = (if hi < rhi { hi } else { rhi }).wrapping_sub(region);
    }
    let guest = region;
    let host_dst = ocerz_g2h(guest) as u64;
    if !refresh
        && crate::ffi::ocerz_addr_readable(guest) != 0
        && crate::ffi::ocerz_addr_readable(guest.wrapping_add(size - 1)) != 0
    {
        return 0;
    }
    super::mem::invalidate_guest_mapping(vm, guest, size);
    let protection = ptr::read_unaligned(ptr::addr_of!(info.protection))
        & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    if crate::ffi::ocerz_map_fixed(guest, size, protection) != crate::ffi::OCERZ_OK as c_int {
        return -1;
    }
    let mut dst = host_dst;
    let mut current = 0;
    let mut maximum = 0;
    crate::ffi::ocerz_jit_require_ordered(vm);
    let kr = mach_vm_remap(
        mach_task_self(),
        &mut dst,
        size,
        0,
        (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) as c_int,
        mach_task_self(),
        region,
        0,
        &mut current,
        &mut maximum,
        VM_INHERIT_DEFAULT,
    );
    if kr != 0 || dst != host_dst {
        crate::ffi::ocerz_unmap(guest, size);
        return -1;
    }
    let mut start = 0;
    let mut end = 0;
    let mut object = 0;
    let mut prot = 0;
    if host_region_entry(host_dst, &mut start, &mut end, &mut object, &mut prot)
        && start <= host_dst
        && object != 0
    {
        alias_reg_add(guest, guest.wrapping_add(size), object);
    }
    crate::ffi::ocerz_low_fill_host_holes();
    if env_set!("OCERZ_MIGTRACE") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SCALIAS pointer=%#llx raw=%#llx size=%#llx shadow=%#llx prot=%d/%d\n".as_ptr(),
            pointer as libc::c_ulonglong,
            region as libc::c_ulonglong,
            size as libc::c_ulonglong,
            host_dst as libc::c_ulonglong,
            current,
            maximum,
        );
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_alias_raw_region(vm: *mut OcerzVM, pointer: u64) -> c_int {
    alias_raw_region(vm, pointer, false)
}

unsafe fn ocerz_alias_raw_contiguous(vm: *mut OcerzVM, pointer: u64) -> c_int {
    let mut first = pointer;
    let mut first_size = 0;
    let mut first_info: VmRegionBasicInfo64 = core::mem::zeroed();
    let mut first_count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
    let mut first_object = 0;
    let kr = mach_vm_region(
        mach_task_self(),
        &mut first,
        &mut first_size,
        VM_REGION_BASIC_INFO_64,
        (&mut first_info as *mut VmRegionBasicInfo64).cast(),
        &mut first_count,
        &mut first_object,
    );
    if first_object != 0 {
        mach_port_deallocate(mach_task_self(), first_object);
    }
    if kr != 0 || first > pointer || pointer - first >= first_size {
        return -1;
    }
    let mut pos = first;
    let limit = pos.wrapping_add(0x0400_0000);
    for _ in 0..4096 {
        if pos >= limit {
            break;
        }
        let mut region = pos;
        let mut size = 0;
        let mut info: VmRegionBasicInfo64 = core::mem::zeroed();
        let mut count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
        let mut object = 0;
        let kr = mach_vm_region(
            mach_task_self(),
            &mut region,
            &mut size,
            VM_REGION_BASIC_INFO_64,
            (&mut info as *mut VmRegionBasicInfo64).cast(),
            &mut count,
            &mut object,
        );
        if object != 0 {
            mach_port_deallocate(mach_task_self(), object);
        }
        if kr != 0 || region != pos || size == 0 || size > limit - pos {
            break;
        }
        if ocerz_alias_raw_region(vm, pos) != 0 {
            return -1;
        }
        pos = pos.wrapping_add(size);
    }
    if crate::ffi::ocerz_addr_readable(pointer) != 0 {
        0
    } else {
        -1
    }
}

unsafe fn guest_vm_allocate_apply(vm: *mut OcerzVM, addrp: u64, size: u64, flags: u64) -> c_int {
    if flags & 1 == 0 {
        let want = if addrp != 0 { ocerz_ld(addrp, 8) } else { 0 };
        super::mem::memtrace(c"vm_alloc".as_ptr(), want, size, 0, flags as c_int);
        super::mem::invalidate_guest_mapping(vm, want, size);
        if want == 0
            || (crate::ffi::ocerz_map_claim_fixed(want, size, libc::PROT_READ | libc::PROT_WRITE)
                != crate::ffi::OCERZ_OK as c_int
                && crate::ffi::ocerz_map_claim_region(
                    want,
                    size,
                    libc::PROT_READ | libc::PROT_WRITE,
                ) != crate::ffi::OCERZ_OK as c_int
                && (crate::ffi::ocerz_mem_register_range(want, want + size)
                    != crate::ffi::OCERZ_OK as c_int
                    || crate::ffi::ocerz_map_claim_region(
                        want,
                        size,
                        libc::PROT_READ | libc::PROT_WRITE,
                    ) != crate::ffi::OCERZ_OK as c_int))
        {
            if !vm.is_null() && (*vm).strace != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: mach_vm_allocate FIXED denied want=%#llx size=%#llx flags=%#llx\n"
                        .as_ptr(),
                    want as libc::c_ulonglong,
                    size as libc::c_ulonglong,
                    flags as libc::c_ulonglong,
                );
            }
            return OCERZ_MACH_KERN_NO_SPACE;
        }
        return OCERZ_MACH_KERN_SUCCESS;
    }
    let gaddr = crate::ffi::ocerz_map_anywhere(size, libc::PROT_READ | libc::PROT_WRITE);
    if gaddr == 0 {
        return OCERZ_MACH_KERN_NO_SPACE;
    }
    super::mem::invalidate_guest_mapping(vm, gaddr, size);
    if addrp != 0 {
        ocerz_st(addrp, 8, gaddr);
    }
    OCERZ_MACH_KERN_SUCCESS
}

unsafe fn guest_vm_deallocate_apply(vm: *mut OcerzVM, addr: u64, size: u64) -> c_int {
    super::mem::memtrace(c"vm_dealloc".as_ptr(), addr, size, 0, 0);
    super::mem::invalidate_guest_mapping(vm, addr, size);
    crate::ffi::ocerz_unmap(addr, size);
    OCERZ_MACH_KERN_SUCCESS
}

unsafe fn guest_vm_protect_apply(
    vm: *mut OcerzVM,
    addr: u64,
    size: u64,
    protection: c_int,
) -> c_int {
    super::mem::memtrace(c"vm_protect".as_ptr(), addr, size, protection, 0);
    super::mem::invalidate_guest_mapping(vm, addr, size);
    crate::ffi::ocerz_protect(addr, size, protection);
    OCERZ_MACH_KERN_SUCCESS
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_vm_allocate(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    task: u64,
    addrp: u64,
    size: u64,
    flags: c_int,
) -> c_int {
    if task != mach_task_self() as u64 {
        return mach_vm_allocate(task as u32, ocerz_g2h(addrp).cast(), size, flags);
    }
    guest_vm_allocate_apply(vm, addrp, size, flags as u32 as u64)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_vm_deallocate(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    task: u64,
    addr: u64,
    size: u64,
) -> c_int {
    if task != mach_task_self() as u64 {
        return mach_vm_deallocate(task as u32, addr, size);
    }
    if crate::ffi::ocerz_mem_overlaps(addr, size) == 0 {
        super::mem::invalidate_guest_mapping(vm, addr, size);
        return mach_vm_deallocate(mach_task_self(), ocerz_g2h(addr) as u64, size);
    }
    guest_vm_deallocate_apply(vm, addr, size)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_vm_protect(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    task: u64,
    addr: u64,
    size: u64,
    set_maximum: c_int,
    protection: c_int,
) -> c_int {
    if task != mach_task_self() as u64 {
        return mach_vm_protect(task as u32, addr, size, set_maximum, protection);
    }
    if crate::ffi::ocerz_mem_overlaps(addr, size) == 0 {
        super::mem::invalidate_guest_mapping(vm, addr, size);
        return mach_vm_protect(
            mach_task_self(),
            ocerz_g2h(addr) as u64,
            size,
            set_maximum,
            protection,
        );
    }
    guest_vm_protect_apply(vm, addr, size, protection)
}

unsafe fn mig_vm_refuse_taken(reply: u64, size: u64) {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_NO_VMMAP_TAKEN".as_ptr()).is_null()) as c_int;
    }
    let host = ocerz_ld(reply.wrapping_add(0x24), 8);
    if OFF != 0
        || crate::ffi::ocerz_low_base == 0
        || size == 0
        || host >= crate::ffi::OCERZ_LOW_LIMIT
        || size > crate::ffi::OCERZ_LOW_LIMIT - host
    {
        return;
    }
    let mut page = host & !((crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64) - 1);
    while page < host.wrapping_add(size) {
        if crate::ffi::ocerz_addr_committed(page) == 1 {
            mach_vm_deallocate(mach_task_self(), host, size);
            ocerz_st(
                reply.wrapping_add(0x20),
                4,
                OCERZ_MACH_KERN_NO_SPACE as u32 as u64,
            );
            ocerz_st(reply.wrapping_add(0x24), 8, 0);
            return;
        }
        page = page.wrapping_add(crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64);
    }
}

unsafe fn mig_vm_reply_relocate(
    vm: *mut OcerzVM,
    reply: u64,
    preserve_address: bool,
    requested_size: u64,
    alignment: u64,
) {
    let host = ocerz_ld(reply.wrapping_add(0x24), 8);
    if host == 0
        || (host >= crate::ffi::ocerz_arena_lo && host < crate::ffi::ocerz_arena_hi)
        || crate::ffi::ocerz_cache_region(host as usize) != 0
        || (crate::ffi::ocerz_low_base != 0
            && host >= crate::ffi::OCERZ_LOW_LIMIT
            && host < crate::ffi::OCERZ_TOP_LO
            && ocerz_g2h(host) as u64 == host
            && crate::ffi::ocerz_addr_committed(host) == 1)
        || (preserve_address && ocerz_g2h(host) as u64 == host)
    {
        return;
    }
    let mut region = host;
    let mut region_size = 0;
    let mut info: VmRegionBasicInfo64 = core::mem::zeroed();
    let mut count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
    let mut object = 0;
    if mach_vm_region(
        mach_task_self(),
        &mut region,
        &mut region_size,
        VM_REGION_BASIC_INFO_64,
        (&mut info as *mut VmRegionBasicInfo64).cast(),
        &mut count,
        &mut object,
    ) != 0
    {
        return;
    }
    if object != 0 {
        mach_port_deallocate(mach_task_self(), object);
    }
    if region > host {
        return;
    }
    let mut region_end = region.wrapping_add(region_size);
    let mut regions = 1;
    if requested_size == 0 {
        for _ in 0..4096 {
            let mut next = region_end;
            let mut next_size = 0;
            let mut next_info: VmRegionBasicInfo64 = core::mem::zeroed();
            let mut next_count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
            let mut next_object = 0;
            if mach_vm_region(
                mach_task_self(),
                &mut next,
                &mut next_size,
                VM_REGION_BASIC_INFO_64,
                (&mut next_info as *mut VmRegionBasicInfo64).cast(),
                &mut next_count,
                &mut next_object,
            ) != 0
                || next != region_end
            {
                if next_object != 0 {
                    mach_port_deallocate(mach_task_self(), next_object);
                }
                break;
            }
            if next_object != 0 {
                mach_port_deallocate(mach_task_self(), next_object);
            }
            region_end = region_end.wrapping_add(next_size);
            regions += 1;
        }
    }
    let size = if requested_size != 0 {
        requested_size.wrapping_add(0x3fff) & !0x3fff
    } else {
        region_end.wrapping_sub(host)
    };
    if size == 0 || size > u64::MAX - host {
        return;
    }
    let mut keep = preserve_address
        || (crate::ffi::ocerz_low_base != 0
            && host < crate::ffi::OCERZ_LOW_LIMIT
            && size <= crate::ffi::OCERZ_LOW_LIMIT - host);
    let guest;
    if keep {
        guest = host;
        super::mem::invalidate_guest_mapping(vm, guest, size);
        if crate::ffi::ocerz_map_claim_region(guest, size, libc::PROT_READ | libc::PROT_WRITE)
            != crate::ffi::OCERZ_OK as c_int
        {
            if preserve_address {
                return;
            }
            keep = false;
        }
    } else {
        guest = 0;
    }
    let guest = if keep {
        guest
    } else {
        let new_address = if alignment != 0 {
            crate::ffi::ocerz_map_anywhere_aligned(
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                alignment,
            )
        } else {
            crate::ffi::ocerz_map_donate(size)
        };
        if new_address == 0 {
            return;
        }
        super::mem::invalidate_guest_mapping(vm, new_address, size);
        new_address
    };
    let host_dst = ocerz_g2h(guest) as u64;
    let mut dst = host_dst;
    let mut current = 0;
    let mut maximum = 0;
    crate::ffi::ocerz_jit_require_ordered(vm);
    let kr = mach_vm_remap(
        mach_task_self(),
        &mut dst,
        size,
        0,
        (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) as c_int,
        mach_task_self(),
        host,
        0,
        &mut current,
        &mut maximum,
        VM_INHERIT_DEFAULT,
    );
    if kr != 0 || dst != host_dst {
        crate::ffi::ocerz_unmap(guest, size);
        return;
    }
    if !keep {
        mach_vm_deallocate(mach_task_self(), host, size);
    }
    ocerz_st(reply.wrapping_add(0x24), 8, guest);
    if env_set!("OCERZ_MIGTRACE") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MIGMOVE id=%u host=%#llx size=%#llx guest=%#llx host_dst=%#llx prot=%d/%d\n"
                .as_ptr(),
            ocerz_ld(reply.wrapping_add(0x14), 4) as u32,
            host as libc::c_ulonglong,
            size as libc::c_ulonglong,
            guest as libc::c_ulonglong,
            host_dst as libc::c_ulonglong,
            current,
            maximum,
        );
        let scan_size = size.min(0x20_0000);
        let mut found = 0;
        let mut offset = 0u64;
        while offset.wrapping_add(8) <= scan_size && found < 32 {
            let value = ocerz_ld(guest.wrapping_add(offset), 8);
            if value >= host && value < host.wrapping_add(size) {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: MIGSELF id=%u off=%#llx value=%#llx delta=%#llx\n".as_ptr(),
                    ocerz_ld(reply.wrapping_add(0x14), 4) as u32,
                    offset as libc::c_ulonglong,
                    value as libc::c_ulonglong,
                    value.wrapping_sub(host) as libc::c_ulonglong,
                );
                found += 1;
            }
            offset += 8;
        }
    }
    if env_set!("OCERZ_MACHMSG") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MIGRELOC id=%u haddr=%#llx raddr=%#llx first_rsize=%#llx coalesced_size=%#llx nregions=%d prot=%d\n"
                .as_ptr(),
            ocerz_ld(reply.wrapping_add(0x14), 4) as u32,
            host as libc::c_ulonglong,
            guest as libc::c_ulonglong,
            region_size as libc::c_ulonglong,
            size as libc::c_ulonglong,
            regions,
            info.protection,
        );
    }
    if (*vm).strace != 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: mig_vm relocate host=%#llx size=%#llx -> guest=%#llx\n".as_ptr(),
            host as libc::c_ulonglong,
            size as libc::c_ulonglong,
            guest as libc::c_ulonglong,
        );
    }
}

unsafe fn ocerz_reply_relocate_ool(reply: u64, recv_size: u32, trap: c_int) {
    let bits = ocerz_ld(reply, 4) as u32;
    if bits & 0x8000_0000 == 0 {
        return;
    }
    let mut size = ocerz_ld(reply.wrapping_add(4), 4) as u32;
    if recv_size != 0 && recv_size < size {
        size = recv_size;
    }
    let count = ocerz_ld(reply.wrapping_add(0x18), 4) as u32;
    if count == 0 || count > 4096 {
        return;
    }
    static mut MACHMSG_LOG: c_int = -1;
    static mut OOLTRACE: c_int = -1;
    if MACHMSG_LOG < 0 {
        MACHMSG_LOG = c_int::from(!libc::getenv(c"OCERZ_MACHMSG".as_ptr()).is_null());
    }
    if OOLTRACE < 0 {
        OOLTRACE = c_int::from(!libc::getenv(c"OCERZ_OOLTRACE".as_ptr()).is_null());
    }
    let reply_id = ocerz_ld(reply.wrapping_add(0x14), 4) as u32;
    let mut off = 0x1c;
    for _ in 0..count {
        if off + 12 > size as u64 {
            break;
        }
        let ty = ocerz_ld(reply.wrapping_add(off).wrapping_add(11), 1) as u8;
        if ty == 0 {
            off += 12;
            continue;
        }
        if ty == 4 {
            off += 16;
            continue;
        }
        if (1..=3).contains(&ty) {
            if off + 16 > size as u64 {
                break;
            }
            let address = ocerz_ld(reply.wrapping_add(off), 8);
            let count = ocerz_ld(reply.wrapping_add(off).wrapping_add(12), 4) as u32;
            let mut descriptor: MachMsgDescriptor = core::mem::zeroed();
            ptr::copy_nonoverlapping(
                ocerz_g2h(reply.wrapping_add(off)).cast::<u8>(),
                (&mut descriptor as *mut MachMsgDescriptor).cast::<u8>(),
                core::mem::size_of::<MachMsgDescriptor>(),
            );
            let bytes = if ty == 2 {
                count as u64 * 4
            } else {
                count as u64
            };
            let host_owned = address != 0
                && !ocerz_host_in_guest_reservation(address as *const c_void)
                && crate::ffi::ocerz_cache_region(address as usize) == 0;
            if MACHMSG_LOG != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: REPLY-OOL(t%d)[%d] type=%u ool_addr=%#llx bytes=%#llx host=%d\n"
                        .as_ptr(),
                    trap,
                    libc::getpid(),
                    ty as c_uint,
                    address as libc::c_ulonglong,
                    bytes as libc::c_ulonglong,
                    host_owned as c_int,
                );
            }
            if host_owned && bytes != 0 {
                let alloc = if bytes <= 64 * 1024 * 1024 {
                    crate::ffi::ocerz_map_anywhere(
                        bytes.wrapping_add(0x3fff) & !0x3fff,
                        libc::PROT_READ | libc::PROT_WRITE,
                    )
                } else {
                    0
                };
                if alloc != 0 {
                    ptr::copy_nonoverlapping(
                        address as *const u8,
                        ocerz_g2h(alloc).cast::<u8>(),
                        bytes as usize,
                    );
                    ocerz_st(reply.wrapping_add(off), 8, alloc);
                    if OOLTRACE != 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: OOLTRACE-COPY(t%d)[%d] id=%u type=%u gb=%#llx..%#llx bytes=%#llx\n"
                                .as_ptr(),
                            trap,
                            libc::getpid(),
                            reply_id,
                            ty as c_uint,
                            alloc as libc::c_ulonglong,
                            alloc.wrapping_add(bytes) as libc::c_ulonglong,
                            bytes as libc::c_ulonglong,
                        );
                    }
                } else {
                    ocerz_st(reply.wrapping_add(off), 8, 0);
                    ocerz_st(reply.wrapping_add(off).wrapping_add(12), 4, 0);
                }
                super::machmsg::ocerz_release_received_ool(
                    &descriptor,
                    ty,
                    address,
                    bytes,
                    (alloc != 0) as c_int,
                );
            }
            off += 16;
            continue;
        }
        if OOLTRACE != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OOLTRACE-BREAK(t%d)[%d] id=%u off=%#llx type=%u (remaining desc left raw)\n"
                    .as_ptr(),
                trap,
                libc::getpid(),
                reply_id,
                off as libc::c_ulonglong,
                ocerz_ld(reply.wrapping_add(off).wrapping_add(11), 1) as u8 as c_uint,
            );
        }
        let _ = trap;
        break;
    }
}

unsafe fn ocerz_reply_alias_iokit(vm: *mut OcerzVM, reply: u64, recv_size: u32) {
    if crate::ffi::ocerz_low_base == 0 || reply == 0 {
        return;
    }
    let id = ocerz_ld(reply.wrapping_add(0x14), 4) as u32;
    if !(2900..=2999).contains(&id) {
        return;
    }
    let mut size = ocerz_ld(reply.wrapping_add(4), 4) as u32;
    if recv_size != 0 && size > recv_size {
        size = recv_size;
    }
    size = size.min(0x200);
    let mut tries = 0;
    static mut INNER: c_int = -1;
    static mut MACHLEAK: c_int = -1;
    if MACHLEAK < 0 {
        MACHLEAK = c_int::from(!libc::getenv(c"OCERZ_MACHLEAK".as_ptr()).is_null());
    }
    if INNER < 0 {
        INNER = libc::getenv(c"OCERZ_NO_IOKIT_INNER_PTR".as_ptr()).is_null() as c_int;
    }
    let mut off = 0x20;
    while off + 8 <= size as u64 && tries < 8 {
        let pointer = ocerz_ld(reply.wrapping_add(off), 8);
        if pointer < 0x1000000 || pointer >= crate::ffi::OCERZ_LOW_LIMIT {
            off += 4;
            continue;
        }
        if pointer & 0xfff != 0 {
            let mut device_protection = 0;
            let guest_page = pointer & !((crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64) - 1);
            let guest_protection = crate::ffi::ocerz_addr_prot(pointer);
            if INNER == 0
                || pointer & 7 != 0
                || ocerz_host_region_is_device(pointer, &mut device_protection) == 0
            {
                off += 4;
                continue;
            }
            if guest_protection > 0 && !alias_page_ours(guest_page) {
                if env_set!("OCERZ_MIGTRACE") {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: IOKIT-COLLIDE[%d] id=%u off=%#llx pointer=%#llx: the guest has its own memory there\n"
                            .as_ptr(),
                        libc::getpid(),
                        id,
                        off as libc::c_ulonglong,
                        pointer as libc::c_ulonglong,
                    );
                }
                off += 4;
                continue;
            }
        }
        let slot_prot = crate::ffi::ocerz_addr_prot(pointer);
        if MACHLEAK != 0 {
            let mut raw = pointer;
            let mut raw_size = 0u64;
            let mut raw_info: VmRegionBasicInfo64 = core::mem::zeroed();
            let mut raw_count = (core::mem::size_of::<VmRegionBasicInfo64>() / 4) as u32;
            let mut raw_object = 0u32;
            let raw_kr = mach_vm_region(
                mach_task_self_,
                &mut raw,
                &mut raw_size,
                VM_REGION_BASIC_INFO_64,
                (&mut raw_info as *mut VmRegionBasicInfo64).cast(),
                &mut raw_count,
                &mut raw_object,
            );
            if raw_object != 0 {
                mach_port_deallocate(mach_task_self_, raw_object);
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKIT-CANDIDATE[%d] id=%u off=%#llx addr=%#llx slot_prot=%d raw_kr=%d raw=%#llx+%#llx raw_prot=%d\n"
                    .as_ptr(),
                libc::getpid(),
                id,
                off as libc::c_ulonglong,
                pointer as libc::c_ulonglong,
                slot_prot,
                raw_kr,
                raw as libc::c_ulonglong,
                raw_size as libc::c_ulonglong,
                if raw_kr == 0 { raw_info.protection } else { -1 },
            );
        }
        if slot_prot >= 0 && slot_prot & libc::PROT_READ != 0 {
            alias_refresh_if_stale(vm, pointer);
            off += 4;
            continue;
        }
        tries += 1;
        let rc = ocerz_alias_raw_region(vm, pointer);
        if MACHLEAK != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKIT-ALIAS[%d] id=%u off=%#llx addr=%#llx rc=%d\n".as_ptr(),
                libc::getpid(),
                id,
                off as libc::c_ulonglong,
                pointer as libc::c_ulonglong,
                rc,
            );
        }
        off += 4;
    }
}

unsafe fn alias_refresh_if_stale(vm: *mut OcerzVM, pointer: u64) -> c_int {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_NO_ALIAS_REFRESH".as_ptr()).is_null()) as c_int;
    }
    let delta = ocerz_g2h(0) as u64;
    if OFF != 0 || delta == 0 || !alias_reg_find(pointer, 0, true) {
        return 0;
    }
    let mut hs = 0;
    let mut he = 0;
    let mut hobj = 0;
    let mut hprot = 0;
    if !host_region_entry(pointer, &mut hs, &mut he, &mut hobj, &mut hprot)
        || hs > pointer
        || hprot & VM_PROT_READ == 0
    {
        return 0;
    }
    let win = 0x1000_0000u64;
    let mut at = if pointer - hs > win {
        pointer - win
    } else {
        hs
    };
    let end = if he - pointer > win {
        pointer.wrapping_add(win)
    } else {
        he
    };
    for _ in 0..64 {
        if at >= end {
            break;
        }
        let mut ss = 0;
        let mut se = 0;
        let mut sobj = 0;
        let mut sprot = 0;
        let translated_at = at.wrapping_add(delta);
        if !host_region_entry(translated_at, &mut ss, &mut se, &mut sobj, &mut sprot) || se <= delta
        {
            break;
        }
        if ss <= translated_at && sobj != hobj && alias_reg_find(at, sobj, false) {
            if env_set!("OCERZ_MIGTRACE") {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: ALIAS-STALE[%d] pointer=%#llx at %#llx alias shows object %#x, host %#x\n"
                        .as_ptr(),
                    libc::getpid(),
                    pointer as libc::c_ulonglong,
                    at as libc::c_ulonglong,
                    sobj,
                    hobj,
                );
            }
            return alias_raw_region(vm, pointer, true);
        }
        at = if ss > translated_at {
            ss - delta
        } else {
            se - delta
        };
    }
    0
}

unsafe fn ocerz_vmmsg_trace(phase: *const c_char, msg: u64, size_limit: u32) {
    static mut ENABLED: c_int = -1;
    if ENABLED < 0 {
        ENABLED = (!libc::getenv(c"OCERZ_VMMSG".as_ptr()).is_null()) as c_int;
    }
    if ENABLED == 0 || msg == 0 {
        return;
    }
    let id = ocerz_ld(msg + 0x14, 4) as u32;
    if !(((4800..=4826).contains(&id) || (4900..=4926).contains(&id))
        && !matches!(id, 4815 | 4816 | 4915 | 4916))
        && !(10050..=10054).contains(&id)
        && !(10150..=10154).contains(&id)
    {
        return;
    }
    let mut size = ocerz_ld(msg + 4, 4) as u32;
    if size == 0 && (10050..=10054).contains(&id) {
        size = 0x90;
    }
    if size_limit != 0 && size > size_limit {
        size = size_limit;
    }
    size = size.min(0x90);
    libc::fprintf(
        crate::log::stderr(),
        c"ocerz: VMMSG-%s[%d] id=%u bits=%#x size=%#x".as_ptr(),
        phase,
        libc::getpid(),
        id,
        ocerz_ld(msg, 4) as u32,
        size,
    );
    let mut off = 0x18;
    while off + 8 <= size as u64 {
        libc::fprintf(
            crate::log::stderr(),
            c" +%#llx=%#llx".as_ptr(),
            off as libc::c_ulonglong,
            ocerz_ld(msg + off, 8) as libc::c_ulonglong,
        );
        off += 8;
    }
    libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
}

unsafe fn thread_act_emulate(cpu: *mut OcerzCPU, buf: u64, id: u32, recv_size: u32) -> bool {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_NO_THREADACT".as_ptr()).is_null()) as c_int;
    }
    let need = if id == 3603 { 40 + 44 * 4 + 8 } else { 36 + 8 };
    if OFF != 0 || recv_size < need {
        return false;
    }
    let port = ocerz_ld(buf + 8, 4) as u32;
    let reply_port = ocerz_ld(buf + 12, 4) as u32;
    let mut state = [0u32; 44];
    let state_ptr = state.as_mut_ptr();
    let mut count = 0u32;
    let kr;
    if id == 3605 {
        kr = crate::ffi::ocerz_vm_thread_suspend(cpu, port);
    } else if id == 3606 {
        kr = crate::ffi::ocerz_vm_thread_resume(port);
    } else {
        let flavor = ocerz_ld(buf + 32, 4) as u32;
        let want = ocerz_ld(buf + 36, 4) as u32;
        let mut g = [0u64; 16];
        let mut rip = 0;
        let mut flags = 0;
        if (flavor != 4 && flavor != 7)
            || crate::ffi::ocerz_vm_thread_regs(port, g.as_mut_ptr(), &mut rip, &mut flags) < 0
        {
            return false;
        }
        let values = [
            g[crate::ffi::OCERZ_RAX as usize],
            g[crate::ffi::OCERZ_RBX as usize],
            g[crate::ffi::OCERZ_RCX as usize],
            g[crate::ffi::OCERZ_RDX as usize],
            g[crate::ffi::OCERZ_RDI as usize],
            g[crate::ffi::OCERZ_RSI as usize],
            g[crate::ffi::OCERZ_RBP as usize],
            g[crate::ffi::OCERZ_RSP as usize],
            g[8],
            g[9],
            g[10],
            g[11],
            g[12],
            g[13],
            g[14],
            g[15],
            rip,
            flags | crate::inline::OCERZ_FLAG_FIXED1,
            0x2b,
            0,
            0,
        ];
        let mut at = 0;
        if flavor == 7 {
            *state_ptr = 4;
            *state_ptr.add(1) = 42;
            at = 2;
        }
        for (i, value) in values.iter().enumerate() {
            *state_ptr.add(at + i * 2) = *value as u32;
            *state_ptr.add(at + i * 2 + 1) = (*value >> 32) as u32;
        }
        count = (at + 42) as u32;
        kr = if want < count {
            OCERZ_MACH_KERN_INVALID_ARGUMENT
        } else {
            OCERZ_MACH_KERN_SUCCESS
        };
    }
    if kr < 0 {
        return false;
    }
    let size = if id == 3603 && kr == 0 {
        40 + count * 4
    } else {
        36
    };
    ocerz_st(buf, 4, 0x1200);
    ocerz_st(buf + 4, 4, size as u64);
    ocerz_st(buf + 8, 4, 0);
    ocerz_st(buf + 12, 4, reply_port as u64);
    ocerz_st(buf + 16, 4, 0);
    ocerz_st(buf + 20, 4, (id + 100) as u64);
    ocerz_st(buf + 24, 8, 0x0000000100000000);
    ocerz_st(buf + 32, 4, kr as u32 as u64);
    if size > 36 {
        ocerz_st(buf + 36, 4, count as u64);
        for i in 0..count {
            ocerz_st(
                buf + 40 + 4 * i as u64,
                4,
                *state_ptr.add(i as usize) as u64,
            );
        }
    }
    ocerz_st(buf + size as u64, 4, 0);
    ocerz_st(buf + size as u64 + 4, 4, 8);
    mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS as u64);
    true
}

unsafe fn dispatch_mach_msg31(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: &mut [u64; 8]) -> c_int {
    let gmsg = a[0];
    let vm_region_req = if gmsg != 0
        && a[1] & 1 != 0
        && ocerz_ld(gmsg.wrapping_add(4), 4) as u32 >= 0x28
        && matches!(ocerz_ld(gmsg.wrapping_add(0x14), 4) as u32, 4815 | 4816)
    {
        ocerz_ld(gmsg.wrapping_add(0x20), 8)
    } else {
        u64::MAX
    };
    let mut saves = [super::machmsg::OcerzOolSave::default(); 64];
    let mut nsaves = 0;
    if gmsg != 0 {
        nsaves = super::machmsg::ocerz_send_xlate_descriptors(
            gmsg,
            ocerz_ld(gmsg.wrapping_add(4), 4) as u32,
            saves.as_mut_ptr(),
            saves.len() as c_int,
        );
    }
    let mut tc: super::machmsg::TcPolicySave = core::mem::zeroed();
    let get_tc = if gmsg != 0 && a[1] & 1 != 0 {
        let get = super::machmsg::tc_policy_get_request(gmsg);
        super::machmsg::tc_policy_send(gmsg, ocerz_ld(gmsg.wrapping_add(4), 4) as u32, &mut tc);
        get
    } else {
        0
    };
    ocerz_vmmsg_trace(
        c"REQ".as_ptr(),
        gmsg,
        if gmsg != 0 {
            ocerz_ld(gmsg.wrapping_add(4), 4) as u32
        } else {
            0
        },
    );
    if gmsg != 0 && a[1] & 2 != 0 {
        super::bsd::disarm_guest_buffer(cpu, gmsg, a[3] as u32 as u64);
    }
    if a[0] != 0 {
        a[0] = ocerz_g2h(a[0]) as u64;
    }
    (*cpu).last_rcv_name = if a[1] & 2 != 0 { a[4] as u32 } else { 0 };
    (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    let result = super::raw::ocerz_host_mach_trap(31, a);
    (*cpu).block_since_ns = 0;
    static mut NO_RCV_TIMEOUT_KICK_31: c_int = -1;
    if NO_RCV_TIMEOUT_KICK_31 < 0 {
        NO_RCV_TIMEOUT_KICK_31 =
            c_int::from(!libc::getenv(c"OCERZ_NO_RCV_TIMEOUT_KICK".as_ptr()).is_null());
    }
    mach_ret(cpu, result);
    if nsaves != 0 {
        super::machmsg::ocerz_send_restore_descriptors(gmsg, saves.as_ptr(), nsaves);
    }
    super::machmsg::tc_policy_send_done(&tc, result);
    if gmsg != 0 && a[1] & 2 != 0 && result == 0 {
        let recv_size = a[3] as u32;
        if get_tc != 0 {
            super::machmsg::tc_policy_get_reply(gmsg, recv_size);
        }
        ocerz_vmmsg_trace(c"REPLY".as_ptr(), gmsg, recv_size);
        super::machmsg::ocerz_reply_xlate_vm_region(gmsg, recv_size, vm_region_req);
        ocerz_reply_relocate_ool(gmsg, recv_size, 31);
        ocerz_reply_alias_iokit(vm, gmsg, recv_size);
        let mut scan_size = ocerz_ld(gmsg.wrapping_add(4), 4) as u32 as u64;
        if a[3] != 0 && scan_size > a[3] as u32 as u64 {
            scan_size = a[3] as u32 as u64;
        }
        super::hostwq::ocerz_shadow_scan(
            c"msg31".as_ptr(),
            ocerz_ld(gmsg.wrapping_add(0x14), 4) as u32 as u64,
            gmsg,
            scan_size,
        );
    }
    if gmsg != 0 && super::machmsg::ocerz_mach_err_interesting(result) != 0 {
        super::machmsg::ocerz_log_mach_send_err(31, result, a, gmsg, cpu);
    }
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn dispatch_mach_msg47(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: &mut [u64; 8]) -> c_int {
    const THREAD_IDENTIFIER_INFO: u32 = 4;
    let msgh_id = (a[4] >> 32) as u32;
    let reply_buf = a[0];
    let vector_mode = a[1] & 0x1_0000_0000 != 0;
    let request_buf = if vector_mode && reply_buf != 0 {
        ocerz_ld(reply_buf, 8)
    } else {
        reply_buf
    };
    let thread_info_flavor = if request_buf != 0
        && msgh_id == 3612
        && ocerz_ld(request_buf.wrapping_add(4), 4) as u32 >= 0x24
    {
        ocerz_ld(request_buf.wrapping_add(0x20), 4) as u32
    } else {
        0
    };
    let sc_map_request = msgh_id == 10052;
    let mut sc_uid = 0u32;
    let mut sc_segment = 0u32;
    let mut vm_result_size = 0u64;
    let mut vm_result_alignment = 0u64;
    if request_buf != 0 && msgh_id == 10054 {
        sc_uid = ocerz_ld(request_buf.wrapping_add(0x30), 4) as u32;
    } else if request_buf != 0 && sc_map_request {
        sc_uid = ocerz_ld(request_buf.wrapping_add(0x20), 4) as u32;
        sc_segment = ocerz_ld(request_buf.wrapping_add(0x24), 4) as u32;
    }
    if request_buf != 0 && msgh_id == 4800 {
        vm_result_size = ocerz_ld(request_buf.wrapping_add(0x28), 8);
    } else if request_buf != 0 && matches!(msgh_id, 4811 | 4813) {
        vm_result_size = ocerz_ld(request_buf.wrapping_add(0x38), 8);
    }
    let vm_fixed_keep = request_buf != 0
        && msgh_id == 4811
        && (ocerz_ld(request_buf.wrapping_add(0x48), 4) as u32)
            & (VM_FLAGS_ANYWHERE | VM_FLAGS_OVERWRITE)
            == 0;
    if request_buf != 0
        && msgh_id == 4811
        && (ocerz_ld(request_buf.wrapping_add(0x48), 4) as u32) & VM_FLAGS_ANYWHERE != 0
    {
        let mask = ocerz_ld(request_buf.wrapping_add(0x40), 8);
        if mask != 0 && mask < 1u64 << 37 && mask & mask.wrapping_add(1) == 0 {
            vm_result_alignment = mask.wrapping_add(1);
        }
    }
    let vm_region_req = if request_buf != 0
        && matches!(msgh_id, 4815 | 4816)
        && ocerz_ld(request_buf.wrapping_add(4), 4) as u32 >= 0x28
    {
        ocerz_ld(request_buf.wrapping_add(0x20), 8)
    } else {
        u64::MAX
    };
    a[6] = ocerz_ld(
        (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(8),
        8,
    );
    a[7] = ocerz_ld(
        (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(16),
        8,
    );
    if reply_buf != 0 && a[1] & 2 != 0 {
        if vector_mode {
            super::bsd::disarm_guest_buffer(
                cpu,
                ocerz_ld(reply_buf.wrapping_add(8), 8),
                ocerz_ld(reply_buf.wrapping_add(20), 4) as u32 as u64,
            );
        } else {
            super::bsd::disarm_guest_buffer(cpu, reply_buf, a[6] as u32 as u64);
        }
    }
    if reply_buf != 0 && !vector_mode && a[1] & 3 == 3 {
        if thread_act_emulate(cpu, request_buf, msgh_id, a[6] as u32) {
            return 0;
        }
    }
    let mut saves = [super::machmsg::OcerzOolSave::default(); 64];
    let mut nsaves = 0;
    if reply_buf != 0 {
        if vector_mode {
            let send_count = if a[1] & 1 != 0 {
                (a[2] >> 32) as u32
            } else {
                0
            };
            let recv_count = if a[1] & 2 != 0 { a[6] as u32 } else { 0 };
            nsaves = super::machmsg::ocerz_send_xlate_vector(
                reply_buf,
                send_count.max(recv_count),
                (a[1] & 1 != 0) as c_int,
                (a[1] & 2 != 0) as c_int,
                saves.as_mut_ptr(),
                saves.len() as c_int,
            );
        } else {
            nsaves = super::machmsg::ocerz_send_xlate_descriptors(
                reply_buf,
                (a[2] >> 32) as u32,
                saves.as_mut_ptr(),
                saves.len() as c_int,
            );
        }
    }
    let mut tc: super::machmsg::TcPolicySave = core::mem::zeroed();
    let get_tc = if !vector_mode && request_buf != 0 && a[1] & 1 != 0 {
        let get = super::machmsg::tc_policy_get_request(request_buf);
        super::machmsg::tc_policy_send(request_buf, (a[2] >> 32) as u32, &mut tc);
        get
    } else {
        0
    };
    if a[0] != 0 {
        a[0] = ocerz_g2h(a[0]) as u64;
    }
    static mut LSTIMEOUT: c_int = -1;
    if LSTIMEOUT < 0 {
        LSTIMEOUT = c_int::from(!libc::getenv(c"OCERZ_LSTIMEOUT".as_ptr()).is_null());
    }
    if LSTIMEOUT != 0 && reply_buf != 0 && a[1] & 1 != 0 && a[1] & 2 != 0 && a[1] & 0x100 == 0 {
        let message_id = (a[4] >> 32) as u32;
        if (10000..10100).contains(&message_id) {
            a[1] |= 0x100;
            a[7] = 1500;
            if env_set!("OCERZ_MACHMSG") {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: LSTIMEOUT forcing 1500ms RCV timeout on id=%u\n".as_ptr(),
                    message_id,
                );
            }
        }
    }
    if env_set!("OCERZ_MSGTIMEOUT") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MSG2 opts=%#llx rcv_timeout_arg=%#llx a6=%#llx rsp=%#llx\n".as_ptr(),
            a[1] as libc::c_ulonglong,
            a[7] as libc::c_ulonglong,
            a[6] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
        );
    }
    if request_buf != 0 {
        ocerz_vmmsg_trace(c"REQ".as_ptr(), request_buf, (a[2] >> 32) as u32);
    }
    static mut IOKITERR: c_int = -1;
    static mut IOKITSEL: c_int = -2;
    if IOKITERR < 0 {
        IOKITERR = c_int::from(!libc::getenv(c"OCERZ_IOKITERR".as_ptr()).is_null());
    }
    if IOKITSEL == -2 {
        let selector = libc::getenv(c"OCERZ_IOKITSEL".as_ptr());
        IOKITSEL = if selector.is_null() {
            -1
        } else if libc::strcmp(selector, c"all".as_ptr()) == 0 {
            c_int::MAX
        } else {
            libc::atoi(selector)
        };
    }
    let mut ioreq = [0u8; 0x208];
    let mut ioreq_n = 0u32;
    if (IOKITERR != 0 || IOKITSEL >= 0)
        && request_buf != 0
        && a[1] & 1 != 0
        && ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32 == 2865
    {
        ioreq_n = ((a[2] >> 32) as u32).min(0x200);
        for offset in (0..ioreq_n as usize).step_by(8) {
            let value = if crate::ffi::ocerz_addr_readable(
                request_buf.wrapping_add(offset as u64).wrapping_add(7),
            ) != 0
            {
                ocerz_ld(request_buf.wrapping_add(offset as u64), 8)
            } else {
                0
            };
            ptr::copy_nonoverlapping(
                (&value as *const u64).cast::<u8>(),
                ioreq.as_mut_ptr().add(offset),
                8,
            );
        }
    }
    static mut MSGNEEDLE: u64 = 0;
    static mut MSGNEEDLE_INIT: c_int = 0;
    if MSGNEEDLE_INIT == 0 {
        let needle = libc::getenv(c"OCERZ_MSGNEEDLE".as_ptr());
        MSGNEEDLE = if needle.is_null() {
            0
        } else {
            libc::strtoull(needle, ptr::null_mut(), 0)
        };
        MSGNEEDLE_INIT = 1;
    }
    if MSGNEEDLE != 0 && request_buf != 0 && a[1] & 1 != 0 {
        let size = ((a[2] >> 32) as u32).min(0x10000);
        for offset in (0..size.saturating_sub(7)).step_by(4) {
            if crate::ffi::ocerz_addr_readable(request_buf.wrapping_add(offset as u64)) == 0
                || crate::ffi::ocerz_addr_readable(
                    request_buf.wrapping_add(offset as u64).wrapping_add(7),
                ) == 0
            {
                break;
            }
            if ocerz_ld(request_buf.wrapping_add(offset as u64), 8) != MSGNEEDLE {
                continue;
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MSGNEEDLE[%d] id=%u size=%#x off=%#x bits=%#x rport=%#x near:".as_ptr(),
                libc::getpid(),
                ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32,
                size,
                offset,
                ocerz_ld(request_buf, 4) as u32,
                ocerz_ld(request_buf.wrapping_add(8), 4) as u32,
            );
            for neighbor in -4i64..6 {
                let at = if neighbor < 0 {
                    request_buf
                        .wrapping_add(offset as u64)
                        .wrapping_sub(neighbor.unsigned_abs() * 8)
                } else {
                    request_buf
                        .wrapping_add(offset as u64)
                        .wrapping_add(neighbor as u64 * 8)
                };
                if at >= request_buf
                    && crate::ffi::ocerz_addr_readable(at) != 0
                    && crate::ffi::ocerz_addr_readable(at.wrapping_add(7)) != 0
                {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" %s%#llx".as_ptr(),
                        if neighbor == 0 {
                            c"*".as_ptr()
                        } else {
                            c"".as_ptr()
                        },
                        ocerz_ld(at, 8) as libc::c_ulonglong,
                    );
                }
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
            if ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32 == 2865
                && ocerz_ld(request_buf, 4) as u32 & 0x8000_0000 == 0
            {
                let selector = ocerz_ld(request_buf.wrapping_add(0x20), 4) as u32;
                let scalars = ocerz_ld(request_buf.wrapping_add(0x24), 4) as u32;
                let inband = request_buf
                    .wrapping_add(0x28)
                    .wrapping_add(8u64.wrapping_mul(scalars as u64));
                let inband_size = ocerz_ld(inband, 4) as u32;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: MSGNEEDLE-IOKIT[%d] selector=%u scalars=%u inband=%u needle-at-inband+%#llx\n"
                        .as_ptr(),
                    libc::getpid(),
                    selector,
                    scalars,
                    inband_size,
                    request_buf
                        .wrapping_add(offset as u64)
                        .wrapping_sub(inband.wrapping_add(4))
                        as libc::c_ulonglong,
                );
            }
        }
    }
    if request_buf != 0 && env_set!("OCERZ_MACHMSG") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MACHMSG-ENTER[%d] opts=%#llx voucher|id=%#llx bits=%#x rport=%#x lport=%#x id=%u xlated=%d\n"
                .as_ptr(),
            libc::getpid(),
            a[1] as libc::c_ulonglong,
            a[4] as libc::c_ulonglong,
            ocerz_ld(request_buf, 4) as u32,
            ocerz_ld(request_buf.wrapping_add(8), 4) as u32,
            ocerz_ld(request_buf.wrapping_add(0xc), 4) as u32,
            ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32,
            nsaves,
        );
    }
    static mut IOKITMIGQ: c_int = -1;
    if IOKITMIGQ < 0 {
        let value = libc::getenv(c"OCERZ_IOKITMIG".as_ptr());
        IOKITMIGQ = if value.is_null() {
            0
        } else {
            libc::atoi(value)
        };
    }
    if IOKITMIGQ >= 2 && request_buf != 0 {
        let qid = ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32;
        if (2800..2900).contains(&qid) {
            let bits = ocerz_ld(request_buf, 4) as u32;
            let size = ocerz_ld(request_buf.wrapping_add(4), 4) as u32;
            let descriptors = if bits & 0x8000_0000 != 0 {
                ocerz_ld(request_buf.wrapping_add(0x18), 4) as u32
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKITREQ[%d] id=%u dest=%#x bits=%#x size=%#x dcnt=%u opts=%#llx\n"
                    .as_ptr(),
                libc::getpid(),
                qid,
                a[3] as u32,
                bits,
                size,
                descriptors,
                a[1] as libc::c_ulonglong,
            );
            let mut offset = 0x1cu64;
            for index in 0..descriptors.min(8) {
                let kind = ocerz_ld(request_buf.wrapping_add(offset).wrapping_add(11), 1) as u8;
                if (1..=3).contains(&kind) {
                    let address = ocerz_ld(request_buf.wrapping_add(offset), 8);
                    let readable = address != 0 && crate::ffi::ocerz_addr_readable(address) != 0;
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: IOKITREQ[%d]   desc[%u] type=%u ool ga=%#llx sz=%#x g2h=%#llx readable=%d head=%08x\n"
                            .as_ptr(),
                        libc::getpid(),
                        index,
                        kind as c_uint,
                        address as libc::c_ulonglong,
                        ocerz_ld(request_buf.wrapping_add(offset).wrapping_add(12), 4) as u32,
                        if address != 0 {
                            ocerz_g2h(address) as u64 as libc::c_ulonglong
                        } else {
                            0
                        },
                        readable as c_int,
                        if readable {
                            ocerz_ld(address, 4) as u32
                        } else {
                            0
                        },
                    );
                    offset += 16;
                } else if kind == 0 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: IOKITREQ[%d]   desc[%u] port=%#x disp=%u\n".as_ptr(),
                        libc::getpid(),
                        index,
                        ocerz_ld(request_buf.wrapping_add(offset), 4) as u32,
                        ocerz_ld(request_buf.wrapping_add(offset).wrapping_add(10), 1) as u8
                            as c_uint,
                    );
                    offset += 12;
                } else {
                    offset += 16;
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKITREQ[%d]   body:".as_ptr(),
                libc::getpid(),
            );
            let mut offset = 0x18u64;
            while offset < 0x98 && offset + 4 <= size as u64 {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %08x".as_ptr(),
                    ocerz_ld(request_buf.wrapping_add(offset), 4) as u32,
                );
                offset += 4;
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        }
    }
    if request_buf != 0
        && env_set!("OCERZ_VMMAPPROBE")
        && ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32 == 4811
    {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: VMMAPREQ id=4811 dcnt=%u portname=%#x raw18:".as_ptr(),
            ocerz_ld(request_buf.wrapping_add(0x18), 4) as u32,
            ocerz_ld(request_buf.wrapping_add(0x1c), 4) as u32,
        );
        for offset in (0x18..0x60).step_by(4) {
            libc::fprintf(
                crate::log::stderr(),
                c" %08x".as_ptr(),
                ocerz_ld(request_buf.wrapping_add(offset), 4) as u32,
            );
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
    if request_buf != 0 && env_set!("OCERZ_MSGDUMP") {
        let mut ascii = [0i8; 321];
        for offset in 0..320u64 {
            let byte = ocerz_ld(request_buf.wrapping_add(0x18).wrapping_add(offset), 1) as u8;
            *ascii.as_mut_ptr().add(offset as usize) = if (0x20..0x7f).contains(&byte) {
                byte as c_char
            } else {
                b'.' as c_char
            };
        }
        let message_id = (a[4] >> 32) as u32;
        if !libc::strstr(ascii.as_ptr(), c"com.apple".as_ptr()).is_null()
            || !libc::strstr(ascii.as_ptr(), c"apple.".as_ptr()).is_null()
            || !libc::strstr(ascii.as_ptr(), c"Server".as_ptr()).is_null()
            || !libc::strstr(ascii.as_ptr(), c"font".as_ptr()).is_null()
            || !libc::strstr(ascii.as_ptr(), c"WindowServer".as_ptr()).is_null()
            || (10000..10100).contains(&message_id)
            || message_id >= 0x4000_0000
        {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MSGDUMP[%d] id=%u rport=%#x: %s\n".as_ptr(),
                libc::getpid(),
                message_id,
                ocerz_ld(request_buf.wrapping_add(8), 4) as u32,
                ascii.as_ptr(),
            );
        }
    }
    static mut SENDRINGLOG: c_int = -1;
    if SENDRINGLOG < 0 {
        SENDRINGLOG = c_int::from(
            !libc::getenv(c"OCERZ_MACHSLOW".as_ptr()).is_null()
                || !libc::getenv(c"OCERZ_PORTDUMP".as_ptr()).is_null(),
        );
    }
    if SENDRINGLOG != 0 && a[1] & 1 != 0 && request_buf != 0 {
        let index = (*cpu).sendring_n as usize % 8;
        *ptr::addr_of_mut!((*cpu).sendring_id)
            .cast::<u32>()
            .add(index) = (a[4] >> 32) as u32;
        *ptr::addr_of_mut!((*cpu).sendring_port)
            .cast::<u32>()
            .add(index) = ocerz_ld(request_buf.wrapping_add(8), 4) as u32;
        *ptr::addr_of_mut!((*cpu).sendring_sz)
            .cast::<u32>()
            .add(index) = (a[2] >> 32) as u32;
        (*cpu).sendring_n = (*cpu).sendring_n.wrapping_add(1);
    }
    (*cpu).last_rcv_name = if a[1] & 2 != 0 { a[5] as u32 } else { 0 };
    (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    let result = super::raw::ocerz_host_mach_trap(47, a);
    (*cpu).block_since_ns = 0;
    if env_set!("OCERZ_MACHMSG") {
        let rb = if vector_mode && reply_buf != 0 {
            ocerz_ld(reply_buf.wrapping_add(8), 8)
        } else {
            reply_buf
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MACHMSG-EXIT[%d] opts=%#llx kr=%#llx sent_id=%u rcv_id=%u rcv_size=%#x\n"
                .as_ptr(),
            libc::getpid(),
            a[1] as libc::c_ulonglong,
            result as libc::c_ulonglong,
            msgh_id,
            if a[1] & 2 != 0 && result == 0 && rb != 0 {
                ocerz_ld(rb + 0x14, 4) as u32
            } else {
                0
            },
            if a[1] & 2 != 0 && result == 0 && rb != 0 {
                ocerz_ld(rb + 4, 4) as u32
            } else {
                0
            },
        );
    }
    static mut MSGSPIN: c_int = -1;
    if MSGSPIN < 0 {
        MSGSPIN = c_int::from(!libc::getenv(c"OCERZ_MSGSPIN".as_ptr()).is_null());
    }
    static MSGSPIN_COUNT: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);
    if MSGSPIN != 0 {
        let spin_count = MSGSPIN_COUNT
            .fetch_add(1, core::sync::atomic::Ordering::Relaxed)
            .wrapping_add(1);
        if spin_count & 0x3ff == 0 {
            let sent_id = if a[1] & 1 != 0 && request_buf != 0 {
                (a[4] >> 32) as u32
            } else {
                0
            };
            let mut sent_dest = if a[1] & 1 != 0 && request_buf != 0 {
                ocerz_ld(request_buf.wrapping_add(8), 4) as u32
            } else {
                0
            };
            if sent_dest == 0 {
                sent_dest = a[3] as u32;
            }
            let mut reply_id = 0;
            if a[1] & 2 != 0 && result == 0 && reply_buf != 0 {
                let rb = if vector_mode {
                    ocerz_ld(reply_buf.wrapping_add(8), 8)
                } else {
                    reply_buf
                };
                if rb != 0 && crate::ffi::ocerz_addr_readable(rb.wrapping_add(0x14)) != 0 {
                    reply_id = ocerz_ld(rb.wrapping_add(0x14), 4) as u32;
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MSGSPIN[%d] n=%llu cpu#%u opt=%#llx kr=%#llx rcvname=%#llx rcvsz=%#llx timeout=%llu sid=%u sdst=%#x rid=%u"
                    .as_ptr(),
            libc::getpid(),
            spin_count as libc::c_ulonglong,
            (*cpu).cpu_number,
            a[1] as libc::c_ulonglong,
            result as libc::c_ulonglong,
            a[5] as libc::c_ulonglong,
            a[6] as libc::c_ulonglong,
            a[7] as libc::c_ulonglong,
            sent_id,
                sent_dest,
                reply_id,
            );
            let mut sp = (*cpu).gpr[crate::ffi::OCERZ_RSP as usize];
            let mut fp = (*cpu).gpr[5];
            if sp != 0 && crate::ffi::ocerz_addr_committed(sp) == 1 {
                libc::fprintf(
                    crate::log::stderr(),
                    c" bt=%#llx".as_ptr(),
                    ocerz_ld(sp, 8) as libc::c_ulonglong,
                );
            }
            for _ in 0..5 {
                if fp == 0
                    || crate::ffi::ocerz_addr_committed(fp) != 1
                    || crate::ffi::ocerz_addr_committed(fp.wrapping_add(8)) != 1
                {
                    break;
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c",%#llx".as_ptr(),
                    ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
                );
                fp = ocerz_ld(fp, 8);
            }
            libc::fputc(b'\n' as c_int, crate::log::stderr());
        }
    }
    static mut WAKELOG: c_int = -1;
    if WAKELOG < 0 {
        WAKELOG = c_int::from(!libc::getenv(c"OCERZ_WAKELOG".as_ptr()).is_null());
    }
    if WAKELOG != 0 {
        if a[1] & 1 != 0 && request_buf != 0 {
            let send_size = (a[2] >> 32) as u32;
            let send_id = (a[4] >> 32) as u32;
            if send_size <= 0x30 && send_id == 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: WAKESEND[%d] cpu#%u dest=%#x sz=%#x -> r=%#llx ic=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    a[3] as u32,
                    send_size,
                    result as libc::c_ulonglong,
                    (*vm).insn_count as libc::c_ulonglong,
                );
            }
        }
        if a[1] & 2 != 0 && result == 0 && reply_buf != 0 {
            let mut wake_reply = reply_buf;
            if vector_mode {
                wake_reply = ocerz_ld(reply_buf.wrapping_add(8), 8);
                if wake_reply == 0 {
                    wake_reply = ocerz_ld(reply_buf, 8);
                }
            }
            let reply_size_actual = if wake_reply != 0 {
                ocerz_ld(wake_reply.wrapping_add(4), 4) as u32
            } else {
                0
            };
            let reply_id = if wake_reply != 0 {
                ocerz_ld(wake_reply.wrapping_add(0x14), 4) as u32
            } else {
                1
            };
            if reply_size_actual <= 0x30 && reply_id == 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: WAKERECV[%d] cpu#%u on=%#x set=%#x sz=%#x ic=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    if wake_reply != 0 {
                        ocerz_ld(wake_reply.wrapping_add(0xc), 4) as u32
                    } else {
                        0
                    },
                    (a[5] >> 32) as u32,
                    reply_size_actual,
                    (*vm).insn_count as libc::c_ulonglong,
                );
            }
        }
    }
    static mut NO_RCV_TIMEOUT_KICK: c_int = -1;
    if NO_RCV_TIMEOUT_KICK < 0 {
        NO_RCV_TIMEOUT_KICK =
            c_int::from(!libc::getenv(c"OCERZ_NO_RCV_TIMEOUT_KICK".as_ptr()).is_null());
    }
    static mut MACHSLOW: c_int = -1;
    if MACHSLOW < 0 {
        MACHSLOW = c_int::from(!libc::getenv(c"OCERZ_MACHSLOW".as_ptr()).is_null());
    }
    static mut MACHSLOW_COUNT: c_int = 0;
    if MACHSLOW != 0 && MACHSLOW_COUNT < 12 && matches!(result, 0x10004003 | 0x10004005) {
        MACHSLOW_COUNT += 1;
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: RCVKICK[%d] cpu#%u rcv=%#llx set=%#llx opts=%#llx lastsends:\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            a[5] as u32 as libc::c_ulonglong,
            (a[5] >> 32) as libc::c_ulonglong,
            a[1] as libc::c_ulonglong,
        );
        let sendring_id = ptr::addr_of!((*cpu).sendring_id).cast::<u32>();
        let sendring_port = ptr::addr_of!((*cpu).sendring_port).cast::<u32>();
        let sendring_sz = ptr::addr_of!((*cpu).sendring_sz).cast::<u32>();
        for i in 0..8usize {
            let j = ((*cpu).sendring_n as usize + i) % 8;
            let id = *sendring_id.add(j);
            let port = *sendring_port.add(j);
            if id != 0 || port != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c" id=%u port=%#x sz=%#x;".as_ptr(),
                    id,
                    port,
                    *sendring_sz.add(j),
                );
            }
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
    static mut KICKLOG: c_int = -1;
    if KICKLOG < 0 {
        KICKLOG = c_int::from(!libc::getenv(c"OCERZ_KICKLOG".as_ptr()).is_null());
    }
    if KICKLOG != 0 && matches!(result, 0x10004005 | 14 | 0x10004003) {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: KICKRET[%d] cpu#%u mach47 -> %#llx rip=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            result as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    super::machmsg::vmmap_pad_restore();
    mach_ret(cpu, result);
    if nsaves != 0 {
        super::machmsg::ocerz_send_restore_descriptors(reply_buf, saves.as_ptr(), nsaves);
    }
    super::machmsg::tc_policy_send_done(&tc, result);
    if ioreq_n >= 0x2c && result != 0 {
        let selector = ptr::read_unaligned(ioreq.as_ptr().add(0x20).cast::<u32>());
        let port = ptr::read_unaligned(ioreq.as_ptr().add(8).cast::<u32>());
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: IOKITERR[%d] mach_msg=%#llx selector=%u port=%#x rip=%#llx rsp=%#llx cpu#%u\n"
                .as_ptr(),
            libc::getpid(),
            result as libc::c_ulonglong,
            selector,
            port,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
            (*cpu).cpu_number,
        );
    }
    if ioreq_n >= 0x2c
        && reply_buf != 0
        && result == 0
        && crate::ffi::ocerz_addr_readable(reply_buf.wrapping_add(0x23)) != 0
    {
        let return_value = ocerz_ld(reply_buf.wrapping_add(0x20), 4) as u32;
        let selector = ptr::read_unaligned(ioreq.as_ptr().add(0x20).cast::<u32>());
        let scalar_count = ptr::read_unaligned(ioreq.as_ptr().add(0x24).cast::<u32>());
        if (IOKITERR != 0 && matches!(return_value, 0xe000_02c8 | 0xe000_02c2))
            || (IOKITSEL >= 0 && (selector == IOKITSEL as u32 || IOKITSEL == c_int::MAX))
        {
            let port = ptr::read_unaligned(ioreq.as_ptr().add(8).cast::<u32>());
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKITERR[%d] ret=%#x selector=%u scalars=%u port=%#x req:".as_ptr(),
                libc::getpid(),
                return_value,
                selector,
                scalar_count,
                port,
            );
            for index in 0..scalar_count.min(8) {
                let offset = 0x28u32.wrapping_add(8u32.wrapping_mul(index));
                if offset.wrapping_add(8) > ioreq_n {
                    break;
                }
                let scalar = ptr::read_unaligned(ioreq.as_ptr().add(offset as usize).cast::<u64>());
                libc::fprintf(
                    crate::log::stderr(),
                    c" s%u=%#llx".as_ptr(),
                    index,
                    scalar as libc::c_ulonglong,
                );
            }
            let input_offset = 0x28u32.wrapping_add(8u32.wrapping_mul(scalar_count));
            let mut input_size = 0u32;
            if input_offset.wrapping_add(4) <= ioreq_n {
                input_size =
                    ptr::read_unaligned(ioreq.as_ptr().add(input_offset as usize).cast::<u32>());
            }
            let output_offset = input_offset
                .wrapping_add(4)
                .wrapping_add(input_size.wrapping_add(3) & !3);
            let mut out_of_line_input = 0u64;
            let mut out_of_line_input_size = 0u64;
            let mut output_input_size = 0u32;
            let mut output_scalar_count = 0u32;
            let mut out_of_line_output = 0u64;
            let mut out_of_line_output_size = 0u64;
            if output_offset.wrapping_add(0x28) <= ioreq_n {
                let output = ioreq.as_ptr().add(output_offset as usize);
                out_of_line_input = ptr::read_unaligned(output.cast::<u64>());
                out_of_line_input_size = ptr::read_unaligned(output.add(8).cast::<u64>());
                output_input_size = ptr::read_unaligned(output.add(16).cast::<u32>());
                output_scalar_count = ptr::read_unaligned(output.add(20).cast::<u32>());
                out_of_line_output = ptr::read_unaligned(output.add(24).cast::<u64>());
                out_of_line_output_size = ptr::read_unaligned(output.add(32).cast::<u64>());
            }
            libc::fprintf(
                crate::log::stderr(),
                c" size=%#x inband_in=%u:".as_ptr(),
                ioreq_n,
                input_size,
            );
            for index in 0..input_size.min(64) {
                let byte_offset = input_offset.wrapping_add(4).wrapping_add(index);
                if byte_offset >= ioreq_n {
                    break;
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c"%02x".as_ptr(),
                    *ioreq.as_ptr().add(byte_offset as usize) as c_uint,
                );
            }
            libc::fprintf(
                crate::log::stderr(),
                c" ool_in=%#llx+%#llx inband_out=%u scalar_out=%u ool_out=%#llx+%#llx reply:"
                    .as_ptr(),
                out_of_line_input as libc::c_ulonglong,
                out_of_line_input_size as libc::c_ulonglong,
                output_input_size,
                output_scalar_count,
                out_of_line_output as libc::c_ulonglong,
                out_of_line_output_size as libc::c_ulonglong,
            );
            let reply_size = ocerz_ld(reply_buf.wrapping_add(4), 4) as u32;
            let mut offset = 0x20u32;
            while offset.wrapping_add(4) <= reply_size
                && offset < 0x180
                && crate::ffi::ocerz_addr_readable(
                    reply_buf.wrapping_add(offset as u64).wrapping_add(3),
                ) != 0
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %08x".as_ptr(),
                    ocerz_ld(reply_buf.wrapping_add(offset as u64), 4) as u32,
                );
                offset = offset.wrapping_add(4);
            }
            libc::fprintf(
                crate::log::stderr(),
                c" rip=%#llx rsp=%#llx cpu#%u\n".as_ptr(),
                (*cpu).rip as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
                (*cpu).cpu_number,
            );
        }
    }
    if request_buf != 0 && super::machmsg::ocerz_mach_err_interesting(result) != 0 {
        super::machmsg::ocerz_log_mach_send_err(47, result, a, request_buf, cpu);
    }
    let mut mach_reply_buf = reply_buf;
    let mut mach_reply_size = a[6] as u32;
    if reply_buf != 0 && vector_mode {
        mach_reply_buf = ocerz_ld(reply_buf.wrapping_add(8), 8);
        if mach_reply_buf == 0 {
            mach_reply_buf = ocerz_ld(reply_buf, 8);
        }
        mach_reply_size = ocerz_ld(reply_buf.wrapping_add(0x14), 4) as u32;
    }
    if mach_reply_buf != 0 && a[1] & 2 != 0 && result == 0 {
        ocerz_vmmsg_trace(c"REPLY".as_ptr(), mach_reply_buf, mach_reply_size);
        super::machmsg::ocerz_reply_xlate_vm_region(mach_reply_buf, mach_reply_size, vm_region_req);
    }
    static mut MACHLEAK: c_int = -1;
    if MACHLEAK < 0 {
        MACHLEAK = c_int::from(!libc::getenv(c"OCERZ_MACHLEAK".as_ptr()).is_null());
    }
    if mach_reply_buf != 0 && MACHLEAK != 0 {
        let mut message_size = ocerz_ld(mach_reply_buf.wrapping_add(4), 4);
        if mach_reply_size != 0 && message_size > mach_reply_size as u64 {
            message_size = mach_reply_size as u64;
        }
        if message_size > 0x160 {
            message_size = 0x160;
        }
        let mut offset = 0x20u64;
        while offset.wrapping_add(8) <= message_size {
            let value = ocerz_ld(mach_reply_buf.wrapping_add(offset), 8);
            if value >= 0x1000_1000
                && value < crate::ffi::OCERZ_LOW_LIMIT as u64
                && value & 0xfff == 0
                && crate::ffi::ocerz_addr_readable(value) == 0
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: MACHLEAK reply_id=%u off=%#llx host_val=%#llx icount=%#llx\n".as_ptr(),
                    ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32,
                    offset as libc::c_ulonglong,
                    value as libc::c_ulonglong,
                    (*vm).insn_count as libc::c_ulonglong,
                );
            }
            offset = offset.wrapping_add(4);
        }
    }
    if mach_reply_buf != 0 && a[1] & 2 != 0 && result == 0 {
        ocerz_reply_relocate_ool(mach_reply_buf, mach_reply_size, 47);
        if get_tc != 0 {
            super::machmsg::tc_policy_get_reply(mach_reply_buf, mach_reply_size);
        }
        ocerz_reply_alias_iokit(vm, mach_reply_buf, mach_reply_size);
    }
    static mut MSGHEX: c_int = -2;
    if MSGHEX == -2 {
        let value = libc::getenv(c"OCERZ_MSGHEX".as_ptr());
        MSGHEX = if value.is_null() {
            0
        } else if libc::strcmp(value, c"all".as_ptr()) == 0 {
            -1
        } else {
            libc::atoi(value)
        };
    }
    if MSGHEX != 0 && request_buf != 0 {
        let request_id = ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32;
        if MSGHEX == -1 || MSGHEX == request_id as c_int {
            let request_size = ocerz_ld(request_buf.wrapping_add(4), 4) as u32;
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MSGHEX[%d] req id=%u bits=%#x size=%#x dst=%#x opts=%#llx kr=%#llx:"
                    .as_ptr(),
                libc::getpid(),
                request_id,
                ocerz_ld(request_buf, 4) as u32,
                request_size,
                a[3] as u32,
                a[1] as libc::c_ulonglong,
                result as libc::c_ulonglong,
            );
            let mut offset = 0x18u64;
            while offset < 0x78 && offset.wrapping_add(4) <= request_size as u64 {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %08x".as_ptr(),
                    ocerz_ld(request_buf.wrapping_add(offset), 4) as u32,
                );
                offset = offset.wrapping_add(4);
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
            if mach_reply_buf != 0 {
                let actual_size = ocerz_ld(mach_reply_buf.wrapping_add(4), 4) as u32;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: MSGHEX[%d]   reply@%#llx id=%u bits=%#x size=%#x (rcvsz=%#llx):"
                        .as_ptr(),
                    libc::getpid(),
                    mach_reply_buf as libc::c_ulonglong,
                    ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32,
                    ocerz_ld(mach_reply_buf, 4) as u32,
                    actual_size,
                    mach_reply_size as libc::c_ulonglong,
                );
                let mut offset = 0x18u64;
                while offset < 0x98 && offset.wrapping_add(4) <= actual_size as u64 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" %08x".as_ptr(),
                        ocerz_ld(mach_reply_buf.wrapping_add(offset), 4) as u32,
                    );
                    offset = offset.wrapping_add(4);
                }
                libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
            }
        }
    }
    static mut IOKITMIG: c_int = -1;
    if IOKITMIG < 0 {
        let value = libc::getenv(c"OCERZ_IOKITMIG".as_ptr());
        IOKITMIG = if value.is_null() {
            0
        } else {
            libc::atoi(value)
        };
        if !value.is_null() && IOKITMIG == 0 {
            IOKITMIG = 1;
        }
    }
    if IOKITMIG != 0 && request_buf != 0 {
        let request_id = ocerz_ld(request_buf.wrapping_add(0x14), 4) as u32;
        if (2800..3000).contains(&request_id) {
            let destination = if a[3] != 0 {
                a[3] as u32
            } else {
                ocerz_ld(request_buf.wrapping_add(8), 4) as u32
            };
            let reply_id = if mach_reply_buf != 0 {
                ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32
            } else {
                0
            };
            let reply_bits = if mach_reply_buf != 0 {
                ocerz_ld(mach_reply_buf, 4) as u32
            } else {
                0
            };
            let word0 = if mach_reply_buf != 0 {
                ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
            } else {
                0
            };
            let word1 = if mach_reply_buf != 0 {
                ocerz_ld(mach_reply_buf.wrapping_add(0x24), 4) as u32
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IOKITMIG[%d] req=%u rport=%#x -> kr=%#llx reply_id=%u bits=%#x w0=%#x w1=%#x\n"
                    .as_ptr(),
                libc::getpid(),
                request_id,
                destination,
                result as libc::c_ulonglong,
                reply_id,
                reply_bits,
                word0,
                word1,
            );
            if result as u64 == 0x1000_0003 {
                let mut port_type = 0;
                let type_result = mach_port_type(mach_task_self(), destination, &mut port_type);
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: IOKITMIG[%d]   dest %#x host mach_port_type kr=%d type=%#x\n".as_ptr(),
                    libc::getpid(),
                    destination,
                    type_result,
                    port_type,
                );
            }
            if IOKITMIG >= 2 && matches!(request_id, 2989 | 2984 | 2988)
                || (IOKITMIG >= 2 && result != 0)
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: IOKITMIG[%d] req=%u qbits=%#x qsize=%#x dcnt=%u opts=%#llx\n".as_ptr(),
                    libc::getpid(),
                    request_id,
                    ocerz_ld(request_buf, 4) as u32,
                    ocerz_ld(request_buf.wrapping_add(4), 4) as u32,
                    if ocerz_ld(request_buf, 4) as u32 & 0x8000_0000 != 0 {
                        ocerz_ld(request_buf.wrapping_add(0x18), 4) as u32
                    } else {
                        0
                    },
                    a[1] as libc::c_ulonglong,
                );
                let request_bits = ocerz_ld(request_buf, 4) as u32;
                let request_size = ocerz_ld(request_buf.wrapping_add(4), 4) as u32;
                let descriptor_count = if request_bits & 0x8000_0000 != 0 {
                    ocerz_ld(request_buf.wrapping_add(0x18), 4) as u32
                } else {
                    0
                };
                let mut descriptor_offset = 0x1c;
                for descriptor in 0..descriptor_count.min(8) {
                    let descriptor_type = ocerz_ld(
                        request_buf.wrapping_add(descriptor_offset).wrapping_add(11),
                        1,
                    ) as u8;
                    if (1..=3).contains(&descriptor_type) {
                        let guest_address =
                            ocerz_ld(request_buf.wrapping_add(descriptor_offset), 8);
                        let bytes = ocerz_ld(
                            request_buf.wrapping_add(descriptor_offset).wrapping_add(12),
                            4,
                        ) as u32;
                        let host_address = if guest_address != 0 {
                            ocerz_g2h(guest_address) as u64
                        } else {
                            0
                        };
                        let readable = guest_address != 0
                            && crate::ffi::ocerz_addr_readable(guest_address) != 0;
                        let first_word = if readable {
                            ocerz_ld(guest_address, 4) as u32
                        } else {
                            0
                        };
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: IOKITMIG[%d]     desc[%u] type=%u ool ga=%#llx sz=%#x g2h=%#llx readable=%d bytes0=%08x\n"
                                .as_ptr(),
                            libc::getpid(),
                            descriptor,
                            descriptor_type as c_uint,
                            guest_address as libc::c_ulonglong,
                            bytes,
                            host_address as libc::c_ulonglong,
                            readable as c_int,
                            first_word,
                        );
                        descriptor_offset += 16;
                    } else if descriptor_type == 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: IOKITMIG[%d]     desc[%u] port=%#x disp=%u\n".as_ptr(),
                            libc::getpid(),
                            descriptor,
                            ocerz_ld(request_buf.wrapping_add(descriptor_offset), 4) as u32,
                            ocerz_ld(
                                request_buf.wrapping_add(descriptor_offset).wrapping_add(10),
                                1,
                            ) as u8 as c_uint,
                        );
                        descriptor_offset += 12;
                    } else {
                        descriptor_offset += 16;
                    }
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: IOKITMIG[%d]     body:".as_ptr(),
                    libc::getpid(),
                );
                let mut offset = 0x18u64;
                while offset < 0x58 && offset + 4 <= request_size as u64 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" %08x".as_ptr(),
                        ocerz_ld(request_buf.wrapping_add(offset), 4) as u32,
                    );
                    offset += 4;
                }
                libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
            }
            if IOKITMIG >= 2 && mach_reply_buf != 0 && reply_bits & 0x8000_0000 != 0 {
                let descriptor_count = ocerz_ld(mach_reply_buf.wrapping_add(0x18), 4) as u32;
                let mut descriptor_offset = 0x1c;
                for descriptor in 0..descriptor_count.min(16) {
                    let descriptor_type = ocerz_ld(
                        mach_reply_buf
                            .wrapping_add(descriptor_offset)
                            .wrapping_add(11),
                        1,
                    ) as u8;
                    if descriptor_type == 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: IOKITMIG[%d]   reply_id=%u port-desc[%u]=%#x disp=%u\n"
                                .as_ptr(),
                            libc::getpid(),
                            reply_id,
                            descriptor,
                            ocerz_ld(mach_reply_buf.wrapping_add(descriptor_offset), 4) as u32,
                            ocerz_ld(
                                mach_reply_buf
                                    .wrapping_add(descriptor_offset)
                                    .wrapping_add(10),
                                1,
                            ) as u8 as c_uint,
                        );
                        descriptor_offset += 12;
                    } else if (1..=4).contains(&descriptor_type) {
                        descriptor_offset += 16;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    if mach_reply_buf != 0 && a[1] & 2 != 0 && result == 0 && env_set!("OCERZ_PORTRECV") {
        let bits = ocerz_ld(mach_reply_buf, 4) as u32;
        let mut size = ocerz_ld(mach_reply_buf.wrapping_add(4), 4) as u32;
        if mach_reply_size != 0 && size > mach_reply_size {
            size = mach_reply_size;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: PORTRECV[%d] id=%u size=%u bits=%#x remote=%#x local=%#x".as_ptr(),
            libc::getpid(),
            ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32,
            size,
            bits,
            ocerz_ld(mach_reply_buf.wrapping_add(8), 4) as u32,
            ocerz_ld(mach_reply_buf.wrapping_add(0xc), 4) as u32,
        );
        if bits & 0x8000_0000 != 0 {
            let descriptor_count = ocerz_ld(mach_reply_buf.wrapping_add(0x18), 4) as u32;
            let mut offset = 0x1cu64;
            for descriptor in 0..descriptor_count.min(8) {
                if offset.wrapping_add(12) > size as u64 {
                    break;
                }
                let descriptor_type =
                    ocerz_ld(mach_reply_buf.wrapping_add(offset).wrapping_add(11), 1) as u8;
                if descriptor_type == 0 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" port[%u]{name=%#x disp=%u}".as_ptr(),
                        descriptor,
                        ocerz_ld(mach_reply_buf.wrapping_add(offset), 4) as u32,
                        ocerz_ld(mach_reply_buf.wrapping_add(offset).wrapping_add(10), 1) as u8
                            as c_uint,
                    );
                    offset = offset.wrapping_add(12);
                } else if (1..=3).contains(&descriptor_type) {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" ool[%u]{addr=%#llx sz=%u ty=%u}".as_ptr(),
                        descriptor,
                        ocerz_ld(mach_reply_buf.wrapping_add(offset), 8) as libc::c_ulonglong,
                        ocerz_ld(mach_reply_buf.wrapping_add(offset).wrapping_add(12), 4) as u32,
                        descriptor_type as c_uint,
                    );
                    offset = offset.wrapping_add(16);
                } else if descriptor_type == 4 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" gport[%u]{name=%#x}".as_ptr(),
                        descriptor,
                        ocerz_ld(mach_reply_buf.wrapping_add(offset).wrapping_add(12), 4) as u32,
                    );
                    offset = offset.wrapping_add(16);
                } else {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" ?ty=%u".as_ptr(),
                        descriptor_type as c_uint,
                    );
                    break;
                }
            }
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
    if mach_reply_buf != 0 {
        let reply_id = ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32;
        if reply_id == 4911
            && vm_fixed_keep
            && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
                == OCERZ_MACH_KERN_SUCCESS as u32
        {
            mig_vm_refuse_taken(mach_reply_buf, vm_result_size);
        }
        if matches!(reply_id, 4900 | 4911 | 4913)
            && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
                == OCERZ_MACH_KERN_SUCCESS as u32
        {
            mig_vm_reply_relocate(
                vm,
                mach_reply_buf,
                false,
                vm_result_size,
                vm_result_alignment,
            );
        }
        let reply_size_actual = ocerz_ld(mach_reply_buf.wrapping_add(4), 4) as u32;
        let reply_bits = ocerz_ld(mach_reply_buf, 4) as u32;
        if reply_id == 10154
            && msgh_id == 10054
            && reply_bits & 0x8000_0000 == 0
            && reply_size_actual >= 0x30
            && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
                == OCERZ_MACH_KERN_SUCCESS as u32
        {
            let universe = ocerz_ld(mach_reply_buf.wrapping_add(0x24), 8);
            if universe != 0 && ocerz_alias_raw_contiguous(vm, universe) == 0 {
                let table = ocerz_ld(universe, 8);
                let entries = ocerz_ld(universe.wrapping_add(0x18), 8);
                if ocerz_alias_raw_region(vm, table) == 0
                    && (entries == 0 || ocerz_alias_raw_region(vm, entries) == 0)
                {
                    ocerz_sc_remember_universe(sc_uid, universe);
                }
            }
        } else if reply_id == 10152
            && sc_map_request
            && reply_bits & 0x8000_0000 == 0
            && reply_size_actual >= 0x28
            && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
                == OCERZ_MACH_KERN_SUCCESS as u32
            && ocerz_ld(mach_reply_buf.wrapping_add(0x24), 1) as u8 != 0
        {
            let universe = ocerz_sc_find_universe(sc_uid);
            if universe != 0 {
                let table = ocerz_ld(universe, 8);
                let kind = (sc_segment >> 29) & 3;
                let slot = (sc_segment >> 23) & 0x3f;
                let slot_ptr = table
                    .wrapping_add(kind as u64 * 0x200)
                    .wrapping_add(slot as u64 * 8);
                if crate::ffi::ocerz_addr_committed(slot_ptr) == 1
                    || ocerz_g2h(slot_ptr) as u64 == slot_ptr
                {
                    ocerz_alias_raw_region(vm, ocerz_ld(slot_ptr, 8));
                }
            }
        }
        if reply_id == 3712
            && msgh_id == 3612
            && thread_info_flavor == THREAD_IDENTIFIER_INFO
            && a[1] & 2 != 0
            && result == 0
            && reply_size_actual >= 0x40
            && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
                == OCERZ_MACH_KERN_SUCCESS as u32
            && ocerz_ld(mach_reply_buf.wrapping_add(0x24), 4) as u32 >= 6
        {
            let host = ocerz_ld(mach_reply_buf.wrapping_add(0x30), 8);
            let mut guest = crate::ffi::ocerz_vm_guest_tsd_for_host(host);
            if guest == 0 && host >= 0x1400_0000 && host < crate::ffi::OCERZ_LOW_LIMIT {
                guest = (*cpu).gs_base;
            }
            if guest != 0 {
                let qualifier = ocerz_ld(mach_reply_buf.wrapping_add(0x38), 8);
                ocerz_st(mach_reply_buf.wrapping_add(0x30), 8, guest);
                ocerz_st(
                    mach_reply_buf.wrapping_add(0x38),
                    8,
                    guest.wrapping_add(qualifier.wrapping_sub(host)),
                );
                if env_set!("OCERZ_SIGTRACE") {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: K1 thread_info handle %#llx -> gs_base %#llx (comm=%d)\n".as_ptr(),
                        host as libc::c_ulonglong,
                        guest as libc::c_ulonglong,
                        crate::ffi::ocerz_addr_committed(guest),
                    );
                }
            }
        }
    }
    if mach_reply_buf != 0 && a[1] & 2 != 0 && result == 0 {
        let mut shadow_size = ocerz_ld(mach_reply_buf.wrapping_add(4), 4) as u32;
        if mach_reply_size != 0 && shadow_size > mach_reply_size {
            shadow_size = mach_reply_size;
        }
        super::hostwq::ocerz_shadow_scan(
            c"msg2".as_ptr(),
            ocerz_ld(mach_reply_buf.wrapping_add(0x14), 4) as u32 as u64,
            mach_reply_buf,
            shadow_size as u64,
        );
    }
    if msgh_id == 8000
        && mach_reply_buf != 0
        && a[1] & 2 != 0
        && result == 0
        && ocerz_ld(mach_reply_buf.wrapping_add(4), 4) as u32 == 0x24
        && ocerz_ld(mach_reply_buf.wrapping_add(0x20), 4) as u32
            == OCERZ_MACH_KERN_NOT_SUPPORTED as u32
    {
        ocerz_st(
            mach_reply_buf.wrapping_add(0x20),
            4,
            OCERZ_MACH_KERN_SUCCESS as u32 as u64,
        );
    }
    crate::ffi::OCERZ_STEP_OK as c_int
}

pub(super) unsafe fn dispatch_mach(vm: *mut OcerzVM, cpu: *mut OcerzCPU, num: c_int) -> c_int {
    static mut STRACE_CPU: c_int = -2;
    if STRACE_CPU == -2 {
        let value = libc::getenv(c"OCERZ_STRACE_CPU".as_ptr());
        STRACE_CPU = if value.is_null() {
            -1
        } else {
            libc::atoi(value)
        };
    }
    if (*cpu).cpu_number as c_int == STRACE_CPU {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SC[%d] cpu#%u mach %d rdi=%#llx rsi=%#llx rdx=%#llx rip=%#llx gs=%#llx gs18=%#llx ic=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            num,
            (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gs_base as libc::c_ulonglong,
            if crate::ffi::ocerz_addr_readable((*cpu).gs_base.wrapping_add(0x18)) != 0 {
                ocerz_ld((*cpu).gs_base.wrapping_add(0x18), 8)
            } else {
                0
            } as libc::c_ulonglong,
            (*vm).insn_count as libc::c_ulonglong,
        );
    }
    let mut a = [0u64; 8];
    a[0] = (*cpu).gpr[crate::ffi::OCERZ_RDI as usize];
    a[1] = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
    a[2] = (*cpu).gpr[crate::ffi::OCERZ_RDX as usize];
    a[3] = (*cpu).gpr[crate::ffi::OCERZ_R10 as usize];
    a[4] = (*cpu).gpr[crate::ffi::OCERZ_R8 as usize];
    a[5] = (*cpu).gpr[crate::ffi::OCERZ_R9 as usize];
    static mut PORTLOG: c_int = -1;
    if PORTLOG < 0 {
        PORTLOG = c_int::from(!libc::getenv(c"OCERZ_PORTLOG".as_ptr()).is_null());
    }
    if PORTLOG != 0 && matches!(num, 17 | 18 | 19 | 25) {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: PORT-DROP[%d] trap=%d name=%#llx a2=%#llx a3=%#llx rip=%#llx\n".as_ptr(),
            libc::getpid(),
            num,
            a[1] as libc::c_ulonglong,
            a[2] as libc::c_ulonglong,
            a[3] as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    match num {
        10 => mach_ret(cpu, guest_vm_allocate_apply(vm, a[1], a[2], a[3]) as u64),
        11 => {
            if a[3] != 0 {
                ocerz_st(a[3], 4, 0);
            }
            mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS as u64);
        }
        12 => mach_ret(cpu, guest_vm_deallocate_apply(vm, a[1], a[2]) as u64),
        14 => mach_ret(
            cpu,
            guest_vm_protect_apply(vm, a[1], a[2], a[4] as c_int) as u64,
        ),
        15 => {
            let size = a[2];
            let mask = a[3];
            let flags = a[4];
            static mut VMLOG: c_int = -1;
            static mut VMLOG_COUNT: c_int = 0;
            if VMLOG < 0 {
                VMLOG = c_int::from(!libc::getenv(c"OCERZ_MACHSLOW".as_ptr()).is_null());
            }
            if VMLOG != 0 && VMLOG_COUNT < 60 {
                VMLOG_COUNT += 1;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: VMMAP[%d] cpu#%u want=%#llx size=%#llx mask=%#llx flags=%#llx rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    if a[1] != 0 && flags & VM_FLAGS_ANYWHERE as u64 == 0 {
                        ocerz_ld(a[1], 8)
                    } else {
                        0
                    } as libc::c_ulonglong,
                    size as libc::c_ulonglong,
                    mask as libc::c_ulonglong,
                    flags as libc::c_ulonglong,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
            if flags & VM_FLAGS_ANYWHERE as u64 == 0 {
                let want = if a[1] != 0 { ocerz_ld(a[1], 8) } else { 0 };
                super::mem::memtrace(c"vm_map".as_ptr(), want, size, 0, flags as c_int);
                if want == 0
                    || (crate::ffi::ocerz_map_claim_fixed(
                        want,
                        size,
                        libc::PROT_READ | libc::PROT_WRITE,
                    ) != crate::ffi::OCERZ_OK as c_int
                        && crate::ffi::ocerz_map_claim_region(
                            want,
                            size,
                            libc::PROT_READ | libc::PROT_WRITE,
                        ) != crate::ffi::OCERZ_OK as c_int
                        && (crate::ffi::ocerz_mem_register_range(want, want.wrapping_add(size))
                            != crate::ffi::OCERZ_OK as c_int
                            || crate::ffi::ocerz_map_claim_region(
                                want,
                                size,
                                libc::PROT_READ | libc::PROT_WRITE,
                            ) != crate::ffi::OCERZ_OK as c_int))
                {
                    static mut DENYLOG: c_int = -1;
                    static DENIED: core::sync::atomic::AtomicU64 =
                        core::sync::atomic::AtomicU64::new(0);
                    if DENYLOG < 0 {
                        DENYLOG = c_int::from(!libc::getenv(c"OCERZ_DENYLOG".as_ptr()).is_null());
                    }
                    let denied = DENIED
                        .fetch_add(1, core::sync::atomic::Ordering::Relaxed)
                        .wrapping_add(1);
                    if (*vm).strace != 0 || (DENYLOG != 0 && (denied & 0x3ff == 0 || denied < 8)) {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: mach_vm_map FIXED denied n=%llu want=%#llx size=%#llx mask=%#llx flags=%#llx rip=%#llx\n".as_ptr(),
                            denied as libc::c_ulonglong,
                            want as libc::c_ulonglong,
                            size as libc::c_ulonglong,
                            mask as libc::c_ulonglong,
                            flags as libc::c_ulonglong,
                            (*cpu).rip as libc::c_ulonglong,
                        );
                    }
                    mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE as u64);
                } else {
                    super::mem::invalidate_guest_mapping(vm, want, size);
                    mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS as u64);
                }
            } else {
                let guest = if mask != 0 {
                    crate::ffi::ocerz_map_anywhere_aligned(
                        size,
                        libc::PROT_READ | libc::PROT_WRITE,
                        mask.wrapping_add(1),
                    )
                } else {
                    crate::ffi::ocerz_map_anywhere(size, libc::PROT_READ | libc::PROT_WRITE)
                };
                if guest == 0 {
                    mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE as u64);
                } else {
                    super::mem::invalidate_guest_mapping(vm, guest, size);
                    if a[1] != 0 {
                        ocerz_st(a[1], 8, guest);
                    }
                    mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS as u64);
                }
            }
        }
        16 => {
            let guest_name = a[2];
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            let result = super::raw::ocerz_host_mach_trap(num as i64, &mut a);
            mach_ret(cpu, result);
            if PORTLOG != 0 && result == 0 && guest_name != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: PORT-NEW[%d] alloc right=%llu name=%#llx rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    a[1] as libc::c_ulonglong,
                    ocerz_ld(guest_name, 4) as libc::c_ulonglong,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
        }
        24 => {
            let guest_name = a[3];
            if a[1] != 0 {
                a[1] = ocerz_g2h(a[1]) as u64;
            }
            if a[3] != 0 {
                a[3] = ocerz_g2h(a[3]) as u64;
            }
            let result = super::raw::ocerz_host_mach_trap(num as i64, &mut a);
            mach_ret(cpu, result);
            if PORTLOG != 0 && result == 0 && guest_name != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: PORT-NEW[%d] construct name=%#llx rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    ocerz_ld(guest_name, 4) as libc::c_ulonglong,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
        }
        40 => {
            if a[3] != 0 {
                a[3] = ocerz_g2h(a[3]) as u64;
            }
            if a[4] != 0 {
                a[4] = ocerz_g2h(a[4]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        18 | 19 | 20 | 21 | 22 | 23 | 25 | 26 | 27 | 28 | 29 | 33 | 34 | 35 | 36 | 37 | 38 | 39
        | 50 | 59 | 60 | 61 | 70 => {
            if num == 70 {
                if a[1] != 0 {
                    a[1] = ocerz_g2h(a[1]) as u64;
                }
                if a[3] != 0 {
                    a[3] = ocerz_g2h(a[3]) as u64;
                }
            }
            static mut PORTLOG_DETAIL: c_int = -1;
            if PORTLOG_DETAIL < 0 {
                PORTLOG_DETAIL = c_int::from(!libc::getenv(c"OCERZ_PORTLOG".as_ptr()).is_null());
            }
            if PORTLOG_DETAIL != 0 && (num == 18 || num == 19) {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: PORTLOG[%d] %s name=%#llx right=%#llx delta=%#llx\n".as_ptr(),
                    libc::getpid(),
                    mach_trap_name(num),
                    a[1] as libc::c_ulonglong,
                    a[2] as libc::c_ulonglong,
                    a[3] as libc::c_ulonglong,
                );
            }
            (*cpu).block_nokick = c_int::from(matches!(num, 36 | 37 | 38 | 39));
            (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            let result = super::raw::ocerz_host_mach_trap(num as i64, &mut a);
            (*cpu).block_since_ns = 0;
            (*cpu).block_nokick = 0;
            static mut KICKLOG: c_int = -1;
            if KICKLOG < 0 {
                KICKLOG = c_int::from(!libc::getenv(c"OCERZ_KICKLOG".as_ptr()).is_null());
            }
            if KICKLOG != 0 && result == 14 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: KICKRET[%d] cpu#%u %s -> ABORTED rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    mach_trap_name(num),
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
            mach_ret(cpu, result);
        }
        31 => {
            let step = dispatch_mach_msg31(vm, cpu, &mut a);
            if step != crate::ffi::OCERZ_STEP_OK as c_int {
                return step;
            }
        }
        43 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        44 | 45 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        46 => {
            if a[1] != 0 {
                a[1] = ocerz_g2h(a[1]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        47 => {
            let step = dispatch_mach_msg47(vm, cpu, &mut a);
            if step != crate::ffi::OCERZ_STEP_OK as c_int {
                return step;
            }
        }
        76 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        77 => {
            a[6] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(8),
                8,
            );
            if a[6] != 0 {
                a[6] = ocerz_g2h(a[6]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        89 => {
            if a[0] != 0 {
                ocerz_st(a[0], 4, 1);
                ocerz_st(a[0].wrapping_add(4), 4, 1);
            }
            mach_ret(cpu, 0);
        }
        90 | 91 | 92 | 93 | 95 => {
            if num == 90 {
                a[0] = super::machmsg::ocerz_guest_ns_to_host_ticks(a[0]);
            } else if num == 93 {
                a[1] = super::machmsg::ocerz_guest_ns_to_host_ticks(a[1]);
            } else if num == 95 {
                a[2] = super::machmsg::ocerz_guest_ns_to_host_ticks(a[2]);
                a[3] = super::machmsg::ocerz_guest_ns_to_host_ticks(a[3]);
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        94 => {
            if a[1] != 0 {
                a[1] = ocerz_g2h(a[1]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        41 | 42 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        100 => {
            a[6] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(8),
                8,
            );
            a[7] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(16),
                8,
            );
            let result = super::raw::ocerz_host_mach_trap(num as i64, &mut a);
            static mut IOKITLOG: c_int = -1;
            if IOKITLOG < 0 {
                IOKITLOG = c_int::from(!libc::getenv(c"OCERZ_IOKITLOG".as_ptr()).is_null());
            }
            if IOKITLOG != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: IOKIT-TRAP conn=%#llx index=%llu a=%#llx,%#llx,%#llx,%#llx,%#llx,%#llx -> ret=%#llx\n".as_ptr(),
                    a[0] as libc::c_ulonglong,
                    a[1] as libc::c_ulonglong,
                    a[2] as libc::c_ulonglong,
                    a[3] as libc::c_ulonglong,
                    a[4] as libc::c_ulonglong,
                    a[5] as libc::c_ulonglong,
                    a[6] as libc::c_ulonglong,
                    a[7] as libc::c_ulonglong,
                    result as libc::c_ulonglong,
                );
            }
            mach_ret(cpu, result);
        }
        13 => {
            if a[0] != 0 {
                a[0] = ocerz_g2h(a[0]) as u64;
            }
            if a[1] != 0 {
                a[1] = ocerz_g2h(a[1]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        62 => {
            if a[4] != 0 {
                a[4] = ocerz_g2h(a[4]) as u64;
            }
            (*cpu).block_nokick = 1;
            (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            let result = super::raw::ocerz_host_mach_trap(num as i64, &mut a);
            (*cpu).block_since_ns = 0;
            (*cpu).block_nokick = 0;
            mach_ret(cpu, result);
        }
        72 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            if a[3] != 0 {
                a[3] = ocerz_g2h(a[3]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        96 => {
            if a[2] != 0 {
                a[2] = ocerz_g2h(a[2]) as u64;
            }
            mach_ret(cpu, super::raw::ocerz_host_mach_trap(num as i64, &mut a));
        }
        _ => {
            let name = mach_trap_name(num);
            if !libc::getenv(c"OCERZ_STRICT_SYSCALL".as_ptr()).is_null() {
                crate::ocerz_fatal!(
                    "unknown Mach trap: class=1 num=%d name=%s rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx ret=%#llx\n",
                    num,
                    if name.is_null() { c"?".as_ptr() } else { name },
                    (*cpu).rip as libc::c_ulonglong,
                    a[0] as libc::c_ulonglong,
                    a[1] as libc::c_ulonglong,
                    a[2] as libc::c_ulonglong,
                    a[3] as libc::c_ulonglong,
                    ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong
                );
                return crate::ffi::OCERZ_STEP_FATAL as c_int;
            }
            static mut PROBED: [u8; 256] = [0; 256];
            if (0..256).contains(&num) {
                let flag = ptr::addr_of_mut!(PROBED).cast::<u8>().add(num as usize);
                if *flag == 0 {
                    *flag = 1;
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: unimplemented Mach trap num=%d name=%s -> KERN_INVALID_ARGUMENT rip=%#llx\n".as_ptr(),
                        num,
                        if name.is_null() { c"?".as_ptr() } else { name },
                        (*cpu).rip as libc::c_ulonglong,
                    );
                }
            }
            mach_ret(cpu, OCERZ_MACH_KERN_INVALID_ARGUMENT as u64);
        }
    }
    if (*vm).strace != 0 {
        let name = mach_trap_name(num);
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: mach_trap %s(num=%d) = %#llx\n".as_ptr(),
            if name.is_null() { c"?".as_ptr() } else { name },
            num,
            (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as libc::c_ulonglong,
        );
    }
    crate::ffi::OCERZ_STEP_OK as c_int
}
