//! Process creation and replacement, including guest argv translation,
//! child wrapping, and launchd plist compatibility adjustments.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int};
use core::ptr;

const PSFA_STRIDE: u64 = 1040;
const SPAWN_FLAGS_PUBLIC: c_int = 0x44cf;
const GUEST_ARGV_MAX: usize = 256;
const GUEST_ENV_MAX: usize = 512;
const PATH_MAX_VALUE: usize = 1024;
const OCERZ_CPU_TYPE_X86_64: u32 = 0x01000007;
const OCERZ_CPU_TYPE_ARM64: u32 = 0x0100000c;

const PSFA_OPEN: c_int = 0;
const PSFA_CLOSE: c_int = 1;
const PSFA_DUP2: c_int = 2;
const PSFA_INHERIT: c_int = 3;
const PSFA_FILEPORT_DUP2: c_int = 4;
const PSFA_CHDIR: c_int = 5;
const PSFA_FCHDIR: c_int = 6;

unsafe extern "C" {
    fn _NSGetExecutablePath(buf: *mut c_char, size: *mut u32) -> c_int;
    fn posix_spawn_file_actions_addinherit_np(
        fa: *mut libc::posix_spawn_file_actions_t,
        fd: c_int,
    ) -> c_int;
    static mut environ: *mut *mut c_char;
}

unsafe fn ocerz_self_path() -> *const c_char {
    static mut BUF: [c_char; 1024] = [0; 1024];
    unsafe {
        let buf = ptr::addr_of_mut!(BUF).cast::<c_char>();
        if *buf == 0 {
            let mut sz = 1024u32;
            if _NSGetExecutablePath(buf, &mut sz) != 0 {
                *buf = 0;
            }
        }
        if *buf != 0 { buf } else { ptr::null() }
    }
}

unsafe fn env_inject_lowbase(henv: *mut *mut c_char, mut m: c_int, cap: c_int) -> c_int {
    static mut LOWBASE_KV: [c_char; 40] = [0; 40];
    static mut TOPBASE_KV: [c_char; 40] = [0; 40];
    static MODE_KV: &[u8] = b"OCERZ_MODE=native\0";
    unsafe {
        let mut have_low = false;
        let mut have_top = false;
        let mut have_nano = false;
        let mut have_mode = false;
        for i in 0..m {
            let s = *henv.offset(i as isize);
            have_low |= libc::strncmp(s, c"OCERZ_LOWBASE=".as_ptr(), 14) == 0;
            have_top |= libc::strncmp(s, c"OCERZ_TOPBASE=".as_ptr(), 14) == 0;
            have_nano |= libc::strncmp(s, c"MallocNanoZone=".as_ptr(), 15) == 0;
            have_mode |= libc::strncmp(s, c"OCERZ_MODE=".as_ptr(), 11) == 0;
        }
        if !have_nano && m < cap {
            *henv.offset(m as isize) = c"MallocNanoZone=0".as_ptr().cast_mut();
            m += 1;
        }
        if crate::ffi::ocerz_mode == (crate::ffi::OCERZ_MODE_NATIVE as c_int)
            && !have_mode
            && m < cap
        {
            *henv.offset(m as isize) = MODE_KV.as_ptr().cast_mut().cast();
            m += 1;
        }
        if crate::ffi::ocerz_low_base == 0 {
            return m;
        }
        if !have_low && m < cap {
            libc::snprintf(
                ptr::addr_of_mut!(LOWBASE_KV).cast(),
                40,
                c"OCERZ_LOWBASE=%#llx".as_ptr(),
                crate::ffi::ocerz_low_base as libc::c_ulonglong,
            );
            *henv.offset(m as isize) = ptr::addr_of_mut!(LOWBASE_KV).cast();
            m += 1;
        }
        if !have_top && m < cap {
            libc::snprintf(
                ptr::addr_of_mut!(TOPBASE_KV).cast(),
                40,
                c"OCERZ_TOPBASE=%#llx".as_ptr(),
                crate::ffi::ocerz_top_base as libc::c_ulonglong,
            );
            *henv.offset(m as isize) = ptr::addr_of_mut!(TOPBASE_KV).cast();
            m += 1;
        }
        m
    }
}

unsafe fn spawn_guest_path(g: u64) -> *const c_char {
    unsafe {
        let p = ocerz_g2h(g).cast::<c_char>();
        if libc::memchr(p.cast(), 0, libc::PATH_MAX as usize).is_null() {
            ptr::null()
        } else {
            p
        }
    }
}

unsafe fn spawn_attr_sanitized(
    at: *mut libc::posix_spawnattr_t,
    flags: i16,
    def: *const libc::sigset_t,
    mut mask: libc::sigset_t,
    pgroup: libc::pid_t,
) {
    unsafe {
        let keep = [
            libc::SIGSEGV,
            libc::SIGBUS,
            libc::SIGILL,
            libc::SIGTRAP,
            libc::SIGFPE,
            libc::SIGUSR1,
            libc::SIGEMT,
        ];
        for sig in keep {
            libc::sigdelset(&mut mask, sig);
        }
        libc::posix_spawnattr_init(at);
        libc::posix_spawnattr_setflags(at, flags & SPAWN_FLAGS_PUBLIC as i16);
        libc::posix_spawnattr_setsigdefault(at, def);
        libc::posix_spawnattr_setsigmask(at, &mask);
        libc::posix_spawnattr_setpgroup(at, pgroup);
    }
}

