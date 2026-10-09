//! fcntl, its $NOCANCEL form, ioctl, sem_open, shm_open, semctl and ulimit:
//! the rest of the variadic calls whose optional argument is fixed.
//!
//! fcntl's argument is an int for most commands and a pointer for the ones
//! Apple's libc lists as taking one - the locks, including the open file
//! description ones, F_PREALLOCATE, F_PUNCHHOLE, F_SETSIZE, the advisory reads,
//! F_LOG2PHYS, the F_GETPATH family, the code-signature commands, F_TRANSCODEKEY,
//! F_TRIM_ACTIVE_FILE, F_SPECULATIVE_READ, F_CHECK_LV and F_ATTRIBUTION_TAG -
//! and only a pointer is translated.  The word is handed on as a pointer-sized
//! slot either way: the host reads an int command's argument as the low half of
//! it, which is exactly the int the guest passed, whatever x86 left in the upper
//! half.  ioctl's argument is always a pointer-sized word, as Apple's ioctl
//! reads it, and it is translated when the request encodes a direction, IOC_IN
//! or IOC_OUT, and handed on as it is for an _IO request that carries a value or
//! nothing.
//!
//! A structure a pointer command points at crosses unconverted, so its layout
//! has to be the same on both architectures.  Every one was checked by
//! compiling offsetof and sizeof for x86_64 and arm64 and comparing: struct
//! flock, struct flocktimeout, fstore_t, struct radvisory, struct log2phys,
//! fsignatures_t, fpunchhole_t, ftrimactivefile_t, fspecread_t, fchecklv_t,
//! fattributiontag_t, struct winsize, struct termios, struct semid_ds, struct
//! ipc_perm, union semun, struct ifreq and struct ifconf are byte for byte the
//! same, and the ioctl request numbers, which encode the structure's size, agree.
//! No command is refused for its layout, because none differs.  The ones whose
//! structure carries a pointer of its own, the signature blobs, F_RDADVISEV's
//! ranges and SIOCGIFCONF's buffer, are right only where a guest address is a
//! host address, which is the identity map a position-independent guest runs in.
//! F_RDADVISEV arrived with the macOS 27 SDK, so an older SDK - the one a
//! release runner builds with - gets it defined by its number (116) instead.
//!
//! These handlers raise a bridge frame around the host call, because the host
//! dereferences guest pointers there, and a fault is reported naming the call.

use core::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};

use super::common::*;
use crate::ffi::*;

const F_RDADVISEV: c_int = 116;
const F_SETLKWTIMEOUT: c_int = 10;
const F_PREALLOCATE: c_int = 42;
const F_SETSIZE: c_int = 43;
const F_RDADVISE: c_int = 44;
const F_LOG2PHYS: c_int = 49;
const F_GETPATH: c_int = 50;
const F_PATHPKG_CHECK: c_int = 52;
const F_ADDSIGS: c_int = 59;
const F_ADDFILESIGS: c_int = 61;
const F_LOG2PHYS_EXT: c_int = 65;
const F_GETLKPID: c_int = 66;
const F_GETPATH_MTMINFO: c_int = 71;
const F_GETCODEDIR: c_int = 72;
const F_TRANSCODEKEY: c_int = 75;
const F_FINDSIGS: c_int = 78;
const F_ADDFILESIGS_FOR_DYLD_SIM: c_int = 83;
const F_OFD_SETLK: c_int = 90;
const F_OFD_SETLKW: c_int = 91;
const F_OFD_GETLK: c_int = 92;
const F_OFD_SETLKWTIMEOUT: c_int = 93;
const F_ADDFILESIGS_RETURN: c_int = 97;
const F_CHECK_LV: c_int = 98;
const F_PUNCHHOLE: c_int = 99;
const F_TRIM_ACTIVE_FILE: c_int = 100;
const F_SPECULATIVE_READ: c_int = 101;
const F_GETPATH_NOFIRMLINK: c_int = 102;
const F_ADDFILESIGS_INFO: c_int = 103;
const F_ADDFILESUPPL: c_int = 104;
const F_GETSIGSINFO: c_int = 105;
const F_ATTRIBUTION_TAG: c_int = 111;
const F_ADDSIGS_MAIN_BINARY: c_int = 113;
const UL_SETFSIZE: c_int = 2;
const SB_IOC_IN: c_ulong = 0x80000000;
const SB_IOC_OUT: c_ulong = 0x40000000;

unsafe extern "C" {
    #[link_name = "fcntl$NOCANCEL"]
    fn host_fcntl_nocancel(fd: c_int, cmd: c_int, ...) -> c_int;
    fn ulimit(cmd: c_int, ...) -> c_long;
}

