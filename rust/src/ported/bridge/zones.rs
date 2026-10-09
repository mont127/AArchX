//! A malloc_zone_t is a table of function pointers the guest code calls
//! itself, so malloc's zones are bridged from the other direction than
//! everything else here: the guest is given a page of its own full of thunks
//! (see the thunk file) that enter this file's brz_ functions, which call the
//! matching entry of the real zone.  ocerz makes one view per host zone the
//! guest meets, linked so a guest-created zone - which ocerz also meets - is
//! answered by forwarding to its guest function pointers rather than to a
//! host zone the guest's own pointers would confuse.

use core::ffi::{c_int, c_uint, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::ffi::*;
unsafe extern "C" {
    fn malloc_destroy_zone(zone: *mut c_void);
    fn malloc_zone_statistics(
        zone: *mut libc::malloc_zone_t,
        stats: *mut libc::malloc_statistics_t,
    );
    fn malloc_set_zone_name(zone: *mut c_void, name: *const c_char);
}

use crate::ported::bridge::common::{br_answer, gpr, ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};
use core::ffi::c_char;

pub type MallocZone = BrMallocZone;
pub type MallocIntrospect = BrMallocIntrospect;

#[repr(C)]
pub struct BrMallocIntrospect {
    pub enumerator: *mut c_void,
    pub good_size: Option<unsafe extern "C" fn(*mut BrMallocZone, usize) -> usize>,
    pub check: Option<unsafe extern "C" fn(*mut BrMallocZone) -> c_int>,
    pub print: Option<unsafe extern "C" fn(*mut BrMallocZone, c_int)>,
    pub log: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void)>,
    pub force_lock: Option<unsafe extern "C" fn(*mut BrMallocZone)>,
    pub force_unlock: Option<unsafe extern "C" fn(*mut BrMallocZone)>,
    pub statistics: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut libc::malloc_statistics_t)>,
    pub zone_locked: Option<unsafe extern "C" fn(*mut BrMallocZone) -> c_int>,
    pub enable_discharge_checking: *mut c_void,
    pub disable_discharge_checking: *mut c_void,
    pub discharge: *mut c_void,
    pub enumerate_discharged_pointers: *mut c_void,
    pub reinit_lock: Option<unsafe extern "C" fn(*mut BrMallocZone)>,
    pub print_task: *mut c_void,
    pub task_statistics: *mut c_void,
}

#[repr(C)]
pub struct BrMallocZone {
    pub reserved1: *mut c_void,
    pub reserved2: *mut c_void,
    pub size: Option<unsafe extern "C" fn(*mut BrMallocZone, *const c_void) -> usize>,
    pub malloc: Option<unsafe extern "C" fn(*mut BrMallocZone, usize) -> *mut c_void>,
    pub calloc: Option<unsafe extern "C" fn(*mut BrMallocZone, usize, usize) -> *mut c_void>,
    pub valloc: Option<unsafe extern "C" fn(*mut BrMallocZone, usize) -> *mut c_void>,
    pub free: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void)>,
    pub realloc: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void, usize) -> *mut c_void>,
    pub destroy: Option<unsafe extern "C" fn(*mut BrMallocZone)>,
    pub zone_name: *const c_char,
    pub batch_malloc:
        Option<unsafe extern "C" fn(*mut BrMallocZone, usize, *mut *mut c_void, c_uint) -> c_uint>,
    pub batch_free: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut *mut c_void, c_uint)>,
    pub introspect: *mut BrMallocIntrospect,
    pub version: c_uint,
    pub memalign: Option<unsafe extern "C" fn(*mut BrMallocZone, usize, usize) -> *mut c_void>,
    pub free_definite_size: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void, usize)>,
    pub pressure_relief: Option<unsafe extern "C" fn(*mut BrMallocZone, usize) -> usize>,
    pub claimed_address: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void) -> c_int>,
    pub try_free_default: Option<unsafe extern "C" fn(*mut BrMallocZone, *mut c_void)>,
}

const BR_ZONE_WORDS: usize = 25;
const BR_ZONE_VERSION_WORD: usize = 13;
const BR_ZONE_VERSION_CAP: u32 = 13;
const BR_ZONE_VIEWS: usize = 32;

#[repr(C)]
struct BrZoneView {
    view: u64,
    native: *mut BrMallocZone,
    made: [u64; BR_ZONE_WORDS],
}

