//! Mach message bridge and address translation for out-of-line descriptors,
//! timer units, VM requests, and thread time-constraint policies.

use super::util::*;
use super::*;

use core::ffi::{c_int, c_void};
use core::ptr;

const OCERZ_OOL_COPY_MAX: u64 = 64 * 1024 * 1024;
const OCERZ_HOST_PAGE_SIZE: u64 = 0x4000;
const OCERZ_EVFILT_TIMER: i16 = -7;
const OCERZ_NOTE_LEEWAY: u32 = 0x10;
const OCERZ_NOTE_MACHTIME: u32 = 0x100;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(super) struct OcerzOolSave {
    pub(super) off: u64,
    pub(super) orig: u64,
}

#[repr(C)]
struct VmmapPad {
    armed: c_int,
    head_lo: u64,
    head_n: u64,
    tail_lo: u64,
    tail_n: u64,
    head: *mut u8,
    tail: *mut u8,
}

#[repr(C)]
pub(super) struct TcPolicySave {
    pub(super) msg: u64,
    pub(super) orig: [u32; 3],
    pub(super) n: c_int,
}

#[repr(C)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

#[thread_local]
static mut G_VMMAP_PAD: VmmapPad = VmmapPad {
    armed: 0,
    head_lo: 0,
    head_n: 0,
    tail_lo: 0,
    tail_n: 0,
    head: ptr::null_mut(),
    tail: ptr::null_mut(),
};
static mut G_SENDXLATE_OFF: c_int = -1;
static mut G_PEER_INIT: c_int = 0;
static mut G_PEER_LOW: u64 = 0;
static mut G_PEER_TOP: u64 = 0;

unsafe extern "C" {
    fn mach_msg_destroy(msg: *mut c_void);
    fn mach_vm_deallocate(task: mach_port_t, addr: u64, size: u64) -> c_int;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
    fn mach_port_type(task: mach_port_t, name: mach_port_t, ptype: *mut u32) -> c_int;
    static mach_task_self_: mach_port_t;
    fn semaphore_signal(semaphore: semaphore_t) -> c_int;
}

pub(super) unsafe fn ocerz_release_received_ool(
    descriptor: *const MachMsgDescriptor,
    ty: u8,
    address: u64,
    bytes: u64,
    keep_port_rights: c_int,
) {
    unsafe {
        if address == 0 || bytes == 0 || !libc::getenv(c"OCERZ_KEEP_RAW_OOL".as_ptr()).is_null() {
            return;
        }
        if ty == MACH_MSG_OOL_PORTS_DESCRIPTOR as u8 && keep_port_rights == 0 {
            let mut message = [0u64; 8];
            let p = message.as_mut_ptr().cast::<u8>();
            ptr::write_unaligned(p.cast::<u32>(), MACH_MSGH_BITS_COMPLEX);
            ptr::write_unaligned(p.add(4).cast::<u32>(), 56);
            ptr::write_unaligned(p.add(24).cast::<u32>(), 1);
            ptr::copy_nonoverlapping(
                descriptor.cast::<u8>(),
                p.add(28),
                core::mem::size_of::<MachMsgOolPortsDescriptor>(),
            );
            mach_msg_destroy(p.cast());
            return;
        }
        mach_vm_deallocate(mach_task_self_, address, bytes);
    }
}