fn sb_fcntl_takes_pointer(cmd: c_int) -> c_int {
    match cmd {
        libc::F_GETLK | libc::F_SETLK | libc::F_SETLKW | F_SETLKWTIMEOUT | F_GETLKPID
        | F_OFD_GETLK | F_OFD_SETLK | F_OFD_SETLKW | F_OFD_SETLKWTIMEOUT
        | F_PREALLOCATE | F_PUNCHHOLE | F_SETSIZE | F_RDADVISE | F_RDADVISEV
        | F_LOG2PHYS | F_LOG2PHYS_EXT | F_GETPATH | F_GETPATH_NOFIRMLINK
        | F_GETPATH_MTMINFO | F_GETCODEDIR | F_PATHPKG_CHECK | F_ADDSIGS
        | F_ADDFILESIGS | F_ADDFILESIGS_FOR_DYLD_SIM | F_ADDFILESIGS_RETURN
        | F_ADDFILESIGS_INFO | F_ADDFILESUPPL | F_ADDSIGS_MAIN_BINARY | F_FINDSIGS
        | F_TRANSCODEKEY | F_TRIM_ACTIVE_FILE | F_SPECULATIVE_READ | F_CHECK_LV
        | F_GETSIGSINFO | F_ATTRIBUTION_TAG => 1,
        _ => 0,
    }
}

unsafe fn sb_fcntl_arg(cmd: c_int, raw: u64) -> *mut c_void {
    unsafe {
        if sb_fcntl_takes_pointer(cmd) != 0 { sb_ptr(raw) } else { raw as *mut c_void }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fcntl(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let cmd = sb_arg(cpu, 1) as c_int;
        let arg = sb_fcntl_arg(cmd, sb_arg(cpu, 2));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_fcntl".as_ptr(), c"i(iip)".as_ptr(), libc::fcntl as *const _);
        let r = libc::fcntl(fd, cmd, arg);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fcntl_nocancel(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let cmd = sb_arg(cpu, 1) as c_int;
        let arg = sb_fcntl_arg(cmd, sb_arg(cpu, 2));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_fcntl$NOCANCEL".as_ptr(), c"i(iip)".as_ptr(), host_fcntl_nocancel as *const _);
        let r = host_fcntl_nocancel(fd, cmd, arg);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_ioctl(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let request = sb_arg(cpu, 1) as c_ulong;
        let raw = sb_arg(cpu, 2);
        let arg = if (request & (SB_IOC_IN | SB_IOC_OUT)) != 0 {
            sb_ptr(raw)
        } else {
            raw as *mut c_void
        };
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_ioctl".as_ptr(), c"i(iLp)".as_ptr(), libc::ioctl as *const _);
        let r = libc::ioctl(fd, request, arg);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sem_open(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let oflag = sb_arg(cpu, 1) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sem_open".as_ptr(), c"p(piiu)".as_ptr(), libc::sem_open as *const _);
        let r = if (oflag & libc::O_CREAT) != 0 {
            libc::sem_open(name, oflag, sb_arg(cpu, 2) as c_int, sb_arg(cpu, 3) as c_uint)
        } else {
            libc::sem_open(name, oflag)
        };
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_shm_open(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let oflag = sb_arg(cpu, 1) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_shm_open".as_ptr(), c"i(pii)".as_ptr(), libc::shm_open as *const _);
        let r = if (oflag & libc::O_CREAT) != 0 {
            libc::shm_open(name, oflag, sb_arg(cpu, 2) as c_int)
        } else {
            libc::shm_open(name, oflag)
        };
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_semctl(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let semid = sb_arg(cpu, 0) as c_int;
        let semnum = sb_arg(cpu, 1) as c_int;
        let cmd = sb_arg(cpu, 2) as c_int;
        let raw = sb_arg(cpu, 3);
        let u = if cmd == libc::IPC_STAT || cmd == libc::IPC_SET {
            sb_ptr(raw)
        } else if cmd == libc::GETALL || cmd == libc::SETALL {
            sb_ptr(raw)
        } else {
            raw as *mut c_void
        };
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_semctl".as_ptr(), c"i(iiip)".as_ptr(), libc::semctl as *const _);
        let r = libc::semctl(semid, semnum, cmd, u);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_ulimit(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cmd = sb_arg(cpu, 0) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_ulimit".as_ptr(), c"l(il)".as_ptr(), ulimit as *const _);
        let r = if cmd == UL_SETFSIZE {
            ulimit(cmd, sb_arg(cpu, 1) as c_long)
        } else {
            ulimit(cmd)
        };
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r)
    }
}