static mut G_BR_VIEWS: [BrZoneView; BR_ZONE_VIEWS] = [const {
    BrZoneView {
        view: 0,
        native: null_mut(),
        made: [0; BR_ZONE_WORDS],
    }
}; BR_ZONE_VIEWS];
static G_BR_VIEWS_N: AtomicU32 = AtomicU32::new(0);
static mut G_BR_VIEWS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn br_view_find(view: u64) -> *const BrZoneView {
    unsafe {
        let n = G_BR_VIEWS_N.load(Ordering::SeqCst);
        let mut k = 0;
        while view != 0 && k < n as usize {
            if G_BR_VIEWS[k].view == view {
                return &raw const G_BR_VIEWS[k];
            }
            k += 1;
        }
        core::ptr::null()
    }
}

unsafe fn brz_native(given: *mut BrMallocZone) -> *mut BrMallocZone {
    unsafe {
        let v = br_view_find(ocerz_h2g(given as *const c_void));
        if v.is_null() { given } else { (*v).native }
    }
}

unsafe extern "C" fn brz_size(g: *mut BrMallocZone, p: *const c_void) -> usize {
    unsafe {
        let z = brz_native(g);
        if let Some(s) = (*z).size { s(z, p) } else { 0 }
    }
}

unsafe extern "C" fn brz_malloc(g: *mut BrMallocZone, n: usize) -> *mut c_void {
    unsafe { ((*brz_native(g)).malloc.unwrap_unchecked())(brz_native(g), n) }
}

unsafe extern "C" fn brz_calloc(g: *mut BrMallocZone, a: usize, b: usize) -> *mut c_void {
    unsafe { ((*brz_native(g)).calloc.unwrap_unchecked())(brz_native(g), a, b) }
}

unsafe extern "C" fn brz_valloc(g: *mut BrMallocZone, n: usize) -> *mut c_void {
    unsafe { ((*brz_native(g)).valloc.unwrap_unchecked())(brz_native(g), n) }
}

unsafe extern "C" fn brz_free(g: *mut BrMallocZone, p: *mut c_void) {
    unsafe { ((*brz_native(g)).free.unwrap_unchecked())(brz_native(g), p) }
}

unsafe extern "C" fn brz_realloc(g: *mut BrMallocZone, p: *mut c_void, n: usize) -> *mut c_void {
    unsafe { ((*brz_native(g)).realloc.unwrap_unchecked())(brz_native(g), p, n) }
}

unsafe extern "C" fn brz_destroy(g: *mut BrMallocZone) {
    unsafe { malloc_destroy_zone(brz_native(g) as *mut c_void) }
}

unsafe extern "C" fn brz_batch_malloc(
    g: *mut BrMallocZone,
    size: usize,
    results: *mut *mut c_void,
    n: c_uint,
) -> c_uint {
    unsafe {
        let z = brz_native(g);
        if let Some(f) = (*z).batch_malloc {
            f(z, size, results, n)
        } else {
            0
        }
    }
}

unsafe extern "C" fn brz_batch_free(g: *mut BrMallocZone, ptrs: *mut *mut c_void, n: c_uint) {
    unsafe {
        let z = brz_native(g);
        if let Some(f) = (*z).batch_free {
            f(z, ptrs, n);
            return;
        }
        for k in 0..n as usize {
            let p = *ptrs.add(k);
            if !p.is_null() {
                ((*z).free.unwrap_unchecked())(z, p);
            }
        }
    }
}

unsafe extern "C" fn brz_memalign(g: *mut BrMallocZone, align: usize, n: usize) -> *mut c_void {
    unsafe {
        let z = brz_native(g);
        if let Some(f) = (*z).memalign {
            f(z, align, n)
        } else {
            null_mut()
        }
    }
}

unsafe extern "C" fn brz_free_definite_size(g: *mut BrMallocZone, p: *mut c_void, n: usize) {
    unsafe {
        let z = brz_native(g);
        if (*z).version >= 6 {
            if let Some(f) = (*z).free_definite_size {
                f(z, p, n);
                return;
            }
        }
        ((*z).free.unwrap_unchecked())(z, p);
    }
}