pub(super) unsafe fn ocerz_bridge_mach_msg(hbuf: u64, sz: u64) -> u64 {
    unsafe {
        if hbuf == 0 || sz == 0 || sz > 0x100000 {
            return 0;
        }
        let mut total = sz;
        if libc::getenv(c"OCERZ_NO_AUXTAIL".as_ptr()).is_null() {
            let mut aux = [0u32; 2];
            ptr::copy_nonoverlapping(
                hbuf.wrapping_add(sz) as usize as *const u32,
                aux.as_mut_ptr(),
                2,
            );
            if (8..=0x4000).contains(&aux[0]) && aux[0] & 7 == 0 && aux[1] == 0 {
                total = sz.wrapping_add(aux[0] as u64);
            }
        }
        let gbuf = crate::ffi::ocerz_map_anywhere(
            total.wrapping_add(0x3fff) & !0x3fff,
            libc::PROT_READ | libc::PROT_WRITE,
        );
        if gbuf == 0 {
            return 0;
        }
        let g = ocerz_g2h(gbuf).cast::<u8>();
        ptr::copy_nonoverlapping(hbuf as usize as *const u8, g, total as usize);
        static MM_LOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut log = MM_LOG.load(core::sync::atomic::Ordering::Relaxed);
        if log < 0 {
            log = c_int::from(!libc::getenv(c"OCERZ_MACHMSG".as_ptr()).is_null());
            MM_LOG.store(log, core::sync::atomic::Ordering::Relaxed);
        }
        let mut mm_ool = 0;
        let mut mm_oolfail = 0;
        let mut mm_unhandled = 0;
        let bits = ptr::read_unaligned(g.cast::<u32>());
        if bits & 0x80000000 != 0 {
            let dcnt = ptr::read_unaligned(g.add(0x18).cast::<u32>());
            let mut off = 0x1cu64;
            for _ in 0..dcnt {
                if off.wrapping_add(12) > sz {
                    break;
                }
                let ty = *g.add(off as usize + 11);
                if ty == 0 {
                    off += 12;
                    continue;
                }
                if ty == 4 {
                    off += 16;
                    continue;
                }
                if ty == 1 || ty == 2 || ty == 3 {
                    if off + 16 > sz {
                        break;
                    }
                    let ool_addr = ptr::read_unaligned(g.add(off as usize).cast::<u64>());
                    let ool_n = ptr::read_unaligned(g.add(off as usize + 12).cast::<u32>());
                    let bytes = if ty == 2 {
                        (ool_n as u64).wrapping_mul(4)
                    } else {
                        ool_n as u64
                    };
                    if log != 0 {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: BRIDGE-OOL type=%u ool_addr=%#llx bytes=%#llx\n".as_ptr(),
                            ty as libc::c_uint,
                            ool_addr as libc::c_ulonglong,
                            bytes as libc::c_ulonglong,
                        );
                    }
                    if ool_addr != 0 && bytes != 0 {
                        let ogb = if bytes <= OCERZ_OOL_COPY_MAX {
                            crate::ffi::ocerz_map_anywhere(
                                bytes.wrapping_add(0x3fff) & !0x3fff,
                                libc::PROT_READ | libc::PROT_WRITE,
                            )
                        } else {
                            0
                        };
                        if ogb != 0 {
                            ptr::copy_nonoverlapping(
                                ool_addr as usize as *const u8,
                                ocerz_g2h(ogb).cast(),
                                bytes as usize,
                            );
                            ptr::write_unaligned(g.add(off as usize).cast::<u64>(), ogb);
                            mm_ool += 1;
                        } else {
                            ptr::write_unaligned(g.add(off as usize).cast::<u64>(), 0);
                            ptr::write_unaligned(g.add(off as usize + 12).cast::<u32>(), 0);
                            mm_oolfail += 1;
                        }
                    }
                    off += 16;
                    continue;
                }
                if log != 0 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: BRIDGE unknown desc type=%u off=%#llx dcnt=%u (walk aborts; rest un-relocated)\n".as_ptr(),
                        ty as libc::c_uint,
                        off as libc::c_ulonglong,
                        dcnt,
                    );
                }
                mm_unhandled = 1;
                break;
            }
        }
        if log != 0 {
            let msgh_size = ptr::read_unaligned(g.add(4).cast::<u32>());
            let msgh_id = ptr::read_unaligned(g.add(0x14).cast::<u32>());
            let dcnt = if bits & 0x80000000 != 0 {
                ptr::read_unaligned(g.add(0x18).cast::<u32>())
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: BRIDGE-MSG recv_sz=%#llx msgh_size=%#x bits=%#x id=%u dcnt=%u ool=%d oolfail=%d unhandled=%d%s\n".as_ptr(),
                sz as libc::c_ulonglong,
                msgh_size,
                bits,
                msgh_id,
                dcnt,
                mm_ool,
                mm_oolfail,
                mm_unhandled,
                if msgh_size as u64 > sz {
                    c" **MSGH_SIZE>RECV**".as_ptr()
                } else {
                    c"".as_ptr()
                },
            );
        }
        hostwq::ocerz_shadow_scan(
            c"bridge".as_ptr(),
            ocerz_ld(gbuf.wrapping_add(0x14), 4) as u32 as u64,
            gbuf,
            total,
        );
        gbuf
    }
}

pub(super) fn ocerz_mach_err_interesting(r: u64) -> c_int {
    if r & 0xfffff000 == 0x10000000 {
        return 1;
    }
    c_int::from(r & 0xfffff000 == 0x10004000 && r != 0x10004003 && r != 0x10004004)
}

unsafe fn timebase() -> (u32, u32) {
    static NUM: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
    static DEN: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
    let mut num = NUM.load(core::sync::atomic::Ordering::Relaxed);
    let mut den = DEN.load(core::sync::atomic::Ordering::Relaxed);
    if den == 0 {
        let mut tb = MachTimebaseInfo { numer: 0, denom: 0 };
        let kr = unsafe { mach_timebase_info(&mut tb) };
        if kr != libc::KERN_SUCCESS || tb.numer == 0 || tb.denom == 0 {
            num = 1;
            den = 1;
        } else {
            num = tb.numer;
            den = tb.denom;
        }
        NUM.store(num, core::sync::atomic::Ordering::Relaxed);
        DEN.store(den, core::sync::atomic::Ordering::Relaxed);
    }
    (num, den)
}

pub(super) unsafe fn ocerz_guest_ns_to_host_ticks(ns: u64) -> u64 {
    let (numer, denom) = unsafe { timebase() };
    if numer == denom || ns == 0 {
        ns
    } else {
        ((ns as u128 * denom as u128) / numer as u128) as u64
    }
}

pub(super) unsafe fn ocerz_host_ticks_to_guest_ns(ticks: u64) -> u64 {
    let (numer, denom) = unsafe { timebase() };
    if numer == denom || ticks == 0 {
        ticks
    } else {
        ((ticks as u128 * numer as u128) / denom as u128) as u64
    }
}

const fn tc_policy_flavor(flavor: u32) -> bool {
    flavor == 2 || flavor == 10
}

unsafe fn tc_policy_xlate_off() -> c_int {
    static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
    if off < 0 {
        off = c_int::from(!unsafe { libc::getenv(c"OCERZ_NO_TC_XLATE".as_ptr()) }.is_null());
        OFF.store(off, core::sync::atomic::Ordering::Relaxed);
    }
    off
}

