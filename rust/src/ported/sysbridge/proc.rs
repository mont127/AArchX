//! ---- processes ----
//! fork and vfork are the fork syscall's body, so ocerz's fork handlers run and
//! the child keeps only the forking thread's cpu and starts without translations,
//! and vfork is a fork, as it is in cache mode.  The JIT never calls them from
//! inside a block (vdylib.h), because the child cannot return into a translation
//! it did not inherit, and for the same reason a fork inside a callback is
//! refused by name when a translated block's frame lies underneath the callback
//! on the host stack; a fork a native thread's callback makes with no translated
//! frame below it, or one made from the outermost level, is performed.  The
//! child is a working native-mode process: the bridge, the callback bank and the
//! API database are memory it inherits whole, their locks are put back to their
//! initial state, and a thread it creates is given a guest personality the way
//! the parent's were.  Native mode installs ocerz's fork handlers before any guest
//! code runs, so a guest's pthread_atfork handlers, which are host handlers
//! calling back into the guest, run their prepare step before ocerz takes its
//! locks and their child step after ocerz has released them.
//!
//! execve, execv, execvp, execvP, execl, execle and execlp, and posix_spawn and
//! posix_spawnp, start the new program the way the syscalls do in cache mode,
//! through src/syscall.c: the program ocerz runs is ocerz itself, told with
//! -path which image to load and handed the guest's argv whole, argv[0]
//! included, and a script is started as its #! line's interpreter under ocerz
//! with the script's path after the interpreter's argument.  Its environment is
//! the guest's with OCERZ_MODE=native added, so the child comes up in native mode
//! too, and that holds for a child given no environment at all.  The execl forms
//! build argv out of their variadic arguments up to the null, execle takes the
//! environment after it, and the search forms walk PATH, or the path they are
//! given, the way Apple's libc does, trying each candidate and going on past the
//! errors it goes on past.  posix_spawn's file actions and attributes are the
//! host's own objects, since posix_spawn_file_actions_init and
//! posix_spawnattr_init are ordinary crossings; the actions are handed to the
//! host's posix_spawn as they are, and of the attributes, as in cache mode, only
//! the public flags, the default and blocked signal sets and the process group
//! are carried over, with the signals ocerz needs delivered taken out of the
//! blocked set.
//!
//! The policy for what the child is comes from cache mode unchanged: every
//! program the guest starts is started under ocerz.  A universal binary runs its
//! x86_64 slice, and a program with no x86_64 slice, arm64-only, is refused by
//! the child ocerz, which cannot read it, with status 65, exactly as a cache-mode
//! guest's is.  system and popen run the command with sh -c, where sh is
//! /bin/sh, as Apple's x86 libc does, so the shell is the x86_64 slice of the
//! system's /bin/sh running under ocerz in native mode, and a program the command
//! names is in turn started under ocerz.  system follows Apple's: SIGINT and
//! SIGQUIT are ignored in the guest's handler table while the command runs and
//! restored to the child's default if the guest had not ignored them, SIGCHLD is
//! blocked, and the status comes from wait4, 127 in the exit status when the
//! shell could not be started.  popen and pclose keep their own list of open
//! streams, each a host FILE over a pipe, or a socket pair for r+, and close
//! every other popen'd stream in the child, as Apple's do.

use core::ffi::{c_char, c_int, c_short, c_void};

use super::common::*;
use crate::ffi::*;

unsafe extern "C" {
    static mut environ: *mut *mut c_char;
    fn fwide(fp: *mut libc::FILE, mode: c_int) -> c_int;
    fn strsep(stringp: *mut *mut c_char, delim: *const c_char) -> *mut c_char;
}

fn w_exitcode(ret: c_int, sig: c_int) -> c_int {
    (ret << 8) | sig
}

#[inline(never)]
fn sb_frame_address() -> usize {
    let fp: usize;
    unsafe {
        core::arch::asm!("mov {}, x29", out(reg) fp, options(nomem, nostack, preserves_flags));
    }
    fp
}