unsafe extern "C" fn brz_pressure_relief(g: *mut BrMallocZone, goal: usize) -> usize {
    unsafe {
        let z = brz_native(g);
        if (*z).version >= 8 {
            if let Some(f) = (*z).pressure_relief {
                return f(z, goal);
            }
        }
        0
    }
}

unsafe extern "C" fn brz_claimed_address(g: *mut BrMallocZone, p: *mut c_void) -> c_int {
    unsafe {
        let z = brz_native(g);
        if (*z).version >= 10 {
            if let Some(f) = (*z).claimed_address {
                return f(z, p);
            }
        }
        if let Some(s) = (*z).size {
            if s(z, p) != 0 {
                return 1;
            }
        }
        0
    }
}

unsafe extern "C" fn brz_try_free_default(g: *mut BrMallocZone, p: *mut c_void) {
    unsafe {
        let z = brz_native(g);
        if (*z).version >= 13 {
            if let Some(f) = (*z).try_free_default {
                f(z, p);
                return;
            }
        }
        libc::free(p);
    }
}

unsafe extern "C" fn brz_good_size(g: *mut BrMallocZone, n: usize) -> usize {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).good_size {
                return f(z, n);
            }
        }
        libc::malloc_good_size(n)
    }
}

unsafe extern "C" fn brz_check(g: *mut BrMallocZone) -> c_int {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).check {
                return f(z);
            }
        }
        1
    }
}

unsafe extern "C" fn brz_print(g: *mut BrMallocZone, verbose: c_int) {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).print {
                f(z, verbose);
            }
        }
    }
}

unsafe extern "C" fn brz_log(g: *mut BrMallocZone, address: *mut c_void) {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).log {
                f(z, address);
            }
        }
    }
}

unsafe extern "C" fn brz_force_lock(g: *mut BrMallocZone) {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).force_lock {
                f(z);
            }
        }
    }
}

unsafe extern "C" fn brz_force_unlock(g: *mut BrMallocZone) {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).force_unlock {
                f(z);
            }
        }
    }
}

unsafe extern "C" fn brz_statistics(g: *mut BrMallocZone, stats: *mut libc::malloc_statistics_t) {
    unsafe { malloc_zone_statistics(brz_native(g) as *mut libc::malloc_zone_t, stats) }
}

unsafe extern "C" fn brz_zone_locked(g: *mut BrMallocZone) -> c_int {
    unsafe {
        let z = brz_native(g);
        if !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).zone_locked {
                return f(z);
            }
        }
        0
    }
}

unsafe extern "C" fn brz_reinit_lock(g: *mut BrMallocZone) {
    unsafe {
        let z = brz_native(g);
        if (*z).version >= 9 && !(*z).introspect.is_null() {
            if let Some(f) = (*(*z).introspect).reinit_lock {
                f(z);
            }
        }
    }
}

const BR_ZONE_INTROSPECT_WORD: usize = 12;
const BR_ZONE_INTROSPECT_AT: u64 = 0x400;
const BR_ZONE_INTROSPECT_WORDS: usize = 17;

#[repr(C)]
struct BrZoneFn {
    word: c_int,
    f: *const c_void,
    name: *const core::ffi::c_char,
    notation: *const core::ffi::c_char,
}
unsafe impl Sync for BrZoneFn {}

macro_rules! zf {
    ($(($w:literal, $f:path, $n:literal, $s:literal),)*) => {
        &[$(BrZoneFn {
            word: $w,
            f: $f as *const c_void,
            name: concat!($n, "\0").as_ptr() as *const core::ffi::c_char,
            notation: concat!($s, "\0").as_ptr() as *const core::ffi::c_char,
        }),*]
    };
}

static G_BR_INTROSPECT_FNS: &[BrZoneFn] = zf! {
    (1, brz_good_size, "(zone good_size)", "L(pL)"),
    (2, brz_check, "(zone check)", "i(p)"),
    (3, brz_print, "(zone print)", "v(pi)"),
    (4, brz_log, "(zone log)", "v(pp)"),
    (5, brz_force_lock, "(zone force_lock)", "v(p)"),
    (6, brz_force_unlock, "(zone force_unlock)", "v(p)"),
    (7, brz_statistics, "(zone statistics)", "v(pp)"),
    (8, brz_zone_locked, "(zone zone_locked)", "i(p)"),
    (13, brz_reinit_lock, "(zone reinit_lock)", "v(p)"),
};

