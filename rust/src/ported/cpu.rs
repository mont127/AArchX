//! CPU reset state, plus the register dump used by the fatal paths and -trace.
//!
//! The guest MXCSR rounding-control bits are propagated to the host arm64 FPCR,
//! which both the JIT's arm64 FP instructions and the interpreter's C-computed
//! SSE ops honour.  x86 and arm64 disagree on the directed encodings - MXCSR 01
//! rounds toward -inf while FPCR 01 rounds toward +inf - so the mapping goes
//! through the fe* constants rather than copying the bits across.  Without it,
//! divsd/sqrtsd and friends always rounded to nearest whatever mode a program
//! selected with ldmxcsr or fesetround.
//!
//! The same call sets FPCR.AH where the processor reports FEAT_AFP, as the M5
//! this was written on does.  With that bit set the arm64 floating-point
//! instructions handle NaNs the way SSE does: an invalid operation produces
//! the negative default NaN x86 calls the real indefinite, and when both
//! operands are NaNs the first one is returned, quieted, whichever of them was
//! signalling, where arm64 otherwise lets a signalling NaN win and answers
//! with a positive default.  src/jit.c
//! then emits add, subtract, multiply, divide and square root bare, with none of
//! the checks and replays that keep those results exact on a processor without
//! the bit.  ocerz_afp_enable decides once, from main, so a unit harness that
//! runs translated code on a thread nothing prepared never gets translations
//! that rely on it, and ocerz_afp answers what it decided.  Native mode leaves
//! the bit alone: the host's own code runs on guest threads there, a crossing
//! would have to clear the bit and set it again, and two FPCR writes cost about
//! fourteen nanoseconds, as much as the rest of the crossing.  OCERZ_NO_AFP=1
//! keeps the software path everywhere.
//!
//! Reset installs Darwin's flat 64-bit user selectors, which is what `mov %ss, r`
//! reads out of reset.

use core::ffi::{c_char, c_int};
use core::ptr;
use core::sync::atomic::{AtomicU8, AtomicU64, Ordering, fence};

use crate::{ffi, inline};

const CPU_FPCR_AH: u64 = 0x2;
const FE_ROUND: [c_int; 4] = [0, 0x800000, 0x400000, 0xC00000];
const COMMPAGE_LO: u64 = 0x00007fffffe00000;
const COMMPAGE_HI: u64 = 0x00007fffffe04000;
const LOW_LIMIT: u64 = 0x0000000300000000;
const NULL_LIMIT: u64 = 0x0000000000010000;
const TOP_LO: u64 = 0x00007ffffe000000;
const TOP_HI: u64 = 0x00007fffffe00000;

unsafe extern "C" {
    fn fesetround(round: c_int) -> c_int;
}

static mut G_AFP: c_int = 0;

#[inline(always)]
unsafe fn guest_ptr(gaddr: u64) -> *const u8 {
    unsafe {
        let commpage = ffi::ocerz_commpage;
        if !commpage.is_null() && (COMMPAGE_LO..COMMPAGE_HI).contains(&gaddr) {
            return commpage.add((gaddr - COMMPAGE_LO) as usize);
        }
        let low_base = ffi::ocerz_low_base;
        if low_base != 0 {
            if gaddr < LOW_LIMIT {
                let pin_map = ffi::ocerz_pin_map;
                if (gaddr < NULL_LIMIT && !pin_map.is_null())
                    || (!pin_map.is_null()
                        && ((*(pin_map.add((gaddr >> 17) as usize)) >> ((gaddr >> 14) & 7)) & 1)
                            != 0)
                {
                    return gaddr as usize as *const u8;
                }
                return gaddr.wrapping_add(low_base) as usize as *const u8;
            }
            if gaddr.wrapping_sub(TOP_LO) < TOP_HI - TOP_LO {
                return gaddr.wrapping_sub(TOP_LO).wrapping_add(ffi::ocerz_top_base) as usize
                    as *const u8;
            }
        }
        gaddr.wrapping_add(ffi::ocerz_guest_base) as usize as *const u8
    }
}

