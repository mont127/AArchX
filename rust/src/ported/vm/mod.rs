//! The run loop, the crash-containment machinery around it, and the
//! thread/signal plumbing.
//!
//! vm.c is the biggest file in the tree because it is where all the
//! process-wide machinery meets: the interpreter/JIT run loop with its
//! setjmp recovery frame, the SIGSEGV/SIGBUS handler that turns host faults
//! into guest state, the thread suspend/resume protocol used by the
//! syscall layer, and the deep diagnostic dumps (threaddump, portdump,
//! ripdump) that make a wedged guest debuggable.
//!
//! The guest call path pushes a sentinel return address onto a fresh guest
//! stack frame and runs the interpreter until it comes back; signal handling
//! converts host faults into either guest signal delivery (Wine TEB aware),
//! JIT fault recovery (hotpatch, RAS overflow, cache patching), or a fatal
//! crash dump.  Everything diagnostic writes with fprintf/str_into/hex_into
//! so it stays async-signal-safe enough.
//!
//! In Rust the two sigsetjmp sites go through a module-private global_asm
//! trampoline (ocerz_vm_setjmp_run): the post-setjmp code is a body function
//! taking a #[repr(C)] context struct in the outer frame, and siglongjmp
//! lands back inside the trampoline, which returns the body's result.  No
//! value with a destructor is live across the jump.  macOS arm64 ucontext /
//! mcontext64 are declared repr(C) here with const layout asserts; libc has
//! no bindings for them.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]

use core::ffi::{c_char, c_int, c_uint, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering, fence};

use crate::ffi;
use crate::ffi::{Ocerz128, OcerzCPU, OcerzJitFaultInfo, OcerzVM, sigjmp_buf};
use crate::ported::globals::ocerz_mode;
use crate::{ocerz_fatal, ocerz_log};

use libc::{
    CLOCK_UPTIME_RAW, MAP_ANON, MAP_PRIVATE, PROT_READ, PROT_WRITE, SA_NODEFER, SA_ONSTACK,
    SA_RESTART, SA_SIGINFO, SIG_BLOCK, SIG_SETMASK, SIGBUS, SIGEMT, SIGILL, SIGINFO, SIGPROF,
    SIGQUIT, SIGSEGV, SIGSTKSZ, SIGSYS, SIGTRAP, SIGUSR1, SIGUSR2, pthread_equal,
    pthread_mach_thread_np, pthread_self, pthread_sigmask, pthread_threadid_np, sigaction,
    sigaltstack, sigemptyset, sigfillset, sigismember, sigpending, stack_t, sysconf, write,
};

pub(super) mod prof;
pub(super) mod run;
pub(super) mod sig;

#[allow(non_camel_case_types)]
pub(super) type sigset_t = libc::sigset_t;
pub(super) use libc::siginfo_t;

#[repr(C)]
pub(super) struct exception_state64 {
    pub far: u64,
    pub esr: u32,
    pub exception: u32,
}

#[repr(C)]
pub(super) struct thread_state64 {
    pub x: [u64; 29],
    pub fp: u64,
    pub lr: u64,
    pub sp: u64,
    pub pc: u64,
    pub cpsr: u32,
    pub pad: u32,
}

#[repr(C)]
#[repr(align(16))]
pub(super) struct neon_state64 {
    pub v: [u128; 32],
    pub fpsr: u32,
    pub fpcr: u32,
}

#[repr(C)]
pub(super) struct mcontext64 {
    pub es: exception_state64,
    pub ss: thread_state64,
    pub ns: neon_state64,
}

#[repr(C)]
pub(super) struct ucontext_t {
    pub uc_onstack: c_int,
    pub uc_sigmask: u32,
    pub uc_stack: stack_t,
    pub uc_link: *mut ucontext_t,
    pub uc_mcsize: usize,
    pub uc_mcontext: *mut mcontext64,
}

const _: () = assert!(core::mem::size_of::<ucontext_t>() == 56);
const _: () = assert!(core::mem::offset_of!(ucontext_t, uc_mcsize) == 40);
const _: () = assert!(core::mem::offset_of!(ucontext_t, uc_mcontext) == 48);
const _: () = assert!(core::mem::size_of::<mcontext64>() == 816);
const _: () = assert!(core::mem::offset_of!(mcontext64, es) == 0);
const _: () = assert!(core::mem::offset_of!(mcontext64, ss) == 16);
const _: () = assert!(core::mem::offset_of!(mcontext64, ns) == 288);
const _: () = assert!(core::mem::size_of::<thread_state64>() == 272);
const _: () = assert!(core::mem::offset_of!(thread_state64, sp) == 248);
const _: () = assert!(core::mem::offset_of!(thread_state64, pc) == 256);
const _: () = assert!(core::mem::offset_of!(thread_state64, cpsr) == 264);
const _: () = assert!(core::mem::offset_of!(exception_state64, esr) == 8);
const _: () = assert!(core::mem::offset_of!(neon_state64, fpsr) == 512);
const _: () = assert!(core::mem::size_of::<sigjmp_buf>() == 196);
const _: () = assert!(core::mem::size_of::<siginfo_t>() == 104);

#[allow(non_camel_case_types)]
pub(super) type arm_thread_state64_t = thread_state64;
#[allow(non_camel_case_types)]
pub(super) type thread_state_t = *mut c_void;
#[allow(non_camel_case_types)]
pub(super) type thread_act_t = u32;
#[allow(non_camel_case_types)]
pub(super) type thread_inspect_t = u32;
#[allow(non_camel_case_types)]
pub(super) type kern_return_t = i32;
#[allow(non_camel_case_types)]
pub(super) type mach_port_t = u32;
#[allow(non_camel_case_types)]
pub(super) type mach_msg_type_number_t = u32;
#[allow(non_camel_case_types)]
pub(super) type mach_vm_address_t = u64;
#[allow(non_camel_case_types)]
pub(super) type mach_vm_offset_t = u64;
#[allow(non_camel_case_types)]
pub(super) type mach_vm_size_t = u64;
#[allow(non_camel_case_types)]
pub(super) type vm_address_t = usize;
#[allow(non_camel_case_types)]
pub(super) type vm_size_t = usize;
#[allow(non_camel_case_types)]
pub(super) type vm_offset_t = usize;
#[allow(non_camel_case_types)]
pub(super) type vm_region_t = u32;
#[allow(non_camel_case_types)]
pub(super) type vm_region_info_t = *mut c_int;
#[allow(non_camel_case_types)]
pub(super) type vm_region_flavor_t = i32;
#[allow(non_camel_case_types)]
pub(super) type thread_info_t = *mut c_int;
#[allow(non_camel_case_types)]
pub(super) type thread_flavor_t = i32;
#[allow(non_camel_case_types)]
pub(super) type mach_port_name_t = u32;
#[allow(non_camel_case_types)]
pub(super) type mach_port_name_array_t = *mut mach_port_name_t;
#[allow(non_camel_case_types)]
pub(super) type mach_port_type_t = u32;
#[allow(non_camel_case_types)]
pub(super) type mach_port_type_array_t = *mut mach_port_type_t;
#[allow(non_camel_case_types)]
pub(super) type mach_port_info_t = *mut c_int;
#[allow(non_camel_case_types)]
pub(super) type mach_port_flavor_t = i32;
#[allow(non_camel_case_types)]
pub(super) type mach_port_seqno_t = u64;
#[allow(non_camel_case_types)]
pub(super) type mach_msg_size_t = u32;
#[allow(non_camel_case_types)]
pub(super) type mach_msg_id_t = i32;
#[allow(non_camel_case_types)]
pub(super) type mach_msg_trailer_info_t = *mut c_char;
#[allow(non_camel_case_types)]
pub(super) type exception_mask_t = u32;

pub(super) const KERN_SUCCESS: kern_return_t = 0;
pub(super) const MACH_PORT_NULL: mach_port_t = 0;
pub(super) const ARM_THREAD_STATE64: thread_flavor_t = 6;
pub(super) const ARM_THREAD_STATE64_COUNT: mach_msg_type_number_t = 68;
pub(super) const THREAD_BASIC_INFO: thread_flavor_t = 3;
pub(super) const THREAD_BASIC_INFO_COUNT: mach_msg_type_number_t = 11;
pub(super) const TH_STATE_RUNNING: i32 = 1;
pub(super) const VM_REGION_BASIC_INFO_64: vm_region_flavor_t = 9;
pub(super) const VM_REGION_BASIC_INFO_COUNT_64: mach_msg_type_number_t = 9;
pub(super) const VM_PROT_READ: u32 = 1;
pub(super) const VM_PROT_WRITE: u32 = 2;
pub(super) const VM_PROT_EXECUTE: u32 = 4;
pub(super) const MACH_PORT_RECEIVE_STATUS: i32 = 1;
pub(super) const MACH_PORT_RECEIVE_STATUS_COUNT: mach_msg_type_number_t = 10;
pub(super) const MACH_PORT_TYPE_RECEIVE: u32 = 1 << 17;
pub(super) const MACH_PORT_TYPE_PORT_SET: u32 = 1 << 3;
pub(super) const MACH_RCV_TRAILER_NULL: u32 = 0;
pub(super) const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
pub(super) const LC_SEGMENT_64: u32 = 0x19;