static G_BR_ZONE_FNS: &[BrZoneFn] = zf! {
    (2, brz_size, "(zone size)", "L(pp)"),
    (3, brz_malloc, "(zone malloc)", "p(pL)"),
    (4, brz_calloc, "(zone calloc)", "p(pLL)"),
    (5, brz_valloc, "(zone valloc)", "p(pL)"),
    (6, brz_free, "(zone free)", "v(pp)"),
    (7, brz_realloc, "(zone realloc)", "p(ppL)"),
    (8, brz_destroy, "(zone destroy)", "v(p)"),
    (10, brz_batch_malloc, "(zone batch_malloc)", "u(pLpu)"),
    (11, brz_batch_free, "(zone batch_free)", "v(ppu)"),
    (14, brz_memalign, "(zone memalign)", "p(pLL)"),
    (15, brz_free_definite_size, "(zone free_definite_size)", "v(ppL)"),
    (16, brz_pressure_relief, "(zone pressure_relief)", "L(pL)"),
    (17, brz_claimed_address, "(zone claimed_address)", "i(pp)"),
    (18, brz_try_free_default, "(zone try_free_default)", "v(pp)"),
};

pub unsafe extern "C" fn br_zone_view(native_zone: *mut c_void) -> u64 {
    unsafe {
        if native_zone.is_null() {
            return 0;
        }
        let z = native_zone as *mut BrMallocZone;
        libc::pthread_mutex_lock(&raw mut G_BR_VIEWS_LOCK);
        let n = G_BR_VIEWS_N.load(Ordering::SeqCst) as usize;
        let mut k = 0;
        while k < n && G_BR_VIEWS[k].native != z {
            k += 1;
        }
        let mut answer = if k < n { G_BR_VIEWS[k].view } else { 0 };
        if answer == 0 && n < BR_ZONE_VIEWS {
            let page = ocerz_map_anywhere(
                OCERZ_GUEST_PAGE_SIZE as u64,
                libc::PROT_READ | libc::PROT_WRITE,
            );
            let v = &raw mut G_BR_VIEWS[n];
            let mut ok = page != 0;
            core::ptr::write_bytes((*v).made.as_mut_ptr(), 0, BR_ZONE_WORDS);
            let mut f = 0;
            while ok && f < G_BR_ZONE_FNS.len() {
                (*v).made[G_BR_ZONE_FNS[f].word as usize] = ocerz_bridge_native_thunk(
                    G_BR_ZONE_FNS[f].f,
                    G_BR_ZONE_FNS[f].name,
                    G_BR_ZONE_FNS[f].notation,
                );
                ok = (*v).made[G_BR_ZONE_FNS[f].word as usize] != 0;
                f += 1;
            }
            let mut introspect = [0u64; BR_ZONE_INTROSPECT_WORDS];
            let mut f = 0;
            while ok && f < G_BR_INTROSPECT_FNS.len() {
                introspect[G_BR_INTROSPECT_FNS[f].word as usize] = ocerz_bridge_native_thunk(
                    G_BR_INTROSPECT_FNS[f].f,
                    G_BR_INTROSPECT_FNS[f].name,
                    G_BR_INTROSPECT_FNS[f].notation,
                );
                ok = introspect[G_BR_INTROSPECT_FNS[f].word as usize] != 0;
                f += 1;
            }
            if ok {
                let version = if (*z).version < BR_ZONE_VERSION_CAP {
                    (*z).version
                } else {
                    BR_ZONE_VERSION_CAP
                };
                (*v).made[BR_ZONE_INTROSPECT_WORD] = page + BR_ZONE_INTROSPECT_AT;
                for w in 0..BR_ZONE_INTROSPECT_WORDS {
                    ocerz_st(
                        page + BR_ZONE_INTROSPECT_AT + 8 * w as u64,
                        8,
                        introspect[w],
                    );
                }
                (*v).made[9] = if (*z).zone_name.is_null() {
                    0
                } else {
                    ocerz_h2g((*z).zone_name as *const c_void)
                };
                (*v).made[BR_ZONE_VERSION_WORD] = version as u64;
                for w in 0..BR_ZONE_WORDS {
                    ocerz_st(page + 8 * w as u64, 8, (*v).made[w]);
                }
                (*v).view = page;
                (*v).native = z;
                G_BR_VIEWS_N.store(n as u32 + 1, Ordering::SeqCst);
                answer = page;
            } else if page != 0 {
                ocerz_unmap(page, OCERZ_GUEST_PAGE_SIZE as u64);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_VIEWS_LOCK);
        answer
    }
}

const BR_ZONE_SLOTS: usize = 64;

static mut G_BR_EFF: [u64; BR_ZONE_SLOTS] = [0; BR_ZONE_SLOTS];
static mut G_BR_EFF_N: c_int = 0;
static mut G_BR_EFF_SEEDED: c_int = 0;
static mut G_BR_ZONES_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn br_zone_seed_locked() {
    unsafe {
        if G_BR_EFF_SEEDED != 0 {
            return;
        }
        G_BR_EFF_SEEDED = 1;
        let f = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"malloc_get_all_zones".as_ptr(),
        );
        if f.is_null() {
            return;
        }
        let mut addrs: u64 = 0;
        let mut count: u32 = 0;
        let f: unsafe extern "C" fn(u32, *mut c_void, u64, u64) -> c_int = core::mem::transmute(f);
        let kr = f(
            libc::mach_task_self(),
            null_mut(),
            &mut addrs as *mut u64 as u64,
            &mut count as *mut u32 as u64,
        );
        if kr != 0 {
            return;
        }
        let list = if addrs != 0 {
            addrs as *const u64
        } else {
            core::ptr::null()
        };
        let mut k = 0;
        while k < count && (G_BR_EFF_N as usize) < BR_ZONE_SLOTS {
            let view = br_zone_view(*list.add(k as usize) as *mut c_void);
            if view != 0 {
                G_BR_EFF[G_BR_EFF_N as usize] = view;
                G_BR_EFF_N += 1;
            }
            k += 1;
        }
    }
}

