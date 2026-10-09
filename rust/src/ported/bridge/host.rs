//! ---- the host libraries are dlopen'd on their own paths ----
//! An apidb library's install_name is a dyld install name, the same name a
//! guest link would have been given: /usr/lib/libSystem.B.dylib resolves to
//! the already-opened process's copy, and anything else is dlopen'd on demand,
//! the first time one of its exports is bridged.  A library's open is taken
//! under a lock and against the host's own signal handlers: dyld loading a
//! framework the guest asked about may in principle install them, and each
//! such hijacking is reported as it happens, because a host handler over the
//! guest's signals would run where ocerz cannot turn the guest's disposition
//! back on.
//!
//! ---- the guest's identity, for what it is worth ----
//! A process's own answers - its name, its argv, the path it believes it runs
//! from - are the host's, because every question is answered by host code.
//! CoreFoundation is the part that looks: its caches and bundle lookups go by
//! the process path it was opened under, so before the first framework open
//! ocerz sets CFProcessPath to the guest's own path, opens CoreFoundation under
//! that name, and puts the host's back.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::Ordering;

use crate::ffi::*;
use crate::ported::bridge::common::br_guest_path;
use crate::ported::bridge::lookup::{BrLib, G_BR_HOST_LOCK, br_lib};

static mut G_BR_CF_IDENTIFIED: c_int = 0;

pub unsafe fn br_identify_corefoundation() {
    unsafe {
        let path = ocerz_dyld_main_path();
        if G_BR_CF_IDENTIFIED != 0 || path.is_null() {
            return;
        }
        G_BR_CF_IDENTIFIED = 1;
        let prev = libc::getenv(OCERZ_BRIDGE_PROCESS_PATH_VAR.as_ptr() as *const c_char);
        let saved = if prev.is_null() {
            null_mut()
        } else {
            libc::strdup(prev)
        };
        libc::setenv(
            OCERZ_BRIDGE_PROCESS_PATH_VAR.as_ptr() as *const c_char,
            path,
            1,
        );
        let mut h = libc::dlopen(
            OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
            libc::RTLD_LAZY | libc::RTLD_LOCAL | RTLD_NOLOAD,
        );
        if h.is_null() {
            h = libc::dlopen(
                OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
                libc::RTLD_LAZY | libc::RTLD_LOCAL,
            );
        }
        if saved.is_null() {
            libc::unsetenv(OCERZ_BRIDGE_PROCESS_PATH_VAR.as_ptr() as *const c_char);
        } else {
            libc::setenv(
                OCERZ_BRIDGE_PROCESS_PATH_VAR.as_ptr() as *const c_char,
                saved,
                1,
            );
        }
        libc::free(saved as *mut c_void);
        if !h.is_null() {
            crate::ocerz_log!(
                "bridge: CoreFoundation initialized with the process path %s\n",
                path
            );
        } else {
            crate::ocerz_log!(
                "bridge: CoreFoundation will not open to take the guest's identity: %s\n",
                libc::dlerror()
            );
        }
    }
}

const RTLD_NOLOAD: c_int = 0x10;
const NSIG: c_int = 32;

unsafe fn br_note_stolen_signals(before: *const libc::sigaction, install_name: *const c_char) {
    unsafe {
        let mut sig = 1;
        while sig < NSIG {
            let mut now: libc::sigaction = core::mem::zeroed();
            if libc::sigaction(sig, core::ptr::null(), &mut now) != 0 {
                sig += 1;
                continue;
            }
            if now.sa_sigaction != (*before.add(sig as usize)).sa_sigaction
                || now.sa_flags != (*before.add(sig as usize)).sa_flags
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: opening %s changed the host disposition of signal %d\n"
                        .as_ptr(),
                    install_name,
                    sig,
                );
            }
            sig += 1;
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_host_library(install_name: *const c_char) -> *mut c_void {
    unsafe {
        let api = ocerz_apidb_library(install_name);
        if api.is_null() {
            return null_mut();
        }
        let lib = br_lib(api);
        if lib.is_null() {
            return null_mut();
        }
        let mut h = (*lib).handle.load(Ordering::SeqCst);
        if !h.is_null() {
            return h;
        }

        libc::pthread_mutex_lock(&raw mut G_BR_HOST_LOCK);
        h = (*lib).handle.load(Ordering::SeqCst);
        if h.is_null() {
            if libc::strcmp(
                install_name,
                OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const c_char,
            ) == 0
            {
                h = libc::RTLD_DEFAULT;
            } else {
                let mut before: [libc::sigaction; NSIG as usize] = core::mem::zeroed();
                for sig in 1..NSIG {
                    libc::sigaction(
                        sig,
                        core::ptr::null(),
                        before.as_mut_ptr().add(sig as usize),
                    );
                }
                if !libc::strstr(install_name, c".framework/".as_ptr()).is_null() {
                    br_identify_corefoundation();
                }
                h = libc::dlopen(
                    install_name,
                    libc::RTLD_LAZY | libc::RTLD_LOCAL | RTLD_NOLOAD,
                );
                if h.is_null() {
                    h = libc::dlopen(install_name, libc::RTLD_LAZY | libc::RTLD_LOCAL);
                }
                if !h.is_null() {
                    br_note_stolen_signals(before.as_ptr(), install_name);
                } else {
                    crate::ocerz_log!(
                        "bridge: host library %s will not open: %s\n",
                        install_name,
                        libc::dlerror()
                    );
                }
            }
            if !h.is_null() {
                (*lib).handle.store(h, Ordering::SeqCst);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_HOST_LOCK);
        h
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_host_symbol(
    install_name: *const c_char,
    host_sym: *const c_char,
) -> *mut c_void {
    unsafe {
        if host_sym.is_null() {
            return null_mut();
        }
        let h = ocerz_bridge_host_library(install_name);
        if h.is_null() {
            null_mut()
        } else {
            libc::dlsym(h, host_sym)
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_set_process_args(argc: c_int, argv: *mut *mut c_char) {
    unsafe {
        *libc::_NSGetArgc() = argc;
        *libc::_NSGetArgv() = argv;
        if argc > 0 && !argv.is_null() && !(*argv).is_null() {
            libc::setprogname(*argv);
        }
    }
}