#[inline(always)]
unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    unsafe {
        let p = guest_ptr(gaddr);
        if size == 1 {
            return (&*p.cast::<AtomicU8>()).load(Ordering::Acquire) as u64;
        }
        if size == 8 && (p as usize & 7) == 0 {
            return (&*p.cast::<AtomicU64>()).load(Ordering::Acquire);
        }
        let mut value = 0u64;
        ptr::copy_nonoverlapping(p, (&mut value as *mut u64).cast::<u8>(), size as usize);
        fence(Ordering::Acquire);
        value
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_afp_enable() {
    let use_afp = unsafe {
        let mut have: c_int = 0;
        let mut len = core::mem::size_of_val(&have) as libc::size_t;
        G_AFP = (libc::getenv(c"OCERZ_NO_AFP".as_ptr()).is_null()
            && ffi::ocerz_mode != ffi::OCERZ_MODE_NATIVE as c_int
            && libc::sysctlbyname(
                c"hw.optional.arm.FEAT_AFP".as_ptr(),
                (&mut have as *mut c_int).cast(),
                &mut len,
                ptr::null_mut(),
                0,
            ) == 0
            && have == 1) as c_int;
        G_AFP != 0
    };
    crate::ocerz_log!(
        "cpu: NaN results come from %s\n",
        if use_afp {
            c"the processor (FPCR.AH)".as_ptr()
        } else {
            c"translated checks".as_ptr()
        }
    );
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_afp() -> c_int {
    unsafe { G_AFP }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apply_mxcsr_round(mxcsr: u32) {
    unsafe { apply_mxcsr_round(mxcsr) };
}

#[inline(always)]
unsafe fn apply_mxcsr_round(mxcsr: u32) {
    unsafe {
        fesetround(FE_ROUND[((mxcsr >> 13) & 3) as usize]);
        if G_AFP != 0 {
            let fpcr: u64;
            core::arch::asm!(
                "mrs {fpcr}, fpcr",
                fpcr = out(reg) fpcr,
                options(nomem, nostack, preserves_flags)
            );
            if fpcr & CPU_FPCR_AH == 0 {
                core::arch::asm!(
                    "msr fpcr, {fpcr}",
                    fpcr = in(reg) fpcr | CPU_FPCR_AH,
                    options(nomem, nostack, preserves_flags)
                );
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cpu_reset(cpu: *mut ffi::OcerzCPU) {
    unsafe {
        let vm = (*cpu).vm;
        ptr::write_bytes(cpu, 0, 1);
        (*cpu).vm = vm;
        (*cpu).rflags = inline::OCERZ_FLAG_FIXED1 | inline::OCERZ_IF;
        (*cpu).fcw = 0x037f;
        (*cpu).mxcsr = 0x1f80;
        (*cpu).cs_sel = 0x2b;
        (*cpu).seg_sel[ffi::OCERZ_SREG_CS as usize] = 0x2b;
        (*cpu).seg_sel[ffi::OCERZ_SREG_SS as usize] = 0x23;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cpu_dump(cpu: *const ffi::OcerzCPU, out: *mut ffi::FILE) {
    unsafe {
        let out = out.cast::<libc::FILE>();
        const NAMES: [&[u8]; 16] = [
            b"rax\0", b"rcx\0", b"rdx\0", b"rbx\0", b"rsp\0", b"rbp\0", b"rsi\0", b"rdi\0",
            b"r8\0", b"r9\0", b"r10\0", b"r11\0", b"r12\0", b"r13\0", b"r14\0", b"r15\0",
        ];
        for i in (0..16).step_by(2) {
            libc::fprintf(
                out,
                c"%-4s=%016llx  %-4s=%016llx\n".as_ptr(),
                NAMES[i].as_ptr().cast::<c_char>(),
                (*cpu).gpr[i] as libc::c_ulonglong,
                NAMES[i + 1].as_ptr().cast::<c_char>(),
                (*cpu).gpr[i + 1] as libc::c_ulonglong,
            );
        }

        for i in 0..16 {
            let v = (*cpu).gpr[i];
            let mut s = [0u8; 192];
            let mut n = 0;
            if v == 0 || ffi::ocerz_addr_readable(v) == 0 {
                continue;
            }
            while n < s.len() as c_int - 1 {
                let a = v.wrapping_add(n as u64);
                if a & 0xfff == 0 && ffi::ocerz_addr_readable(a) == 0 {
                    break;
                }
                let c = ocerz_ld(a, 1) as u8;
                if c == 0 {
                    break;
                }
                if !(0x20..=0x7e).contains(&c) {
                    n = -1;
                    break;
                }
                s[n as usize] = c;
                n += 1;
            }
            if n >= 8 {
                s[n as usize] = 0;
                libc::fprintf(
                    out,
                    c"%-4s->\"%s\"\n".as_ptr(),
                    NAMES[i].as_ptr().cast::<c_char>(),
                    s.as_ptr().cast::<c_char>(),
                );
            }
        }

        let mp = libc::getenv(c"OCERZ_MSGPTR".as_ptr());
        if !mp.is_null() {
            let slot = libc::strtoull(mp, ptr::null_mut(), 0) as u64;
            let p = if slot != 0 && ffi::ocerz_addr_committed(slot) == 1 {
                ocerz_ld(slot, 8)
            } else {
                0
            };
            libc::fprintf(
                out,
                c"msgptr@%#llx -> %#llx".as_ptr(),
                slot as libc::c_ulonglong,
                p as libc::c_ulonglong,
            );
            if p != 0 && ffi::ocerz_addr_committed(p) == 1 {
                libc::fputs(c" \"".as_ptr(), out);
                for k in 0..200 {
                    let a = p.wrapping_add(k);
                    if a & 0xfff == 0 && ffi::ocerz_addr_committed(a) != 1 {
                        break;
                    }
                    let c = ocerz_ld(a, 1) as u8;
                    if c == 0 {
                        break;
                    }
                    libc::fputc(
                        if (0x20..0x7f).contains(&c) {
                            c as c_int
                        } else {
                            b'.' as c_int
                        },
                        out,
                    );
                }
                libc::fputc(b'"' as c_int, out);
            }
            libc::fputc(b'\n' as c_int, out);
        }
        libc::fprintf(
            out,
            c"rip =%016llx  rflags=%08llx [%c%c%c%c%c%c%c]\n".as_ptr(),
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).rflags as libc::c_ulonglong,
            if (*cpu).rflags & inline::OCERZ_OF != 0 {
                b'O' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_SF != 0 {
                b'S' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_ZF != 0 {
                b'Z' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_AF != 0 {
                b'A' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_PF != 0 {
                b'P' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_CF != 0 {
                b'C' as c_int
            } else {
                b'-' as c_int
            },
            if (*cpu).rflags & inline::OCERZ_DF != 0 {
                b'D' as c_int
            } else {
                b'-' as c_int
            },
        );
        libc::fprintf(
            out,
            c"fs_base=%016llx gs_base=%016llx\n".as_ptr(),
            (*cpu).fs_base as libc::c_ulonglong,
            (*cpu).gs_base as libc::c_ulonglong,
        );
        for i in (0..16).step_by(2) {
            libc::fprintf(
                out,
                c"xmm%-2d=%016llx:%016llx  xmm%-2d=%016llx:%016llx\n".as_ptr(),
                i as c_int,
                (*cpu).xmm[i].hi as libc::c_ulonglong,
                (*cpu).xmm[i].lo as libc::c_ulonglong,
                (i + 1) as c_int,
                (*cpu).xmm[i + 1].hi as libc::c_ulonglong,
                (*cpu).xmm[i + 1].lo as libc::c_ulonglong,
            );
        }
    }
}