unsafe fn br_zone_add(zone: u64) {
    unsafe {
        if zone == 0 {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        if (G_BR_EFF_N as usize) < BR_ZONE_SLOTS {
            G_BR_EFF[G_BR_EFF_N as usize] = zone;
            G_BR_EFF_N += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
    }
}

unsafe fn br_zone_remove(zone: u64) {
    unsafe {
        if zone == 0 {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        for k in 0..G_BR_EFF_N as usize {
            if G_BR_EFF[k] == zone {
                G_BR_EFF_N -= 1;
                G_BR_EFF[k] = G_BR_EFF[G_BR_EFF_N as usize];
                break;
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
    }
}

pub unsafe extern "C" fn br_malloc_zone_register_tracked(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        crate::ocerz_log!(
            "bridge: malloc zone register takes zone %#llx, tracked for guest queries\n",
            zone
        );
        br_zone_add(zone);
        br_answer(vm, cpu, 0)
    }
}

pub unsafe extern "C" fn br_malloc_zone_unregister_tracked(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        crate::ocerz_log!("bridge: malloc zone unregister takes zone %#llx\n", zone);
        br_zone_remove(zone);
        br_answer(vm, cpu, 0)
    }
}

static G_BR_DEFAULT_ZONE_FN: core::sync::atomic::AtomicPtr<c_void> =
    core::sync::atomic::AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_malloc_default_zone_tracked(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let mut first = 0;
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        if G_BR_EFF_N > 0 {
            first = G_BR_EFF[0];
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
        if first != 0 {
            return br_answer(vm, cpu, first);
        }
        let mut f = G_BR_DEFAULT_ZONE_FN.load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(
                OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
                c"malloc_default_zone".as_ptr(),
            );
            if f.is_null() {
                return br_answer(vm, cpu, 0);
            }
            G_BR_DEFAULT_ZONE_FN.store(f, Ordering::SeqCst);
        }
        let f: unsafe extern "C" fn() -> *mut c_void = core::mem::transmute(f);
        let r = f();
        br_answer(vm, cpu, br_zone_view(r))
    }
}

pub unsafe extern "C" fn br_malloc_default_purgeable_zone(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let f = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"malloc_default_purgeable_zone".as_ptr(),
        );
        if f.is_null() {
            return br_answer(vm, cpu, 0);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"_malloc_default_purgeable_zone".as_ptr(),
            c"p()".as_ptr(),
            f,
        );
        let f: unsafe extern "C" fn() -> *mut c_void = core::mem::transmute(f);
        let r = f();
        ocerz_bridge_lower(&outer);
        let view = br_zone_view(r);
        let mut known = false;
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        for k in 0..G_BR_EFF_N as usize {
            known |= G_BR_EFF[k] == view;
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
        if !known {
            br_zone_add(view);
        }
        br_answer(vm, cpu, view)
    }
}

static G_BR_GET_ALL_ZONES_FN: core::sync::atomic::AtomicPtr<c_void> =
    core::sync::atomic::AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_malloc_get_all_zones_tracked(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let reader = gpr(cpu, OCERZ_RSI);
        let mut native_reader: *mut c_void = null_mut();
        if reader != 0 {
            native_reader = ocerz_abi_callback_intern(reader, c"i(uLLp)".as_ptr());
            if native_reader.is_null() {
                return br_answer(vm, cpu, libc::KERN_FAILURE as u32 as u64);
            }
        }
        let mut f = G_BR_GET_ALL_ZONES_FN.load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(
                OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
                c"malloc_get_all_zones".as_ptr(),
            );
            if f.is_null() {
                return br_answer(vm, cpu, libc::KERN_FAILURE as u32 as u64);
            }
            G_BR_GET_ALL_ZONES_FN.store(f, Ordering::SeqCst);
        }
        let task = gpr(cpu, OCERZ_RDI);
        let addresses = gpr(cpu, OCERZ_RDX);
        let countp = gpr(cpu, OCERZ_RCX);
        let f: unsafe extern "C" fn(u32, *mut c_void, u64, u64) -> c_int = core::mem::transmute(f);
        let kr = f(task as u32, native_reader, addresses, countp);
        if kr != 0 || addresses == 0 || countp == 0 {
            return br_answer(vm, cpu, kr as u32 as u64);
        }
        let mut eff = [0u64; BR_ZONE_SLOTS];
        let mut neff = 0;
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        let mut k = 0;
        while k < G_BR_EFF_N as usize && neff < BR_ZONE_SLOTS {
            eff[neff] = G_BR_EFF[k];
            neff += 1;
            k += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
        if neff == 0 {
            return br_answer(vm, cpu, kr as u32 as u64);
        }
        let out = libc::malloc(neff * 8) as *mut u64;
        if out.is_null() {
            return br_answer(vm, cpu, kr as u32 as u64);
        }
        for k in 0..neff {
            *out.add(k) = eff[k];
        }
        ocerz_st(addresses, 8, out as u64);
        ocerz_st(countp, 4, neff as u64);
        br_answer(vm, cpu, kr as u32 as u64)
    }
}

