//! Command-line parsing and the entry point.
//!
//! Three startup decisions are made here rather than lazily. Some shared-cache
//! images' Objective-C categories must be visible before any class is realized,
//! because a dlopen registers them too late for classes that already exist:
//! CoreSpotlight adds encodeWithCSCoder: categories to Foundation's collection
//! classes, and without them the indexing AppKit kicks off about 20 s into a
//! session throws an unrecognized-selector NSException inside a dispatch block
//! and takes the process down. A default list is preloaded;
//! OCERZ_PRELOAD_OBJC=@cat preloads every category-bearing image instead.
//!
//! The process also starts in the single-observer ("plain") memory model and
//! only retires it when a second observer actually appears - a thread, a
//! fork/spawn, a hostwq worker or a writable shared mapping - which the syscall
//! layer does through ocerz_jit_require_ordered(). OCERZ_NOJIT_EXE interprets
//! only the processes whose command line matches, and since the environment
//! inherits through Wine's exec chain that singles one process out for the
//! full-visibility interpreter while the rest stay on the JIT.
//! OCERZ_STRACE_EXE turns on the syscall trace the same way, for one process of
//! Wine's tree instead of all of them, and OCERZ_EXE_ENV sets variables the same
//! way: "cs2.exe:OCERZ_NO_SUPERBLOCK=1,OCERZ_TSO_STRICT=1;other.exe:..." gives
//! each listed process its own JIT knobs before any of them is read. A process
//! whose loader is named wine is marked as one of Wine's, which fixes the base
//! of its memory arena (see mem.c). OCERZ_STDERR_FILE appends every process's
//! standard error to one file before anything else runs: Chromium and others
//! start their children with standard error closed or pointed at nothing, and a
//! child that dies early under ocerz otherwise leaves no trace of why.
//!
//! The third decision is which universe the guest binds against. -native and
//! -cache pick the mode outright and the last one on the command line wins;
//! with neither, OCERZ_MODE decides, which is how a child inherits the mode
//! across a spawn or an exec, so a value there that is neither native nor cache
//! is refused rather than quietly ignored. Native mode has no static loader
//! path at all - a program that links against nothing has nothing to bridge
//! into - so a non-dynamic image is refused before the VM starts.
//!
//! The loader is handed a copy of the environment, not environ itself. Loading
//! a guest in native mode opens the host's own frameworks, and their
//! initializers run then: CoreFoundation's calls setenv for
//! __CF_USER_TEXT_ENCODING, which reallocates environ and frees the array a
//! pointer taken earlier still names. The guest's initial stack is built from
//! that pointer only after loading, so it read freed memory, and whether that
//! crashed depended on whether the block had been reused yet - which it was
//! when the environment happened to be the size bash passes and not the size
//! zsh passes. The copy is taken once, owns its strings, and lives as long as
//! the process, so no framework the guest makes ocerz open can pull the guest's
//! environment out from under it. It is also older than the CFProcessPath
//! variable bridge.c sets for the length of CoreFoundation's initializer to
//! give native CoreFoundation the guest's executable as the process path, so
//! the guest's initial stack never carries that variable, and the host
//! environment, which the guest's environ names, holds it only while that
//! initializer runs.
//!
//! A program named by its application bundle runs the bundle's executable:
//! CFBundleExecutable from Contents/Info.plist, or, when that cannot be read
//! as XML, the bundle's own name, under Contents/MacOS. Steam's launch options
//! substitute the bundle for %command%, so without this a game launched from
//! Steam as "ocerz %command%" ended at once with "cannot read". The guest's
//! argv[0] becomes the executable's path, as it is for a bundle started by the
//! system.
//!
//! DYLD_INSERT_LIBRARIES names libraries for the guest, but the host's dyld
//! reads it first and loads their arm64 slices into ocerz itself. Steam sets
//! it to its loader and overlay for every game it launches, and inside ocerz
//! they broke startup within half a second. So ocerz started with the variable
//! set moves it to OCERZ_GUEST_DYLD_INSERT_LIBRARIES, removes it and re-executes
//! itself, which keeps the process ID Steam tracks; the clean copy puts the
//! variable back before the guest's environment is taken, so the guest sees it
//! as it was set and the loader inserts the libraries into the guest alone.

