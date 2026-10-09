//! ---- encodings ----
//! The encoding read is arm64's, since it is the native runtime's, and it
//! converts class by class: c b, C B, s h, S H, i i, I u, q l, Q L, f f, d d,
//! v as a result only, and * @ # : and any pointer p.  l and L are 32-bit in
//! an encoding and become i and u.  B is BOOL on arm64, where BOOL is bool,
//! while an x86 guest's BOOL is a signed char, and it becomes b, as the API
//! database writes it.  A structure {name=members} becomes the engine's
//! braces, nested structures nested, an array member flattened into that many
//! members, field names in quotes skipped.  Type qualifiers r n N o O R V and
//! A and the frame offsets between types are skipped.  A pointer is p whatever
//! it points at, and an array argument is a pointer.  A union whose members
//! are all integers and pointers becomes as many integers of its own alignment
//! as fill its size.  Everything else stops the send, naming the reason, with
//! OCERZ_BRIDGE_UNIMPL_EXIT.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null, null_mut};

use crate::ffi::*;

pub struct ObOut {
    pub buf: *mut c_char,
    pub cap: usize,
    pub len: usize,
    pub overflow: c_int,
}

#[inline]
pub unsafe fn ob_put(o: *mut ObOut, c: c_char) {
    unsafe {
        if (*o).len + 1 >= (*o).cap {
            (*o).overflow = 1;
            return;
        }
        *(*o).buf.add((*o).len) = c;
        (*o).len += 1;
        *(*o).buf.add((*o).len) = 0;
    }
}

pub const OB_RESULT: c_int = 0;
pub const OB_ARG: c_int = 1;
pub const OB_MEMBER: c_int = 2;
pub const OB_BLOCK: c_int = 1;
pub const OB_FNPTR: c_int = 2;
pub const OB_OBJECT: c_int = 3;
pub const OB_SMALL_STRUCT: usize = 16;

pub unsafe fn ob_quals(p: *const c_char) -> *const c_char {
    unsafe {
        let mut p = p;
        while *p != 0 && !libc::strchr(c"rnNoORVA".as_ptr(), *p as c_int).is_null() {
            p = p.add(1);
        }
        p
    }
}

pub unsafe fn ob_offset(p: *const c_char) -> *const c_char {
    unsafe {
        let mut p = p;
        while *p == b'-' as c_char || libc::isdigit(*p as u8 as c_int) != 0 {
            p = p.add(1);
        }
        p
    }
}

pub unsafe fn ob_group_end(p: *const c_char, open: c_char, close: c_char) -> *const c_char {
    unsafe {
        let mut depth = 0;
        let mut p = p;
        while *p != 0 {
            if *p == b'"' as c_char {
                let q = libc::strchr(p.add(1), b'"' as c_int);
                if q.is_null() {
                    return null();
                }
                p = q;
            } else if *p == open {
                depth += 1;
            } else if *p == close {
                depth -= 1;
                if depth == 0 {
                    return p.add(1);
                }
            }
            p = p.add(1);
        }
        null()
    }
}

pub unsafe fn ob_skip(p: *const c_char) -> *const c_char {
    unsafe {
        let mut p = ob_quals(p);
        match *p as u8 {
            0 => return null(),
            b'^' | b'j' => return ob_skip(p.add(1)),
            b'@' => {
                p = p.add(1);
                if *p == b'?' as c_char {
                    return if *p.add(1) == b'<' as c_char {
                        ob_group_end(p.add(1), b'<' as c_char, b'>' as c_char)
                    } else {
                        p.add(1)
                    };
                }
                if *p == b'"' as c_char {
                    let q = libc::strchr(p.add(1), b'"' as c_int);
                    return if q.is_null() { null() } else { q.add(1) };
                }
                return p;
            }
            b'{' => return ob_group_end(p, b'{' as c_char, b'}' as c_char),
            b'(' => return ob_group_end(p, b'(' as c_char, b')' as c_char),
            b'[' => return ob_group_end(p, b'[' as c_char, b']' as c_char),
            b'b' => {
                p = p.add(1);
                while libc::isdigit(*p as u8 as c_int) != 0 {
                    p = p.add(1);
                }
                return p;
            }
            _ => return p.add(1),
        }
    }
}