const BR_ZONE_SIZE: u64 = 16;
const BR_ZONE_MALLOC: u64 = 24;
const BR_ZONE_CALLOC: u64 = 32;
const BR_ZONE_VALLOC: u64 = 40;
const BR_ZONE_FREE: u64 = 48;
const BR_ZONE_REALLOC: u64 = 56;
const BR_ZONE_DESTROY: u64 = 64;
const BR_ZONE_NAME: u64 = 72;
const BR_ZONE_MEMALIGN: u64 = 112;
const BR_ZONE_PRESSURE_RELIEF: u64 = 128;

unsafe fn br_zone_is_guest(zone: u64) -> c_int {
    unsafe {
        if zone == 0 {
            return 0;
        }
        let f = ocerz_ld(zone + BR_ZONE_MALLOC, 8);
        (f != 0 && ocerz_abi_is_guest_code(f) != 0) as c_int
    }
}

unsafe fn br_zone_forward(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    export_name: *const core::ffi::c_char,
    sig: *const core::ffi::c_char,
    slot: u64,
) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        let view = br_view_find(zone);
        let target = if zone != 0 {
            ocerz_ld(zone + slot, 8)
        } else {
            0
        };
        if if !view.is_null() {
            target != (*view).made[(slot / 8) as usize]
        } else {
            br_zone_is_guest(zone) != 0
        } {
            if target == 0 {
                return br_answer(vm, cpu, 0);
            }
            (*cpu).rip = target;
            return OCERZ_STEP_OK as c_int;
        }
        let f = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            export_name.add(1),
        );
        if f.is_null() {
            return br_answer(vm, cpu, 0);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            export_name,
            sig,
            f,
        );
        let f: unsafe extern "C" fn(u64, u64, u64, u64) -> u64 = core::mem::transmute(f);
        let r = f(
            if view.is_null() {
                zone
            } else {
                (*view).native as u64
            },
            gpr(cpu, OCERZ_RSI),
            gpr(cpu, OCERZ_RDX),
            gpr(cpu, OCERZ_RCX),
        );
        ocerz_bridge_lower(&outer);
        br_answer(vm, cpu, r)
    }
}