use core::ffi::{c_char, c_int};
use core::ptr;

use crate::ffi::{self, OcerzVM};

const MOCK_KEYCHAIN_ARGS: usize = 512;
const PATH_MAX: usize = libc::PATH_MAX as usize;

static mut MOCK_KEYCHAIN_ARGV: [*mut c_char; MOCK_KEYCHAIN_ARGS + 2] =
    [ptr::null_mut(); MOCK_KEYCHAIN_ARGS + 2];
static mut BUNDLE_EXE: [c_char; PATH_MAX] = [0; PATH_MAX];
static mut BUNDLE_PLIST_BUF: [c_char; 1 << 20] = [0; 1 << 20];
static mut VM: OcerzVM = unsafe { core::mem::zeroed() };

unsafe extern "C" {
    static mut environ: *mut *mut c_char;
}

unsafe fn envp() -> *mut *mut c_char {
    unsafe { ptr::addr_of_mut!(environ).read() }
}

unsafe fn env_snapshot(env: *mut *mut c_char) -> *mut *mut c_char {
    unsafe {
        let mut n = 0usize;
        while !env.is_null() && !(*env.add(n)).is_null() {
            n += 1;
        }
        let copy = libc::calloc(n + 1, core::mem::size_of::<*mut c_char>()).cast::<*mut c_char>();
        if copy.is_null() {
            return env;
        }
        for k in 0..n {
            *copy.add(k) = libc::strdup(*env.add(k));
            if (*copy.add(k)).is_null() {
                return env;
            }
        }
        copy
    }
}

unsafe fn usage() {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-native|-cache] [-path file] [--] program [args...]\n       ocerz version\n".as_ptr(),
        );
    }
}

unsafe fn is_wine_loader(path: *const c_char) -> c_int {
    unsafe {
        let mut resolved = [0 as c_char; PATH_MAX];
        let path = if libc::realpath(path, resolved.as_mut_ptr()).is_null() {
            path
        } else {
            resolved.as_ptr()
        };
        let mut base = libc::strrchr(path, b'/' as c_int);
        base = if base.is_null() { path.cast_mut() } else { base.add(1) };
        (libc::strcmp(base, c"wine".as_ptr()) == 0
            || libc::strcmp(base, c"wine64".as_ptr()) == 0) as c_int
    }
}

unsafe fn apply_wine_defaults(path: *const c_char) {
    unsafe {
        if is_wine_loader(path) == 0 {
            return;
        }
        ptr::addr_of_mut!(crate::ported::mem::ocerz_wine_process).write(1);
        let preload = libc::getenv(c"OCERZ_PRELOAD_OBJC".as_ptr());
        if preload.is_null() || libc::strcmp(preload, c"1".as_ptr()) == 0 {
            libc::setenv(
                c"OCERZ_PRELOAD_OBJC".as_ptr(),
                c"/AppKit.framework/,/QuartzCore.framework/,/HIToolbox.framework/,/CoreSpotlight.framework/"
                    .as_ptr(),
                1,
            );
        }
    }
}

unsafe fn mock_keychain_argv(
    path: *const c_char,
    argc: *mut c_int,
    argv: *mut *mut c_char,
) -> *mut *mut c_char {
    unsafe {
        let mut base = libc::strrchr(path, b'/' as c_int);
        base = if base.is_null() { path.cast_mut() } else { base.add(1) };
        if libc::strcmp(base, c"Steam Helper".as_ptr()) != 0
            || !libc::getenv(c"OCERZ_NO_MOCK_KEYCHAIN".as_ptr()).is_null()
            || argc.read() < 1
            || argc.read() as usize >= MOCK_KEYCHAIN_ARGS
        {
            return argv;
        }
        for k in 1..argc.read() as usize {
            let arg = *argv.add(k);
            if libc::strcmp(arg, c"--use-mock-keychain".as_ptr()) == 0
                || libc::strncmp(arg, c"--type=crashpad-handler".as_ptr(), 23) == 0
            {
                return argv;
            }
        }
        let out = ptr::addr_of_mut!(MOCK_KEYCHAIN_ARGV).cast::<*mut c_char>();
        *out = *argv;
        *out.add(1) = c"--use-mock-keychain".as_ptr().cast_mut();
        for k in 1..argc.read() as usize {
            *out.add(k + 1) = *argv.add(k);
        }
        *out.add(argc.read() as usize + 1) = ptr::null_mut();
        argc.write(argc.read() + 1);
        out
    }
}