unsafe fn sb_translated_frame_below(vm: *mut OcerzVM) -> c_int {
    unsafe {
        let self_ = libc::pthread_self();
        let top = libc::pthread_get_stackaddr_np(self_) as usize;
        let bottom = top - libc::pthread_get_stacksize_np(self_);
        let mut fp = sb_frame_address();
        let mut depth = 0;
        while depth < 65536 && fp >= bottom && fp + 16 <= top && (fp & 7) == 0 {
            let record = fp as *const usize;
            if ocerz_jit_pc_in_arena(vm, record.add(1).read() as *const c_void) != 0 {
                return 1;
            }
            let prev = record.read();
            if prev <= fp {
                break;
            }
            fp = prev;
            depth += 1;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fork(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cb = ocerz_bridge_callback_frame();
        if !cb.is_null() && sb_translated_frame_below(vm) != 0 {
            let mut why = [0 as c_char; 256];
            libc::snprintf(
                why.as_mut_ptr(),
                why.len(),
                c"was called inside a callback %s made from translated code, and the child would return into a translation it does not inherit; refused".as_ptr(),
                if !(*cb).sym.is_null() { (*cb).sym } else { c"native code".as_ptr() },
            );
            sb_refuse(c"_fork".as_ptr(), why.as_ptr());
        }
        let mut pid = 0;
        let e = ocerz_guest_fork(vm, cpu, &mut pid);
        if e != 0 {
            *errno() = e;
            return sb_ret(vm, cpu, -1);
        }
        sb_ret(vm, cpu, pid as i64)
    }
}

unsafe fn sb_exec(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
) -> c_int {
    unsafe { ocerz_guest_execve(vm, cpu, path, argv, envp) }
}

type SbTry = unsafe extern "C" fn(*mut c_void, *const c_char, *const *mut c_char) -> c_int;

unsafe fn sb_path_search(
    name: *const c_char,
    search: *const c_char,
    argv: *const *mut c_char,
    try_: SbTry,
    ctx: *mut c_void,
) -> c_int {
    unsafe {
        if name.is_null() {
            return libc::EFAULT;
        }
        let mut eacces = 0;
        let mut buf = [0 as c_char; libc::PATH_MAX as usize];
        let mut bp = name;
        let mut cur: *mut c_char = core::ptr::null_mut();
        let mut copy: *mut c_char = core::ptr::null_mut();
        let single = !libc::strchr(name, b'/' as c_int).is_null();
        if !single {
            if *name == 0 {
                return libc::ENOENT;
            }
            copy = libc::strdup(if !search.is_null() { search } else { libc::_PATH_DEFPATH });
            if copy.is_null() {
                return libc::ENOMEM;
            }
            cur = copy;
        }
        let mut err = libc::ENOENT;
        loop {
            if !single {
                let mut p = strsep(&mut cur, c":".as_ptr());
                if p.is_null() {
                    break;
                }
                let lp = if *p != 0 { libc::strlen(p) } else { 1 };
                let ln = libc::strlen(name);
                if *p == 0 {
                    p = c".".as_ptr() as *mut c_char;
                }
                if lp + ln + 2 > buf.len() {
                    err = libc::ENAMETOOLONG;
                    continue;
                }
                core::ptr::copy_nonoverlapping(p, buf.as_mut_ptr(), lp);
                *buf.get_unchecked_mut(lp) = b'/' as c_char;
                core::ptr::copy_nonoverlapping(name, buf.as_mut_ptr().add(lp + 1), ln);
                *buf.get_unchecked_mut(lp + ln + 1) = 0;
                bp = buf.as_ptr();
            }
            err = try_(ctx, bp, argv);
            match err {
                0 | libc::E2BIG | libc::ENOMEM | libc::ETXTBSY => {
                    libc::free(copy as *mut c_void);
                    return err;
                }
                libc::ELOOP | libc::ENAMETOOLONG | libc::ENOENT | libc::ENOTDIR => {}
                libc::ENOEXEC => {
                    let mut cnt = 0usize;
                    while !argv.is_null() && !(*argv.add(cnt)).is_null() {
                        cnt += 1;
                    }
                    let memp = libc::calloc(cnt + 2, size_of::<*mut c_char>()) as *mut *mut c_char;
                    if memp.is_null() {
                        libc::free(copy as *mut c_void);
                        return libc::ENOEXEC;
                    }
                    *memp = c"sh".as_ptr() as *mut c_char;
                    *memp.add(1) = bp as *mut c_char;
                    for k in 1..cnt {
                        *memp.add(k + 1) = *argv.add(k);
                    }
                    err = try_(ctx, libc::_PATH_BSHELL, memp);
                    libc::free(memp as *mut c_void);
                    libc::free(copy as *mut c_void);
                    return err;
                }
                _ => {
                    let mut sb: libc::stat = core::mem::zeroed();
                    if libc::stat(bp, &mut sb) != 0 {
                        // fall through to next path element
                    } else if err == libc::EACCES {
                        eacces = 1;
                    } else {
                        libc::free(copy as *mut c_void);
                        return err;
                    }
                }
            }
            if single {
                break;
            }
        }
        libc::free(copy as *mut c_void);
        if eacces != 0 { libc::EACCES } else { libc::ENOENT }
    }
}

struct SbExecCtx {
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    envp: *const *mut c_char,
}

unsafe extern "C" fn sb_try_exec(ctx: *mut c_void, path: *const c_char, argv: *const *mut c_char) -> c_int {
    unsafe {
        let c = ctx as *mut SbExecCtx;
        sb_exec((*c).vm, (*c).cpu, path, argv, (*c).envp)
    }
}

unsafe fn sb_exec_search(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    name: *const c_char,
    search: *const c_char,
    argv: *const *mut c_char,
) -> c_int {
    unsafe {
        let mut c = SbExecCtx { vm, cpu, envp: environ };
        sb_path_search(name, search, argv, sb_try_exec, &mut c as *mut SbExecCtx as *mut c_void)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execve(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let mut envp: [*mut c_char; SB_ENV_MAX] = [core::ptr::null_mut(); SB_ENV_MAX];
        let gargv = sb_arg(cpu, 1);
        let genv = sb_arg(cpu, 2);
        sb_vector(gargv, argv.as_mut_ptr(), SB_ARGV_MAX as c_int);
        sb_vector(genv, envp.as_mut_ptr(), SB_ENV_MAX as c_int);
        sb_posix(
            vm,
            cpu,
            sb_exec(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                if gargv != 0 { argv.as_mut_ptr() } else { core::ptr::null() },
                if genv != 0 { envp.as_mut_ptr() } else { core::ptr::null() },
            ),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let gargv = sb_arg(cpu, 1);
        sb_vector(gargv, argv.as_mut_ptr(), SB_ARGV_MAX as c_int);
        sb_posix(
            vm,
            cpu,
            sb_exec(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                if gargv != 0 { argv.as_mut_ptr() } else { core::ptr::null() },
                environ,
            ),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execvp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        sb_vector(sb_arg(cpu, 1), argv.as_mut_ptr(), SB_ARGV_MAX as c_int);
        sb_posix(
            vm,
            cpu,
            sb_exec_search(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                libc::getenv(c"PATH".as_ptr()),
                argv.as_mut_ptr(),
            ),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execvP(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        sb_vector(sb_arg(cpu, 2), argv.as_mut_ptr(), SB_ARGV_MAX as c_int);
        let search = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        sb_posix(
            vm,
            cpu,
            sb_exec_search(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                if !search.is_null() { search } else { libc::_PATH_DEFPATH },
                argv.as_mut_ptr(),
            ),
        )
    }
}

unsafe fn sb_list_argv(
    cpu: *const OcerzCPU,
    first: c_int,
    out: *mut *mut c_char,
    cap: c_int,
    after: *mut c_int,
) -> c_int {
    unsafe {
        let mut n = 0;
        let mut i = first;
        loop {
            let p = sb_arg(cpu, i);
            i += 1;
            if p == 0 {
                break;
            }
            if n >= cap - 1 {
                return libc::E2BIG;
            }
            *out.add(n as usize) = ocerz_g2h(p) as *mut c_char;
            n += 1;
        }
        *out.add(n as usize) = core::ptr::null_mut();
        if !after.is_null() {
            *after = i;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execl(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let e = sb_list_argv(cpu, 1, argv.as_mut_ptr(), SB_ARGV_MAX as c_int, core::ptr::null_mut());
        if e != 0 {
            return sb_posix(vm, cpu, e);
        }
        sb_posix(
            vm,
            cpu,
            sb_exec(vm, cpu, sb_ptr(sb_arg(cpu, 0)) as *const c_char, argv.as_mut_ptr(), environ),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execle(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let mut envp: [*mut c_char; SB_ENV_MAX] = [core::ptr::null_mut(); SB_ENV_MAX];
        let mut after = 0;
        let e = sb_list_argv(cpu, 1, argv.as_mut_ptr(), SB_ARGV_MAX as c_int, &mut after);
        if e != 0 {
            return sb_posix(vm, cpu, e);
        }
        let genv = sb_arg(cpu, after);
        sb_vector(genv, envp.as_mut_ptr(), SB_ENV_MAX as c_int);
        sb_posix(
            vm,
            cpu,
            sb_exec(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                argv.as_mut_ptr(),
                if genv != 0 { envp.as_mut_ptr() } else { core::ptr::null() },
            ),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_execlp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let e = sb_list_argv(cpu, 1, argv.as_mut_ptr(), SB_ARGV_MAX as c_int, core::ptr::null_mut());
        if e != 0 {
            return sb_posix(vm, cpu, e);
        }
        sb_posix(
            vm,
            cpu,
            sb_exec_search(
                vm,
                cpu,
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                libc::getenv(c"PATH".as_ptr()),
                argv.as_mut_ptr(),
            ),
        )
    }
}

struct SbSpawnCtx {
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    pid: c_int,
    fa: *const libc::posix_spawn_file_actions_t,
    attr: *const libc::posix_spawnattr_t,
    envp: *const *mut c_char,
}

unsafe extern "C" fn sb_try_spawn(ctx: *mut c_void, path: *const c_char, argv: *const *mut c_char) -> c_int {
    unsafe {
        let c = ctx as *mut SbSpawnCtx;
        ocerz_guest_posix_spawn(
            (*c).vm,
            (*c).cpu,
            &mut (*c).pid,
            path,
            (*c).fa as *const _,
            (*c).attr as *const _,
            argv,
            (*c).envp,
        )
    }
}

unsafe fn sb_spawn(vm: *mut OcerzVM, cpu: *mut OcerzCPU, search: c_int) -> c_int {
    unsafe {
        let mut argv: [*mut c_char; SB_ARGV_MAX] = [core::ptr::null_mut(); SB_ARGV_MAX];
        let mut envp: [*mut c_char; SB_ENV_MAX] = [core::ptr::null_mut(); SB_ENV_MAX];
        let gpid = sb_arg(cpu, 0);
        let gargv = sb_arg(cpu, 4);
        let genv = sb_arg(cpu, 5);
        sb_vector(gargv, argv.as_mut_ptr(), SB_ARGV_MAX as c_int);
        sb_vector(genv, envp.as_mut_ptr(), SB_ENV_MAX as c_int);
        let mut c = SbSpawnCtx {
            vm,
            cpu,
            pid: 0,
            fa: sb_ptr(sb_arg(cpu, 2)) as *const libc::posix_spawn_file_actions_t,
            attr: sb_ptr(sb_arg(cpu, 3)) as *const libc::posix_spawnattr_t,
            envp: if genv != 0 { envp.as_mut_ptr() } else { core::ptr::null() },
        };
        let path = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        let argv_p = if gargv != 0 { argv.as_mut_ptr() } else { core::ptr::null() };
        let e = if search != 0 {
            sb_path_search(path, libc::getenv(c"PATH".as_ptr()), argv_p, sb_try_spawn, &mut c as *mut SbSpawnCtx as *mut c_void)
        } else {
            sb_try_spawn(&mut c as *mut SbSpawnCtx as *mut c_void, path, argv_p)
        };
        if e == 0 && gpid != 0 {
            ocerz_st(gpid, 4, c.pid as u64);
        }
        sb_ret(vm, cpu, e as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_posix_spawn(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_spawn(vm, cpu, 0) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_posix_spawnp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_spawn(vm, cpu, 1) }
}

static mut G_SB_SYSTEM_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_SB_SYSTEM_COUNT: c_int = 0;
static mut G_SB_INTACT: [u8; 16] = [0; 16];
static mut G_SB_QUITACT: [u8; 16] = [0; 16];

unsafe fn sb_ignore(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    sig: c_int,
    scratch: u64,
    saved: *mut u8,
    defaults: *mut libc::sigset_t,
    flags: *mut c_short,
) {
    unsafe {
        let act = scratch;
        let oact = scratch + 16;
        ocerz_st(act, 8, libc::SIG_IGN as u64);
        ocerz_st(act + 8, 4, 0);
        ocerz_st(act + 12, 4, 0);
        ocerz_guest_sigaction_user(vm, cpu, sig, act, oact);
        core::ptr::copy_nonoverlapping(ocerz_g2h(oact) as *const u8, saved, 16);
        if ocerz_ld(oact, 8) != libc::SIG_IGN as u64 {
            libc::sigaddset(&mut *defaults, sig);
            *flags |= libc::POSIX_SPAWN_SETSIGDEF as c_short;
        }
    }
}

unsafe fn sb_restore(vm: *mut OcerzVM, cpu: *mut OcerzCPU, sig: c_int, scratch: u64, saved: *const u8) {
    unsafe {
        core::ptr::copy_nonoverlapping(saved, ocerz_g2h(scratch) as *mut u8, 16);
        ocerz_guest_sigaction_user(vm, cpu, sig, scratch, 0);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_system(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let gcmd = sb_arg(cpu, 0);
        if gcmd == 0 {
            return sb_ret(
                vm,
                cpu,
                if libc::access(libc::_PATH_BSHELL, libc::F_OK) == -1 { 0 } else { 1 },
            );
        }
        let scratch = ((*cpu).gpr[OCERZ_RSP as usize] - 128 - 64) & !0xf;
        let set = scratch + 32;
        let oset = scratch + 48;
        let mut defaults: libc::sigset_t = core::mem::zeroed();
        libc::sigemptyset(&mut defaults);
        let mut flags: c_short = libc::POSIX_SPAWN_SETSIGMASK as c_short;

        libc::pthread_mutex_lock(&raw mut G_SB_SYSTEM_LOCK);
        if { let c = &raw mut G_SB_SYSTEM_COUNT; let n = *c; *c = n + 1; n } == 0 {
            sb_ignore(vm, cpu, libc::SIGINT, scratch, &raw mut G_SB_INTACT as *mut u8, &mut defaults, &mut flags);
            sb_ignore(vm, cpu, libc::SIGQUIT, scratch, &raw mut G_SB_QUITACT as *mut u8, &mut defaults, &mut flags);
        }
        libc::pthread_mutex_unlock(&raw mut G_SB_SYSTEM_LOCK);
        ocerz_st(set, 4, 1u64 << (libc::SIGCHLD - 1));
        ocerz_guest_sigprocmask(vm, cpu, libc::SIG_BLOCK, set, oset);
        let old = ocerz_ld(oset, 4) as libc::sigset_t;

        let mut attr: libc::posix_spawnattr_t = core::mem::zeroed();
        libc::posix_spawnattr_init(&mut attr);
        libc::posix_spawnattr_setsigmask(&mut attr, &old);
        if flags & libc::POSIX_SPAWN_SETSIGDEF as c_short != 0 {
            libc::posix_spawnattr_setsigdefault(&mut attr, &defaults);
        }
        libc::posix_spawnattr_setflags(&mut attr, flags);
        let mut argv: [*mut c_char; 4] = [
            c"sh".as_ptr() as *mut c_char,
            c"-c".as_ptr() as *mut c_char,
            ocerz_g2h(gcmd) as *mut c_char,
            core::ptr::null_mut(),
        ];
        let mut pid = 0;
        let err = ocerz_guest_posix_spawn(
            vm,
            cpu,
            &mut pid,
            libc::_PATH_BSHELL,
            core::ptr::null(),
            &attr as *const _ as *const _,
            argv.as_mut_ptr(),
            environ,
        );
        libc::posix_spawnattr_destroy(&mut attr);

        let pstat;
        if err == 0 {
            let mut w;
            let mut st = 0;
            loop {
                w = libc::wait4(pid, &mut st, 0, core::ptr::null_mut());
                if !(w == -1 && *errno() == libc::EINTR) {
                    break;
                }
            }
            pstat = if w == -1 { -1 } else { st };
        } else if err == libc::ENOMEM || err == libc::EAGAIN {
            pstat = -1;
        } else {
            pstat = w_exitcode(127, 0);
        }
        let saved_errno = *errno();

        libc::pthread_mutex_lock(&raw mut G_SB_SYSTEM_LOCK);
        let now = { let c = &raw mut G_SB_SYSTEM_COUNT; *c -= 1; *c };
        if now == 0 {
            sb_restore(vm, cpu, libc::SIGINT, scratch, &raw const G_SB_INTACT as *const u8);
            sb_restore(vm, cpu, libc::SIGQUIT, scratch, &raw const G_SB_QUITACT as *const u8);
        }
        libc::pthread_mutex_unlock(&raw mut G_SB_SYSTEM_LOCK);
        ocerz_guest_sigprocmask(vm, cpu, libc::SIG_SETMASK, oset, 0);
        *errno() = saved_errno;
        sb_ret(vm, cpu, pstat as i64)
    }
}

#[repr(C)]
struct SbPopen {
    next: *mut SbPopen,
    fp: *mut libc::FILE,
    pid: libc::pid_t,
}

static mut G_SB_POPEN: *mut SbPopen = core::ptr::null_mut();
static mut G_SB_POPEN_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn sb_popen(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    command: *const c_char,
    mut type_: *const c_char,
) -> *mut libc::FILE {
    unsafe {
        let mut twoway = 0;
        if type_.is_null() {
            *errno() = libc::EINVAL;
            return core::ptr::null_mut();
        }
        if !libc::strchr(type_, b'+' as c_int).is_null() {
            twoway = 1;
            type_ = c"r+".as_ptr();
        } else if (*type_ != b'r' as c_char && *type_ != b'w' as c_char) || *type_.add(1) != 0 {
            *errno() = libc::EINVAL;
            return core::ptr::null_mut();
        }
        let cur = libc::malloc(size_of::<SbPopen>()) as *mut SbPopen;
        if cur.is_null() {
            return core::ptr::null_mut();
        }
        let mut pdes = [0 as c_int; 2];
        if (if twoway != 0 {
            libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, pdes.as_mut_ptr())
        } else {
            libc::pipe(pdes.as_mut_ptr())
        }) < 0
        {
            libc::free(cur as *mut c_void);
            return core::ptr::null_mut();
        }
        let iop;
        let other;
        if *type_ == b'r' as c_char {
            iop = libc::fdopen(pdes[0], type_);
            other = pdes[1];
        } else {
            iop = libc::fdopen(pdes[1], type_);
            other = pdes[0];
        }
        if iop.is_null() {
            let e = *errno();
            libc::close(pdes[0]);
            libc::close(pdes[1]);
            libc::free(cur as *mut c_void);
            *errno() = e;
            return core::ptr::null_mut();
        }
        let mut fa: libc::posix_spawn_file_actions_t = core::mem::zeroed();
        let mut err = libc::posix_spawn_file_actions_init(&mut fa);
        if err != 0 {
            libc::fclose(iop);
            libc::close(other);
            libc::free(cur as *mut c_void);
            *errno() = err;
            return core::ptr::null_mut();
        }
        if *type_ == b'r' as c_char {
            if pdes[1] != libc::STDOUT_FILENO {
                libc::posix_spawn_file_actions_adddup2(&mut fa, pdes[1], libc::STDOUT_FILENO);
                libc::posix_spawn_file_actions_addclose(&mut fa, pdes[1]);
                if twoway != 0 {
                    libc::posix_spawn_file_actions_adddup2(&mut fa, libc::STDOUT_FILENO, libc::STDIN_FILENO);
                }
            } else if twoway != 0 && pdes[1] != libc::STDIN_FILENO {
                libc::posix_spawn_file_actions_adddup2(&mut fa, pdes[1], libc::STDIN_FILENO);
            }
            libc::posix_spawn_file_actions_addclose(&mut fa, pdes[0]);
        } else {
            if pdes[0] != libc::STDIN_FILENO {
                libc::posix_spawn_file_actions_adddup2(&mut fa, pdes[0], libc::STDIN_FILENO);
                libc::posix_spawn_file_actions_addclose(&mut fa, pdes[0]);
            }
            libc::posix_spawn_file_actions_addclose(&mut fa, pdes[1]);
        }
        libc::pthread_mutex_lock(&raw mut G_SB_POPEN_LOCK);
        let mut p = G_SB_POPEN;
        while !p.is_null() {
            libc::posix_spawn_file_actions_addclose(&mut fa, libc::fileno((*p).fp));
            p = (*p).next;
        }
        libc::pthread_mutex_unlock(&raw mut G_SB_POPEN_LOCK);

        let mut argv: [*mut c_char; 4] = [
            c"sh".as_ptr() as *mut c_char,
            c"-c".as_ptr() as *mut c_char,
            command as *mut c_char,
            core::ptr::null_mut(),
        ];
        let mut pid = 0;
        err = ocerz_guest_posix_spawn(
            vm,
            cpu,
            &mut pid,
            libc::_PATH_BSHELL,
            &fa as *const _ as *const _,
            core::ptr::null(),
            argv.as_mut_ptr(),
            environ,
        );
        libc::posix_spawn_file_actions_destroy(&mut fa);
        if err == libc::ENOMEM || err == libc::EAGAIN {
            libc::fclose(iop);
            libc::close(other);
            libc::free(cur as *mut c_void);
            *errno() = err;
            return core::ptr::null_mut();
        } else if err != 0 {
            pid = -1;
        }
        if *type_ == b'r' as c_char {
            libc::close(pdes[1]);
        } else {
            libc::close(pdes[0]);
        }
        (*cur).fp = iop;
        (*cur).pid = pid;
        libc::pthread_mutex_lock(&raw mut G_SB_POPEN_LOCK);
        (*cur).next = G_SB_POPEN;
        G_SB_POPEN = cur;
        libc::pthread_mutex_unlock(&raw mut G_SB_POPEN_LOCK);
        fwide(iop, -1);
        iop
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_popen(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let iop = sb_popen(
            vm,
            cpu,
            sb_ptr(sb_arg(cpu, 0)) as *const c_char,
            sb_ptr(sb_arg(cpu, 1)) as *const c_char,
        );
        sb_ret(vm, cpu, if !iop.is_null() { ocerz_h2g(iop as *const c_void) as i64 } else { 0 })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pclose(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let iop = sb_ptr(sb_arg(cpu, 0)) as *mut libc::FILE;
        let mut link: *mut *mut SbPopen = &raw mut G_SB_POPEN;
        let mut cur;
        libc::pthread_mutex_lock(&raw mut G_SB_POPEN_LOCK);
        loop {
            cur = *link;
            if cur.is_null() || (*cur).fp == iop {
                break;
            }
            link = &mut (*cur).next;
        }
        if !cur.is_null() {
            *link = (*cur).next;
        }
        libc::pthread_mutex_unlock(&raw mut G_SB_POPEN_LOCK);
        if cur.is_null() {
            return sb_ret(vm, cpu, -1);
        }
        libc::fclose(iop);
        if (*cur).pid < 0 {
            libc::free(cur as *mut c_void);
            return sb_ret(vm, cpu, w_exitcode(127, 0) as i64);
        }
        let mut pstat = 0;
        let mut pid;
        loop {
            pid = libc::wait4((*cur).pid, &mut pstat, 0, core::ptr::null_mut());
            if !(pid == -1 && *errno() == libc::EINTR) {
                break;
            }
        }
        libc::free(cur as *mut c_void);
        sb_ret(vm, cpu, if pid == -1 { -1 } else { pstat as i64 })
    }
}
