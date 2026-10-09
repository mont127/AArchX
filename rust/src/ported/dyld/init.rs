//! Dependency-ordered initializer and image-load phases.

use super::*;

const INIT_VISITED_MAX: usize = 8192;
pub(super) const INIT_CLOSURE_CAP: usize = 4096;
const DYLIB_USE_MARKER: u32 = 0x1a74_1800;
const DYLIB_USE_UPWARD: u32 = 0x04;
const DYLIB_USE_SIZE: u32 = 28;

static mut G_INIT_VISITED: [u64; INIT_VISITED_MAX] = [0; INIT_VISITED_MAX];
static mut G_INIT_GEN: [u32; INIT_VISITED_MAX] = [0; INIT_VISITED_MAX];
static mut G_INIT_DONE: [u8; INIT_VISITED_MAX] = [0; INIT_VISITED_MAX];
static mut G_LOAD_DONE: [u8; INIT_VISITED_MAX] = [0; INIT_VISITED_MAX];
pub(super) static mut G_INIT_BEING: [u8; INIT_VISITED_MAX] = [0; INIT_VISITED_MAX];
static mut G_INIT_VISITED_N: c_int = 0;
pub(super) static mut G_INIT_CUR_GEN: u32 = 0;
pub(super) static mut G_INIT_FORCE: c_int = 0;
static mut G_INIT_COLLECT_DEPTH: c_int = 0;
pub(super) static mut G_LIBSYS_MH: u64 = 0;
pub(super) static mut g_init_dlopen_restricted: c_int = 0;
pub(super) static mut G_FOUNDATION_INITED: c_int = 0;

unsafe fn image_id_name(mh: u64) -> *const c_char {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return ptr::null();
    }
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_ID_DYLIB {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                return lc.add(noff as usize).cast();
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    ptr::null()
}

unsafe fn image_is_objc_core(mh: u64) -> bool {
    let id = image_id_name(mh);
    !id.is_null()
        && (!libc::strstr(id, cstr_ptr(c"/Foundation.framework/")).is_null()
            || !libc::strstr(id, cstr_ptr(c"/CoreFoundation.framework/")).is_null()
            || !libc::strstr(id, cstr_ptr(c"/libobjc.A.dylib")).is_null())
}

unsafe fn image_is_foundation(mh: u64) -> bool {
    let id = image_id_name(mh);
    !id.is_null() && !libc::strstr(id, cstr_ptr(c"/Foundation.framework/")).is_null()
}