#[repr(C)]
pub(super) struct thread_time_value {
    pub seconds: c_int,
    pub microseconds: c_int,
}

#[repr(C)]
pub(super) struct thread_basic_info_data {
    pub user_time: thread_time_value,
    pub system_time: thread_time_value,
    pub cpu_usage: c_int,
    pub policy: c_int,
    pub run_state: c_int,
    pub flags: c_int,
    pub suspend_count: c_int,
    pub sleep_time: c_int,
}

const _: () = assert!(core::mem::size_of::<thread_basic_info_data>() == 40);

#[repr(C)]
pub(super) struct mach_port_status {
    pub mps_pset: mach_port_t,
    pub mps_seqno: mach_port_seqno_t,
    pub mps_mscount: u32,
    pub mps_qlimit: u32,
    pub mps_msgcount: u32,
    pub mps_sorights: u32,
    pub mps_srights: c_int,
    pub mps_pdrequest: c_int,
    pub mps_nsrequest: c_int,
    pub mps_flags: c_int,
}

#[repr(C)]
pub(super) struct vm_region_basic_info_data_64 {
    pub protection: u32,
    pub max_protection: u32,
    pub inheritance: u32,
    pub shared: c_int,
    pub reserved: c_int,
    pub offset: u64,
    pub behavior: c_int,
    pub user_wired_count: u16,
}