macro_rules! zf_handler {
    ($name:ident, $sym:literal, $sig:literal, $slot:expr) => {
        pub unsafe extern "C" fn $name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
            unsafe {
                br_zone_forward(
                    vm,
                    cpu,
                    concat!($sym, "\0").as_ptr() as *const core::ffi::c_char,
                    concat!($sig, "\0").as_ptr() as *const core::ffi::c_char,
                    $slot,
                )
            }
        }
    };
}

zf_handler!(
    br_malloc_zone_malloc,
    "_malloc_zone_malloc",
    "p(pL)",
    BR_ZONE_MALLOC
);
zf_handler!(
    br_malloc_zone_calloc,
    "_malloc_zone_calloc",
    "p(pLL)",
    BR_ZONE_CALLOC
);
zf_handler!(
    br_malloc_zone_valloc,
    "_malloc_zone_valloc",
    "p(pL)",
    BR_ZONE_VALLOC
);
zf_handler!(
    br_malloc_zone_free,
    "_malloc_zone_free",
    "v(pp)",
    BR_ZONE_FREE
);
zf_handler!(
    br_malloc_zone_realloc,
    "_malloc_zone_realloc",
    "p(ppL)",
    BR_ZONE_REALLOC
);
zf_handler!(
    br_malloc_zone_memalign,
    "_malloc_zone_memalign",
    "p(pLL)",
    BR_ZONE_MEMALIGN
);

zf_handler!(
    br_malloc_type_zone_malloc,
    "_malloc_type_zone_malloc",
    "p(pLL)",
    BR_ZONE_MALLOC
);
zf_handler!(
    br_malloc_type_zone_calloc,
    "_malloc_type_zone_calloc",
    "p(pLLL)",
    BR_ZONE_CALLOC
);
zf_handler!(
    br_malloc_type_zone_valloc,
    "_malloc_type_zone_valloc",
    "p(pLL)",
    BR_ZONE_VALLOC
);
zf_handler!(
    br_malloc_type_zone_free,
    "_malloc_type_zone_free",
    "v(ppL)",
    BR_ZONE_FREE
);
zf_handler!(
    br_malloc_type_zone_realloc,
    "_malloc_type_zone_realloc",
    "p(ppLL)",
    BR_ZONE_REALLOC
);
zf_handler!(
    br_malloc_type_zone_memalign,
    "_malloc_type_zone_memalign",
    "p(pLLL)",
    BR_ZONE_MEMALIGN
);
zf_handler!(
    br_malloc_zone_pressure_relief,
    "_malloc_zone_pressure_relief",
    "L(pL)",
    BR_ZONE_PRESSURE_RELIEF
);

/// malloc_zone_statistics fills a malloc_statistics_t, a count and three sizes
/// laid out alike on both sides: a view answers its host zone's numbers, a null
/// zone every host zone's, and a zone of the guest's own nothing ocerz counts.
pub unsafe extern "C" fn br_malloc_zone_statistics(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        let out = gpr(cpu, OCERZ_RSI);
        let view = br_view_find(zone);
        let mut st: libc::malloc_statistics_t = core::mem::zeroed();
        if zone == 0 || !view.is_null() {
            let mut outer: OcerzBridgeFrame = core::mem::zeroed();
            ocerz_bridge_raise(
                &mut outer,
                OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
                c"_malloc_zone_statistics".as_ptr(),
                c"v(pp)".as_ptr(),
                libc::malloc_zone_statistics as *const c_void,
            );
            libc::malloc_zone_statistics(
                if view.is_null() {
                    null_mut()
                } else {
                    (*view).native as *mut libc::malloc_zone_t
                },
                &mut st,
            );
            ocerz_bridge_lower(&outer);
        }
        if out != 0 {
            ocerz_st(out, 8, st.blocks_in_use as u64);
            ocerz_st(out + 8, 8, st.size_in_use as u64);
            ocerz_st(out + 16, 8, st.max_size_in_use as u64);
            ocerz_st(out + 24, 8, st.size_allocated as u64);
        }
        br_answer(vm, cpu, 0)
    }
}

