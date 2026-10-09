//! The signature-driven crossing from x86-64 System V to Apple's arm64 ABI,
//! and the reverse crossing for native code calling a guest callback.
//!
//! Integer and floating-point arguments have independent register counters on
//! both sides, and their stack layouts differ as well. This module classifies
//! each signature at the crossing, copies structures as bytes while converting
//! pointer members, extends narrow integers consistently, and translates
//! register and stack results back to the guest.
//!
//! Register-only calls use only the two argument banks. General calls use a
//! stack-resident OcerzAbiCall, with its large result and structure-copy arrays
//! left uninitialized until needed. The callback bank binds a guest function
//! and its signature to one permanent trampoline slot, allowing native callers
//! to retain and compare callback addresses. Callback dispatch reconstructs a
//! guest call from Apple's registers and packed stack, then restores the
//! enclosing native errno after carrying the guest errno through its GS slot.

#![allow(unsafe_op_in_unsafe_fn)]

use crate::ffi;
use crate::inline::{ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};
use crate::log;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{MaybeUninit, offset_of, size_of};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

const ABI_GUEST_INT_REGS: usize = 6;
const ABI_GUEST_FP_REGS: usize = 8;
const ABI_HOST_INT_REGS: usize = 8;
const ABI_HOST_FP_REGS: usize = 8;
const ABI_HOST_HFA_MAX: usize = 4;
const ABI_STRUCT_DEPTH: c_int = 8;
const ABI_SMALL_STRUCT: usize = 16;
const ABI_DTIME_MONOTONIC: u64 = 1 << 63;
const ABI_DTIME_WALL: u64 = 1 << 62;
const ABI_SHAPE_BUCKETS: usize = 1024;
const ABI_CB_BUCKETS: usize = 16384;

const ABI_GUEST_INT_REG: [u8; ABI_GUEST_INT_REGS] = [
    ffi::OCERZ_RDI as u8,
    ffi::OCERZ_RSI as u8,
    ffi::OCERZ_RDX as u8,
    ffi::OCERZ_RCX as u8,
    ffi::OCERZ_R8 as u8,
    ffi::OCERZ_R9 as u8,
];
const ABI_RESULT_INT_REG: [u8; 2] = [ffi::OCERZ_RAX as u8, ffi::OCERZ_RDX as u8];
const ABI_FPCR_ROUND_MASK: u64 = 0x00c0_0000;
const ABI_FPCR_ROUND_NEAREST: u64 = 0;
const ABI_MXCSR_ROUND: [u64; 4] = [0, 0x0080_0000, 0x0040_0000, 0x00c0_0000];

#[repr(C)]
#[derive(Clone, Copy)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

#[repr(C)]
struct DlInfo {
    fname: *const c_char,
    fbase: *mut c_void,
    sname: *const c_char,
    saddr: *mut c_void,
}

unsafe extern "C" {
    fn _dyld_get_shared_cache_range(length: *mut usize) -> *const c_void;
    fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    #[link_name = "ldexp"]
    fn abi_libc_ldexp(value: f64, exponent: c_int) -> f64;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
    fn ocerz_abi_call_native(
        function: *const c_void,
        x: *const u64,
        v: *const u64,
        stack: *const u64,
        stack_bytes: u64,
        x8: *mut c_void,
        out_x: *mut u64,
        out_v: *mut u64,
    );
}

static mut G_ABI_TIMEBASE: MachTimebaseInfo = MachTimebaseInfo { numer: 0, denom: 0 };
static mut G_ABI_TIMEBASE_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

#[inline(always)]
fn abi_is_scalar_class(class: c_char) -> bool {
    matches!(
        class as u8,
        b'b' | b'B' | b'h' | b'H' | b'i' | b'u' | b'l' | b'L' | b'T' | b'p' | b'f' | b'd'
    )
}

#[inline(always)]
fn abi_is_arg_class(class: c_char) -> bool {
    matches!(class as u8, b'c' | b'k' | b'{') || class as u8 == b'D' || abi_is_scalar_class(class)
}

#[inline(always)]
fn abi_is_ret_class(class: c_char) -> bool {
    matches!(class as u8, b'v' | b'k' | b'c' | b'{')
        || class as u8 == b'D'
        || abi_is_scalar_class(class)
}

#[inline(always)]
fn abi_is_fp(class: c_char) -> bool {
    matches!(class as u8, b'f' | b'd' | b'D')
}

unsafe fn abi_f80_to_double(mantissa: u64, sign_exp: u16) -> f64 {
    let sign = (sign_exp >> 15) & 1;
    let exponent = sign_exp & 0x7fff;
    let value = if exponent == 0 && mantissa == 0 {
        0.0
    } else if exponent == 0x7fff {
        if mantissa.wrapping_shl(1) == 0 {
            f64::INFINITY
        } else {
            f64::NAN
        }
    } else {
        abi_libc_ldexp(
            (mantissa as f64) / 9_223_372_036_854_775_808.0,
            exponent as c_int - 16383,
        )
    };
    if sign != 0 { -value } else { value }
}

#[inline(always)]
fn abi_class_size(class: c_char) -> usize {
    match class as u8 {
        b'b' | b'B' => 1,
        b'h' | b'H' => 2,
        b'i' | b'u' | b'f' => 4,
        _ => 8,
    }
}

#[inline(always)]
fn abi_align_up(value: usize, align: usize) -> usize {
    value.wrapping_add(align - 1) & !(align - 1)
}

unsafe fn abi_reject(notation: *const c_char, class: c_char) {
    match class as u8 {
        b'}' => crate::ocerz_log!("%s closes a structure it never opened\n", notation),
        b'v' => crate::ocerz_log!(
            "%s uses v as an argument class, which is a result class only\n",
            notation
        ),
        0 => crate::ocerz_log!("%s ends where a class was expected\n", notation),
        _ => crate::ocerz_log!(
            "%s names a class '%c' that does not exist\n",
            notation,
            class as c_int
        ),
    }
}