unsafe fn bundle_executable(dir: *const c_char, out: *mut c_char, n: usize) -> c_int {
    unsafe {
        let mut st: libc::stat = core::mem::zeroed();
        if libc::stat(dir, &mut st) != 0
            || (st.st_mode & libc::S_IFMT as u16) != libc::S_IFDIR as u16
        {
            return 0;
        }
        let mut plist = [0 as c_char; PATH_MAX];
        let mut name = [0 as c_char; PATH_MAX];
        libc::snprintf(
            plist.as_mut_ptr(),
            plist.len(),
            c"%s/Contents/Info.plist".as_ptr(),
            dir,
        );
        let file = libc::fopen(plist.as_ptr(), c"rb".as_ptr());
        if !file.is_null() {
            let buf = ptr::addr_of_mut!(BUNDLE_PLIST_BUF).cast::<c_char>();
            let len = libc::fread(buf.cast(), 1, (1 << 20) - 1, file);
            libc::fclose(file);
            *buf.add(len) = 0;
            let key = libc::strstr(buf, c"<key>CFBundleExecutable</key>".as_ptr());
            let value = if key.is_null() {
                ptr::null_mut()
            } else {
                libc::strstr(key, c"<string>".as_ptr())
            };
            let end = if value.is_null() {
                ptr::null_mut()
            } else {
                libc::strstr(value, c"</string>".as_ptr())
            };
            let name_len = if end.is_null() {
                -1
            } else {
                end.offset_from(value) - 8
            };
            if name_len >= 0 && (name_len as usize) < name.len() {
                let len = name_len as usize;
                libc::memcpy(name.as_mut_ptr().cast(), value.add(8).cast(), len);
                *name.as_mut_ptr().add(len) = 0;
            }
        }
        if name[0] == 0 {
            let mut base = libc::strrchr(dir, b'/' as c_int);
            base = if base.is_null() { dir.cast_mut() } else { base.add(1) };
            let mut len = libc::strlen(base);
            while len > 0 && *base.add(len - 1) == b'/' as c_char {
                len -= 1;
            }
            if len > 4 && libc::strncmp(base.add(len - 4), c".app".as_ptr(), 4) == 0 {
                len -= 4;
            }
            if len == 0 || len >= name.len() {
                return 0;
            }
            libc::memcpy(name.as_mut_ptr().cast(), base.cast(), len);
            *name.as_mut_ptr().add(len) = 0;
        }
        let len = libc::snprintf(
            out,
            n,
            c"%s/Contents/MacOS/%s".as_ptr(),
            dir,
            name.as_ptr(),
        );
        (len > 0
            && (len as usize) < n
            && libc::stat(out, &mut st) == 0
            && (st.st_mode & libc::S_IFMT as u16) == libc::S_IFREG as u16) as c_int
    }
}

#[allow(deprecated)]
unsafe fn reexec_without_host_insertion(argv: *mut *mut c_char) {
    unsafe {
        let list = libc::getenv(c"DYLD_INSERT_LIBRARIES".as_ptr());
        if list.is_null()
            || *list == 0
            || !libc::getenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr()).is_null()
        {
            return;
        }
        let mut self_path = [0 as c_char; PATH_MAX];
        let mut size = PATH_MAX as u32;
        if libc::_NSGetExecutablePath(self_path.as_mut_ptr(), &mut size) != 0
            || libc::setenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr(), list, 1) != 0
        {
            return;
        }
        libc::unsetenv(c"DYLD_INSERT_LIBRARIES".as_ptr());
        libc::execv(self_path.as_ptr(), argv.cast::<*const c_char>());
        libc::setenv(
            c"DYLD_INSERT_LIBRARIES".as_ptr(),
            libc::getenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr()),
            1,
        );
        libc::unsetenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr());
    }
}

