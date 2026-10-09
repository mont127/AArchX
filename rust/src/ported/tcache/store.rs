//! Fingerprinting, store lifecycle, index creation, and mapped data-file lookup.

use super::*;
use crate::ffi;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{MaybeUninit, size_of, zeroed};
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering};

const LC_UUID: u32 = 0x1b;

unsafe fn dir_ptr() -> *mut c_char {
    ptr::addr_of_mut!(G_DIR).cast::<c_char>()
}

pub(super) unsafe fn mode() -> c_int {
    let cached = G_MODE.load(Ordering::Relaxed);
    if cached >= 0 {
        return cached;
    }
    let e = libc::getenv(c"OCERZ_TCACHE".as_ptr());
    let mut m = ffi::OCERZ_TC_ON as c_int;
    if !e.is_null()
        && (libc::strcmp(e, c"0".as_ptr()) == 0 || libc::strcmp(e, c"off".as_ptr()) == 0)
    {
        m = ffi::OCERZ_TC_OFF as c_int;
    } else if !e.is_null() && libc::strcmp(e, c"verify".as_ptr()) == 0 {
        m = ffi::OCERZ_TC_VERIFY as c_int;
    } else if !e.is_null() && libc::strcmp(e, c"roundtrip".as_ptr()) == 0 {
        m = ffi::OCERZ_TC_ROUNDTRIP as c_int;
    }
    G_MODE.store(m, Ordering::Relaxed);
    m
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tcache_mode() -> c_int {
    mode()
}

pub(super) unsafe fn fnv(mut h: u64, p: *const u8, n: usize) -> u64 {
    let mut i = 0;
    while i < n {
        h ^= *p.add(i) as u64;
        h = h.wrapping_mul(0x100000001b3);
        i += 1;
    }
    h
}

pub(super) fn mix(mut h: u64, w: u64) -> u64 {
    h = (h ^ w).wrapping_mul(0x9e3779b97f4a7c15);
    h ^ (h >> 29)
}

unsafe fn env_ignored(kv: *const c_char) -> bool {
    let skip = [
        c"OCERZ_TCACHE=",
        c"OCERZ_TCACHE_LOG=",
        c"OCERZ_TCACHE_DIR=",
        c"OCERZ_TCACHE_TRACE=",
        c"OCERZ_TCACHE_MAX_MB=",
        c"OCERZ_TCACHE_MIN_FREE_MB=",
        c"OCERZ_GUESTPROF=",
        c"OCERZ_GUESTPROF_PERIOD=",
        c"OCERZ_LOWBASE=",
        c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES=",
        c"OCERZ_PRELOAD_OBJC=",
    ];
    for prefix in skip {
        if libc::strncmp(kv, prefix.as_ptr(), prefix.to_bytes().len()) == 0 {
            return true;
        }
    }
    false
}

unsafe extern "C" fn cmp_str(a: *const c_void, b: *const c_void) -> c_int {
    let x = *a.cast::<*const c_char>();
    let y = *b.cast::<*const c_char>();
    libc::strcmp(x, y)
}

unsafe fn fingerprint() -> u64 {
    let mut h = fnv(0xcbf29ce484222325, c"ocerz-tc-2".as_ptr().cast(), 10);
    let mh = _dyld_get_image_header(0);
    if !mh.is_null() {
        let mut lc = mh.cast::<u8>().add(size_of::<MachHeader64>());
        let mut i = 0;
        while i < (*mh).ncmds {
            let cmd = lc.cast::<LoadCommand>();
            if (*cmd).cmd == LC_UUID {
                let uuid = lc.cast::<UuidCommand>();
                h = fnv(h, ptr::addr_of!((*uuid).uuid).cast::<u8>(), 16);
            }
            lc = lc.add((*cmd).cmdsize as usize);
            i += 1;
        }
    }

    let v = [
        ffi::ocerz_mode as u64,
        ffi::ocerz_low_base,
        ffi::ocerz_top_base,
        ffi::ocerz_guest_base,
    ];
    h = fnv(h, v.as_ptr().cast::<u8>(), size_of::<[u64; 4]>());

    let mut model = [0 as c_char; 128];
    let mut ml = (model.len() - 1) as libc::size_t;
    if libc::sysctlbyname(
        c"hw.model".as_ptr(),
        model.as_mut_ptr().cast::<c_void>(),
        &mut ml,
        ptr::null_mut(),
        0,
    ) == 0
    {
        h = fnv(
            h,
            model.as_ptr().cast::<u8>(),
            libc::strnlen(model.as_ptr(), model.len()),
        );
    }

    let mut n = 0usize;
    let mut e = ptr::addr_of_mut!(environ).read();
    while !e.is_null() && !(*e).is_null() {
        let kv = *e;
        if libc::strncmp(kv, c"OCERZ_".as_ptr(), 6) == 0 && !env_ignored(kv) {
            n += 1;
        }
        e = e.add(1);
    }
    if n != 0 {
        let kv =
            libc::malloc(n.wrapping_mul(size_of::<*const c_char>()).max(1)).cast::<*const c_char>();
        if !kv.is_null() {
            let mut k = 0usize;
            let mut e = ptr::addr_of_mut!(environ).read();
            while !e.is_null() && !(*e).is_null() && k < n {
                let item = *e;
                if libc::strncmp(item, c"OCERZ_".as_ptr(), 6) == 0 && !env_ignored(item) {
                    kv.add(k).write(item);
                    k += 1;
                }
                e = e.add(1);
            }
            libc::qsort(
                kv.cast_mut().cast::<c_void>(),
                k,
                size_of::<*const c_char>(),
                Some(cmp_str),
            );
            let mut i = 0usize;
            while i < k {
                let item = *kv.add(i);
                h = fnv(h, item.cast::<u8>(), libc::strlen(item) + 1);
                i += 1;
            }
            libc::free(kv.cast_mut().cast::<c_void>());
        }
    }
    h
}

unsafe fn mkdirs(path: *const c_char) -> c_int {
    let mut tmp = [0 as c_char; 1024];
    libc::snprintf(tmp.as_mut_ptr(), tmp.len(), c"%s".as_ptr(), path);
    let mut p = tmp.as_mut_ptr().add(1);
    while *p != 0 {
        if *p == b'/' as c_char {
            p.write(0);
            libc::mkdir(tmp.as_ptr(), 0o755);
            p.write(b'/' as c_char);
        }
        p = p.add(1);
    }
    if libc::mkdir(tmp.as_ptr(), 0o755) == 0 || *libc::__error() == libc::EEXIST {
        0
    } else {
        -1
    }
}

pub(super) unsafe fn free_bytes() -> u64 {
    let mut s = core::mem::MaybeUninit::<libc::statfs>::uninit();
    if libc::statfs(dir_ptr(), s.as_mut_ptr()) == 0 {
        let s = s.assume_init();
        (s.f_bavail as u64).wrapping_mul(s.f_bsize as u64)
    } else {
        u64::MAX
    }
}

unsafe fn remove_dir(dir: *const c_char) {
    let d = libc::opendir(dir);
    if d.is_null() {
        return;
    }
    loop {
        let de = libc::readdir(d);
        if de.is_null() {
            break;
        }
        let name = ptr::addr_of!((*de).d_name).cast::<c_char>();
        if *name == b'.' as c_char {
            continue;
        }
        let mut path = [0 as c_char; 1400];
        libc::snprintf(path.as_mut_ptr(), path.len(), c"%s/%s".as_ptr(), dir, name);
        libc::unlink(path.as_ptr());
    }
    libc::closedir(d);
    libc::rmdir(dir);
}

unsafe fn dir_bytes(dir: *const c_char) -> u64 {
    let mut total = 0u64;
    let d = libc::opendir(dir);
    if d.is_null() {
        return 0;
    }
    loop {
        let de = libc::readdir(d);
        if de.is_null() {
            break;
        }
        let name = ptr::addr_of!((*de).d_name).cast::<c_char>();
        if *name == b'.' as c_char {
            continue;
        }
        let mut path = [0 as c_char; 1400];
        let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
        libc::snprintf(path.as_mut_ptr(), path.len(), c"%s/%s".as_ptr(), dir, name);
        if libc::stat(path.as_ptr(), st.as_mut_ptr()) == 0 {
            let st = st.assume_init();
            total = total.wrapping_add((st.st_blocks as u64).wrapping_mul(512));
        }
    }
    libc::closedir(d);
    total
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PruneEnt {
    path: [c_char; 1200],
    mt: libc::time_t,
    bytes: u64,
}

extern "C" fn prune_other_dirs_entry(arg: *mut c_void) -> *mut c_void {
    unsafe { prune_other_dirs(arg) }
}

unsafe extern "C" fn prune_other_dirs(_arg: *mut c_void) -> *mut c_void {
    let mut parent = [0 as c_char; 1024];
    libc::snprintf(parent.as_mut_ptr(), parent.len(), c"%s".as_ptr(), dir_ptr());
    let slash = libc::strrchr(parent.as_mut_ptr(), b'/' as c_int);
    if slash.is_null() || libc::strncmp(slash.add(1), c"tc-".as_ptr(), 3) != 0 {
        return ptr::null_mut();
    }
    slash.write(0);
    let d = libc::opendir(parent.as_ptr());
    if d.is_null() {
        return ptr::null_mut();
    }

    let mut keep: [PruneEnt; 64] = zeroed();
    let mut nk = 0usize;
    let mut total = 0u64;
    let now = libc::time(ptr::null_mut());
    loop {
        let de = libc::readdir(d);
        if de.is_null() {
            break;
        }
        let name = ptr::addr_of!((*de).d_name).cast::<c_char>();
        if libc::strncmp(name, c"tc-".as_ptr(), 3) != 0 || libc::strlen(name) != 19 {
            continue;
        }
        let mut path = [0 as c_char; 1200];
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s/%s".as_ptr(),
            parent.as_ptr(),
            name,
        );
        if libc::strcmp(path.as_ptr(), dir_ptr()) == 0 {
            continue;
        }
        let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
        if libc::stat(path.as_ptr(), st.as_mut_ptr()) != 0 {
            continue;
        }
        let st = st.assume_init();
        if (st.st_mode & libc::S_IFMT) != libc::S_IFDIR {
            continue;
        }
        let age = now.wrapping_sub(st.st_mtime);
        if age < 3600 {
            continue;
        }
        if age > 86400 || nk == 64 {
            remove_dir(path.as_ptr());
            continue;
        }
        libc::snprintf(
            keep[nk].path.as_mut_ptr(),
            keep[nk].path.len(),
            c"%s".as_ptr(),
            path.as_ptr(),
        );
        keep[nk].mt = st.st_mtime;
        keep[nk].bytes = dir_bytes(path.as_ptr());
        total = total.wrapping_add(keep[nk].bytes);
        nk += 1;
    }
    libc::closedir(d);

    let budget = if free_bytes() < G_FLOOR_BYTES {
        0
    } else {
        2u64 << 30
    };
    while nk > 0 && total > budget {
        let mut oldest = 0usize;
        let mut i = 1usize;
        while i < nk {
            if keep[i].mt < keep[oldest].mt {
                oldest = i;
            }
            i += 1;
        }
        remove_dir(keep[oldest].path.as_ptr());
        total = total.wrapping_sub(keep[oldest].bytes);
        keep[oldest] = keep[nk - 1];
        nk -= 1;
    }
    ptr::null_mut()
}

unsafe fn open_index() -> bool {
    let mut path = [0 as c_char; 1200];
    let mut tmp = [0 as c_char; 1200];
    libc::snprintf(
        path.as_mut_ptr(),
        path.len(),
        c"%s/index".as_ptr(),
        dir_ptr(),
    );
    let len = size_of::<TcIdxHdr>() + TC_SLOTS as usize * size_of::<TcSlot>();
    let mut fd = libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC);
    if fd < 0 {
        libc::snprintf(
            tmp.as_mut_ptr(),
            tmp.len(),
            c"%s/index.%d".as_ptr(),
            dir_ptr(),
            libc::getpid(),
        );
        let t = libc::open(
            tmp.as_ptr(),
            libc::O_RDWR | libc::O_CREAT | libc::O_TRUNC | libc::O_CLOEXEC,
            0o644 as c_int,
        );
        if t < 0 {
            return false;
        }
        let h = TcIdxHdr {
            magic: TC_IDX_MAGIC,
            version: 1,
            fp: G_FP,
            slots: TC_SLOTS,
            next_file: 1,
            bytes: 0,
            pad: [0; 3],
        };
        if libc::ftruncate(t, len as libc::off_t) != 0
            || libc::pwrite(
                t,
                (&h as *const TcIdxHdr).cast::<c_void>(),
                size_of::<TcIdxHdr>(),
                0,
            ) != size_of::<TcIdxHdr>() as libc::ssize_t
        {
            libc::close(t);
            libc::unlink(tmp.as_ptr());
            return false;
        }
        if libc::link(tmp.as_ptr(), path.as_ptr()) == 0 {
            fd = t;
        } else {
            libc::close(t);
            fd = libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC);
        }
        libc::unlink(tmp.as_ptr());
        if fd < 0 {
            return false;
        }
    }

    let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
    if libc::fstat(fd, st.as_mut_ptr()) != 0 || (st.assume_init().st_size as usize) != len {
        libc::close(fd);
        return false;
    }
    let p = libc::mmap(
        ptr::null_mut(),
        len,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_SHARED,
        fd,
        0,
    );
    libc::close(fd);
    if p == libc::MAP_FAILED {
        return false;
    }
    let h = p.cast::<TcIdxHdr>();
    if (*h).magic != TC_IDX_MAGIC || (*h).version != 1 || (*h).fp != G_FP || (*h).slots != TC_SLOTS
    {
        libc::munmap(p, len);
        return false;
    }
    G_HDR = h;
    G_SLOT = h.add(1).cast::<TcSlot>();
    G_MASK = TC_SLOTS - 1;
    true
}