#[allow(unused_assignments)]
unsafe fn spawn_guest_args(
    adesc: u64,
    at: *mut libc::posix_spawnattr_t,
    have_at: *mut c_int,
    fa: *mut libc::posix_spawn_file_actions_t,
    have_fa: *mut c_int,
) -> c_int {
    unsafe {
        *have_at = 0;
        *have_fa = 0;
        if adesc == 0 {
            return 0;
        }
        let attr_size = ocerz_ld(adesc, 8);
        let attrp = ocerz_ld(adesc.wrapping_add(8), 8);
        let fa_size = ocerz_ld(adesc.wrapping_add(16), 8);
        let fap = ocerz_ld(adesc.wrapping_add(24), 8);
        if attrp != 0 && attr_size >= 16 {
            let def = ocerz_ld(attrp.wrapping_add(4), 4) as libc::sigset_t;
            let mask = ocerz_ld(attrp.wrapping_add(8), 4) as libc::sigset_t;
            spawn_attr_sanitized(
                at,
                ocerz_ld(attrp, 2) as i16,
                &def,
                mask,
                ocerz_ld(attrp.wrapping_add(12), 4) as libc::pid_t,
            );
            *have_at = 1;
        }
        if fap == 0 || fa_size < 8 {
            return 0;
        }
        let count = ocerz_ld(fap.wrapping_add(4), 4) as u32;
        if 8u64.wrapping_add((count as u64).wrapping_mul(PSFA_STRIDE)) > fa_size {
            return libc::EINVAL;
        }
        libc::posix_spawn_file_actions_init(fa);
        *have_fa = 1;
        for i in 0..count {
            let act = fap
                .wrapping_add(8)
                .wrapping_add((i as u64).wrapping_mul(PSFA_STRIDE));
            let ty = ocerz_ld(act, 4) as u32 as c_int;
            let fd = ocerz_ld(act.wrapping_add(4), 4) as u32 as c_int;
            let arg = ocerz_ld(act.wrapping_add(8), 4) as u32 as c_int;
            let mut path = ptr::null();
            let rc = match ty {
                PSFA_OPEN => {
                    path = spawn_guest_path(act.wrapping_add(14));
                    if path.is_null() {
                        libc::EINVAL
                    } else {
                        libc::posix_spawn_file_actions_addopen(
                            fa,
                            fd,
                            path,
                            arg,
                            ocerz_ld(act.wrapping_add(12), 2) as u32 as libc::mode_t,
                        )
                    }
                }
                PSFA_CLOSE => libc::posix_spawn_file_actions_addclose(fa, fd),
                PSFA_DUP2 => libc::posix_spawn_file_actions_adddup2(fa, fd, arg),
                PSFA_INHERIT => posix_spawn_file_actions_addinherit_np(fa, fd),
                PSFA_FILEPORT_DUP2 => {
                    type AddFileport = unsafe extern "C" fn(
                        *mut libc::posix_spawn_file_actions_t,
                        libc::mach_port_t,
                        c_int,
                    ) -> c_int;
                    static mut ADD_FILEPORT: Option<AddFileport> = None;
                    let mut fun = ADD_FILEPORT;
                    if fun.is_none() {
                        fun = Some(core::mem::transmute(libc::dlsym(
                            libc::RTLD_DEFAULT,
                            c"posix_spawn_file_actions_add_fileportdup2_np".as_ptr(),
                        )));
                        ADD_FILEPORT = fun;
                    }
                    fun.map_or(libc::ENOTSUP, |f| f(fa, fd as _, arg))
                }
                PSFA_CHDIR | PSFA_FCHDIR => {
                    type AddChdir = unsafe extern "C" fn(
                        *mut libc::posix_spawn_file_actions_t,
                        *const c_char,
                    ) -> c_int;
                    type AddFchdir =
                        unsafe extern "C" fn(*mut libc::posix_spawn_file_actions_t, c_int) -> c_int;
                    static mut ADD_CHDIR: Option<AddChdir> = None;
                    static mut ADD_FCHDIR: Option<AddFchdir> = None;
                    let mut add_chdir = ptr::read(ptr::addr_of!(ADD_CHDIR));
                    let mut add_fchdir = ptr::read(ptr::addr_of!(ADD_FCHDIR));
                    if add_chdir.is_none() {
                        add_chdir = Some(core::mem::transmute(libc::dlsym(
                            libc::RTLD_DEFAULT,
                            c"posix_spawn_file_actions_addchdir_np".as_ptr(),
                        )));
                        add_fchdir = Some(core::mem::transmute(libc::dlsym(
                            libc::RTLD_DEFAULT,
                            c"posix_spawn_file_actions_addfchdir_np".as_ptr(),
                        )));
                        ptr::write(ptr::addr_of_mut!(ADD_CHDIR), add_chdir);
                        ptr::write(ptr::addr_of_mut!(ADD_FCHDIR), add_fchdir);
                    }
                    if ty == PSFA_FCHDIR {
                        add_fchdir.map_or(libc::ENOTSUP, |f| f(fa, fd))
                    } else {
                        path = spawn_guest_path(act.wrapping_add(8));
                        if path.is_null() {
                            libc::EINVAL
                        } else {
                            add_chdir.map_or(libc::ENOTSUP, |f| f(fa, path))
                        }
                    }
                }
                _ => {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: posix_spawn: skipping unknown file action %d\n".as_ptr(),
                        ty,
                    );
                    0
                }
            };
            if rc != 0 {
                return rc;
            }
        }
        0
    }
}