unsafe fn abi_parse_struct(
    notation: *const c_char,
    cursor: *mut *const c_char,
    depth: c_int,
    structure: *mut ffi::OcerzAbiStruct,
    size_out: *mut usize,
    align_out: *mut usize,
) -> c_int {
    let mut p = (*cursor).add(1);
    let mut offset = 0usize;
    let mut align = 1usize;
    let mut count = 0 as c_int;

    if depth > ABI_STRUCT_DEPTH {
        crate::ocerz_log!(
            "%s nests structures more than %d deep\n",
            notation,
            ABI_STRUCT_DEPTH
        );
        return ffi::OCERZ_EUNSUP as c_int;
    }

    while *p != b'}' as c_char {
        let class = *p;
        if class as u8 == b'{' {
            let first = (*structure).nmember as usize;
            let mut nested_size = 0usize;
            let mut nested_align = 1usize;
            let rc = abi_parse_struct(
                notation,
                &mut p,
                depth.wrapping_add(1),
                structure,
                &mut nested_size,
                &mut nested_align,
            );
            if rc != ffi::OCERZ_OK as c_int {
                return rc;
            }
            offset = abi_align_up(offset, nested_align);
            let members = ptr::addr_of_mut!((*structure).offset).cast::<u8>();
            for k in first..(*structure).nmember as usize {
                let member_offset = members.add(k);
                *member_offset = (*member_offset).wrapping_add(offset as u8);
            }
            offset = offset.wrapping_add(nested_size);
            if nested_align > align {
                align = nested_align;
            }
        } else if abi_is_scalar_class(class) {
            let size = abi_class_size(class);
            let nmember = (*structure).nmember as usize;
            if nmember >= ffi::OCERZ_ABI_STRUCT_MEMBERS as usize {
                crate::ocerz_log!(
                    "%s has a structure with more than the %d members one may have\n",
                    notation,
                    ffi::OCERZ_ABI_STRUCT_MEMBERS as c_int
                );
                return ffi::OCERZ_ETOOLONG as c_int;
            }
            offset = abi_align_up(offset, size);
            *ptr::addr_of_mut!((*structure).member)
                .cast::<c_char>()
                .add(nmember) = class;
            *ptr::addr_of_mut!((*structure).offset)
                .cast::<u8>()
                .add(nmember) = offset as u8;
            (*structure).nmember = (*structure).nmember.wrapping_add(1);
            offset = offset.wrapping_add(size);
            if size > align {
                align = size;
            }
            p = p.add(1);
        } else if class as u8 == b'v' {
            crate::ocerz_log!(
                "%s puts v inside a structure, where only a result may be void\n",
                notation
            );
            return ffi::OCERZ_EUNSUP as c_int;
        } else if class as u8 == b'c' {
            crate::ocerz_log!(
                "%s puts a callback inside a structure, which this engine does not carry\n",
                notation
            );
            return ffi::OCERZ_EUNSUP as c_int;
        } else if class as u8 == b'k' {
            crate::ocerz_log!(
                "%s puts a block inside a structure, which this engine does not carry\n",
                notation
            );
            return ffi::OCERZ_EUNSUP as c_int;
        } else if class == 0 {
            crate::ocerz_log!("%s opens a structure and never closes it\n", notation);
            return ffi::OCERZ_EFORMAT as c_int;
        } else {
            crate::ocerz_log!(
                "%s has '%c' inside a structure, which is no member class\n",
                notation,
                class as c_int
            );
            return ffi::OCERZ_EFORMAT as c_int;
        }
        count = count.wrapping_add(1);
    }

    if count == 0 {
        crate::ocerz_log!("%s has a structure with no members\n", notation);
        return ffi::OCERZ_EFORMAT as c_int;
    }

    *cursor = p.add(1);
    *size_out = abi_align_up(offset, align);
    *align_out = align;
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_parse_layout(
    notation: *const c_char,
    cursor: *mut *const c_char,
    out: *mut ffi::OcerzAbiStruct,
) -> c_int {
    let mut structure = ffi::OcerzAbiStruct::default();
    let mut size = 0usize;
    let mut align = 1usize;
    let rc = abi_parse_struct(notation, cursor, 1, &mut structure, &mut size, &mut align);
    if rc != ffi::OCERZ_OK as c_int {
        return rc;
    }
    structure.size = size as u16;
    structure.align = align as u8;
    *out = structure;
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_parse_callback(
    notation: *const c_char,
    index: c_int,
    cursor: *mut *const c_char,
    out: *mut c_char,
) -> c_int {
    let mut open = *cursor;
    if *open != b'{' as c_char {
        crate::ocerz_log!(
            "%s gives argument %d class c with no signature in braces after it\n",
            notation,
            index
        );
        return ffi::OCERZ_EFORMAT as c_int;
    }
    open = open.add(1);

    let mut close = open;
    let mut depth = 0 as c_int;
    while *close != 0 {
        match *close as u8 {
            b'{' => depth = depth.wrapping_add(1),
            b'}' if depth == 0 => break,
            b'}' => depth = depth.wrapping_sub(1),
            _ => {}
        }
        close = close.add(1);
    }
    if *close != b'}' as c_char {
        crate::ocerz_log!(
            "%s opens a callback signature for argument %d and never closes it\n",
            notation,
            index
        );
        return ffi::OCERZ_EFORMAT as c_int;
    }

    let len = close.offset_from(open) as usize;
    if !libc::memchr(open.cast(), b'c' as c_int, len).is_null() {
        crate::ocerz_log!(
            "%s gives callback argument %d a signature that takes a callback of its own\n",
            notation,
            index
        );
        return ffi::OCERZ_EUNSUP as c_int;
    }
    if len >= ffi::OCERZ_ABI_CB_MAX as usize {
        crate::ocerz_log!(
            "%s gives callback argument %d a signature longer than the %d characters one may have\n",
            notation,
            index,
            ffi::OCERZ_ABI_CB_MAX as c_int - 1
        );
        return ffi::OCERZ_ETOOLONG as c_int;
    }

    let mut nested = [0 as c_char; ffi::OCERZ_ABI_CB_MAX as usize];
    ptr::copy_nonoverlapping(open, nested.as_mut_ptr(), len);
    let mut inner = ffi::OcerzAbiSig::default();
    let rc = ocerz_abi_parse(nested.as_ptr(), &mut inner);
    if rc != ffi::OCERZ_OK as c_int {
        crate::ocerz_log!(
            "%s gives callback argument %d the signature %s, which does not parse\n",
            notation,
            index,
            nested.as_ptr()
        );
        return rc;
    }

    ptr::copy_nonoverlapping(nested.as_ptr(), out, len.wrapping_add(1));
    *cursor = close.add(1);
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_parse_block(
    notation: *const c_char,
    index: c_int,
    cursor: *mut *const c_char,
    out: *mut c_char,
) -> c_int {
    let mut open = *cursor;
    let mut where_buf = [0 as c_char; 32];
    if index < 0 {
        libc::snprintf(
            where_buf.as_mut_ptr(),
            where_buf.len(),
            c"its result".as_ptr(),
        );
    } else {
        libc::snprintf(
            where_buf.as_mut_ptr(),
            where_buf.len(),
            c"argument %d".as_ptr(),
            index,
        );
    }

    if *open != b'{' as c_char {
        crate::ocerz_log!(
            "%s gives %s class k with no signature in braces after it\n",
            notation,
            where_buf.as_ptr()
        );
        return ffi::OCERZ_EFORMAT as c_int;
    }
    open = open.add(1);

    let mut close = open;
    let mut depth = 0 as c_int;
    while *close != 0 {
        match *close as u8 {
            b'{' => depth = depth.wrapping_add(1),
            b'}' if depth == 0 => break,
            b'}' => depth = depth.wrapping_sub(1),
            _ => {}
        }
        close = close.add(1);
    }
    if *close != b'}' as c_char {
        crate::ocerz_log!(
            "%s opens a block signature for %s and never closes it\n",
            notation,
            where_buf.as_ptr()
        );
        return ffi::OCERZ_EFORMAT as c_int;
    }

    let len = close.offset_from(open) as usize;
    if !libc::memchr(open.cast(), b'c' as c_int, len).is_null() {
        crate::ocerz_log!(
            "%s gives the block in %s a signature that takes a callback\n",
            notation,
            where_buf.as_ptr()
        );
        return ffi::OCERZ_EUNSUP as c_int;
    }
    if len >= ffi::OCERZ_ABI_CB_MAX as usize {
        crate::ocerz_log!(
            "%s gives the block in %s a signature longer than the %d characters one may have\n",
            notation,
            where_buf.as_ptr(),
            ffi::OCERZ_ABI_CB_MAX as c_int - 1
        );
        return ffi::OCERZ_ETOOLONG as c_int;
    }

    let mut nested = [0 as c_char; ffi::OCERZ_ABI_CB_MAX as usize];
    ptr::copy_nonoverlapping(open, nested.as_mut_ptr(), len);
    if len > 0 {
        let mut inner = ffi::OcerzAbiSig::default();
        let rc = ocerz_abi_parse(nested.as_ptr(), &mut inner);
        if rc != ffi::OCERZ_OK as c_int {
            crate::ocerz_log!(
                "%s gives the block in %s the signature %s, which does not parse\n",
                notation,
                where_buf.as_ptr(),
                nested.as_ptr()
            );
            return rc;
        }
    }

    ptr::copy_nonoverlapping(nested.as_ptr(), out, len.wrapping_add(1));
    *cursor = close.add(1);
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_parse(
    notation: *const c_char,
    out: *mut ffi::OcerzAbiSig,
) -> c_int {
    if notation.is_null() || out.is_null() {
        return ffi::OCERZ_EUNDEF as c_int;
    }

    let mut sig = ffi::OcerzAbiSig::default();
    let mut p = notation;
    let ret = *p;
    if !abi_is_ret_class(ret) {
        abi_reject(notation, ret);
        return ffi::OCERZ_EUNSUP as c_int;
    }
    sig.ret = ret;
    if ret as u8 == b'{' {
        let rc = abi_parse_layout(notation, &mut p, &mut sig.ret_struct);
        if rc != ffi::OCERZ_OK as c_int {
            return rc;
        }
    } else {
        p = p.add(1);
        if ret as u8 == b'k' {
            let rc = abi_parse_block(notation, -1, &mut p, sig.ret_cb.as_mut_ptr());
            if rc != ffi::OCERZ_OK as c_int {
                return rc;
            }
        } else if ret as u8 == b'c' {
            let rc = abi_parse_callback(notation, -1, &mut p, sig.ret_cb.as_mut_ptr());
            if rc != ffi::OCERZ_OK as c_int {
                return rc;
            }
        }
    }

    if *p != b'(' as c_char {
        crate::ocerz_log!("%s has no argument list\n", notation);
        return ffi::OCERZ_EFORMAT as c_int;
    }
    p = p.add(1);
    while *p != 0 && *p != b')' as c_char {
        let class = *p;
        if !abi_is_arg_class(class) {
            abi_reject(notation, class);
            return ffi::OCERZ_EUNSUP as c_int;
        }
        let nargs = sig.nargs as usize;
        if nargs >= ffi::OCERZ_ABI_MAX_ARGS as usize {
            crate::ocerz_log!(
                "%s has more than the %d arguments this engine carries\n",
                notation,
                ffi::OCERZ_ABI_MAX_ARGS as c_int
            );
            return ffi::OCERZ_ETOOLONG as c_int;
        }
        if class as u8 == b'{' {
            let arg_struct = ptr::addr_of_mut!(sig.arg_struct).cast::<ffi::OcerzAbiStruct>();
            let rc = abi_parse_layout(notation, &mut p, arg_struct.add(nargs));
            if rc != ffi::OCERZ_OK as c_int {
                return rc;
            }
        } else {
            p = p.add(1);
            let cb = ptr::addr_of_mut!(sig.cb)
                .cast::<c_char>()
                .add(nargs.wrapping_mul(ffi::OCERZ_ABI_CB_MAX as usize));
            if class as u8 == b'c' {
                let rc = abi_parse_callback(notation, sig.nargs, &mut p, cb);
                if rc != ffi::OCERZ_OK as c_int {
                    return rc;
                }
            } else if class as u8 == b'k' {
                let rc = abi_parse_block(notation, sig.nargs, &mut p, cb);
                if rc != ffi::OCERZ_OK as c_int {
                    return rc;
                }
            }
        }
        *ptr::addr_of_mut!(sig.arg).cast::<c_char>().add(nargs) = class;
        sig.nargs = sig.nargs.wrapping_add(1);
    }
    if *p != b')' as c_char {
        crate::ocerz_log!("%s has no closing parenthesis\n", notation);
        return ffi::OCERZ_EFORMAT as c_int;
    }
    p = p.add(1);
    if *p != 0 {
        crate::ocerz_log!("%s has %s after its argument list\n", notation, p);
        return ffi::OCERZ_EFORMAT as c_int;
    }

    *out = sig;
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_struct_valid(structure: *const ffi::OcerzAbiStruct) -> bool {
    let nmember = (*structure).nmember as usize;
    if nmember == 0 || nmember > ffi::OCERZ_ABI_STRUCT_MEMBERS as usize {
        return false;
    }
    let size = (*structure).size as usize;
    if size == 0 || size > ffi::OCERZ_ABI_STRUCT_BYTES as usize {
        return false;
    }

    let mut align = 1usize;
    let mut end = 0usize;
    let mut starts = 0u32;
    let member = ptr::addr_of!((*structure).member).cast::<c_char>();
    let offsets = ptr::addr_of!((*structure).offset).cast::<u8>();
    for k in 0..nmember {
        let class = *member.add(k);
        if !abi_is_scalar_class(class) {
            return false;
        }
        let field_size = abi_class_size(class);
        let at = *offsets.add(k) as usize;
        if at % field_size != 0 || at < end {
            return false;
        }
        end = at.wrapping_add(field_size);
        if field_size > align {
            align = field_size;
        }
        starts |= 1u32 << (at / 8);
    }

    if end > size || (*structure).align as usize != align || size % align != 0 {
        return false;
    }
    if size <= ABI_SMALL_STRUCT && starts != if size > 8 { 3 } else { 1 } {
        return false;
    }
    true
}

unsafe fn abi_hfa(structure: *const ffi::OcerzAbiStruct) -> c_char {
    let member = ptr::addr_of!((*structure).member).cast::<c_char>();
    let nmember = (*structure).nmember as usize;
    let class = *member;
    if nmember > ABI_HOST_HFA_MAX || !abi_is_fp(class) {
        return 0;
    }
    for k in 1..nmember {
        if *member.add(k) != class {
            return 0;
        }
    }
    class
}

#[inline(always)]
unsafe fn abi_host_indirect(structure: *const ffi::OcerzAbiStruct) -> bool {
    (*structure).size as usize > ABI_SMALL_STRUCT && abi_hfa(structure) == 0
}

unsafe fn abi_sysv_classify(
    structure: *const ffi::OcerzAbiStruct,
    classes: *mut c_char,
    nint: *mut c_int,
    nsse: *mut c_int,
) -> usize {
    *nint = 0;
    *nsse = 0;
    let size = (*structure).size as usize;
    if size > ABI_SMALL_STRUCT {
        return 0;
    }
    let n = size.wrapping_add(7) / 8;
    *classes = b'S' as c_char;
    *classes.add(1) = b'S' as c_char;
    let member = ptr::addr_of!((*structure).member).cast::<c_char>();
    let offsets = ptr::addr_of!((*structure).offset).cast::<u8>();
    for k in 0..(*structure).nmember as usize {
        if !abi_is_fp(*member.add(k)) {
            *classes.add((*offsets.add(k) as usize) / 8) = b'I' as c_char;
        }
    }
    for k in 0..n {
        if *classes.add(k) == b'I' as c_char {
            *nint = (*nint).wrapping_add(1);
        } else {
            *nsse = (*nsse).wrapping_add(1);
        }
    }
    n
}

#[inline(always)]
unsafe fn abi_struct_words(structure: *const ffi::OcerzAbiStruct) -> usize {
    ((*structure).size as usize).wrapping_add(7) / 8
}

#[inline(always)]
unsafe fn abi_word(buffer: *const u8, at: usize, len: usize) -> u64 {
    let mut word = 0u64;
    ptr::copy_nonoverlapping(buffer.add(at), (&mut word as *mut u64).cast::<u8>(), len);
    word
}

#[inline(always)]
unsafe fn abi_put_word(buffer: *mut u8, at: usize, word: u64, len: usize) {
    ptr::copy_nonoverlapping((&word as *const u64).cast::<u8>(), buffer.add(at), len);
}

unsafe fn abi_zero_tail(structure: *const ffi::OcerzAbiStruct, buffer: *mut u8) {
    ptr::write_bytes(
        buffer.add((*structure).size as usize),
        0,
        abi_struct_words(structure)
            .wrapping_mul(8)
            .wrapping_sub((*structure).size as usize),
    );
}

unsafe fn abi_struct_pointers(
    structure: *const ffi::OcerzAbiStruct,
    buffer: *mut u8,
    to_host: bool,
) {
    let member = ptr::addr_of!((*structure).member).cast::<c_char>();
    let offsets = ptr::addr_of!((*structure).offset).cast::<u8>();
    for k in 0..(*structure).nmember as usize {
        if *member.add(k) != b'p' as c_char {
            continue;
        }
        let at = *offsets.add(k) as usize;
        let mut word = abi_word(buffer, at, 8);
        if word != 0 {
            word = if to_host {
                ocerz_g2h(word) as u64
            } else {
                ocerz_h2g(word as usize as *const c_void)
            };
        }
        abi_put_word(buffer, at, word, 8);
    }
}

unsafe fn abi_guest_read(address: u64, buffer: *mut u8, len: usize) {
    let mut at = 0usize;
    while at.wrapping_add(8) <= len {
        abi_put_word(buffer, at, ocerz_ld(address.wrapping_add(at as u64), 8), 8);
        at = at.wrapping_add(8);
    }
    while at < len {
        *buffer.add(at) = ocerz_ld(address.wrapping_add(at as u64), 1) as u8;
        at = at.wrapping_add(1);
    }
}

unsafe fn abi_guest_write(address: u64, buffer: *const u8, len: usize) {
    let mut at = 0usize;
    while at.wrapping_add(8) <= len {
        ocerz_st(address.wrapping_add(at as u64), 8, abi_word(buffer, at, 8));
        at = at.wrapping_add(8);
    }
    while at < len {
        ocerz_st(address.wrapping_add(at as u64), 1, *buffer.add(at) as u64);
        at = at.wrapping_add(1);
    }
}

#[inline(always)]
fn abi_narrow(class: c_char, raw: u64) -> u64 {
    match class as u8 {
        b'b' => (raw as i8 as i64) as u64,
        b'B' => raw as u8 as u64,
        b'h' => (raw as i16 as i64) as u64,
        b'H' => raw as u16 as u64,
        b'i' => (raw as i32 as i64) as u64,
        b'u' | b'f' => raw as u32 as u64,
        _ => raw,
    }
}

unsafe extern "C" fn abi_timebase_init() {
    let timebase = ptr::addr_of_mut!(G_ABI_TIMEBASE);
    mach_timebase_info(timebase);
    if (*timebase).numer == 0 || (*timebase).denom == 0 {
        (*timebase).numer = 1;
        (*timebase).denom = 1;
    }
}

unsafe fn abi_dtime(time: u64, to_host: bool) -> u64 {
    if time == 0
        || (time & (ABI_DTIME_MONOTONIC | ABI_DTIME_WALL)) == (ABI_DTIME_MONOTONIC | ABI_DTIME_WALL)
    {
        return time;
    }
    let clock = time & ABI_DTIME_MONOTONIC;
    let value = time & !ABI_DTIME_MONOTONIC;
    if value >= ABI_DTIME_WALL {
        return time;
    }
    libc::pthread_once(
        ptr::addr_of_mut!(G_ABI_TIMEBASE_ONCE),
        Some(abi_timebase_init),
    );
    let timebase = ptr::addr_of!(G_ABI_TIMEBASE);
    let multiplier = if to_host {
        (*timebase).denom
    } else {
        (*timebase).numer
    };
    let divisor = if to_host {
        (*timebase).numer
    } else {
        (*timebase).denom
    };
    let scaled = (value as u128).wrapping_mul(multiplier as u128) / divisor as u128;
    (if scaled >= ABI_DTIME_WALL as u128 {
        ABI_DTIME_WALL - 1
    } else {
        scaled as u64
    }) | clock
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_dtime_to_host(time: u64) -> u64 {
    abi_dtime(time, true)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_dtime_to_guest(time: u64) -> u64 {
    abi_dtime(time, false)
}

#[inline(always)]
fn abi_host_stack_at(offset: usize, size: c_int) -> usize {
    abi_align_up(offset, size as usize)
}

unsafe fn abi_push_host_bytes(
    call: *mut ffi::OcerzAbiCall,
    offset: *mut usize,
    bytes: *const u8,
    len: usize,
    align: usize,
) -> c_int {
    let at = abi_align_up(*offset, align);
    if at.wrapping_add(len) > size_of::<[u64; ffi::OCERZ_ABI_MAX_STACK as usize]>() {
        return ffi::OCERZ_ETOOLONG as c_int;
    }
    ptr::copy_nonoverlapping(
        bytes,
        ptr::addr_of_mut!((*call).stack).cast::<u8>().add(at),
        len,
    );
    *offset = at.wrapping_add(len);
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_push_host_stack(
    call: *mut ffi::OcerzAbiCall,
    offset: *mut usize,
    value: u64,
    size: c_int,
) -> c_int {
    let mut bytes = [0u8; 8];
    ptr::copy_nonoverlapping(
        (&value as *const u64).cast::<u8>(),
        bytes.as_mut_ptr(),
        bytes.len(),
    );
    abi_push_host_bytes(call, offset, bytes.as_ptr(), size as usize, size as usize)
}

unsafe fn abi_pull_host_stack(stack: *const u8, offset: *mut usize, size: c_int) -> u64 {
    let step = size as usize;
    let at = abi_host_stack_at(*offset, size);
    let mut value = 0u64;
    ptr::copy_nonoverlapping(stack.add(at), (&mut value as *mut u64).cast::<u8>(), step);
    *offset = at.wrapping_add(step);
    value
}

unsafe fn abi_pull_host_bytes(
    stack: *const u8,
    offset: *mut usize,
    bytes: *mut u8,
    len: usize,
    align: usize,
) {
    let at = abi_align_up(*offset, align);
    ptr::copy_nonoverlapping(stack.add(at), bytes, len);
    *offset = at.wrapping_add(len);
}

unsafe fn abi_guest_struct_in(
    structure: *const ffi::OcerzAbiStruct,
    cpu: *const ffi::OcerzCPU,
    guest_int: *mut c_int,
    guest_fp: *mut c_int,
    guest_slot: *mut c_int,
    buffer: *mut u8,
) {
    let mut classes = [0 as c_char; 2];
    let mut nint = 0 as c_int;
    let mut nsse = 0 as c_int;
    let n = abi_sysv_classify(structure, classes.as_mut_ptr(), &mut nint, &mut nsse);
    if n != 0
        && (*guest_int + nint) as usize <= ABI_GUEST_INT_REGS
        && (*guest_fp + nsse) as usize <= ABI_GUEST_FP_REGS
    {
        let gpr = ptr::addr_of!((*cpu).gpr).cast::<u64>();
        let xmm = ptr::addr_of!((*cpu).xmm).cast::<ffi::Ocerz128>();
        for k in 0..n {
            let word = if *classes.as_ptr().add(k) == b'I' as c_char {
                let reg = *ABI_GUEST_INT_REG.as_ptr().add(*guest_int as usize);
                *gpr.add(reg as usize)
            } else {
                let low = ptr::addr_of!((*xmm.add(*guest_fp as usize)).lo);
                *guest_fp = (*guest_fp).wrapping_add(1);
                *low
            };
            if *classes.as_ptr().add(k) == b'I' as c_char {
                *guest_int = (*guest_int).wrapping_add(1);
            }
            abi_put_word(buffer, 8usize.wrapping_mul(k), word, 8);
        }
    } else {
        let rsp = *ptr::addr_of!((*cpu).gpr)
            .cast::<u64>()
            .add(ffi::OCERZ_RSP as usize);
        let words = abi_struct_words(structure);
        for k in 0..words {
            let slot = *guest_slot as u64;
            let address = rsp.wrapping_add(8).wrapping_add(8u64.wrapping_mul(slot));
            abi_put_word(buffer, 8usize.wrapping_mul(k), ocerz_ld(address, 8), 8);
            *guest_slot = (*guest_slot).wrapping_add(1);
        }
    }
    abi_zero_tail(structure, buffer);
}

unsafe fn abi_guest_next(
    cpu: *const ffi::OcerzCPU,
    fp: bool,
    guest_int: *mut c_int,
    guest_fp: *mut c_int,
    guest_slot: *mut c_int,
) -> u64 {
    if fp && (*guest_fp as usize) < ABI_GUEST_FP_REGS {
        let low = ptr::addr_of!(
            (*ptr::addr_of!((*cpu).xmm)
                .cast::<ffi::Ocerz128>()
                .add(*guest_fp as usize))
            .lo
        );
        let word = *low;
        *guest_fp = (*guest_fp).wrapping_add(1);
        return word;
    }
    if !fp && (*guest_int as usize) < ABI_GUEST_INT_REGS {
        let register = *ABI_GUEST_INT_REG.as_ptr().add(*guest_int as usize) as usize;
        let word = *ptr::addr_of!((*cpu).gpr).cast::<u64>().add(register);
        *guest_int = (*guest_int).wrapping_add(1);
        return word;
    }
    let rsp = *ptr::addr_of!((*cpu).gpr)
        .cast::<u64>()
        .add(ffi::OCERZ_RSP as usize);
    let address = rsp
        .wrapping_add(8)
        .wrapping_add(8u64.wrapping_mul(*guest_slot as u64));
    *guest_slot = (*guest_slot).wrapping_add(1);
    ocerz_ld(address, 8)
}

unsafe fn abi_host_struct_out(
    structure: *const ffi::OcerzAbiStruct,
    buffer: *const u8,
    call: *mut ffi::OcerzAbiCall,
    offset: *mut usize,
) -> c_int {
    let hfa = abi_hfa(structure);
    let words = abi_struct_words(structure);
    let size = (*structure).size as usize;
    let nx = ptr::addr_of_mut!((*call).nx);
    let nv = ptr::addr_of_mut!((*call).nv);
    if hfa != 0 {
        let step = abi_class_size(hfa);
        if (*nv as usize).wrapping_add((*structure).nmember as usize) <= ABI_HOST_FP_REGS {
            let values = ptr::addr_of_mut!((*call).v).cast::<u64>();
            for k in 0..(*structure).nmember as usize {
                *values.add(*nv as usize) = abi_word(buffer, k.wrapping_mul(step), step);
                *nv = (*nv).wrapping_add(1);
            }
            return ffi::OCERZ_OK as c_int;
        }
        *nv = ABI_HOST_FP_REGS as c_int;
        return abi_push_host_bytes(call, offset, buffer, size, step);
    }

    if size > ABI_SMALL_STRUCT {
        let mem_index = (*call).nmem as usize;
        let copy = ptr::addr_of_mut!((*call).mem).cast::<u64>().add(mem_index);
        (*call).nmem = (*call).nmem.wrapping_add(words as c_int);
        *copy.add(words.wrapping_sub(1)) = 0;
        ptr::copy_nonoverlapping(buffer, copy.cast::<u8>(), size);
        let pointer = copy as u64;
        if (*nx as usize) < ABI_HOST_INT_REGS {
            *ptr::addr_of_mut!((*call).x).cast::<u64>().add(*nx as usize) = pointer;
            *nx = (*nx).wrapping_add(1);
            return ffi::OCERZ_OK as c_int;
        }
        return abi_push_host_stack(call, offset, pointer, 8);
    }

    if (*nx as usize).wrapping_add(words) <= ABI_HOST_INT_REGS {
        let values = ptr::addr_of_mut!((*call).x).cast::<u64>();
        for k in 0..words {
            *values.add(*nx as usize) = abi_word(buffer, 8usize.wrapping_mul(k), 8);
            *nx = (*nx).wrapping_add(1);
        }
        return ffi::OCERZ_OK as c_int;
    }
    *nx = ABI_HOST_INT_REGS as c_int;
    abi_push_host_bytes(call, offset, buffer, words.wrapping_mul(8), 8)
}

unsafe fn abi_read_guest(
    sig: *const ffi::OcerzAbiSig,
    cpu: *const ffi::OcerzCPU,
    call: *mut ffi::OcerzAbiCall,
) -> c_int {
    if (*sig).nargs < 0 || (*sig).nargs > ffi::OCERZ_ABI_MAX_ARGS as c_int {
        return ffi::OCERZ_ETOOLONG as c_int;
    }
    if !abi_is_ret_class((*sig).ret) {
        crate::ocerz_log!(
            "abi: result has class '%c', which no signature can name\n",
            (*sig).ret as c_int
        );
        return ffi::OCERZ_EUNSUP as c_int;
    }
    if (*sig).ret == b'{' as c_char && !abi_struct_valid(ptr::addr_of!((*sig).ret_struct)) {
        crate::ocerz_log!("abi: the result structure has a layout no notation describes\n");
        return ffi::OCERZ_EUNSUP as c_int;
    }

    ptr::write_bytes(call.cast::<u8>(), 0, offset_of!(ffi::OcerzAbiCall, ret));
    let mut guest_int = 0 as c_int;
    let mut guest_fp = 0 as c_int;
    let mut guest_slot = 0 as c_int;
    let mut host_offset = 0usize;

    if (*sig).ret == b'{' as c_char {
        let structure = ptr::addr_of!((*sig).ret_struct);
        if (*structure).size as usize > ABI_SMALL_STRUCT {
            let register = *ABI_GUEST_INT_REG.as_ptr().add(guest_int as usize) as usize;
            (*call).guest_ret = *ptr::addr_of!((*cpu).gpr).cast::<u64>().add(register);
            guest_int = guest_int.wrapping_add(1);
        }
        if abi_host_indirect(structure) {
            (*call).x8 = ptr::addr_of_mut!((*call).ret).cast();
        }
    }

    for i in 0..(*sig).nargs as usize {
        let class = *ptr::addr_of!((*sig).arg).cast::<c_char>().add(i);
        let fp = abi_is_fp(class);
        if !abi_is_arg_class(class) {
            crate::ocerz_log!(
                "abi: argument %d has class '%c', which no signature can name\n",
                i as c_int,
                class as c_int
            );
            return ffi::OCERZ_EUNSUP as c_int;
        }

        if class == b'{' as c_char {
            let structure = ptr::addr_of!((*sig).arg_struct)
                .cast::<ffi::OcerzAbiStruct>()
                .add(i);
            let mut buffer = MaybeUninit::<[u8; ffi::OCERZ_ABI_STRUCT_BYTES as usize]>::uninit();
            let buffer_ptr = buffer.as_mut_ptr().cast::<u8>();
            if !abi_struct_valid(structure) {
                crate::ocerz_log!(
                    "abi: argument %d is a structure with a layout no notation describes\n",
                    i as c_int
                );
                return ffi::OCERZ_EUNSUP as c_int;
            }
            abi_guest_struct_in(
                structure,
                cpu,
                &mut guest_int,
                &mut guest_fp,
                &mut guest_slot,
                buffer_ptr,
            );
            abi_struct_pointers(structure, buffer_ptr, true);
            if abi_host_struct_out(structure, buffer_ptr, call, &mut host_offset)
                != ffi::OCERZ_OK as c_int
            {
                crate::ocerz_log!(
                    "abi: argument %d spills past the %d-byte host argument window\n",
                    i as c_int,
                    size_of::<[u64; ffi::OCERZ_ABI_MAX_STACK as usize]>() as c_int
                );
                return ffi::OCERZ_ETOOLONG as c_int;
            }
            continue;
        }

        let mut raw;
        if class == b'D' as c_char {
            guest_slot = guest_slot.wrapping_add(guest_slot & 1);
            let rsp = *ptr::addr_of!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RSP as usize);
            let address = rsp
                .wrapping_add(8)
                .wrapping_add(8u64.wrapping_mul(guest_slot as u64));
            let double = abi_f80_to_double(
                ocerz_ld(address, 8),
                ocerz_ld(address.wrapping_add(8), 2) as u16,
            );
            guest_slot = guest_slot.wrapping_add(2);
            raw = 0u64;
            ptr::copy_nonoverlapping(
                (&double as *const f64).cast::<u8>(),
                (&mut raw as *mut u64).cast::<u8>(),
                size_of::<f64>(),
            );
        } else {
            raw = abi_guest_next(cpu, fp, &mut guest_int, &mut guest_fp, &mut guest_slot);
        }

        let value = match class as u8 {
            b'p' => {
                if raw == 0 {
                    0
                } else {
                    ocerz_g2h(raw) as u64
                }
            }
            b'c' => {
                let mut function = 0u64;
                let notation = ptr::addr_of!((*sig).cb)
                    .cast::<c_char>()
                    .add(i.wrapping_mul(ffi::OCERZ_ABI_CB_MAX as usize));
                if ocerz_abi_callback_convert(raw, notation, &mut function)
                    != ffi::OCERZ_OK as c_int
                {
                    libc::fprintf(
                        log::stderr(),
                        c"ocerz: abi: argument %d is guest function %#llx, which could not be bound to a callback trampoline, so the call is refused\n".as_ptr(),
                        i as c_int,
                        raw as libc::c_ulonglong,
                    );
                    return ffi::OCERZ_EUNSUP as c_int;
                }
                if function == 0 {
                    0
                } else {
                    ocerz_g2h(function) as u64
                }
            }
            b'k' => {
                let mut native = 0u64;
                let mut owned = 0u64;
                let notation = ptr::addr_of!((*sig).cb)
                    .cast::<c_char>()
                    .add(i.wrapping_mul(ffi::OCERZ_ABI_CB_MAX as usize));
                if ffi::ocerz_block_to_native(raw, notation, &mut native, &mut owned)
                    != ffi::OCERZ_OK as c_int
                {
                    libc::fprintf(
                        log::stderr(),
                        c"ocerz: abi: argument %d is block %#llx, which could not be made a block native code can call, so the call is refused\n".as_ptr(),
                        i as c_int,
                        raw as libc::c_ulonglong,
                    );
                    return ffi::OCERZ_EUNSUP as c_int;
                }
                if owned != 0 {
                    *ptr::addr_of_mut!((*call).owned)
                        .cast::<u64>()
                        .add((*call).nowned as usize) = owned;
                    (*call).nowned = (*call).nowned.wrapping_add(1);
                }
                native
            }
            b'T' => abi_dtime(raw, true),
            _ => abi_narrow(class, raw),
        };

        let value = value;
        if fp && ((*call).nv as usize) < ABI_HOST_FP_REGS {
            *ptr::addr_of_mut!((*call).v)
                .cast::<u64>()
                .add((*call).nv as usize) = value;
            (*call).nv = (*call).nv.wrapping_add(1);
        } else if !fp && ((*call).nx as usize) < ABI_HOST_INT_REGS {
            *ptr::addr_of_mut!((*call).x)
                .cast::<u64>()
                .add((*call).nx as usize) = value;
            (*call).nx = (*call).nx.wrapping_add(1);
        } else if abi_push_host_stack(
            call,
            &mut host_offset,
            value,
            abi_class_size(class) as c_int,
        ) != ffi::OCERZ_OK as c_int
        {
            crate::ocerz_log!(
                "abi: argument %d spills past the %d-byte host argument window\n",
                i as c_int,
                size_of::<[u64; ffi::OCERZ_ABI_MAX_STACK as usize]>() as c_int
            );
            return ffi::OCERZ_ETOOLONG as c_int;
        }
    }

    (*call).nstack = host_offset.wrapping_add(7).wrapping_div(8) as c_int;
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_read_guest(
    sig: *const ffi::OcerzAbiSig,
    cpu: *const ffi::OcerzCPU,
    call: *mut ffi::OcerzAbiCall,
) -> c_int {
    if sig.is_null() || cpu.is_null() || call.is_null() {
        return ffi::OCERZ_EUNDEF as c_int;
    }
    (*call).nowned = 0;
    let rc = abi_read_guest(sig, cpu, call);
    if rc != ffi::OCERZ_OK as c_int {
        ocerz_abi_release_owned(call);
    }
    rc
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_release_owned(call: *mut ffi::OcerzAbiCall) {
    if call.is_null() {
        return;
    }
    let nowned = (*call).nowned;
    let owned = ptr::addr_of!((*call).owned).cast::<u64>();
    let mut k = nowned;
    while k > 0 {
        k = k.wrapping_sub(1);
        ffi::ocerz_block_release(*owned.add(k as usize));
    }
    (*call).nowned = 0;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_va_start(
    named: *const ffi::OcerzAbiSig,
    cpu: *const ffi::OcerzCPU,
    va: *mut ffi::OcerzAbiVaList,
) -> c_int {
    if named.is_null() || cpu.is_null() || va.is_null() {
        return ffi::OCERZ_EUNDEF as c_int;
    }
    if (*named).nargs < 0 || (*named).nargs > ffi::OCERZ_ABI_MAX_ARGS as c_int {
        return ffi::OCERZ_ETOOLONG as c_int;
    }
    ptr::write_bytes(va, 0, 1);
    if (*named).ret == b'{' as c_char {
        let structure = ptr::addr_of!((*named).ret_struct);
        if !abi_struct_valid(structure) {
            return ffi::OCERZ_EUNSUP as c_int;
        }
        if (*structure).size as usize > ABI_SMALL_STRUCT {
            (*va).gi = (*va).gi.wrapping_add(1);
        }
    }
    for i in 0..(*named).nargs as usize {
        let class = *ptr::addr_of!((*named).arg).cast::<c_char>().add(i);
        if class == b'{' as c_char {
            let structure = ptr::addr_of!((*named).arg_struct)
                .cast::<ffi::OcerzAbiStruct>()
                .add(i);
            let mut buffer = MaybeUninit::<[u8; ffi::OCERZ_ABI_STRUCT_BYTES as usize]>::uninit();
            if !abi_struct_valid(structure) {
                return ffi::OCERZ_EUNSUP as c_int;
            }
            abi_guest_struct_in(
                structure,
                cpu,
                &mut (*va).gi,
                &mut (*va).gf,
                &mut (*va).gslot,
                buffer.as_mut_ptr().cast::<u8>(),
            );
        } else if abi_is_arg_class(class) {
            abi_guest_next(
                cpu,
                abi_is_fp(class),
                &mut (*va).gi,
                &mut (*va).gf,
                &mut (*va).gslot,
            );
        } else {
            return ffi::OCERZ_EUNSUP as c_int;
        }
    }
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_va_arg(
    va: *mut ffi::OcerzAbiVaList,
    cpu: *const ffi::OcerzCPU,
    class: c_char,
    out: *mut u64,
) -> c_int {
    if va.is_null() || cpu.is_null() || out.is_null() {
        return ffi::OCERZ_EUNDEF as c_int;
    }
    *out = 0;
    if !matches!(class as u8, b'i' | b'u' | b'l' | b'L' | b'p' | b'd') {
        return ffi::OCERZ_EUNSUP as c_int;
    }
    let raw = abi_guest_next(
        cpu,
        class == b'd' as c_char,
        &mut (*va).gi,
        &mut (*va).gf,
        &mut (*va).gslot,
    );
    *out = if class == b'p' as c_char {
        if raw == 0 { 0 } else { ocerz_g2h(raw) as u64 }
    } else {
        abi_narrow(class, raw)
    };
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_guest_struct_result(
    structure: *const ffi::OcerzAbiStruct,
    cpu: *mut ffi::OcerzCPU,
    call: *const ffi::OcerzAbiCall,
) {
    let mut buffer = MaybeUninit::<[u8; ffi::OCERZ_ABI_STRUCT_BYTES as usize]>::uninit();
    let bytes = buffer.as_mut_ptr().cast::<u8>();
    let words = abi_struct_words(structure);
    ptr::write_bytes(bytes, 0, words.wrapping_mul(8));
    let hfa = abi_hfa(structure);
    if hfa != 0 {
        let step = abi_class_size(hfa);
        let rv = ptr::addr_of!((*call).rv).cast::<u64>();
        for k in 0..(*structure).nmember as usize {
            abi_put_word(bytes, k.wrapping_mul(step), *rv.add(k), step);
        }
    } else if (*structure).size as usize > ABI_SMALL_STRUCT {
        ptr::copy_nonoverlapping(
            ptr::addr_of!((*call).ret).cast::<u64>().cast::<u8>(),
            bytes,
            (*structure).size as usize,
        );
    } else {
        ptr::copy_nonoverlapping(
            ptr::addr_of!((*call).rx).cast::<u64>().cast::<u8>(),
            bytes,
            (*structure).size as usize,
        );
    }
    abi_struct_pointers(structure, bytes, false);

    let mut classes = [0 as c_char; 2];
    let mut nint = 0 as c_int;
    let mut nsse = 0 as c_int;
    let n = abi_sysv_classify(structure, classes.as_mut_ptr(), &mut nint, &mut nsse);
    if n == 0 {
        abi_guest_write((*call).guest_ret, bytes, (*structure).size as usize);
        *ptr::addr_of_mut!((*cpu).gpr)
            .cast::<u64>()
            .add(ffi::OCERZ_RAX as usize) = (*call).guest_ret;
        return;
    }

    let mut ri = 0usize;
    let mut si = 0usize;
    let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
    let xmm = ptr::addr_of_mut!((*cpu).xmm).cast::<ffi::Ocerz128>();
    for k in 0..n {
        let word = abi_word(bytes, 8usize.wrapping_mul(k), 8);
        if *classes.as_ptr().add(k) == b'I' as c_char {
            let register = *ABI_RESULT_INT_REG.as_ptr().add(ri) as usize;
            *gpr.add(register) = word;
            ri = ri.wrapping_add(1);
        } else {
            (*xmm.add(si)).lo = word;
            (*xmm.add(si)).hi = 0;
            si = si.wrapping_add(1);
        }
    }
}

#[inline(always)]
unsafe fn abi_write_scalar_result(ret: c_char, cpu: *mut ffi::OcerzCPU, rx0: u64, rv0: u64) {
    match ret as u8 {
        b'v' => {
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = 0
        }
        b'p' => {
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = if rx0 == 0 {
                0
            } else {
                ocerz_h2g(rx0 as usize as *const c_void)
            }
        }
        b'T' => {
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = abi_dtime(rx0, false)
        }
        b'f' => {
            (*ptr::addr_of_mut!((*cpu).xmm).cast::<ffi::Ocerz128>()).lo = rv0 as u32 as u64;
            (*ptr::addr_of_mut!((*cpu).xmm).cast::<ffi::Ocerz128>()).hi = 0;
        }
        b'd' => {
            (*ptr::addr_of_mut!((*cpu).xmm).cast::<ffi::Ocerz128>()).lo = rv0;
            (*ptr::addr_of_mut!((*cpu).xmm).cast::<ffi::Ocerz128>()).hi = 0;
        }
        _ => {
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = abi_narrow(ret, rx0)
        }
    }
    let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
    let rsp = *gpr.add(ffi::OCERZ_RSP as usize);
    (*cpu).rip = ocerz_ld(rsp, 8);
    *gpr.add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
}

unsafe fn abi_function_to_guest(native: u64, notation: *const c_char) -> u64 {
    if native == 0 {
        return 0;
    }
    let mut guest = 0u64;
    if !ocerz_abi_callback_sig(native as usize as *const c_void, &mut guest).is_null() && guest != 0
    {
        return guest;
    }
    let as_guest = ocerz_h2g(native as usize as *const c_void);
    if ocerz_abi_is_guest_code(as_guest) != 0 {
        return as_guest;
    }
    let thunk = ffi::ocerz_bridge_native_thunk(
        native as usize as *const c_void,
        c"(returned function)".as_ptr(),
        notation,
    );
    if thunk == 0 {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: native code returned function %#llx (%s), and no thunk could be made for the guest to call it, so the guest is handed null\n".as_ptr(),
            native as libc::c_ulonglong,
            notation,
        );
    }
    thunk
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_write_result(
    sig: *const ffi::OcerzAbiSig,
    cpu: *mut ffi::OcerzCPU,
    call: *const ffi::OcerzAbiCall,
) {
    if sig.is_null() || cpu.is_null() || call.is_null() {
        return;
    }
    let rx0 = *ptr::addr_of!((*call).rx).cast::<u64>();
    let rv0 = *ptr::addr_of!((*call).rv).cast::<u64>();
    match (*sig).ret as u8 {
        b'D' => {
            let mut value = 0f64;
            ptr::copy_nonoverlapping(
                ptr::addr_of!((*call).rv).cast::<u64>().cast::<u8>(),
                (&mut value as *mut f64).cast::<u8>(),
                size_of::<f64>(),
            );
            ffi::ocerz_x87_push(cpu, value);
            let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
            let rsp = *gpr.add(ffi::OCERZ_RSP as usize);
            (*cpu).rip = ocerz_ld(rsp, 8);
            *gpr.add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
            return;
        }
        b'c' => {
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = abi_function_to_guest(rx0, (*sig).ret_cb.as_ptr());
            let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
            let rsp = *gpr.add(ffi::OCERZ_RSP as usize);
            (*cpu).rip = ocerz_ld(rsp, 8);
            *gpr.add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
            return;
        }
        b'k' => {
            let mut guest = 0u64;
            if ffi::ocerz_block_result_to_guest(
                rx0,
                (*sig).ret_cb.as_ptr(),
                (*call).borrowed,
                &mut guest,
            ) != ffi::OCERZ_OK as c_int
            {
                libc::fprintf(
                    log::stderr(),
                    c"ocerz: abi: native code returned block %#llx, which could not be made a block guest code can call, so the guest is handed null\n".as_ptr(),
                    rx0 as libc::c_ulonglong,
                );
            }
            *ptr::addr_of_mut!((*cpu).gpr)
                .cast::<u64>()
                .add(ffi::OCERZ_RAX as usize) = guest;
            let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
            let rsp = *gpr.add(ffi::OCERZ_RSP as usize);
            (*cpu).rip = ocerz_ld(rsp, 8);
            *gpr.add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
            return;
        }
        b'{' => {
            abi_guest_struct_result(ptr::addr_of!((*sig).ret_struct), cpu, call);
            let gpr = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
            let rsp = *gpr.add(ffi::OCERZ_RSP as usize);
            (*cpu).rip = ocerz_ld(rsp, 8);
            *gpr.add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
            return;
        }
        _ => {}
    }
    abi_write_scalar_result((*sig).ret, cpu, rx0, rv0);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_register_only(sig: *const ffi::OcerzAbiSig) -> c_int {
    if sig.is_null() || (*sig).nargs < 0 || (*sig).nargs > ffi::OCERZ_ABI_MAX_ARGS as c_int {
        return 0;
    }
    if (*sig).ret != b'v' as c_char && !abi_is_scalar_class((*sig).ret) {
        return 0;
    }
    let mut ni = 0usize;
    let mut nf = 0usize;
    let args = ptr::addr_of!((*sig).arg).cast::<c_char>();
    for i in 0..(*sig).nargs as usize {
        let class = *args.add(i);
        if !abi_is_scalar_class(class) {
            return 0;
        }
        if abi_is_fp(class) {
            nf = nf.wrapping_add(1);
            if nf > ABI_GUEST_FP_REGS {
                return 0;
            }
        } else {
            ni = ni.wrapping_add(1);
            if ni > ABI_GUEST_INT_REGS {
                return 0;
            }
        }
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_xmm_contract(
    sig: *const ffi::OcerzAbiSig,
    in_: *mut u16,
    out: *mut u16,
) {
    let mut read = 0u16;
    let mut written = 0u16;
    let mut nf = 0usize;
    if !sig.is_null() {
        let args = ptr::addr_of!((*sig).arg).cast::<c_char>();
        let mut i = 0usize;
        while (i as c_int) < (*sig).nargs && i < ffi::OCERZ_ABI_MAX_ARGS as usize {
            let class = *args.add(i);
            if class == b'{' as c_char {
                read = (1u16 << ABI_GUEST_FP_REGS) - 1;
                nf = ABI_GUEST_FP_REGS;
            } else if abi_is_fp(class) && nf < ABI_GUEST_FP_REGS {
                read |= 1u16 << nf;
                nf = nf.wrapping_add(1);
            }
            i = i.wrapping_add(1);
        }
        if (*sig).ret == b'f' as c_char || (*sig).ret == b'd' as c_char {
            written = 1;
        } else if (*sig).ret == b'{' as c_char {
            written = 3;
        }
    }
    *in_ = read;
    *out = written;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_perform_registers(
    sig: *const ffi::OcerzAbiSig,
    function: *const c_void,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    let mut x = [0u64; ABI_HOST_INT_REGS];
    let mut v = [0u64; ABI_HOST_FP_REGS];
    let mut rx = MaybeUninit::<[u64; 2]>::uninit();
    let mut rv = MaybeUninit::<[u64; 4]>::uninit();
    let mut guest_int = 0usize;
    let mut guest_fp = 0usize;
    let mut nx = 0usize;
    let mut nv = 0usize;
    let x_ptr = x.as_mut_ptr();
    let v_ptr = v.as_mut_ptr();
    let args = ptr::addr_of!((*sig).arg).cast::<c_char>();
    let gpr = ptr::addr_of!((*cpu).gpr).cast::<u64>();
    let xmm = ptr::addr_of!((*cpu).xmm).cast::<ffi::Ocerz128>();

    for i in 0..(*sig).nargs as usize {
        let class = *args.add(i);
        if abi_is_fp(class) {
            *v_ptr.add(nv) = abi_narrow(class, (*xmm.add(guest_fp)).lo);
            nv = nv.wrapping_add(1);
            guest_fp = guest_fp.wrapping_add(1);
        } else {
            let register = *ABI_GUEST_INT_REG.as_ptr().add(guest_int) as usize;
            let raw = *gpr.add(register);
            *x_ptr.add(nx) = match class as u8 {
                b'p' => {
                    if raw == 0 {
                        0
                    } else {
                        ocerz_g2h(raw) as u64
                    }
                }
                b'T' => abi_dtime(raw, true),
                _ => abi_narrow(class, raw),
            };
            nx = nx.wrapping_add(1);
            guest_int = guest_int.wrapping_add(1);
        }
    }

    let fpcr = ocerz_abi_round_swap(ABI_FPCR_ROUND_NEAREST);
    ocerz_abi_call_native(
        function,
        x.as_ptr(),
        v.as_ptr(),
        ptr::null(),
        0,
        ptr::null_mut(),
        rx.as_mut_ptr().cast::<u64>(),
        rv.as_mut_ptr().cast::<u64>(),
    );
    ocerz_abi_round_swap(fpcr & ABI_FPCR_ROUND_MASK);
    abi_write_scalar_result(
        (*sig).ret,
        cpu,
        *rx.as_ptr().cast::<u64>(),
        *rv.as_ptr().cast::<u64>(),
    );
    ffi::OCERZ_STEP_OK as c_int
}

#[inline(never)]
unsafe fn abi_perform_general(
    sig: *const ffi::OcerzAbiSig,
    function: *const c_void,
    cpu: *mut ffi::OcerzCPU,
    borrowed: c_int,
) -> c_int {
    let mut call = MaybeUninit::<ffi::OcerzAbiCall>::uninit();
    let call_ptr = call.as_mut_ptr();
    if ocerz_abi_read_guest(sig, cpu, call_ptr) != ffi::OCERZ_OK as c_int {
        return ffi::OCERZ_STEP_FATAL as c_int;
    }
    (*call_ptr).borrowed = borrowed;

    let fpcr = ocerz_abi_round_swap(ABI_FPCR_ROUND_NEAREST);
    ocerz_abi_call_native(
        function,
        ptr::addr_of!((*call_ptr).x).cast::<u64>(),
        ptr::addr_of!((*call_ptr).v).cast::<u64>(),
        ptr::addr_of!((*call_ptr).stack).cast::<u64>(),
        ((*call_ptr).nstack as u64).wrapping_mul(8),
        (*call_ptr).x8,
        ptr::addr_of_mut!((*call_ptr).rx).cast::<u64>(),
        ptr::addr_of_mut!((*call_ptr).rv).cast::<u64>(),
    );
    ocerz_abi_round_swap(fpcr & ABI_FPCR_ROUND_MASK);
    ocerz_abi_write_result(sig, cpu, call_ptr);
    ocerz_abi_release_owned(call_ptr);
    ffi::OCERZ_STEP_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_perform(
    sig: *const ffi::OcerzAbiSig,
    function: *const c_void,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    if sig.is_null() || function.is_null() || cpu.is_null() {
        crate::ocerz_fatal!("abi: a crossing with no signature, no address or no cpu\n");
        return ffi::OCERZ_STEP_FATAL as c_int;
    }
    if ocerz_abi_register_only(sig) != 0 {
        return ocerz_abi_perform_registers(sig, function, cpu);
    }
    abi_perform_general(sig, function, cpu, 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_perform_borrowed(
    sig: *const ffi::OcerzAbiSig,
    function: *const c_void,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    if sig.is_null() || function.is_null() || cpu.is_null() {
        crate::ocerz_fatal!("abi: a crossing with no signature, no address or no cpu\n");
        return ffi::OCERZ_STEP_FATAL as c_int;
    }
    if ocerz_abi_register_only(sig) != 0 {
        return ocerz_abi_perform_registers(sig, function, cpu);
    }
    abi_perform_general(sig, function, cpu, 1)
}

#[inline(always)]
unsafe fn ocerz_abi_round_swap(mode: u64) -> u64 {
    let fpcr: u64;
    core::arch::asm!(
        "mrs {fpcr}, fpcr",
        fpcr = out(reg) fpcr,
        options(nomem, nostack, preserves_flags)
    );
    if fpcr & ABI_FPCR_ROUND_MASK != mode {
        let next = (fpcr & !ABI_FPCR_ROUND_MASK) | mode;
        core::arch::asm!(
            "msr fpcr, {next}",
            next = in(reg) next,
            options(nomem, nostack, preserves_flags)
        );
    }
    fpcr
}

#[inline(always)]
unsafe fn ocerz_abi_round_of_mxcsr(mxcsr: u32) -> u64 {
    *ABI_MXCSR_ROUND.as_ptr().add(((mxcsr >> 13) & 3) as usize)
}

#[repr(C)]
struct AbiShape {
    next: *mut AbiShape,
    sig: ffi::OcerzAbiSig,
    notation: [c_char; 0],
}

#[repr(C)]
struct AbiCallback {
    guest_fn: u64,
    shape: *const AbiShape,
    next: u32,
    used: AtomicI32,
}

static mut G_ABI_CB: [AbiCallback; ffi::OCERZ_ABI_CALLBACK_SLOTS as usize] = [const {
    AbiCallback {
        guest_fn: 0,
        shape: ptr::null(),
        next: 0,
        used: AtomicI32::new(0),
    }
};
    ffi::OCERZ_ABI_CALLBACK_SLOTS as usize];
static mut G_ABI_CB_N: u32 = 0;
static mut G_ABI_SHAPES: [*mut AbiShape; ABI_SHAPE_BUCKETS] = [ptr::null_mut(); ABI_SHAPE_BUCKETS];
static mut G_ABI_CB_BUCKET: [u32; ABI_CB_BUCKETS] = [0; ABI_CB_BUCKETS];
static mut G_ABI_CB_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn abi_shape_notation(shape: *const AbiShape) -> *const c_char {
    ptr::addr_of!((*shape).notation).cast::<c_char>()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_postfork_child() {
    ptr::write(
        ptr::addr_of_mut!(G_ABI_CB_LOCK),
        libc::PTHREAD_MUTEX_INITIALIZER,
    );
}

unsafe fn abi_callback_capacity() -> u32 {
    let lo = ptr::addr_of!(ffi::ocerz_abi_callback_bank) as usize;
    let hi = ptr::addr_of!(ffi::ocerz_abi_callback_bank_end) as usize;
    let length = hi.wrapping_sub(lo);
    let count = length / ffi::OCERZ_ABI_CALLBACK_STRIDE as usize;
    count.min(ffi::OCERZ_ABI_CALLBACK_SLOTS as usize) as u32
}

unsafe fn abi_callback_address(slot: u32) -> *mut c_void {
    let bank = ptr::addr_of!(ffi::ocerz_abi_callback_bank).cast::<u8>();
    bank.add((slot as usize).wrapping_mul(ffi::OCERZ_ABI_CALLBACK_STRIDE as usize)) as *mut c_void
}

unsafe fn abi_notation_hash(mut notation: *const c_char) -> u32 {
    let mut hash = 2166136261u32;
    while *notation != 0 {
        hash = (hash ^ *notation as u8 as u32).wrapping_mul(16777619);
        notation = notation.add(1);
    }
    hash
}

unsafe fn abi_callback_hash(guest_fn: u64, shape: *const AbiShape) -> u32 {
    let key = guest_fn.wrapping_mul(0x9e37_79b9_7f4a_7c15)
        ^ ((shape as usize as u64 >> 4).wrapping_mul(0xc2b2_ae3d_27d4_eb4f));
    (key ^ (key >> 31)) as u32 & (ABI_CB_BUCKETS as u32 - 1)
}

unsafe fn abi_shape_locked(guest_fn: u64, notation: *const c_char, len: usize) -> *const AbiShape {
    let bucket = (abi_notation_hash(notation) as usize) & (ABI_SHAPE_BUCKETS - 1);
    let buckets = ptr::addr_of_mut!(G_ABI_SHAPES).cast::<*mut AbiShape>();
    let mut shape = *buckets.add(bucket) as *const AbiShape;
    while !shape.is_null() {
        if libc::strcmp(abi_shape_notation(shape), notation) == 0 {
            return shape;
        }
        shape = (*shape).next;
    }

    let allocation = size_of::<AbiShape>().wrapping_add(len).wrapping_add(1);
    let made = libc::calloc(1, allocation).cast::<AbiShape>();
    if made.is_null() {
        crate::ocerz_log!(
            "abi: no memory to parse the signature %s of callback %#llx\n",
            notation,
            guest_fn as libc::c_ulonglong
        );
        return ptr::null();
    }
    let made_notation = abi_shape_notation(made);
    ptr::copy_nonoverlapping(notation, made_notation.cast_mut(), len.wrapping_add(1));
    if ocerz_abi_parse(made_notation, ptr::addr_of_mut!((*made).sig)) != ffi::OCERZ_OK as c_int {
        crate::ocerz_log!(
            "abi: callback %#llx is declared %s, which does not parse\n",
            guest_fn as libc::c_ulonglong,
            notation
        );
        libc::free(made.cast());
        return ptr::null();
    }
    for i in 0..(*made).sig.nargs as usize {
        if *ptr::addr_of!((*made).sig.arg).cast::<c_char>().add(i) == b'c' as c_char {
            crate::ocerz_log!(
                "abi: callback %#llx is declared %s, which takes a callback of its own\n",
                guest_fn as libc::c_ulonglong,
                notation
            );
            libc::free(made.cast());
            return ptr::null();
        }
    }
    (*made).next = *buckets.add(bucket);
    *buckets.add(bucket) = made;
    made
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_callback_intern(
    guest_fn: u64,
    notation: *const c_char,
) -> *mut c_void {
    if guest_fn == 0 || notation.is_null() {
        crate::ocerz_log!(
            "abi: a callback with no guest function or no signature cannot be interned\n"
        );
        return ptr::null_mut();
    }
    let len = libc::strnlen(notation, ffi::OCERZ_ABI_CALLBACK_NOTATION_MAX as usize);
    if len >= ffi::OCERZ_ABI_CALLBACK_NOTATION_MAX as usize {
        crate::ocerz_log!(
            "abi: callback %#llx has a signature longer than the %d characters one may have\n",
            guest_fn as libc::c_ulonglong,
            ffi::OCERZ_ABI_CALLBACK_NOTATION_MAX as c_int - 1
        );
        return ptr::null_mut();
    }

    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_ABI_CB_LOCK));
    let shape = abi_shape_locked(guest_fn, notation, len);
    if shape.is_null() {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ABI_CB_LOCK));
        return ptr::null_mut();
    }

    let bucket = abi_callback_hash(guest_fn, shape) as usize;
    let buckets = ptr::addr_of_mut!(G_ABI_CB_BUCKET).cast::<u32>();
    let entries = ptr::addr_of_mut!(G_ABI_CB).cast::<AbiCallback>();
    let mut k = *buckets.add(bucket);
    while k != 0 {
        let entry = entries.add(k.wrapping_sub(1) as usize);
        if (*entry).guest_fn == guest_fn && (*entry).shape == shape {
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ABI_CB_LOCK));
            return abi_callback_address(k.wrapping_sub(1));
        }
        k = (*entry).next;
    }

    let capacity = abi_callback_capacity();
    let count = *ptr::addr_of!(G_ABI_CB_N);
    if count >= capacity {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ABI_CB_LOCK));
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: the callback bank is exhausted: all %u slots are bound, so guest function %#llx declared %s gets none\n".as_ptr(),
            capacity,
            guest_fn as libc::c_ulonglong,
            notation,
        );
        return ptr::null_mut();
    }

    let slot = count;
    let entry = entries.add(slot as usize);
    (*entry).guest_fn = guest_fn;
    (*entry).shape = shape;
    (*entry).next = *buckets.add(bucket);
    (*entry).used.store(1, Ordering::SeqCst);
    *buckets.add(bucket) = slot.wrapping_add(1);
    *ptr::addr_of_mut!(G_ABI_CB_N) = slot.wrapping_add(1);
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_ABI_CB_LOCK));
    abi_callback_address(slot)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_callback_sig(
    slot_address: *const c_void,
    guest_fn: *mut u64,
) -> *const ffi::OcerzAbiSig {
    let address = slot_address as usize;
    let lo = ptr::addr_of!(ffi::ocerz_abi_callback_bank) as usize;
    if !guest_fn.is_null() {
        *guest_fn = 0;
    }
    if address < lo || address.wrapping_sub(lo) % ffi::OCERZ_ABI_CALLBACK_STRIDE as usize != 0 {
        return ptr::null();
    }
    let slot = address.wrapping_sub(lo) / ffi::OCERZ_ABI_CALLBACK_STRIDE as usize;
    if slot >= abi_callback_capacity() as usize {
        return ptr::null();
    }
    let entry = ptr::addr_of!(G_ABI_CB).cast::<AbiCallback>().add(slot);
    if (*entry).used.load(Ordering::SeqCst) == 0 {
        return ptr::null();
    }
    if !guest_fn.is_null() {
        *guest_fn = (*entry).guest_fn;
    }
    ptr::addr_of!((*(*entry).shape).sig)
}

unsafe fn abi_host_in_guest_reservation(host: *const c_void) -> bool {
    let address = host as usize as u64;
    if ffi::ocerz_low_base != 0 {
        if address.wrapping_sub(ffi::ocerz_low_base) < ffi::OCERZ_LOW_LIMIT {
            return true;
        }
        if address.wrapping_sub(ffi::ocerz_top_base) < ffi::OCERZ_TOP_HI - ffi::OCERZ_TOP_LO {
            return true;
        }
    }
    let guest = address.wrapping_sub(ffi::ocerz_guest_base);
    guest >= ffi::ocerz_arena_lo && guest < ffi::ocerz_arena_hi
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_is_guest_code(gptr: u64) -> c_int {
    if gptr < 0x10000 {
        return 0;
    }
    let host = ocerz_g2h(gptr);
    if abi_host_in_guest_reservation(host) {
        return 1;
    }
    let mut cache_len = 0usize;
    let cache = _dyld_get_shared_cache_range(&mut cache_len);
    if !cache.is_null() && (host as usize).wrapping_sub(cache as usize) < cache_len {
        return 0;
    }
    let mut info = MaybeUninit::<DlInfo>::uninit();
    if dladdr(host, info.as_mut_ptr()) != 0 {
        0
    } else {
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_callback_convert(
    gptr: u64,
    notation: *const c_char,
    out: *mut u64,
) -> c_int {
    if out.is_null() {
        return ffi::OCERZ_EUNDEF as c_int;
    }
    *out = 0;
    if gptr == 0 {
        return ffi::OCERZ_OK as c_int;
    }
    if ocerz_abi_is_guest_code(gptr) == 0 {
        *out = gptr;
        return ffi::OCERZ_OK as c_int;
    }
    let trampoline = ocerz_abi_callback_intern(gptr, notation);
    if trampoline.is_null() {
        return ffi::OCERZ_EUNSUP as c_int;
    }
    *out = ocerz_h2g(trampoline);
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_release_callback_owned(owned: *mut u64, nowned: *mut c_int) {
    while *nowned > 0 {
        *nowned = (*nowned).wrapping_sub(1);
        ffi::ocerz_block_release(*owned.add(*nowned as usize));
    }
}

unsafe fn abi_host_struct_in(
    structure: *const ffi::OcerzAbiStruct,
    x: *const u64,
    v: *const u64,
    stack: *const u8,
    nx: *mut c_int,
    nv: *mut c_int,
    offset: *mut usize,
    buffer: *mut u8,
) -> c_int {
    let hfa = abi_hfa(structure);
    let words = abi_struct_words(structure);
    ptr::write_bytes(buffer, 0, words.wrapping_mul(8));
    if hfa != 0 {
        let step = abi_class_size(hfa);
        if (*nv as usize).wrapping_add((*structure).nmember as usize) <= ABI_HOST_FP_REGS {
            for k in 0..(*structure).nmember as usize {
                abi_put_word(buffer, k.wrapping_mul(step), *v.add(*nv as usize), step);
                *nv = (*nv).wrapping_add(1);
            }
            return ffi::OCERZ_OK as c_int;
        }
        *nv = ABI_HOST_FP_REGS as c_int;
        if stack.is_null() {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        abi_pull_host_bytes(stack, offset, buffer, (*structure).size as usize, step);
        return ffi::OCERZ_OK as c_int;
    }

    if (*structure).size as usize > ABI_SMALL_STRUCT {
        let pointer = if (*nx as usize) < ABI_HOST_INT_REGS {
            let value = *x.add(*nx as usize);
            *nx = (*nx).wrapping_add(1);
            value
        } else if !stack.is_null() {
            abi_pull_host_stack(stack, offset, 8)
        } else {
            return ffi::OCERZ_EUNDEF as c_int;
        };
        if pointer == 0 {
            return ffi::OCERZ_EFORMAT as c_int;
        }
        ptr::copy_nonoverlapping(
            pointer as usize as *const u8,
            buffer,
            (*structure).size as usize,
        );
        return ffi::OCERZ_OK as c_int;
    }

    if (*nx as usize).wrapping_add(words) <= ABI_HOST_INT_REGS {
        for k in 0..words {
            abi_put_word(buffer, 8usize.wrapping_mul(k), *x.add(*nx as usize), 8);
            *nx = (*nx).wrapping_add(1);
        }
    } else {
        *nx = ABI_HOST_INT_REGS as c_int;
        if stack.is_null() {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        abi_pull_host_bytes(stack, offset, buffer, words.wrapping_mul(8), 8);
    }
    abi_zero_tail(structure, buffer);
    ffi::OCERZ_OK as c_int
}

unsafe fn abi_guest_struct_out(
    structure: *const ffi::OcerzAbiStruct,
    buffer: *const u8,
    call: *mut ffi::OcerzGuestCall,
    guest_int: *mut c_int,
    guest_fp: *mut c_int,
) -> bool {
    let slots = size_of::<[u64; 16]>() / size_of::<u64>();
    let mut classes = [0 as c_char; 2];
    let mut nint = 0 as c_int;
    let mut nsse = 0 as c_int;
    let n = abi_sysv_classify(structure, classes.as_mut_ptr(), &mut nint, &mut nsse);
    if n != 0
        && (*guest_int + nint) as usize <= ABI_GUEST_INT_REGS
        && (*guest_fp + nsse) as usize <= ABI_GUEST_FP_REGS
    {
        let gpr = ptr::addr_of_mut!((*call).gpr).cast::<u64>();
        let xmm = ptr::addr_of_mut!((*call).xmm).cast::<u64>();
        for k in 0..n {
            let word = abi_word(buffer, 8usize.wrapping_mul(k), 8);
            if *classes.as_ptr().add(k) == b'I' as c_char {
                *gpr.add(*guest_int as usize) = word;
                *guest_int = (*guest_int).wrapping_add(1);
            } else {
                *xmm.add(*guest_fp as usize) = word;
                *guest_fp = (*guest_fp).wrapping_add(1);
            }
        }
        return true;
    }

    let words = abi_struct_words(structure);
    if ((*call).nstack as usize).wrapping_add(words) > slots {
        return false;
    }
    let stack = ptr::addr_of_mut!((*call).stack).cast::<u64>();
    for k in 0..words {
        *stack.add((*call).nstack as usize) = abi_word(buffer, 8usize.wrapping_mul(k), 8);
        (*call).nstack = (*call).nstack.wrapping_add(1);
    }
    true
}

unsafe fn abi_host_struct_result(
    structure: *const ffi::OcerzAbiStruct,
    call: *const ffi::OcerzGuestCall,
    guest_ret: u64,
    x8: *mut c_void,
    out_x: *mut u64,
    out_v: *mut u64,
) {
    let mut buffer = MaybeUninit::<[u8; ffi::OCERZ_ABI_STRUCT_BYTES as usize + 16]>::uninit();
    let bytes = buffer.as_mut_ptr().cast::<u8>();
    let words = abi_struct_words(structure);
    ptr::write_bytes(bytes, 0, words.wrapping_mul(8).wrapping_add(16));
    let mut classes = [0 as c_char; 2];
    let mut nint = 0 as c_int;
    let mut nsse = 0 as c_int;
    let n = abi_sysv_classify(structure, classes.as_mut_ptr(), &mut nint, &mut nsse);
    if n == 0 {
        abi_guest_read(guest_ret, bytes, (*structure).size as usize);
    } else {
        let integer = [(*call).rax, (*call).rdx];
        let floating = [(*call).xmm0, (*call).xmm1];
        let mut ri = 0usize;
        let mut si = 0usize;
        for k in 0..n {
            let word = if *classes.as_ptr().add(k) == b'I' as c_char {
                let result = *integer.as_ptr().add(ri);
                ri = ri.wrapping_add(1);
                result
            } else {
                let result = *floating.as_ptr().add(si);
                si = si.wrapping_add(1);
                result
            };
            abi_put_word(bytes, 8usize.wrapping_mul(k), word, 8);
        }
        abi_zero_tail(structure, bytes);
    }
    abi_struct_pointers(structure, bytes, true);

    let hfa = abi_hfa(structure);
    if hfa != 0 {
        let step = abi_class_size(hfa);
        for k in 0..(*structure).nmember as usize {
            *out_v.add(k) = abi_word(bytes, k.wrapping_mul(step), step);
        }
    } else if (*structure).size as usize > ABI_SMALL_STRUCT {
        ptr::copy_nonoverlapping(bytes, x8.cast::<u8>(), (*structure).size as usize);
    } else {
        *out_x = abi_word(bytes, 0, 8);
        *out_x.add(1) = abi_word(bytes, 8, 8);
    }
}

unsafe fn abi_callback_dispatch_inner(
    slot: u32,
    x: *const u64,
    v: *const u64,
    stack: *const u8,
    x8: *mut c_void,
    out_x: *mut u64,
    out_v: *mut u64,
    entered_errno: c_int,
    gs_out: *mut u64,
) {
    if !out_x.is_null() {
        ptr::write_bytes(out_x, 0, 2);
    }
    if !out_v.is_null() {
        ptr::write_bytes(out_v, 0, 4);
    }
    if x.is_null() || v.is_null() || out_x.is_null() || out_v.is_null() {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: callback slot %u was dispatched without its argument or result words\n"
                .as_ptr(),
            slot,
        );
        return;
    }

    if slot >= ffi::OCERZ_ABI_CALLBACK_SLOTS
        || (*ptr::addr_of!(G_ABI_CB)
            .cast::<AbiCallback>()
            .add(slot as usize))
        .used
        .load(Ordering::SeqCst)
            == 0
    {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: native code called callback slot %u, %s\n".as_ptr(),
            slot,
            if slot >= ffi::OCERZ_ABI_CALLBACK_SLOTS {
                c"which lies outside the bank".as_ptr()
            } else {
                c"to which no guest function was ever bound".as_ptr()
            },
        );
        return;
    }

    let entry = ptr::addr_of!(G_ABI_CB)
        .cast::<AbiCallback>()
        .add(slot as usize);
    let shape = (*entry).shape;
    let sig = ptr::addr_of!((*shape).sig);
    let notation = abi_shape_notation(shape);
    if !libc::strchr(notation, b'D' as c_int).is_null() {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: native code called guest function %#llx (callback slot %u, %s), and a long double does not cross in that direction\n".as_ptr(),
            (*entry).guest_fn as libc::c_ulonglong,
            slot,
            notation,
        );
        return;
    }
    if (*sig).ret == b'{' as c_char
        && abi_host_indirect(ptr::addr_of!((*sig).ret_struct))
        && x8.is_null()
    {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: guest function %#llx (callback slot %u, %s) returns a structure through x8, and its native caller passed no buffer there\n".as_ptr(),
            (*entry).guest_fn as libc::c_ulonglong,
            slot,
            notation,
        );
        return;
    }

    let mut cpu = ffi::ocerz_vm_current_cpu();
    if cpu.is_null() {
        cpu = ffi::ocerz_thread_attach(ffi::ocerz_vm_process());
    }
    if cpu.is_null() {
        libc::fprintf(
            log::stderr(),
            c"ocerz: abi: native code called guest function %#llx (callback slot %u, %s) on a thread with no guest cpu, and no guest personality could be attached to it\n".as_ptr(),
            (*entry).guest_fn as libc::c_ulonglong,
            slot,
            notation,
        );
        return;
    }

    let vm = (*cpu).vm;
    if vm.is_null() || (*vm).exited != 0 {
        return;
    }
    if (*cpu).gs_base != 0 {
        *gs_out = (*cpu).gs_base;
        ocerz_st(
            (*cpu).gs_base.wrapping_add(ffi::OCERZ_ERRNO_SLOT as u64),
            4,
            entered_errno as u32 as u64,
        );
    }

    let mut call = ffi::OcerzGuestCall::default();
    let mut owned = MaybeUninit::<[u64; ffi::OCERZ_ABI_MAX_ARGS as usize]>::uninit();
    let owned_ptr = owned.as_mut_ptr().cast::<u64>();
    let slots = size_of::<[u64; 16]>() / size_of::<u64>();
    let mut nx = 0 as c_int;
    let mut nv = 0 as c_int;
    let mut guest_int = 0 as c_int;
    let mut guest_fp = 0 as c_int;
    let mut nowned = 0 as c_int;
    let mut offset = 0usize;
    let rsp = *ptr::addr_of!((*cpu).gpr)
        .cast::<u64>()
        .add(ffi::OCERZ_RSP as usize);
    let mut stack_top = rsp.wrapping_sub(128) & !0xf;
    let mut guest_ret = 0u64;
    if (*sig).ret == b'{' as c_char && (*sig).ret_struct.size as usize > ABI_SMALL_STRUCT {
        stack_top = stack_top.wrapping_sub((*sig).ret_struct.size as u64) & !0xf;
        guest_ret = stack_top;
        *ptr::addr_of_mut!(call.gpr)
            .cast::<u64>()
            .add(guest_int as usize) = guest_ret;
        guest_int = guest_int.wrapping_add(1);
    }

    for i in 0..(*sig).nargs as usize {
        let class = *ptr::addr_of!((*sig).arg).cast::<c_char>().add(i);
        let fp = abi_is_fp(class);
        if class == b'{' as c_char {
            let structure = ptr::addr_of!((*sig).arg_struct)
                .cast::<ffi::OcerzAbiStruct>()
                .add(i);
            let mut buffer = MaybeUninit::<[u8; ffi::OCERZ_ABI_STRUCT_BYTES as usize]>::uninit();
            let buffer_ptr = buffer.as_mut_ptr().cast::<u8>();
            let rc = abi_host_struct_in(
                structure,
                x,
                v,
                stack,
                &mut nx,
                &mut nv,
                &mut offset,
                buffer_ptr,
            );
            if rc != ffi::OCERZ_OK as c_int {
                libc::fprintf(
                    log::stderr(),
                    c"ocerz: abi: guest function %#llx (callback slot %u, %s) takes structure argument %d %s\n".as_ptr(),
                    (*entry).guest_fn as libc::c_ulonglong,
                    slot,
                    notation,
                    i as c_int,
                    if rc == ffi::OCERZ_EFORMAT as c_int {
                        c"through a copy, and the native caller passed a null address".as_ptr()
                    } else {
                        c"from the native caller's stack, and no stack was passed".as_ptr()
                    },
                );
                abi_release_callback_owned(owned_ptr, &mut nowned);
                return;
            }
            abi_struct_pointers(structure, buffer_ptr, false);
            if !abi_guest_struct_out(
                structure,
                buffer_ptr,
                &mut call,
                &mut guest_int,
                &mut guest_fp,
            ) {
                libc::fprintf(
                    log::stderr(),
                    c"ocerz: abi: guest function %#llx (callback slot %u, %s) stacks more than the %d eightbytes a guest call carries\n".as_ptr(),
                    (*entry).guest_fn as libc::c_ulonglong,
                    slot,
                    notation,
                    slots as c_int,
                );
                abi_release_callback_owned(owned_ptr, &mut nowned);
                return;
            }
            continue;
        }

        let raw = if fp && (nv as usize) < ABI_HOST_FP_REGS {
            let value = *v.add(nv as usize);
            nv = nv.wrapping_add(1);
            value
        } else if !fp && (nx as usize) < ABI_HOST_INT_REGS {
            let value = *x.add(nx as usize);
            nx = nx.wrapping_add(1);
            value
        } else if !stack.is_null() {
            abi_pull_host_stack(stack, &mut offset, abi_class_size(class) as c_int)
        } else {
            libc::fprintf(
                log::stderr(),
                c"ocerz: abi: guest function %#llx (callback slot %u, %s) takes argument %d from the native caller's stack, and no stack was passed\n".as_ptr(),
                (*entry).guest_fn as libc::c_ulonglong,
                slot,
                notation,
                i as c_int,
            );
            abi_release_callback_owned(owned_ptr, &mut nowned);
            return;
        };

        let value = match class as u8 {
            b'p' => {
                if raw == 0 {
                    0
                } else {
                    ocerz_h2g(raw as usize as *const c_void)
                }
            }
            b'k' => {
                let mut guest = 0u64;
                let mut made = 0u64;
                let cb = ptr::addr_of!((*sig).cb)
                    .cast::<c_char>()
                    .add(i.wrapping_mul(ffi::OCERZ_ABI_CB_MAX as usize));
                if ffi::ocerz_block_to_guest(raw, cb, &mut guest, &mut made)
                    != ffi::OCERZ_OK as c_int
                {
                    libc::fprintf(
                        log::stderr(),
                        c"ocerz: abi: guest function %#llx (callback slot %u, %s) is handed native block %#llx as argument %d, which could not be made a block guest code can call\n".as_ptr(),
                        (*entry).guest_fn as libc::c_ulonglong,
                        slot,
                        notation,
                        raw as libc::c_ulonglong,
                        i as c_int,
                    );
                    abi_release_callback_owned(owned_ptr, &mut nowned);
                    return;
                }
                if made != 0 {
                    *owned_ptr.add(nowned as usize) = made;
                    nowned = nowned.wrapping_add(1);
                }
                guest
            }
            b'T' => abi_dtime(raw, false),
            _ => abi_narrow(class, raw),
        };
        if fp && (guest_fp as usize) < ABI_GUEST_FP_REGS {
            *ptr::addr_of_mut!(call.xmm)
                .cast::<u64>()
                .add(guest_fp as usize) = value;
            guest_fp = guest_fp.wrapping_add(1);
        } else if !fp && (guest_int as usize) < ABI_GUEST_INT_REGS {
            *ptr::addr_of_mut!(call.gpr)
                .cast::<u64>()
                .add(guest_int as usize) = value;
            guest_int = guest_int.wrapping_add(1);
        } else if (call.nstack as usize) < slots {
            *ptr::addr_of_mut!(call.stack)
                .cast::<u64>()
                .add(call.nstack as usize) = value;
            call.nstack = call.nstack.wrapping_add(1);
        } else {
            libc::fprintf(
                log::stderr(),
                c"ocerz: abi: guest function %#llx (callback slot %u, %s) stacks more than the %d eightbytes a guest call carries\n".as_ptr(),
                (*entry).guest_fn as libc::c_ulonglong,
                slot,
                notation,
                slots as c_int,
            );
            abi_release_callback_owned(owned_ptr, &mut nowned);
            return;
        }
    }

    let mut saved = MaybeUninit::<ffi::OcerzBridgeFrame>::uninit();
    ffi::ocerz_bridge_guest_enter(saved.as_mut_ptr());
    let fpcr = ocerz_abi_round_swap(ocerz_abi_round_of_mxcsr((*cpu).mxcsr));
    let rc = ffi::ocerz_vm_call_abi(vm, (*entry).guest_fn, &mut call, stack_top);
    ocerz_abi_round_swap(fpcr & ABI_FPCR_ROUND_MASK);
    ffi::ocerz_bridge_guest_leave(saved.as_ptr());
    if rc != ffi::OCERZ_OK as c_int || (*vm).exited != 0 {
        abi_release_callback_owned(owned_ptr, &mut nowned);
        return;
    }

    match (*sig).ret as u8 {
        b'v' => {}
        b'c' => {
            let mut native = call.rax;
            if native != 0
                && ocerz_abi_is_guest_code(native) != 0
                && ocerz_abi_callback_convert(native, (*sig).ret_cb.as_ptr(), &mut native)
                    != ffi::OCERZ_OK as c_int
            {
                libc::fprintf(
                    log::stderr(),
                    c"ocerz: abi: guest function %#llx (callback slot %u, %s) returned function %#llx, which could not be bound for native code, so native code is handed null\n".as_ptr(),
                    (*entry).guest_fn as libc::c_ulonglong,
                    slot,
                    notation,
                    call.rax as libc::c_ulonglong,
                );
                native = 0;
            }
            *out_x = if native == 0 {
                0
            } else {
                ocerz_g2h(native) as u64
            };
        }
        b'k' => {
            if ffi::ocerz_block_result_to_native(call.rax, (*sig).ret_cb.as_ptr(), out_x)
                != ffi::OCERZ_OK as c_int
            {
                libc::fprintf(
                    log::stderr(),
                    c"ocerz: abi: guest function %#llx (callback slot %u, %s) returned block %#llx, which could not be made a block native code can call, so native code is handed null\n".as_ptr(),
                    (*entry).guest_fn as libc::c_ulonglong,
                    slot,
                    notation,
                    call.rax as libc::c_ulonglong,
                );
            }
        }
        b'{' => abi_host_struct_result(
            ptr::addr_of!((*sig).ret_struct),
            &call,
            guest_ret,
            x8,
            out_x,
            out_v,
        ),
        b'p' => {
            *out_x = if call.rax == 0 {
                0
            } else {
                ocerz_g2h(call.rax) as u64
            };
        }
        b'f' => *out_v = call.xmm0 as u32 as u64,
        b'd' => *out_v = call.xmm0,
        b'T' => *out_x = abi_dtime(call.rax, true),
        _ => *out_x = abi_narrow((*sig).ret, call.rax),
    }
    abi_release_callback_owned(owned_ptr, &mut nowned);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_abi_callback_dispatch(
    slot: u32,
    x: *const u64,
    v: *const u64,
    stack: *const u8,
    x8: *mut c_void,
    out_x: *mut u64,
    out_v: *mut u64,
) {
    let errno = libc::__error();
    let entered_errno = *errno;
    let mut gs = 0u64;
    abi_callback_dispatch_inner(slot, x, v, stack, x8, out_x, out_v, entered_errno, &mut gs);
    *errno = if gs != 0 {
        ocerz_ld(gs.wrapping_add(ffi::OCERZ_ERRNO_SLOT as u64), 4) as c_int
    } else {
        entered_errno
    };
}
