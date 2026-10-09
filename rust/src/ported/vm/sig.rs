use core::ffi::{c_char, c_int, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use super::*;
use crate::ffi::OcerzJitFaultInfo;
use libc::siginfo_t;

pub(super) unsafe extern "C" fn ripdump_handler(
    _sig: c_int,
    _si: *mut siginfo_t,
    _ctx: *mut c_void,
) {
    unsafe {
        if G_BTRACE_ON > 0 {
            AtomicI32::from_ptr(&raw mut G_BTRACE_REQ).store(1, Ordering::Release);
        }
        {
            static LAST_FAN: AtomicU64 = AtomicU64::new(0);
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            let mut prev = LAST_FAN.load(Ordering::Relaxed);
            if now - prev > 1000000000
                && LAST_FAN
                    .compare_exchange(prev, now, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                let self_ = pthread_self();
                for i in 0..G_CPUS_N {
                    if pthread_equal(G_CPU_THREADS[i as usize], self_) == 0 {
                        libc::pthread_kill(G_CPU_THREADS[i as usize], SIGUSR1);
                    }
                }
            }
        }
        if G_CUR_CPU.is_null() {
            return;
        }
        let mut b = [0u8; 640];
        let mut p = b.as_mut_ptr() as *mut c_char;
        {
            static ONCE: AtomicI32 = AtomicI32::new(0);
            if ONCE
                .compare_exchange(0, 1, Ordering::Relaxed, Ordering::Relaxed)
                .is_ok()
            {
                let mut names: mach_port_name_array_t = ptr::null_mut();
                let mut types: mach_port_type_array_t = ptr::null_mut();
                let mut nn: mach_msg_type_number_t = 0;
                let mut nt: mach_msg_type_number_t = 0;
                if mach_port_names(mach_task_self(), &mut names, &mut nn, &mut types, &mut nt)
                    == KERN_SUCCESS
                {
                    for i in 0..nn {
                        if *types.add(i as usize) & MACH_PORT_TYPE_PORT_SET == 0 {
                            continue;
                        }
                        let mut rb = [0u8; 512];
                        let mut q = rb.as_mut_ptr() as *mut c_char;
                        q = str_into(q, c"ocerz: PORTSET ".as_ptr());
                        q = hex_into(q, *names.add(i as usize) as u64);
                        q = str_into(q, c" ->".as_ptr());
                        let mut mem: mach_port_name_array_t = ptr::null_mut();
                        let mut nm: mach_msg_type_number_t = 0;
                        if mach_port_get_set_status(
                            mach_task_self(),
                            *names.add(i as usize),
                            &mut mem,
                            &mut nm,
                        ) == KERN_SUCCESS
                        {
                            let mut j = 0u32;
                            while j < nm && j < 32 {
                                q = str_into(q, c" ".as_ptr());
                                q = hex_into(q, *mem.add(j as usize) as u64);
                                let mut pst: mach_port_status = core::mem::zeroed();
                                let mut pn: mach_msg_type_number_t = MACH_PORT_RECEIVE_STATUS_COUNT;
                                if mach_port_get_attributes(
                                    mach_task_self(),
                                    *mem.add(j as usize),
                                    MACH_PORT_RECEIVE_STATUS,
                                    &mut pst as *mut _ as mach_port_info_t,
                                    &mut pn,
                                ) == KERN_SUCCESS
                                    && pst.mps_msgcount != 0
                                {
                                    q = str_into(q, c":n=".as_ptr());
                                    q = hex_into(q, pst.mps_msgcount as u64);
                                }
                                j += 1;
                            }
                            if !mem.is_null() {
                                mach_vm_deallocate(
                                    mach_task_self(),
                                    mem as mach_vm_address_t,
                                    (nm as usize * core::mem::size_of::<mach_port_name_t>())
                                        as mach_vm_size_t,
                                );
                            }
                        }
                        q = str_into(q, c"\n".as_ptr());
                        write(
                            2,
                            rb.as_ptr() as *const c_void,
                            q as usize - rb.as_ptr() as usize,
                        );
                    }
                    mach_vm_deallocate(
                        mach_task_self(),
                        names as mach_vm_address_t,
                        (nn as usize * core::mem::size_of::<mach_port_name_t>()) as mach_vm_size_t,
                    );
                    mach_vm_deallocate(
                        mach_task_self(),
                        types as mach_vm_address_t,
                        (nt as usize * core::mem::size_of::<mach_port_type_t>()) as mach_vm_size_t,
                    );
                }
                for fd in 0..96 {
                    let mut st: libc::stat = core::mem::zeroed();
                    let mut nread: c_int = 0;
                    if libc::fstat(fd, &mut st) != 0 || (st.st_mode & libc::S_IFMT) != libc::S_IFIFO
                    {
                        continue;
                    }
                    if libc::ioctl(fd, libc::FIONREAD, &mut nread) != 0 {
                        continue;
                    }
                    if nread == 0 {
                        continue;
                    }
                    let mut fb = [0u8; 96];
                    let mut fq = fb.as_mut_ptr() as *mut c_char;
                    fq = str_into(fq, c"ocerz: PIPE fd=".as_ptr());
                    fq = hex_into(fq, fd as u64);
                    fq = str_into(fq, c" nread=".as_ptr());
                    fq = hex_into(fq, nread as u64);
                    fq = str_into(fq, c"\n".as_ptr());
                    write(
                        2,
                        fb.as_ptr() as *const c_void,
                        fq as usize - fb.as_ptr() as usize,
                    );
                }
                ONCE.store(0, Ordering::Relaxed);
            }
        }
        if (*G_CUR_CPU).last_rcv_name != 0 {
            let mut rb = [0u8; 400];
            let mut q = rb.as_mut_ptr() as *mut c_char;
            q = str_into(q, c"ocerz: RIPDUMP rcv-port=".as_ptr());
            q = hex_into(q, (*G_CUR_CPU).last_rcv_name as u64);
            let mut members: mach_port_name_array_t = ptr::null_mut();
            let mut nmem: mach_msg_type_number_t = 0;
            if mach_port_get_set_status(
                mach_task_self(),
                (*G_CUR_CPU).last_rcv_name,
                &mut members,
                &mut nmem,
            ) == KERN_SUCCESS
            {
                q = str_into(q, c" set-members:".as_ptr());
                let mut i = 0u32;
                while i < nmem && i < 24 {
                    q = str_into(q, c" ".as_ptr());
                    q = hex_into(q, *members.add(i as usize) as u64);
                    i += 1;
                }
                if !members.is_null() {
                    mach_vm_deallocate(
                        mach_task_self(),
                        members as mach_vm_address_t,
                        (nmem as usize * core::mem::size_of::<mach_port_name_t>())
                            as mach_vm_size_t,
                    );
                }
            } else {
                q = str_into(q, c" (not-a-set)".as_ptr());
            }
            q = str_into(q, c"\n".as_ptr());
            write(
                2,
                rb.as_ptr() as *const c_void,
                q as usize - rb.as_ptr() as usize,
            );
        }
        p = str_into(p, c"ocerz: RIPDUMP cpu#".as_ptr());
        p = hex_into(p, (*G_CUR_CPU).cpu_number as u64);
        p = str_into(p, c" slot3=".as_ptr());
        p = hex_into(
            p,
            if ocerz_addr_readable((*G_CUR_CPU).gs_base + 0x18) != 0 {
                ocerz_ld((*G_CUR_CPU).gs_base + 0x18, 8)
            } else {
                0
            },
        );
        p = str_into(p, c" ctid=".as_ptr());
        p = hex_into(
            p,
            if ocerz_addr_readable((*G_CUR_CPU).gs_base.wrapping_sub(8)) != 0 {
                ocerz_ld((*G_CUR_CPU).gs_base.wrapping_sub(8), 8)
            } else {
                0
            },
        );
        p = str_into(p, c" rip=".as_ptr());
        p = hex_into(p, (*G_CUR_CPU).rip);
        p = str_into(p, c" gs=".as_ptr());
        p = hex_into(p, (*G_CUR_CPU).gs_base);
        p = str_into(p, c" hist:".as_ptr());
        for i in 1..=12u32 {
            p = str_into(p, c" ".as_ptr());
            p = hex_into(p, G_RIPHIST[(G_RIPHIST_N.wrapping_sub(i) & 31) as usize]);
        }
        *p = '\n' as c_char;
        p = p.add(1);
        write(
            2,
            b.as_ptr() as *const c_void,
            p as usize - b.as_ptr() as usize,
        );
        p = b.as_mut_ptr() as *mut c_char;
        static mut RN: [*const c_char; 16] = [
            c"rax".as_ptr(),
            c"rcx".as_ptr(),
            c"rdx".as_ptr(),
            c"rbx".as_ptr(),
            c"rsp".as_ptr(),
            c"rbp".as_ptr(),
            c"rsi".as_ptr(),
            c"rdi".as_ptr(),
            c"r8".as_ptr(),
            c"r9".as_ptr(),
            c"r10".as_ptr(),
            c"r11".as_ptr(),
            c"r12".as_ptr(),
            c"r13".as_ptr(),
            c"r14".as_ptr(),
            c"r15".as_ptr(),
        ];
        p = str_into(p, c"ocerz:   regs".as_ptr());
        for i in 0..16 {
            p = str_into(p, c" ".as_ptr());
            p = str_into(p, RN[i]);
            p = str_into(p, c"=".as_ptr());
            p = hex_into(p, (*G_CUR_CPU).gpr[i]);
        }
        *p = '\n' as c_char;
        p = p.add(1);
        write(
            2,
            b.as_ptr() as *const c_void,
            p as usize - b.as_ptr() as usize,
        );
        {
            static mut MBASE: u64 = 0;
            static mut MINIT: c_int = 0;
            if MINIT == 0 {
                let e = libc::getenv(c"OCERZ_MACDRVDUMP".as_ptr());
                MBASE = if !e.is_null() {
                    libc::strtoull(e, ptr::null_mut(), 0)
                } else {
                    0
                };
                MINIT = 1;
            }
            static MONCE: AtomicI32 = AtomicI32::new(0);
            if MBASE != 0
                && MONCE
                    .compare_exchange(0, 1, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                let mut mb = [0u8; 512];
                let mut mq = mb.as_mut_ptr() as *mut c_char;
                mq = str_into(mq, c"ocerz: MACDRV token=".as_ptr());
                mq = hex_into(
                    mq,
                    if ocerz_addr_readable(MBASE + 0x560f8) != 0 {
                        ocerz_ld(MBASE + 0x560f8, 8)
                    } else {
                        0
                    },
                );
                mq = str_into(mq, c" ctrl=".as_ptr());
                let slot = MBASE + 0x560f0;
                let ctrl = if ocerz_addr_readable(slot) != 0 {
                    ocerz_ld(slot, 8)
                } else {
                    0
                };
                mq = hex_into(mq, ctrl);
                if ctrl != 0
                    && ocerz_addr_readable(ctrl) != 0
                    && ocerz_addr_readable(ctrl + 0x1f) != 0
                {
                    let src = ocerz_ld(ctrl + 0x8, 8);
                    let arr = ocerz_ld(ctrl + 0x10, 8);
                    let mnq = ocerz_ld(ctrl + 0x18, 8);
                    mq = str_into(mq, c" src=".as_ptr());
                    mq = hex_into(mq, src);
                    mq = str_into(mq, c" arr=".as_ptr());
                    mq = hex_into(mq, arr);
                    mq = str_into(mq, c" q=".as_ptr());
                    mq = hex_into(mq, mnq);
                    if src != 0
                        && ocerz_addr_readable(src) != 0
                        && ocerz_addr_readable(src + 0x5f) != 0
                    {
                        mq = str_into(mq, c" src.info=".as_ptr());
                        mq = hex_into(mq, ocerz_ld(src + 8, 8));
                        mq = str_into(mq, c" src.sig58=".as_ptr());
                        mq = hex_into(mq, ocerz_ld(src + 0x58, 8));
                    }
                    if arr != 0
                        && ocerz_addr_readable(arr) != 0
                        && ocerz_addr_readable(arr + 0x2f) != 0
                    {
                        mq = str_into(mq, c" arrw:".as_ptr());
                        for k in 0..6u64 {
                            mq = str_into(mq, c" ".as_ptr());
                            mq = hex_into(mq, ocerz_ld(arr + 8 * k, 8));
                        }
                        let st_ = ocerz_ld(arr + 0x10, 8);
                        if st_ != 0
                            && ocerz_addr_readable(st_) != 0
                            && ocerz_addr_readable(st_ + 0x2f) != 0
                        {
                            mq = str_into(mq, c" deque:".as_ptr());
                            for k in 0..6u64 {
                                mq = str_into(mq, c" ".as_ptr());
                                mq = hex_into(mq, ocerz_ld(st_ + 8 * k, 8));
                            }
                            for k in 0..4u64 {
                                let obj = ocerz_ld(st_ + 8 * k, 8);
                                if obj != 0
                                    && ocerz_addr_readable(obj) != 0
                                    && ocerz_addr_readable(obj + 0x17) != 0
                                {
                                    mq = str_into(mq, c" inv".as_ptr());
                                    mq = hex_into(mq, k);
                                    mq = str_into(mq, c"=".as_ptr());
                                    mq = hex_into(mq, ocerz_ld(obj + 0x10, 8));
                                }
                            }
                            {
                                let w3 = ocerz_ld(arr + 0x18, 8);
                                let w4 = ocerz_ld(arr + 0x20, 8);
                                let cnt = w4 >> 32;
                                let head = w3 & 0xffffffff;
                                let cap = w3 >> 32;
                                mq = str_into(mq, c" COUNT=".as_ptr());
                                mq = hex_into(mq, cnt);
                                mq = str_into(mq, c" head=".as_ptr());
                                mq = hex_into(mq, head);
                                mq = str_into(mq, c" cap=".as_ptr());
                                mq = hex_into(mq, cap);
                                if st_ != 0 && cap != 0 && ocerz_addr_readable(st_) != 0 {
                                    mq = str_into(mq, c" elems:".as_ptr());
                                    let mut e = 0u64;
                                    while e < cnt && e < 6 {
                                        let idx = if cap != 0 { (head + e) % cap } else { e };
                                        let el = ocerz_ld(st_ + 8 * idx, 8);
                                        mq = str_into(mq, c" ".as_ptr());
                                        mq = hex_into(mq, el);
                                        if el != 0 && ocerz_addr_readable(el + 0x17) != 0 {
                                            mq = str_into(mq, c"/inv=".as_ptr());
                                            mq = hex_into(mq, ocerz_ld(el + 0x10, 8));
                                        }
                                        e += 1;
                                    }
                                }
                            }
                        }
                    }
                }
                mq = str_into(mq, c"\n".as_ptr());
                write(
                    2,
                    mb.as_ptr() as *const c_void,
                    mq as usize - mb.as_ptr() as usize,
                );
                if !libc::getenv(c"OCERZ_UNFREEZE".as_ptr()).is_null()
                    && ctrl != 0
                    && ocerz_addr_readable(ctrl + 8) != 0
                {
                    let fsrc = ocerz_ld(ctrl + 8, 8);
                    if fsrc != 0 && ocerz_addr_readable(fsrc + 0x58) != 0 {
                        ocerz_st(fsrc + 0x58, 8, 0x123456789ab);
                        let msg = c"ocerz: UNFREEZE signal word forced\n";
                        write(2, msg.as_ptr() as *const c_void, msg.count_bytes());
                    }
                }
                {
                    let inv1 = MBASE + 0x11a50;
                    let inv2 = MBASE + 0x1e770;
                    let mut ctrl_isa = 0u64;
                    {
                        let sl = MBASE + 0x560f0;
                        let c0 = if ocerz_addr_readable(sl) != 0 {
                            ocerz_ld(sl, 8)
                        } else {
                            0
                        };
                        if c0 != 0 && ocerz_addr_readable(c0) != 0 {
                            ctrl_isa = ocerz_ld(c0, 8);
                        }
                    }
                    static RANGES: [[u64; 2]; 2] =
                        [[0x100000000, 0x140000000], [0x7040000000, 0x7070000000]];
                    for ri in 0..2 {
                        let mut page = RANGES[ri][0];
                        while page < RANGES[ri][1] {
                            if ocerz_addr_readable(page) != 0 {
                                let mut off = 0u64;
                                while off < 0x1000 {
                                    let a = page + off;
                                    let w = ocerz_ld(a, 8);
                                    if ctrl_isa != 0 && w == ctrl_isa && off == 0 {
                                        let mut cb = [0u8; 80];
                                        let mut cq = cb.as_mut_ptr() as *mut c_char;
                                        cq = str_into(cq, c"ocerz: CTRLOBJ ".as_ptr());
                                        cq = hex_into(cq, a);
                                        cq = str_into(cq, c"\n".as_ptr());
                                        write(
                                            2,
                                            cb.as_ptr() as *const c_void,
                                            cq as usize - cb.as_ptr() as usize,
                                        );
                                    }
                                    if w == inv1 || w == inv2 {
                                        let blk = a - 0x10;
                                        let isa = ocerz_ld(blk, 8);
                                        let fl = ocerz_ld(blk + 8, 8);
                                        let mut hb = [0u8; 300];
                                        let mut hq = hb.as_mut_ptr() as *mut c_char;
                                        hq = str_into(hq, c"ocerz: WRAPBLK ".as_ptr());
                                        hq = hex_into(hq, blk);
                                        hq = str_into(
                                            hq,
                                            if w == inv1 {
                                                c" kind=wrapper".as_ptr()
                                            } else {
                                                c" kind=thunk".as_ptr()
                                            },
                                        );
                                        hq = str_into(hq, c" isa=".as_ptr());
                                        hq = hex_into(hq, isa);
                                        hq = str_into(hq, c" fl=".as_ptr());
                                        hq = hex_into(hq, fl);
                                        hq = str_into(hq, c" caps:".as_ptr());
                                        for k in 0..5u64 {
                                            hq = str_into(hq, c" ".as_ptr());
                                            hq = hex_into(
                                                hq,
                                                if ocerz_addr_readable(blk + 0x20 + 8 * k) != 0 {
                                                    ocerz_ld(blk + 0x20 + 8 * k, 8)
                                                } else {
                                                    0
                                                },
                                            );
                                        }
                                        hq = str_into(hq, c"\n".as_ptr());
                                        write(
                                            2,
                                            hb.as_ptr() as *const c_void,
                                            hq as usize - hb.as_ptr() as usize,
                                        );
                                        if w == inv1 && fl as u32 == 0xc3000002 {
                                            for rj in 0..2 {
                                                let mut p2 = RANGES[rj][0];
                                                while p2 < RANGES[rj][1] {
                                                    if ocerz_addr_readable(p2) != 0 {
                                                        let mut o2 = 0u64;
                                                        while o2 < 0x1000 {
                                                            if ocerz_ld(p2 + o2, 8) == blk {
                                                                let mut rb2 = [0u8; 100];
                                                                let mut rq =
                                                                    rb2.as_mut_ptr() as *mut c_char;
                                                                rq = str_into(
                                                                    rq,
                                                                    c"ocerz: WRAPREF holder="
                                                                        .as_ptr(),
                                                                );
                                                                rq = hex_into(rq, p2 + o2);
                                                                rq = str_into(rq, c"\n".as_ptr());
                                                                write(
                                                                    2,
                                                                    rb2.as_ptr() as *const c_void,
                                                                    rq as usize
                                                                        - rb2.as_ptr() as usize,
                                                                );
                                                            }
                                                            o2 += 8;
                                                        }
                                                    }
                                                    p2 += 0x1000;
                                                }
                                            }
                                        }
                                    }
                                    off += 16;
                                }
                            }
                            page += 0x1000;
                        }
                    }
                }
                MONCE.store(0, Ordering::Relaxed);
            }
        }
        {
            p = b.as_mut_ptr() as *mut c_char;
            p = str_into(p, c"ocerz:   gbt".as_ptr());
            let mut fp = (*G_CUR_CPU).gpr[5];
            for _i in 0..24 {
                if fp == 0
                    || (fp & 7) != 0
                    || ocerz_addr_readable(fp) == 0
                    || ocerz_addr_readable(fp + 15) == 0
                {
                    break;
                }
                let ra = ocerz_ld(fp + 8, 8);
                let nfp = ocerz_ld(fp, 8);
                if ra == 0 {
                    break;
                }
                {
                    static mut WBASE: u64 = 0;
                    static mut WINIT: c_int = 0;
                    if WINIT == 0 {
                        let e = libc::getenv(c"OCERZ_MACDRVDUMP".as_ptr());
                        WBASE = if !e.is_null() {
                            libc::strtoull(e, ptr::null_mut(), 0)
                        } else {
                            0
                        };
                        WINIT = 1;
                    }
                    if WBASE != 0
                        && ra == WBASE + 0x1e763
                        && ocerz_addr_readable(fp - 0x40) != 0
                        && ocerz_addr_readable(fp - 0x39) != 0
                    {
                        let fwd = ocerz_ld(fp - 0x40, 8);
                        let mut fb = [0u8; 200];
                        let mut fq = fb.as_mut_ptr() as *mut c_char;
                        fq = str_into(fq, c"\nocerz:   FINISHED cell=".as_ptr());
                        fq = hex_into(fq, fp - 0x48);
                        fq = str_into(fq, c" fwd=".as_ptr());
                        fq = hex_into(fq, fwd);
                        if fwd != 0
                            && ocerz_addr_readable(fwd) != 0
                            && ocerz_addr_readable(fwd + 0x1f) != 0
                        {
                            fq = str_into(fq, c" flags=".as_ptr());
                            fq = hex_into(fq, ocerz_ld(fwd + 0x10, 8));
                            fq = str_into(fq, c" val=".as_ptr());
                            fq = hex_into(fq, ocerz_ld(fwd + 0x18, 1));
                        }
                        fq = str_into(fq, c" stackval=".as_ptr());
                        fq = hex_into(fq, ocerz_ld(fp - 0x48 + 0x18, 1));
                        fq = str_into(fq, c"\n".as_ptr());
                        write(
                            2,
                            fb.as_ptr() as *const c_void,
                            fq as usize - fb.as_ptr() as usize,
                        );
                    }
                }
                p = str_into(p, c" ".as_ptr());
                p = hex_into(p, ra);
                if nfp <= fp {
                    break;
                }
                fp = nfp;
                if p as usize - b.as_ptr() as usize > 640 - 40 {
                    break;
                }
            }
            *p = '\n' as c_char;
            p = p.add(1);
            write(
                2,
                b.as_ptr() as *const c_void,
                p as usize - b.as_ptr() as usize,
            );
        }
        for i in 0..24u32 {
            let r = G_RIPHIST[(G_RIPHIST_N.wrapping_sub(1).wrapping_sub(i) & 31) as usize];
            if r == 0 {
                continue;
            }
            let mut dup = false;
            for j in 0..i {
                if G_RIPHIST[(G_RIPHIST_N.wrapping_sub(1).wrapping_sub(j) & 31) as usize] == r {
                    dup = true;
                    break;
                }
            }
            if dup {
                continue;
            }
            let mut x = [0u8; 160];
            let mut q = x.as_mut_ptr() as *mut c_char;
            q = str_into(q, c"ocerz:   @".as_ptr());
            q = hex_into(q, r);
            if ocerz_addr_readable(r) == 0 || ocerz_addr_readable(r + 15) == 0 {
                q = str_into(q, c" (uncommitted)\n".as_ptr());
                write(
                    2,
                    x.as_ptr() as *const c_void,
                    q as usize - x.as_ptr() as usize,
                );
                continue;
            }
            q = str_into(q, c" =".as_ptr());
            for k in 0..16u64 {
                let by = ocerz_ld(r + k, 1);
                *q = ' ' as c_char;
                q = q.add(1);
                *q = b"0123456789abcdef"[((by >> 4) & 0xf) as usize] as c_char;
                q = q.add(1);
                *q = b"0123456789abcdef"[(by & 0xf) as usize] as c_char;
                q = q.add(1);
            }
            *q = '\n' as c_char;
            q = q.add(1);
            write(
                2,
                x.as_ptr() as *const c_void,
                q as usize - x.as_ptr() as usize,
            );
        }
    }
}

pub(super) unsafe fn wild_dump() {
    unsafe {
        ffi::ocerz_cpu_dump(G_CUR_CPU, stderr() as *mut ffi::FILE);
        let rsp = (*G_CUR_CPU).gpr[OCERZ_RSP];
        libc::fprintf(
            stderr(),
            c"ocerz: WILD-STACK rsp=%#llx:".as_ptr(),
            rsp as c_ulonglong,
        );
        for i in 0..24u64 {
            libc::fprintf(
                stderr(),
                c" %#llx".as_ptr(),
                if ocerz_addr_readable(rsp + 8 * i) != 0 {
                    ocerz_ld(rsp + 8 * i, 8)
                } else {
                    0
                } as c_ulonglong,
            );
        }
        libc::fprintf(stderr(), c"\nocerz: WILD-RIPHIST:".as_ptr());
        for i in 1..=16u32 {
            libc::fprintf(
                stderr(),
                c" %#llx".as_ptr(),
                G_RIPHIST[(G_RIPHIST_N.wrapping_sub(i) & 31) as usize] as c_ulonglong,
            );
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
}

#[thread_local]
static mut CRASH_DEPTH: c_int = 0;
#[thread_local]
static mut LAST_ALIAS_PAGE: u64 = 0;
#[thread_local]
static mut RETRY_ADDR: u64 = 0;
#[thread_local]
static mut RETRY_N: c_int = 0;
static REWIND_OFF: AtomicI32 = AtomicI32::new(-1);
static FAULTLOG: AtomicI32 = AtomicI32::new(-1);
static FAULT_DREG: AtomicI32 = AtomicI32::new(-2);
static mut FAULT_DB: [u8; 4096] = [0; 4096];
static mut FAULT_SB: [u8; 2048] = [0; 2048];
static WILD_LOGS: AtomicU32 = AtomicU32::new(0);
static CPUREG_RECOV: AtomicU32 = AtomicU32::new(0);

pub(super) unsafe extern "C" fn crash_handler(sig: c_int, si: *mut siginfo_t, ctx: *mut c_void) {
    unsafe {
        let mut align_fault = false;
        if sig == SIGSEGV || sig == SIGBUS {
            let luc = ctx as *const ucontext_t;
            let lesr = if !luc.is_null() {
                (*(*luc).uc_mcontext).es.esr as u64
            } else {
                0
            };
            align_fault = sig == SIGBUS && (lesr & 0x3f) == 0x21;
            if !align_fault && ffi::ocerz_cache_lazy_fault((*si).si_addr as usize) != 0 {
                return;
            }
        }
        if !ocerz_jit_decode_recover.is_null() {
            siglongjmp(ocerz_jit_decode_recover, 1);
        }
        if (sig == SIGSEGV || sig == SIGBUS)
            && !ctx.is_null()
            && ocerz_mode == ffi::OCERZ_MODE_NATIVE as c_int
        {
            let suc = ctx as *mut ucontext_t;
            let spc = (*(*suc).uc_mcontext).ss.pc;
            if (*si).si_addr as u64 == spc
                && ffi::ocerz_objcbridge_swift_destroy_fault(spc, (*(*suc).uc_mcontext).ss.x[20])
                    != 0
            {
                (*(*suc).uc_mcontext).ss.x[0] = (*(*suc).uc_mcontext).ss.x[20];
                (*(*suc).uc_mcontext).ss.x[1] = spc;
                (*(*suc).uc_mcontext).ss.pc = ffi::ocerz_objcbridge_swift_destroy as u64;
                return;
            }
        }
        if (sig == SIGSEGV || sig == SIGBUS)
            && !align_fault
            && !G_VM.is_null()
            && ocerz_host_in_guest_space((*si).si_addr) != 0
        {
            let ga = ocerz_h2g((*si).si_addr);
            let page = ga & !0x3fffu64;
            if ga < ffi::OCERZ_LOW_LIMIT && page != LAST_ALIAS_PAGE && ocerz_addr_readable(ga) == 0
            {
                let mut hprot: c_int = 0;
                if ocerz_host_region_is_device(ga, &mut hprot) != 0 {
                    LAST_ALIAS_PAGE = page;
                    if ocerz_alias_raw_region(G_VM, ga) == 0 && ocerz_addr_readable(ga) != 0 {
                        if G_SIGTRACE != 0 {
                            let mut ab = [0u8; 120];
                            let mut w = ab.as_mut_ptr() as *mut c_char;
                            w = str_into(w, c"ocerz: RAWALIAS-ON-FAULT gaddr=".as_ptr());
                            w = hex_into(w, ga);
                            w = str_into(w, c"\n".as_ptr());
                            write(
                                2,
                                ab.as_ptr() as *const c_void,
                                w as usize - ab.as_ptr() as usize,
                            );
                        }
                        return;
                    }
                }
            }
        }

        if (sig == SIGSEGV || sig == SIGBUS)
            && CRASH_DEPTH == 0
            && !G_CUR_CPU.is_null()
            && !G_SIG_RECOVER.is_null()
            && !ctx.is_null()
        {
            let uc = ctx as *const ucontext_t;
            let hpc = (*(*uc).uc_mcontext).ss.pc as *const u32;
            let fvm = (*G_CUR_CPU).vm;
            if !fvm.is_null()
                && ffi::ocerz_jit_pc_in_arena(fvm, hpc as *const c_void) != 0
                && (*hpc & 0xffff83e0) == 0xa9bf03e0
                && ((*si).si_addr as u64).wrapping_sub((*(*uc).uc_mcontext).ss.sp - 16) < 32
            {
                CRASH_DEPTH = 1;
                ffi::ocerz_jit_fault_recover_regs(
                    fvm,
                    hpc as *const c_void,
                    (*(*uc).uc_mcontext).ss.x.as_ptr(),
                    G_CUR_CPU,
                );
                ffi::ocerz_jit_fault_recover_xmm(
                    fvm,
                    hpc as *const c_void,
                    (*(*uc).uc_mcontext).ns.v.as_ptr() as *const c_void,
                    G_CUR_CPU,
                );
                ffi::ocerz_jit_fault_recover_flags(fvm, hpc as *const c_void, G_CUR_CPU);
                ffi::ocerz_flags_materialize(G_CUR_CPU);
                let mut jrip: u64 = 0;
                if ffi::ocerz_jit_fault_rip(fvm, hpc as *const c_void, &mut jrip) != 0 {
                    (*G_CUR_CPU).rip = jrip;
                    (*G_CUR_CPU).sig_repeat = 0;
                    (*G_CUR_CPU).interp_once = 1;
                    if env_cache!("OCERZ_RASLOG") != 0 {
                        libc::fprintf(
                            stderr(),
                            c"ocerz: host RAS overflow[%d] at rip=%#llx sp=%#llx: CALL via interpreter\n"
                                .as_ptr(),
                            libc::getpid(),
                            jrip as c_ulonglong,
                            (*(*uc).uc_mcontext).ss.sp as c_ulonglong,
                        );
                    }
                    ocerz_recov_note(1, jrip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                CRASH_DEPTH = 0;
            }
        }

        if align_fault && CRASH_DEPTH == 0 && !ctx.is_null() {
            let uc = ctx as *const ucontext_t;
            let hpc = (*(*uc).uc_mcontext).ss.pc as *const c_void;
            let pvm = if !G_CUR_CPU.is_null() {
                (*G_CUR_CPU).vm
            } else {
                G_VM
            };
            if !pvm.is_null() && ffi::ocerz_jit_pc_in_arena(pvm, hpc) != 0 {
                let hp = ffi::ocerz_jit_hotpatch_align(pvm, hpc);
                if hp != 0 {
                    if env_cache!("OCERZ_ALFAULTLOG") != 0 {
                        libc::fprintf(
                            stderr(),
                            c"ocerz: ALFAULT hotpatch=%d hpc=%p addr=%p\n".as_ptr(),
                            hp,
                            hpc,
                            (*si).si_addr,
                        );
                    }
                    return;
                }
            }
        }
        if align_fault
            && CRASH_DEPTH == 0
            && !G_CUR_CPU.is_null()
            && !G_SIG_RECOVER.is_null()
            && !ctx.is_null()
        {
            let uc = ctx as *const ucontext_t;
            let hpc = (*(*uc).uc_mcontext).ss.pc as *const c_void;
            let fvm = (*G_CUR_CPU).vm;
            if !fvm.is_null() && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0 {
                CRASH_DEPTH = 1;
                ffi::ocerz_jit_fault_recover_regs(
                    fvm,
                    hpc,
                    (*(*uc).uc_mcontext).ss.x.as_ptr(),
                    G_CUR_CPU,
                );
                ffi::ocerz_jit_fault_recover_xmm(
                    fvm,
                    hpc,
                    (*(*uc).uc_mcontext).ns.v.as_ptr() as *const c_void,
                    G_CUR_CPU,
                );
                ffi::ocerz_jit_fault_recover_flags(fvm, hpc, G_CUR_CPU);
                ffi::ocerz_flags_materialize(G_CUR_CPU);
                let mut jrip: u64 = 0;
                if ffi::ocerz_jit_fault_rip(fvm, hpc, &mut jrip) != 0
                    && ffi::ocerz_jit_note_align_fault(fvm, hpc, jrip) != 0
                {
                    (*G_CUR_CPU).rip = jrip;
                    (*G_CUR_CPU).sig_repeat = 0;
                    (*G_CUR_CPU).interp_once = 1;
                    if env_cache!("OCERZ_ALFAULTLOG") != 0 {
                        libc::fprintf(
                            stderr(),
                            c"ocerz: ALFAULT[%d] rip=%#llx addr=%p\n".as_ptr(),
                            libc::getpid(),
                            jrip as c_ulonglong,
                            (*si).si_addr,
                        );
                    }
                    ocerz_recov_note(2, jrip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                CRASH_DEPTH = 0;
            }
        }

        if (sig == SIGSEGV || sig == SIGBUS)
            && !align_fault
            && CRASH_DEPTH == 0
            && !G_CUR_CPU.is_null()
            && !G_SIG_RECOVER.is_null()
            && !ctx.is_null()
        {
            let uc = ctx as *const ucontext_t;
            let esr = (*(*uc).uc_mcontext).es.esr as u64;
            let ec = ((esr >> 26) & 0x3f) as u32;
            let mut armed_hit: c_int = 0;
            if ec != 0x20
                && ec != 0x21
                && (esr & (1 << 6)) != 0
                && (ffi::ocerz_cache_write_fault((*si).si_addr as usize) != 0
                    || (host_addr_is_guest_page((*si).si_addr) != 0
                        && {
                            armed_hit = ffi::ocerz_mem_exec_write_fault(ocerz_h2g((*si).si_addr));
                            armed_hit != 0
                        }
                        && (armed_hit != 2
                            || (if RETRY_ADDR == (*si).si_addr as u64 {
                                RETRY_N += 1;
                                RETRY_N
                            } else {
                                RETRY_N = 1;
                                RETRY_ADDR = (*si).si_addr as u64;
                                1
                            }) <= 4)))
            {
                if armed_hit != 2 {
                    RETRY_N = 0;
                }
                let fvm = (*G_CUR_CPU).vm;
                let hpc = (*(*uc).uc_mcontext).ss.pc as *const c_void;
                let page = ocerz_h2g((*si).si_addr) & !(ffi::OCERZ_HOST_PAGE_SIZE as u64 - 1);
                let mut jrip: u64 = 0;
                let in_jit = !fvm.is_null()
                    && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0
                    && ffi::ocerz_jit_fault_rip(fvm, hpc, &mut jrip) != 0;
                if in_jit {
                    CRASH_DEPTH = 1;
                    ffi::ocerz_jit_fault_recover_regs(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ss.x.as_ptr(),
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_xmm(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ns.v.as_ptr() as *const c_void,
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_flags(fvm, hpc, G_CUR_CPU);
                    ffi::ocerz_flags_materialize(G_CUR_CPU);
                }
                if armed_hit != 2 {
                    ffi::ocerz_jit_invalidate_range(fvm, page, ffi::OCERZ_HOST_PAGE_SIZE as u64);
                }
                if env_cache!("OCERZ_CACHEPATCHLOG") != 0 {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: CACHEPATCH[%d] rip=%#llx addr=%p injit=%d\n".as_ptr(),
                        libc::getpid(),
                        jrip as c_ulonglong,
                        (*si).si_addr,
                        in_jit as c_int,
                    );
                }
                if in_jit {
                    (*G_CUR_CPU).rip = jrip;
                    (*G_CUR_CPU).sig_repeat = 0;
                    ocerz_recov_note(7, jrip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                return;
            }
        }

        if (sig == SIGSEGV || sig == SIGBUS)
            && !align_fault
            && CRASH_DEPTH == 0
            && !ctx.is_null()
            && !(false || !G_CUR_CPU.is_null() && !G_SIG_RECOVER.is_null())
            && !G_VM.is_null()
            && host_addr_is_guest_page((*si).si_addr) != 0
        {
            let uc = ctx as *const ucontext_t;
            let esr = (*(*uc).uc_mcontext).es.esr as u64;
            let ec = ((esr >> 26) & 0x3f) as u32;
            if ec != 0x20 && ec != 0x21 && (esr & (1 << 6)) != 0 {
                let ga = ocerz_h2g((*si).si_addr);
                let h = ffi::ocerz_mem_exec_write_fault(ga);
                if h == 1 {
                    ffi::ocerz_jit_invalidate_range(
                        G_VM,
                        ga & !(ffi::OCERZ_HOST_PAGE_SIZE as u64 - 1),
                        ffi::OCERZ_HOST_PAGE_SIZE as u64,
                    );
                }
                if h != 0 {
                    return;
                }
            }
        }

        if (sig == SIGSEGV || sig == SIGBUS)
            && !ctx.is_null()
            && ocerz_mode != ffi::OCERZ_MODE_NATIVE as c_int
        {
            let luc = ctx as *mut ucontext_t;
            let lpc = (*(*luc).uc_mcontext).ss.pc;
            let llr = (*(*luc).uc_mcontext).ss.lr;
            if ocerz_leaf_site(lpc, llr) != lpc {
                (*(*luc).uc_mcontext).ss.x[9] = 1;
                (*(*luc).uc_mcontext).ss.pc = llr & 0x0000ffffffffffff;
                return;
            }
        }

        if sig == SIGSEGV || sig == SIGBUS {
            let bf = ffi::ocerz_bridge_in_flight();
            if !bf.is_null() {
                let buc = ctx as *const ucontext_t;
                let bpc = if !buc.is_null() {
                    (*(*buc).uc_mcontext).ss.pc
                } else {
                    0
                };
                let blib = if !(*bf).lib.is_null() {
                    (*bf).lib
                } else {
                    c"?".as_ptr()
                };
                let bsym = if !(*bf).sym.is_null() {
                    (*bf).sym
                } else {
                    c"?".as_ptr()
                };
                let bsig = if !(*bf).sig.is_null() {
                    (*bf).sig
                } else {
                    c"?".as_ptr()
                };
                libc::fprintf(
                    stderr(),
                    c"ocerz: BRIDGE-FAULT[%d] %s inside a bridged call, not in guest code\n"
                        .as_ptr(),
                    libc::getpid(),
                    if sig == SIGBUS {
                        c"SIGBUS".as_ptr()
                    } else {
                        c"SIGSEGV".as_ptr()
                    },
                );
                libc::fprintf(
                    stderr(),
                    c"ocerz:   call=%s:%s sig='%s' host_fn=%p depth=%d\n".as_ptr(),
                    blib,
                    bsym,
                    bsig,
                    (*bf).host_fn as *const c_void,
                    (*bf).depth,
                );
                libc::fprintf(
                    stderr(),
                    c"ocerz:   fault_addr=%p host_pc=%#llx guest_rip=%#llx\n".as_ptr(),
                    (*si).si_addr,
                    bpc as c_ulonglong,
                    (if !G_CUR_CPU.is_null() {
                        (*G_CUR_CPU).cur_rip
                    } else {
                        0
                    }) as c_ulonglong,
                );
                {
                    let rip = if !G_CUR_CPU.is_null() {
                        (*G_CUR_CPU).cur_rip
                    } else {
                        0
                    };
                    let mut rbase: u64 = 0;
                    let mut fbase: u64 = 0;
                    let rname = if rip != 0 {
                        ocerz_dyld_name_for_addr(rip, &mut rbase)
                    } else {
                        ptr::null()
                    };
                    let fga = if ocerz_host_in_guest_space((*si).si_addr) != 0 {
                        ocerz_h2g((*si).si_addr)
                    } else {
                        0
                    };
                    let fname = if fga != 0 {
                        ocerz_dyld_name_for_addr(fga, &mut fbase)
                    } else {
                        ptr::null()
                    };
                    if !rname.is_null() {
                        libc::fprintf(
                            stderr(),
                            c"ocerz:   guest_rip is %s+%#llx\n".as_ptr(),
                            rname,
                            rip.wrapping_sub(rbase) as c_ulonglong,
                        );
                    }
                    if !fname.is_null() {
                        libc::fprintf(
                            stderr(),
                            c"ocerz:   fault_addr is %s+%#llx\n".as_ptr(),
                            fname,
                            fga.wrapping_sub(fbase) as c_ulonglong,
                        );
                    }
                    if !buc.is_null() {
                        let mut di: Dl_info = core::mem::zeroed();
                        let lr = (*(*buc).uc_mcontext).ss.lr;
                        if dladdr(lr as *const c_void, &mut di) != 0 && !di.dli_sname.is_null() {
                            libc::fprintf(
                                stderr(),
                                c"ocerz:   host_lr=%#llx %s in %s\n".as_ptr(),
                                lr as c_ulonglong,
                                di.dli_sname,
                                if !di.dli_fname.is_null() {
                                    di.dli_fname
                                } else {
                                    c"?".as_ptr()
                                },
                            );
                        } else {
                            libc::fprintf(
                                stderr(),
                                c"ocerz:   host_lr=%#llx\n".as_ptr(),
                                lr as c_ulonglong,
                            );
                        }
                    }
                }
                if bpc != 0
                    && bpc as *mut c_void == (*si).si_addr
                    && ocerz_host_in_guest_space((*si).si_addr) != 0
                {
                    libc::fprintf(
                        stderr(),
                        c"ocerz:   cause: native code jumped to %#llx, which is guest memory: it was handed an x86 function pointer no crossing converted\n"
                            .as_ptr(),
                        ocerz_h2g((*si).si_addr) as c_ulonglong,
                    );
                } else if ocerz_host_in_guest_space((*si).si_addr) != 0 {
                    libc::fprintf(
                        stderr(),
                        c"ocerz:   cause: the fault address is in guest space, so the guest passed a bad pointer to %s (guest addr %#llx)\nocerz:   note: in the identity map native mode uses that test is one upper bound, so it is strong evidence and not proof\n"
                            .as_ptr(),
                        bsym,
                        ocerz_h2g((*si).si_addr) as c_ulonglong,
                    );
                } else {
                    libc::fprintf(
                        stderr(),
                        c"ocerz:   cause: the fault address is outside guest space, so ocerz marshalled this crossing wrong; the guest's arguments are not implicated\n"
                            .as_ptr(),
                    );
                }
                libc::fprintf(
                    stderr(),
                    c"ocerz:   a native frame cannot be resumed or unwound, so the process stops here\n"
                        .as_ptr(),
                );
                libc::fflush(stderr());
                libc::_exit(139);
            }
        }
        let mut delivered = 0;
        if true {
            if CRASH_DEPTH == 0
                && !G_CUR_CPU.is_null()
                && !G_SIG_RECOVER.is_null()
                && ocerz_host_in_guest_space((*si).si_addr) != 0
            {
                CRASH_DEPTH = 1;
                let uc = ctx as *const ucontext_t;
                let hpc = if !uc.is_null() {
                    ocerz_leaf_site((*(*uc).uc_mcontext).ss.pc, (*(*uc).uc_mcontext).ss.lr)
                        as *const c_void
                } else {
                    ptr::null()
                };
                let fvm = (*G_CUR_CPU).vm;
                let in_jit =
                    !hpc.is_null() && !fvm.is_null() && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0;
                if in_jit {
                    ffi::ocerz_jit_fault_recover_regs(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ss.x.as_ptr(),
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_xmm(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ns.v.as_ptr() as *const c_void,
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_flags(fvm, hpc, G_CUR_CPU);
                }
                ffi::ocerz_flags_materialize(G_CUR_CPU);
                let mut fault_rip = (*G_CUR_CPU).cur_rip;
                let mut rip_exact = 1;
                if in_jit {
                    let mut jrip: u64 = 0;
                    if ffi::ocerz_jit_fault_rip(fvm, hpc, &mut jrip) != 0 {
                        fault_rip = jrip;
                    } else {
                        rip_exact = 0;
                    }
                }
                let gs = (*G_CUR_CPU).gs_base;
                let gaddr = ocerz_h2g((*si).si_addr);
                let code = if ocerz_addr_committed(gaddr) == 1 {
                    2
                } else {
                    1
                };

                let esr = if !ctx.is_null() {
                    (*(*(ctx as *const ucontext_t)).uc_mcontext).es.esr as u64
                } else {
                    0
                };
                let ec = ((esr >> 26) & 0x3f) as u32;
                let is_fetch = ec == 0x20 || ec == 0x21;
                let is_write = !is_fetch && (esr & (1 << 6)) != 0;
                let mut err = 0x4u32;
                if code == 2 {
                    err |= 0x1;
                }
                if is_write {
                    err |= 0x2;
                }
                if is_fetch {
                    err |= 0x10;
                }

                if gaddr != (*G_CUR_CPU).sig_last_fault {
                    (*G_CUR_CPU).sig_last_fault = gaddr;
                    (*G_CUR_CPU).sig_repeat = 0;
                }
                (*G_CUR_CPU).sig_repeat += 1;
                let looping = (*G_CUR_CPU).sig_repeat > OCERZ_SIG_MAX_REPEAT as c_int;

                let mut no_wine_teb = (*G_CUR_CPU).sig_altstack_sp == 0;
                if !no_wine_teb {
                    let sb = (*G_CUR_CPU).gpr[OCERZ_RSP] & !0xffffu64;
                    if ocerz_addr_readable(sb) != 0 && ocerz_addr_committed(ocerz_ld(sb, 8)) != 1 {
                        no_wine_teb = true;
                    }
                }
                if looping && no_wine_teb {
                    {
                        let mut wb = [0u8; 200];
                        let mut w = wb.as_mut_ptr() as *mut c_char;
                        w = str_into(w, c"ocerz: gs0x320 WORKER-TERMINATE pid=".as_ptr());
                        w = hex_into(w, libc::getpid() as u64);
                        w = str_into(w, c" gaddr=".as_ptr());
                        w = hex_into(w, gaddr);
                        w = str_into(w, c" gs=".as_ptr());
                        w = hex_into(w, gs);
                        w = str_into(w, c" icount=".as_ptr());
                        w = hex_into(
                            w,
                            if !G_VM.is_null() {
                                (*G_VM).insn_count
                            } else {
                                0
                            },
                        );
                        w = str_into(w, c"\n".as_ptr());
                        write(
                            2,
                            wb.as_ptr() as *const c_void,
                            w as usize - wb.as_ptr() as usize,
                        );
                    }
                    (*G_CUR_CPU).terminated = 1;
                    ocerz_recov_note(3, (*G_CUR_CPU).rip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }

                {
                    if REWIND_OFF.load(Ordering::Relaxed) < 0 {
                        REWIND_OFF.store(
                            (!libc::getenv(c"OCERZ_NO_RIP_REWIND".as_ptr()).is_null()) as i32,
                            Ordering::Relaxed,
                        );
                    }
                    if rip_exact != 0 && REWIND_OFF.load(Ordering::Relaxed) == 0 {
                        (*G_CUR_CPU).rip = fault_rip;
                    }
                }
                {
                    if FAULTLOG.load(Ordering::Relaxed) < 0 {
                        FAULTLOG.store(
                            (!libc::getenv(c"OCERZ_FAULTLOG".as_ptr()).is_null()) as i32,
                            Ordering::Relaxed,
                        );
                    }
                    if FAULTLOG.load(Ordering::Relaxed) != 0 {
                        let mut rb: u64 = 0;
                        let mut rs: u64 = 0;
                        let hp = ocerz_host_region_prot(gaddr, &mut rb, &mut rs);
                        let mut fb = [0u8; 384];
                        let mut f = fb.as_mut_ptr() as *mut c_char;
                        f = str_into(f, c"ocerz: FAULT-MAP addr=".as_ptr());
                        f = hex_into(f, gaddr);
                        f = str_into(f, c" owned=".as_ptr());
                        f = hex_into(f, ocerz_addr_committed(gaddr) as i64 as u64);
                        f = str_into(f, c" slot_prot=".as_ptr());
                        f = hex_into(f, ocerz_addr_prot(gaddr) as i64 as u64);
                        f = str_into(f, c" host_prot=".as_ptr());
                        f = hex_into(f, hp as u64);
                        f = str_into(f, c" region=".as_ptr());
                        f = hex_into(f, rb);
                        f = str_into(f, c"+".as_ptr());
                        f = hex_into(f, rs);
                        f = str_into(f, c" rip=".as_ptr());
                        f = hex_into(f, (*G_CUR_CPU).rip);
                        f = str_into(f, c" hpc=".as_ptr());
                        f = hex_into(f, hpc as u64);
                        f = str_into(f, c" hinsn=".as_ptr());
                        f = hex_into(
                            f,
                            if !hpc.is_null() {
                                *(hpc as *const u32) as u64
                            } else {
                                0
                            },
                        );
                        f = str_into(f, c" esr=".as_ptr());
                        f = hex_into(f, esr);
                        f = str_into(f, c" host_addr=".as_ptr());
                        f = hex_into(f, (*si).si_addr as u64);
                        f = str_into(f, c" pinned=".as_ptr());
                        f = hex_into(f, ocerz_pinned_page(gaddr) as u64);
                        f = str_into(f, c" pid=".as_ptr());
                        f = hex_into(f, libc::getpid() as u64);
                        f = str_into(f, c"\n".as_ptr());
                        write(
                            2,
                            fb.as_ptr() as *const c_void,
                            f as usize - fb.as_ptr() as usize,
                        );
                        if in_jit && !uc.is_null() {
                            let mut ji: OcerzJitFaultInfo = core::mem::zeroed();
                            if ffi::ocerz_jit_fault_info(fvm, hpc, &mut ji) != 0 {
                                let mut jb = [0u8; 640];
                                let mut j = jb.as_mut_ptr() as *mut c_char;
                                j = str_into(j, c"ocerz: JITFAULT block=".as_ptr());
                                j = hex_into(j, ji.block_rip);
                                j = str_into(j, c" insn=".as_ptr());
                                j = hex_into(j, ji.insn_rip);
                                j = str_into(j, c" idx=".as_ptr());
                                j = hex_into(j, ji.insn_index as u32 as u64);
                                j = str_into(j, c" hoff=".as_ptr());
                                j = hex_into(j, ji.host_word as u64);
                                j = str_into(j, c" class=".as_ptr());
                                j = hex_into(j, ji.pin_class as u64);
                                j = str_into(j, c" pins=".as_ptr());
                                j = hex_into(j, ji.n_pinned as u64);
                                for i in 0..ji.n_pinned {
                                    j = str_into(j, c" x".as_ptr());
                                    j = hex_into(j, (21 + i) as u64);
                                    j = str_into(j, c"/g".as_ptr());
                                    j = hex_into(
                                        j,
                                        *ji.host_holds.get_unchecked(i as usize) as u64,
                                    );
                                    j = str_into(j, c"=".as_ptr());
                                    j = hex_into(
                                        j,
                                        *(*(*uc).uc_mcontext).ss.x.get_unchecked(21 + i as usize),
                                    );
                                }
                                j = str_into(j, c"\n".as_ptr());
                                write(
                                    2,
                                    jb.as_ptr() as *const c_void,
                                    j as usize - jb.as_ptr() as usize,
                                );
                            }
                        }
                    }
                }
                if in_jit
                    && rip_exact != 0
                    && ocerz_guest_base == 0
                    && ((!ocerz_commpage.is_null()
                        && gaddr >= ffi::OCERZ_COMMPAGE_LO
                        && gaddr < ffi::OCERZ_COMMPAGE_HI)
                        || (ocerz_low_base != 0
                            && gaddr >= ffi::OCERZ_TOP_LO
                            && gaddr < ffi::OCERZ_COMMPAGE_HI
                            && (*si).si_addr as u64 == gaddr))
                    && ffi::ocerz_jit_note_commpage_fault(fvm, hpc, fault_rip) != 0
                {
                    (*G_CUR_CPU).rip = fault_rip;
                    (*G_CUR_CPU).sig_repeat = 0;
                    (*G_CUR_CPU).interp_once = 1;
                    if env_cache!("OCERZ_CPFAULTLOG") != 0 {
                        libc::fprintf(
                            stderr(),
                            c"ocerz: CPFAULT rip=%#llx gaddr=%#llx\n".as_ptr(),
                            fault_rip as c_ulonglong,
                            gaddr as c_ulonglong,
                        );
                        ffi::ocerz_cpu_dump(G_CUR_CPU, stderr() as *mut ffi::FILE);
                    }
                    ocerz_recov_note(4, fault_rip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                if in_jit && rip_exact != 0 && ffi::ocerz_jit_fault_pair(hpc) != 0 {
                    (*G_CUR_CPU).rip = fault_rip;
                    (*G_CUR_CPU).sig_repeat = 0;
                    (*G_CUR_CPU).interp_once = 1;
                    ocerz_recov_note(8, fault_rip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                let fault_rsp = (*G_CUR_CPU).gpr[OCERZ_RSP];
                if FAULT_DREG.load(Ordering::Relaxed) == -2 {
                    let e = libc::getenv(c"OCERZ_FAULTDUMP".as_ptr());
                    FAULT_DREG.store(
                        if !e.is_null() { libc::atoi(e) } else { -1 },
                        Ordering::Relaxed,
                    );
                }
                let fault_dreg_v = FAULT_DREG.load(Ordering::Relaxed);
                let fault_dumpval = if fault_dreg_v >= 0 && fault_dreg_v < 16 {
                    (*G_CUR_CPU).gpr[fault_dreg_v as usize]
                } else {
                    0
                };
                delivered = if looping {
                    0
                } else {
                    ffi::ocerz_signal_deliver(G_CUR_CPU, SIGSEGV, gaddr, code, err)
                };
                if G_WINEFAULTLOG != 0 && delivered != 0 && (*G_CUR_CPU).sig_altstack_sp != 0 {
                    let teb_committed = gs <= u64::MAX - 16
                        && ocerz_addr_readable(gs + 8) != 0
                        && ocerz_addr_readable(gs + 16) != 0;
                    let stack_base = if teb_committed {
                        ocerz_ld(gs + 8, 8)
                    } else {
                        0
                    };
                    let stack_limit = if teb_committed {
                        ocerz_ld(gs + 16, 8)
                    } else {
                        0
                    };
                    let mut wb = [0u8; 512];
                    let mut w = wb.as_mut_ptr() as *mut c_char;
                    w = str_into(w, c"ocerz: WINEFAULT[".as_ptr());
                    w = hex_into(w, libc::getpid() as u64);
                    w = str_into(w, c"] host_sig=".as_ptr());
                    w = hex_into(w, sig as u64);
                    w = str_into(w, c" addr=".as_ptr());
                    w = hex_into(w, gaddr);
                    w = str_into(w, c" rip=".as_ptr());
                    w = hex_into(w, fault_rip);
                    w = str_into(w, c" gs=".as_ptr());
                    w = hex_into(w, gs);
                    w = str_into(w, c" rsp=".as_ptr());
                    w = hex_into(w, fault_rsp);
                    w = str_into(w, c" TEB+8.StackBase=".as_ptr());
                    w = hex_into(w, stack_base);
                    w = str_into(w, c" TEB+16.StackLimit=".as_ptr());
                    w = hex_into(w, stack_limit);
                    w = str_into(w, c" teb_committed=".as_ptr());
                    w = hex_into(w, teb_committed as u64);
                    w = str_into(w, c" low_base=".as_ptr());
                    w = hex_into(w, ocerz_low_base);
                    w = str_into(w, c" h2g(rsp)=".as_ptr());
                    w = hex_into(w, ocerz_h2g(fault_rsp as *const c_void));
                    w = str_into(w, c"\n".as_ptr());
                    write(
                        2,
                        wb.as_ptr() as *const c_void,
                        w as usize - wb.as_ptr() as usize,
                    );
                    {
                        let mut tb = [0u8; 1024];
                        let mut t = tb.as_mut_ptr() as *mut c_char;
                        t = str_into(t, c"ocerz: WINEFAULT-TSD cpu#".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).cpu_number as u64);
                        t = str_into(t, c" host_tid=".as_ptr());
                        {
                            let mut htid: u64 = 0;
                            pthread_threadid_np(0, &mut htid);
                            t = hex_into(t, htid);
                        }
                        for k in 0..24u64 {
                            let mut v: u64 = 0;
                            let mut got: mach_vm_size_t = 0;
                            if mach_vm_read_overwrite(
                                mach_task_self(),
                                ocerz_g2h(gs + 8 * k) as mach_vm_address_t,
                                8,
                                &mut v as *mut u64 as mach_vm_address_t,
                                &mut got,
                            ) != KERN_SUCCESS
                            {
                                break;
                            }
                            t = str_into(t, c" ".as_ptr());
                            t = hex_into(t, v);
                        }
                        t = str_into(t, c"\n".as_ptr());
                        write(
                            2,
                            tb.as_ptr() as *const c_void,
                            t as usize - tb.as_ptr() as usize,
                        );
                    }
                    {
                        let dreg = fault_dreg_v;
                        if dreg >= 0 && dreg < 16 {
                            let base = (fault_dumpval & !0xfu64).wrapping_sub(0x40);
                            let mut q = (&raw mut FAULT_DB) as *mut c_char;
                            q = str_into(q, c"ocerz: FAULTDUMP reg=".as_ptr());
                            q = hex_into(q, dreg as u64);
                            q = str_into(q, c" val=".as_ptr());
                            q = hex_into(q, fault_dumpval);
                            q = str_into(q, c"\n".as_ptr());
                            let mut o = 0u64;
                            while o < 0x140 {
                                q = str_into(q, c"  ".as_ptr());
                                q = hex_into(q, base + o);
                                q = str_into(q, c":".as_ptr());
                                let mut k = 0u64;
                                while k < 16 {
                                    q = str_into(q, c" ".as_ptr());
                                    if ocerz_addr_readable(base + o + k) != 0
                                        && ocerz_addr_readable(base + o + k + 7) != 0
                                    {
                                        q = hex_into(q, ocerz_ld(base + o + k, 8));
                                    } else {
                                        q = str_into(q, c"????????".as_ptr());
                                    }
                                    k += 8;
                                }
                                q = str_into(q, c"\n".as_ptr());
                                o += 0x10;
                            }
                            write(
                                2,
                                (&raw const FAULT_DB) as *const c_void,
                                q as usize - (&raw const FAULT_DB) as usize,
                            );
                        }
                    }
                    {
                        let mut q = (&raw mut FAULT_SB) as *mut c_char;
                        q = str_into(q, c"ocerz: WINEFAULT-SCAN rsp=".as_ptr());
                        q = hex_into(q, fault_rsp);
                        q = str_into(q, c":".as_ptr());
                        let mut printed = 0;
                        let mut o = 0u64;
                        while o < 0x3000
                            && printed < 40
                            && (q as usize) < (&raw const FAULT_SB) as usize + 2048 - 48
                        {
                            if ocerz_addr_readable(fault_rsp + o) == 0 {
                                break;
                            }
                            let v = ocerz_ld(fault_rsp + o, 8);
                            let codey = (v >= 0x6fff00000000 && v < 0x7ffc00000000)
                                || (v >= 0x100000000 && v < 0x200000000);
                            if codey {
                                q = str_into(q, c" +".as_ptr());
                                q = hex_into(q, o);
                                q = str_into(q, c":".as_ptr());
                                q = hex_into(q, v);
                                printed += 1;
                            }
                            o += 8;
                        }
                        q = str_into(q, c"\n".as_ptr());
                        write(
                            2,
                            (&raw const FAULT_SB) as *const c_void,
                            q as usize - (&raw const FAULT_SB) as usize,
                        );
                    }
                }
                if G_SIGTRACE != 0 {
                    let mut tb = [0u8; 256];
                    let mut t = tb.as_mut_ptr() as *mut c_char;
                    t = str_into(
                        t,
                        if delivered != 0 {
                            c"ocerz: SIG[".as_ptr()
                        } else {
                            c"ocerz: SIGNH[".as_ptr()
                        },
                    );
                    t = hex_into(t, libc::getpid() as u64);
                    t = str_into(t, c"] deliver addr=".as_ptr());
                    t = hex_into(t, gaddr);
                    t = str_into(t, c" rip=".as_ptr());
                    t = hex_into(t, fault_rip);
                    t = str_into(t, c" gs=".as_ptr());
                    t = hex_into(t, gs);
                    t = str_into(
                        t,
                        if gaddr == gs.wrapping_sub(8) {
                            c" [==gs-8]".as_ptr()
                        } else {
                            c"".as_ptr()
                        },
                    );
                    t = str_into(t, c" comm(addr)=".as_ptr());
                    t = hex_into(t, ocerz_addr_committed(gaddr) as i64 as u64);
                    t = str_into(t, c" comm(gs)=".as_ptr());
                    t = hex_into(t, ocerz_addr_committed(gs) as i64 as u64);
                    t = str_into(t, c" ->tramp=".as_ptr());
                    t = hex_into(t, if delivered != 0 { (*G_CUR_CPU).rip } else { 0 });
                    t = str_into(t, c" icount=".as_ptr());
                    t = hex_into(
                        t,
                        if !G_VM.is_null() {
                            (*G_VM).insn_count
                        } else {
                            0
                        },
                    );
                    t = str_into(t, c"\n".as_ptr());
                    write(
                        2,
                        tb.as_ptr() as *const c_void,
                        t as usize - tb.as_ptr() as usize,
                    );
                    {
                        let mut bb = [0u8; 640];
                        let mut w = bb.as_mut_ptr() as *mut c_char;
                        let mut ib: u64 = 0;
                        let in_ = ocerz_dyld_name_for_addr(gaddr, &mut ib);
                        w = str_into(w, c"ocerz:   addr-image=".as_ptr());
                        w = str_into(
                            w,
                            if !in_.is_null() {
                                in_
                            } else {
                                c"<none>".as_ptr()
                            },
                        );
                        w = str_into(w, c" bt:".as_ptr());
                        let mut fp = (*G_CUR_CPU).gpr[OCERZ_RBP];
                        let mut d = 0;
                        while d < 12
                            && fp > 0x1000
                            && ocerz_addr_readable(fp + 8) != 0
                            && (w as usize) < bb.as_ptr() as usize + 560
                        {
                            w = str_into(w, c" ".as_ptr());
                            w = hex_into(w, ocerz_ld(fp + 8, 8));
                            let nf = if ocerz_addr_readable(fp) != 0 {
                                ocerz_ld(fp, 8)
                            } else {
                                0
                            };
                            if nf <= fp {
                                break;
                            }
                            fp = nf;
                            d += 1;
                        }
                        w = str_into(w, c"\n".as_ptr());
                        write(
                            2,
                            bb.as_ptr() as *const c_void,
                            w as usize - bb.as_ptr() as usize,
                        );
                    }
                    if fault_rip != 0 {
                        let mut xb = [0u8; 200];
                        let mut x = xb.as_mut_ptr() as *mut c_char;
                        x = str_into(x, c"ocerz:   insn@".as_ptr());
                        x = hex_into(x, fault_rip);
                        x = str_into(x, c" =".as_ptr());
                        for i in -3i64..12 {
                            let b_ = ocerz_ld(fault_rip.wrapping_add(i as u64), 1);
                            *x = ' ' as c_char;
                            x = x.add(1);
                            *x = b"0123456789abcdef"[((b_ >> 4) & 0xf) as usize] as c_char;
                            x = x.add(1);
                            *x = b"0123456789abcdef"[(b_ & 0xf) as usize] as c_char;
                            x = x.add(1);
                        }
                        x = str_into(x, c"\n".as_ptr());
                        write(
                            2,
                            xb.as_ptr() as *const c_void,
                            x as usize - xb.as_ptr() as usize,
                        );
                    } else {
                        let mut xb = [0u8; 512];
                        let mut x = xb.as_mut_ptr() as *mut c_char;
                        x = str_into(x, c"ocerz:   rip0 hist:".as_ptr());
                        for i in 2..=24u32 {
                            x = str_into(x, c" ".as_ptr());
                            x = hex_into(x, G_RIPHIST[(G_RIPHIST_N.wrapping_sub(i) & 31) as usize]);
                        }
                        x = str_into(x, c"\n".as_ptr());
                        write(
                            2,
                            xb.as_ptr() as *const c_void,
                            x as usize - xb.as_ptr() as usize,
                        );
                    }
                    {
                        let mut gb = [0u8; 256];
                        let mut g = gb.as_mut_ptr() as *mut c_char;
                        static mut NM: [*const c_char; 16] = [
                            c"rax".as_ptr(),
                            c"rcx".as_ptr(),
                            c"rdx".as_ptr(),
                            c"rbx".as_ptr(),
                            c"rsp".as_ptr(),
                            c"rbp".as_ptr(),
                            c"rsi".as_ptr(),
                            c"rdi".as_ptr(),
                            c"r8".as_ptr(),
                            c"r9".as_ptr(),
                            c"r10".as_ptr(),
                            c"r11".as_ptr(),
                            c"r12".as_ptr(),
                            c"r13".as_ptr(),
                            c"r14".as_ptr(),
                            c"r15".as_ptr(),
                        ];
                        g = str_into(g, c"ocerz:   gpr".as_ptr());
                        for i in 0..16 {
                            g = str_into(g, c" ".as_ptr());
                            g = str_into(g, NM[i]);
                            g = str_into(g, c"=".as_ptr());
                            g = hex_into(g, (*G_CUR_CPU).gpr[i]);
                        }
                        g = str_into(g, c"\n".as_ptr());
                        write(
                            2,
                            gb.as_ptr() as *const c_void,
                            g as usize - gb.as_ptr() as usize,
                        );
                    }
                    {
                        static REGS: [usize; 3] = [OCERZ_RSI, OCERZ_R12, OCERZ_R11];
                        static mut RN2: [*const c_char; 3] =
                            [c"rsi".as_ptr(), c"r12".as_ptr(), c"r11".as_ptr()];
                        for k in 0..3 {
                            let b_ = (*G_CUR_CPU).gpr[REGS[k]];
                            let mut mb = [0u8; 256];
                            let mut m = mb.as_mut_ptr() as *mut c_char;
                            m = str_into(m, c"ocerz:   mem[".as_ptr());
                            m = str_into(m, RN2[k]);
                            m = str_into(m, c"=".as_ptr());
                            m = hex_into(m, b_);
                            m = str_into(m, c"]:".as_ptr());
                            for q in 0..8u64 {
                                let a = b_ + q * 8;
                                m = str_into(m, c" ".as_ptr());
                                if ocerz_addr_readable(a) != 0 {
                                    m = hex_into(m, ocerz_ld(a, 8));
                                } else {
                                    m = str_into(m, c"<unc>".as_ptr());
                                }
                            }
                            m = str_into(m, c"\n".as_ptr());
                            write(
                                2,
                                mb.as_ptr() as *const c_void,
                                m as usize - mb.as_ptr() as usize,
                            );
                        }
                    }
                    if code == 1 {
                        let mut cb = [0u8; 400];
                        let mut c_ = cb.as_mut_ptr() as *mut c_char;
                        c_ = str_into(c_, c"ocerz:   commitmap ".as_ptr());
                        let pg = gaddr & !0xfffu64;
                        let scanlo = pg - 0x10000;
                        c_ = hex_into(c_, scanlo);
                        c_ = str_into(c_, c"..: ".as_ptr());
                        for i in 0..36u64 {
                            let a = scanlo + i * 0x1000;
                            let cm = ocerz_addr_committed(a);
                            *c_ = if a == pg {
                                '[' as c_char
                            } else {
                                ' ' as c_char
                            };
                            c_ = c_.add(1);
                            *c_ = if cm == 1 {
                                'C' as c_char
                            } else if cm == 0 {
                                '.' as c_char
                            } else {
                                '?' as c_char
                            };
                            c_ = c_.add(1);
                            *c_ = if a == pg {
                                ']' as c_char
                            } else {
                                ' ' as c_char
                            };
                            c_ = c_.add(1);
                        }
                        c_ = str_into(c_, c"\n".as_ptr());
                        write(
                            2,
                            cb.as_ptr() as *const c_void,
                            c_ as usize - cb.as_ptr() as usize,
                        );
                    }
                }
                CRASH_DEPTH = 0;
                write(2, c"P3 post dlv\n".as_ptr() as *const c_void, 11);
                if delivered != 0 {
                    ocerz_recov_note(5, fault_rip);
                    siglongjmp(G_SIG_RECOVER, 1);
                }
            }
        }
        let _ = &delivered;

        if !G_CUR_CPU.is_null()
            && !G_SIG_RECOVER.is_null()
            && CRASH_DEPTH == 0
            && ocerz_host_in_guest_space((*si).si_addr) == 0
        {
            let mut wine_teb = 0u64;
            {
                let gs = (*G_CUR_CPU).gs_base;
                if gs != 0 && ocerz_addr_readable(gs + 0x30) != 0 {
                    let t = ocerz_ld(gs + 0x30, 8);
                    if t != 0 && ocerz_addr_readable(t + 0x30) != 0 && ocerz_ld(t + 0x30, 8) == t {
                        wine_teb = t;
                    }
                }
            }
            let mut no_teb = wine_teb == 0;
            if !no_teb {
                let uc = ctx as *const ucontext_t;
                let hpc = if !uc.is_null() {
                    (*(*uc).uc_mcontext).ss.pc as *const c_void
                } else {
                    ptr::null()
                };
                let fvm = (*G_CUR_CPU).vm;
                let mut jrip: u64 = 0;
                let in_jit = !hpc.is_null()
                    && !fvm.is_null()
                    && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0
                    && ffi::ocerz_jit_fault_rip(fvm, hpc, &mut jrip) != 0;
                CRASH_DEPTH = 1;
                if in_jit {
                    ffi::ocerz_jit_fault_recover_regs(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ss.x.as_ptr(),
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_xmm(
                        fvm,
                        hpc,
                        (*(*uc).uc_mcontext).ns.v.as_ptr() as *const c_void,
                        G_CUR_CPU,
                    );
                    ffi::ocerz_jit_fault_recover_flags(fvm, hpc, G_CUR_CPU);
                    (*G_CUR_CPU).rip = jrip;
                } else {
                    (*G_CUR_CPU).rip = (*G_CUR_CPU).cur_rip;
                }
                ffi::ocerz_flags_materialize(G_CUR_CPU);
                let wesr = if !uc.is_null() {
                    (*(*uc).uc_mcontext).es.esr as u64
                } else {
                    0
                };
                let wec = ((wesr >> 26) & 0x3f) as u32;
                let wfetch = wec == 0x20 || wec == 0x21;
                let wwrite = !wfetch && (wesr & (1 << 6)) != 0;
                let werr = 0x4u32 | if wwrite { 0x2 } else { 0 } | if wfetch { 0x10 } else { 0 };
                let wgaddr = ocerz_h2g((*si).si_addr);
                {
                    let mut ab = [0u8; 400];
                    let mut a = ab.as_mut_ptr() as *mut c_char;
                    a = str_into(a, c"ocerz: WILD-FAULT-AV pid=".as_ptr());
                    a = hex_into(a, libc::getpid() as u64);
                    a = str_into(a, c" wtid=".as_ptr());
                    a = hex_into(
                        a,
                        if ocerz_addr_readable(wine_teb + 0x48) != 0 {
                            ocerz_ld(wine_teb + 0x48, 8)
                        } else {
                            0
                        },
                    );
                    a = str_into(a, c" addr=".as_ptr());
                    a = hex_into(a, (*si).si_addr as u64);
                    a = str_into(a, c" rip=".as_ptr());
                    a = hex_into(a, (*G_CUR_CPU).rip);
                    a = str_into(a, c" injit=".as_ptr());
                    a = hex_into(a, in_jit as u64);
                    a = str_into(a, c" hpc=".as_ptr());
                    a = hex_into(a, hpc as u64);
                    a = str_into(a, c" arena=".as_ptr());
                    a = hex_into(
                        a,
                        (!hpc.is_null()
                            && !fvm.is_null()
                            && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0)
                            as u64,
                    );
                    {
                        let mut fi: OcerzJitFaultInfo = core::mem::zeroed();
                        if !hpc.is_null()
                            && !fvm.is_null()
                            && ffi::ocerz_jit_pc_in_arena(fvm, hpc) != 0
                            && ffi::ocerz_jit_fault_info(fvm, hpc, &mut fi) != 0
                        {
                            a = str_into(a, c" blk=".as_ptr());
                            a = hex_into(a, fi.block_rip);
                            a = str_into(a, c" insn=".as_ptr());
                            a = hex_into(a, fi.insn_rip);
                            a = str_into(a, c" hinsn=".as_ptr());
                            a = hex_into(a, *(hpc as *const u32) as u64);
                        }
                    }
                    a = str_into(a, c"\n".as_ptr());
                    write(
                        2,
                        ab.as_ptr() as *const c_void,
                        a as usize - ab.as_ptr() as usize,
                    );
                }
                if !libc::getenv(c"OCERZ_WILDDUMP".as_ptr()).is_null() {
                    wild_dump();
                }
                if ffi::ocerz_signal_deliver(G_CUR_CPU, SIGSEGV, wgaddr, 1, werr) != 0 {
                    ocerz_recov_note(5, (*G_CUR_CPU).rip);
                    CRASH_DEPTH = 0;
                    siglongjmp(G_SIG_RECOVER, 1);
                }
                CRASH_DEPTH = 0;
                no_teb = true;
            }
            if no_teb {
                {
                    let n = WILD_LOGS.fetch_add(1, Ordering::Relaxed);
                    if n < 32 {
                        let uc = ctx as *const ucontext_t;
                        let mut tb = [0u8; 512];
                        let mut t = tb.as_mut_ptr() as *mut c_char;
                        t = str_into(t, c"ocerz: WILD-WORKER-TERMINATE pid=".as_ptr());
                        t = hex_into(t, libc::getpid() as u64);
                        t = str_into(t, c" cpu=".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).cpu_number as u64);
                        {
                            let gs = (*G_CUR_CPU).gs_base;
                            let mut teb = 0u64;
                            let mut wtid = 0u64;
                            if gs != 0 && ocerz_addr_readable(gs + 0x30) != 0 {
                                teb = ocerz_ld(gs + 0x30, 8);
                            }
                            if teb != 0
                                && ocerz_addr_readable(teb + 0x48) != 0
                                && ocerz_ld(teb + 0x30, 8) == teb
                            {
                                wtid = ocerz_ld(teb + 0x48, 8);
                            }
                            t = str_into(t, c" teb=".as_ptr());
                            t = hex_into(t, teb);
                            t = str_into(t, c" wtid=".as_ptr());
                            t = hex_into(t, wtid);
                        }
                        t = str_into(t, c" addr=".as_ptr());
                        t = hex_into(t, (*si).si_addr as u64);
                        t = str_into(t, c" host_pc=".as_ptr());
                        t = hex_into(
                            t,
                            if !uc.is_null() {
                                (*(*uc).uc_mcontext).ss.pc
                            } else {
                                0
                            },
                        );
                        t = str_into(t, c" rip=".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).rip);
                        t = str_into(t, c" cur_rip=".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).cur_rip);
                        t = str_into(t, c" rsp=".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).gpr[OCERZ_RSP]);
                        t = str_into(t, c" gs=".as_ptr());
                        t = hex_into(t, (*G_CUR_CPU).gs_base);
                        let mut wji: OcerzJitFaultInfo = core::mem::zeroed();
                        if !uc.is_null()
                            && !(*G_CUR_CPU).vm.is_null()
                            && ffi::ocerz_jit_pc_in_arena(
                                (*G_CUR_CPU).vm,
                                (*(*uc).uc_mcontext).ss.pc as *const c_void,
                            ) != 0
                            && ffi::ocerz_jit_fault_info(
                                (*G_CUR_CPU).vm,
                                (*(*uc).uc_mcontext).ss.pc as *const c_void,
                                &mut wji,
                            ) != 0
                        {
                            t = str_into(t, c" jit-block=".as_ptr());
                            t = hex_into(t, wji.block_rip);
                            t = str_into(t, c" insn=".as_ptr());
                            t = hex_into(t, wji.insn_rip);
                            t = str_into(t, c" hoff=".as_ptr());
                            t = hex_into(t, wji.host_word as u64);
                            t = str_into(t, c" hinsn=".as_ptr());
                            t = hex_into(t, *((*(*uc).uc_mcontext).ss.pc as *const u32) as u64);
                            t = str_into(t, c" esr=".as_ptr());
                            t = hex_into(t, (*(*uc).uc_mcontext).es.esr as u64);
                        }
                        t = str_into(t, c"\n".as_ptr());
                        write(
                            2,
                            tb.as_ptr() as *const c_void,
                            t as usize - tb.as_ptr() as usize,
                        );
                    }
                }
                if !libc::getenv(c"OCERZ_WILDDUMP".as_ptr()).is_null() {
                    wild_dump();
                }
                (*G_CUR_CPU).terminated = 1;
                ocerz_recov_note(6, (*G_CUR_CPU).rip);
                siglongjmp(G_SIG_RECOVER, 1);
            }
        }
        let mut buf = [0u8; 640];
        let mut p = buf.as_mut_ptr() as *mut c_char;

        if CRASH_DEPTH > 0 && ocerz_host_in_guest_space((*si).si_addr) != 0 {
            let fg = ocerz_h2g((*si).si_addr);
            if ocerz_addr_committed(fg) == 0 && ocerz_commit_fault_page(fg) != 0 {
                return;
            }
        }
        CRASH_DEPTH += 1;
        if CRASH_DEPTH > 1 {
            p = str_into(
                p,
                c"ocerz: nested fault inside crash handler host_addr=".as_ptr(),
            );
            p = hex_into(p, (*si).si_addr as u64);
            if !ctx.is_null() {
                let uc = ctx as *const ucontext_t;
                p = str_into(p, c" host_pc=".as_ptr());
                p = hex_into(p, (*(*uc).uc_mcontext).ss.pc);
            }
            p = str_into(p, c" gaddr=".as_ptr());
            p = hex_into(
                p,
                if ocerz_host_in_guest_space((*si).si_addr) != 0 {
                    ocerz_h2g((*si).si_addr)
                } else {
                    0
                },
            );
            p = str_into(p, c" comm=".as_ptr());
            p = hex_into(
                p,
                (if ocerz_host_in_guest_space((*si).si_addr) != 0 {
                    ocerz_addr_committed(ocerz_h2g((*si).si_addr)) as i64
                } else {
                    -2
                }) as u64,
            );
            p = str_into(p, c" prot=".as_ptr());
            p = hex_into(
                p,
                (if ocerz_host_in_guest_space((*si).si_addr) != 0 {
                    ocerz_addr_prot(ocerz_h2g((*si).si_addr)) as i64
                } else {
                    -2
                }) as u64,
            );
            p = str_into(p, c" slide=".as_ptr());
            p = hex_into(p, G_IMAGE_SLIDE);
            p = str_into(p, c"\n".as_ptr());
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            libc::_exit(139);
        }
        p = str_into(p, c"ocerz: guest crash[".as_ptr());
        p = hex_into(p, libc::getpid() as u32 as u64);
        p = str_into(p, c"] cpu#".as_ptr());
        p = hex_into(
            p,
            if !G_CUR_CPU.is_null() {
                (*G_CUR_CPU).cpu_number as u64
            } else {
                0xffff
            },
        );
        p = str_into(p, c" ".as_ptr());
        p = str_into(
            p,
            if sig == SIGBUS {
                c"SIGBUS".as_ptr()
            } else if sig == SIGILL {
                c"SIGILL".as_ptr()
            } else if sig == SIGTRAP {
                c"SIGTRAP".as_ptr()
            } else if sig == SIGSYS {
                c"SIGSYS".as_ptr()
            } else {
                c"SIGSEGV".as_ptr()
            },
        );
        p = str_into(p, c" host_addr=".as_ptr());
        p = hex_into(p, (*si).si_addr as u64);
        if !ctx.is_null() {
            let uc = ctx as *const ucontext_t;
            let hpc = (*(*uc).uc_mcontext).ss.pc;
            p = str_into(p, c" host_pc=".as_ptr());
            p = hex_into(p, hpc);
            p = str_into(p, c" host_insn=".as_ptr());
            p = hex_into(p, *(hpc as *const u32) as u64);
            p = str_into(p, c" host_lr=".as_ptr());
            p = hex_into(p, (*(*uc).uc_mcontext).ss.lr);
            p = str_into(p, c" slide=".as_ptr());
            p = hex_into(p, G_IMAGE_SLIDE);
            p = str_into(p, c" ocerz_base=".as_ptr());
            p = hex_into(p, _dyld_get_image_header(0) as u64);
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            p = buf.as_mut_ptr() as *mut c_char;
            {
                let mut ibase: u64 = 0;
                let iname = ocerz_dyld_name_for_addr(hpc, &mut ibase);
                if !iname.is_null() {
                    p = str_into(p, c"\n  host_pc is guest code: ".as_ptr());
                    p = str_into(p, iname);
                    p = str_into(p, c"+".as_ptr());
                    p = hex_into(p, hpc - ibase);
                }
                let mut di: Dl_info = core::mem::zeroed();
                let lr = (*(*uc).uc_mcontext).ss.lr;
                if dladdr(lr as *const c_void, &mut di) != 0 && !di.dli_fname.is_null() {
                    p = str_into(p, c"\n  host_lr is in ".as_ptr());
                    p = str_into(p, di.dli_fname);
                    p = str_into(p, c" ".as_ptr());
                    p = str_into(
                        p,
                        if !di.dli_sname.is_null() {
                            di.dli_sname
                        } else {
                            c"?".as_ptr()
                        },
                    );
                }
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
                p = buf.as_mut_ptr() as *mut c_char;
            }
            p = str_into(p, c"\n  host-x:".as_ptr());
            for i in 0..29 {
                p = str_into(
                    p,
                    if i % 8 == 0 {
                        c"\n    ".as_ptr()
                    } else {
                        c" ".as_ptr()
                    },
                );
                p = hex_into(p, (*(*uc).uc_mcontext).ss.x[i]);
            }
            p = str_into(p, c" fp=".as_ptr());
            p = hex_into(p, (*(*uc).uc_mcontext).ss.fp);
            p = str_into(p, c" lr=".as_ptr());
            p = hex_into(p, (*(*uc).uc_mcontext).ss.lr);
            p = str_into(p, c" sp=".as_ptr());
            p = hex_into(p, (*(*uc).uc_mcontext).ss.sp);
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            p = buf.as_mut_ptr() as *mut c_char;
            if sig == SIGILL {
                let pc0 = (*(*uc).uc_mcontext).ss.pc;
                let mut cb: *const u32 = ptr::null();
                let mut ce: *const u32 = ptr::null();
                if !G_VM.is_null() && ffi::ocerz_jit_code_range(G_VM, &mut cb, &mut ce) != 0 {
                    p = str_into(p, c"  jit-owner=".as_ptr());
                    p = hex_into(p, ffi::ocerz_jit_owner_pid(G_VM) as u32 as u64);
                    p = str_into(p, c" self=".as_ptr());
                    p = hex_into(p, libc::getpid() as u32 as u64);
                    p = str_into(p, c" arena=[".as_ptr());
                    p = hex_into(p, cb as u64);
                    p = str_into(p, c",".as_ptr());
                    p = hex_into(p, ce as u64);
                    p = str_into(p, c") base-word=".as_ptr());
                    p = hex_into(p, *cb as u64);
                    p = str_into(p, c"\n  branch-sites->pc:".as_ptr());
                    let mut found = 0;
                    let mut w = cb;
                    while w < ce && found < 8 {
                        let v = *w;
                        let mut off: i64 = 0;
                        let mut is_ = false;
                        if (v & 0x7c000000) == 0x14000000 {
                            off = ((((v << 6) as i32) >> 6) as i64) * 4;
                            is_ = true;
                        } else if (v & 0xff000010) == 0x54000000 || (v & 0x7e000000) == 0x34000000 {
                            off = ((((v >> 5) << 13) as i32) >> 13) as i64 * 4;
                            is_ = true;
                        }
                        if is_ && (w as u64).wrapping_add(off as u64) == pc0 {
                            p = str_into(p, c" ".as_ptr());
                            p = hex_into(p, w as u64);
                            p = str_into(p, c"/w=".as_ptr());
                            p = hex_into(p, v as u64);
                            found += 1;
                        }
                        w = w.add(1);
                    }
                    if found == 0 {
                        p = str_into(p, c" none".as_ptr());
                    }
                    p = str_into(p, c"\n".as_ptr());
                    write(
                        2,
                        buf.as_ptr() as *const c_void,
                        p as usize - buf.as_ptr() as usize,
                    );
                    p = buf.as_mut_ptr() as *mut c_char;
                    {
                        let mut fi: OcerzJitFaultInfo = core::mem::zeroed();
                        if ffi::ocerz_jit_fault_info(G_VM, pc0 as *const c_void, &mut fi) != 0 {
                            p = str_into(p, c"  owner-block: rip=".as_ptr());
                            p = hex_into(p, fi.block_rip);
                            p = str_into(p, c" word=".as_ptr());
                            p = hex_into(p, fi.host_word as u64);
                            p = str_into(p, c" insn=".as_ptr());
                            p = hex_into(p, fi.insn_index as i64 as u64);
                            p = str_into(p, c" pin=".as_ptr());
                            p = hex_into(p, fi.pin_class as u64);
                            p = str_into(p, c"\n".as_ptr());
                        } else {
                            p = str_into(p, c"  owner-block: none\n".as_ptr());
                        }
                        write(
                            2,
                            buf.as_ptr() as *const c_void,
                            p as usize - buf.as_ptr() as usize,
                        );
                        p = buf.as_mut_ptr() as *mut c_char;
                    }
                    {
                        let mut w = [0u32; 64];
                        let mut got: vm_size_t = 0;
                        if mach_vm_read_overwrite(
                            mach_task_self(),
                            pc0 - 0x80,
                            core::mem::size_of_val(&w) as mach_vm_size_t,
                            w.as_mut_ptr() as mach_vm_address_t,
                            &mut got as *mut vm_size_t as *mut mach_vm_size_t,
                        ) == KERN_SUCCESS
                            && got == core::mem::size_of_val(&w)
                        {
                            p = str_into(p, c"  mem@pc-80:".as_ptr());
                            for i in 0..64 {
                                p = str_into(
                                    p,
                                    if i == 32 {
                                        c" |".as_ptr()
                                    } else {
                                        c" ".as_ptr()
                                    },
                                );
                                p = hex_into(p, w[i] as u64);
                                if i == 47 {
                                    p = str_into(p, c"\n   ".as_ptr());
                                    write(
                                        2,
                                        buf.as_ptr() as *const c_void,
                                        p as usize - buf.as_ptr() as usize,
                                    );
                                    p = buf.as_mut_ptr() as *mut c_char;
                                }
                            }
                            p = str_into(p, c"\n".as_ptr());
                            write(
                                2,
                                buf.as_ptr() as *const c_void,
                                p as usize - buf.as_ptr() as usize,
                            );
                            p = buf.as_mut_ptr() as *mut c_char;
                        }
                    }
                }
            }
            p = str_into(p, c"  host-stack:".as_ptr());
            {
                let a0 = (*(*uc).uc_mcontext).ss.sp;
                let mut got: vm_size_t = 0;
                let mut w = [0u64; 12];
                if mach_vm_read_overwrite(
                    mach_task_self(),
                    a0,
                    core::mem::size_of_val(&w) as mach_vm_size_t,
                    w.as_mut_ptr() as mach_vm_address_t,
                    &mut got as *mut vm_size_t as *mut mach_vm_size_t,
                ) == KERN_SUCCESS
                {
                    let mut i = 0u64;
                    while i * 8 < got as u64 && i < 12 {
                        p = str_into(p, c" ".as_ptr());
                        p = hex_into(p, w[i as usize]);
                        i += 1;
                    }
                }
            }
            p = str_into(p, c"\n".as_ptr());
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            p = buf.as_mut_ptr() as *mut c_char;
            p = str_into(p, c" host_bt=".as_ptr());
            let mut fp = (*(*uc).uc_mcontext).ss.fp;
            let mut i = 0;
            while i < 8
                && fp != 0
                && (fp & 7) == 0
                && ocerz_addr_readable(fp) != 0
                && ocerz_addr_readable(fp + 8) != 0
            {
                let ret = *((fp + 8) as *const u64);
                if ret == 0 {
                    break;
                }
                p = hex_into(p, ret);
                p = str_into(p, c",".as_ptr());
                fp = *(fp as *const u64);
                i += 1;
            }
        }
        let c = if !G_CUR_CPU.is_null() {
            G_CUR_CPU
        } else if !G_VM.is_null() {
            &raw mut (*G_VM).cpu
        } else {
            ptr::null_mut()
        };
        if !c.is_null() {
            p = str_into(p, c" guest_rip=".as_ptr());
            p = hex_into(p, (*c).rip);
            p = str_into(p, c" guest_addr=".as_ptr());
            p = hex_into(p, ocerz_h2g((*si).si_addr));
            p = str_into(p, c" icount=".as_ptr());
            p = hex_into(
                p,
                if !G_VM.is_null() {
                    (*G_VM).insn_count
                } else {
                    0
                },
            );
            p = str_into(p, c" cur_rip=".as_ptr());
            p = hex_into(p, (*c).cur_rip);
            p = str_into(p, c" interp_once=".as_ptr());
            p = hex_into(p, (*c).interp_once as u64);
            p = str_into(p, c" exec_state=".as_ptr());
            p = hex_into(p, ocerz_jit_exec_state as u64);
        }
        p = str_into(p, c"\n".as_ptr());
        write(
            2,
            buf.as_ptr() as *const c_void,
            p as usize - buf.as_ptr() as usize,
        );
        if !c.is_null() {
            static mut RNM: [*const c_char; 8] = [
                c"rax".as_ptr(),
                c"rcx".as_ptr(),
                c"rdx".as_ptr(),
                c"rbx".as_ptr(),
                c"rsi".as_ptr(),
                c"rdi".as_ptr(),
                c"rbp".as_ptr(),
                c"r8".as_ptr(),
            ];
            static RI: [usize; 8] = [
                OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX, OCERZ_RSI, OCERZ_RDI, OCERZ_RBP,
                OCERZ_R8,
            ];
            p = buf.as_mut_ptr() as *mut c_char;
            p = str_into(p, c"  regs:".as_ptr());
            for i in 0..8 {
                p = str_into(p, c" ".as_ptr());
                p = str_into(p, RNM[i]);
                p = str_into(p, c"=".as_ptr());
                p = hex_into(p, (*c).gpr[RI[i]]);
            }
            p = str_into(p, c"\n".as_ptr());
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            {
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  gs_base=".as_ptr());
                p = hex_into(p, (*c).gs_base);
                p = str_into(p, c" fs_base=".as_ptr());
                p = hex_into(p, (*c).fs_base);
                p = str_into(p, c" insn@rip-24=".as_ptr());
                for i in -24i64..16 {
                    let b_ = ocerz_ld((*c).rip.wrapping_add(i as u64), 1);
                    *p = ' ' as c_char;
                    p = p.add(1);
                    if i == 0 {
                        *p = '[' as c_char;
                        p = p.add(1);
                    }
                    *p = b"0123456789abcdef"[((b_ >> 4) & 0xf) as usize] as c_char;
                    p = p.add(1);
                    *p = b"0123456789abcdef"[(b_ & 0xf) as usize] as c_char;
                    p = p.add(1);
                }
                *p = '\n' as c_char;
                p = p.add(1);
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  r12=".as_ptr());
                p = hex_into(p, (*c).gpr[OCERZ_R12]);
                p = str_into(p, c" r13=".as_ptr());
                p = hex_into(p, (*c).gpr[OCERZ_R13]);
                p = str_into(p, c" r14=".as_ptr());
                p = hex_into(p, (*c).gpr[OCERZ_R14]);
                p = str_into(p, c" r15=".as_ptr());
                p = hex_into(p, (*c).gpr[OCERZ_R15]);
                p = str_into(p, c" [r15+0x18]=".as_ptr());
                p = hex_into(
                    p,
                    if ocerz_addr_readable((*c).gpr[OCERZ_R15] + 0x18) != 0 {
                        ocerz_ld((*c).gpr[OCERZ_R15] + 0x18, 8)
                    } else {
                        0
                    },
                );
                *p = '\n' as c_char;
                p = p.add(1);
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  blockhist:".as_ptr());
                for i in 1..=16u32 {
                    p = str_into(p, c" ".as_ptr());
                    p = hex_into(p, G_RIPHIST[(G_RIPHIST_N.wrapping_sub(i) & 31) as usize]);
                }
                *p = '\n' as c_char;
                p = p.add(1);
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
            }
            {
                let comm = ocerz_addr_committed(ocerz_h2g((*si).si_addr));
                let cs = if comm == 1 {
                    c"  fault-page: COMMITTED".as_ptr()
                } else if comm == 0 {
                    c"  fault-page: UNCOMMITTED".as_ptr()
                } else {
                    c"  fault-page: outside-arena".as_ptr()
                };
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, cs);
                p = str_into(
                    p,
                    if sig == SIGBUS {
                        if (*si).si_code == 1 {
                            c" si_code=ADRALN".as_ptr()
                        } else if (*si).si_code == 2 {
                            c" si_code=ADRERR".as_ptr()
                        } else if (*si).si_code == 3 {
                            c" si_code=OBJERR".as_ptr()
                        } else {
                            c" si_code=?".as_ptr()
                        }
                    } else {
                        c"".as_ptr()
                    },
                );
                let mut rbase: u64 = 0;
                let mut rsize: u64 = 0;
                let hp = ocerz_host_region_prot(ocerz_h2g((*si).si_addr), &mut rbase, &mut rsize);
                p = str_into(p, c" host_prot=".as_ptr());
                p = hex_into(p, hp as u64);
                p = str_into(p, c" region=[".as_ptr());
                p = hex_into(p, rbase);
                p = str_into(p, c",".as_ptr());
                p = hex_into(p, rbase + rsize);
                p = str_into(p, c")\n".as_ptr());
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
                let mut ripbase: u64 = 0;
                let mut ripsize: u64 = 0;
                let riphp = ocerz_host_region_prot((*c).rip, &mut ripbase, &mut ripsize);
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  rip-region=[".as_ptr());
                p = hex_into(p, ripbase);
                p = str_into(p, c",".as_ptr());
                p = hex_into(p, ripbase + ripsize);
                p = str_into(p, c") prot=".as_ptr());
                p = hex_into(p, riphp as u64);
                p = str_into(p, c"\n".as_ptr());
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
            }
            let mut fp = (*c).gpr[OCERZ_RBP];
            p = buf.as_mut_ptr() as *mut c_char;
            p = str_into(p, c"  rbp-chain:".as_ptr());
            let mut d = 0;
            while d < 9 && fp >= 0x300000000 {
                if ocerz_addr_readable(fp) == 0 || ocerz_addr_readable(fp + 8) == 0 {
                    break;
                }
                p = str_into(p, c" ".as_ptr());
                p = hex_into(p, ocerz_ld(fp + 8, 8));
                let nf = ocerz_ld(fp, 8);
                if nf <= fp {
                    break;
                }
                fp = nf;
                d += 1;
            }
            p = str_into(p, c"\n".as_ptr());
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            let pk = libc::getenv(c"OCERZ_PEEK".as_ptr());
            if !pk.is_null() {
                let mut pk = pk;
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  peek:".as_ptr());
                while *pk != 0 {
                    let a = libc::strtoull(pk, &mut pk, 0);
                    if *pk == ',' as c_char {
                        pk = pk.add(1);
                    }
                    p = str_into(p, c" [".as_ptr());
                    p = hex_into(p, a);
                    p = str_into(p, c"]=".as_ptr());
                    if ocerz_addr_readable(a) != 0 {
                        p = hex_into(p, ocerz_ld(a, 8));
                    } else {
                        p = str_into(p, c"uncommitted".as_ptr());
                    }
                }
                p = str_into(p, c"\n".as_ptr());
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
            }
            let sd = libc::getenv(c"OCERZ_STRDUMP".as_ptr());
            if !sd.is_null() {
                let a = libc::strtoull(sd, ptr::null_mut(), 0);
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  strdump@".as_ptr());
                p = hex_into(p, a);
                p = str_into(p, c"=".as_ptr());
                let mut i = 0;
                while i < 200 && (p as usize) < buf.as_ptr() as usize + 250 {
                    let b_ = ocerz_ld(a + i, 1);
                    *p = if b_ >= 32 && b_ < 127 {
                        b_ as c_char
                    } else {
                        '.' as c_char
                    };
                    p = p.add(1);
                    i += 1;
                }
                *p = '\n' as c_char;
                p = p.add(1);
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
            }
            let sp = (*c).gpr[OCERZ_RSP];
            let mut shown = 0usize;
            let mut frames = [0u64; 14];
            p = buf.as_mut_ptr() as *mut c_char;
            p = str_into(p, c"  bt:".as_ptr());
            let mut a = sp;
            while a < sp + 0x400 && shown < 14 {
                if ocerz_addr_readable(a) == 0 {
                    break;
                }
                let v = ocerz_ld(a, 8);
                if v >= 0x7ff802000000 && v < 0x7ff818000000 {
                    p = str_into(p, c" ".as_ptr());
                    p = hex_into(p, v);
                    frames[shown] = v;
                    shown += 1;
                }
                a += 8;
            }
            p = str_into(p, c"\n".as_ptr());
            write(
                2,
                buf.as_ptr() as *const c_void,
                p as usize - buf.as_ptr() as usize,
            );
            for k in 0..shown {
                let mut fb: u64 = 0;
                let fn_ = ocerz_dyld_name_for_addr(frames[k], &mut fb);
                if fn_.is_null() {
                    continue;
                }
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"    frame ".as_ptr());
                p = hex_into(p, frames[k]);
                p = str_into(p, c" ".as_ptr());
                p = str_into(p, fn_);
                p = str_into(p, c"+".as_ptr());
                p = hex_into(p, frames[k] - fb);
                p = str_into(p, c"\n".as_ptr());
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
            }
            if G_CRASH_STACK != 0 {
                static mut AN: [*const c_char; 8] = [
                    c"r9".as_ptr(),
                    c"r10".as_ptr(),
                    c"r11".as_ptr(),
                    c"r12".as_ptr(),
                    c"r13".as_ptr(),
                    c"r14".as_ptr(),
                    c"r15".as_ptr(),
                    c"rsp".as_ptr(),
                ];
                static AI: [usize; 8] = [
                    OCERZ_R9, OCERZ_R10, OCERZ_R11, OCERZ_R12, OCERZ_R13, OCERZ_R14, OCERZ_R15,
                    OCERZ_RSP,
                ];
                p = buf.as_mut_ptr() as *mut c_char;
                p = str_into(p, c"  regs2:".as_ptr());
                for i in 0..8 {
                    p = str_into(p, c" ".as_ptr());
                    p = str_into(p, AN[i]);
                    p = str_into(p, c"=".as_ptr());
                    p = hex_into(p, (*c).gpr[AI[i]]);
                }
                p = str_into(p, c"\n".as_ptr());
                write(
                    2,
                    buf.as_ptr() as *const c_void,
                    p as usize - buf.as_ptr() as usize,
                );
                let rbp = (*c).gpr[OCERZ_RBP];
                for row in 0..14u64 {
                    let base = rbp.wrapping_sub(0x80).wrapping_add(row * 0x10);
                    p = buf.as_mut_ptr() as *mut c_char;
                    p = str_into(p, c"  [rbp".as_ptr());
                    p = str_into(
                        p,
                        if base >= rbp {
                            c"+".as_ptr()
                        } else {
                            c"-".as_ptr()
                        },
                    );
                    p = hex_into(p, if base >= rbp { base - rbp } else { rbp - base });
                    p = str_into(p, c"]:".as_ptr());
                    for col in 0..2u64 {
                        p = str_into(p, c" ".as_ptr());
                        p = hex_into(p, ocerz_ld(base + col * 8, 8));
                    }
                    p = str_into(p, c"\n".as_ptr());
                    write(
                        2,
                        buf.as_ptr() as *const c_void,
                        p as usize - buf.as_ptr() as usize,
                    );
                }
                {
                    let a = (*c).gpr[OCERZ_R14];
                    if a >= 0x100000000 {
                        for row in 0..8usize {
                            p = buf.as_mut_ptr() as *mut c_char;
                            p = str_into(p, c"  *r14+".as_ptr());
                            p = hex_into(p, row as u64 * 0x20);
                            p = str_into(p, c":".as_ptr());
                            for col in 0..4usize {
                                p = str_into(p, c" ".as_ptr());
                                p = hex_into(p, ocerz_ld(a + (row * 4 + col) as u64 * 8, 8));
                            }
                            p = str_into(p, c"\n".as_ptr());
                            write(
                                2,
                                buf.as_ptr() as *const c_void,
                                p as usize - buf.as_ptr() as usize,
                            );
                        }
                    }
                }
            }
        }
        libc::_exit(139);
    }
}

static mut G_HOSTSIG_PROBE_STATE: c_int = 0;
static mut G_HOSTSIG_PROBE_MASK: u32 = 0;
static mut G_HOSTSIG_PROBE_PEND: u32 = 0;

pub(super) unsafe extern "C" fn hostsig_probe_handler(
    _sig: c_int,
    _si: *mut siginfo_t,
    uc_: *mut c_void,
) {
    unsafe {
        let uc = uc_ as *mut ucontext_t;
        let mut pend: sigset_t = core::mem::zeroed();
        let mut m = 0u32;
        let mut p = 0u32;
        sigpending(&mut pend);
        for sg in 1..32 {
            if sigismember(&(*uc).uc_sigmask as *const u32 as *const sigset_t, sg) != 0 {
                m |= 1u32 << sg;
            }
            if sigismember(&pend, sg) != 0 {
                p |= 1u32 << sg;
            }
        }
        G_HOSTSIG_PROBE_MASK = m;
        G_HOSTSIG_PROBE_PEND = p;
        AtomicI32::from_ptr(&raw mut G_HOSTSIG_PROBE_STATE).store(2, Ordering::Release);
    }
}

unsafe fn hostsig_probe(c: *mut OcerzCPU, th: libc::pthread_t) {
    unsafe {
        if libc::getenv(c"OCERZ_HOSTSIG_PROBE".as_ptr()).is_null()
            || pthread_equal(th, pthread_self()) != 0
        {
            return;
        }
        let mut sa: libc::sigaction = core::mem::zeroed();
        let mut old: libc::sigaction = core::mem::zeroed();
        sa.sa_sigaction = hostsig_probe_handler as libc::sighandler_t;
        sa.sa_flags = (SA_SIGINFO | SA_RESTART) as c_int;
        sigemptyset(&mut sa.sa_mask);
        if libc::sigaction(SIGPROF, &sa, &mut old) != 0 {
            return;
        }
        AtomicI32::from_ptr(&raw mut G_HOSTSIG_PROBE_STATE).store(1, Ordering::Release);
        let ok = libc::pthread_kill(th, SIGPROF) == 0;
        let mut w = 0;
        while ok
            && w < 200
            && AtomicI32::from_ptr(&raw mut G_HOSTSIG_PROBE_STATE).load(Ordering::Acquire) != 2
        {
            libc::usleep(250);
            w += 1;
        }
        if AtomicI32::from_ptr(&raw mut G_HOSTSIG_PROBE_STATE).load(Ordering::Acquire) == 2 {
            libc::fprintf(
                stderr(),
                c"ocerz: THREADDUMP-HOSTSIG[%d] cpu#%u host_tid=%#llx mask=%#x pending=%#x\n"
                    .as_ptr(),
                libc::getpid(),
                (*c).cpu_number,
                (*c).host_tid as c_ulonglong,
                G_HOSTSIG_PROBE_MASK,
                G_HOSTSIG_PROBE_PEND,
            );
        } else {
            libc::fprintf(
                stderr(),
                c"ocerz: THREADDUMP-HOSTSIG[%d] cpu#%u host_tid=%#llx no reply (SIGPROF blocked or thread gone)\n"
                    .as_ptr(),
                libc::getpid(),
                (*c).cpu_number,
                (*c).host_tid as c_ulonglong,
            );
        }
        AtomicI32::from_ptr(&raw mut G_HOSTSIG_PROBE_STATE).store(0, Ordering::Release);
        libc::sigaction(SIGPROF, &old, ptr::null_mut());
    }
}

pub(super) unsafe extern "C" fn threaddump_handler(
    _sig: c_int,
    _si: *mut siginfo_t,
    _ctx: *mut c_void,
) {
    unsafe {
        let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        libc::fprintf(
            stderr(),
            c"ocerz: THREADDUMP[%d] begin cpus=%d\n".as_ptr(),
            libc::getpid(),
            G_CPUS_N,
        );
        for i in 0..G_CPUS_N {
            let c = G_CPUS[i as usize];
            if c.is_null() {
                continue;
            }
            let mut tport: mach_port_t = 0;
            for k in 0..G_CPUS_N {
                if G_CPUS[k as usize] == c {
                    tport = pthread_mach_thread_np(G_CPU_THREADS[k as usize]);
                    break;
                }
            }
            libc::fprintf(
                stderr(),
                c"ocerz: THREADDUMP[%d] cpu#%u host_tid=%#llx port=%#x rip=%#llx rsp=%#llx rax=%#llx sys=%d/%d in_sig=%u blocked=%.1fs sigpend=%#llx sigmask=%#llx hostmask=%#x quit=%u/%u usr1=%u/%u handback=%u\n"
                    .as_ptr(),
                libc::getpid(),
                (*c).cpu_number,
                (*c).host_tid as c_ulonglong,
                tport,
                (*c).rip as c_ulonglong,
                (*c).gpr[OCERZ_RSP] as c_ulonglong,
                (*c).gpr[OCERZ_RAX] as c_ulonglong,
                (*c).cur_sys_class,
                (*c).cur_sys_num,
                (*c).in_sighandler,
                if (*c).block_since_ns != 0 {
                    (now - (*c).block_since_ns) as f64 / 1e9
                } else {
                    0.0
                },
                (*c).sig_pending as c_ulonglong,
                (*c).sig_mask as c_ulonglong,
                (*c).host_mask_last,
                (*c).sig_host_rcvd[SIGQUIT as usize],
                (*c).sig_delivered[SIGQUIT as usize],
                (*c).sig_host_rcvd[SIGUSR1 as usize],
                (*c).sig_delivered[SIGUSR1 as usize],
                (*c).nested_sig_handback,
            );
            for k in 0..G_CPUS_N {
                if G_CPUS[k as usize] == c {
                    hostsig_probe(c, G_CPU_THREADS[k as usize]);
                    break;
                }
            }
            let mut fp = (*c).gpr[OCERZ_RBP];
            let sp = (*c).gpr[OCERZ_RSP];
            libc::fprintf(
                stderr(),
                c"ocerz: THREADDUMP[%d] cpu#%u bt: ret=%#llx".as_ptr(),
                libc::getpid(),
                (*c).cpu_number,
                (if ocerz_addr_readable(sp) != 0 {
                    ocerz_ld(sp, 8)
                } else {
                    0
                }) as c_ulonglong,
            );
            let mut d = 0;
            while d < 24 && fp > 0x1000 && (fp & 7) == 0 && ocerz_addr_readable(fp + 8) != 0 {
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
            ocerz_pe_stack_dump(c, c"THREADDUMP-PE".as_ptr());
            let rn = (*c).sysring_n;
            let mut k = if rn > 24 { rn - 24 } else { 0 };
            while k < rn {
                let e = &(*c).sysring[(k % 24) as usize];
                libc::fprintf(
                    stderr(),
                    c"ocerz: THREADDUMP-RING[%d] cpu#%u %8.1fms %s%d/%d a0=%#llx a1=%#llx a2=%#llx ret=%#llx peek=%#llx/%#llx/%#llx/%#llx\n"
                        .as_ptr(),
                    libc::getpid(),
                    (*c).cpu_number,
                    (e.t as i64 - now as i64) as f64 / 1e6,
                    if e.num < 0 { c"sig ".as_ptr() } else { c"".as_ptr() },
                    if e.num < 0 { 0 } else { e.num >> 24 },
                    if e.num < 0 { -e.num } else { e.num & 0xffffff },
                    e.a0 as c_ulonglong,
                    e.a1 as c_ulonglong,
                    e.a2 as c_ulonglong,
                    e.ret as c_ulonglong,
                    e.peek as c_ulonglong,
                    e.peek2 as c_ulonglong,
                    e.peek3 as c_ulonglong,
                    e.peek4 as c_ulonglong,
                );
                k += 1;
            }
        }
        if !libc::getenv(c"OCERZ_HOSTSIG_PROBE".as_ptr()).is_null() {
            let n = G_HSIG_N.load(Ordering::Relaxed);
            let mut k = if n > 512 { n - 512 } else { 0 };
            while k < n {
                let e = &G_HSIG_RING[(k % 512) as usize];
                libc::fprintf(
                    stderr(),
                    c"ocerz: HOSTSIGRX[%d] %10.1fms sig=%d tid=%#llx cpu=%p cpu#%d from=%d\n"
                        .as_ptr(),
                    libc::getpid(),
                    (e.t as i64 - now as i64) as f64 / 1e6,
                    e.sig,
                    e.tid as c_ulonglong,
                    e.cpu as *const c_void,
                    if !e.cpu.is_null() {
                        (*e.cpu).cpu_number
                    } else {
                        -1
                    },
                    e.pid_from,
                );
                k += 1;
            }
        }
        ffi::ocerz_bigring_dump();
        libc::fprintf(
            stderr(),
            c"ocerz: THREADDUMP[%d] end\n".as_ptr(),
            libc::getpid(),
        );
    }
}

pub(super) unsafe extern "C" fn portdump_handler(
    _sig: c_int,
    _si: *mut siginfo_t,
    _ctx: *mut c_void,
) {
    unsafe {
        let mut names: mach_port_name_array_t = ptr::null_mut();
        let mut types: mach_port_type_array_t = ptr::null_mut();
        let mut ncnt: mach_msg_type_number_t = 0;
        let mut tcnt: mach_msg_type_number_t = 0;
        libc::fprintf(
            stderr(),
            c"ocerz: PORTDUMP[%d] begin cpus=%d\n".as_ptr(),
            libc::getpid(),
            G_CPUS_N,
        );
        let pkr = mach_port_names(
            mach_task_self(),
            &mut names,
            &mut ncnt,
            &mut types,
            &mut tcnt,
        );
        if pkr != KERN_SUCCESS {
            libc::fprintf(
                stderr(),
                c"ocerz: PORTDUMP[%d] mach_port_names failed kr=%d\n".as_ptr(),
                libc::getpid(),
                pkr,
            );
            ncnt = 0;
            names = ptr::null_mut();
            types = ptr::null_mut();
        }
        libc::fprintf(
            stderr(),
            c"ocerz: PORTDUMP[%d] rights=%u\n".as_ptr(),
            libc::getpid(),
            ncnt,
        );
        {
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            for i in 0..G_CPUS_N {
                let c = G_CPUS[i as usize];
                if c.is_null() {
                    continue;
                }
                libc::fprintf(
                    stderr(),
                    c"ocerz: PORTDUMP[%d] cpu#%u rip=%#llx rax=%#llx rdi=%#llx blocked=%.1fs rcv_name=%#x usr1=%u/%u sigpend=%#llx sigmask=%#llx hostmask=%#x(%u) sends:"
                        .as_ptr(),
                    libc::getpid(),
                    (*c).cpu_number,
                    (*c).rip as c_ulonglong,
                    (*c).gpr[OCERZ_RAX] as c_ulonglong,
                    (*c).gpr[OCERZ_RDI] as c_ulonglong,
                    if (*c).block_since_ns != 0 {
                        (now - (*c).block_since_ns) as f64 / 1e9
                    } else {
                        0.0
                    },
                    (*c).last_rcv_name,
                    (*c).sig_host_rcvd[30],
                    (*c).sig_delivered[30],
                    (*c).sig_pending as c_ulonglong,
                    (*c).sig_mask as c_ulonglong,
                    (*c).host_mask_last,
                    (*c).host_mask_changes,
                );
                let mut k = 0;
                while k < 8 && k < (*c).sendring_n {
                    let j = ((*c).sendring_n - 1 - k) % 8;
                    libc::fprintf(
                        stderr(),
                        c" [id=%u port=%#x sz=%u]".as_ptr(),
                        (*c).sendring_id[j as usize],
                        (*c).sendring_port[j as usize],
                        (*c).sendring_sz[j as usize],
                    );
                    k += 1;
                }
                libc::fprintf(stderr(), c"\n".as_ptr());
                let sp = (*c).gpr[OCERZ_RSP];
                let mut fp = (*c).gpr[OCERZ_RBP];
                libc::fprintf(
                    stderr(),
                    c"ocerz: PORTDUMP[%d] cpu#%u rsp=%#llx rbp=%#llx ret-chain:".as_ptr(),
                    libc::getpid(),
                    (*c).cpu_number,
                    sp as c_ulonglong,
                    fp as c_ulonglong,
                );
                let mut d = 0;
                while d < 12 && ocerz_addr_readable(fp) != 0 && ocerz_addr_readable(fp + 8) != 0 {
                    libc::fprintf(
                        stderr(),
                        c" %#llx".as_ptr(),
                        ocerz_ld(fp + 8, 8) as c_ulonglong,
                    );
                    let nfp = ocerz_ld(fp, 8);
                    if nfp <= fp {
                        break;
                    }
                    fp = nfp;
                    d += 1;
                }
                libc::fprintf(
                    stderr(),
                    c"\nocerz: PORTDUMP[%d] cpu#%u stackscan:".as_ptr(),
                    libc::getpid(),
                    (*c).cpu_number,
                );
                let mut printed = 0;
                let mut o = 0u64;
                while o < 0x4000 && printed < 40 {
                    if ocerz_addr_readable(sp + o) == 0 {
                        break;
                    }
                    let v = ocerz_ld(sp + o, 8);
                    let codey = (v >= 0x6fff00000000 && v < 0x7ffc00000000)
                        || (v >= 0x100000000 && v < 0x200000000);
                    if codey {
                        libc::fprintf(
                            stderr(),
                            c" +%llx:%#llx".as_ptr(),
                            o as c_ulonglong,
                            v as c_ulonglong,
                        );
                        printed += 1;
                    }
                    o += 8;
                }
                libc::fprintf(stderr(), c"\n".as_ptr());
                let teb = if (*c).gs_base != 0 && ocerz_addr_readable((*c).gs_base + 0x30) != 0 {
                    ocerz_ld((*c).gs_base + 0x30, 8)
                } else {
                    0
                };
                let frame = if teb != 0 && ocerz_addr_readable(teb + 0x378) != 0 {
                    ocerz_ld(teb + 0x378, 8)
                } else {
                    0
                };
                if frame != 0
                    && ocerz_addr_readable(frame) != 0
                    && ocerz_addr_readable(frame + 0xa0) != 0
                {
                    let urip = ocerz_ld(frame + 0x70, 8);
                    let ursp = ocerz_ld(frame + 0x88, 8);
                    let ucs = ocerz_ld(frame + 0x78, 8);
                    let wtid = if ocerz_addr_readable(teb + 0x48) != 0 {
                        ocerz_ld(teb + 0x48, 8)
                    } else {
                        0
                    };
                    libc::fprintf(
                        stderr(),
                        c"ocerz: PORTDUMP[%d] cpu#%u teb=%#llx tid=%04llx cs=%#llx pe-rip=%#llx pe-rsp=%#llx pe-scan:"
                            .as_ptr(),
                        libc::getpid(),
                        (*c).cpu_number,
                        teb as c_ulonglong,
                        wtid as c_ulonglong,
                        ucs as c_ulonglong,
                        urip as c_ulonglong,
                        ursp as c_ulonglong,
                    );
                    printed = 0;
                    let mut o = 0u64;
                    while o < 0x8000 && printed < 48 {
                        if ocerz_addr_readable(ursp + o) == 0 {
                            break;
                        }
                        let v = ocerz_ld(ursp + o, 8);
                        let codey = (v >= 0x6fff00000000 && v < 0x7ffc00000000)
                            || (v >= 0x100000000 && v < 0x200000000);
                        if codey {
                            libc::fprintf(
                                stderr(),
                                c" +%llx:%#llx".as_ptr(),
                                o as c_ulonglong,
                                v as c_ulonglong,
                            );
                            printed += 1;
                        }
                        o += 8;
                    }
                    libc::fprintf(stderr(), c"\n".as_ptr());
                } else {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: PORTDUMP[%d] cpu#%u teb=%#llx frame=%#llx (no pe frame)\n"
                            .as_ptr(),
                        libc::getpid(),
                        (*c).cpu_number,
                        teb as c_ulonglong,
                        frame as c_ulonglong,
                    );
                }
            }
        }
        {
            let mut cur: libc::sigaction = core::mem::zeroed();
            let mut hm: sigset_t = core::mem::zeroed();
            sigemptyset(&mut hm);
            if libc::sigaction(SIGUSR1, ptr::null(), &mut cur) == 0 {
                libc::fprintf(
                    stderr(),
                    c"ocerz: PORTDUMP[%d] host SIGUSR1 disposition=%p (async_sig_handler=%p) flags=%#x\n"
                        .as_ptr(),
                    libc::getpid(),
                    cur.sa_sigaction as *const c_void,
                    async_sig_handler as *const c_void,
                    cur.sa_flags,
                );
            }
            if pthread_sigmask(SIG_BLOCK, ptr::null(), &mut hm) == 0 {
                libc::fprintf(
                    stderr(),
                    c"ocerz: PORTDUMP[%d] this host thread sigmask has SIGUSR1 blocked=%d\n"
                        .as_ptr(),
                    libc::getpid(),
                    sigismember(&hm, SIGUSR1),
                );
            }
        }
        if !libc::getenv(c"OCERZ_PORTDUMP_KICK".as_ptr()).is_null() {
            let mut before = [0u32; 64];
            let nb = if G_CPUS_N < 64 { G_CPUS_N } else { 64 };
            for i in 0..nb {
                if !G_CPUS[i as usize].is_null() {
                    before[i as usize] = (*G_CPUS[i as usize]).sig_host_rcvd[30];
                }
            }
            for i in 0..nb {
                let c = G_CPUS[i as usize];
                if c.is_null()
                    || (*c).host_pthread.is_null()
                    || pthread_equal((*c).host_pthread as libc::pthread_t, pthread_self()) != 0
                {
                    continue;
                }
                let rc = libc::pthread_kill((*c).host_pthread as libc::pthread_t, SIGUSR1);
                if rc != 0 {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: PORTDUMP[%d] KICK cpu#%u pthread_kill failed rc=%d\n".as_ptr(),
                        libc::getpid(),
                        (*c).cpu_number,
                        rc,
                    );
                }
            }
            libc::usleep(400000);
            for i in 0..nb {
                let c = G_CPUS[i as usize];
                if c.is_null() {
                    continue;
                }
                libc::fprintf(
                    stderr(),
                    c"ocerz: PORTDUMP[%d] KICK cpu#%u kport=%#x rcvd %u -> %u delivered=%u%s\n"
                        .as_ptr(),
                    libc::getpid(),
                    (*c).cpu_number,
                    (*c).host_kport,
                    before[i as usize],
                    (*c).sig_host_rcvd[30],
                    (*c).sig_delivered[30],
                    if (*c).sig_host_rcvd[30] == before[i as usize]
                        && !(*c).host_pthread.is_null()
                        && pthread_equal((*c).host_pthread as libc::pthread_t, pthread_self()) != 0
                    {
                        c"   <== NOT RECEIVED".as_ptr()
                    } else {
                        c"".as_ptr()
                    },
                );
            }
        }
        for i in 0..ncnt {
            if *types.add(i as usize) & MACH_PORT_TYPE_RECEIVE == 0 {
                continue;
            }
            let mut st: mach_port_status = core::mem::zeroed();
            let mut cnt: mach_msg_type_number_t = MACH_PORT_RECEIVE_STATUS_COUNT;
            if mach_port_get_attributes(
                mach_task_self(),
                *names.add(i as usize),
                MACH_PORT_RECEIVE_STATUS,
                &mut st as *mut _ as mach_port_info_t,
                &mut cnt,
            ) != KERN_SUCCESS
            {
                continue;
            }
            if st.mps_msgcount == 0 {
                continue;
            }
            let mut seq: mach_port_seqno_t = 0;
            let mut msize: mach_msg_size_t = 0;
            let mut mid: mach_msg_id_t = 0;
            let mut tinfo = [0u8; 68];
            let mut tsz: mach_msg_type_number_t = 68;
            let pk = mach_port_peek(
                mach_task_self(),
                *names.add(i as usize),
                MACH_RCV_TRAILER_NULL,
                &mut seq,
                &mut msize,
                &mut mid,
                tinfo.as_mut_ptr() as mach_msg_trailer_info_t,
                &mut tsz,
            );
            libc::fprintf(
                stderr(),
                c"ocerz: PORTDUMP[%d] port=%#x msgs=%u psets=%u srights=%u sorights=%u peek_kr=%d id=%#x size=%u\n"
                    .as_ptr(),
                libc::getpid(),
                *names.add(i as usize),
                st.mps_msgcount,
                st.mps_pset,
                st.mps_srights,
                st.mps_sorights,
                pk,
                mid,
                msize,
            );
        }
        vm_deallocate(
            mach_task_self(),
            names as vm_address_t,
            ncnt as usize * core::mem::size_of::<mach_port_name_t>(),
        );
        vm_deallocate(
            mach_task_self(),
            types as vm_address_t,
            tcnt as usize * core::mem::size_of::<mach_port_type_t>(),
        );
    }
}

pub(super) unsafe extern "C" fn async_sig_handler(
    sig: c_int,
    si: *mut siginfo_t,
    _ctx: *mut c_void,
) {
    unsafe {
        {
            let k = (G_HSIG_N.fetch_add(1, Ordering::Relaxed) % 512) as usize;
            let mut tid: u64 = 0;
            pthread_threadid_np(0, &mut tid);
            G_HSIG_RING[k].t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            G_HSIG_RING[k].tid = tid;
            G_HSIG_RING[k].cpu = G_CUR_CPU;
            G_HSIG_RING[k].sig = sig;
            G_HSIG_RING[k].pid_from = if !si.is_null() { (*si).si_pid } else { -1 };
        }
        if sig > 0 && sig < 32 {
            let c = G_CUR_CPU;
            if !c.is_null() && G_ASYNC_SHARED_ONLY == 0 {
                AtomicU64::from_ptr(&raw mut (*c).sig_pending)
                    .fetch_or(1u64 << (sig - 1), Ordering::SeqCst);
            } else {
                AtomicU32::from_ptr(&raw mut G_PENDING_ASYNC_MASK_TLS)
                    .fetch_or(1u32 << sig, Ordering::SeqCst);
            }
            if !c.is_null() {
                AtomicU32::from_ptr(&raw mut (*c).sig_host_rcvd[sig as usize])
                    .fetch_add(1, Ordering::Relaxed);
            }
        }
    }
}

unsafe extern "C" fn ocerz_kick_handler(_sig: c_int, _si: *mut siginfo_t, _uc: *mut c_void) {}

pub(super) unsafe fn ocerz_install_kick_handler() {
    unsafe {
        let mut sa: libc::sigaction = core::mem::zeroed();
        sa.sa_sigaction = ocerz_kick_handler as libc::sighandler_t;
        sa.sa_flags = (SA_SIGINFO | SA_ONSTACK) as c_int;
        sigemptyset(&mut sa.sa_mask);
        libc::sigaction(SIGEMT, &sa, ptr::null_mut());
    }
}
