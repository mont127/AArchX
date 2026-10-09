//! Builds the initial process stack the way XNU's exec path lays it out: the
//! strings at the top, then the apple[] array, envp, argv and argc below them,
//! with the whole thing 16-byte aligned at entry.  Guest crt0 code and dyld both
//! read this layout positionally, so the order and the alignment are the
//! contract, not an implementation detail.

use core::ffi::{CStr, c_char, c_int, c_ulonglong};
use core::ptr;

use crate::ffi::{
    OCERZ_ENOMEM, OCERZ_OK, OCERZ_RBP, OCERZ_RSP, OcerzImage, OcerzVM, ocerz_map_anywhere,
};
use crate::inline::{ocerz_g2h, ocerz_st};

const GUEST_STACK_SIZE: u64 = 8 << 20;
const STACK_TOP_PAD: c_int = 16;
const APPLE_PREFIX: &CStr = c"executable_path=";

#[inline(always)]
fn align_down16(v: u64) -> u64 {
    v & !0xf
}

#[inline(always)]
unsafe fn push_string(top: &mut u64, s: *const c_char) -> u64 {
    unsafe {
        let n = libc::strlen(s).wrapping_add(1);
        let at = top.wrapping_sub(n as u64);
        libc::memcpy(ocerz_g2h(at), s.cast(), n);
        *top = at;
        at
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_setup_stack(
    vm: *mut OcerzVM,
    img: *const OcerzImage,
    argc: c_int,
    argv: *mut *mut c_char,
    envp: *mut *mut c_char,
) -> c_int {
    unsafe {
        let base = ocerz_map_anywhere(GUEST_STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE);
        if base == 0 {
            crate::ocerz_fatal!("cannot map guest stack\n");
            return OCERZ_ENOMEM;
        }
        (*vm).stack_lo = base;
        (*vm).stack_hi = base.wrapping_add(GUEST_STACK_SIZE);

        let mut envc: c_int = 0;
        while !envp.is_null() && !(*envp.add(envc as usize)).is_null() {
            envc += 1;
        }

        let mut top = (*vm).stack_hi;
        top = top.wrapping_sub(STACK_TOP_PAD as u64);
        libc::memset(ocerz_g2h(top), 0, STACK_TOP_PAD as usize);

        let path = (*img).path.as_ptr();
        let pathlen = libc::strlen(path);
        let prefix_len = APPLE_PREFIX.to_bytes().len();
        let applestr = libc::malloc(pathlen.wrapping_add(prefix_len + 1)).cast::<c_char>();
        if applestr.is_null() {
            crate::ocerz_fatal!("out of memory building apple[0]\n");
            return OCERZ_ENOMEM;
        }
        libc::memcpy(applestr.cast(), APPLE_PREFIX.as_ptr().cast(), prefix_len);
        libc::memcpy(
            applestr.add(prefix_len).cast(),
            path.cast(),
            pathlen.wrapping_add(1),
        );
        let apple_gaddr = push_string(&mut top, applestr);
        libc::free(applestr.cast());

        let mut env_gaddr: *mut u64 = ptr::null_mut();
        if envc > 0 {
            env_gaddr =
                libc::malloc((envc as usize).wrapping_mul(core::mem::size_of::<u64>())).cast();
            if env_gaddr.is_null() {
                crate::ocerz_fatal!("out of memory building envp vector\n");
                return OCERZ_ENOMEM;
            }
        }
        for i in 0..envc {
            *env_gaddr.add(i as usize) = push_string(&mut top, *envp.add(i as usize));
        }

        let mut arg_gaddr: *mut u64 = ptr::null_mut();
        if argc > 0 {
            arg_gaddr =
                libc::malloc((argc as usize).wrapping_mul(core::mem::size_of::<u64>())).cast();
            if arg_gaddr.is_null() {
                crate::ocerz_fatal!("out of memory building argv vector\n");
                libc::free(env_gaddr.cast());
                return OCERZ_ENOMEM;
            }
        }
        for i in 0..argc {
            *arg_gaddr.add(i as usize) = push_string(&mut top, *argv.add(i as usize));
        }

        let strings_base = align_down16(top);
        let unixthread = (*img).entry_is_unixthread != 0;
        let mut nwords = 0u64;
        if !unixthread {
            nwords = nwords.wrapping_add(1);
        }
        nwords = nwords.wrapping_add(1);
        nwords = nwords.wrapping_add((argc as u64).wrapping_add(1));
        nwords = nwords.wrapping_add((envc as u64).wrapping_add(1));
        nwords = nwords.wrapping_add(2);

        let ptr_area = nwords.wrapping_mul(8);
        let rsp = align_down16(strings_base.wrapping_sub(ptr_area));

        let mut w = rsp;
        if !unixthread {
            ocerz_st(w, 8, (*img).mh_gaddr);
            w = w.wrapping_add(8);
        }
        ocerz_st(w, 8, argc as u32 as u64);
        w = w.wrapping_add(8);
        for i in 0..argc {
            ocerz_st(w, 8, *arg_gaddr.add(i as usize));
            w = w.wrapping_add(8);
        }
        ocerz_st(w, 8, 0);
        w = w.wrapping_add(8);
        for i in 0..envc {
            ocerz_st(w, 8, *env_gaddr.add(i as usize));
            w = w.wrapping_add(8);
        }
        ocerz_st(w, 8, 0);
        w = w.wrapping_add(8);
        ocerz_st(w, 8, apple_gaddr);
        w = w.wrapping_add(8);
        ocerz_st(w, 8, 0);

        libc::free(arg_gaddr.cast());
        libc::free(env_gaddr.cast());

        (*vm).cpu.gpr[OCERZ_RSP as usize] = rsp;
        (*vm).cpu.gpr[OCERZ_RBP as usize] = 0;

        crate::ocerz_log!(
            "guest stack [%#llx,%#llx) rsp=%#llx argc=%d envc=%d %s\n",
            (*vm).stack_lo as c_ulonglong,
            (*vm).stack_hi as c_ulonglong,
            rsp as c_ulonglong,
            argc,
            envc,
            if unixthread {
                c"unixthread".as_ptr()
            } else {
                c"dyld".as_ptr()
            }
        );
        OCERZ_OK
    }
}