unsafe fn reset_store() {
    let d = libc::opendir(dir_ptr());
    if d.is_null() {
        return;
    }
    loop {
        let de = libc::readdir(d);
        if de.is_null() {
            break;
        }
        let name = ptr::addr_of!((*de).d_name).cast::<c_char>();
        if libc::strncmp(name, c"d-".as_ptr(), 2) != 0 && libc::strcmp(name, c"index".as_ptr()) != 0
        {
            continue;
        }
        let mut path = [0 as c_char; 1400];
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s/%s".as_ptr(),
            dir_ptr(),
            name,
        );
        libc::unlink(path.as_ptr());
    }
    libc::closedir(d);
}

unsafe fn claim_store() -> bool {
    let mut path = [0 as c_char; 1200];
    libc::snprintf(
        path.as_mut_ptr(),
        path.len(),
        c"%s/users".as_ptr(),
        dir_ptr(),
    );
    G_UFD = libc::open(
        path.as_ptr(),
        libc::O_RDWR | libc::O_CREAT | libc::O_CLOEXEC,
        0o644 as c_int,
    );
    if G_UFD < 0 {
        return false;
    }
    if libc::flock(G_UFD, libc::LOCK_EX | libc::LOCK_NB) == 0 {
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s/index".as_ptr(),
            dir_ptr(),
        );
        let fd = libc::open(path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
        if fd >= 0 {
            let mut h = MaybeUninit::<TcIdxHdr>::uninit();
            if libc::pread(
                fd,
                h.as_mut_ptr().cast::<c_void>(),
                size_of::<TcIdxHdr>(),
                0,
            ) == size_of::<TcIdxHdr>() as libc::ssize_t
            {
                let h = h.assume_init();
                if h.pad[0] != 0 || h.bytes > G_CAP_BYTES {
                    if G_LOG > 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: TCACHE[%d] %s is full; starting it again\n".as_ptr(),
                            libc::getpid(),
                            dir_ptr(),
                        );
                    }
                    reset_store();
                } else if free_bytes() < G_FLOOR_BYTES {
                    if G_LOG > 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: TCACHE[%d] %s: the disk is under the floor; removing the store\n"
                                .as_ptr(),
                            libc::getpid(),
                            dir_ptr(),
                        );
                    }
                    reset_store();
                }
            }
            libc::close(fd);
        }
    }
    libc::flock(G_UFD, libc::LOCK_SH) == 0
}