pub(super) unsafe fn tc_policy_send(gmsg: u64, send_size: u32, ts: *mut TcPolicySave) {
    unsafe {
        (*ts).n = 0;
        if gmsg == 0
            || tc_policy_xlate_off() != 0
            || ocerz_ld(gmsg.wrapping_add(0x14), 4) as u32 != 3617
            || ocerz_ld(gmsg, 4) as u32 & 0x80000000 != 0
        {
            return;
        }
        let msz = ocerz_ld(gmsg.wrapping_add(4), 4) as u32;
        let flavor = ocerz_ld(gmsg.wrapping_add(0x20), 4) as u32;
        let cnt = ocerz_ld(gmsg.wrapping_add(0x24), 4) as u32;
        if msz < 0x34
            || (send_size != 0 && send_size < msz)
            || !tc_policy_flavor(flavor)
            || !(3..=16).contains(&cnt)
            || 0x28 + cnt * 4 > msz
        {
            return;
        }
        (*ts).msg = gmsg;
        for i in 0..3 {
            (*ts).orig[i] = ocerz_ld(gmsg.wrapping_add(0x28 + i as u64 * 4), 4) as u32;
            let ticks = ocerz_guest_ns_to_host_ticks((*ts).orig[i] as u64);
            ocerz_st(
                gmsg.wrapping_add(0x28 + i as u64 * 4),
                4,
                ticks.min(u32::MAX as u64),
            );
        }
        (*ts).n = 3;
        if env_set!("OCERZ_POLICYLOG") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TCPOLICY[%d] set flavor=%u ns %u/%u/%u -> ticks %u/%u/%u\n".as_ptr(),
                libc::getpid(),
                flavor,
                (*ts).orig[0],
                (*ts).orig[1],
                (*ts).orig[2],
                ocerz_ld(gmsg.wrapping_add(0x28), 4) as u32,
                ocerz_ld(gmsg.wrapping_add(0x2c), 4) as u32,
                ocerz_ld(gmsg.wrapping_add(0x30), 4) as u32,
            );
        }
    }
}

pub(super) unsafe fn tc_policy_send_done(ts: *const TcPolicySave, kr: u64) {
    unsafe {
        if (*ts).n == 0 || kr & 0xfffff000 != 0x10000000 {
            return;
        }
        for i in 0..3 {
            ocerz_st(
                (*ts).msg.wrapping_add(0x28 + i as u64 * 4),
                4,
                (*ts).orig[i] as u64,
            );
        }
    }
}

pub(super) unsafe fn tc_policy_get_request(gmsg: u64) -> c_int {
    unsafe {
        c_int::from(
            gmsg != 0
                && tc_policy_xlate_off() == 0
                && ocerz_ld(gmsg.wrapping_add(0x14), 4) as u32 == 3618
                && ocerz_ld(gmsg.wrapping_add(4), 4) as u32 >= 0x2c
                && tc_policy_flavor(ocerz_ld(gmsg.wrapping_add(0x20), 4) as u32),
        )
    }
}

pub(super) unsafe fn tc_policy_get_reply(reply: u64, rcv_size: u32) {
    unsafe {
        if reply == 0 || ocerz_ld(reply.wrapping_add(0x14), 4) as u32 != 3718 {
            return;
        }
        let rsize = ocerz_ld(reply.wrapping_add(4), 4) as u32;
        if rcv_size != 0 && rsize > rcv_size {
            return;
        }
        let cnt = ocerz_ld(reply.wrapping_add(0x24), 4) as u32;
        if rsize < 0x34
            || ocerz_ld(reply.wrapping_add(0x20), 4) != 0
            || !(3..=16).contains(&cnt)
            || 0x28 + cnt * 4 > rsize
        {
            return;
        }
        for i in 0..3 {
            let ns = ocerz_host_ticks_to_guest_ns(ocerz_ld(
                reply.wrapping_add(0x28 + i as u64 * 4),
                4,
            ) as u32 as u64);
            ocerz_st(
                reply.wrapping_add(0x28 + i as u64 * 4),
                4,
                ns.min(u32::MAX as u64),
            );
        }
    }
}