unsafe fn file_has_slice(path: *const c_char, cputype: u32) -> c_int {
    unsafe {
        let fd = libc::open(path, libc::O_RDONLY);
        if fd < 0 {
            return 0;
        }
        let mut h = [0u8; 4096];
        let n = libc::read(fd, h.as_mut_ptr().cast(), h.len());
        libc::close(fd);
        if n < 8 {
            return 0;
        }
        let m = u32::from_be_bytes(h[0..4].try_into().unwrap());
        if m == 0xcafebabe || m == 0xcafebabf {
            let nf = u32::from_be_bytes(h[4..8].try_into().unwrap());
            let is64 = m == 0xcafebabf;
            let esz = if is64 { 32usize } else { 20usize };
            let mut off = 8usize;
            for _ in 0..nf {
                if off + esz > n as usize {
                    break;
                }
                let ct = u32::from_be_bytes(h[off..off + 4].try_into().unwrap());
                if ct == cputype {
                    return 1;
                }
                off += esz;
            }
            return 0;
        }
        if m == 0xcffaedfe && u32::from_le_bytes(h[4..8].try_into().unwrap()) == cputype {
            return 1;
        }
        0
    }
}

unsafe fn file_has_x86_slice(path: *const c_char) -> c_int {
    unsafe { file_has_slice(path, OCERZ_CPU_TYPE_X86_64) }
}

unsafe fn file_links_xcselect(path: *const c_char) -> c_int {
    unsafe {
        let fd = libc::open(path, libc::O_RDONLY | libc::O_CLOEXEC);
        if fd < 0 {
            return 0;
        }
        let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
        let mut found = 0;
        if libc::fstat(fd, st.as_mut_ptr()) == 0 {
            let st = st.assume_init();
            if st.st_size > 0 && st.st_size <= 8 << 20 {
                let map = libc::mmap(
                    ptr::null_mut(),
                    st.st_size as usize,
                    libc::PROT_READ,
                    libc::MAP_PRIVATE,
                    fd,
                    0,
                );
                if map != libc::MAP_FAILED {
                    found = c_int::from(
                        !libc::memmem(
                            map,
                            st.st_size as usize,
                            c"/usr/lib/libxcselect.dylib".as_ptr().cast(),
                            c"/usr/lib/libxcselect.dylib".to_bytes().len(),
                        )
                        .is_null(),
                    );
                    libc::munmap(map, st.st_size as usize);
                }
            }
        }
        libc::close(fd);
        found
    }
}

unsafe fn guest_child_runs_native(path: *const c_char) -> c_int {
    static NATIVE_ROOTS: [&[u8]; 5] = [
        b"/usr/\0",
        b"/bin/\0",
        b"/sbin/\0",
        b"/System/\0",
        b"/Library/Apple/\0",
    ];
    static LAUNCHERS: [&[u8]; 19] = [
        b"sh\0",
        b"bash\0",
        b"zsh\0",
        b"dash\0",
        b"ksh\0",
        b"csh\0",
        b"tcsh\0",
        b"env\0",
        b"xargs\0",
        b"nohup\0",
        b"nice\0",
        b"time\0",
        b"script\0",
        b"sudo\0",
        b"arch\0",
        b"caffeinate\0",
        b"perl\0",
        b"python3\0",
        b"ruby\0",
    ];
    static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
        if off < 0 {
            off = c_int::from(!libc::getenv(c"OCERZ_NO_NATIVE_CHILDREN".as_ptr()).is_null());
            OFF.store(off, core::sync::atomic::Ordering::Relaxed);
        }
        if off != 0 || path.is_null() {
            return 0;
        }
        let mut real = [0 as c_char; PATH_MAX_VALUE];
        if libc::realpath(path, real.as_mut_ptr()).is_null() {
            return 0;
        }
        let x86 = file_has_x86_slice(real.as_ptr()) != 0;
        let arm = file_has_slice(real.as_ptr(), OCERZ_CPU_TYPE_ARM64) != 0;
        if !x86 && arm {
            return 1;
        }
        if libc::strncmp(real.as_ptr(), c"/usr/local/".as_ptr(), 11) == 0 {
            return 0;
        }
        let mut system_path = false;
        for root in NATIVE_ROOTS {
            if libc::strncmp(real.as_ptr(), root.as_ptr().cast(), root.len() - 1) == 0 {
                system_path = true;
                break;
            }
        }
        if !system_path {
            return 0;
        }
        if arm && file_links_xcselect(real.as_ptr()) != 0 {
            return 1;
        }
        if crate::ffi::ocerz_mode != (crate::ffi::OCERZ_MODE_NATIVE as c_int) {
            return 0;
        }
        let leaf = libc::strrchr(real.as_ptr(), b'/' as c_int);
        let leaf = if leaf.is_null() {
            real.as_ptr()
        } else {
            leaf.add(1)
        };
        for launcher in LAUNCHERS {
            if libc::strcmp(leaf, launcher.as_ptr().cast()) == 0 {
                return 0;
            }
        }
        c_int::from(arm)
    }
}

