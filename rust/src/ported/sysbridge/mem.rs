//! ---- memory ----
//! mmap, munmap, mprotect and madvise, and mach_vm_allocate, mach_vm_deallocate
//! and mach_vm_protect with their vm_ spellings, which are the same calls on a
//! 64-bit task, go to the entry points src/syscall.c exports for exactly this.
//! Each is the body the cache-mode syscall or Mach trap runs, so guest memory is
//! handed out of the guest arena and accounted for in 4 KB pages, a mapping or
//! protection change retires the translations made from it, a page the guest
//! makes writable after running code from it is watched the way cache mode
//! watches it, and a fixed mapping at an address outside the arena registers it
//! the same way.  Native mode runs in the identity map, so the address the guest
//! gets back is the host address as well.  What native mode adds is memory the
//! syscall path never meets: the guest's heap is the host's, so a guest that
//! protects a page it got from posix_memalign, or deallocates the thread list
//! task_threads handed it, names a range ocerz does not track, and such a range
//! goes to the host kernel after its translations are dropped.  msync, mlock,
//! munlock, minherit and mincore stay ordinary crossings: the syscall layer's own
//! handling of them is to translate the pointer and forward the call, which in
//! the identity map is the crossing itself.  mach_vm_map and mach_vm_remap, with
//! their vm_ spellings, reach the host kernel once the translations of a fixed
//! target range are dropped.
//!
//! mach_vm_region and vm_region_64 are asked about the guest's own memory, and
//! the host kernel answers about host pages: 16 KB of them, which ocerz cannot
//! always give the protection the guest set on one 4 KB page of four.  Electron
//! protects a page read-only, asks mach_vm_region whether it is, and executes an
//! int3 when the answer is anything else, which killed Discord's renderers in
//! native mode as it once did in cache mode (src/syscall.c).  So after the host
//! answers a basic-info query about the process's own task, the region,
//! protection and maximum protection are taken from ocerz's own map when that
//! map has a region holding the address asked about, or else the first region
//! above it; the host's heap lies between ocerz's mappings in the identity map,
//! and a region of it that comes first keeps the host's answer.  The host's
//! region can start lower than ocerz's and still hold the address, since the
//! kernel merges neighbours of one protection that ocerz keeps apart.

use core::ffi::{c_int, c_uint, c_void};

use super::common::*;
use crate::ffi::*;

pub type vm_region_flavor_t = c_int;

const VM_REGION_BASIC_INFO_64: c_int = 9;
const VM_REGION_BASIC_INFO: c_int = 10;

const SB_USER_VA_END: u64 = 0x0000800000000000;

