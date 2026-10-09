//! ---- method implementations ----
//! A native IMP a guest asks for through method_getImplementation or
//! methodForSelector: cannot be handed over: guest code would call arm64 code.
//! Each is given a guest thunk instead: a page of 16-byte stubs, each carrying
//! its number in r10 and jumping into ocerz's native-IMP trampoline, which
//! lands back in ocerz_objc_imp_trap and the send it describes.  64 pages of
//! 128 stubs each, made on demand and never freed, same as C.
//!
//! imp_implementationWithBlock's implementation is guest code, as libobjc's
//! own trampolines are: a stub that moves self over _cmd, puts the block in
//! front of it and jumps to the block's invoke, or for a block that returns a
//! structure in memory does the same one register further on.  A stub is never
//! rewritten once written, since a translation of it may exist, so
//! imp_removeBlock releases the block and leaves the bytes.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::bridge::common::*;
use crate::ported::objcbridge::common::*;
use crate::ported::objcbridge::send::*;
use crate::ported::objcbridge::encode::OB_SMALL_STRUCT;

const OB_IMP_PER_PAGE: usize = 128;
const OB_IMP_PAGES: usize = 64;
const OB_IMP_MAX: usize = OB_IMP_PER_PAGE * OB_IMP_PAGES;
const OB_IMP_STRIDE: usize = 16;
const OB_IMP_SLOT: usize = 0x800;

#[repr(C)]
pub struct ObImp {
    pub imp: *mut c_void,
    pub types: *const c_char,
    pub stret: c_int,
}

pub static mut G_OB_IMPS: [ObImp; OB_IMP_MAX] = [const {
    ObImp { imp: null_mut(), types: null_mut() as *const c_char, stret: 0 }
}; OB_IMP_MAX];
pub static G_OB_IMPS_N: AtomicU32 = AtomicU32::new(0);
static G_OB_IMP_PAGES: [AtomicU64; OB_IMP_PAGES] =
    [const { AtomicU64::new(0) }; OB_IMP_PAGES];
static mut G_OB_IMP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

pub static G_OB_SEL_METHOD_FOR: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
pub static G_OB_SEL_INSTANCE_METHOD_FOR: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe fn ob_answers_imp(sel: *mut c_void) -> bool {
    unsafe {
        let mut a = G_OB_SEL_METHOD_FOR.load(Ordering::SeqCst);
        if a.is_null() {
            G_OB_SEL_INSTANCE_METHOD_FOR
                .store(ob_sel_register_name(c"instanceMethodForSelector:".as_ptr()), Ordering::SeqCst);
            a = ob_sel_register_name(c"methodForSelector:".as_ptr());
            G_OB_SEL_METHOD_FOR.store(a, Ordering::SeqCst);
        }
        sel == a || sel == G_OB_SEL_INSTANCE_METHOD_FOR.load(Ordering::SeqCst)
    }
}

unsafe fn ob_types_stret(types: *const c_char) -> c_int {
    unsafe {
        let mut notation = [0i8; OCERZ_OBJC_NOTATION_MAX as usize];
        let mut nargs = 0;
        let mut blocks = 0u32;
        let mut fnptrs = 0u32;
        #[thread_local]
        static mut SIG: OcerzAbiSig = OcerzAbiSig {
            ret: 0,
            arg: [0; 16],
            cb: [[0; 48]; 16],
            nargs: 0,
            ret_struct: OcerzAbiStruct { size: 0, align: 0, nmember: 0, member: [0; 16], offset: [0; 16] },
            arg_struct: [const {
                OcerzAbiStruct { size: 0, align: 0, nmember: 0, member: [0; 16], offset: [0; 16] }
            }; 16],
            ret_cb: [0; 48],
        };
        if types.is_null()
            || ocerz_objc_notation(types, notation.as_mut_ptr(), notation.len(), &mut nargs, &mut blocks, &mut fnptrs)
                != OCERZ_OBJC_OK as c_int
            || ocerz_abi_parse(notation.as_ptr(), &raw mut SIG) != OCERZ_OK as c_int
        {
            return 0;
        }
        (SIG.ret == b'{' as c_char && SIG.ret_struct.size as usize > OB_SMALL_STRUCT) as c_int
    }
}