pub unsafe extern "C" fn br_malloc_destroy_zone(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_zone_remove(gpr(cpu, OCERZ_RDI));
        br_zone_forward(
            vm,
            cpu,
            c"_malloc_destroy_zone".as_ptr(),
            c"v(p)".as_ptr(),
            BR_ZONE_DESTROY,
        )
    }
}

pub unsafe extern "C" fn br_malloc_create_zone(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let f = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"malloc_create_zone".as_ptr(),
        );
        if f.is_null() {
            return br_answer(vm, cpu, 0);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"_malloc_create_zone".as_ptr(),
            c"p(Lu)".as_ptr(),
            f,
        );
        let f: unsafe extern "C" fn(u64, c_uint) -> *mut c_void = core::mem::transmute(f);
        let zone = f(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI) as c_uint);
        ocerz_bridge_lower(&outer);
        let g = br_zone_view(zone);
        br_zone_add(g);
        br_answer(vm, cpu, g)
    }
}

pub unsafe extern "C" fn br_malloc_zone_from_ptr(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let ptr = gpr(cpu, OCERZ_RDI);
        let f = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
            c"malloc_zone_from_ptr".as_ptr(),
        );
        let mut zone: *mut c_void = null_mut();
        if !f.is_null() {
            let mut outer: OcerzBridgeFrame = core::mem::zeroed();
            ocerz_bridge_raise(
                &mut outer,
                OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const core::ffi::c_char,
                c"_malloc_zone_from_ptr".as_ptr(),
                c"p(p)".as_ptr(),
                f,
            );
            let f: unsafe extern "C" fn(*const c_void) -> *mut c_void = core::mem::transmute(f);
            zone = f(if ptr != 0 {
                ocerz_g2h(ptr)
            } else {
                core::ptr::null_mut()
            });
            ocerz_bridge_lower(&outer);
        }
        if !zone.is_null() {
            return br_answer(vm, cpu, br_zone_view(zone));
        }
        let mut eff = [0u64; BR_ZONE_SLOTS];
        let mut neff = 0;
        libc::pthread_mutex_lock(&raw mut G_BR_ZONES_LOCK);
        br_zone_seed_locked();
        for k in 0..G_BR_EFF_N as usize {
            eff[neff] = G_BR_EFF[k];
            neff += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_ZONES_LOCK);
        for k in 0..neff {
            if !br_view_find(eff[k]).is_null() || br_zone_is_guest(eff[k]) == 0 {
                continue;
            }
            let size_fn = ocerz_ld(eff[k] + BR_ZONE_SIZE, 8);
            if size_fn == 0 {
                continue;
            }
            let args = [eff[k], ptr];
            if ocerz_vm_call(
                vm,
                size_fn,
                args.as_ptr(),
                2,
                (gpr(cpu, OCERZ_RSP) - 256) & !0xf,
            ) != 0
            {
                return br_answer(vm, cpu, eff[k]);
            }
        }
        br_answer(vm, cpu, 0)
    }
}

pub unsafe extern "C" fn br_malloc_get_zone_name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        br_answer(
            vm,
            cpu,
            if zone != 0 {
                ocerz_ld(zone + BR_ZONE_NAME, 8)
            } else {
                0
            },
        )
    }
}

pub unsafe extern "C" fn br_malloc_set_zone_name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let zone = gpr(cpu, OCERZ_RDI);
        let name = gpr(cpu, OCERZ_RSI);
        let view = br_view_find(zone);
        if !view.is_null() || br_zone_is_guest(zone) != 0 {
            let copy = if name != 0 {
                libc::strdup(ocerz_g2h(name) as *const core::ffi::c_char)
            } else {
                null_mut()
            };
            ocerz_st(
                zone + BR_ZONE_NAME,
                8,
                if copy.is_null() {
                    0
                } else {
                    ocerz_h2g(copy as *const c_void)
                },
            );
            if !view.is_null() {
                malloc_set_zone_name((*view).native as *mut c_void, copy);
            }
            return br_answer(vm, cpu, 0);
        }
        malloc_set_zone_name(
            ocerz_g2h(zone),
            if name != 0 {
                ocerz_g2h(name) as *const core::ffi::c_char
            } else {
                core::ptr::null()
            },
        );
        br_answer(vm, cpu, 0)
    }
}