pub(super) unsafe fn run_image_inits(vm: *mut OcerzVM, mh: u64, ia: *const u64, stack_top: u64) {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    let slide = super::eager::image_slide_d(mh);
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let iscan = libc::getenv(cstr_ptr(c"OCERZ_INITSCAN"));
    let do_scan = !iscan.is_null()
        && (iscan.read() == b'*' as c_char || libc::strtoull(iscan, ptr::null_mut(), 0) == mh);
    if do_scan {
        for _ in 0..ncmds {
            let cmd = rd32(lc);
            if cmd == LC_SEGMENT_64 {
                let ns = rd32(lc.add(64));
                let mut sec = lc.add(72);
                for _ in 0..ns {
                    let flags = rd32(sec.add(64));
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"INITSCAN mh=%#llx seg=%.16s sect=%.16s type=%#x addr=%#llx sz=%#llx\n"),
                        mh as c_ulonglong,
                        sec.add(16).cast::<c_char>(),
                        sec.cast::<c_char>(),
                        flags & 0xff,
                        rd64(sec.add(32)) as c_ulonglong,
                        rd64(sec.add(40)) as c_ulonglong,
                    );
                    sec = sec.add(80);
                }
            } else {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"INITSCAN mh=%#llx LC cmd=%#x\n"),
                    mh as c_ulonglong,
                    cmd,
                );
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
        lc = h.add(core::mem::size_of::<MachHeader64>());
    }
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 {
            let ns = rd32(lc.add(64));
            let mut sec = lc.add(72);
            for _ in 0..ns {
                let ty = (rd32(sec.add(64)) & 0xff) as u8;
                let sa = (rd64(sec.add(32)) as i64).wrapping_add(slide) as u64;
                let size = rd64(sec.add(40));
                if ty == 0x16 {
                    let mut off = 0u64;
                    while off.wrapping_add(4) <= size {
                        let fnaddr =
                            mh.wrapping_add(rd32(sa.wrapping_add(off) as *const u8) as u64);
                        if !libc::getenv(cstr_ptr(c"OCERZ_INITLOG")).is_null() {
                            libc::fprintf(
                                crate::log::stderr(),
                                cstr_ptr(c"INIT mh=%#llx fn=%#llx\n"),
                                mh as c_ulonglong,
                                fnaddr as c_ulonglong,
                            );
                        }
                        ffi::ocerz_vm_call(vm, fnaddr, ia, 5, stack_top);
                        if (*vm).exited != 0 {
                            return;
                        }
                        off = off.wrapping_add(4);
                    }
                } else if ty == 0x09 {
                    let mut off = 0u64;
                    while off.wrapping_add(8) <= size {
                        let fnaddr = rd64(sa.wrapping_add(off) as *const u8);
                        if fnaddr != 0 {
                            if !libc::getenv(cstr_ptr(c"OCERZ_INITLOG")).is_null() {
                                libc::fprintf(
                                    crate::log::stderr(),
                                    cstr_ptr(c"INIT mh=%#llx fn=%#llx\n"),
                                    mh as c_ulonglong,
                                    fnaddr as c_ulonglong,
                                );
                            }
                            ffi::ocerz_vm_call(vm, fnaddr, ia, 5, stack_top);
                        }
                        if (*vm).exited != 0 {
                            return;
                        }
                        off = off.wrapping_add(8);
                    }
                }
                sec = sec.add(80);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if image_is_foundation(mh) {
        if G_FOUNDATION_INITED == 0 && !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: FOUNDATION-INITED mh=%#llx\n"),
                mh as c_ulonglong,
            );
        }
        G_FOUNDATION_INITED = 1;
    }
}

pub(super) unsafe fn init_mark(mh: u64) -> c_int {
    let visited = ptr::addr_of_mut!(G_INIT_VISITED).cast::<u64>();
    for i in 0..G_INIT_VISITED_N {
        if visited.add(i as usize).read() == mh {
            return i;
        }
    }
    if G_INIT_VISITED_N >= INIT_VISITED_MAX as c_int {
        return -1;
    }
    let i = G_INIT_VISITED_N;
    G_INIT_VISITED_N += 1;
    visited.add(i as usize).write(mh);
    ptr::addr_of_mut!(G_INIT_GEN)
        .cast::<u32>()
        .add(i as usize)
        .write(0);
    ptr::addr_of_mut!(G_INIT_DONE)
        .cast::<u8>()
        .add(i as usize)
        .write(0);
    ptr::addr_of_mut!(G_INIT_BEING)
        .cast::<u8>()
        .add(i as usize)
        .write(0);
    i
}

pub(super) unsafe fn init_is_done(mh: u64) -> bool {
    let visited = ptr::addr_of!(G_INIT_VISITED).cast::<u64>();
    let done = ptr::addr_of!(G_INIT_DONE).cast::<u8>();
    for i in 0..G_INIT_VISITED_N {
        if visited.add(i as usize).read() == mh {
            return done.add(i as usize).read() != 0;
        }
    }
    false
}

unsafe fn is_umbrella_path(path: *const c_char) -> bool {
    !path.is_null() && libc::strncmp(path, cstr_ptr(c"/usr/lib/system/"), 16) == 0
}