pub(super) unsafe fn sys_gettimeofday(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let args = &*a;
        let mut fa = *args;
        for i in 0..3 {
            if fa[i] != 0 {
                fa[i] = ocerz_g2h(fa[i]) as usize as u64;
            }
        }
        let mut ret2 = 0u64;
        let mut err = 0;
        let r = raw::ocerz_host_syscall(116, &fa, &mut ret2, &mut err);
        if err != 0 {
            ret_err(cpu, r as i32 as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if args[2] != 0 {
            ocerz_st(
                args[2],
                8,
                ocerz_host_ticks_to_guest_ns(ocerz_ld(args[2], 8)),
            );
        }
        ret_ok(cpu, r);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn ocerz_kev_timer_to_host(hev: *mut c_void, nev: c_int) {
    unsafe {
        if hev.is_null() || nev <= 0 {
            return;
        }
        let stride = workers::ocerz_kev_stride() as usize;
        for i in 0..nev as usize {
            let e = hev.cast::<u8>().add(i * stride);
            if ptr::read_unaligned(e.add(8).cast::<i16>()) != OCERZ_EVFILT_TIMER {
                continue;
            }
            let fflags = ptr::read_unaligned(e.add(0x18).cast::<u32>());
            if fflags & OCERZ_NOTE_MACHTIME == 0 {
                continue;
            }
            let value = ptr::read_unaligned(e.add(0x20).cast::<u64>());
            ptr::write_unaligned(
                e.add(0x20).cast::<u64>(),
                ocerz_guest_ns_to_host_ticks(value),
            );
            if fflags & OCERZ_NOTE_LEEWAY != 0 {
                let value = ptr::read_unaligned(e.add(0x30).cast::<u64>());
                ptr::write_unaligned(
                    e.add(0x30).cast::<u64>(),
                    ocerz_guest_ns_to_host_ticks(value),
                );
            }
        }
    }
}

pub(super) unsafe fn ocerz_log_mach_send_err(
    trap: c_int,
    kr: u64,
    a: *const [u64; 8],
    gmsg: u64,
    cpu: *const OcerzCPU,
) {
    static SLOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut slog = SLOG.load(core::sync::atomic::Ordering::Relaxed);
        if slog < 0 {
            slog = c_int::from(!libc::getenv(c"OCERZ_MACHMSG".as_ptr()).is_null());
            SLOG.store(slog, core::sync::atomic::Ordering::Relaxed);
        }
        if slog == 0 {
            return;
        }
        let a = &*a;
        let rbits = a[2] as u32;
        let rsize = (a[2] >> 32) as u32;
        let rdcnt = a[5] as u32;
        let complex = if trap == 47 {
            rbits & 0x80000000 != 0
        } else {
            ocerz_ld(gmsg, 4) as u32 & 0x80000000 != 0
        };
        let mut dc = if complex {
            if trap == 47 {
                rdcnt
            } else {
                ocerz_ld(gmsg.wrapping_add(0x18), 4) as u32
            }
        } else {
            0
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MACH-SEND-ERR(t%d)[%d] kr=%#llx opt=%#llx REG{bits=%#x size=%#x ports=%#llx dcnt=%u} BUF{bits=%#x size=%#x} complex=%d".as_ptr(),
            trap,
            libc::getpid(),
            kr as libc::c_ulonglong,
            a[1] as libc::c_ulonglong,
            rbits,
            rsize,
            a[3] as libc::c_ulonglong,
            rdcnt,
            ocerz_ld(gmsg, 4) as u32,
            ocerz_ld(gmsg.wrapping_add(4), 4) as u32,
            c_int::from(complex),
        );
        dc = dc.min(8);
        for di in 0..dc {
            let doff = gmsg.wrapping_add(0x1c + di as u64 * 16);
            let da = ocerz_ld(doff, 8);
            libc::fprintf(
                crate::log::stderr(),
                c" d%u{addr=%#llx type=%u committed=%d low=%d}".as_ptr(),
                di,
                da as libc::c_ulonglong,
                ocerz_ld(doff.wrapping_add(11), 1) as u8 as u32,
                crate::ffi::ocerz_addr_committed(da),
                c_int::from(da < crate::ffi::OCERZ_LOW_LIMIT),
            );
        }
        libc::fprintf(crate::log::stderr(), c" buf:".as_ptr());
        for k in (0..0x30).step_by(8) {
            libc::fprintf(
                crate::log::stderr(),
                c" %016llx".as_ptr(),
                ocerz_ld(gmsg.wrapping_add(k), 8) as libc::c_ulonglong,
            );
        }
        let dest = if trap == 47 {
            a[3] as mach_port_t
        } else {
            ocerz_ld(gmsg.wrapping_add(8), 4) as u32
        };
        let mut ptype = 0u32;
        let pkr = mach_port_type(mach_task_self_, dest, &mut ptype);
        libc::fprintf(
            crate::log::stderr(),
            c" dest=%#x type_kr=%d type=%#x rip=%#llx ret0=%#llx bt:".as_ptr(),
            dest,
            pkr,
            ptype,
            (*cpu).rip as libc::c_ulonglong,
            ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong,
        );
        let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
        for _ in 0..6 {
            if fp <= 0x1000 {
                break;
            }
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
            );
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
}

unsafe fn vmmap_pad_take(lo: u64, n: u64) -> *mut u8 {
    unsafe {
        if n == 0 {
            return ptr::null_mut();
        }
        let b = libc::malloc(n as usize).cast::<u8>();
        if b.is_null() {
            return ptr::null_mut();
        }
        for i in 0..n {
            if crate::ffi::ocerz_addr_readable(lo.wrapping_add(i)) == 0
                && crate::ffi::ocerz_cache_region(lo.wrapping_add(i) as usize) == 0
            {
                libc::free(b.cast());
                return ptr::null_mut();
            }
            *b.add(i as usize) = ocerz_ld(lo.wrapping_add(i), 1) as u8;
        }
        b
    }
}

unsafe fn vmmap_pad_save(gmsg: u64, send_size: u32) {
    unsafe {
        G_VMMAP_PAD.armed = 0;
        if send_size != 0 && send_size < 0x4c {
            return;
        }
        let flags = ocerz_ld(gmsg.wrapping_add(0x48), 4) as u32;
        if flags & 1 != 0 || flags & 0x4000 == 0 {
            return;
        }
        let addr = ocerz_ld(gmsg.wrapping_add(0x30), 8);
        let size = ocerz_ld(gmsg.wrapping_add(0x38), 8);
        if addr == 0 || size == 0 || size > 1 << 30 {
            return;
        }
        let hp = OCERZ_HOST_PAGE_SIZE;
        let lo = addr & !(hp - 1);
        let hi = addr.wrapping_add(size).wrapping_add(hp - 1) & !(hp - 1);
        if lo == addr && hi == addr.wrapping_add(size) {
            return;
        }
        G_VMMAP_PAD.head_lo = lo;
        G_VMMAP_PAD.head_n = addr - lo;
        G_VMMAP_PAD.tail_lo = addr.wrapping_add(size);
        G_VMMAP_PAD.tail_n = hi.wrapping_sub(addr.wrapping_add(size));
        G_VMMAP_PAD.head = vmmap_pad_take(G_VMMAP_PAD.head_lo, G_VMMAP_PAD.head_n);
        G_VMMAP_PAD.tail = vmmap_pad_take(G_VMMAP_PAD.tail_lo, G_VMMAP_PAD.tail_n);
        G_VMMAP_PAD.armed = c_int::from(!G_VMMAP_PAD.head.is_null() || !G_VMMAP_PAD.tail.is_null());
        if G_VMMAP_PAD.armed != 0 && !libc::getenv(c"OCERZ_VMMAPLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: VMMAP-PAD save addr=%#llx size=%#llx head=%#llx+%#llx tail=%#llx+%#llx\n"
                    .as_ptr(),
                addr as libc::c_ulonglong,
                size as libc::c_ulonglong,
                G_VMMAP_PAD.head_lo as libc::c_ulonglong,
                G_VMMAP_PAD.head_n as libc::c_ulonglong,
                G_VMMAP_PAD.tail_lo as libc::c_ulonglong,
                G_VMMAP_PAD.tail_n as libc::c_ulonglong,
            );
        }
    }
}

pub(super) unsafe fn vmmap_pad_restore() {
    unsafe {
        if G_VMMAP_PAD.armed == 0 {
            return;
        }
        G_VMMAP_PAD.armed = 0;
        if !G_VMMAP_PAD.head.is_null() {
            for i in 0..G_VMMAP_PAD.head_n {
                ocerz_st(
                    G_VMMAP_PAD.head_lo.wrapping_add(i),
                    1,
                    *G_VMMAP_PAD.head.add(i as usize) as u64,
                );
            }
            libc::free(G_VMMAP_PAD.head.cast());
            G_VMMAP_PAD.head = ptr::null_mut();
        }
        if !G_VMMAP_PAD.tail.is_null() {
            for i in 0..G_VMMAP_PAD.tail_n {
                ocerz_st(
                    G_VMMAP_PAD.tail_lo.wrapping_add(i),
                    1,
                    *G_VMMAP_PAD.tail.add(i as usize) as u64,
                );
            }
            libc::free(G_VMMAP_PAD.tail.cast());
            G_VMMAP_PAD.tail = ptr::null_mut();
        }
    }
}

unsafe fn peer_g2h(ga: u64) -> u64 {
    unsafe {
        if G_PEER_INIT == 0 {
            let l = libc::getenv(c"OCERZ_LOWBASE".as_ptr());
            let t = libc::getenv(c"OCERZ_TOPBASE".as_ptr());
            G_PEER_LOW = if l.is_null() {
                0
            } else {
                libc::strtoull(l, ptr::null_mut(), 0)
            };
            G_PEER_TOP = if t.is_null() {
                0
            } else {
                libc::strtoull(t, ptr::null_mut(), 0)
            };
            G_PEER_INIT = 1;
        }
        if G_PEER_LOW != 0 && ga < crate::ffi::OCERZ_LOW_LIMIT {
            return ga.wrapping_add(G_PEER_LOW);
        }
        if G_PEER_TOP != 0
            && ga.wrapping_sub(crate::ffi::OCERZ_TOP_LO)
                < crate::ffi::OCERZ_TOP_HI - crate::ffi::OCERZ_TOP_LO
        {
            return ga
                .wrapping_sub(crate::ffi::OCERZ_TOP_LO)
                .wrapping_add(G_PEER_TOP);
        }
        ga
    }
}

unsafe fn xlate_task_vm_request(gmsg: u64, send_size: u32, msg_id: u32) {
    static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
        if off < 0 {
            off = c_int::from(!libc::getenv(c"OCERZ_NO_PEER_VM".as_ptr()).is_null());
            OFF.store(off, core::sync::atomic::Ordering::Relaxed);
        }
        if off != 0 {
            return;
        }
        let aoff = if msg_id == 4806 { 0x34 } else { 0x20 };
        let need = if msg_id == 4806 {
            0x40
        } else if msg_id == 4808 {
            0x38
        } else {
            0x30
        };
        if send_size != 0 && send_size < need {
            return;
        }
        let self_task = ocerz_ld(gmsg.wrapping_add(8), 4) as u32 == mach_task_self_ as u32;
        let ga = ocerz_ld(gmsg.wrapping_add(aoff), 8);
        let ha = if self_task {
            ocerz_g2h(ga) as usize as u64
        } else {
            peer_g2h(ga)
        };
        if ha != ga {
            ocerz_st(gmsg.wrapping_add(aoff), 8, ha);
        }
        if msg_id == 4808 {
            let local = ocerz_ld(gmsg.wrapping_add(0x30), 8);
            let hl = ocerz_g2h(local) as usize as u64;
            if hl != local {
                ocerz_st(gmsg.wrapping_add(0x30), 8, hl);
            }
        }
    }
}

unsafe fn xlate_gpu_nocopy_buffer(gmsg: u64, send_size: u32, saved: *mut OcerzOolSave) -> c_int {
    static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
        if off < 0 {
            off = c_int::from(!libc::getenv(c"OCERZ_NO_GPU_NOCOPY_XLATE".as_ptr()).is_null());
            OFF.store(off, core::sync::atomic::Ordering::Relaxed);
        }
        if off != 0
            || ocerz_ld(gmsg.wrapping_add(0x20), 4) as u32 != 9
            || ocerz_ld(gmsg.wrapping_add(0x24), 4) as u32 != 0
        {
            return 0;
        }
        let ib = gmsg.wrapping_add(0x2c);
        if ocerz_ld(gmsg.wrapping_add(0x28), 4) as u32 != 104
            || (send_size != 0 && 0x2c + 104 > send_size)
        {
            return 0;
        }
        let a0 = ocerz_ld(ib.wrapping_add(0x38), 8);
        let a1 = ocerz_ld(ib.wrapping_add(0x40), 8);
        let len = ocerz_ld(ib.wrapping_add(0x48), 8);
        if a0 == 0
            || a0 != a1
            || a0 >= crate::ffi::OCERZ_LOW_LIMIT
            || len == 0
            || len > crate::ffi::OCERZ_LOW_LIMIT - a0
        {
            return 0;
        }
        let ha = ocerz_g2h(a0) as usize as u64;
        if ha == a0
            || crate::ffi::ocerz_addr_readable(a0) == 0
            || crate::ffi::ocerz_addr_readable(a0.wrapping_add(len - 1)) == 0
        {
            return 0;
        }
        for k in 0..2 {
            (*saved.add(k)).off = 0x2c + 0x38 + 8 * k as u64;
            (*saved.add(k)).orig = a0;
            ocerz_st(gmsg.wrapping_add((*saved.add(k)).off), 8, ha);
        }
        if !libc::getenv(c"OCERZ_MIGTRACE".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: GPU-NOCOPY[%d] buffer %#llx+%#llx -> host %#llx\n".as_ptr(),
                libc::getpid(),
                a0 as libc::c_ulonglong,
                len as libc::c_ulonglong,
                ha as libc::c_ulonglong,
            );
        }
        2
    }
}

