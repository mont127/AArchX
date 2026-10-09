//! ---- formatted output ----
//! printf's family and NSLog take variadic arguments the guest put in System V
//! order.  The format itself names the classes of argument it takes, which is
//! what ocerz_objc_format_classes extracts from it for each dialect — C,
//! CoreFoundation, a predicate format, a list of ObjC type characters, or a
//! wide string.  The named arguments cross under a fixed signature and the
//! variadic ones are gathered into a stack frame the host v-function reads as
//! its own va_list.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null;

use crate::ffi::*;
use crate::ported::objcbridge::common::*;
use crate::ported::objcbridge::send::*;

#[repr(C)]
struct ObVeneer {
    sym: *const c_char,
    vform: ObSym,
    named: *const c_char,
    sig: *const c_char,
    fmt: c_int,
    dialect: c_int,
}
unsafe impl Sync for ObVeneer {}

macro_rules! veneer {
    ($sym:literal, $lib:expr, $vname:literal, $named:literal, $sig:literal, $fmt:expr, $dial:expr) => {
        ObVeneer {
            sym: concat!($sym, "\0").as_ptr() as *const c_char,
            vform: obsym!($lib, $vname),
            named: concat!($named, "\0").as_ptr() as *const c_char,
            sig: concat!($sig, "\0").as_ptr() as *const c_char,
            fmt: $fmt as c_int,
            dialect: $dial as c_int,
        }
    };
}