unsafe fn ob_int_layout(p: *const c_char, size: *mut usize, align: *mut usize) -> *const c_char {
    unsafe {
        let mut p = ob_quals(p);
        let mut s = 0usize;
        let mut a = 0usize;
        match *p as u8 {
            b'c' | b'C' | b'B' => {
                s = 1;
                a = 1;
                p = p.add(1);
            }
            b's' | b'S' => {
                s = 2;
                a = 2;
                p = p.add(1);
            }
            b'i' | b'I' | b'l' | b'L' => {
                s = 4;
                a = 4;
                p = p.add(1);
            }
            b'q' | b'Q' | b'*' | b'#' | b':' | b'%' => {
                s = 8;
                a = 8;
                p = p.add(1);
            }
            b'@' | b'^' => {
                p = ob_skip(p);
                s = 8;
                a = 8;
            }
            b'[' => {
                let mut n = 0usize;
                let mut es = 0usize;
                p = p.add(1);
                while libc::isdigit(*p as u8 as c_int) != 0 && n <= 4096 {
                    n = n * 10 + (*p as u8 - b'0') as usize;
                    p = p.add(1);
                }
                p = ob_int_layout(p, &mut es, &mut a);
                if p.is_null() || *p != b']' as c_char || n == 0 {
                    return null();
                }
                s = n * es;
                p = p.add(1);
            }
            b'{' | b'(' => {
                let close = if *p == b'{' as c_char { b'}' as c_char } else { b')' as c_char };
                let is_union = *p == b'(' as c_char;
                while *p != 0 && *p != b'=' as c_char && *p != close {
                    p = p.add(1);
                }
                if *p != b'=' as c_char {
                    return null();
                }
                p = p.add(1);
                let mut end = 0usize;
                a = 1;
                while *p != close {
                    if *p == b'"' as c_char {
                        let q = libc::strchr(p.add(1), b'"' as c_int);
                        if q.is_null() {
                            return null();
                        }
                        p = q.add(1);
                        continue;
                    }
                    let mut ms = 0usize;
                    let mut ma = 0usize;
                    p = if *p != 0 { ob_int_layout(p, &mut ms, &mut ma) } else { null() };
                    if p.is_null() {
                        return null();
                    }
                    a = if ma > a { ma } else { a };
                    end = if is_union {
                        if ms > end { ms } else { end }
                    } else {
                        (end + ma - 1) / ma * ma + ms
                    };
                }
                if end == 0 {
                    return null();
                }
                s = (end + a - 1) / a * a;
                p = p.add(1);
            }
            _ => return null(),
        }
        *size = s;
        *align = a;
        p
    }
}