unsafe fn extract_plist_path(cmd: *const c_char, out: *mut c_char, outsz: usize) -> c_int {
    unsafe {
        let dot = libc::strstr(cmd, c".plist".as_ptr());
        if dot.is_null() {
            return 0;
        }
        let end = dot.add(6).cast_const();
        let mut q = ptr::null();
        let mut c = dot.cast_const();
        while c > cmd {
            if *c.offset(-1) == b'\'' as c_char || *c.offset(-1) == b'"' as c_char {
                q = c;
                break;
            }
            c = c.offset(-1);
        }
        if q.is_null() {
            return 0;
        }
        let len = end.offset_from(q) as usize;
        if len == 0 || len >= outsz {
            return 0;
        }
        ptr::copy_nonoverlapping(q, out, len);
        *out.add(len) = 0;
        1
    }
}

unsafe fn launchctl_enable_label(label: *const c_char) {
    unsafe {
        if label.is_null() || *label == 0 {
            return;
        }
        let mut target = [0 as c_char; 512];
        libc::snprintf(
            target.as_mut_ptr(),
            target.len(),
            c"gui/%u/%s".as_ptr(),
            libc::getuid(),
            label,
        );
        let mut argv = [
            c"launchctl".as_ptr().cast_mut(),
            c"enable".as_ptr().cast_mut(),
            target.as_mut_ptr(),
            ptr::null_mut(),
        ];
        let mut pid = 0;
        if libc::posix_spawn(
            &mut pid,
            c"/bin/launchctl".as_ptr(),
            ptr::null(),
            ptr::null(),
            argv.as_mut_ptr(),
            environ,
        ) != 0
            || pid <= 0
        {
            return;
        }
        let mut status = 0;
        while libc::waitpid(pid, &mut status, 0) < 0 && *libc::__error() == libc::EINTR {}
    }
}

unsafe fn plistbuddy(
    path: *const c_char,
    cmd: *const c_char,
    out: *mut c_char,
    outsz: usize,
) -> c_int {
    unsafe {
        let mut tmpl = *b"/tmp/ocerz_plb.XXXXXX\0";
        let mut fd = -1;
        if !out.is_null() {
            fd = libc::mkstemp(tmpl.as_mut_ptr().cast());
            if fd < 0 {
                return 0;
            }
        }
        let mut argv = [
            c"PlistBuddy".as_ptr().cast_mut(),
            c"-c".as_ptr().cast_mut(),
            cmd.cast_mut(),
            path.cast_mut(),
            ptr::null_mut(),
        ];
        let mut fa = core::mem::MaybeUninit::<libc::posix_spawn_file_actions_t>::uninit();
        let mut have_fa = false;
        if fd >= 0 {
            libc::posix_spawn_file_actions_init(fa.as_mut_ptr());
            libc::posix_spawn_file_actions_adddup2(fa.as_mut_ptr(), fd, 1);
            have_fa = true;
        }
        let mut pid = 0;
        let rc = libc::posix_spawn(
            &mut pid,
            c"/usr/libexec/PlistBuddy".as_ptr(),
            if have_fa {
                fa.as_mut_ptr()
            } else {
                ptr::null_mut()
            },
            ptr::null(),
            argv.as_mut_ptr(),
            environ,
        );
        if have_fa {
            libc::posix_spawn_file_actions_destroy(fa.as_mut_ptr());
        }
        if fd >= 0 {
            libc::close(fd);
        }
        if rc != 0 || pid <= 0 {
            if fd >= 0 {
                libc::unlink(tmpl.as_ptr().cast());
            }
            return 0;
        }
        let mut status = 0;
        while libc::waitpid(pid, &mut status, 0) < 0 && *libc::__error() == libc::EINTR {}
        let mut ok = libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0;
        if fd >= 0 {
            if ok {
                let r = libc::open(tmpl.as_ptr().cast(), libc::O_RDONLY);
                if r >= 0 {
                    let n = libc::read(r, out.cast(), outsz.saturating_sub(1));
                    libc::close(r);
                    if n > 0 {
                        *out.add(n as usize) = 0;
                        let nl = libc::strchr(out, b'\n' as c_int);
                        if !nl.is_null() {
                            *nl = 0;
                        }
                    } else {
                        ok = false;
                    }
                } else {
                    ok = false;
                }
            }
            libc::unlink(tmpl.as_ptr().cast());
        }
        c_int::from(ok)
    }
}

