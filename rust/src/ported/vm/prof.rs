use core::ffi::{c_char, c_int, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::Ordering;

use super::*;
use crate::ffi::OcerzJitFaultInfo;

static mut G_GP_KEY: [u64; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GP_CNT: [u32; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GH_KEY: [u64; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GH_CNT: [u32; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GP_SAMPLES: u64 = 0;
static mut G_GP_JIT: u64 = 0;
static mut G_GF_KEY: [u64; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GF_CNT: [u32; GP_SLOTS] = [0; GP_SLOTS];
static mut G_GP_VM: *mut OcerzVM = ptr::null_mut();
static mut G_GP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_GP_START: u64 = 0;

unsafe fn gp_bump(keys: *mut u64, cnts: *mut u32, key: u64, w: u32) {
    unsafe {
        let mut h = ((key.wrapping_mul(0x9E3779B97F4A7C15) >> 48) as u32) & (GP_SLOTS as u32 - 1);
        let mut probe = 0u32;
        while probe < GP_SLOTS as u32 {
            let kv = *keys.add(h as usize);
            if kv == key {
                *cnts.add(h as usize) = (*cnts.add(h as usize)).wrapping_add(w);
                return;
            }
            if kv == 0 {
                *keys.add(h as usize) = key;
                *cnts.add(h as usize) = w;
                return;
            }
            probe += 1;
            h = (h + 1) & (GP_SLOTS as u32 - 1);
        }
    }
}

#[repr(C)]
struct GpTop {
    key: u64,
    cnt: u32,
}

unsafe fn gp_top(keys: *const u64, cnts: *const u32, top: *mut GpTop, want: c_int) -> c_int {
    unsafe {
        let mut n = 0i32;
        for i in 0..GP_SLOTS {
            if *keys.add(i) == 0 {
                continue;
            }
            let mut at = if n < want { n } else { want };
            while at > 0 && (*top.add((at - 1) as usize)).cnt < *cnts.add(i) {
                at -= 1;
            }
            if at >= want {
                continue;
            }
            let last = if n < want { n } else { want - 1 };
            let mut k = last;
            while k > at {
                *top.add(k as usize) = ptr::read(top.add((k - 1) as usize));
                k -= 1;
            }
            (*top.add(at as usize)).key = *keys.add(i);
            (*top.add(at as usize)).cnt = *cnts.add(i);
            if n < want {
                n += 1;
            }
        }
        n
    }
}

unsafe fn gp_report(secs: f64) {
    unsafe {
        let total = if G_GP_SAMPLES != 0 { G_GP_SAMPLES } else { 1 };
        libc::fprintf(
            stderr(),
            c"ocerz: GUESTPROF[%d] %.0fs cpu_ms=%llu translated=%.1f%% runtime=%.1f%%\n".as_ptr(),
            libc::getpid(),
            secs,
            (G_GP_SAMPLES / 1000) as c_ulonglong,
            100.0 * G_GP_JIT as f64 / total as f64,
            100.0 * (G_GP_SAMPLES - G_GP_JIT) as f64 / total as f64,
        );
        {
            static mut LAST_TR: u64 = 0;
            static mut LAST_RET: u64 = 0;
            static mut LAST_FLIP: u64 = 0;
            static mut LAST_NS: u64 = 0;
            static mut LAST_RNS: u64 = 0;
            let mut tr: u64 = 0;
            let mut live: u64 = 0;
            let mut ret: u64 = 0;
            let mut flip: u64 = 0;
            ffi::ocerz_jit_prof_stats(G_GP_VM, &mut tr, &mut live, &mut ret, &mut flip);
            let ns = AtomicU64::from_ptr(&raw mut ocerz_jit_xlat_ns).load(Ordering::Relaxed);
            let rns = AtomicU64::from_ptr(&raw mut ocerz_jit_retire_ns).load(Ordering::Relaxed);
            libc::fprintf(
                stderr(),
                c"ocerz: GUESTPROF[%d]   jit translated=%llu live=%llu retires=%llu flips=%llu xlat_ms=%llu retire_ms=%llu\n"
                    .as_ptr(),
                libc::getpid(),
                tr.wrapping_sub(LAST_TR) as c_ulonglong,
                live as c_ulonglong,
                ret.wrapping_sub(LAST_RET) as c_ulonglong,
                flip.wrapping_sub(LAST_FLIP) as c_ulonglong,
                (ns.wrapping_sub(LAST_NS) / 1000000) as c_ulonglong,
                (rns.wrapping_sub(LAST_RNS) / 1000000) as c_ulonglong,
            );
            LAST_TR = tr;
            LAST_RET = ret;
            LAST_FLIP = flip;
            LAST_NS = ns;
            LAST_RNS = rns;
        }
        static mut TOP: [GpTop; 40] = [const { GpTop { key: 0, cnt: 0 } }; 40];
        let n = gp_top(
            (&raw mut G_GP_KEY) as *mut u64,
            (&raw mut G_GP_CNT) as *mut u32,
            (&raw mut TOP) as *mut GpTop,
            40,
        );
        for i in 0..n {
            let rip = (*((&raw const TOP) as *const GpTop).add(i as usize)).key & !(1u64 << 63);
            let mut base: u64 = 0;
            let img = ocerz_dyld_name_for_addr(rip, &mut base);
            let leaf = if !img.is_null() {
                libc::strrchr(img, '/' as c_int)
            } else {
                ptr::null()
            };
            libc::fprintf(
                stderr(),
                c"ocerz: GUESTPROF[%d]   %5.1f%% %s rip=%#llx %s+%#llx\n".as_ptr(),
                libc::getpid(),
                100.0 * (*((&raw const TOP) as *const GpTop).add(i as usize)).cnt as f64
                    / total as f64,
                if (*((&raw const TOP) as *const GpTop).add(i as usize)).key >> 63 != 0 {
                    c"rt ".as_ptr()
                } else {
                    c"jit".as_ptr()
                },
                rip as c_ulonglong,
                if !leaf.is_null() {
                    leaf.add(1)
                } else if !img.is_null() {
                    img
                } else {
                    c"?".as_ptr()
                },
                rip.wrapping_sub(base) as c_ulonglong,
            );
        }
        {
            #[repr(C)]
            struct AggImg {
                img: *const c_char,
                lo: u64,
                jit: u32,
                rt: u32,
            }
            static mut AGG_IMG: [AggImg; 48] = [const {
                AggImg {
                    img: ptr::null(),
                    lo: 0,
                    jit: 0,
                    rt: 0,
                }
            }; 48];
            let mut ni = 0i32;
            for i in 0..GP_SLOTS {
                if G_GP_KEY[i] == 0 {
                    continue;
                }
                let rip = G_GP_KEY[i] & !(1u64 << 63);
                let mut base: u64 = 0;
                let mut img = ocerz_dyld_name_for_addr(rip, &mut base);
                let leaf = if !img.is_null() {
                    libc::strrchr(img, '/' as c_int)
                } else {
                    ptr::null()
                };
                if !leaf.is_null() {
                    img = leaf.add(1);
                }
                let lo = if img.is_null() { rip >> 24 } else { 0 };
                let mut k = 0i32;
                while k < ni
                    && !(AGG_IMG[k as usize].lo == lo
                        && (if !img.is_null() {
                            !AGG_IMG[k as usize].img.is_null()
                                && libc::strcmp(AGG_IMG[k as usize].img, img) == 0
                        } else {
                            AGG_IMG[k as usize].img.is_null()
                        }))
                {
                    k += 1;
                }
                if k == ni {
                    if ni == 48 {
                        continue;
                    }
                    AGG_IMG[ni as usize].img = img;
                    AGG_IMG[ni as usize].lo = lo;
                    AGG_IMG[ni as usize].jit = 0;
                    AGG_IMG[ni as usize].rt = 0;
                    ni += 1;
                }
                if G_GP_KEY[i] >> 63 != 0 {
                    AGG_IMG[k as usize].rt += G_GP_CNT[i];
                } else {
                    AGG_IMG[k as usize].jit += G_GP_CNT[i];
                }
            }
            let mut i = 0;
            while i < ni && i < 12 {
                let mut best = i;
                let mut k = i + 1;
                while k < ni {
                    if AGG_IMG[k as usize].jit + AGG_IMG[k as usize].rt
                        > AGG_IMG[best as usize].jit + AGG_IMG[best as usize].rt
                    {
                        best = k;
                    }
                    k += 1;
                }
                if best != i {
                    ptr::swap(
                        &mut AGG_IMG[i as usize] as *mut AggImg,
                        &mut AGG_IMG[best as usize] as *mut AggImg,
                    );
                }
                let mut anon = [0u8; 32];
                if AGG_IMG[i as usize].img.is_null() {
                    libc::snprintf(
                        anon.as_mut_ptr() as *mut c_char,
                        32,
                        c"?%#llx".as_ptr(),
                        (AGG_IMG[i as usize].lo << 24) as c_ulonglong,
                    );
                }
                libc::fprintf(
                    stderr(),
                    c"ocerz: GUESTPROF[%d]   image %5.1f%% jit %5.1f%% rt %s\n".as_ptr(),
                    libc::getpid(),
                    100.0 * AGG_IMG[i as usize].jit as f64 / total as f64,
                    100.0 * AGG_IMG[i as usize].rt as f64 / total as f64,
                    if !AGG_IMG[i as usize].img.is_null() {
                        AGG_IMG[i as usize].img
                    } else {
                        anon.as_ptr() as *const c_char
                    },
                );
                i += 1;
            }
        }
        static mut HOST: [GpTop; 64] = [const { GpTop { key: 0, cnt: 0 } }; 64];
        let hn = gp_top(
            (&raw mut G_GH_KEY) as *mut u64,
            (&raw mut G_GH_CNT) as *mut u32,
            (&raw mut HOST) as *mut GpTop,
            64,
        );
        #[repr(C)]
        struct Agg {
            sym: *const c_void,
            name: *const c_char,
            cnt: u32,
        }
        static mut AGG: [Agg; 64] = [const {
            Agg {
                sym: ptr::null(),
                name: ptr::null(),
                cnt: 0,
            }
        }; 64];
        let mut an = 0i32;
        for i in 0..hn {
            let mut di: Dl_info = core::mem::zeroed();
            let mut sym: *const c_void = ptr::null();
            let mut name: *const c_char = c"?".as_ptr();
            if dladdr(
                (*((&raw const HOST) as *const GpTop).add(i as usize)).key as *const c_void,
                &mut di,
            ) != 0
                && !di.dli_sname.is_null()
            {
                sym = di.dli_saddr;
                name = di.dli_sname;
            }
            let mut k = 0i32;
            while k < an && AGG[k as usize].sym != sym {
                k += 1;
            }
            if k == an {
                AGG[an as usize].sym = sym;
                AGG[an as usize].name = name;
                AGG[an as usize].cnt = 0;
                an += 1;
            }
            AGG[k as usize].cnt += (*((&raw const HOST) as *const GpTop).add(i as usize)).cnt;
        }
        let mut i = 0;
        while i < an && i < 30 {
            let mut best = i;
            let mut k = i + 1;
            while k < an {
                if AGG[k as usize].cnt > AGG[best as usize].cnt {
                    best = k;
                }
                k += 1;
            }
            if best != i {
                ptr::swap(
                    &mut AGG[i as usize] as *mut Agg,
                    &mut AGG[best as usize] as *mut Agg,
                );
            }
            libc::fprintf(
                stderr(),
                c"ocerz: GUESTPROF[%d]   host %5.1f%% %s\n".as_ptr(),
                libc::getpid(),
                100.0 * AGG[i as usize].cnt as f64 / total as f64,
                AGG[i as usize].name,
            );
            i += 1;
        }
        static mut FORM: [GpTop; 16] = [const { GpTop { key: 0, cnt: 0 } }; 16];
        let fn_ = gp_top(
            (&raw mut G_GF_KEY) as *mut u64,
            (&raw mut G_GF_CNT) as *mut u32,
            (&raw mut FORM) as *mut GpTop,
            16,
        );
        for i in 0..fn_ {
            let f = (*((&raw const FORM) as *const GpTop).add(i as usize)).key;
            static KC: [u8; 8] = *b"-rxsmiM?";
            let mut imm = [0u8; 8];
            if (f & 0xffff) == ffi::OCERZ_OP_SYSCALL as u64 {
                libc::fprintf(
                    stderr(),
                    c"ocerz: GUESTPROF[%d]   interp %5.1f%% syscall %#llx\n".as_ptr(),
                    libc::getpid(),
                    100.0 * (*((&raw const FORM) as *const GpTop).add(i as usize)).cnt as f64
                        / total as f64,
                    (f >> 32) as c_ulonglong,
                );
                continue;
            }
            if (f >> 48) & 1 != 0 {
                libc::snprintf(
                    imm.as_mut_ptr() as *mut c_char,
                    8,
                    c" %#x".as_ptr(),
                    ((f >> 40) & 0xff) as u32,
                );
            }
            libc::fprintf(
                stderr(),
                c"ocerz: GUESTPROF[%d]   interp %5.1f%% %s%s %c%c%c/%u%s\n".as_ptr(),
                libc::getpid(),
                100.0 * (*((&raw const FORM) as *const GpTop).add(i as usize)).cnt as f64
                    / total as f64,
                if (f >> 16) & 1 != 0 {
                    c"v".as_ptr()
                } else {
                    c"".as_ptr()
                },
                ffi::ocerz_op_name((f & 0xffff) as u32),
                KC[((f >> 18) & 7) as usize] as c_int,
                KC[((f >> 21) & 7) as usize] as c_int,
                KC[((f >> 24) & 7) as usize] as c_int,
                ((f >> 32) & 0xff) as u32,
                imm.as_ptr() as *const c_char,
            );
        }
        ptr::write_bytes((&raw mut G_GF_KEY) as *mut u64, 0, GP_SLOTS);
        ptr::write_bytes((&raw mut G_GF_CNT) as *mut u32, 0, GP_SLOTS);
        ptr::write_bytes((&raw mut G_GP_KEY) as *mut u64, 0, GP_SLOTS);
        ptr::write_bytes((&raw mut G_GP_CNT) as *mut u32, 0, GP_SLOTS);
        ptr::write_bytes((&raw mut G_GH_KEY) as *mut u64, 0, GP_SLOTS);
        ptr::write_bytes((&raw mut G_GH_CNT) as *mut u32, 0, GP_SLOTS);
        G_GP_SAMPLES = 0;
        G_GP_JIT = 0;
    }
}

#[repr(C)]
struct GpSeen {
    port: mach_port_t,
    cpu_us: u64,
    period_us: u64,
}

extern "C" fn guestprof_thread(arg: *mut c_void) -> *mut c_void {
    unsafe {
        let vm = arg as *mut OcerzVM;
        let iv = libc::getenv(c"OCERZ_GUESTPROF".as_ptr());
        let pv = libc::getenv(c"OCERZ_GUESTPROF_PERIOD".as_ptr());
        let mut us: u32 = if !iv.is_null() {
            libc::strtoul(iv, ptr::null_mut(), 0) as u32
        } else {
            1000
        };
        if us < 100 {
            us = 1000;
        }
        let period = (if !pv.is_null() {
            libc::strtoull(pv, ptr::null_mut(), 0)
        } else {
            10
        }) * 1000000000;
        let start = G_GP_START;
        let mut next = start + period;
        static mut SEEN: [GpSeen; 256] = [const {
            GpSeen {
                port: 0,
                cpu_us: 0,
                period_us: 0,
            }
        }; 256];
        let hot_only = !libc::getenv(c"OCERZ_GUESTPROF_HOT".as_ptr()).is_null();
        let mut hot: mach_port_t = MACH_PORT_NULL;
        let mut pstart = start;
        let mut seed = start | 1;
        loop {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            libc::usleep(us / 2 + (seed % us as u64) as u32);
            let mut ports = [0u32; OCERZ_MAX_CPUS];
            let mut cpus: [*mut OcerzCPU; OCERZ_MAX_CPUS] = [ptr::null_mut(); OCERZ_MAX_CPUS];
            let mut n = 0usize;
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            for i in 0..G_CPUS_N {
                ports[n] = pthread_mach_thread_np(G_CPU_THREADS[i as usize]);
                cpus[n] = G_CPUS[i as usize];
                n += 1;
            }
            libc::pthread_mutex_lock(&raw mut G_GP_LOCK);
            for i in 0..n {
                let mut bi: thread_basic_info_data = core::mem::zeroed();
                let mut bc: mach_msg_type_number_t = THREAD_BASIC_INFO_COUNT;
                if thread_info(
                    ports[i],
                    THREAD_BASIC_INFO,
                    &mut bi as *mut _ as thread_info_t,
                    &mut bc,
                ) != KERN_SUCCESS
                {
                    continue;
                }
                let cpu_us = ((bi.user_time.seconds + bi.system_time.seconds) as u64) * 1000000
                    + (bi.user_time.microseconds + bi.system_time.microseconds) as u64;
                let mut slot = ports[i].wrapping_mul(2654435761) & 255;
                while SEEN[slot as usize].port != 0 && SEEN[slot as usize].port != ports[i] {
                    slot = (slot + 1) & 255;
                }
                let prev = if SEEN[slot as usize].port != 0 {
                    SEEN[slot as usize].cpu_us
                } else {
                    cpu_us
                };
                SEEN[slot as usize].port = ports[i];
                SEEN[slot as usize].cpu_us = cpu_us;
                if cpu_us > prev {
                    SEEN[slot as usize].period_us += cpu_us - prev;
                }
                if bi.run_state != TH_STATE_RUNNING || cpu_us <= prev {
                    continue;
                }
                if hot_only && ports[i] != hot {
                    continue;
                }
                let w64 = cpu_us - prev;
                let w = (if w64 > 4 * us as u64 {
                    4 * us as u64
                } else {
                    w64
                }) as u32;
                if thread_suspend(ports[i]) != KERN_SUCCESS {
                    continue;
                }
                let mut st: thread_state64 = core::mem::zeroed();
                let mut sc: mach_msg_type_number_t = ARM_THREAD_STATE64_COUNT;
                let kr = thread_get_state(
                    ports[i],
                    ARM_THREAD_STATE64,
                    &mut st as *mut _ as thread_state_t,
                    &mut sc,
                );
                let grip = (*cpus[i]).rip;
                let sop = (*cpus[i]).slow_op;
                thread_resume(ports[i]);
                if kr != KERN_SUCCESS {
                    continue;
                }
                let pc = arm_thread_state64_get_pc(&st) as *const c_void;
                let mut fi: OcerzJitFaultInfo = core::mem::zeroed();
                G_GP_SAMPLES += w as u64;
                if ffi::ocerz_jit_pc_in_arena(vm, pc) != 0
                    && ffi::ocerz_jit_fault_info(vm, pc, &mut fi) != 0
                {
                    G_GP_JIT += w as u64;
                    gp_bump(
                        (&raw mut G_GP_KEY) as *mut u64,
                        (&raw mut G_GP_CNT) as *mut u32,
                        if fi.block_rip != 0 { fi.block_rip } else { 1 },
                        w,
                    );
                } else {
                    gp_bump(
                        (&raw mut G_GP_KEY) as *mut u64,
                        (&raw mut G_GP_CNT) as *mut u32,
                        (if grip != 0 { grip } else { 1 }) | (1u64 << 63),
                        w,
                    );
                    gp_bump(
                        (&raw mut G_GH_KEY) as *mut u64,
                        (&raw mut G_GH_CNT) as *mut u32,
                        pc as u64,
                        w,
                    );
                    if sop & 0xffff != 0 {
                        gp_bump(
                            (&raw mut G_GF_KEY) as *mut u64,
                            (&raw mut G_GF_CNT) as *mut u32,
                            sop,
                            w,
                        );
                    }
                }
            }
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if now >= next {
                if hot_only {
                    let mut best = 0u64;
                    for k in 0..256 {
                        if SEEN[k].port != 0 && SEEN[k].period_us > best {
                            best = SEEN[k].period_us;
                            hot = SEEN[k].port;
                        }
                        SEEN[k].period_us = 0;
                    }
                    libc::fprintf(
                        stderr(),
                        c"ocerz: GUESTPROF[%d] next period samples only thread %#x (%.0f%% of a core)\n"
                            .as_ptr(),
                        libc::getpid(),
                        hot,
                        100.0 * best as f64 / ((now - pstart) as f64 / 1000.0 + 1.0),
                    );
                }
                gp_report((now - start) as f64 / 1e9);
                next = now + period;
                pstart = now;
            }
            libc::pthread_mutex_unlock(&raw mut G_GP_LOCK);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guestprof_final() {
    unsafe {
        if G_GP_VM.is_null() {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_GP_LOCK);
        if G_GP_SAMPLES != 0 {
            gp_report((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - G_GP_START) as f64 / 1e9);
        }
        libc::pthread_mutex_unlock(&raw mut G_GP_LOCK);
    }
}

extern "C" fn guestprof_atexit_safe() {
    unsafe {
        ocerz_guestprof_final();
    }
}

pub(super) unsafe fn guestprof_start(vm: *mut OcerzVM) {
    unsafe {
        static mut STARTED: c_int = 0;
        if STARTED != 0 || vm.is_null() || libc::getenv(c"OCERZ_GUESTPROF".as_ptr()).is_null() {
            return;
        }
        STARTED = 1;
        G_GP_VM = vm;
        G_GP_START = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        ocerz_jit_time_xlat = 1;
        libc::atexit(guestprof_atexit_safe);
        let mut t: libc::pthread_t = 0;
        if libc::pthread_create(&mut t, ptr::null(), guestprof_thread, vm as *mut c_void) == 0 {
            libc::pthread_detach(t);
        }
    }
}