unsafe fn ob_conv(
    p: *const c_char,
    o: *mut ObOut,
    w: c_int,
    rc: *mut c_int,
    special: *mut c_int,
) -> *const c_char {
    unsafe {
        let p0 = ob_quals(p);
        match *p0 as u8 {
            b'c' => {
                ob_put(o, b'b' as c_char);
                p0.add(1)
            }
            b'C' => {
                ob_put(o, b'B' as c_char);
                p0.add(1)
            }
            b's' => {
                ob_put(o, b'h' as c_char);
                p0.add(1)
            }
            b'S' => {
                ob_put(o, b'H' as c_char);
                p0.add(1)
            }
            b'i' => {
                ob_put(o, b'i' as c_char);
                p0.add(1)
            }
            b'I' => {
                ob_put(o, b'u' as c_char);
                p0.add(1)
            }
            b'l' => {
                ob_put(o, b'i' as c_char);
                p0.add(1)
            }
            b'L' => {
                ob_put(o, b'u' as c_char);
                p0.add(1)
            }
            b'q' => {
                ob_put(o, b'l' as c_char);
                p0.add(1)
            }
            b'Q' => {
                ob_put(o, b'L' as c_char);
                p0.add(1)
            }
            b'f' => {
                ob_put(o, b'f' as c_char);
                p0.add(1)
            }
            b'd' => {
                ob_put(o, b'd' as c_char);
                p0.add(1)
            }
            b'B' => {
                ob_put(o, b'b' as c_char);
                p0.add(1)
            }
            b'v' => {
                if w != OB_RESULT {
                    *rc = OCERZ_OBJC_VOID_VALUE as c_int;
                    return null();
                }
                ob_put(o, b'v' as c_char);
                p0.add(1)
            }
            b'*' | b'#' | b':' | b'%' => {
                ob_put(o, b'p' as c_char);
                p0.add(1)
            }
            b'@' | b'^' => {
                let q = ob_skip(p0);
                if q.is_null() {
                    *rc = OCERZ_OBJC_MALFORMED as c_int;
                    return null();
                }
                if *p0 == b'@' as c_char && *p0.add(1) == b'?' as c_char && w != OB_MEMBER {
                    if !special.is_null() {
                        *special = OB_BLOCK;
                    }
                    ob_put(o, b'k' as c_char);
                    ob_put(o, b'{' as c_char);
                    ob_put(o, b'}' as c_char);
                    return q;
                }
                if !special.is_null() && *p0.add(1) == b'?' as c_char {
                    *special = if *p0 == b'@' as c_char { OB_BLOCK } else { OB_FNPTR };
                } else if !special.is_null() && *p0 == b'@' as c_char {
                    *special = OB_OBJECT;
                }
                ob_put(o, b'p' as c_char);
                q
            }
            b'[' => {
                if w != OB_MEMBER {
                    let q = ob_group_end(p0, b'[' as c_char, b']' as c_char);
                    if q.is_null() {
                        *rc = OCERZ_OBJC_MALFORMED as c_int;
                        return null();
                    }
                    ob_put(o, b'p' as c_char);
                    return q;
                }
                let mut p = p0.add(1);
                let mut count = 0usize;
                while libc::isdigit(*p as u8 as c_int) != 0 {
                    count = count * 10 + (*p as u8 - b'0') as usize;
                    if count > OCERZ_ABI_STRUCT_MEMBERS as usize {
                        *rc = OCERZ_OBJC_ENGINE as c_int;
                        return null();
                    }
                    p = p.add(1);
                }
                if count == 0 {
                    *rc = OCERZ_OBJC_ENGINE as c_int;
                    return null();
                }
                let mut after = null();
                for _k in 0..count {
                    after = ob_conv(p, o, OB_MEMBER, rc, null_mut());
                    if after.is_null() {
                        return null();
                    }
                }
                if *after != b']' as c_char {
                    *rc = OCERZ_OBJC_MALFORMED as c_int;
                    return null();
                }
                after.add(1)
            }
            b'{' => {
                let mut p = p0.add(1);
                while *p != 0 && *p != b'=' as c_char && *p != b'}' as c_char {
                    p = p.add(1);
                }
                if *p == b'}' as c_char {
                    *rc = OCERZ_OBJC_OPAQUE as c_int;
                    return null();
                }
                if *p != b'=' as c_char {
                    *rc = OCERZ_OBJC_MALFORMED as c_int;
                    return null();
                }
                p = p.add(1);
                ob_put(o, b'{' as c_char);
                let mut members = 0;
                loop {
                    if *p == b'"' as c_char {
                        let q = libc::strchr(p.add(1), b'"' as c_int);
                        if q.is_null() {
                            *rc = OCERZ_OBJC_MALFORMED as c_int;
                            return null();
                        }
                        p = q.add(1);
                    }
                    if *p == b'}' as c_char {
                        break;
                    }
                    if *p == 0 {
                        *rc = OCERZ_OBJC_MALFORMED as c_int;
                        return null();
                    }
                    p = ob_conv(p, o, OB_MEMBER, rc, null_mut());
                    if p.is_null() {
                        return null();
                    }
                    members += 1;
                }
                if members == 0 {
                    *rc = OCERZ_OBJC_OPAQUE as c_int;
                    return null();
                }
                ob_put(o, b'}' as c_char);
                p.add(1)
            }
            b'(' => {
                let mut size = 0usize;
                let mut align = 0usize;
                let q = ob_int_layout(p0, &mut size, &mut align);
                if q.is_null() {
                    *rc = OCERZ_OBJC_UNION as c_int;
                    return null();
                }
                if size / align > OCERZ_ABI_STRUCT_MEMBERS as usize {
                    *rc = OCERZ_OBJC_ENGINE as c_int;
                    return null();
                }
                let unit = if align == 1 {
                    b'b' as c_char
                } else if align == 2 {
                    b'h' as c_char
                } else if align == 4 {
                    b'i' as c_char
                } else {
                    b'l' as c_char
                };
                if w != OB_MEMBER {
                    ob_put(o, b'{' as c_char);
                }
                for _k in 0..size / align {
                    ob_put(o, unit);
                }
                if w != OB_MEMBER {
                    ob_put(o, b'}' as c_char);
                }
                q
            }
            b'b' => {
                *rc = OCERZ_OBJC_BITFIELD as c_int;
                null()
            }
            b'D' => {
                *rc = OCERZ_OBJC_LONG_DOUBLE as c_int;
                null()
            }
            b'j' => {
                *rc = OCERZ_OBJC_COMPLEX as c_int;
                null()
            }
            b't' | b'T' => {
                *rc = OCERZ_OBJC_INT128 as c_int;
                null()
            }
            b'?' => {
                *rc = OCERZ_OBJC_UNKNOWN as c_int;
                null()
            }
            _ => {
                *rc = OCERZ_OBJC_MALFORMED as c_int;
                null()
            }
        }
    }
}