unsafe fn file_contains(path: *const c_char, needle: *const c_char) -> c_int {
    unsafe {
        let fd = libc::open(path, libc::O_RDONLY);
        if fd < 0 {
            return 0;
        }
        let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
        if libc::fstat(fd, st.as_mut_ptr()) != 0 {
            libc::close(fd);
            return 0;
        }
        let st = st.assume_init();
        if st.st_size <= 0 || st.st_size > (1 << 20) {
            libc::close(fd);
            return 0;
        }
        let buf = libc::malloc(st.st_size as usize).cast::<c_char>();
        if buf.is_null() {
            libc::close(fd);
            return 0;
        }
        let n = libc::read(fd, buf.cast(), st.st_size as usize);
        libc::close(fd);
        let found = c_int::from(
            n > 0
                && !libc::memmem(buf.cast(), n as usize, needle.cast(), libc::strlen(needle))
                    .is_null(),
        );
        libc::free(buf.cast());
        found
    }
}

unsafe fn launchd_plist_mode(path: *const c_char) {
    unsafe {
        let mut cur = [0 as c_char; 64];
        let has = plistbuddy(
            path,
            c"Print :EnvironmentVariables:OCERZ_MODE".as_ptr(),
            cur.as_mut_ptr(),
            cur.len(),
        ) != 0
            && cur[0] != 0;
        if crate::ffi::ocerz_mode != (crate::ffi::OCERZ_MODE_NATIVE as c_int) {
            if has {
                plistbuddy(
                    path,
                    c"Delete :EnvironmentVariables:OCERZ_MODE".as_ptr(),
                    ptr::null_mut(),
                    0,
                );
            }
            return;
        }
        if has && libc::strcmp(cur.as_ptr(), c"native".as_ptr()) == 0 {
            return;
        }
        plistbuddy(
            path,
            c"Add :EnvironmentVariables dict".as_ptr(),
            ptr::null_mut(),
            0,
        );
        if plistbuddy(
            path,
            c"Add :EnvironmentVariables:OCERZ_MODE string native".as_ptr(),
            ptr::null_mut(),
            0,
        ) == 0
        {
            plistbuddy(
                path,
                c"Set :EnvironmentVariables:OCERZ_MODE native".as_ptr(),
                ptr::null_mut(),
                0,
            );
        }
    }
}

unsafe fn rewrite_launchd_plist_for_ocerz(path: *const c_char, self_: *const c_char) {
    unsafe {
        let mut label = [0 as c_char; 256];
        if plistbuddy(
            path,
            c"Print :Label".as_ptr(),
            label.as_mut_ptr(),
            label.len(),
        ) != 0
        {
            launchctl_enable_label(label.as_ptr());
        }
        if file_contains(path, self_) != 0 {
            launchd_plist_mode(path);
            return;
        }
        let mut exe = [0 as c_char; 1024];
        let had_args = plistbuddy(
            path,
            c"Print :ProgramArguments:0".as_ptr(),
            exe.as_mut_ptr(),
            exe.len(),
        ) != 0
            && exe[0] != 0;
        if !had_args
            && (plistbuddy(
                path,
                c"Print :Program".as_ptr(),
                exe.as_mut_ptr(),
                exe.len(),
            ) == 0
                || exe[0] == 0)
        {
            return;
        }
        if libc::strcmp(exe.as_ptr(), self_) == 0 || file_has_x86_slice(exe.as_ptr()) == 0 {
            return;
        }
        let mut cmd = [0 as c_char; 1400];
        if !had_args {
            plistbuddy(
                path,
                c"Delete :ProgramArguments".as_ptr(),
                ptr::null_mut(),
                0,
            );
            if plistbuddy(
                path,
                c"Add :ProgramArguments array".as_ptr(),
                ptr::null_mut(),
                0,
            ) == 0
            {
                return;
            }
        }
        libc::snprintf(
            cmd.as_mut_ptr(),
            cmd.len(),
            c"Add :ProgramArguments:0 string %s".as_ptr(),
            self_,
        );
        if plistbuddy(path, cmd.as_ptr(), ptr::null_mut(), 0) == 0 {
            return;
        }
        if !had_args {
            libc::snprintf(
                cmd.as_mut_ptr(),
                cmd.len(),
                c"Add :ProgramArguments:1 string %s".as_ptr(),
                exe.as_ptr(),
            );
            plistbuddy(path, cmd.as_ptr(), ptr::null_mut(), 0);
        }
        libc::snprintf(
            cmd.as_mut_ptr(),
            cmd.len(),
            c"Add :Program string %s".as_ptr(),
            self_,
        );
        plistbuddy(path, cmd.as_ptr(), ptr::null_mut(), 0);
        libc::snprintf(
            cmd.as_mut_ptr(),
            cmd.len(),
            c"Set :Program %s".as_ptr(),
            self_,
        );
        plistbuddy(path, cmd.as_ptr(), ptr::null_mut(), 0);
        launchd_plist_mode(path);
        if !libc::getenv(c"OCERZ_IPCLOG".as_ptr()).is_null() {
            for cmd in [
                c"Add :StandardErrorPath string /tmp/ocerz_ipcserver.err",
                c"Set :StandardErrorPath /tmp/ocerz_ipcserver.err",
                c"Add :StandardOutPath string /tmp/ocerz_ipcserver.err",
                c"Set :StandardOutPath /tmp/ocerz_ipcserver.err",
            ] {
                plistbuddy(path, cmd.as_ptr(), ptr::null_mut(), 0);
            }
        }
    }
}