pub(super) unsafe fn init_mark_done_closure(cache: *mut OcerzCache, mh: u64) {
    if mh == 0 {
        return;
    }
    let idx = init_mark(mh);
    if idx < 0
        || ptr::addr_of!(G_INIT_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .read()
            != 0
        || ptr::addr_of!(G_INIT_BEING)
            .cast::<u8>()
            .add(idx as usize)
            .read()
            != 0
    {
        return;
    }
    ptr::addr_of_mut!(G_INIT_BEING)
        .cast::<u8>()
        .add(idx as usize)
        .write(1);
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) == MH_MAGIC_64 {
        let ncmds = rd32(h.add(16));
        let mut lc = h.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            let cmd = rd32(lc);
            if cmd == 0xc || cmd == 0x8000_0018 || cmd == 0x8000_001f {
                let noff = rd32(lc.add(8));
                let dpath = lc.add(noff as usize).cast::<c_char>();
                if noff < rd32(lc.add(4)) && is_umbrella_path(dpath) {
                    init_mark_done_closure(cache, super::eager::dep_mh(cache, dpath));
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
    }
    ptr::addr_of_mut!(G_INIT_BEING)
        .cast::<u8>()
        .add(idx as usize)
        .write(0);
    ptr::addr_of_mut!(G_INIT_DONE)
        .cast::<u8>()
        .add(idx as usize)
        .write(1);
}

unsafe fn upward_init_enabled() -> bool {
    static mut ON: c_int = -1;
    if ON < 0 {
        ON = libc::getenv(cstr_ptr(c"OCERZ_NO_UPWARD_INIT")).is_null() as c_int;
    }
    ON != 0
}

unsafe fn dlopen_upward_enabled() -> bool {
    static mut ON: c_int = -1;
    if ON < 0 {
        ON = (upward_init_enabled() && libc::getenv(cstr_ptr(c"OCERZ_NO_DLOPEN_UPWARD")).is_null())
            as c_int;
    }
    ON != 0
}

pub(super) unsafe fn dylib_lc_is_init_dep(lc: *const u8) -> bool {
    let cmd = rd32(lc);
    if cmd != 0xc && cmd != 0x8000_0018 && cmd != 0x8000_001f {
        return false;
    }
    if cmd != 0x8000_001f
        && rd32(lc.add(4)) >= DYLIB_USE_SIZE
        && rd32(lc.add(8)) == DYLIB_USE_SIZE
        && rd32(lc.add(12)) == DYLIB_USE_MARKER
    {
        return rd32(lc.add(24)) & DYLIB_USE_UPWARD == 0;
    }
    true
}

unsafe fn dylib_lc_is_upward_dep(lc: *const u8) -> bool {
    let cmd = rd32(lc);
    if cmd == 0x8000_0023 {
        return true;
    }
    if (cmd == 0xc || cmd == 0x8000_0018)
        && rd32(lc.add(4)) >= DYLIB_USE_SIZE
        && rd32(lc.add(8)) == DYLIB_USE_SIZE
        && rd32(lc.add(12)) == DYLIB_USE_MARKER
    {
        return rd32(lc.add(24)) & DYLIB_USE_UPWARD != 0;
    }
    false
}

pub(super) unsafe fn init_collect(
    cache: *mut OcerzCache,
    mh: u64,
    list: *mut u64,
    n: *mut c_int,
    cap: c_int,
) {
    if mh == 0 {
        return;
    }
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return;
    }
    let idx = init_mark(mh);
    if idx < 0
        || ptr::addr_of!(G_INIT_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .read()
            != 0
        || ptr::addr_of!(G_INIT_BEING)
            .cast::<u8>()
            .add(idx as usize)
            .read()
            != 0
    {
        return;
    }
    ptr::addr_of_mut!(G_INIT_BEING)
        .cast::<u8>()
        .add(idx as usize)
        .write(1);
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if dylib_lc_is_init_dep(lc) {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                let path = lc.add(noff as usize).cast::<c_char>();
                init_collect(cache, super::eager::dep_mh(cache, path), list, n, cap);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if *n < cap {
        list.add(*n as usize).write(mh);
        *n += 1;
    }
    if !dlopen_upward_enabled() {
        return;
    }
    lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if dylib_lc_is_upward_dep(lc) {
            let noff = rd32(lc.add(8));
            let umh = if noff < rd32(lc.add(4)) {
                super::eager::dep_mh(cache, lc.add(noff as usize).cast())
            } else {
                0
            };
            if umh != 0 && image_is_objc_core(umh) {
                init_collect(cache, umh, list, n, cap);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

pub(super) unsafe fn init_closure(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    mh: u64,
    ia: *const u64,
    stack_top: u64,
) {
    if (*vm).exited != 0 || mh == 0 {
        return;
    }
    static mut LIST: [u64; INIT_CLOSURE_CAP] = [0; INIT_CLOSURE_CAP];
    let reentrant = G_INIT_COLLECT_DEPTH > 0;
    let mut list = if reentrant {
        libc::malloc(core::mem::size_of::<u64>() * INIT_CLOSURE_CAP).cast::<u64>()
    } else {
        ptr::addr_of_mut!(LIST).cast::<u64>()
    };
    if list.is_null() {
        return;
    }
    G_INIT_COLLECT_DEPTH += 1;
    let mut n = 0;
    init_collect(cache, mh, list, &mut n, INIT_CLOSURE_CAP as c_int);
    for i in 0..n {
        let idx = init_mark(list.add(i as usize).read());
        if idx >= 0 {
            ptr::addr_of_mut!(G_INIT_BEING)
                .cast::<u8>()
                .add(idx as usize)
                .write(0);
        }
    }
    for i in 0..n {
        if (*vm).exited != 0 {
            break;
        }
        let current = list.add(i as usize).read();
        let idx = init_mark(current);
        if idx < 0
            || ptr::addr_of!(G_INIT_DONE)
                .cast::<u8>()
                .add(idx as usize)
                .read()
                != 0
        {
            continue;
        }
        if g_init_dlopen_restricted != 0 && !image_is_objc_core(current) {
            if !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"INITCLOSURE skip-restricted mh=%#llx\n"),
                    current as c_ulonglong,
                );
            }
            continue;
        }
        if !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
            let id = image_id_name(current);
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"INITCLOSURE[%d] run mh=%#llx %s\n"),
                libc::getpid(),
                current as c_ulonglong,
                if id.is_null() { cstr_ptr(c"?") } else { id },
            );
        }
        let prev_tol = ffi::ocerz_init_tolerant;
        ffi::ocerz_init_tolerant = 1;
        let load_done = ptr::addr_of!(G_LOAD_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .read();
        if load_done == 0 && libc::getenv(cstr_ptr(c"OCERZ_NO_CLOSURE_LOADS")).is_null() {
            ffi::ocerz_dyldapi_run_image_loads(vm, current, stack_top);
            ptr::addr_of_mut!(G_LOAD_DONE)
                .cast::<u8>()
                .add(idx as usize)
                .write(1);
            if (*vm).exited != 0 {
                ffi::ocerz_init_tolerant = prev_tol;
                break;
            }
        }
        run_image_inits(vm, current, ia, stack_top);
        ffi::ocerz_init_tolerant = prev_tol;
        ptr::addr_of_mut!(G_INIT_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .write(1);
    }
    G_INIT_COLLECT_DEPTH -= 1;
    if reentrant {
        libc::free(list.cast());
    }
}

#[repr(C)]
struct InitUpward {
    mh: *mut u64,
    n: c_int,
    cap: c_int,
}

unsafe fn init_upward_add(up: *mut InitUpward, mh: u64) {
    if up.is_null() || mh == 0 {
        return;
    }
    for i in 0..(*up).n {
        if (*up).mh.add(i as usize).read() == mh {
            return;
        }
    }
    if (*up).n == (*up).cap {
        let cap = if (*up).cap != 0 { (*up).cap * 2 } else { 64 };
        let p = libc::realloc(
            (*up).mh.cast(),
            core::mem::size_of::<u64>().wrapping_mul(cap as usize),
        )
        .cast::<u64>();
        if p.is_null() {
            return;
        }
        (*up).mh = p;
        (*up).cap = cap;
    }
    (*up).mh.add((*up).n as usize).write(mh);
    (*up).n += 1;
}

unsafe fn run_init_phase(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    mh: u64,
    ia: *const u64,
    stack_top: u64,
    skip_mh: u64,
    up: *mut InitUpward,
) {
    if (*vm).exited != 0 || mh == 0 {
        return;
    }
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return;
    }
    let idx = init_mark(mh);
    if idx >= 0 {
        let done = ptr::addr_of!(G_INIT_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .read();
        let generation = ptr::addr_of!(G_INIT_GEN)
            .cast::<u32>()
            .add(idx as usize)
            .read();
        if done != 0 || generation == G_INIT_CUR_GEN {
            if !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"INITTRACE skip mh=%#llx done=%d gen=%u cur=%u\n"),
                    mh as c_ulonglong,
                    done as c_int,
                    generation,
                    G_INIT_CUR_GEN,
                );
            }
            return;
        }
        ptr::addr_of_mut!(G_INIT_GEN)
            .cast::<u32>()
            .add(idx as usize)
            .write(G_INIT_CUR_GEN);
    }
    if !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"INITTRACE enter mh=%#llx force=%d\n"),
            mh as c_ulonglong,
            G_INIT_FORCE,
        );
    }
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let cfdump = libc::getenv(cstr_ptr(c"OCERZ_CFDUMP"));
    if !cfdump.is_null() && mh == libc::strtoull(cfdump, ptr::null_mut(), 0) {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"CFDUMP mh=%#llx ncmds=%u (h=%p)\n"),
            mh as c_ulonglong,
            ncmds,
            h.cast::<c_void>(),
        );
        let mut p = lc;
        for j in 0..ncmds {
            let cmd = rd32(p);
            let size = rd32(p.add(4));
            if cmd == 0xc || cmd == 0x8000_001f || cmd == 0x8000_0018 || cmd == 0x8000_0022 {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"  CFDUMP [%u] cmd=%#x sz=%u name=%s\n"),
                    j,
                    cmd,
                    size,
                    p.add(rd32(p.add(8)) as usize).cast::<c_char>(),
                );
            }
            p = p.add(size as usize);
        }
    }
    for _ in 0..ncmds {
        if dylib_lc_is_init_dep(lc) {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                let path = lc.add(noff as usize).cast::<c_char>();
                let dmh = super::eager::dep_mh(cache, path);
                if !libc::getenv(cstr_ptr(c"OCERZ_INITEDGE")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"INITEDGE %#llx -> %#llx \"%s\"\n"),
                        mh as c_ulonglong,
                        dmh as c_ulonglong,
                        path,
                    );
                }
                if dmh == 0 && !libc::getenv(cstr_ptr(c"OCERZ_INITTRACE")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"INITTRACE dep-unresolved mh=%#llx dep=\"%s\"\n"),
                        mh as c_ulonglong,
                        path,
                    );
                }
                run_init_phase(vm, cache, dmh, ia, stack_top, skip_mh, up);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if (*vm).exited != 0 {
        return;
    }
    let eager_n = super::eager::G_EAGER_N;
    if mh != skip_mh && (G_INIT_FORCE != 0 || eager_n == 0 || super::eager::eager_has(mh)) {
        let load_done = idx >= 0
            && ptr::addr_of!(G_LOAD_DONE)
                .cast::<u8>()
                .add(idx as usize)
                .read()
                != 0;
        if idx < 0 || !load_done {
            ffi::ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
            if idx >= 0 {
                ptr::addr_of_mut!(G_LOAD_DONE)
                    .cast::<u8>()
                    .add(idx as usize)
                    .write(1);
            }
            if (*vm).exited != 0 {
                return;
            }
        }
        run_image_inits(vm, mh, ia, stack_top);
        if idx >= 0 {
            ptr::addr_of_mut!(G_INIT_DONE)
                .cast::<u8>()
                .add(idx as usize)
                .write(1);
        }
    } else if !libc::getenv(cstr_ptr(c"OCERZ_INITLOG")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"INITSKIP mh=%#llx eager=%d is_libsystem=%d\n"),
            mh as c_ulonglong,
            super::eager::eager_has(mh) as c_int,
            (mh == skip_mh) as c_int,
        );
    }
    if !upward_init_enabled() || (*vm).exited != 0 {
        return;
    }
    lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if dylib_lc_is_upward_dep(lc) {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                let path = lc.add(noff as usize).cast::<c_char>();
                let umh = super::eager::dep_mh(cache, path);
                if !libc::getenv(cstr_ptr(c"OCERZ_INITEDGE")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"INITEDGE-UPWARD %#llx -> %#llx \"%s\"\n"),
                        mh as c_ulonglong,
                        umh as c_ulonglong,
                        path,
                    );
                }
                init_upward_add(up, umh);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