pub unsafe fn ob_notation(
    encoding: *const c_char,
    out: *mut c_char,
    outlen: usize,
    nargs: *mut c_int,
    blocks: *mut u32,
    fnptrs: *mut u32,
    objects: *mut u32,
) -> c_int {
    unsafe {
        let mut local = [0i8; OCERZ_OBJC_NOTATION_MAX as usize];
        let mut o = ObOut {
            buf: if out.is_null() { local.as_mut_ptr() } else { out },
            cap: if out.is_null() { OCERZ_OBJC_NOTATION_MAX as usize } else { outlen },
            len: 0,
            overflow: 0,
        };
        let mut rc = OCERZ_OBJC_OK as c_int;
        let mut n = 0;
        let mut bmask = 0u32;
        let mut fmask = 0u32;
        let mut omask = 0u32;

        if o.cap == 0 {
            return OCERZ_OBJC_TOO_LONG as c_int;
        }
        *o.buf = 0;
        if encoding.is_null() || *encoding == 0 {
            return OCERZ_OBJC_MALFORMED as c_int;
        }

        let mut p = ob_conv(encoding, &mut o, OB_RESULT, &mut rc, null_mut());
        if p.is_null() {
            return rc;
        }
        p = ob_offset(p);
        ob_put(&mut o, b'(' as c_char);
        while *p != 0 {
            let mut special = 0;
            if n >= OCERZ_ABI_MAX_ARGS as c_int {
                return OCERZ_OBJC_TOO_MANY_ARGS as c_int;
            }
            p = ob_conv(p, &mut o, OB_ARG, &mut rc, &mut special);
            if p.is_null() {
                return rc;
            }
            if special == OB_BLOCK {
                bmask |= 1 << n;
            } else if special == OB_FNPTR {
                fmask |= 1 << n;
            } else if special == OB_OBJECT {
                omask |= 1 << n;
            }
            p = ob_offset(p);
            n += 1;
            if o.overflow != 0 {
                return OCERZ_OBJC_TOO_LONG as c_int;
            }
        }
        ob_put(&mut o, b')' as c_char);
        if o.overflow != 0 {
            return OCERZ_OBJC_TOO_LONG as c_int;
        }

        let mut sig: OcerzAbiSig = core::mem::zeroed();
        if ocerz_abi_parse(o.buf, &mut sig) != OCERZ_OK as c_int {
            return OCERZ_OBJC_ENGINE as c_int;
        }
        if !nargs.is_null() {
            *nargs = n;
        }
        if !blocks.is_null() {
            *blocks = bmask;
        }
        if !fnptrs.is_null() {
            *fnptrs = fmask;
        }
        if !objects.is_null() {
            *objects = omask;
        }
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_notation(
    encoding: *const c_char,
    out: *mut c_char,
    outlen: usize,
    nargs: *mut c_int,
    blocks: *mut u32,
    fnptrs: *mut u32,
) -> c_int {
    unsafe { ob_notation(encoding, out, outlen, nargs, blocks, fnptrs, null_mut()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_refusal(code: c_int) -> *const c_char {
    match code as u32 {
        x if x == OCERZ_OBJC_OK => c"nothing refused".as_ptr(),
        x if x == OCERZ_OBJC_UNION => c"a union passed by value".as_ptr(),
        x if x == OCERZ_OBJC_BITFIELD => c"a bitfield".as_ptr(),
        x if x == OCERZ_OBJC_LONG_DOUBLE => c"a long double".as_ptr(),
        x if x == OCERZ_OBJC_COMPLEX => c"a complex number".as_ptr(),
        x if x == OCERZ_OBJC_INT128 => c"a 128-bit integer".as_ptr(),
        x if x == OCERZ_OBJC_UNKNOWN => c"a value of unknown type".as_ptr(),
        x if x == OCERZ_OBJC_OPAQUE => {
            c"a structure by value whose members the encoding does not give".as_ptr()
        }
        x if x == OCERZ_OBJC_VOID_VALUE => c"void where a value belongs".as_ptr(),
        x if x == OCERZ_OBJC_TOO_MANY_ARGS => {
            c"more arguments than the ABI engine carries".as_ptr()
        }
        x if x == OCERZ_OBJC_TOO_LONG => c"a notation longer than ocerz keeps".as_ptr(),
        x if x == OCERZ_OBJC_ENGINE => {
            c"a structure the ABI engine cannot lay out".as_ptr()
        }
        x if x == OCERZ_OBJC_NOT_METHOD => {
            c"no self and _cmd as its first two arguments".as_ptr()
        }
        x if x == OCERZ_OBJC_NULL => c"a null address where a structure belongs".as_ptr(),
        x if x == OCERZ_OBJC_BAD_LIST => {
            c"a list whose entry size ocerz cannot read".as_ptr()
        }
        x if x == OCERZ_OBJC_CYCLE => c"a class that is its own superclass".as_ptr(),
        _ => c"an encoding ocerz cannot parse".as_ptr(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_format_classes(
    fmt: *const c_char,
    dialect: c_int,
    out: *mut c_char,
    outlen: usize,
    why: *mut *const c_char,
) -> c_int {
    unsafe {
        let mut unused: *const c_char = null();
        let why = if why.is_null() { &mut unused } else { &mut *why };
        *why = null();
        let mut n = 0usize;
        let mut quote = 0i8;

        macro_rules! emit {
            ($c:expr) => {{
                if n + 1 >= outlen {
                    *why = c"more variadic arguments than ocerz gathers".as_ptr();
                    return -1;
                }
                *out.add(n) = $c;
                n += 1;
                *out.add(n) = 0;
            }};
        }

        if out.is_null() || outlen == 0 {
            *why = c"no room for a single argument".as_ptr();
            return -1;
        }
        *out = 0;
        if fmt.is_null() {
            return 0;
        }

        if dialect == OCERZ_OBJC_FMT_TYPES as c_int {
            let mut p = fmt;
            while *p != 0 {
                let q = ob_skip(p);
                if q.is_null() {
                    *why = c"a type list ocerz cannot parse".as_ptr();
                    return -1;
                }
                emit!(b'p' as c_char);
                p = ob_offset(q);
            }
            return n as c_int;
        }

        let mut p = fmt;
        while *p != 0 {
            if dialect == OCERZ_OBJC_FMT_PREDICATE as c_int {
                if quote != 0 {
                    if *p == b'\\' as c_char && *p.add(1) != 0 {
                        p = p.add(1);
                    } else if *p == quote {
                        quote = 0;
                    }
                    p = p.add(1);
                    continue;
                }
                if *p == b'\'' as c_char || *p == b'"' as c_char {
                    quote = *p;
                    p = p.add(1);
                    continue;
                }
            }
            if *p != b'%' as c_char {
                p = p.add(1);
                continue;
            }
            p = p.add(1);
            if *p == b'%' as c_char {
                p = p.add(1);
                continue;
            }
            if *p == 0 {
                break;
            }

            let mut d = p;
            while libc::isdigit(*d as u8 as c_int) != 0 {
                d = d.add(1);
            }
            if d > p && *d == b'$' as c_char {
                *why = c"a positional argument (%n$)".as_ptr();
                return -1;
            }
            while *p != 0 && !libc::strchr(c"-+ #0'".as_ptr(), *p as c_int).is_null() {
                p = p.add(1);
            }
            if *p == b'*' as c_char {
                emit!(b'i' as c_char);
                p = p.add(1);
                if libc::isdigit(*p as u8 as c_int) != 0 {
                    *why = c"a positional width (*n$)".as_ptr();
                    return -1;
                }
            } else {
                while libc::isdigit(*p as u8 as c_int) != 0 {
                    p = p.add(1);
                }
            }
            if *p == b'.' as c_char {
                p = p.add(1);
                if *p == b'*' as c_char {
                    emit!(b'i' as c_char);
                    p = p.add(1);
                    if libc::isdigit(*p as u8 as c_int) != 0 {
                        *why = c"a positional precision (.*n$)".as_ptr();
                        return -1;
                    }
                } else {
                    while libc::isdigit(*p as u8 as c_int) != 0 {
                        p = p.add(1);
                    }
                }
            }

            let mut len = 0i8;
            if *p == b'h' as c_char && *p.add(1) == b'h' as c_char {
                len = b'H' as c_char;
                p = p.add(2);
            } else if *p == b'h' as c_char {
                len = b'h' as c_char;
                p = p.add(1);
            } else if *p == b'l' as c_char && *p.add(1) == b'l' as c_char {
                len = b'q' as c_char;
                p = p.add(2);
            } else if *p == b'l' as c_char {
                len = b'l' as c_char;
                p = p.add(1);
            } else if *p == b'q' as c_char
                || *p == b'j' as c_char
                || *p == b'z' as c_char
                || *p == b't' as c_char
            {
                len = b'q' as c_char;
                p = p.add(1);
            } else if *p == b'L' as c_char {
                len = b'L' as c_char;
                p = p.add(1);
            }

            match *p as u8 {
                b'd' | b'i' | b'o' | b'u' | b'x' | b'X' => {
                    emit!(if len == b'l' as c_char || len == b'q' as c_char { b'l' as c_char } else { b'i' as c_char });
                }
                b'D' | b'O' | b'U' => {
                    emit!(
                        if dialect == OCERZ_OBJC_FMT_C as c_int || len == b'l' as c_char || len == b'q' as c_char {
                            b'l' as c_char
                        } else {
                            b'i' as c_char
                        }
                    );
                }
                b'c' | b'C' => emit!(b'i' as c_char),
                b's' | b'S' => emit!(b'p' as c_char),
                b'p' => emit!(b'L' as c_char),
                b'e' | b'E' | b'f' | b'F' | b'g' | b'G' | b'a' | b'A' => {
                    if len == b'L' as c_char {
                        *why = c"a long double conversion (%L)".as_ptr();
                        return -1;
                    }
                    emit!(b'd' as c_char);
                }
                b'n' => {
                    *why = c"%n, which writes through its argument".as_ptr();
                    return -1;
                }
                b'@' => {
                    if dialect == OCERZ_OBJC_FMT_C as c_int {
                        *why = c"%@, which a C format does not have".as_ptr();
                        return -1;
                    }
                    emit!(b'p' as c_char);
                }
                b'K' => {
                    if dialect != OCERZ_OBJC_FMT_PREDICATE as c_int {
                        *why = c"%K, which only a predicate format has".as_ptr();
                        return -1;
                    }
                    emit!(b'p' as c_char);
                }
                0 => return n as c_int,
                _ => {
                    *why = c"a conversion this format dialect does not have".as_ptr();
                    return -1;
                }
            }
            p = p.add(1);
        }
        n as c_int
    }
}

#[repr(C)]
struct ObVariadic {
    sel: *const c_char,
    kind: c_int,
    arg: c_int,
    dialect: c_int,
    attributed: c_int,
    va_arg: c_int,
}
unsafe impl Sync for ObVariadic {}

macro_rules! va {
    ($sel:literal, $k:expr, $a:expr, $d:expr, $at:expr, $v:expr) => {
        ObVariadic {
            sel: concat!($sel, "\0").as_ptr() as *const c_char,
            kind: $k as c_int,
            arg: $a as c_int,
            dialect: $d as c_int,
            attributed: $at as c_int,
            va_arg: $v as c_int,
        }
    };
}

static G_OB_VARIADIC: &[ObVariadic] = &[
    va!("stringWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("localizedStringWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("initWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("initWithFormat:locale:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("appendFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("stringByAppendingFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("stringWithValidatedFormat:validFormatSpecifiers:error:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("localizedStringWithValidatedFormat:validFormatSpecifiers:error:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("initWithValidatedFormat:validFormatSpecifiers:error:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("initWithValidatedFormat:validFormatSpecifiers:locale:error:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("localizedAttributedStringWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 1, 0),
    va!("appendLocalizedFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 1, 0),
    va!("raise:format:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("handleFailureInMethod:object:file:lineNumber:description:", OCERZ_OBJC_VA_FORMAT, 6, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("handleFailureInFunction:file:lineNumber:description:", OCERZ_OBJC_VA_FORMAT, 5, OCERZ_OBJC_FMT_CF, 0, 0),
    va!("predicateWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_PREDICATE, 0, 0),
    va!("expressionWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_PREDICATE, 0, 0),
    va!("encodeValuesOfObjCTypes:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_TYPES, 0, 0),
    va!("decodeValuesOfObjCTypes:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_TYPES, 0, 0),
    va!("arrayWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("initWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("setWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("orderedSetWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("dictionaryWithObjectsAndKeys:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("initWithObjectsAndKeys:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0, 0),
    va!("initWithFormat:arguments:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_CF, 0, 3),
    va!("initWithFormat:locale:arguments:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_CF, 0, 4),
    va!("initWithValidatedFormat:validFormatSpecifiers:arguments:error:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_CF, 0, 4),
    va!("initWithValidatedFormat:validFormatSpecifiers:locale:arguments:error:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_CF, 0, 5),
    va!("initWithFormat:options:locale:arguments:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_CF, 1, 5),
    va!("raise:format:arguments:", OCERZ_OBJC_VA_LIST, 3, OCERZ_OBJC_FMT_CF, 0, 4),
    va!("predicateWithFormat:arguments:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_PREDICATE, 0, 3),
    va!("expressionWithFormat:arguments:", OCERZ_OBJC_VA_LIST, 2, OCERZ_OBJC_FMT_PREDICATE, 0, 3),
];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_variadic(sel: *const c_char) -> *const OcerzObjcVariadic {
    unsafe {
        if sel.is_null() {
            return null();
        }
        for v in G_OB_VARIADIC {
            if libc::strcmp(v.sel, sel) == 0 {
                return v as *const ObVariadic as *const OcerzObjcVariadic;
            }
        }
        null()
    }
}

#[repr(C)]
struct ObFnarg {
    sel: *const c_char,
    arg: c_int,
    notation: *const c_char,
}
unsafe impl Sync for ObFnarg {}

static G_OB_FNARGS: &[ObFnarg] = &[
    ObFnarg { sel: c"sortSubviewsUsingFunction:context:".as_ptr(), arg: 2, notation: c"l(ppp)".as_ptr() },
    ObFnarg { sel: c"sortedArrayUsingFunction:context:".as_ptr(), arg: 2, notation: c"l(ppp)".as_ptr() },
    ObFnarg { sel: c"sortedArrayUsingFunction:context:hint:".as_ptr(), arg: 2, notation: c"l(ppp)".as_ptr() },
    ObFnarg { sel: c"sortUsingFunction:context:".as_ptr(), arg: 2, notation: c"l(ppp)".as_ptr() },
];

pub unsafe fn ob_fnarg_notation(
    sel: *const c_char,
    notation: *const c_char,
    fnptrs: *mut u32,
    out: *mut c_char,
    outlen: usize,
) -> c_int {
    unsafe {
        for i in 0..G_OB_FNARGS.len() {
            if sel.is_null() || libc::strcmp(G_OB_FNARGS[i].sel, sel) != 0 {
                continue;
            }
            let arg = G_OB_FNARGS[i].arg;
            let at = crate::ported::objcbridge::send::ob_arg_at(notation, arg);
            if at.is_null() || *at != b'p' as c_char || (*fnptrs & (1 << arg)) == 0 {
                return 0;
            }
            let n = libc::snprintf(
                out,
                outlen,
                c"%.*sc{%s}%s".as_ptr(),
                (at as usize - notation as usize) as c_int,
                notation,
                G_OB_FNARGS[i].notation,
                at.add(1),
            );
            if n < 0 || n as usize >= outlen {
                return 0;
            }
            *fnptrs &= !(1u32 << arg);
            return 1;
        }
        0
    }
}