unsafe fn ob_imp_page(page: usize) -> u64 {
    unsafe {
        let have = G_OB_IMP_PAGES[page].load(Ordering::SeqCst);
        if have != 0 {
            return have;
        }
        let tramp = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_IMP);
        let made = if tramp != 0 {
            ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_WRITE)
        } else {
            0
        };
        if made == 0 {
            return 0;
        }
        let buf = ocerz_g2h(made) as *mut u8;
        core::ptr::write_bytes(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE as usize);
        for k in 0..OB_IMP_PER_PAGE {
            let t = buf.add(k * OB_IMP_STRIDE);
            let number = (page * OB_IMP_PER_PAGE + k) as u32;
            let rel = (OB_IMP_SLOT as i64 - (k * OB_IMP_STRIDE + 12) as i64) as i32;
            *t = 0x41;
            *t.add(1) = 0xba;
            core::ptr::write_unaligned(t.add(2) as *mut u32, number);
            *t.add(6) = 0xff;
            *t.add(7) = 0x25;
            core::ptr::write_unaligned(t.add(8) as *mut i32, rel);
        }
        core::ptr::write_unaligned(buf.add(OB_IMP_SLOT) as *mut u64, tramp);
        if ocerz_protect(made, OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_EXEC)
            != OCERZ_OK as c_int
        {
            ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE as u64);
            return 0;
        }
        G_OB_IMP_PAGES[page].store(made, Ordering::SeqCst);
        made
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_for_guest(native_imp: *mut c_void, types: *const c_char) -> u64 {
    unsafe {
        if native_imp.is_null() {
            return 0;
        }
        let mut guest_fn = 0u64;
        if !ocerz_abi_callback_sig(native_imp, &mut guest_fn).is_null() && guest_fn != 0 {
            return guest_fn;
        }
        let as_guest = ocerz_h2g(native_imp);
        if ocerz_abi_is_guest_code(as_guest) != 0 {
            return as_guest;
        }

        let mut answer = 0u64;
        libc::pthread_mutex_lock(&raw mut G_OB_IMP_LOCK);
        let n = G_OB_IMPS_N.load(Ordering::SeqCst) as usize;
        let mut k = 0usize;
        while k < n {
            if G_OB_IMPS[k].imp == native_imp {
                break;
            }
            k += 1;
        }
        if k == n && n < OB_IMP_MAX {
            G_OB_IMPS[n].imp = native_imp;
            G_OB_IMPS[n].types = if types.is_null() { null_mut() } else { libc::strdup(types) };
            G_OB_IMPS[n].stret = ob_types_stret(types);
            G_OB_IMPS_N.store((n + 1) as u32, Ordering::SeqCst);
        }
        if k < OB_IMP_MAX {
            if G_OB_IMPS[k].types.is_null() && !types.is_null() {
                G_OB_IMPS[k].types = libc::strdup(types);
                G_OB_IMPS[k].stret = ob_types_stret(types);
            }
            let page = ob_imp_page(k / OB_IMP_PER_PAGE);
            if page != 0 {
                answer = page + (k % OB_IMP_PER_PAGE * OB_IMP_STRIDE) as u64;
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_IMP_LOCK);
        if answer == 0 {
            ob_stop!(
                "no thunk is left for native implementation %p: all %u are bound, or no guest page could be made for them",
                native_imp,
                OB_IMP_MAX as u32
            );
        }
        answer
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_from_guest(guest_imp: u64) -> *mut c_void {
    unsafe {
        if guest_imp == 0 {
            return null_mut();
        }
        for p in 0..OB_IMP_PAGES {
            let page = G_OB_IMP_PAGES[p].load(Ordering::SeqCst);
            if page == 0 {
                break;
            }
            let off = guest_imp.wrapping_sub(page);
            if off >= (OB_IMP_PER_PAGE * OB_IMP_STRIDE) as u64 || off % OB_IMP_STRIDE as u64 != 0 {
                continue;
            }
            let k = p * OB_IMP_PER_PAGE + (off / OB_IMP_STRIDE as u64) as usize;
            return if k < G_OB_IMPS_N.load(Ordering::SeqCst) as usize {
                G_OB_IMPS[k].imp
            } else {
                null_mut()
            };
        }
        null_mut()
    }
}

const OB_BLOCK_IMP_BYTES: usize = 32;
const OB_BLOCK_IMPS: usize = 4096;

static mut G_OB_BLOCK_IMP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_OB_BLOCK_IMP_PAGE: u64 = 0;
static mut G_OB_BLOCK_IMP_USED: u64 = 0;
static mut G_OB_BLOCK_IMP_AT: [u64; OB_BLOCK_IMPS] = [0; OB_BLOCK_IMPS];
static mut G_OB_BLOCK_IMP_BLOCK: [u64; OB_BLOCK_IMPS] = [0; OB_BLOCK_IMPS];
static mut G_OB_BLOCK_IMPS: c_int = 0;

unsafe fn ob_block_imp_find_locked(imp: u64) -> c_int {
    unsafe {
        let mut k = 0;
        while imp != 0 && k < G_OB_BLOCK_IMPS {
            if G_OB_BLOCK_IMP_AT[k as usize] == imp {
                return k;
            }
            k += 1;
        }
        -1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_implementationWithBlock(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let given = gpr(cpu, OCERZ_RDI);
        if given == 0 {
            ob_stop!("imp_implementationWithBlock was given no block");
        }
        let block = ocerz_block_copy_guest(given);
        let invoke = ocerz_ld(block + 16, 8);
        if ocerz_abi_is_guest_code(invoke) == 0 {
            ob_stop!(
                "imp_implementationWithBlock was given a native block (%#llx), whose invoke an x86 implementation cannot call",
                given
            );
        }
        let stret = (ocerz_ld(block + 8, 4) & (1 << 29)) != 0;
        let mut code = [0xccu8; OB_BLOCK_IMP_BYTES];
        static PLAIN: [u8; 5] = [0x48, 0x89, 0xfe, 0x48, 0xbf];
        static PLAIN_JUMP: [u8; 3] = [0xff, 0x67, 0x10];
        static MEMORY: [u8; 5] = [0x48, 0x89, 0xf2, 0x48, 0xbe];
        static MEMORY_JUMP: [u8; 3] = [0xff, 0x66, 0x10];
        code[..5].copy_from_slice(if stret { &MEMORY } else { &PLAIN });
        core::ptr::write_unaligned(code.as_mut_ptr().add(5) as *mut u64, block);
        code[13..16].copy_from_slice(if stret { &MEMORY_JUMP } else { &PLAIN_JUMP });
        libc::pthread_mutex_lock(&raw mut G_OB_BLOCK_IMP_LOCK);
        if G_OB_BLOCK_IMPS == OB_BLOCK_IMPS as c_int {
            ob_stop!(
                "imp_implementationWithBlock has made %d implementations, as many as ocerz keeps",
                OB_BLOCK_IMPS as c_int
            );
        }
        if G_OB_BLOCK_IMP_PAGE == 0 || G_OB_BLOCK_IMP_USED + OB_BLOCK_IMP_BYTES as u64 > OCERZ_GUEST_PAGE_SIZE as u64 {
            G_OB_BLOCK_IMP_PAGE = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_WRITE);
            if G_OB_BLOCK_IMP_PAGE == 0 {
                ob_stop!("no guest page could be set up for an implementation made from a block");
            }
            core::ptr::write_bytes(ocerz_g2h(G_OB_BLOCK_IMP_PAGE) as *mut u8, 0xcc, OCERZ_GUEST_PAGE_SIZE as usize);
            G_OB_BLOCK_IMP_USED = 0;
        } else if ocerz_protect(G_OB_BLOCK_IMP_PAGE, OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_WRITE) != OCERZ_OK as c_int {
            ob_stop!("the page of implementations made from blocks could not be made writable");
        }
        let imp = G_OB_BLOCK_IMP_PAGE + G_OB_BLOCK_IMP_USED;
        libc::memcpy(ocerz_g2h(imp), code.as_ptr() as *const c_void, OB_BLOCK_IMP_BYTES);
        G_OB_BLOCK_IMP_USED += OB_BLOCK_IMP_BYTES as u64;
        if ocerz_protect(G_OB_BLOCK_IMP_PAGE, OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_EXEC) != OCERZ_OK as c_int {
            ob_stop!("the page of implementations made from blocks could not be made executable");
        }
        G_OB_BLOCK_IMP_AT[G_OB_BLOCK_IMPS as usize] = imp;
        G_OB_BLOCK_IMP_BLOCK[G_OB_BLOCK_IMPS as usize] = block;
        G_OB_BLOCK_IMPS += 1;
        libc::pthread_mutex_unlock(&raw mut G_OB_BLOCK_IMP_LOCK);
        ob_return(cpu, imp);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_getBlock(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        static FN: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "imp_getBlock");
        let imp = gpr(cpu, OCERZ_RDI);
        libc::pthread_mutex_lock(&raw mut G_OB_BLOCK_IMP_LOCK);
        let k = ob_block_imp_find_locked(imp);
        let mut block = if k >= 0 { G_OB_BLOCK_IMP_BLOCK[k as usize] } else { 0 };
        libc::pthread_mutex_unlock(&raw mut G_OB_BLOCK_IMP_LOCK);
        if k < 0 && imp != 0 && ocerz_abi_is_guest_code(imp) == 0 {
            let f: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
                core::mem::transmute(ob_need(&raw const FN));
            let b = f(ocerz_g2h(imp));
            block = if b.is_null() { 0 } else { ocerz_h2g(b) };
        }
        ob_return(cpu, block);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_removeBlock(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        static FN: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "imp_removeBlock");
        static RELEASE: ObSym = obsym!(OCERZ_BRIDGE_LIBSYSTEM, "_Block_release");
        let imp = gpr(cpu, OCERZ_RDI);
        libc::pthread_mutex_lock(&raw mut G_OB_BLOCK_IMP_LOCK);
        let k = ob_block_imp_find_locked(imp);
        let block = if k >= 0 { G_OB_BLOCK_IMP_BLOCK[k as usize] } else { 0 };
        if k >= 0 {
            G_OB_BLOCK_IMP_BLOCK[k as usize] = 0;
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_BLOCK_IMP_LOCK);
        let mut removed = 0u64;
        if k >= 0 {
            if block != 0 {
                let r: unsafe extern "C" fn(*mut c_void) =
                    core::mem::transmute(ob_need(&raw const RELEASE));
                r(ocerz_g2h(block));
            }
            removed = (block != 0) as u64;
        } else if imp != 0 && ocerz_abi_is_guest_code(imp) == 0 {
            let f: unsafe extern "C" fn(*mut c_void) -> bool = core::mem::transmute(ob_need(&raw const FN));
            removed = f(ocerz_g2h(imp)) as u64;
        }
        ob_return(cpu, removed);
        ob_settle(vm, cpu)
    }
}