unsafe extern "C" {
    fn mach_vm_region(
        target_task: libc::mach_port_t,
        address: *mut libc::mach_vm_address_t,
        size: *mut libc::mach_vm_size_t,
        flavor: vm_region_flavor_t,
        info: *mut c_void,
        info_count: *mut c_uint,
        object_name: *mut libc::mach_port_t,
    ) -> libc::kern_return_t;
    fn mach_vm_remap(
        target_task: libc::mach_port_t,
        target_address: *mut libc::mach_vm_address_t,
        size: libc::mach_vm_size_t,
        mask: libc::mach_vm_offset_t,
        flags: c_int,
        src_task: libc::mach_port_t,
        src_address: libc::mach_vm_address_t,
        copy: libc::boolean_t,
        cur_protection: *mut libc::vm_prot_t,
        max_protection: *mut libc::vm_prot_t,
        inheritance: libc::vm_inherit_t,
    ) -> libc::kern_return_t;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_mmap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut out = 0u64;
        let e = ocerz_guest_mmap(
            vm,
            cpu,
            sb_arg(cpu, 0),
            sb_arg(cpu, 1),
            sb_arg(cpu, 2) as c_int,
            sb_arg(cpu, 3) as c_int,
            sb_arg(cpu, 4) as c_int,
            sb_arg(cpu, 5),
            &mut out,
        );
        if e != 0 {
            *errno() = e;
            return sb_ret(vm, cpu, -1);
        }
        sb_ret(vm, cpu, out as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_munmap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_posix(vm, cpu, ocerz_guest_munmap(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1))) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_mprotect(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_posix(
            vm,
            cpu,
            ocerz_guest_mprotect(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1), sb_arg(cpu, 2) as c_int),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_madvise(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_posix(
            vm,
            cpu,
            ocerz_guest_madvise(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1), sb_arg(cpu, 2) as c_int),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_vm_allocate(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_ret(
            vm,
            cpu,
            ocerz_guest_vm_allocate(
                vm,
                cpu,
                sb_arg(cpu, 0),
                sb_arg(cpu, 1),
                sb_arg(cpu, 2),
                sb_arg(cpu, 3) as c_int,
            ) as i64,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_vm_deallocate(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_ret(
            vm,
            cpu,
            ocerz_guest_vm_deallocate(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1), sb_arg(cpu, 2))
                as i64,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_vm_protect(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_ret(
            vm,
            cpu,
            ocerz_guest_vm_protect(
                vm,
                cpu,
                sb_arg(cpu, 0),
                sb_arg(cpu, 1),
                sb_arg(cpu, 2),
                sb_arg(cpu, 3) as c_int,
                sb_arg(cpu, 4) as c_int,
            ) as i64,
        )
    }
}

unsafe fn sb_vm_region(vm: *mut OcerzVM, cpu: *mut OcerzCPU, export: *const core::ffi::c_char) -> c_int {
    unsafe {
        let task = sb_arg(cpu, 0) as libc::mach_port_t;
        let addrp = sb_arg(cpu, 1);
        let sizep = sb_arg(cpu, 2);
        let info = sb_arg(cpu, 4);
        let flavor = sb_arg(cpu, 3) as vm_region_flavor_t;
        let query = if addrp != 0 { ocerz_ld(addrp, 8) } else { 0 };
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, export, c"i(uppippp)".as_ptr(), mach_vm_region as *const _);
        let kr = mach_vm_region(
            task,
            sb_ptr(addrp) as *mut libc::mach_vm_address_t,
            sb_ptr(sizep) as *mut libc::mach_vm_size_t,
            flavor,
            sb_ptr(info),
            sb_ptr(sb_arg(cpu, 5)) as *mut c_uint,
            sb_ptr(sb_arg(cpu, 6)) as *mut libc::mach_port_t,
        );
        ocerz_bridge_lower(&outer);
        if kr == libc::KERN_SUCCESS
            && task == libc::mach_task_self()
            && addrp != 0
            && sizep != 0
            && info != 0
            && (flavor == VM_REGION_BASIC_INFO_64 || flavor == VM_REGION_BASIC_INFO)
        {
            let mut ga = query;
            let mut gsz = 0u64;
            let mut prot = 0u32;
            let mut maxprot = 0u32;
            ocerz_guest_vm_region(&mut ga, &mut gsz, &mut prot, &mut maxprot);
            if (prot != 0 || maxprot != 0)
                && ((ga <= query && query - ga < gsz) || ga <= ocerz_ld(addrp, 8))
            {
                ocerz_st(addrp, 8, ga);
                ocerz_st(sizep, 8, gsz);
                ocerz_st(info, 4, prot as u64);
                ocerz_st(info + 4, 4, maxprot as u64);
            }
        }
        sb_ret(vm, cpu, kr as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_mach_vm_region(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_vm_region(vm, cpu, c"_mach_vm_region".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_vm_region_64(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_vm_region(vm, cpu, c"_vm_region_64".as_ptr()) }
}

/* A fixed mach_vm_map below the low limit in a Wine process names the guest's
   shadow, not the host's address: Wine probes free space with FIXED and no
   OVERWRITE before it maps there, which the host answered for its own memory,
   and msync maps the wineserver's shared memory object.  The range is checked
   and claimed in ocerz's tables, and an object is mapped over the claimed
   shadow pages. */

/* With a low shadow, guest code reads every address below 12 GB through the
   shadow, so a host mapping the kernel placed there would be one page to the
   caller and another to the code that uses it: MacNdCheese's wineserver keeps
   msync's wait words in such pages and its wakes were lost.  An anywhere map
   the guest asks for searches from 12 GB up. */
unsafe fn sb_anywhere_floor(
    target: libc::mach_port_t,
    flags: c_int,
    addr: libc::mach_vm_address_t,
) -> libc::mach_vm_address_t {
    unsafe {
        if ocerz_low_base != 0
            && target == libc::mach_task_self()
            && (flags & libc::VM_FLAGS_ANYWHERE) != 0
            && addr < OCERZ_LOW_LIMIT
        {
            return OCERZ_LOW_LIMIT;
        }
        addr
    }
}

unsafe fn sb_vm_map_low(
    vm: *mut OcerzVM,
    addr: u64,
    size: libc::mach_vm_size_t,
    flags: c_int,
    object: libc::mach_port_t,
    offset: u64,
    copy: libc::boolean_t,
    cur: libc::vm_prot_t,
    max: libc::vm_prot_t,
    inherit: libc::vm_inherit_t,
) -> libc::kern_return_t {
    unsafe {
        if size == 0 || (addr & (OCERZ_HOST_PAGE_SIZE as u64 - 1)) != 0 {
            return libc::KERN_INVALID_ARGUMENT;
        }
        if size > OCERZ_LOW_LIMIT - addr {
            return libc::KERN_NO_SPACE;
        }
        if (flags & libc::VM_FLAGS_OVERWRITE) == 0 && ocerz_mem_range_in_use(addr, size) != 0 {
            return libc::KERN_NO_SPACE;
        }
        if ocerz_mem_pinned(addr, size) != 0 {
            return libc::KERN_NO_SPACE;
        }
        ocerz_jit_invalidate_range(vm, addr, size);
        let prot = (if cur & libc::VM_PROT_READ != 0 { libc::PROT_READ } else { 0 })
            | (if cur & libc::VM_PROT_WRITE != 0 { libc::PROT_WRITE } else { 0 })
            | (if cur & libc::VM_PROT_EXECUTE != 0 { libc::PROT_EXEC } else { 0 });
        if ocerz_map_fixed(
            addr,
            size,
            if object == libc::MACH_PORT_NULL as libc::mach_port_t {
                prot
            } else {
                libc::PROT_READ | libc::PROT_WRITE
            },
        ) != OCERZ_OK as c_int
        {
            return libc::KERN_NO_SPACE;
        }
        if object == libc::MACH_PORT_NULL as libc::mach_port_t {
            return libc::KERN_SUCCESS;
        }
        let mut host = ocerz_g2h(addr) as libc::mach_vm_address_t;
        let kr = libc::mach_vm_map(
            libc::mach_task_self(),
            &mut host,
            size,
            0,
            libc::VM_FLAGS_FIXED | libc::VM_FLAGS_OVERWRITE,
            object,
            offset,
            copy,
            cur & !libc::VM_PROT_EXECUTE,
            max,
            inherit,
        );
        if kr != libc::KERN_SUCCESS {
            ocerz_unmap(addr, size);
        }
        kr
    }
}

/* An anonymous fixed map above the low window is guest memory, as a fixed
   mmap is: cache mode claims it in ocerz's map (syscall.c) and so does this.
   Sent to the host instead, it was a mapping ocerz did not know, and Wine's
   anon_mmap_tryfixed - this map, then mmap(MAP_FIXED) over it - found its own
   reservation in the way, so Chromium's 16 GB PartitionAlloc pools failed. */
unsafe fn sb_vm_map_claim(
    vm: *mut OcerzVM,
    addr: u64,
    size: libc::mach_vm_size_t,
    flags: c_int,
) -> libc::kern_return_t {
    unsafe {
        const RW: c_int = libc::PROT_READ | libc::PROT_WRITE;
        let ok;
        if flags & libc::VM_FLAGS_OVERWRITE != 0 {
            ok = ocerz_map_fixed(addr, size, RW) == OCERZ_OK as c_int
                || (ocerz_mem_register_range(addr, addr + size) == OCERZ_OK as c_int
                    && ocerz_map_fixed(addr, size, RW) == OCERZ_OK as c_int);
        } else {
            ok = ocerz_map_claim_fixed(addr, size, RW) == OCERZ_OK as c_int
                || ocerz_map_claim_region(addr, size, RW) == OCERZ_OK as c_int
                || (ocerz_mem_register_range(addr, addr + size) == OCERZ_OK as c_int
                    && ocerz_map_claim_region(addr, size, RW) == OCERZ_OK as c_int);
        }
        if !ok {
            return libc::KERN_NO_SPACE;
        }
        ocerz_jit_invalidate_range(vm, addr, size);
        libc::KERN_SUCCESS
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_mach_vm_map(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let target = sb_arg(cpu, 0) as libc::mach_port_t;
        let addrp = sb_arg(cpu, 1);
        let size = sb_arg(cpu, 2) as libc::mach_vm_size_t;
        let flags = sb_arg(cpu, 4) as c_int;
        let mut addr = if addrp != 0 { ocerz_ld(addrp, 8) } else { 0 };
        /* An anywhere hint above user space cannot be met, and only an uninitialized
           variable passes one: MacNdCheese's wineserver maps its msync pages from a
           stack slot x86 libsystem frames would have overwritten under Rosetta and
           that native mode leaves holding old strings, so the hint is dropped. */
        if (flags & libc::VM_FLAGS_ANYWHERE) != 0 && addr >= SB_USER_VA_END {
            addr = 0;
        }
        addr = sb_anywhere_floor(target, flags, addr);
        if target == libc::mach_task_self()
            && ocerz_low_base != 0
            && (flags & libc::VM_FLAGS_ANYWHERE) == 0
            && addr < OCERZ_LOW_LIMIT
        {
            return sb_ret(
                vm,
                cpu,
                sb_vm_map_low(
                    vm,
                    addr,
                    size,
                    flags,
                    sb_arg(cpu, 5) as libc::mach_port_t,
                    sb_arg(cpu, 6),
                    sb_arg(cpu, 7) as libc::boolean_t,
                    sb_arg(cpu, 8) as libc::vm_prot_t,
                    sb_arg(cpu, 9) as libc::vm_prot_t,
                    sb_arg(cpu, 10) as libc::vm_inherit_t,
                ) as i64,
            );
        }
        if target == libc::mach_task_self()
            && (flags & libc::VM_FLAGS_ANYWHERE) == 0
            && size != 0
            && sb_arg(cpu, 5) as libc::mach_port_t == libc::MACH_PORT_NULL as libc::mach_port_t
            && addr >= OCERZ_LOW_LIMIT
        {
            return sb_ret(vm, cpu, sb_vm_map_claim(vm, addr, size, flags) as i64);
        }
        if target == libc::mach_task_self() && (flags & libc::VM_FLAGS_ANYWHERE) == 0 && size != 0 {
            ocerz_jit_invalidate_range(vm, addr, size);
        }
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_mach_vm_map".as_ptr(), c"i(upLLiuLiiiu)".as_ptr(), libc::mach_vm_map as *const _);
        let kr = libc::mach_vm_map(
            target,
            &mut addr,
            size,
            sb_arg(cpu, 3),
            flags,
            sb_arg(cpu, 5) as libc::mach_port_t,
            sb_arg(cpu, 6),
            sb_arg(cpu, 7) as libc::boolean_t,
            sb_arg(cpu, 8) as libc::vm_prot_t & !libc::VM_PROT_EXECUTE,
            sb_arg(cpu, 9) as libc::vm_prot_t,
            sb_arg(cpu, 10) as libc::vm_inherit_t,
        );
        ocerz_bridge_lower(&outer);
        if kr == libc::KERN_SUCCESS && addrp != 0 {
            ocerz_st(addrp, 8, addr);
        }
        sb_ret(vm, cpu, kr as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_mach_vm_remap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let target = sb_arg(cpu, 0) as libc::mach_port_t;
        let addrp = sb_arg(cpu, 1);
        let curp = sb_arg(cpu, 8);
        let maxp = sb_arg(cpu, 9);
        let size = sb_arg(cpu, 2) as libc::mach_vm_size_t;
        let flags = sb_arg(cpu, 4) as c_int;
        let mut addr = if addrp != 0 { ocerz_ld(addrp, 8) } else { 0 };
        let mut cur = 0 as libc::vm_prot_t;
        let mut max = 0 as libc::vm_prot_t;
        addr = sb_anywhere_floor(target, flags, addr);
        if target == libc::mach_task_self() && (flags & libc::VM_FLAGS_ANYWHERE) == 0 && size != 0 {
            ocerz_jit_invalidate_range(vm, addr, size);
        }
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_mach_vm_remap".as_ptr(), c"i(upLLiuLippu)".as_ptr(), mach_vm_remap as *const _);
        let kr = mach_vm_remap(
            target,
            &mut addr,
            size,
            sb_arg(cpu, 3),
            flags,
            sb_arg(cpu, 5) as libc::mach_port_t,
            sb_arg(cpu, 6),
            sb_arg(cpu, 7) as libc::boolean_t,
            &mut cur,
            &mut max,
            sb_arg(cpu, 10) as libc::vm_inherit_t,
        );
        ocerz_bridge_lower(&outer);
        if kr == libc::KERN_SUCCESS {
            if addrp != 0 {
                ocerz_st(addrp, 8, addr);
            }
            if curp != 0 {
                ocerz_st(curp, 4, cur as u64);
            }
            if maxp != 0 {
                ocerz_st(maxp, 4, max as u64);
            }
        }
        sb_ret(vm, cpu, kr as i64)
    }
}
