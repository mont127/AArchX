//! x86 LDT descriptors shared by the Mach-dependent syscalls and the native
//! libSystem entry points used by translated 32-bit code.

use super::util::*;
use super::*;

use core::ffi::{c_int, c_long};
use core::ptr;

const OCERZ_LDT_MAX: usize = 8192;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct OcerzLdtEntry {
    base: u64,
    limit: u32,
    access: u8,
    big: u8,
    is_long: u8,
    gran: u8,
    present: u8,
}

static mut G_LDT: [OcerzLdtEntry; OCERZ_LDT_MAX] = [OcerzLdtEntry {
    base: 0,
    limit: 0,
    access: 0,
    big: 0,
    is_long: 0,
    gran: 0,
    present: 0,
}; OCERZ_LDT_MAX];
static mut G_LDT_NEXT: c_int = 1;
static mut G_LDT_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_LDT_INSTALLED: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_base(sel: u32) -> u64 {
    let index = (sel >> 3) as usize;
    if sel & 4 == 0 || index >= OCERZ_LDT_MAX {
        return 0;
    }
    let entry = &*ptr::addr_of!(G_LDT).cast::<OcerzLdtEntry>().add(index);
    if entry.present != 0 { entry.base } else { 0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_is_big(sel: u32) -> c_int {
    let index = (sel >> 3) as usize;
    if sel & 4 == 0 || index >= OCERZ_LDT_MAX {
        return 0;
    }
    let entry = &*ptr::addr_of!(G_LDT).cast::<OcerzLdtEntry>().add(index);
    (entry.present != 0 && entry.big != 0) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_installed() -> c_int {
    G_LDT_INSTALLED
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_is_long(sel: u32) -> c_int {
    let index = (sel >> 3) as usize;
    if sel & 4 == 0 || index >= OCERZ_LDT_MAX {
        return 0;
    }
    let entry = &*ptr::addr_of!(G_LDT).cast::<OcerzLdtEntry>().add(index);
    (entry.present != 0 && entry.is_long != 0) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_install(
    sel: u32,
    base: u64,
    limit: u32,
    access: u8,
    big: c_int,
    is_long: c_int,
    gran: c_int,
) {
    let index = (sel >> 3) as usize;
    if sel & 4 == 0 || index >= OCERZ_LDT_MAX {
        return;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LDT_LOCK));
    *ptr::addr_of_mut!(G_LDT).cast::<OcerzLdtEntry>().add(index) = OcerzLdtEntry {
        base,
        limit,
        access,
        big: (big != 0) as u8,
        is_long: (is_long != 0) as u8,
        gran: (gran != 0) as u8,
        present: (access >> 7) & 1,
    };
    if index as c_int + 1 > G_LDT_NEXT {
        G_LDT_NEXT = index as c_int + 1;
    }
    if access & 0x80 != 0 {
        G_LDT_INSTALLED = 1;
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
}

unsafe fn ocerz_ldt_unpack(descriptor: u64, entry: *mut OcerzLdtEntry) {
    (*entry).limit = (descriptor as u32 & 0xffff) | ((((descriptor >> 48) & 0xf) as u32) << 16);
    (*entry).base = ((descriptor >> 16) & 0xffffff) | (((descriptor >> 56) & 0xff) << 24);
    (*entry).access = (descriptor >> 40) as u8;
    (*entry).is_long = ((descriptor >> 53) & 1) as u8;
    (*entry).big = ((descriptor >> 54) & 1) as u8;
    (*entry).gran = ((descriptor >> 55) & 1) as u8;
    (*entry).present = ((*entry).access >> 7) & 1;
}

unsafe fn ocerz_ldt_pack(entry: *const OcerzLdtEntry) -> u64 {
    let limit = (*entry).limit as u64;
    let base = (*entry).base;
    (limit & 0xffff)
        | ((base & 0xffffff) << 16)
        | (((*entry).access as u64) << 40)
        | (((limit >> 16) & 0xf) << 48)
        | (((*entry).is_long as u64 & 1) << 53)
        | (((*entry).big as u64 & 1) << 54)
        | (((*entry).gran as u64 & 1) << 55)
        | (((base >> 24) & 0xff) << 56)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_ldt_call(num: c_int, start: i32, descs: u64, count: i32) -> c_long {
    if count < 0 || count as usize > OCERZ_LDT_MAX || descs == 0 {
        return -(libc::EINVAL as c_long);
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LDT_LOCK));
    if num == 5 {
        let index = if start < 0 {
            if G_LDT_NEXT + count > OCERZ_LDT_MAX as c_int {
                libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
                return -(libc::ENOMEM as c_long);
            }
            let index = G_LDT_NEXT;
            G_LDT_NEXT += count;
            index
        } else {
            let index = start;
            if index < 0 || index + count > OCERZ_LDT_MAX as c_int {
                libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
                return -(libc::EINVAL as c_long);
            }
            if index + count > G_LDT_NEXT {
                G_LDT_NEXT = index + count;
            }
            index
        };
        let entries = ptr::addr_of_mut!(G_LDT).cast::<OcerzLdtEntry>();
        for i in 0..count as usize {
            ocerz_ldt_unpack(
                ocerz_ld(descs.wrapping_add((i * 8) as u64), 8),
                entries.add(index as usize + i),
            );
            if (*entries.add(index as usize + i)).present != 0 {
                G_LDT_INSTALLED = 1;
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
        if !libc::getenv(c"OCERZ_LDTLOG".as_ptr()).is_null() {
            let entry = &*entries.add(index as usize);
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: LDT set idx=%d count=%lld base=%#llx big=%d access=%#x\n".as_ptr(),
                index,
                count as libc::c_longlong,
                entry.base as libc::c_ulonglong,
                entry.big as libc::c_uint,
                entry.access as c_int,
            );
        }
        return index as c_long;
    }
    let index = start;
    if index < 0 || index + count > OCERZ_LDT_MAX as c_int {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
        return -(libc::EINVAL as c_long);
    }
    let used = G_LDT_NEXT;
    let entries = ptr::addr_of!(G_LDT).cast::<OcerzLdtEntry>();
    for i in 0..count as usize {
        if index + i as c_int >= used {
            break;
        }
        ocerz_st(
            descs.wrapping_add((i * 8) as u64),
            8,
            ocerz_ldt_pack(entries.add(index as usize + i)),
        );
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LDT_LOCK));
    used as c_long
}

pub(super) unsafe fn dispatch_machdep_ldt(cpu: *mut OcerzCPU, num: c_int) -> c_int {
    let result = ocerz_ldt_call(
        num,
        (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as i32,
        (*cpu).gpr[crate::ffi::OCERZ_RSI as usize],
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as i32,
    );
    if result < 0 {
        machdep_err(cpu, result.wrapping_neg() as u64);
    } else {
        machdep_ret(cpu, result as u32 as u64);
    }
    crate::ffi::OCERZ_STEP_OK as c_int
}

pub(super) unsafe fn dispatch_machdep(vm: *mut OcerzVM, cpu: *mut OcerzCPU, num: c_int) -> c_int {
    if num == 5 || num == 6 {
        return dispatch_machdep_ldt(cpu, num);
    }
    if num == 3 {
        let newgs = (*cpu).gpr[crate::ffi::OCERZ_RDI as usize];
        static mut GSSETLOG: c_int = -1;
        if GSSETLOG < 0 {
            GSSETLOG = c_int::from(!libc::getenv(c"OCERZ_GSSETLOG".as_ptr()).is_null());
        }
        if GSSETLOG != 0 {
            static mut SEEN: [u64; 64] = [0; 64];
            static mut N_SEEN: c_int = 0;
            let key = newgs & !0xfff;
            let mut found = false;
            for i in 0..N_SEEN as usize {
                if *ptr::addr_of!(SEEN).cast::<u64>().add(i) == key {
                    found = true;
                    break;
                }
            }
            if !found && N_SEEN < 64 {
                *ptr::addr_of_mut!(SEEN).cast::<u64>().add(N_SEEN as usize) = key;
                N_SEEN += 1;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: GSSET[%d] cpu#%u newgs=%#llx oldgs=%#llx rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    newgs as libc::c_ulonglong,
                    (*cpu).gs_base as libc::c_ulonglong,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
        }
        if newgs < 0x100000 && (*cpu).gs_base >= 0x100000 {
            if env_set!("OCERZ_GSTRACE") {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: GS machdep KEEP gs=%#llx (rejected junk %#llx) rip=%#llx\n".as_ptr(),
                    (*cpu).gs_base as libc::c_ulonglong,
                    newgs as libc::c_ulonglong,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
            machdep_ret(cpu, (*cpu).gs_base);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if ocerz_gs_is_teb_band(newgs) && !ocerz_gs_is_teb_band((*cpu).gs_base) {
            (*cpu).unix_gs_base = (*cpu).gs_base;
        }
        (*cpu).gs_base = newgs;
        if ocerz_gs_is_teb_band(newgs) {
            (*cpu).wine_teb_base = newgs;
        }
        machdep_ret(cpu, (*cpu).gs_base);
        if env_set!("OCERZ_GSTRACE") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: GS machdep[%d] cpu#%u gs=%#llx teb=%#llx rip=%#llx icount=%#llx\n"
                    .as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                (*cpu).gs_base as libc::c_ulonglong,
                (*cpu).wine_teb_base as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
                (*vm).insn_count as libc::c_ulonglong,
            );
        }
        if env_set!("OCERZ_GSTRACE") && (*cpu).gs_base < 0x100000 {
            let r14 = (*cpu).gpr[crate::ffi::OCERZ_R14 as usize];
            let td = ocerz_ld(r14, 8);
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz:   GSBAD caller-ret=%#llx rdi=%#llx r14=%#llx rsp=%#llx [r14]=td=%#llx td_commit=%d [td+0x320]=%#llx [td-0]=%#llx [td+8]=%#llx\n".as_ptr(),
                ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8)
                    as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
                r14 as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
                td as libc::c_ulonglong,
                crate::ffi::ocerz_addr_committed(td),
                if td != 0 {
                    ocerz_ld(td.wrapping_add(0x320), 8)
                } else {
                    0
                } as libc::c_ulonglong,
                if td != 0 { ocerz_ld(td, 8) } else { 0 } as libc::c_ulonglong,
                if td != 0 {
                    ocerz_ld(td.wrapping_add(8), 8)
                } else {
                    0
                } as libc::c_ulonglong,
            );
        }
        if (*vm).strace != 0 || env_set!("OCERZ_SIGTRACE") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: machdep set_cthread_self gs=%#llx comm(gs)=%d comm(gs-8)=%d icount=%#llx\n"
                    .as_ptr(),
                (*cpu).gs_base as libc::c_ulonglong,
                crate::ffi::ocerz_addr_committed((*cpu).gs_base),
                crate::ffi::ocerz_addr_committed((*cpu).gs_base.wrapping_sub(8)),
                (*vm).insn_count as libc::c_ulonglong,
            );
        }
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    crate::ocerz_fatal!("unknown machine-dependent syscall: class=3 num=%d\n", num);
    crate::ffi::OCERZ_STEP_FATAL as c_int
}