unsafe fn spawn_rewrite_launchd(hargv: *mut *mut c_char, n: c_int, self_: *const c_char) {
    unsafe {
        for k in 0..n {
            let arg = *hargv.offset(k as isize);
            if arg.is_null()
                || libc::strstr(arg, c"launchctl".as_ptr()).is_null()
                || libc::strstr(arg, c".plist".as_ptr()).is_null()
            {
                continue;
            }
            let mut path = [0 as c_char; 1200];
            if extract_plist_path(arg, path.as_mut_ptr(), path.len()) != 0
                && !libc::strstr(path.as_ptr(), c"valvesoftware.steam".as_ptr()).is_null()
            {
                rewrite_launchd_plist_for_ocerz(path.as_ptr(), self_);
            }
        }
    }
}

unsafe fn spawn_mock_keychain(
    hargv: *mut *mut c_char,
    n: c_int,
    at: c_int,
    gpath: *const c_char,
) -> c_int {
    unsafe {
        let base = libc::strrchr(gpath, b'/' as c_int);
        let base = if base.is_null() { gpath } else { base.add(1) };
        if libc::strcmp(base, c"Steam Helper".as_ptr()) != 0
            || !libc::getenv(c"OCERZ_NO_MOCK_KEYCHAIN".as_ptr()).is_null()
            || n >= 259
            || at > n
        {
            return n;
        }
        for k in at..n {
            let arg = *hargv.offset(k as isize);
            if !arg.is_null()
                && (libc::strcmp(arg, c"--use-mock-keychain".as_ptr()) == 0
                    || libc::strncmp(arg, c"--type=crashpad-handler".as_ptr(), 23) == 0)
            {
                return n;
            }
        }
        ptr::copy(
            hargv.offset(at as isize),
            hargv.offset(at as isize + 1),
            (n - at) as usize,
        );
        *hargv.offset(at as isize) = c"--use-mock-keychain".as_ptr().cast_mut();
        n + 1
    }
}

unsafe fn shebang_split(
    path: *const c_char,
    line: *mut c_char,
    cap: usize,
    interp: *mut *const c_char,
    arg: *mut *const c_char,
) -> c_int {
    unsafe {
        *interp = ptr::null();
        *arg = ptr::null();
        let fd = libc::open(path, libc::O_RDONLY);
        if fd < 0 {
            return 0;
        }
        let n = libc::read(fd, line.cast(), cap.saturating_sub(1));
        libc::close(fd);
        if n < 2 || *line != b'#' as c_char || *line.add(1) != b'!' as c_char {
            return 0;
        }
        *line.add(n as usize) = 0;
        let nl = libc::strchr(line, b'\n' as c_int);
        if !nl.is_null() {
            *nl = 0;
        }
        let mut p = line.add(2);
        while *p == b' ' as c_char || *p == b'\t' as c_char {
            p = p.add(1);
        }
        if *p == 0 {
            return 0;
        }
        *interp = p;
        while *p != 0 && *p != b' ' as c_char && *p != b'\t' as c_char {
            p = p.add(1);
        }
        if *p != 0 {
            *p = 0;
            p = p.add(1);
            while *p == b' ' as c_char || *p == b'\t' as c_char {
                p = p.add(1);
            }
            if *p != 0 {
                *arg = p;
            }
        }
        1
    }
}

unsafe fn guest_child_argv(
    hargv: *mut *mut c_char,
    cap: c_int,
    self_: *const c_char,
    path: *const c_char,
    argv: *const *mut c_char,
    shline: *mut c_char,
    shcap: usize,
) -> c_int {
    unsafe {
        let mut n: c_int = 0;
        *hargv.offset(n as isize) = self_.cast_mut();
        n += 1;
        *hargv.offset(n as isize) = c"-path".as_ptr().cast_mut();
        n += 1;
        let mut interp = ptr::null();
        let mut iarg = ptr::null();
        if shebang_split(path, shline, shcap, &mut interp, &mut iarg) != 0 {
            *hargv.offset(n as isize) = interp.cast_mut();
            n += 1;
            *hargv.offset(n as isize) = c"--".as_ptr().cast_mut();
            n += 1;
            *hargv.offset(n as isize) = interp.cast_mut();
            n += 1;
            if !iarg.is_null() {
                *hargv.offset(n as isize) = iarg.cast_mut();
                n += 1;
            }
            *hargv.offset(n as isize) = path.cast_mut();
            n += 1;
            let mut k: c_int = 1;
            while !argv.is_null()
                && !(*argv).is_null()
                && !(*argv.add(k as usize)).is_null()
                && n < cap - 2
            {
                *hargv.offset(n as isize) = *argv.add(k as usize);
                n += 1;
                k += 1;
            }
        } else {
            *hargv.offset(n as isize) = path.cast_mut();
            n += 1;
            *hargv.offset(n as isize) = c"--".as_ptr().cast_mut();
            n += 1;
            let first = n;
            let mut k: c_int = 0;
            while !argv.is_null() && !(*argv.add(k as usize)).is_null() && n < cap - 2 {
                *hargv.offset(n as isize) = *argv.add(k as usize);
                n += 1;
                k += 1;
            }
            if n == first {
                *hargv.offset(n as isize) = path.cast_mut();
                n += 1;
            }
            n = spawn_mock_keychain(hargv, n, first + 1, path);
        }
        *hargv.offset(n as isize) = ptr::null_mut();
        n
    }
}