static G_OB_PRINTF: ObVeneer = veneer!("_printf", OCERZ_BRIDGE_LIBSYSTEM, "vprintf", "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_C);
static G_OB_FPRINTF: ObVeneer = veneer!("_fprintf", OCERZ_BRIDGE_LIBSYSTEM, "vfprintf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_SPRINTF: ObVeneer = veneer!("_sprintf", OCERZ_BRIDGE_LIBSYSTEM, "vsprintf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_SNPRINTF: ObVeneer = veneer!("_snprintf", OCERZ_BRIDGE_LIBSYSTEM, "vsnprintf", "i(pLp)", "i(pLpp)", 2, OCERZ_OBJC_FMT_C);
static G_OB_SNPRINTF_L: ObVeneer = veneer!("_snprintf_l", OCERZ_BRIDGE_LIBSYSTEM, "vsnprintf_l", "i(pLpp)", "i(pLppp)", 3, OCERZ_OBJC_FMT_C);
static G_OB_ASPRINTF: ObVeneer = veneer!("_asprintf", OCERZ_BRIDGE_LIBSYSTEM, "vasprintf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_ASPRINTF_L: ObVeneer = veneer!("_asprintf_l", OCERZ_BRIDGE_LIBSYSTEM, "vasprintf_l", "i(ppp)", "i(pppp)", 2, OCERZ_OBJC_FMT_C);
static G_OB_DPRINTF: ObVeneer = veneer!("_dprintf", OCERZ_BRIDGE_LIBSYSTEM, "vdprintf", "i(ip)", "i(ipp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_SYSLOG: ObVeneer = veneer!("_syslog", OCERZ_BRIDGE_LIBSYSTEM, "vsyslog", "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_WARN: ObVeneer = veneer!("_warn", OCERZ_BRIDGE_LIBSYSTEM, "vwarn", "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_C);
static G_OB_WARNX: ObVeneer = veneer!("_warnx", OCERZ_BRIDGE_LIBSYSTEM, "vwarnx", "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_C);
static G_OB_WARNC: ObVeneer = veneer!("_warnc", OCERZ_BRIDGE_LIBSYSTEM, "vwarnc", "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_ERR: ObVeneer = veneer!("_err", OCERZ_BRIDGE_LIBSYSTEM, "verr", "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_ERRX: ObVeneer = veneer!("_errx", OCERZ_BRIDGE_LIBSYSTEM, "verrx", "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_ERRC: ObVeneer = veneer!("_errc", OCERZ_BRIDGE_LIBSYSTEM, "verrc", "v(iip)", "v(iipp)", 2, OCERZ_OBJC_FMT_C);
static G_OB_SWPRINTF: ObVeneer = veneer!("_swprintf", OCERZ_BRIDGE_LIBSYSTEM, "vswprintf", "i(pLp)", "i(pLpp)", 2, OCERZ_OBJC_FMT_WIDE);
static G_OB_WPRINTF: ObVeneer = veneer!("_wprintf", OCERZ_BRIDGE_LIBSYSTEM, "vwprintf", "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_WIDE);
static G_OB_FWPRINTF: ObVeneer = veneer!("_fwprintf", OCERZ_BRIDGE_LIBSYSTEM, "vfwprintf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_WIDE);
static G_OB_SPRINTF_CHK: ObVeneer = veneer!("___sprintf_chk", OCERZ_BRIDGE_LIBSYSTEM, "__vsprintf_chk", "i(piLp)", "i(piLpp)", 3, OCERZ_OBJC_FMT_C);
static G_OB_SNPRINTF_CHK: ObVeneer = veneer!("___snprintf_chk", OCERZ_BRIDGE_LIBSYSTEM, "__vsnprintf_chk", "i(pLiLp)", "i(pLiLpp)", 4, OCERZ_OBJC_FMT_C);
static G_OB_SSCANF: ObVeneer = veneer!("_sscanf", OCERZ_BRIDGE_LIBSYSTEM, "vsscanf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_SSCANF_L: ObVeneer = veneer!("_sscanf_l", OCERZ_BRIDGE_LIBSYSTEM, "vsscanf_l", "i(ppp)", "i(pppp)", 2, OCERZ_OBJC_FMT_C);
static G_OB_SCANF: ObVeneer = veneer!("_scanf", OCERZ_BRIDGE_LIBSYSTEM, "vscanf", "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_C);
static G_OB_FSCANF: ObVeneer = veneer!("_fscanf", OCERZ_BRIDGE_LIBSYSTEM, "vfscanf", "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C);
static G_OB_NSLOG: ObVeneer = veneer!("_NSLog", OCERZ_OBJC_FOUNDATION, "NSLogv", "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_CF);
static G_OB_CFSTRING_CREATE_WITH_FORMAT: ObVeneer = veneer!("_CFStringCreateWithFormat", OCERZ_BRIDGE_COREFOUNDATION, "CFStringCreateWithFormatAndArguments", "p(ppp)", "p(pppp)", 2, OCERZ_OBJC_FMT_CF);
static G_OB_CFSTRING_APPEND_FORMAT: ObVeneer = veneer!("_CFStringAppendFormat", OCERZ_BRIDGE_COREFOUNDATION, "CFStringAppendFormatAndArguments", "v(ppp)", "v(pppp)", 2, OCERZ_OBJC_FMT_CF);

unsafe fn ob_veneer_call(vm: *mut OcerzVM, cpu: *mut OcerzCPU, vn: *const ObVeneer, guest_va: c_int, sym: *const c_char) -> c_int {
    unsafe {
        let mut named: OcerzAbiSig = core::mem::MaybeUninit::uninit().assume_init();
        if ocerz_abi_parse((*vn).named, &mut named) != OCERZ_OK as c_int {
            ob_stop!("%s is declared %s, which the ABI engine refuses", sym, (*vn).named);
        }
        let vfn = ob_need(&raw const (*vn).vform);

        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, (*vn).vform.lib, sym, (*vn).sig, vfn);

        let fmt = ob_named(&named, cpu, (*vn).fmt, 'p' as c_char);
        let mut text: ObText = core::mem::MaybeUninit::uninit().assume_init();
        let mut dialect = (*vn).dialect;
        if dialect == OCERZ_OBJC_FMT_C as c_int {
            text.heap = core::ptr::null_mut();
            text.s = if fmt != 0 { fmt as *const c_char } else { c"".as_ptr() };
        } else if dialect == OCERZ_OBJC_FMT_WIDE as c_int {
            ob_text_wide(fmt as *const i32, &mut text, sym);
            dialect = OCERZ_OBJC_FMT_C as c_int;
        } else {
            ob_text(fmt as *mut c_void, &mut text, sym);
        }

        let mut slots: [u64; OCERZ_OBJC_VARIADIC_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let n;
        if guest_va != 0 {
            let address = ob_named(&named, cpu, named.nargs, 'p' as c_char);
            n = ob_gather_va_format(sym, text.s, dialect, if address != 0 { ocerz_h2g(address as *mut c_void) } else { 0 }, slots.as_mut_ptr());
        } else {
            n = ob_gather_format(sym, text.s, dialect, &named, cpu, slots.as_mut_ptr());
        }
        ob_text_free(&mut text);

        let err = ob_perform(cpu, &named, vfn, slots.as_ptr(), n, 1, sym);
        ocerz_bridge_lower(&outer);
        *libc::__error() = err;
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_scan_count(what: *const c_char, text: *const c_char) -> c_int {
    unsafe {
        let mut n = 0;
        let mut p = text as *const u8;
        while *p != 0 {
            if *p != b'%' {
                p = p.add(1);
                continue;
            }
            p = p.add(1);
            if *p == b'%' {
                p = p.add(1);
                continue;
            }
            if *p == 0 {
                ob_stop!("%s refuses the format \"%.200s\": it ends in a lone percent", what, text);
            }
            let mut suppress = 0;
            if *p == b'*' {
                suppress = 1;
                p = p.add(1);
            }
            while *p >= b'0' && *p <= b'9' {
                p = p.add(1);
            }
            if *p == b'$' {
                ob_stop!("%s refuses the format \"%.200s\": it has positional arguments", what, text);
            }
            let mut is_l = 0;
            if *p == b'h' && *p.add(1) == b'h' {
                p = p.add(2);
            } else if *p == b'l' && *p.add(1) == b'l' {
                p = p.add(2);
            } else if *p == b'h' || *p == b'l' || *p == b'j' || *p == b'z' || *p == b't' {
                p = p.add(1);
            } else if *p == b'L' {
                is_l = 1;
                p = p.add(1);
            }
            if *p == 0 {
                ob_stop!("%s refuses the format \"%.200s\": it ends inside a conversion", what, text);
            }
            let c = *p;
            if c == b'[' {
                if is_l != 0 {
                    ob_stop!("%s refuses the format \"%.200s\": it reads a long double", what, text);
                }
                p = p.add(1);
                if *p == b'^' {
                    p = p.add(1);
                }
                if *p == b']' {
                    p = p.add(1);
                }
                while *p != 0 && *p != b']' {
                    p = p.add(1);
                }
                if *p != b']' {
                    ob_stop!("%s refuses the format \"%.200s\": it ends inside a scanset", what, text);
                }
                if suppress == 0 {
                    n += 1;
                }
                p = p.add(1);
                continue;
            }
            match c {
                b'd' | b'i' | b'o' | b'u' | b'x' | b'X' | b'f' | b'e' | b'E' | b'g' | b'G' | b'a'
                | b'A' | b'c' | b's' | b'p' | b'n' => {}
                _ => ob_stop!("%s refuses the format \"%.200s\": it has an unsupported conversion", what, text),
            }
            if is_l != 0 {
                ob_stop!("%s refuses the format \"%.200s\": it reads a long double", what, text);
            }
            if suppress == 0 {
                n += 1;
            }
            p = p.add(1);
        }
        if n > OCERZ_OBJC_VARIADIC_MAX as c_int {
            ob_stop!("%s refuses the format \"%.200s\": it takes more arguments than cross", what, text);
        }
        n
    }
}

unsafe fn ob_scan_veneer_call(vm: *mut OcerzVM, cpu: *mut OcerzCPU, vn: *const ObVeneer, guest_va: c_int, sym: *const c_char) -> c_int {
    unsafe {
        let mut named: OcerzAbiSig = core::mem::MaybeUninit::uninit().assume_init();
        if ocerz_abi_parse((*vn).named, &mut named) != OCERZ_OK as c_int {
            ob_stop!("%s is declared %s, which the ABI engine refuses", sym, (*vn).named);
        }
        let vfn = ob_need(&raw const (*vn).vform);

        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, (*vn).vform.lib, sym, (*vn).sig, vfn);

        let fmt = ob_named(&named, cpu, (*vn).fmt, 'p' as c_char);
        let text: *const c_char = if fmt != 0 { fmt as *const c_char } else { c"".as_ptr() };
        let n = ob_scan_count(sym, text);

        let mut slots: [u64; OCERZ_OBJC_VARIADIC_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        if guest_va != 0 {
            let address = ob_named(&named, cpu, named.nargs, 'p' as c_char);
            if n > 0 {
                if address == 0 {
                    ob_stop!("%s: null guest va_list", sym);
                }
                let mut gp = ocerz_ld(address, 4) as u32;
                let mut overflow = ocerz_ld(address + 8, 8);
                let saved = ocerz_ld(address + 16, 8);
                if gp > 48 || (gp & 7) != 0 {
                    ob_stop!("%s: invalid guest va_list offsets (gp=%u)", sym, gp);
                }
                for k in 0..n as usize {
                    let from: u64;
                    if gp < 48 {
                        if saved == 0 {
                            ob_stop!("%s: null guest va_list register save area", sym);
                        }
                        from = saved + gp as u64;
                        gp += 8;
                    } else {
                        if overflow == 0 {
                            ob_stop!("%s: null guest va_list overflow area", sym);
                        }
                        from = overflow;
                        overflow += 8;
                    }
                    let raw = ocerz_ld(from, 8);
                    *slots.as_mut_ptr().add(k) = if raw != 0 { ocerz_g2h(raw) as u64 } else { 0 };
                }
            }
        } else {
            let mut va: OcerzAbiVaList = core::mem::MaybeUninit::uninit().assume_init();
            if ocerz_abi_va_start(&named, cpu as *mut OcerzCPU, &mut va) != OCERZ_OK as c_int {
                ob_stop!("%s: the ABI engine cannot find where the variadic arguments begin", sym);
            }
            for k in 0..n as usize {
                ocerz_abi_va_arg(&mut va, cpu, 'p' as c_char, slots.as_mut_ptr().add(k));
            }
        }

        let err = ob_perform(cpu, &named, vfn, slots.as_ptr(), n, 1, sym);
        ocerz_bridge_lower(&outer);
        *libc::__error() = err;
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_scan_veneer(vm: *mut OcerzVM, cpu: *mut OcerzCPU, vn: *const ObVeneer, sym: *const c_char) -> c_int {
    unsafe { ob_scan_veneer_call(vm, cpu, vn, 0, sym) }
}
unsafe fn ob_scan_va_veneer(vm: *mut OcerzVM, cpu: *mut OcerzCPU, vn: *const ObVeneer, sym: *const c_char) -> c_int {
    unsafe { ob_scan_veneer_call(vm, cpu, vn, 1, sym) }
}
unsafe fn ob_veneer(vm: *mut OcerzVM, cpu: *mut OcerzCPU, vn: *const ObVeneer) -> c_int {
    unsafe { ob_veneer_call(vm, cpu, vn, 0, (*vn).sym) }
}
unsafe fn ob_va_veneer(vm: *mut OcerzVM, cpu: *mut OcerzCPU, base: *const ObVeneer, sym: *const c_char) -> c_int {
    unsafe { ob_veneer_call(vm, cpu, base, 1, sym) }
}

macro_rules! fmt_fn {
    ($name:ident, $v:ident) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
            unsafe { ob_veneer(vm, cpu, &raw const $v) }
        }
    };
}
macro_rules! fmt_va_fn {
    ($name:ident, $v:ident, $sym:literal) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
            unsafe { ob_va_veneer(vm, cpu, &raw const $v, concat!($sym, "\0").as_ptr() as *const c_char) }
        }
    };
}
macro_rules! fmt_scan_fn {
    ($name:ident, $v:ident, $sym:literal, $va:expr) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
            unsafe { ob_scan_veneer_call(vm, cpu, &raw const $v, $va, concat!($sym, "\0").as_ptr() as *const c_char) }
        }
    };
}

fmt_fn!(ocerz_fmt_printf, G_OB_PRINTF);
fmt_fn!(ocerz_fmt_fprintf, G_OB_FPRINTF);
fmt_fn!(ocerz_fmt_sprintf, G_OB_SPRINTF);
fmt_fn!(ocerz_fmt_snprintf, G_OB_SNPRINTF);
fmt_fn!(ocerz_fmt_snprintf_l, G_OB_SNPRINTF_L);
fmt_fn!(ocerz_fmt_asprintf, G_OB_ASPRINTF);
fmt_fn!(ocerz_fmt_asprintf_l, G_OB_ASPRINTF_L);
fmt_fn!(ocerz_fmt_dprintf, G_OB_DPRINTF);
fmt_fn!(ocerz_fmt_syslog, G_OB_SYSLOG);
fmt_fn!(ocerz_fmt_warn, G_OB_WARN);
fmt_fn!(ocerz_fmt_warnx, G_OB_WARNX);
fmt_fn!(ocerz_fmt_warnc, G_OB_WARNC);
fmt_fn!(ocerz_fmt_err, G_OB_ERR);
fmt_fn!(ocerz_fmt_errx, G_OB_ERRX);
fmt_fn!(ocerz_fmt_errc, G_OB_ERRC);
fmt_fn!(ocerz_fmt_swprintf, G_OB_SWPRINTF);
fmt_fn!(ocerz_fmt_wprintf, G_OB_WPRINTF);
fmt_fn!(ocerz_fmt_fwprintf, G_OB_FWPRINTF);
fmt_va_fn!(ocerz_fmt_vswprintf, G_OB_SWPRINTF, "_vswprintf");
fmt_va_fn!(ocerz_fmt_vwprintf, G_OB_WPRINTF, "_vwprintf");
fmt_va_fn!(ocerz_fmt_vfwprintf, G_OB_FWPRINTF, "_vfwprintf");
fmt_fn!(ocerz_fmt_sprintf_chk, G_OB_SPRINTF_CHK);
fmt_fn!(ocerz_fmt_snprintf_chk, G_OB_SNPRINTF_CHK);
fmt_fn!(ocerz_fmt_NSLog, G_OB_NSLOG);
fmt_va_fn!(ocerz_fmt_vprintf, G_OB_PRINTF, "_vprintf");
fmt_va_fn!(ocerz_fmt_vfprintf, G_OB_FPRINTF, "_vfprintf");
fmt_va_fn!(ocerz_fmt_vsprintf, G_OB_SPRINTF, "_vsprintf");
fmt_va_fn!(ocerz_fmt_vsnprintf, G_OB_SNPRINTF, "_vsnprintf");
fmt_va_fn!(ocerz_fmt_vsnprintf_l, G_OB_SNPRINTF_L, "_vsnprintf_l");
fmt_va_fn!(ocerz_fmt_vasprintf, G_OB_ASPRINTF, "_vasprintf");
fmt_va_fn!(ocerz_fmt_vasprintf_l, G_OB_ASPRINTF_L, "_vasprintf_l");
fmt_va_fn!(ocerz_fmt_vdprintf, G_OB_DPRINTF, "_vdprintf");
fmt_va_fn!(ocerz_fmt_vsyslog, G_OB_SYSLOG, "_vsyslog");
fmt_va_fn!(ocerz_fmt_vwarn, G_OB_WARN, "_vwarn");
fmt_va_fn!(ocerz_fmt_vwarnx, G_OB_WARNX, "_vwarnx");
fmt_va_fn!(ocerz_fmt_vwarnc, G_OB_WARNC, "_vwarnc");
fmt_va_fn!(ocerz_fmt_verr, G_OB_ERR, "_verr");
fmt_va_fn!(ocerz_fmt_verrx, G_OB_ERRX, "_verrx");
fmt_va_fn!(ocerz_fmt_verrc, G_OB_ERRC, "_verrc");
fmt_va_fn!(ocerz_fmt_NSLogv, G_OB_NSLOG, "_NSLogv");
fmt_va_fn!(ocerz_fmt_vsprintf_chk, G_OB_SPRINTF_CHK, "___vsprintf_chk");
fmt_va_fn!(ocerz_fmt_vsnprintf_chk, G_OB_SNPRINTF_CHK, "___vsnprintf_chk");
fmt_scan_fn!(ocerz_fmt_sscanf, G_OB_SSCANF, "_sscanf", 0);
fmt_scan_fn!(ocerz_fmt_sscanf_l, G_OB_SSCANF_L, "_sscanf_l", 0);
fmt_scan_fn!(ocerz_fmt_scanf, G_OB_SCANF, "_scanf", 0);
fmt_scan_fn!(ocerz_fmt_fscanf, G_OB_FSCANF, "_fscanf", 0);
fmt_scan_fn!(ocerz_fmt_vsscanf, G_OB_SSCANF, "_vsscanf", 1);
fmt_scan_fn!(ocerz_fmt_vsscanf_l, G_OB_SSCANF_L, "_vsscanf_l", 1);
fmt_scan_fn!(ocerz_fmt_vscanf, G_OB_SCANF, "_vscanf", 1);
fmt_scan_fn!(ocerz_fmt_vfscanf, G_OB_FSCANF, "_vfscanf", 1);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_fmt_CFStringCreateWithFormat(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_veneer(vm, cpu, &raw const G_OB_CFSTRING_CREATE_WITH_FORMAT) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_fmt_CFStringAppendFormat(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_veneer(vm, cpu, &raw const G_OB_CFSTRING_APPEND_FORMAT) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_fmt_CFStringCreateWithFormatAndArguments(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_va_veneer(vm, cpu, &raw const G_OB_CFSTRING_CREATE_WITH_FORMAT, c"_CFStringCreateWithFormatAndArguments".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_fmt_CFStringAppendFormatAndArguments(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_va_veneer(vm, cpu, &raw const G_OB_CFSTRING_APPEND_FORMAT, c"_CFStringAppendFormatAndArguments".as_ptr()) }
}