unsafe fn restore_guest_insertion() {
    unsafe {
        let list = libc::getenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr());
        if !list.is_null() {
            libc::setenv(c"DYLD_INSERT_LIBRARIES".as_ptr(), list, 1);
            libc::unsetenv(c"OCERZ_GUEST_DYLD_INSERT_LIBRARIES".as_ptr());
        }
    }
}

#[allow(unused_unsafe)]
unsafe fn main_inner(argc: c_int, argv: *mut *mut c_char) -> c_int {
    unsafe {
        let ef = libc::getenv(c"OCERZ_STDERR_FILE".as_ptr());
        let efd = if !ef.is_null() && *ef != 0 {
            libc::open(
                ef,
                libc::O_WRONLY | libc::O_CREAT | libc::O_APPEND,
                0o644 as c_int,
            )
        } else {
            -1
        };
        if efd >= 0 {
            libc::dup2(efd, 2);
            libc::close(efd);
        }

        reexec_without_host_insertion(argv);
        restore_guest_insertion();

        if !libc::getenv(c"OCERZ_HOSTMASKLOG".as_ptr()).is_null() {
            let mut mask = core::mem::MaybeUninit::<libc::sigset_t>::uninit();
            let mut value = 0u32;
            if libc::pthread_sigmask(libc::SIG_BLOCK, ptr::null(), mask.as_mut_ptr()) == 0 {
                for sig in 1..32 {
                    if libc::sigismember(mask.as_ptr(), sig) != 0 {
                        value |= 1u32 << sig;
                    }
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: HOSTMASK-START[%d] mask=%#x argv1=%s\n".as_ptr(),
                libc::getpid() as c_int,
                value,
                if argc > 1 { *argv.add(1) } else { c"".as_ptr() },
            );
        }
        if !libc::getenv(c"OCERZ_EXECLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: EXECSTART[%d<-%d]".as_ptr(),
                libc::getpid() as c_int,
                libc::getppid() as c_int,
            );
            for k in 0..argc as usize {
                let arg = *argv.add(k);
                libc::fprintf(
                    crate::log::stderr(),
                    c" %s".as_ptr(),
                    if arg.is_null() { c"(null)".as_ptr() } else { arg },
                );
            }
            let mut envc = 0;
            let mut noexec = -1;
            let mut reserve = -1;
            let mut socket = -1;
            let mut env = envp();
            while !(*env).is_null() {
                let value = *env;
                if libc::strncmp(value, c"WINELOADERNOEXEC=".as_ptr(), 17) == 0 {
                    noexec = envc;
                } else if libc::strncmp(value, c"WINEPRELOADRESERVE=".as_ptr(), 19) == 0 {
                    reserve = envc;
                } else if libc::strncmp(value, c"WINESERVERSOCKET=".as_ptr(), 17) == 0 {
                    socket = envc;
                }
                envc += 1;
                env = env.add(1);
            }
            libc::fprintf(
                crate::log::stderr(),
                c" envc=%d wine_env=%d/%d/%d\n".as_ptr(),
                envc,
                noexec,
                reserve,
                socket,
            );
        }

        let summary =
            ptr::addr_of_mut!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
        let mut w = summary;
        let end = summary.add(255);
        for i in 1..argc as usize {
            if w >= end {
                break;
            }
            let arg = *argv.add(i);
            let base = {
                let p = libc::strrchr(arg, b'/' as c_int);
                if p.is_null() { arg } else { p.add(1) }
            };
            if w != summary && w < end {
                *w = b' ' as c_char;
                w = w.add(1);
            }
            let mut p = base;
            while *p != 0 && w < end {
                *w = *p;
                w = w.add(1);
                p = p.add(1);
            }
        }
        *w = 0;

        let mut trace = 0;
        let mut strace = 0;
        let mut nojit = 0;
        let mut mode_from_flag = 0;
        let mut load_path: *const c_char = ptr::null();
        libc::setenv(c"MallocNanoZone".as_ptr(), c"0".as_ptr(), 1);
        let mut i = 1usize;
        if argc == 2 {
            let arg = *argv.add(1);
            if libc::strcmp(arg, c"version".as_ptr()) == 0
                || libc::strcmp(arg, c"-version".as_ptr()) == 0
                || libc::strcmp(arg, c"--version".as_ptr()) == 0
            {
                libc::printf(c"%s %s\n".as_ptr(), c"AArchX".as_ptr(), c"0.6".as_ptr());
                return 0;
            }
        }
        while i < argc as usize {
            let arg = *argv.add(i);
            if *arg != b'-' as c_char {
                break;
            }
            if libc::strcmp(arg, c"--".as_ptr()) == 0 {
                i += 1;
                break;
            } else if libc::strcmp(arg, c"-v".as_ptr()) == 0 {
                let v = ptr::addr_of_mut!(crate::ported::globals::ocerz_verbose);
                v.write(v.read().wrapping_add(1));
            } else if libc::strcmp(arg, c"-trace".as_ptr()) == 0 {
                trace = 1;
            } else if libc::strcmp(arg, c"-strace".as_ptr()) == 0 {
                strace = 1;
            } else if libc::strcmp(arg, c"-no-jit".as_ptr()) == 0 {
                nojit = 1;
            } else if libc::strcmp(arg, c"-native".as_ptr()) == 0 {
                ptr::addr_of_mut!(crate::ported::globals::ocerz_mode)
                    .write(ffi::OCERZ_MODE_NATIVE as c_int);
                mode_from_flag = 1;
            } else if libc::strcmp(arg, c"-cache".as_ptr()) == 0 {
                ptr::addr_of_mut!(crate::ported::globals::ocerz_mode)
                    .write(ffi::OCERZ_MODE_CACHE as c_int);
                mode_from_flag = 1;
            } else if libc::strcmp(arg, c"-path".as_ptr()) == 0 && i + 1 < argc as usize {
                i += 1;
                load_path = *argv.add(i);
            } else {
                usage();
                return 64;
            }
            i += 1;
        }
        if mode_from_flag == 0 {
            let mode_env = libc::getenv(c"OCERZ_MODE".as_ptr());
            if !mode_env.is_null() && *mode_env != 0 {
                if libc::strcmp(mode_env, c"native".as_ptr()) == 0 {
                    ptr::addr_of_mut!(crate::ported::globals::ocerz_mode)
                        .write(ffi::OCERZ_MODE_NATIVE as c_int);
                } else if libc::strcmp(mode_env, c"cache".as_ptr()) == 0 {
                    ptr::addr_of_mut!(crate::ported::globals::ocerz_mode)
                        .write(ffi::OCERZ_MODE_CACHE as c_int);
                } else {
                    crate::ocerz_fatal!("unknown OCERZ_MODE value '%s', want native or cache\n", mode_env);
                    return 64;
                }
            }
        }
        let mode = ptr::addr_of!(crate::ported::globals::ocerz_mode).read();
        crate::ocerz_log!(
            "mode: %s\n",
            if mode == ffi::OCERZ_MODE_NATIVE as c_int {
                c"native".as_ptr()
            } else {
                c"cache".as_ptr()
            }
        );
        ffi::ocerz_afp_enable();
        if i >= argc as usize {
            usage();
            return 64;
        }
        if load_path.is_null() {
            load_path = *argv.add(i);
        }
        let bundle_exe = ptr::addr_of_mut!(BUNDLE_EXE).cast::<c_char>();
        if bundle_executable(load_path, bundle_exe, PATH_MAX) != 0 {
            if load_path == *argv.add(i) {
                *argv.add(i) = bundle_exe;
            }
            load_path = bundle_exe;
        }

        apply_wine_defaults(load_path);
        if ptr::addr_of!(crate::ported::globals::ocerz_verbose).read() >= 2 {
            trace = 1;
        }

        let vm = ptr::addr_of_mut!(VM);
        ffi::ocerz_vm_init(vm);
        (*vm).trace = trace;
        (*vm).strace = strace;
        (*vm).jit_enabled =
            (nojit == 0 && libc::getenv(c"OCERZ_NOJIT".as_ptr()).is_null()) as c_int;
        let sx = libc::getenv(c"OCERZ_STRACE_EXE".as_ptr());
        if !sx.is_null() && *sx != 0 && !libc::strstr(summary, sx).is_null() {
            (*vm).strace = 1;
        }
        let ev = libc::getenv(c"OCERZ_EXE_ENV".as_ptr());
        let mut spec = [0 as c_char; 1024];
        libc::snprintf(
            spec.as_mut_ptr(),
            spec.len(),
            c"%s".as_ptr(),
            if ev.is_null() { c"".as_ptr() } else { ev },
        );
        let mut rule = libc::strtok(spec.as_mut_ptr(), c";".as_ptr());
        while !rule.is_null() {
            let colon = libc::strchr(rule, b':' as c_int);
            if !colon.is_null() {
                *colon = 0;
                if *rule != 0 && !libc::strstr(summary, rule).is_null() {
                    let mut save = ptr::null_mut();
                    let mut kv = libc::strtok_r(colon.add(1), c",".as_ptr(), &mut save);
                    while !kv.is_null() {
                        let eq = libc::strchr(kv, b'=' as c_int);
                        if !eq.is_null() {
                            *eq = 0;
                            libc::setenv(kv, eq.add(1), 1);
                            libc::fprintf(
                                crate::log::stderr(),
                                c"ocerz: EXE-ENV %s=%s for '%s'\n".as_ptr(),
                                kv,
                                eq.add(1),
                                summary,
                            );
                        }
                        kv = libc::strtok_r(ptr::null_mut(), c",".as_ptr(), &mut save);
                    }
                }
            }
            rule = libc::strtok(ptr::null_mut(), c";".as_ptr());
        }
        ffi::ocerz_bigring_init();
        let nx = libc::getenv(c"OCERZ_NOJIT_EXE".as_ptr());
        if !nx.is_null() && *nx != 0 && !libc::strstr(summary, nx).is_null() {
            (*vm).jit_enabled = 0;
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: NOJIT-EXE interpreting '%s'\n".as_ptr(),
                summary,
            );
        }

        let dynamic = ffi::ocerz_peek_dynamic(load_path);
        if dynamic == -2 {
            crate::ocerz_fatal!(
                "%s has no x86_64 slice: it is not an Intel program, so there is nothing to translate\n",
                load_path
            );
            return 64;
        }
        if dynamic < 0 {
            crate::ocerz_fatal!("cannot read %s\n", load_path);
            return 65;
        }
        if dynamic == 0 && mode == ffi::OCERZ_MODE_NATIVE as c_int {
            crate::ocerz_fatal!("native mode cannot run the static image %s\n", load_path);
            return 64;
        }
        (*vm).jit_plain_mem = libc::getenv(c"OCERZ_NO_PLAIN_MEM".as_ptr()).is_null() as c_int;

        if dynamic != 0 {
            let mut gargc = argc - i as c_int;
            let gargv = mock_keychain_argv(load_path, &mut gargc, argv.add(i));
            return ffi::ocerz_dyld_run(vm, load_path, gargc, gargv, env_snapshot(envp()));
        }
        if ffi::ocerz_mem_init(0x100000000, 0x900000000) != ffi::OCERZ_OK as c_int {
            return 70;
        }
        if ffi::ocerz_load_image(load_path, ptr::addr_of_mut!((*vm).image))
            != ffi::OCERZ_OK as c_int
        {
            crate::ocerz_fatal!("cannot load %s\n", load_path);
            return 65;
        }
        if ffi::ocerz_setup_stack(
            vm,
            ptr::addr_of!((*vm).image),
            argc - i as c_int,
            argv.add(i),
            envp(),
        ) != ffi::OCERZ_OK as c_int
        {
            crate::ocerz_fatal!("cannot build guest stack\n");
            return 70;
        }
        (*vm).cpu.rip = (*vm).image.entry;
        ffi::ocerz_vm_run(vm)
    }
}

#[unsafe(no_mangle)]
#[linkage = "weak"]
pub unsafe extern "C" fn main(argc: c_int, argv: *mut *mut c_char) -> c_int {
    unsafe { main_inner(argc, argv) }
}