pub(super) unsafe fn open_store() -> bool {
    if G_OPENED != 0 {
        return !G_HDR.is_null();
    }
    G_OPENED = 1;
    if G_LOG < 0 {
        G_LOG = (!libc::getenv(c"OCERZ_TCACHE_LOG".as_ptr()).is_null()) as c_int;
    }
    let mx = libc::getenv(c"OCERZ_TCACHE_MAX_MB".as_ptr());
    let mx_value = if !mx.is_null() && libc::atoi(mx) > 0 {
        libc::atoi(mx) as u64
    } else {
        4096
    };
    G_CAP_BYTES = mx_value.wrapping_shl(20);
    let mf = libc::getenv(c"OCERZ_TCACHE_MIN_FREE_MB".as_ptr());
    let floor = if !mf.is_null() && *mf != 0 {
        libc::strtoull(mf, ptr::null_mut(), 10) as u64
    } else {
        10240
    };
    G_FLOOR_BYTES = floor.wrapping_shl(20);
    G_FP = fingerprint();

    let dir = libc::getenv(c"OCERZ_TCACHE_DIR".as_ptr());
    let home = libc::getenv(c"HOME".as_ptr());
    if !dir.is_null() && *dir != 0 {
        libc::snprintf(
            dir_ptr(),
            1024,
            c"%s/tc-%016llx".as_ptr(),
            dir,
            G_FP as libc::c_ulonglong,
        );
    } else if !home.is_null() && *home != 0 {
        libc::snprintf(
            dir_ptr(),
            1024,
            c"%s/Library/Caches/ocerz/tc-%016llx".as_ptr(),
            home,
            G_FP as libc::c_ulonglong,
        );
    } else {
        return false;
    }
    if mkdirs(dir_ptr()) != 0 || !claim_store() || !open_index() {
        return false;
    }
    libc::utimes(dir_ptr(), ptr::null());
    let fr = free_bytes();
    let bytes_ptr = ptr::addr_of_mut!((*G_HDR).bytes);
    let bytes = AtomicU64::from_ptr(bytes_ptr).load(Ordering::Relaxed);
    G_ROOM_END = bytes.wrapping_add(if fr > G_FLOOR_BYTES {
        (fr - G_FLOOR_BYTES) / 2
    } else {
        0
    });
    if fr < G_FLOOR_BYTES {
        G_FULL.store(1, Ordering::Relaxed);
        if G_LOG > 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TCACHE[%d] %s: %llu MB free, under the %llu MB floor; not writing\n"
                    .as_ptr(),
                libc::getpid(),
                dir_ptr(),
                (fr >> 20) as libc::c_ulonglong,
                (G_FLOOR_BYTES >> 20) as libc::c_ulonglong,
            );
        }
    }
    let ds = compression_decode_scratch_buffer_size(COMPRESSION_LZ4_RAW);
    G_RBUF = libc::malloc(TC_REC_MAX).cast::<u8>();
    G_DSCRATCH = libc::malloc(if ds != 0 { ds } else { 1 }).cast::<u8>();
    if G_RBUF.is_null() || G_DSCRATCH.is_null() {
        return false;
    }
    if G_LOG > 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: TCACHE[%d] store %s mode=%d low=%#llx top=%#llx base=%#llx\n".as_ptr(),
            libc::getpid(),
            dir_ptr(),
            ffi::ocerz_mode,
            ffi::ocerz_low_base as libc::c_ulonglong,
            ffi::ocerz_top_base as libc::c_ulonglong,
            ffi::ocerz_guest_base as libc::c_ulonglong,
        );
    }
    let mut t = zeroed::<libc::pthread_t>();
    let mut at = zeroed::<libc::pthread_attr_t>();
    libc::pthread_attr_init(&mut at);
    libc::pthread_attr_setdetachstate(&mut at, libc::PTHREAD_CREATE_DETACHED);
    libc::pthread_create(&mut t, &at, prune_other_dirs_entry, ptr::null_mut());
    libc::pthread_attr_destroy(&mut at);
    true
}