pub(super) unsafe fn ocerz_send_xlate_descriptors(
    gmsg: u64,
    send_size: u32,
    saved: *mut OcerzOolSave,
    max_saved: c_int,
) -> c_int {
    unsafe {
        if G_SENDXLATE_OFF < 0 {
            G_SENDXLATE_OFF = c_int::from(!libc::getenv(c"OCERZ_NO_SENDXLATE".as_ptr()).is_null());
        }
        if G_SENDXLATE_OFF != 0 || gmsg == 0 {
            return 0;
        }
        let bits = ocerz_ld(gmsg, 4) as u32;
        let msg_id = ocerz_ld(gmsg.wrapping_add(0x14), 4) as u32;
        let mut n = 0;
        if (msg_id == 2888 || msg_id == 2889) && n < max_saved {
            let msz = ocerz_ld(gmsg.wrapping_add(4), 4) as u32;
            if msz >= 0x30 && (send_size == 0 || msz <= send_size) {
                let poff = (msz - 0x10) as u64;
                let ga = ocerz_ld(gmsg.wrapping_add(poff), 8);
                if ga != 0 {
                    let ha = ocerz_g2h(ga) as usize as u64;
                    if ha != ga {
                        (*saved.add(n as usize)).off = poff;
                        (*saved.add(n as usize)).orig = ga;
                        n += 1;
                        ocerz_st(gmsg.wrapping_add(poff), 8, ha);
                    }
                }
            }
        }
        if (msg_id == 2865 || msg_id == 2866) && n + 2 <= max_saved {
            let msz = ocerz_ld(gmsg.wrapping_add(4), 4) as u32;
            if msz >= 0x50 && (send_size == 0 || msz <= send_size) {
                for poff in [(msz - 0x28) as u64, (msz - 0x10) as u64] {
                    let ga = ocerz_ld(gmsg.wrapping_add(poff), 8);
                    if ga == 0 {
                        continue;
                    }
                    let ha = ocerz_g2h(ga) as usize as u64;
                    if ha != ga {
                        (*saved.add(n as usize)).off = poff;
                        (*saved.add(n as usize)).orig = ga;
                        n += 1;
                        ocerz_st(gmsg.wrapping_add(poff), 8, ha);
                    }
                }
            }
        }
        if msg_id == 2865
            && crate::ffi::ocerz_low_base != 0
            && bits & 0x80000000 == 0
            && n + 2 <= max_saved
        {
            n += xlate_gpu_nocopy_buffer(gmsg, send_size, saved.add(n as usize));
        }
        if matches!(msg_id, 4802 | 4804 | 4806 | 4808 | 4809) {
            xlate_task_vm_request(gmsg, send_size, msg_id);
        }
        if msg_id == 4811 {
            if (send_size == 0 || send_size >= 0x4c)
                && !libc::getenv(c"OCERZ_VMMAPLOG".as_ptr()).is_null()
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: VMMAP-REQ addr=%#llx size=%#llx mask=%#llx flags=%#x\n".as_ptr(),
                    ocerz_ld(gmsg.wrapping_add(0x30), 8) as libc::c_ulonglong,
                    ocerz_ld(gmsg.wrapping_add(0x38), 8) as libc::c_ulonglong,
                    ocerz_ld(gmsg.wrapping_add(0x40), 8) as libc::c_ulonglong,
                    ocerz_ld(gmsg.wrapping_add(0x48), 4) as u32,
                );
            }
            vmmap_pad_save(gmsg, send_size);
        }
        if msg_id == 4811
            && crate::ffi::ocerz_low_base != 0
            && bits & 0x80000000 != 0
            && (send_size == 0 || send_size >= 0x4c)
            && libc::getenv(c"OCERZ_NO_VMMAP_STEER".as_ptr()).is_null()
        {
            let flags = ocerz_ld(gmsg.wrapping_add(0x48), 4) as u32;
            let size = ocerz_ld(gmsg.wrapping_add(0x38), 8);
            let mask = ocerz_ld(gmsg.wrapping_add(0x40), 8);
            if flags & 1 != 0 && size != 0 && size < 1u64 << 32 && mask < 1u64 << 30 {
                let mut align = if mask != 0 {
                    mask.wrapping_add(1)
                } else {
                    0x4000
                };
                align = align.max(0x4000);
                let rsz = size.wrapping_add(0x3fff) & !0x3fff;
                let g = crate::ffi::ocerz_map_anywhere_aligned(
                    rsz,
                    libc::PROT_READ | libc::PROT_WRITE,
                    align,
                );
                if g != 0
                    && g >= crate::ffi::OCERZ_LOW_LIMIT
                    && g < crate::ffi::OCERZ_TOP_LO
                    && ocerz_g2h(g) as usize as u64 == g
                {
                    ocerz_st(gmsg.wrapping_add(0x30), 8, g);
                    ocerz_st(gmsg.wrapping_add(0x48), 4, ((flags & !1) | 0x4000) as u64);
                    if !libc::getenv(c"OCERZ_MIGTRACE".as_ptr()).is_null() {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: VMMAP-STEER[%d] size=%#llx mask=%#llx -> guest identity %#llx\n".as_ptr(),
                            libc::getpid(),
                            size as libc::c_ulonglong,
                            mask as libc::c_ulonglong,
                            g as libc::c_ulonglong,
                        );
                    }
                }
            }
        }
        if (msg_id == 4815 || msg_id == 4816)
            && (send_size == 0 || send_size >= 0x28)
            && n < max_saved
        {
            let ga = ocerz_ld(gmsg.wrapping_add(0x20), 8);
            let ha = if crate::ffi::ocerz_low_base != 0 {
                crate::ffi::ocerz_low_base
            } else {
                ocerz_g2h(ga) as usize as u64
            };
            if ha != ga {
                (*saved.add(n as usize)).off = 0x20;
                (*saved.add(n as usize)).orig = ga;
                n += 1;
                ocerz_st(gmsg.wrapping_add(0x20), 8, ha);
            }
        }
        if bits & 0x80000000 == 0 {
            return n;
        }
        let dcnt = ocerz_ld(gmsg.wrapping_add(0x18), 4) as u32;
        if dcnt == 0 || dcnt > 4096 {
            return n;
        }
        let mut off = 0x1cu64;
        for _ in 0..dcnt {
            if send_size != 0 && off + 12 > send_size as u64 {
                break;
            }
            let ty = ocerz_ld(gmsg.wrapping_add(off + 11), 1) as u8;
            if ty == 0 {
                off += 12;
                continue;
            }
            if matches!(ty, 1 | 2 | 3) {
                if send_size != 0 && off + 16 > send_size as u64 {
                    break;
                }
                let ga = ocerz_ld(gmsg.wrapping_add(off), 8);
                if ga != 0 {
                    let ha = ocerz_g2h(ga) as usize as u64;
                    if ha != ga && n < max_saved {
                        (*saved.add(n as usize)).off = off;
                        (*saved.add(n as usize)).orig = ga;
                        n += 1;
                        ocerz_st(gmsg.wrapping_add(off), 8, ha);
                    }
                }
                off += 16;
                continue;
            }
            if ty == 4 {
                off += 16;
                continue;
            }
            break;
        }
        n
    }
}