pub(super) unsafe fn run_init_root(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    mh: u64,
    ia: *const u64,
    stack_top: u64,
    skip_mh: u64,
) {
    let mut up = InitUpward {
        mh: ptr::null_mut(),
        n: 0,
        cap: 0,
    };
    run_init_phase(vm, cache, mh, ia, stack_top, skip_mh, &mut up);
    let mut i = 0;
    while i < up.n && (*vm).exited == 0 {
        run_init_phase(
            vm,
            cache,
            up.mh.add(i as usize).read(),
            ia,
            stack_top,
            skip_mh,
            &mut up,
        );
        i += 1;
    }
    libc::free(up.mh.cast());
}

pub(super) unsafe fn run_load_phase(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    mh: u64,
    stack_top: u64,
    skip_mh: u64,
) {
    if (*vm).exited != 0 || mh == 0 {
        return;
    }
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return;
    }
    let idx = init_mark(mh);
    if idx >= 0 {
        let done = ptr::addr_of!(G_LOAD_DONE)
            .cast::<u8>()
            .add(idx as usize)
            .read();
        let generation = ptr::addr_of!(G_INIT_GEN)
            .cast::<u32>()
            .add(idx as usize)
            .read();
        if done != 0 || generation == G_INIT_CUR_GEN {
            return;
        }
        ptr::addr_of_mut!(G_INIT_GEN)
            .cast::<u32>()
            .add(idx as usize)
            .write(G_INIT_CUR_GEN);
    }
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if dylib_lc_is_init_dep(lc) {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                let dep = super::eager::dep_mh(cache, lc.add(noff as usize).cast());
                run_load_phase(vm, cache, dep, stack_top, skip_mh);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if (*vm).exited != 0 {
        return;
    }
    if mh != skip_mh
        && (G_INIT_FORCE != 0 || super::eager::G_EAGER_N == 0 || super::eager::eager_has(mh))
    {
        ffi::ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
        if idx >= 0 {
            ptr::addr_of_mut!(G_LOAD_DONE)
                .cast::<u8>()
                .add(idx as usize)
                .write(1);
        }
    }
    if !dlopen_upward_enabled() || G_INIT_FORCE == 0 || (*vm).exited != 0 {
        return;
    }
    lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if dylib_lc_is_upward_dep(lc) {
            let noff = rd32(lc.add(8));
            let umh = if noff < rd32(lc.add(4)) {
                super::eager::dep_mh(cache, lc.add(noff as usize).cast())
            } else {
                0
            };
            if umh != 0 && image_is_objc_core(umh) {
                run_load_phase(vm, cache, umh, stack_top, skip_mh);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}