#[repr(C)]
pub(super) struct mach_header_64 {
    pub magic: u32,
    pub cputype: i32,
    pub cpusubtype: i32,
    pub filetype: u32,
    pub ncmds: u32,
    pub sizeofcmds: u32,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
pub(super) struct load_command {
    pub cmd: u32,
    pub cmdsize: u32,
}

#[repr(C)]
pub(super) struct segment_command_64 {
    pub cmd: u32,
    pub cmdsize: u32,
    pub segname: [c_char; 16],
    pub vmaddr: u64,
    pub vmsize: u64,
    pub fileoff: u64,
    pub filesize: u64,
    pub maxprot: i32,
    pub initprot: i32,
    pub nsects: u32,
    pub flags: u32,
}

#[repr(C)]
pub(super) struct dl_info {
    pub dli_fname: *const c_char,
    pub dli_fbase: *mut c_void,
    pub dli_sname: *const c_char,
    pub dli_saddr: *mut c_void,
}
pub(super) type Dl_info = dl_info;

unsafe extern "C" {
    pub fn siglongjmp(env: *mut sigjmp_buf, val: c_int) -> !;
    pub(super) static mach_task_self_: mach_port_t;
    pub(super) fn thread_suspend(target_act: thread_act_t) -> kern_return_t;
    pub(super) fn thread_resume(target_act: thread_act_t) -> kern_return_t;
    pub(super) fn thread_get_state(
        target_act: thread_act_t,
        flavor: thread_flavor_t,
        old_state: thread_state_t,
        old_state_cnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn thread_info(
        target_act: thread_inspect_t,
        flavor: thread_flavor_t,
        thread_info_out: thread_info_t,
        thread_info_outCnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn mach_vm_read_overwrite(
        target_task: vm_map_t,
        address: mach_vm_address_t,
        size: mach_vm_size_t,
        data: mach_vm_address_t,
        outsize: *mut mach_vm_size_t,
    ) -> kern_return_t;
    pub(super) fn mach_vm_region(
        target_task: vm_map_t,
        address: *mut mach_vm_address_t,
        size: *mut mach_vm_size_t,
        flavor: vm_region_flavor_t,
        info: vm_region_info_t,
        infoCnt: *mut mach_msg_type_number_t,
        object_name: *mut mach_port_t,
    ) -> kern_return_t;
    pub(super) fn vm_deallocate(
        target_task: vm_map_t,
        address: vm_address_t,
        size: vm_size_t,
    ) -> kern_return_t;
    pub(super) fn mach_port_names(
        target: vm_map_t,
        names: *mut mach_port_name_array_t,
        ncnt: *mut mach_msg_type_number_t,
        types: *mut mach_port_type_array_t,
        tcnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn mach_port_get_attributes(
        task: vm_map_t,
        name: mach_port_name_t,
        flavor: mach_port_flavor_t,
        port_info_out: mach_port_info_t,
        port_info_outCnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn mach_port_peek(
        task: vm_map_t,
        name: mach_port_name_t,
        trailer_type: u32,
        trailer_seqno: *mut mach_port_seqno_t,
        msg_sizep: *mut mach_msg_size_t,
        msg_idp: *mut mach_msg_id_t,
        trailer_infop: mach_msg_trailer_info_t,
        trailer_infopCnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn mach_port_deallocate(task: vm_map_t, name: mach_port_name_t) -> kern_return_t;
    pub(super) fn mach_vm_deallocate(
        target_task: vm_map_t,
        address: mach_vm_address_t,
        size: mach_vm_size_t,
    ) -> kern_return_t;
    pub(super) fn mach_port_get_set_status(
        task: vm_map_t,
        name: mach_port_name_t,
        members: *mut mach_port_name_array_t,
        membersCnt: *mut mach_msg_type_number_t,
    ) -> kern_return_t;
    pub(super) fn dladdr(addr: *const c_void, info: *mut Dl_info) -> c_int;
    pub(super) fn malloc_zone_malloc(zone: *mut c_void, size: usize) -> *mut c_void;
    pub(super) fn _dyld_get_image_header(image_index: u32) -> *const mach_header_64;
    pub(super) fn _dyld_get_image_vmaddr_slide(image_index: u32) -> isize;
    pub(super) fn sysctlbyname(
        name: *const c_char,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *const c_void,
        newlen: usize,
    ) -> c_int;

    static ocerz_leaf_lo: c_char;
    static ocerz_leaf_hi: c_char;
    static ocerz_leaf_near_lo: u64;
    static ocerz_leaf_near_hi: u64;
    pub(super) fn clock_gettime_nsec_np(clock_id: u32) -> u64;
    fn ocerz_host_region_is_device(addr: u64, prot: *mut c_int) -> c_int;
    fn ocerz_alias_raw_region(vm: *mut OcerzVM, gaddr: u64) -> c_int;
    fn ocerz_pe_stack_dump(cpu: *mut OcerzCPU, tag: *const c_char);
    fn ocerz_tl60_now() -> u64;

    #[thread_local]
    pub(super) static mut ocerz_jit_exec_state: c_int;
    #[thread_local]
    pub(super) static mut ocerz_jit_decode_recover: *mut sigjmp_buf;
    pub(super) static mut ocerz_cftrap_on: c_int;
    pub(super) static mut ocerz_jit_time_xlat: c_int;
    pub(super) static mut ocerz_jit_xlat_ns: u64;
    pub(super) static mut ocerz_jit_retire_ns: u64;
}

#[allow(non_camel_case_types)]
pub(super) type vm_map_t = mach_port_t;

#[inline(always)]
pub(super) unsafe fn mach_task_self() -> mach_port_t {
    unsafe { mach_task_self_ }
}

#[inline(always)]
pub(super) unsafe fn arm_thread_state64_get_pc(st: &thread_state64) -> u64 {
    st.pc
}
#[inline(always)]
pub(super) unsafe fn arm_thread_state64_get_lr(st: &thread_state64) -> u64 {
    st.lr
}
#[inline(always)]
pub(super) unsafe fn arm_thread_state64_get_fp(st: &thread_state64) -> u64 {
    st.fp
}
#[inline(always)]
pub(super) unsafe fn arm_thread_state64_get_sp(st: &thread_state64) -> u64 {
    st.sp
}

#[inline(always)]
pub(super) unsafe fn env_set(name: &[u8]) -> bool {
    unsafe { !libc::getenv(name.as_ptr() as *const c_char).is_null() }
}

pub(super) const OCERZ_CALL_SENTINEL: u64 = 0x00000000deadca11;
pub(super) const OCERZ_MAX_CPUS: usize = 512;
pub(super) const OCERZ_SIG_MAX_REPEAT: u32 = 16;
pub(super) const SUSP_NATIVE_TRIES: u32 = 50000;
pub(super) const SUSP_FRAMES: usize = 12;
pub(super) const GP_SLOTS: usize = 1 << 16;

macro_rules! env_cache {
    ($name:literal) => {{
        static SET_: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let v = SET_.load(Ordering::Relaxed);
        if v < 0 {
            let s =
                unsafe { !libc::getenv(concat!($name, "\0").as_ptr() as *const c_char).is_null() }
                    as i32;
            SET_.store(s, Ordering::Relaxed);
            s
        } else {
            v
        }
    }};
}
pub(super) use env_cache;

pub(super) static mut G_VM: *mut OcerzVM = ptr::null_mut();
#[thread_local]
pub(super) static mut G_CUR_CPU: *mut OcerzCPU = ptr::null_mut();

pub(super) static mut G_IMAGE_SLIDE: u64 = 0;
pub(super) static mut G_CPUS: [*mut OcerzCPU; OCERZ_MAX_CPUS] = [ptr::null_mut(); OCERZ_MAX_CPUS];
pub(super) static mut G_CPU_THREADS: [libc::pthread_t; OCERZ_MAX_CPUS] = [0; OCERZ_MAX_CPUS];
pub(super) static mut G_CPUS_N: c_int = 0;
pub(super) static mut G_CPUS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

#[unsafe(no_mangle)]
pub static mut ocerz_exc_trap_rip: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_cxa_throw_rip: u64 = 0;
pub(super) static mut G_BTRACE_REQ: c_int = 0;
pub(super) static mut G_BTRACE_ON: c_int = -1;
pub(super) static mut G_FORK_SURVIVING_CPU: *mut OcerzCPU = ptr::null_mut();
pub(super) static G_UNSTICK_STARTED: AtomicI32 = AtomicI32::new(0);
pub(super) static mut G_SUSP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub(super) static mut G_SUSP_CV: libc::pthread_cond_t = libc::PTHREAD_COND_INITIALIZER;
#[thread_local]
pub(super) static mut G_PENDING_ASYNC_MASK_TLS: u32 = 0;

#[thread_local]
pub(super) static mut G_SIG_RECOVER: *mut sigjmp_buf = ptr::null_mut();
#[thread_local]
pub(super) static mut T_JIT_ESCAPE_R: c_int = 0;

#[repr(C)]
pub(super) struct RecovEnt {
    pub kind: u8,
    pub rip: u64,
    pub icount: u64,
}
#[repr(C)]
pub(super) struct RecovRing {
    pub n: u32,
    pub e: [RecovEnt; 16],
}
#[thread_local]
pub(super) static mut G_RECOV_RING: RecovRing = RecovRing {
    n: 0,
    e: [const {
        RecovEnt {
            kind: 0,
            rip: 0,
            icount: 0,
        }
    }; 16],
};
pub(super) static mut G_RECOV_NAMES: [*const c_char; 8] = [
    c"?".as_ptr(),
    c"ras-overflow".as_ptr(),
    c"align-interp".as_ptr(),
    c"worker-term".as_ptr(),
    c"commpage-interp".as_ptr(),
    c"sig-deliver".as_ptr(),
    c"wild-term".as_ptr(),
    c"cache-patch".as_ptr(),
];

#[thread_local]
pub(super) static mut G_RIPHIST: [u64; 32] = [0; 32];
#[thread_local]
pub(super) static mut G_RIPHIST_N: u32 = 0;
pub(super) static mut G_CRASH_STACK: c_int = 0;
pub(super) static mut G_FORK_KEEPJIT: c_int = -1;
pub(super) static mut G_SIGTRACE: c_int = 0;
pub(super) static mut G_WINEFAULTLOG: c_int = 0;
pub(super) static mut G_ASYNC_SHARED_ONLY: c_int = 0;
pub(super) static mut G_MALLOC_LO: u64 = 0;
pub(super) static mut G_MALLOC_HI: u64 = 0;
pub(super) static mut G_MALLOC_RANGE_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

#[unsafe(no_mangle)]
pub static mut ocerz_watch_addr: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_watch_len: u64 = 8;
#[unsafe(no_mangle)]
pub static mut ocerz_watch_val: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_watch_shadow: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_exc_trap: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_init_tolerant: c_int = 0;
pub(super) static mut ocerz_sel_trap: u64 = 0;
pub(super) static mut ocerz_ctx_trap: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_bt_lo: u64 = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_bt_hi: u64 = 0;
pub(super) static mut ocerz_bt_done: c_int = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_arg_trap: u64 = 0;

#[repr(C)]
pub(super) struct HsigEnt {
    pub t: u64,
    pub tid: u64,
    pub cpu: *mut OcerzCPU,
    pub sig: c_int,
    pub pid_from: c_int,
}
pub(super) static mut G_HSIG_RING: [HsigEnt; 512] = [const {
    HsigEnt {
        t: 0,
        tid: 0,
        cpu: ptr::null_mut(),
        sig: 0,
        pid_from: 0,
    }
}; 512];
pub(super) static G_HSIG_N: AtomicU32 = AtomicU32::new(0);

use ffi::{
    ocerz_addr_committed, ocerz_addr_prot, ocerz_addr_readable, ocerz_arena_hi, ocerz_arena_lo,
    ocerz_commpage, ocerz_dyld_name_for_addr, ocerz_guest_base, ocerz_jit_lock_held_self,
    ocerz_low_base, ocerz_pin_map, ocerz_top_base,
};
pub(super) use ffi::{
    ocerz_addr_committed as f_ocerz_addr_committed, ocerz_addr_prot as f_ocerz_addr_prot,
    ocerz_addr_readable as f_ocerz_addr_readable, ocerz_commit_fault_page, ocerz_host_region_prot,
    ocerz_unmap,
};

pub(super) const OCERZ_RSP: usize = 4;
pub(super) const OCERZ_RAX: usize = 0;
pub(super) const OCERZ_RCX: usize = 1;
pub(super) const OCERZ_RDX: usize = 2;
pub(super) const OCERZ_RBX: usize = 3;
pub(super) const OCERZ_RBP: usize = 5;
pub(super) const OCERZ_RSI: usize = 6;
pub(super) const OCERZ_RDI: usize = 7;
pub(super) const OCERZ_R8: usize = 8;
pub(super) const OCERZ_R9: usize = 9;
pub(super) const OCERZ_R10: usize = 10;
pub(super) const OCERZ_R11: usize = 11;
pub(super) const OCERZ_R12: usize = 12;
pub(super) const OCERZ_R13: usize = 13;
pub(super) const OCERZ_R14: usize = 14;
pub(super) const OCERZ_R15: usize = 15;

#[inline(always)]
#[inline(always)]
pub(super) unsafe fn ocerz_leaf_site(pc: u64, lr: u64) -> u64 {
    unsafe {
        let at = pc & 0x0000ffffffffffff;
        if (at >= &raw const ocerz_leaf_lo as u64 && at < &raw const ocerz_leaf_hi as u64)
            || (at
                >= AtomicU64::from_ptr((&raw const ocerz_leaf_near_lo) as *mut u64)
                    .load(Ordering::Acquire)
                && at
                    < AtomicU64::from_ptr((&raw const ocerz_leaf_near_hi) as *mut u64)
                        .load(Ordering::Acquire))
        {
            return (lr & 0x0000ffffffffffff).wrapping_sub(4);
        }
        pc
    }
}

pub(super) unsafe fn ocerz_pinned_page(gaddr: u64) -> c_int {
    unsafe {
        let pm = ocerz_pin_map;
        if !pm.is_null()
            && gaddr < ffi::OCERZ_LOW_LIMIT
            && ((*pm.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
        {
            1
        } else {
            0
        }
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_g2h(gaddr: u64) -> *mut c_void {
    unsafe {
        let cp = ocerz_commpage;
        if !cp.is_null() && gaddr >= ffi::OCERZ_COMMPAGE_LO && gaddr < ffi::OCERZ_COMMPAGE_HI {
            return cp.add((gaddr - ffi::OCERZ_COMMPAGE_LO) as usize) as *mut c_void;
        }
        if ocerz_low_base != 0 {
            if gaddr < ffi::OCERZ_LOW_LIMIT {
                if (gaddr < ffi::OCERZ_NULL_LIMIT as u64 && !ocerz_pin_map.is_null())
                    || ocerz_pinned_page(gaddr) != 0
                {
                    return gaddr as *mut c_void;
                }
                return gaddr.wrapping_add(ocerz_low_base) as *mut c_void;
            }
            if gaddr.wrapping_sub(ffi::OCERZ_TOP_LO) < ffi::OCERZ_TOP_HI - ffi::OCERZ_TOP_LO {
                return gaddr
                    .wrapping_sub(ffi::OCERZ_TOP_LO)
                    .wrapping_add(ocerz_top_base) as *mut c_void;
            }
        }
        gaddr.wrapping_add(ocerz_guest_base) as *mut c_void
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_h2g(haddr: *const c_void) -> u64 {
    unsafe {
        let h = haddr as u64;
        if ocerz_low_base != 0 {
            if h.wrapping_sub(ocerz_low_base) < ffi::OCERZ_LOW_LIMIT {
                return h.wrapping_sub(ocerz_low_base);
            }
            if h.wrapping_sub(ocerz_top_base) < ffi::OCERZ_TOP_HI - ffi::OCERZ_TOP_LO {
                return h.wrapping_sub(ocerz_top_base) + ffi::OCERZ_TOP_LO;
            }
        }
        h.wrapping_sub(ocerz_guest_base)
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_host_in_guest_space(haddr: *const c_void) -> c_int {
    unsafe {
        let h = haddr as u64;
        let cp = ocerz_commpage;
        if !cp.is_null() {
            let c = cp as u64;
            if h.wrapping_sub(c) < ffi::OCERZ_COMMPAGE_HI - ffi::OCERZ_COMMPAGE_LO {
                return 1;
            }
            if ocerz_guest_base == 0
                && h.wrapping_sub(ffi::OCERZ_COMMPAGE_LO)
                    < ffi::OCERZ_COMMPAGE_HI - ffi::OCERZ_COMMPAGE_LO
            {
                return 1;
            }
        }
        if ocerz_low_base != 0 {
            if h.wrapping_sub(ocerz_low_base) < ffi::OCERZ_LOW_LIMIT {
                return 1;
            }
            if h.wrapping_sub(ocerz_top_base) < ffi::OCERZ_TOP_HI - ffi::OCERZ_TOP_LO {
                return 1;
            }
            if h.wrapping_sub(ffi::OCERZ_TOP_LO) < ffi::OCERZ_COMMPAGE_HI - ffi::OCERZ_TOP_LO {
                return 1;
            }
        }
        (h.wrapping_sub(ocerz_guest_base) < ocerz_arena_hi) as c_int
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        match size {
            1 => {
                return core::sync::atomic::AtomicU8::from_ptr(p as *mut u8).load(Ordering::Acquire)
                    as u64;
            }
            2 => {
                if (p as usize & 1) == 0 {
                    return core::sync::atomic::AtomicU16::from_ptr(p as *mut u16)
                        .load(Ordering::Acquire) as u64;
                }
            }
            4 => {
                if (p as usize & 3) == 0 {
                    return AtomicU32::from_ptr(p as *mut u32).load(Ordering::Acquire) as u64;
                }
            }
            8 => {
                if (p as usize & 7) == 0 {
                    return AtomicU64::from_ptr(p as *mut u64).load(Ordering::Acquire);
                }
            }
            _ => {}
        }
        let mut v: u64 = 0;
        ptr::copy_nonoverlapping(p as *const u8, &mut v as *mut u64 as *mut u8, size as usize);
        fence(Ordering::Acquire);
        v
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_st(gaddr: u64, size: c_int, v: u64) {
    unsafe {
        if ocerz_watch_addr != 0
            && gaddr < ocerz_watch_addr + ocerz_watch_len
            && gaddr + size as u64 > ocerz_watch_addr
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_val != 0 && v == ocerz_watch_val {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_shadow != 0
            && ocerz_low_base != 0
            && size == 8
            && v.wrapping_sub(ocerz_low_base) < ffi::OCERZ_LOW_LIMIT
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        let p = ocerz_g2h(gaddr);
        match size {
            1 => {
                core::sync::atomic::AtomicU8::from_ptr(p as *mut u8)
                    .store(v as u8, Ordering::Release);
                return;
            }
            2 => {
                if (p as usize & 1) == 0 {
                    core::sync::atomic::AtomicU16::from_ptr(p as *mut u16)
                        .store(v as u16, Ordering::Release);
                    return;
                }
            }
            4 => {
                if (p as usize & 3) == 0 {
                    AtomicU32::from_ptr(p as *mut u32).store(v as u32, Ordering::Release);
                    return;
                }
            }
            8 => {
                if (p as usize & 7) == 0 {
                    AtomicU64::from_ptr(p as *mut u64).store(v, Ordering::Release);
                    return;
                }
            }
            _ => {}
        }
        ptr::copy_nonoverlapping(&v as *const u64 as *const u8, p as *mut u8, size as usize);
        fence(Ordering::Release);
    }
}

#[inline(always)]
pub(super) unsafe fn host_addr_is_guest_page(h: *const c_void) -> c_int {
    unsafe {
        (ocerz_host_in_guest_space(h) != 0 || ocerz_addr_committed(ocerz_h2g(h)) == 1) as c_int
    }
}

#[inline(always)]
pub(super) unsafe fn stderr() -> *mut libc::FILE {
    crate::log::stderr()
}

use core::sync::atomic::AtomicPtr;

pub(super) unsafe fn ocerz_cpu_register(cpu: *mut OcerzCPU) {
    unsafe {
        (*cpu).slow_op = 0;
        if G_BTRACE_ON < 0 {
            G_BTRACE_ON = (!libc::getenv(c"OCERZ_BTRACE".as_ptr()).is_null()) as c_int;
        }
        if (*cpu).btrace.is_null() && G_BTRACE_ON > 0 {
            let n: u32 = 1 << 16;
            (*cpu).btrace = libc::calloc(n as usize, 8) as *mut u64;
            if !(*cpu).btrace.is_null() {
                (*cpu).btrace_n = 0;
                AtomicU32::from_ptr(&raw mut (*cpu).btrace_mask).store(n - 1, Ordering::Release);
            }
        }
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let mut i = 0;
        while i < G_CPUS_N {
            if G_CPUS[i as usize] == cpu {
                static DUPS: AtomicU32 = AtomicU32::new(0);
                if !libc::getenv(c"OCERZ_CPUREG_LOG".as_ptr()).is_null() {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: CPUREG dup #%u cpu=%p (would have dangled)\n".as_ptr(),
                        DUPS.fetch_add(1, Ordering::SeqCst).wrapping_add(1),
                        cpu as *const c_void,
                    );
                }
                libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
                return;
            }
            i += 1;
        }
        if G_CPUS_N < OCERZ_MAX_CPUS as c_int {
            G_CPUS[G_CPUS_N as usize] = cpu;
            G_CPU_THREADS[G_CPUS_N as usize] = pthread_self();
            G_CPUS_N += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        prof::guestprof_start((*cpu).vm);
    }
}

pub(super) unsafe fn ocerz_cpu_unregister(cpu: *mut OcerzCPU) {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let mut i = 0;
        while i < G_CPUS_N {
            if G_CPUS[i as usize] == cpu {
                G_CPUS_N -= 1;
                let last = G_CPUS_N;
                G_CPUS[i as usize] = G_CPUS[last as usize];
                G_CPU_THREADS[i as usize] = G_CPU_THREADS[last as usize];
                G_CPUS[last as usize] = ptr::null_mut();
                i -= 1;
            }
            i += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

pub(super) unsafe fn host_tsd_self() -> u64 {
    let mut v: u64;
    core::arch::asm!("mrs {}, tpidrro_el0", out(reg) v);
    v & !7
}

pub(super) unsafe fn cpu_guest_tsd(c: *const OcerzCPU) -> u64 {
    unsafe {
        if ocerz_gs_is_teb_band((*c).gs_base) != 0 {
            return (*c).unix_gs_base;
        }
        (*c).gs_base
    }
}

#[inline(always)]
unsafe fn ocerz_gs_is_teb_band(gs: u64) -> c_int {
    ((gs & !0xffffu64) >= ffi::OCERZ_TOP_LO && (gs & !0xffffu64) < ffi::OCERZ_TOP_HI) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_guest_tsd_for_host(host_tsd: u64) -> u64 {
    unsafe {
        if host_tsd == 0 {
            return 0;
        }
        if !G_CUR_CPU.is_null() && (*G_CUR_CPU).host_tsd == host_tsd {
            return cpu_guest_tsd(G_CUR_CPU);
        }
        let mut g = 0u64;
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let mut i = 0;
        while i < G_CPUS_N && g == 0 {
            if (*G_CPUS[i as usize]).host_tsd == host_tsd {
                g = cpu_guest_tsd(G_CPUS[i as usize]);
            }
            i += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        g
    }
}

pub(super) unsafe fn cpu_by_kport_locked(port: u32) -> *mut OcerzCPU {
    unsafe {
        let mut i = 0;
        while i < G_CPUS_N {
            if (*G_CPUS[i as usize]).host_kport == port {
                return G_CPUS[i as usize];
            }
            i += 1;
        }
        ptr::null_mut()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_suspend_point(cpu: *mut OcerzCPU) {
    unsafe {
        if AtomicI32::from_ptr(&raw mut (*cpu).suspend_count).load(Ordering::Acquire) == 0 {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_SUSP_LOCK);
        AtomicI32::from_ptr(&raw mut (*cpu).susp_parked).store(1, Ordering::Release);
        libc::pthread_cond_broadcast(&raw mut G_SUSP_CV);
        while AtomicI32::from_ptr(&raw mut (*cpu).suspend_count).load(Ordering::Acquire) > 0 {
            libc::pthread_cond_wait(&raw mut G_SUSP_CV, &raw mut G_SUSP_LOCK);
        }
        AtomicI32::from_ptr(&raw mut (*cpu).susp_parked).store(0, Ordering::Release);
        libc::pthread_mutex_unlock(&raw mut G_SUSP_LOCK);
    }
}

pub(super) unsafe fn susp_stop_is_safe(t: *mut OcerzCPU) -> c_int {
    unsafe {
        if ffi::ocerz_jit_lock_owner_cpu() == t {
            return 0;
        }
        if AtomicU64::from_ptr(&raw mut (*t).block_since_ns).load(Ordering::Acquire) != 0 {
            return 1;
        }
        let mut hs: thread_state64 = core::mem::zeroed();
        let mut n: mach_msg_type_number_t = ARM_THREAD_STATE64_COUNT;
        if thread_get_state(
            (*t).host_kport,
            ARM_THREAD_STATE64,
            &mut hs as *mut _ as thread_state_t,
            &mut n,
        ) != KERN_SUCCESS
        {
            return 0;
        }
        ptr::copy_nonoverlapping((*t).gpr.as_ptr(), (*t).susp_gpr.as_mut_ptr(), 16);
        if ffi::ocerz_jit_guest_gprs_at(
            (*t).vm,
            hs.pc as *const c_void,
            hs.x.as_ptr(),
            t,
            (*t).susp_gpr.as_mut_ptr(),
        ) == 0
        {
            return 0;
        }
        (*t).susp_have_gpr = 1;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_thread_suspend(self_: *mut OcerzCPU, port: u32) -> c_int {
    unsafe {
        let mut counted = 0;
        loop {
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            let t = cpu_by_kport_locked(port);
            if t.is_null() {
                libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
                return -1;
            }
            if counted == 0 {
                counted = 1;
                if AtomicI32::from_ptr(&raw mut (*t).suspend_count).fetch_add(1, Ordering::AcqRel)
                    + 1
                    > 1
                    || t == self_
                {
                    libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
                    return KERN_SUCCESS;
                }
            }
            let mut ok = AtomicI32::from_ptr(&raw mut (*t).susp_parked).load(Ordering::Acquire);
            if ok == 0 && thread_suspend((*t).host_kport) == KERN_SUCCESS {
                (*t).susp_have_gpr = 0;
                if AtomicI32::from_ptr(&raw mut (*t).susp_parked).load(Ordering::Acquire) != 0 {
                    thread_resume((*t).host_kport);
                    ok = 1;
                } else if susp_stop_is_safe(t) != 0 {
                    (*t).susp_host = 1;
                    ok = 1;
                } else {
                    (*t).susp_have_gpr = 0;
                    thread_resume((*t).host_kport);
                }
            }
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
            if ok != 0 {
                return KERN_SUCCESS;
            }
            let mut ts: libc::timespec = core::mem::zeroed();
            libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts);
            ts.tv_nsec += 200 * 1000;
            if ts.tv_nsec >= 1000000000 {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            libc::pthread_mutex_lock(&raw mut G_SUSP_LOCK);
            libc::pthread_cond_timedwait(&raw mut G_SUSP_CV, &raw mut G_SUSP_LOCK, &ts);
            libc::pthread_mutex_unlock(&raw mut G_SUSP_LOCK);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_thread_resume(port: u32) -> c_int {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let t = cpu_by_kport_locked(port);
        if t.is_null()
            || AtomicI32::from_ptr(&raw mut (*t).suspend_count).load(Ordering::Acquire) <= 0
        {
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
            return -1;
        }
        if AtomicI32::from_ptr(&raw mut (*t).suspend_count).fetch_sub(1, Ordering::AcqRel) - 1 == 0
        {
            if (*t).susp_host != 0 {
                (*t).susp_host = 0;
                thread_resume((*t).host_kport);
            }
            (*t).susp_have_gpr = 0;
            libc::pthread_mutex_lock(&raw mut G_SUSP_LOCK);
            libc::pthread_cond_broadcast(&raw mut G_SUSP_CV);
            libc::pthread_mutex_unlock(&raw mut G_SUSP_LOCK);
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        KERN_SUCCESS
    }
}

pub(super) unsafe fn susp_malloc_range() {
    unsafe {
        let mut di: Dl_info = core::mem::zeroed();
        if dladdr(malloc_zone_malloc as *const c_void, &mut di) == 0 || di.dli_fbase.is_null() {
            return;
        }
        let mh = di.dli_fbase as *const mach_header_64;
        let mut lc = mh.add(1) as *const u8;
        let mut i = 0u32;
        while i < (*mh).ncmds {
            let l = lc as *const load_command;
            if (*l).cmd == LC_SEGMENT_64 {
                let sg = lc as *const segment_command_64;
                if libc::strcmp(sg_segname(sg), c"__TEXT".as_ptr()) == 0 {
                    G_MALLOC_LO = mh as u64;
                    G_MALLOC_HI = G_MALLOC_LO + (*sg).vmsize;
                    return;
                }
            }
            lc = lc.add((*l).cmdsize as usize);
            i += 1;
        }
    }
}

unsafe fn sg_segname(sg: *const segment_command_64) -> *const c_char {
    unsafe { (*sg).segname.as_ptr() }
}

pub(super) unsafe fn susp_in_allocator(port: u32) -> c_int {
    unsafe {
        let mut hs: thread_state64 = core::mem::zeroed();
        let mut n: mach_msg_type_number_t = ARM_THREAD_STATE64_COUNT;
        if G_MALLOC_HI == 0
            || thread_get_state(
                port,
                ARM_THREAD_STATE64,
                &mut hs as *mut _ as thread_state_t,
                &mut n,
            ) != KERN_SUCCESS
        {
            return 0;
        }
        let mut pcs = [0u64; SUSP_FRAMES + 2];
        let mut np = 0usize;
        pcs[np] = hs.pc;
        np += 1;
        pcs[np] = hs.lr;
        np += 1;
        let mut fp = hs.fp;
        let sp = hs.sp;
        let mut k = 0;
        while k < SUSP_FRAMES && fp >= sp && fp - sp < (64 << 20) && (fp & 7) == 0 {
            let next = ptr::read_unaligned(fp as *const u64);
            let ret = ptr::read_unaligned((fp + 8) as *const u64);
            pcs[np] = ret;
            np += 1;
            if next <= fp {
                break;
            }
            fp = next;
            k += 1;
        }
        for k in 0..np {
            let a = pcs[k] & 0x0000ffffffffffff;
            if a >= G_MALLOC_LO && a < G_MALLOC_HI {
                return 1;
            }
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_thread_suspend_native(port: u32) -> c_int {
    unsafe {
        libc::pthread_once(&raw mut G_MALLOC_RANGE_ONCE, Some(susp_malloc_range_call));
        let mut tries = 0u32;
        loop {
            let mut safe = 1;
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            let kr = thread_suspend(port);
            if kr != KERN_SUCCESS {
                libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
                return kr;
            }
            let t = cpu_by_kport_locked(port);
            if !t.is_null() {
                (*t).susp_have_gpr = 0;
                if (!(*t).jit_lock_depth.is_null() && *(*t).jit_lock_depth > 0)
                    || susp_in_allocator(port) != 0
                {
                    safe = 0;
                } else if !(!(*t).bridge_depth.is_null() && *(*t).bridge_depth > 0) {
                    let mut hs: thread_state64 = core::mem::zeroed();
                    let mut n: mach_msg_type_number_t = ARM_THREAD_STATE64_COUNT;
                    ptr::copy_nonoverlapping((*t).gpr.as_ptr(), (*t).susp_gpr.as_mut_ptr(), 16);
                    if thread_get_state(
                        port,
                        ARM_THREAD_STATE64,
                        &mut hs as *mut _ as thread_state_t,
                        &mut n,
                    ) == KERN_SUCCESS
                        && ffi::ocerz_jit_guest_gprs_at(
                            (*t).vm,
                            ocerz_leaf_site(hs.pc, hs.lr) as *const c_void,
                            hs.x.as_ptr(),
                            t,
                            (*t).susp_gpr.as_mut_ptr(),
                        ) != 0
                    {
                        (*t).susp_have_gpr = 1;
                    }
                }
            }
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
            if safe != 0 || tries >= SUSP_NATIVE_TRIES {
                return KERN_SUCCESS;
            }
            thread_resume(port);
            let ts = libc::timespec {
                tv_sec: 0,
                tv_nsec: 100000,
            };
            libc::nanosleep(&ts, ptr::null_mut());
            tries += 1;
        }
    }
}

unsafe extern "C" fn susp_malloc_range_call() {
    unsafe { susp_malloc_range() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_thread_resume_native(port: u32) -> c_int {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let t = cpu_by_kport_locked(port);
        if !t.is_null() {
            (*t).susp_have_gpr = 0;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        thread_resume(port)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_thread_regs(
    port: u32,
    gpr: *mut u64,
    rip: *mut u64,
    rflags: *mut u64,
) -> c_int {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let t = cpu_by_kport_locked(port);
        if !t.is_null() {
            let src = if (*t).susp_have_gpr != 0 {
                (*t).susp_gpr.as_ptr()
            } else {
                (*t).gpr.as_ptr()
            };
            ptr::copy_nonoverlapping(src, gpr, 16);
            *rip = (*t).rip;
            *rflags = (*t).rflags;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        if t.is_null() { -1 } else { 0 }
    }
}

unsafe fn ras_clear(cpu: *mut OcerzCPU) {
    unsafe {
        for i in 0..ffi::OCERZ_RAS_SIZE as usize {
            AtomicPtr::<c_void>::from_ptr(&raw mut (*cpu).ras[i].host_entry)
                .store(ptr::null_mut(), Ordering::Relaxed);
            AtomicU64::from_ptr(&raw mut (*cpu).ras[i].guest_rip).store(0, Ordering::Relaxed);
        }
        AtomicU32::from_ptr(&raw mut (*cpu).ras_top).store(0, Ordering::Release);
    }
}

unsafe fn ras_clear_if(
    cpu: *mut OcerzCPU,
    stale: Option<unsafe extern "C" fn(*const c_void, *mut c_void) -> c_int>,
    arg: *mut c_void,
) {
    unsafe {
        let stale = match stale {
            Some(f) => f,
            None => return,
        };
        for i in 0..ffi::OCERZ_RAS_SIZE as usize {
            let e = AtomicPtr::<c_void>::from_ptr(&raw mut (*cpu).ras[i].host_entry)
                .load(Ordering::Relaxed);
            if !e.is_null() && stale(e, arg) != 0 {
                AtomicPtr::<c_void>::from_ptr(&raw mut (*cpu).ras[i].host_entry)
                    .store(ptr::null_mut(), Ordering::Release);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_purge_jit_ras_if(
    vm: *mut OcerzVM,
    stale: Option<unsafe extern "C" fn(*const c_void, *mut c_void) -> c_int>,
    arg: *mut c_void,
) {
    unsafe {
        if vm.is_null() {
            return;
        }
        ras_clear_if(&raw mut (*vm).cpu, stale, arg);
        if !G_CUR_CPU.is_null() && (*G_CUR_CPU).vm == vm {
            ras_clear_if(G_CUR_CPU, stale, arg);
        }
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        for i in 0..G_CPUS_N {
            let c = G_CPUS[i as usize];
            if !c.is_null() && (*c).vm == vm {
                ras_clear_if(c, stale, arg);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_purge_jit_refs(vm: *mut OcerzVM) {
    unsafe {
        if vm.is_null() {
            return;
        }
        ocerz_vm_purge_jit_ras(vm);
        (*vm).cpu.side_blk = ptr::null_mut();
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        for i in 0..G_CPUS_N {
            let c = G_CPUS[i as usize];
            if !c.is_null() && (*c).vm == vm {
                (*c).side_blk = ptr::null_mut();
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_purge_jit_ras(vm: *mut OcerzVM) {
    unsafe {
        if vm.is_null() {
            return;
        }
        ras_clear(&raw mut (*vm).cpu);
        if !G_CUR_CPU.is_null() && (*G_CUR_CPU).vm == vm {
            ras_clear(G_CUR_CPU);
        }
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        for i in 0..G_CPUS_N {
            let c = G_CPUS[i as usize];
            if !c.is_null() && (*c).vm == vm {
                ras_clear(c);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_peek_pending_async_sig() -> u32 {
    unsafe { AtomicU32::from_ptr(&raw mut G_PENDING_ASYNC_MASK_TLS).load(Ordering::SeqCst) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_take_pending_async_sig() -> u32 {
    unsafe { AtomicU32::from_ptr(&raw mut G_PENDING_ASYNC_MASK_TLS).swap(0, Ordering::SeqCst) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_take_pending_async_sig_mask(accept: u32) -> u32 {
    unsafe {
        let a = AtomicU32::from_ptr(&raw mut G_PENDING_ASYNC_MASK_TLS);
        let mut old = a.load(Ordering::SeqCst);
        while old & accept != 0 {
            match a.compare_exchange(old, old & !accept, Ordering::SeqCst, Ordering::SeqCst) {
                Ok(_) => return old & accept,
                Err(o) => old = o,
            }
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_jit_escape(r: c_int) -> ! {
    unsafe {
        T_JIT_ESCAPE_R = r;
        siglongjmp(G_SIG_RECOVER, 2);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_recov_note(kind: c_int, rip: u64) {
    unsafe {
        let i = (G_RECOV_RING.n & 15) as usize;
        G_RECOV_RING.n = G_RECOV_RING.n.wrapping_add(1);
        G_RECOV_RING.e[i].kind = kind as u8;
        G_RECOV_RING.e[i].rip = rip;
        G_RECOV_RING.e[i].icount = if !G_VM.is_null() {
            (*G_VM).insn_count
        } else {
            0
        };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_recov_dump(f: *mut libc::FILE) {
    unsafe {
        libc::fprintf(
            f,
            c"ocerz: RECOV[%d] total=%u:".as_ptr(),
            libc::getpid(),
            G_RECOV_RING.n,
        );
        let n = if G_RECOV_RING.n < 16 {
            G_RECOV_RING.n
        } else {
            16
        };
        for i in 0..n {
            let j = (G_RECOV_RING.n.wrapping_sub(n).wrapping_add(i) & 15) as usize;
            libc::fprintf(
                f,
                c" %s@%#llx/ic=%#llx".as_ptr(),
                G_RECOV_NAMES[G_RECOV_RING.e[j].kind as usize],
                G_RECOV_RING.e[j].rip as c_ulonglong,
                G_RECOV_RING.e[j].icount as c_ulonglong,
            );
        }
        libc::fprintf(f, c"\n".as_ptr());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_atfork_prepare() {
    unsafe {
        let self_ = pthread_self();
        if G_FORK_KEEPJIT < 0 {
            G_FORK_KEEPJIT = (!libc::getenv(c"OCERZ_FORK_KEEPJIT".as_ptr()).is_null()) as c_int;
        }
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        G_FORK_SURVIVING_CPU = ptr::null_mut();
        for i in 0..G_CPUS_N {
            if pthread_equal(G_CPU_THREADS[i as usize], self_) != 0 {
                G_FORK_SURVIVING_CPU = G_CPUS[i as usize];
                break;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_atfork_parent() {
    unsafe {
        G_FORK_SURVIVING_CPU = ptr::null_mut();
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_atfork_child() {
    unsafe {
        let survivor = G_FORK_SURVIVING_CPU;
        for i in 0..G_CPUS_N {
            G_CPUS[i as usize] = ptr::null_mut();
        }
        G_CPUS_N = 0;
        if !survivor.is_null() {
            G_CPUS[0] = survivor;
            G_CPU_THREADS[0] = pthread_self();
            G_CPUS_N = 1;
        }
        G_FORK_SURVIVING_CPU = ptr::null_mut();
        ptr::write_volatile(&raw mut G_PENDING_ASYNC_MASK_TLS, 0);
        if !survivor.is_null() {
            (*survivor).sig_pending = 0;
        }
        G_RIPHIST_N = 0;
        G_UNSTICK_STARTED.store(0, Ordering::Release);
        if !G_VM.is_null() && (*G_VM).jit_enabled != 0 && G_FORK_KEEPJIT <= 0 {
            ffi::ocerz_jit_forget(G_VM);
            if !survivor.is_null() {
                (*survivor).interp_once = 1;
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_riphist(out: *mut u64, max: c_uint) -> c_uint {
    unsafe {
        let mut n = if G_RIPHIST_N < 32 { G_RIPHIST_N } else { 32 };
        if n > max {
            n = max;
        }
        for i in 0..n {
            *out.add(i as usize) =
                G_RIPHIST[(G_RIPHIST_N.wrapping_sub(1).wrapping_sub(i) & 31) as usize];
        }
        n
    }
}

pub(super) unsafe fn ctx_trap_report(c: *const OcerzCPU) {
    unsafe {
        static mut HITS: c_int = 0;
        if HITS >= 12 {
            return;
        }
        HITS += 1;
        let f = (*c).gpr[OCERZ_RCX];
        let s = (*c).gpr[OCERZ_RSI];
        libc::fprintf(
            stderr(),
            c"ocerz: CTXTRAP pid=%d rip=%#llx rdi=%#llx rsi(ctx_t)=%#llx ctx.machine=%#x ctx.flags=%#x ctl.rip=%#llx ctl.rsp=%#llx | f(rcx)=%#llx f.rip=%#llx f.rsp=%#llx gs=%#llx icount=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            (*c).rip as c_ulonglong,
            (*c).gpr[OCERZ_RDI] as c_ulonglong,
            s as c_ulonglong,
            ocerz_ld(s + 0x00, 4) as u32,
            ocerz_ld(s + 0x04, 4) as u32,
            ocerz_ld(s + 0x08, 8) as c_ulonglong,
            ocerz_ld(s + 0x10, 8) as c_ulonglong,
            f as c_ulonglong,
            ocerz_ld(f + 0x70, 8) as c_ulonglong,
            ocerz_ld(f + 0x88, 8) as c_ulonglong,
            (*c).gs_base as c_ulonglong,
            (if !G_VM.is_null() { (*G_VM).insn_count } else { 0 }) as c_ulonglong,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bt_report(c: *const OcerzCPU) {
    unsafe {
        if ocerz_bt_done != 0 {
            return;
        }
        ocerz_bt_done = 1;
        libc::fprintf(
            stderr(),
            c"ocerz: BTTRAP rip=%#llx chain:".as_ptr(),
            (*c).rip as c_ulonglong,
        );
        let mut fp = (*c).gpr[OCERZ_RBP];
        let mut d = 0;
        while d < 18 && fp >= 0x300000000 {
            libc::fprintf(
                stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp + 8, 8) as c_ulonglong,
            );
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
}

pub(super) unsafe fn exc_dump_cfstr(tag: *const c_char, s: u64) {
    unsafe {
        if s < 0x100000000 {
            libc::fprintf(stderr(), c" %s=<%#llx>".as_ptr(), tag, s as c_ulonglong);
            return;
        }
        let cstr = ocerz_ld(s + 0x10, 8);
        let len = ocerz_ld(s + 0x18, 8);
        if cstr >= 0x100000000 && len > 0 && len < 4096 {
            libc::fprintf(stderr(), c" %s=\"".as_ptr(), tag);
            for i in 0..len {
                let ch = ocerz_ld(cstr + i, 1) as c_int;
                libc::fputc(
                    if ch >= 32 && ch < 127 {
                        ch
                    } else {
                        '?' as c_int
                    },
                    stderr(),
                );
            }
            libc::fputc('"' as c_int, stderr());
        } else {
            libc::fprintf(stderr(), c" %s=inline\"".as_ptr(), tag);
            for i in 0x10u64..0x140 {
                let ch = ocerz_ld(s + i, 1) as c_int;
                if ch == 0 {
                    break;
                }
                libc::fputc(
                    if ch >= 32 && ch < 127 {
                        ch
                    } else {
                        '?' as c_int
                    },
                    stderr(),
                );
            }
            libc::fputc('"' as c_int, stderr());
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_exc_report(c: *const OcerzCPU) {
    unsafe {
        let exc = (*c).gpr[OCERZ_RDI];
        libc::fprintf(
            stderr(),
            c"ocerz: EXCTRAP exc=%#llx isa=%#llx".as_ptr(),
            exc as c_ulonglong,
            ocerz_ld(exc, 8) as c_ulonglong,
        );
        exc_dump_cfstr(c"name".as_ptr(), ocerz_ld(exc + 0x08, 8));
        exc_dump_cfstr(c"reason".as_ptr(), ocerz_ld(exc + 0x10, 8));
        libc::fputc('\n' as c_int, stderr());
    }
}

pub(super) unsafe fn arg_trap_report(c: *const OcerzCPU) {
    unsafe {
        static mut A: [(*const c_char, usize); 6] = [
            (c"rdi".as_ptr(), OCERZ_RDI),
            (c"rsi".as_ptr(), OCERZ_RSI),
            (c"rdx".as_ptr(), OCERZ_RDX),
            (c"rcx".as_ptr(), OCERZ_RCX),
            (c"r8".as_ptr(), OCERZ_R8),
            (c"r9".as_ptr(), OCERZ_R9),
        ];
        libc::fprintf(
            stderr(),
            c"ocerz: ARGTRAP[%d] rip=%#llx".as_ptr(),
            libc::getpid(),
            (*c).rip as c_ulonglong,
        );
        {
            let mut rh = [0u64; 32];
            let rn = ocerz_vm_riphist(rh.as_mut_ptr(), 32);
            libc::fprintf(stderr(), c" hist:".as_ptr());
            for i in 0..rn.min(16) {
                libc::fprintf(stderr(), c" %#llx".as_ptr(), rh[i as usize] as c_ulonglong);
            }
        }
        if ocerz_addr_readable((*c).gpr[OCERZ_RSP]) != 0 {
            libc::fprintf(
                stderr(),
                c" ra=%#llx".as_ptr(),
                ocerz_ld((*c).gpr[OCERZ_RSP], 8) as c_ulonglong,
            );
            let mut o = 8u64;
            while o < 0x3000 {
                if ocerz_addr_readable((*c).gpr[OCERZ_RSP] + o) == 0 {
                    break;
                }
                let v = ocerz_ld((*c).gpr[OCERZ_RSP] + o, 8);
                let codey = (v >= 0x7ff800000000 && v < 0x7ffb00000000)
                    || (v >= 0x700000000000 && v < 0x710000000000)
                    || (v >= 0x6fff00000000 && v < 0x700000000000)
                    || (v >= 0x140000000 && v < 0x180000000)
                    || (v >= 0x7ff000000000 && v < 0x7ff100000000);
                if codey {
                    libc::fprintf(
                        stderr(),
                        c" +%#llx:%#llx".as_ptr(),
                        o as c_ulonglong,
                        v as c_ulonglong,
                    );
                }
                o += 8;
            }
        }
        for i in 0..6usize {
            let v = (*c).gpr[A[i].1];
            libc::fprintf(stderr(), c" %s=%#llx".as_ptr(), A[i].0, v as c_ulonglong);
            if v != 0 && ocerz_addr_readable(v) != 0 && ocerz_addr_readable(v + 8) != 0 {
                libc::fprintf(
                    stderr(),
                    c"->{%#llx,%#llx}".as_ptr(),
                    ocerz_ld(v, 8) as c_ulonglong,
                    ocerz_ld(v + 8, 8) as c_ulonglong,
                );
            } else if v != 0 {
                libc::fprintf(stderr(), c"->UNCOMMITTED".as_ptr());
            }
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
}

pub(super) unsafe fn sel_trap_report(c: *const OcerzCPU) {
    unsafe {
        let recv = (*c).gpr[OCERZ_RDI];
        let sel = (*c).gpr[OCERZ_RSI];
        let isa = if recv != 0 {
            ocerz_ld(recv, 8) & 0x00007ffffffffff8
        } else {
            0
        };
        let mut nm = [0u8; 80];
        let mut i = 0usize;
        while i < 79 && sel != 0 {
            let b = ocerz_ld(sel + i as u64, 1) as u8;
            nm[i] = b;
            if b == 0 {
                break;
            }
            i += 1;
        }
        nm[i] = 0;
        libc::fprintf(
            stderr(),
            c"ocerz: SELTRAP rip=%#llx recv=%#llx isa=%#llx sel=%#llx \"%s\" gs=%#llx icount=%llu\n"
                .as_ptr(),
            (*c).rip as c_ulonglong,
            recv as c_ulonglong,
            isa as c_ulonglong,
            sel as c_ulonglong,
            nm.as_ptr() as *const c_char,
            (*c).gs_base as c_ulonglong,
            (if !G_VM.is_null() { (*G_VM).insn_count } else { 0 }) as c_ulonglong,
        );
        if isa != 0 && !libc::getenv(c"OCERZ_METHDUMP".as_ptr()).is_null() {
            ffi::ocerz_dyldapi_dump_method(isa, c"allocWithZone:".as_ptr());
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_current_cpu() -> *mut OcerzCPU {
    unsafe { G_CUR_CPU }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_process() -> *mut OcerzVM {
    unsafe { G_VM }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_current_guest_rip() -> u64 {
    unsafe {
        if !G_CUR_CPU.is_null() {
            (*G_CUR_CPU).rip
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_current_guest_rsp() -> u64 {
    unsafe {
        if !G_CUR_CPU.is_null() {
            (*G_CUR_CPU).gpr[OCERZ_RSP]
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_current_guest_gpr(i: c_int) -> u64 {
    unsafe {
        if !G_CUR_CPU.is_null() && i >= 0 && i < 16 {
            (*G_CUR_CPU).gpr[i as usize]
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_current_dbg_ind_src() -> u64 {
    unsafe {
        if !G_CUR_CPU.is_null() {
            (*G_CUR_CPU).dbg_ind_src
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_watch_hit(gaddr: u64, size: c_int, lo: u64, hi: u64) {
    unsafe {
        let c = if !G_CUR_CPU.is_null() {
            G_CUR_CPU
        } else if !G_VM.is_null() {
            &raw mut (*G_VM).cpu
        } else {
            ptr::null_mut()
        };
        libc::fprintf(
            stderr(),
            c"ocerz: WATCH[pid=%d] st [%#llx] size=%d val=%#llx:%#llx rip=%#llx icount=%llu rdi=%#llx rsi=%#llx rax=%#llx rbx=%#llx r14=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            gaddr as c_ulonglong,
            size,
            hi as c_ulonglong,
            lo as c_ulonglong,
            (if !c.is_null() { (*c).rip } else { 0 }) as c_ulonglong,
            (if !G_VM.is_null() { (*G_VM).insn_count } else { 0 }) as c_ulonglong,
            (if !c.is_null() { (*c).gpr[OCERZ_RDI] } else { 0 }) as c_ulonglong,
            (if !c.is_null() { (*c).gpr[OCERZ_RSI] } else { 0 }) as c_ulonglong,
            (if !c.is_null() { (*c).gpr[OCERZ_RAX] } else { 0 }) as c_ulonglong,
            (if !c.is_null() { (*c).gpr[OCERZ_RBX] } else { 0 }) as c_ulonglong,
            (if !c.is_null() { (*c).gpr[OCERZ_R14] } else { 0 }) as c_ulonglong,
        );
        if !c.is_null() && !libc::getenv(c"OCERZ_WATCHBT".as_ptr()).is_null() {
            let mut fp = (*c).gpr[OCERZ_RBP];
            libc::fprintf(
                stderr(),
                c"ocerz:   WATCHBT gs+0x18=%#llx bt:".as_ptr(),
                ocerz_ld((*c).gs_base + 0x18, 8) as c_ulonglong,
            );
            let mut d = 0;
            while d < 14 && fp >= 0x300000000 {
                libc::fprintf(
                    stderr(),
                    c" %#llx".as_ptr(),
                    ocerz_ld(fp + 8, 8) as c_ulonglong,
                );
                let nf = ocerz_ld(fp, 8);
                if nf <= fp {
                    break;
                }
                fp = nf;
                d += 1;
            }
            libc::fprintf(stderr(), c"\n".as_ptr());
        }
    }
}

#[inline(always)]
pub(super) unsafe fn hex_into(mut p: *mut c_char, v: u64) -> *mut c_char {
    static DIGITS: &[u8; 16] = b"0123456789abcdef";
    unsafe {
        *p = '0' as c_char;
        p = p.add(1);
        *p = 'x' as c_char;
        p = p.add(1);
        let mut started = false;
        let mut shift = 60i32;
        while shift >= 0 {
            let d = ((v >> shift) & 0xf) as usize;
            if d != 0 || started || shift == 0 {
                *p = DIGITS[d] as c_char;
                p = p.add(1);
                started = true;
            }
            shift -= 4;
        }
        p
    }
}

#[inline(always)]
pub(super) unsafe fn str_into(mut p: *mut c_char, mut s: *const c_char) -> *mut c_char {
    unsafe {
        while *s != 0 {
            *p = *s;
            p = p.add(1);
            s = s.add(1);
        }
        p
    }
}

pub(super) unsafe fn ocerz_host_sigmask_clear(where_: *const c_char) {
    unsafe {
        let mut cur: sigset_t = core::mem::zeroed();
        let mut empty: sigset_t = core::mem::zeroed();
        if pthread_sigmask(SIG_BLOCK, ptr::null(), &mut cur) != 0 {
            return;
        }
        let mut v = 0u32;
        for sg in 1..32 {
            if sigismember(&cur, sg) != 0 {
                v |= 1u32 << sg;
            }
        }
        if v == 0 {
            return;
        }
        sigemptyset(&mut empty);
        pthread_sigmask(SIG_SETMASK, &empty, ptr::null_mut());
        if env_cache!("OCERZ_HOSTMASKLOG") != 0 {
            libc::fprintf(
                stderr(),
                c"ocerz: HOSTMASK-CLEARED[%d] %s had %#x blocked\n".as_ptr(),
                libc::getpid(),
                where_,
                v,
            );
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cpu_restore_saved(cpu: *mut OcerzCPU, saved: *const OcerzCPU) {
    unsafe {
        let mut all: sigset_t = core::mem::zeroed();
        let mut old: sigset_t = core::mem::zeroed();
        sigfillset(&mut all);
        pthread_sigmask(SIG_BLOCK, &all, &mut old);
        let pend = AtomicU64::from_ptr(&raw mut (*cpu).sig_pending).load(Ordering::SeqCst);
        let mut rcvd = [0u32; 32];
        let mut dlv = [0u32; 32];
        ptr::copy_nonoverlapping((*cpu).sig_host_rcvd.as_ptr(), rcvd.as_mut_ptr(), 32);
        ptr::copy_nonoverlapping((*cpu).sig_delivered.as_ptr(), dlv.as_mut_ptr(), 32);
        let handback = (*cpu).nested_sig_handback;
        let terminated = (*cpu).terminated;
        let interrupt = (*cpu).interrupt;
        let suspend_count = (*cpu).suspend_count;
        let susp_parked = (*cpu).susp_parked;
        let ring_n = (*cpu).sysring_n;
        let mut ring = (*cpu).sysring;
        ptr::copy_nonoverlapping((*cpu).sysring.as_ptr(), ring.as_mut_ptr(), 24);
        *cpu = *saved;
        (*cpu).sig_pending = pend;
        ptr::copy_nonoverlapping(rcvd.as_ptr(), (*cpu).sig_host_rcvd.as_mut_ptr(), 32);
        ptr::copy_nonoverlapping(dlv.as_ptr(), (*cpu).sig_delivered.as_mut_ptr(), 32);
        (*cpu).nested_sig_handback = handback;
        (*cpu).terminated = terminated;
        (*cpu).interrupt = interrupt;
        (*cpu).suspend_count = suspend_count;
        (*cpu).susp_parked = susp_parked;
        ptr::copy_nonoverlapping(ring.as_ptr(), (*cpu).sysring.as_mut_ptr(), 24);
        (*cpu).sysring_n = ring_n;
        pthread_sigmask(SIG_SETMASK, &old, ptr::null_mut());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_mirror_host_signal(sig: c_int, kind: c_int) {
    unsafe {
        if sig == SIGUSR1 && !libc::getenv(c"OCERZ_PORTDUMP".as_ptr()).is_null() {
            libc::fprintf(
                stderr(),
                c"ocerz: SIGACT[%d] guest sigaction(SIGUSR1) kind=%d\n".as_ptr(),
                libc::getpid(),
                kind,
            );
        }
        match sig {
            libc::SIGPIPE
            | libc::SIGHUP
            | libc::SIGINT
            | libc::SIGTERM
            | libc::SIGALRM
            | libc::SIGCHLD
            | libc::SIGWINCH
            | libc::SIGURG
            | libc::SIGIO
            | libc::SIGVTALRM
            | libc::SIGPROF
            | libc::SIGXCPU
            | libc::SIGXFSZ
            | libc::SIGTSTP
            | libc::SIGTTIN
            | libc::SIGTTOU
            | libc::SIGCONT
            | libc::SIGINFO => {}
            libc::SIGUSR1 => {
                if !libc::getenv(c"OCERZ_RIPDUMP".as_ptr()).is_null() {
                    return;
                }
            }
            libc::SIGUSR2 => {
                if !libc::getenv(c"OCERZ_PORTDUMP".as_ptr()).is_null() {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: PORTDUMP[%d] guest sigaction(SIGUSR2, kind=%d) ignored, dump handler kept\n"
                            .as_ptr(),
                        libc::getpid(),
                        kind,
                    );
                    return;
                }
            }
            _ => return,
        }
        let mut sa: sigaction = core::mem::zeroed();
        G_ASYNC_SHARED_ONLY =
            (!libc::getenv(c"OCERZ_NO_THREAD_SIGNALS".as_ptr()).is_null()) as c_int;
        if kind == 1 {
            sa.sa_sigaction = libc::SIG_IGN;
        } else if kind == 2 {
            sa.sa_sigaction = sig::async_sig_handler as libc::sighandler_t;
            sa.sa_flags = (SA_SIGINFO | SA_NODEFER | SA_RESTART) as c_int;
            if sig == SIGUSR1 || sig == SIGUSR2 {
                sa.sa_flags &= !(SA_RESTART as c_int);
            }
        } else {
            sa.sa_sigaction = libc::SIG_DFL;
        }
        libc::sigaction(sig, &sa, ptr::null_mut());
    }
}