pub(super) unsafe fn file_at(no: u64, off: u64, need: u64) -> *const u8 {
    if no == 0 || no >= (1 << 24) {
        return ptr::null();
    }
    if no >= G_NFILE {
        let mut n = if G_NFILE != 0 { G_NFILE } else { 64 };
        while n <= no {
            n = n.wrapping_mul(2);
        }
        let nv = libc::realloc(
            G_FILE.cast::<c_void>(),
            n.wrapping_mul(size_of::<TcFile>() as u64) as usize,
        )
        .cast::<TcFile>();
        if nv.is_null() {
            return ptr::null();
        }
        let mut i = G_NFILE;
        while i < n {
            nv.add(i as usize).write(TcFile {
                p: ptr::null(),
                len: 0,
                fd: -1,
            });
            i += 1;
        }
        G_FILE = nv;
        G_NFILE = n;
    }
    let f = G_FILE.add(no as usize);
    if off.wrapping_add(need) <= (*f).len as u64 {
        return (*f).p.add(off as usize);
    }
    if (*f).fd < 0 {
        let mut path = [0 as c_char; 1200];
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s/d-%llu.td".as_ptr(),
            dir_ptr(),
            no as libc::c_ulonglong,
        );
        (*f).fd = libc::open(path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
        if (*f).fd < 0 {
            return ptr::null();
        }
    }
    let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
    if libc::fstat((*f).fd, st.as_mut_ptr()) != 0 {
        return ptr::null();
    }
    let st = st.assume_init();
    if (st.st_size as u64) < off.wrapping_add(need) {
        return ptr::null();
    }
    let len = st.st_size as usize;
    let p = libc::mmap(
        ptr::null_mut(),
        len,
        libc::PROT_READ,
        libc::MAP_SHARED,
        (*f).fd,
        0,
    );
    if p == libc::MAP_FAILED {
        return ptr::null();
    }
    if !(*f).p.is_null() {
        libc::munmap((*f).p.cast_mut().cast::<c_void>(), (*f).len);
    }
    (*f).p = p.cast::<u8>();
    (*f).len = len;
    (*f).p.add(off as usize)
}