unsafe fn guest_child_env(henv: *mut *mut c_char, cap: c_int, envp: *const *mut c_char) {
    unsafe {
        let mut m = 0;
        let mut k = 0;
        while !envp.is_null() && !(*envp.add(k)).is_null() && m < cap - 2 {
            *henv.offset(m as isize) = *envp.add(k);
            m += 1;
            k += 1;
        }
        m = env_inject_lowbase(henv, m, cap - 2);
        *henv.offset(m as isize) = ptr::null_mut();
    }
}

unsafe fn guest_vector(gv: u64, out: *mut *mut c_char, cap: usize) -> c_int {
    unsafe {
        let mut n = 0;
        let mut at = gv;
        while gv != 0 && n < cap - 1 {
            let p = ocerz_ld(at, 8);
            if p == 0 {
                break;
            }
            *out.add(n) = ocerz_g2h(p).cast();
            n += 1;
            at = at.wrapping_add(8);
        }
        *out.add(n) = ptr::null_mut();
        n as c_int
    }
}

unsafe fn guest_spawn_apply(
    vm: *mut OcerzVM,
    pid: *mut libc::pid_t,
    path: *const c_char,
    fa: *const libc::posix_spawn_file_actions_t,
    at: *const libc::posix_spawnattr_t,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_int {
    unsafe {
        let self_ = ocerz_self_path();
        if self_.is_null() || path.is_null() {
            return libc::EINVAL;
        }
        if libc::access(path, libc::X_OK) != 0 {
            return *libc::__error();
        }
        if guest_child_runs_native(path) != 0 {
            let mut count = 0;
            while !argv.is_null() && !(*argv.add(count)).is_null() {
                count += 1;
            }
            spawn_rewrite_launchd(argv.cast_mut(), count as c_int, self_);
            return libc::posix_spawn(
                pid,
                path,
                fa.cast_mut(),
                at.cast_mut(),
                argv.cast_mut(),
                envp.cast_mut(),
            );
        }
        let mut hargv = [ptr::null_mut(); GUEST_ARGV_MAX + 12];
        let mut shline = [0 as c_char; 1024];
        let n = guest_child_argv(
            hargv.as_mut_ptr(),
            (GUEST_ARGV_MAX + 12) as c_int,
            self_,
            path,
            argv,
            shline.as_mut_ptr(),
            shline.len(),
        );
        spawn_rewrite_launchd(hargv.as_mut_ptr(), n, self_);
        let mut henv = [ptr::null_mut(); GUEST_ENV_MAX + 8];
        guest_child_env(henv.as_mut_ptr(), (GUEST_ENV_MAX + 8) as c_int, envp);
        crate::ffi::ocerz_jit_require_ordered(vm);
        libc::posix_spawn(
            pid,
            self_,
            fa.cast_mut(),
            at.cast_mut(),
            hargv.as_mut_ptr(),
            henv.as_mut_ptr(),
        )
    }
}

unsafe fn guest_exec_apply(
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_int {
    unsafe {
        let self_ = ocerz_self_path();
        if self_.is_null() || path.is_null() || argv.is_null() {
            return libc::EINVAL;
        }
        if !libc::getenv(c"OCERZ_HOSTMASKLOG".as_ptr()).is_null() {
            let mut hm = core::mem::MaybeUninit::<libc::sigset_t>::uninit();
            let mut hv = 0u32;
            if libc::pthread_sigmask(libc::SIG_BLOCK, ptr::null(), hm.as_mut_ptr()) == 0 {
                let hm = hm.assume_init();
                for sg in 1..32 {
                    if libc::sigismember(&hm, sg) != 0 {
                        hv |= 1u32 << sg;
                    }
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: HOSTMASK-EXEC[%d] mask=%#x path=%s\n".as_ptr(),
                libc::getpid(),
                hv,
                path,
            );
        }
        if libc::access(path, libc::X_OK) != 0 {
            return *libc::__error();
        }
        crate::ffi::ocerz_tcache_flush();
        if guest_child_runs_native(path) != 0 {
            libc::execve(
                path,
                argv.cast::<*const c_char>(),
                envp.cast::<*const c_char>(),
            );
            return *libc::__error();
        }
        let mut hargv = [ptr::null_mut(); GUEST_ARGV_MAX + 12];
        let mut shline = [0 as c_char; 1024];
        let n = guest_child_argv(
            hargv.as_mut_ptr(),
            (GUEST_ARGV_MAX + 12) as c_int,
            self_,
            path,
            argv,
            shline.as_mut_ptr(),
            shline.len(),
        );
        let mut henv = [ptr::null_mut(); GUEST_ENV_MAX + 8];
        guest_child_env(henv.as_mut_ptr(), (GUEST_ENV_MAX + 8) as c_int, envp);
        if !libc::getenv(c"OCERZ_EXECLOG".as_ptr()).is_null() {
            libc::fprintf(crate::log::stderr(), c"ocerz: EXECLOG ->".as_ptr());
            for k in 0..n {
                let arg = hargv[k as usize];
                libc::fprintf(
                    crate::log::stderr(),
                    c" %s".as_ptr(),
                    if arg.is_null() {
                        c"(null)".as_ptr()
                    } else {
                        arg
                    },
                );
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        }
        libc::execve(
            self_,
            hargv.as_ptr().cast::<*const c_char>(),
            henv.as_ptr().cast::<*const c_char>(),
        );
        let exec_errno = *libc::__error();
        if !libc::getenv(c"OCERZ_EXECLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: EXECFAIL[%d] %s: %s (%d)\n".as_ptr(),
                libc::getpid(),
                self_,
                libc::strerror(exec_errno),
                exec_errno,
            );
        }
        exec_errno
    }
}

pub(super) unsafe fn sys_posix_spawn(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        if a[1] == 0 {
            ret_err(cpu, libc::EINVAL as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        let mut argv = [ptr::null_mut(); GUEST_ARGV_MAX];
        let mut envp = [ptr::null_mut(); GUEST_ENV_MAX];
        guest_vector(a[3], argv.as_mut_ptr(), GUEST_ARGV_MAX);
        guest_vector(a[4], envp.as_mut_ptr(), GUEST_ENV_MAX);
        let mut at = core::mem::MaybeUninit::<libc::posix_spawnattr_t>::uninit();
        let mut fa = core::mem::MaybeUninit::<libc::posix_spawn_file_actions_t>::uninit();
        let mut have_at = 0;
        let mut have_fa = 0;
        let mut hpid = 0;
        let mut rc = spawn_guest_args(
            a[2],
            at.as_mut_ptr(),
            &mut have_at,
            fa.as_mut_ptr(),
            &mut have_fa,
        );
        if rc == 0 {
            rc = guest_spawn_apply(
                vm,
                &mut hpid,
                ocerz_g2h(a[1]).cast(),
                if have_fa != 0 {
                    fa.as_ptr()
                } else {
                    ptr::null()
                },
                if have_at != 0 {
                    at.as_ptr()
                } else {
                    ptr::null()
                },
                if a[3] != 0 {
                    argv.as_ptr()
                } else {
                    ptr::null()
                },
                if a[4] != 0 {
                    envp.as_ptr()
                } else {
                    ptr::null()
                },
            );
        }
        if have_fa != 0 {
            libc::posix_spawn_file_actions_destroy(fa.as_mut_ptr());
        }
        if have_at != 0 {
            libc::posix_spawnattr_destroy(at.as_mut_ptr());
        }
        if rc != 0 {
            ret_err(cpu, rc as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if a[0] != 0 {
            ocerz_st(a[0], 4, hpid as u32 as u64);
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_execve(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let a = &*a;
        if a[0] == 0 || a[1] == 0 {
            ret_err(cpu, libc::EINVAL as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        let mut argv = [ptr::null_mut(); GUEST_ARGV_MAX];
        let mut envp = [ptr::null_mut(); GUEST_ENV_MAX];
        guest_vector(a[1], argv.as_mut_ptr(), GUEST_ARGV_MAX);
        guest_vector(a[2], envp.as_mut_ptr(), GUEST_ENV_MAX);
        let err = guest_exec_apply(
            ocerz_g2h(a[0]).cast(),
            argv.as_ptr(),
            if a[2] != 0 {
                envp.as_ptr()
            } else {
                ptr::null()
            },
        );
        ret_err(cpu, err as u64);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_execve(
    _vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_int {
    unsafe { guest_exec_apply(path, argv, envp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_posix_spawn(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    pid: *mut c_int,
    path: *const c_char,
    fa: *const libc::posix_spawn_file_actions_t,
    attr: *const libc::posix_spawnattr_t,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_int {
    unsafe {
        let mut at = core::mem::MaybeUninit::<libc::posix_spawnattr_t>::uninit();
        let mut have_at = 0;
        if !attr.is_null() && !(*attr).is_null() {
            let mut flags = 0i16;
            let mut def = core::mem::MaybeUninit::<libc::sigset_t>::uninit();
            let mut mask = core::mem::MaybeUninit::<libc::sigset_t>::uninit();
            let mut pgroup = 0;
            libc::sigemptyset(def.as_mut_ptr());
            libc::sigemptyset(mask.as_mut_ptr());
            libc::posix_spawnattr_getflags(attr, &mut flags);
            libc::posix_spawnattr_getsigdefault(attr, def.as_mut_ptr());
            libc::posix_spawnattr_getsigmask(attr, mask.as_mut_ptr());
            libc::posix_spawnattr_getpgroup(attr, &mut pgroup);
            spawn_attr_sanitized(
                &mut at.assume_init(),
                flags,
                def.as_ptr(),
                mask.assume_init(),
                pgroup,
            );
            have_at = 1;
        }
        let mut hpid = 0;
        let rc = guest_spawn_apply(
            vm,
            &mut hpid,
            path,
            if !fa.is_null() && !(*fa).is_null() {
                fa
            } else {
                ptr::null()
            },
            if have_at != 0 {
                at.as_ptr()
            } else {
                ptr::null()
            },
            argv,
            envp,
        );
        if have_at != 0 {
            libc::posix_spawnattr_destroy(at.as_mut_ptr());
        }
        if rc == 0 && !pid.is_null() {
            *pid = hpid;
        }
        rc
    }
}