pub(super) unsafe fn ocerz_send_restore_descriptors(
    gmsg: u64,
    saved: *const OcerzOolSave,
    n: c_int,
) {
    unsafe {
        for i in 0..n as usize {
            let ha = ocerz_g2h((*saved.add(i)).orig) as usize as u64;
            let at = gmsg.wrapping_add((*saved.add(i)).off);
            if ocerz_ld(at, 8) == ha {
                ocerz_st(at, 8, (*saved.add(i)).orig);
            }
        }
    }
}

pub(super) unsafe fn ocerz_reply_xlate_vm_region(reply_buf: u64, recv_size: u32, req_addr: u64) {
    unsafe {
        if reply_buf == 0 {
            return;
        }
        let bits = ocerz_ld(reply_buf, 4) as u32;
        let mut size = ocerz_ld(reply_buf.wrapping_add(4), 4) as u32;
        if recv_size != 0 && size > recv_size {
            size = recv_size;
        }
        let reply_id = ocerz_ld(reply_buf.wrapping_add(0x14), 4) as u32;
        let address_off = if reply_id == 4915 {
            if bits & 0x80000000 != 0
                || size < 0x2c
                || ocerz_ld(reply_buf.wrapping_add(0x20), 4) as u32
                    != OCERZ_MACH_KERN_SUCCESS as u32
            {
                return;
            }
            0x24
        } else if reply_id == 4916 {
            if bits & 0x80000000 == 0 || size < 0x38 {
                return;
            }
            0x30
        } else {
            return;
        };
        if req_addr != u64::MAX {
            let mut ga = req_addr;
            let mut gsz = 0;
            let mut prot = 0u32;
            let mut maxprot = 0u32;
            crate::ffi::ocerz_guest_vm_region(&mut ga, &mut gsz, &mut prot, &mut maxprot);
            if crate::ffi::ocerz_low_base == 0 && prot == 0 && maxprot == 0 {
                // Fall through to host-address conversion.
            } else {
                ocerz_st(reply_buf.wrapping_add(address_off), 8, ga);
                ocerz_st(reply_buf.wrapping_add(address_off + 8), 8, gsz);
                if reply_id == 4915 && size >= 0x44 {
                    ocerz_st(reply_buf.wrapping_add(0x34), 4, 0);
                    ocerz_st(reply_buf.wrapping_add(0x3c), 4, prot as u64);
                    ocerz_st(reply_buf.wrapping_add(0x40), 4, maxprot as u64);
                } else if reply_id == 4916 && size >= 0x4c {
                    ocerz_st(reply_buf.wrapping_add(0x44), 4, prot as u64);
                    ocerz_st(reply_buf.wrapping_add(0x48), 4, maxprot as u64);
                }
                return;
            }
        }
        let haddr = ocerz_ld(reply_buf.wrapping_add(address_off), 8);
        if ocerz_host_in_guest_space(haddr as usize as *const c_void) {
            ocerz_st(
                reply_buf.wrapping_add(address_off),
                8,
                ocerz_h2g(haddr as usize as *const c_void),
            );
        }
    }
}

