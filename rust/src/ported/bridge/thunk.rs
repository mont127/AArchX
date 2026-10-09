//! A native function that calls into guest code cannot call a guest address
//! directly; the trampoline it calls through is a page ocerz fills with
//! guest-mode code.  ocerz_bridge_native_thunk interns a host function into a
//! slot of that page (each slot: `mov r10d, <slot>; jmp [BR_THUNK_SLOT]`,
//! where the slot word holds the native-fn trampoline), and the trap handler
//! on the guest side runs the host function through the ABI engine.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::bridge::common::{br_settle, gpr, ocerz_g2h};

const BR_THUNK_MAX: usize = 128;
const BR_THUNK_STRIDE: u64 = 16;
const BR_THUNK_SLOT: u64 = 0x800;

#[repr(C)]
struct BrThunk {
    f: *const c_void,
    name: *const c_char,
    notation: *const c_char,
    sig: *mut OcerzAbiSig,
}

static mut G_BR_THUNKS: [BrThunk; BR_THUNK_MAX] = [const {
    BrThunk {
        f: core::ptr::null(),
        name: core::ptr::null(),
        notation: core::ptr::null(),
        sig: null_mut(),
    }
}; BR_THUNK_MAX];
static G_BR_THUNKS_N: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
static G_BR_THUNK_PAGE: AtomicU64 = AtomicU64::new(0);
static mut G_BR_THUNK_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn br_thunk_page_locked() -> u64 {
    unsafe {
        let have = G_BR_THUNK_PAGE.load(Ordering::SeqCst);
        if have != 0 {
            return have;
        }
        let tramp = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_FN);
        let made = if tramp != 0 {
            ocerz_map_anywhere(
                OCERZ_GUEST_PAGE_SIZE as u64,
                libc::PROT_READ | libc::PROT_WRITE,
            )
        } else {
            0
        };
        if made == 0 {
            return 0;
        }
        let buf = ocerz_g2h(made).cast::<u8>();
        core::ptr::write_bytes(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE as usize);
        for k in 0..BR_THUNK_MAX {
            let t = buf.add(k * BR_THUNK_STRIDE as usize);
            let number = k as u32;
            let rel = (BR_THUNK_SLOT as i64 - (k as i64 * BR_THUNK_STRIDE as i64 + 12)) as i32;
            *t = 0x41;
            *t.add(1) = 0xba;
            core::ptr::copy_nonoverlapping(&number as *const u32 as *const u8, t.add(2), 4);
            *t.add(6) = 0xff;
            *t.add(7) = 0x25;
            core::ptr::copy_nonoverlapping(&rel as *const i32 as *const u8, t.add(8), 4);
        }
        core::ptr::copy_nonoverlapping(
            &tramp as *const u64 as *const u8,
            buf.add(BR_THUNK_SLOT as usize),
            8,
        );
        if ocerz_protect(
            made,
            OCERZ_GUEST_PAGE_SIZE as u64,
            libc::PROT_READ | libc::PROT_EXEC,
        ) != OCERZ_OK as c_int
        {
            ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE as u64);
            return 0;
        }
        G_BR_THUNK_PAGE.store(made, Ordering::SeqCst);
        made
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_native_thunk(
    f: *const c_void,
    name: *const c_char,
    notation: *const c_char,
) -> u64 {
    unsafe {
        if f.is_null() || notation.is_null() {
            return 0;
        }
        let mut answer = 0;
        libc::pthread_mutex_lock(&raw mut G_BR_THUNK_LOCK);
        let n = G_BR_THUNKS_N.load(Ordering::SeqCst) as usize;
        let mut k = 0;
        while k < n && G_BR_THUNKS[k].f != f {
            k += 1;
        }
        if k == n && n < BR_THUNK_MAX {
            let sig = libc::calloc(1, size_of::<OcerzAbiSig>()) as *mut OcerzAbiSig;
            if !sig.is_null() && ocerz_abi_parse(notation, sig) == OCERZ_OK as c_int {
                G_BR_THUNKS[n].f = f;
                G_BR_THUNKS[n].name = if name.is_null() {
                    c"(native function)".as_ptr()
                } else {
                    name
                };
                G_BR_THUNKS[n].notation = libc::strdup(notation);
                G_BR_THUNKS[n].sig = sig;
                G_BR_THUNKS_N.store(n as u32 + 1, Ordering::SeqCst);
            } else {
                libc::free(sig as *mut c_void);
                k = BR_THUNK_MAX;
            }
        }
        if k < BR_THUNK_MAX {
            let page = br_thunk_page_locked();
            if page != 0 {
                answer = page + k as u64 * BR_THUNK_STRIDE;
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_THUNK_LOCK);
        answer
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_thunk_trap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let k = (gpr(cpu, OCERZ_R10) & 0xffff_ffff) as u32;
        if k >= G_BR_THUNKS_N.load(Ordering::SeqCst) {
            crate::ocerz_fatal!(
                "bridge: a thunk numbered %u for a native function was called, and ocerz made no such thunk\n",
                k
            );
            return OCERZ_STEP_FATAL as c_int;
        }
        let t = &raw const G_BR_THUNKS[k as usize];
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            c"ocerz".as_ptr(),
            (*t).name,
            (*t).notation,
            (*t).f,
        );
        let rc = ocerz_abi_perform((*t).sig, (*t).f as *mut c_void, cpu);
        ocerz_bridge_lower(&outer);
        if rc != OCERZ_STEP_OK as c_int {
            return rc;
        }
        br_settle(vm, cpu)
    }
}