pub(super) unsafe fn ocerz_send_xlate_vector(
    gvec: u64,
    count: u32,
    sending: c_int,
    receiving: c_int,
    saved: *mut OcerzOolSave,
    max_saved: c_int,
) -> c_int {
    unsafe {
        if G_SENDXLATE_OFF < 0 {
            G_SENDXLATE_OFF = c_int::from(!libc::getenv(c"OCERZ_NO_SENDXLATE".as_ptr()).is_null());
        }
        if G_SENDXLATE_OFF != 0 || gvec == 0 || count == 0 || count > 64 {
            return 0;
        }
        static VLOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut vlog = VLOG.load(core::sync::atomic::Ordering::Relaxed);
        if vlog < 0 {
            vlog = c_int::from(!libc::getenv(c"OCERZ_MACHMSG".as_ptr()).is_null());
            VLOG.store(vlog, core::sync::atomic::Ordering::Relaxed);
        }
        let seg0 = ocerz_ld(gvec, 8);
        let seg0_size = ocerz_ld(gvec.wrapping_add(0x10), 4) as u32;
        let mut n = 0;
        for i in 0..count {
            let e = gvec.wrapping_add(i as u64 * 24);
            if vlog != 0 && i < 4 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: VEC[%u] data=%#llx rcv_addr=%#llx ssize=%#x rsize=%#x\n".as_ptr(),
                    i,
                    ocerz_ld(e, 8) as libc::c_ulonglong,
                    ocerz_ld(e.wrapping_add(8), 8) as libc::c_ulonglong,
                    ocerz_ld(e.wrapping_add(0x10), 4) as u32,
                    ocerz_ld(e.wrapping_add(0x14), 4) as u32,
                );
            }
            for f in [0u64, 8] {
                let rcv_addr = ocerz_ld(e.wrapping_add(8), 8);
                if (f == 0 && sending == 0 && !(receiving != 0 && rcv_addr == 0))
                    || (f != 0 && receiving == 0)
                {
                    continue;
                }
                let ga = ocerz_ld(e.wrapping_add(f), 8);
                if ga == 0 {
                    continue;
                }
                let ha = ocerz_g2h(ga) as usize as u64;
                if ha != ga && n < max_saved {
                    (*saved.add(n as usize)).off = e.wrapping_add(f).wrapping_sub(gvec);
                    (*saved.add(n as usize)).orig = ga;
                    n += 1;
                    ocerz_st(e.wrapping_add(f), 8, ha);
                }
            }
        }
        if sending != 0 && seg0 != 0 && crate::ffi::ocerz_addr_committed(seg0) == 1 {
            let mut segsv = [OcerzOolSave::default(); 32];
            let segn = ocerz_send_xlate_descriptors(seg0, seg0_size, segsv.as_mut_ptr(), 32);
            for j in 0..segn {
                if n >= max_saved {
                    break;
                }
                (*saved.add(n as usize)).off =
                    seg0.wrapping_add(segsv[j as usize].off).wrapping_sub(gvec);
                (*saved.add(n as usize)).orig = segsv[j as usize].orig;
                n += 1;
            }
        }
        n
    }
}
