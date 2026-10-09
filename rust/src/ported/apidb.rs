//! Reading the API database.
//!
//! ocerz/apidb.h states the format, where the files live and how a version
//! directory is chosen; what follows is what this file knows that the header
//! does not.
//!
//! ---- refusing whole, and saying where ----
//! The parser walks the file once, in order, and stops at the first line that
//! breaks a rule, writing path:line and the rule into the caller's buffer.  Every
//! rule is checked on that walk except the ones a struct record can only be held
//! to once the whole file has been read, since a struct record may come before
//! the fn and shape records it names: those are checked after the walk, in the
//! order the struct records appear, and a failure there names the struct
//! record's own line.  Nothing is published until every rule has held, so a
//! caller never sees a library that is correct up to some line.
//!
//! A signature is held to the notation by ocerz_abi_parse itself, the parser the
//! bridge will hand it to, not by a copy of its grammar, so a signature this file
//! accepts is one the crossing can take.  A shape word is held to the stricter
//! rule a callback signature has to meet, no callback of its own and short
//! enough for the bank to store, because that is what the bridge does with it.
//! A signature the engine refuses is a broken file rather than an export to skip:
//! the generator wrote a crossing, and a crossing that silently became a stub
//! would move the failure from generation time to the middle of some program.
//!
//! ---- the root is made absolute before anything uses it ----
//! The guest runs inside this process and its chdir is the process's chdir.  A
//! library is loaded the first time something asks for it, which for a dlopen
//! can be long after the guest has changed directory, so a relative OCERZ_APIDB
//! resolved late would name a different directory from the one resolved early.
//! The root is therefore put through realpath when the version directory is
//! chosen, and every later path is built on that.
//!
//! ---- versions are numbers ----
//! A version directory's name is read as up to three decimal components and
//! packed the way Mach-O packs a minimum OS version, sixteen bits of major and
//! eight each of minor and patch, and every comparison is between packed
//! numbers.  Compared as strings, 10.13 sorts below 10.9 and 27.0 would not
//! outrank 26.6 once a 100 appeared; packed, they order the way the releases do.
//! A name that is not such a version, or that is not a directory, is ignored,
//! and two names that pack to the same number are settled by the smaller name,
//! so the choice never depends on the order the file system lists them in.
//!
//! ---- one answer per install name, including no ----
//! The loader asks whether an install name has a library for every dependency it
//! resolves an import through, so an answer of no is asked for far more often
//! than a yes and is remembered exactly like one: a list of install names with
//! the library or the absence of one, published by a single atomic store of its
//! head under the lock, and read without the lock.  A name is loaded at most
//! once, the pointer handed out for it never changes, and nothing is freed.  A
//! file whose library record names a different install name answers no for the
//! name that led to it, since two install names can share a last component.
//! A refused file is announced on stderr whatever the verbosity, because the
//! consequence - a library native mode does not synthesize - otherwise shows up
//! only as a list of imports nothing binds.
//!
//! ---- lookup by name ----
//! An export is found through an open-addressed hash of the entries' indices,
//! built while parsing, which is also what catches a name declared twice on the
//! line where the second declaration is.  The table and the source text the
//! entries' strings point into belong to a private structure the public one is
//! the first member of.
//!
use core::ffi::{c_char, c_int, c_uint};
use core::{mem, ptr};
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

use crate::ffi;

const AD_VAR_BYTES_MAX: u64 = 1 << 20;
const AD_FIELDS_MAX: usize = ffi::OCERZ_APIDB_SHAPE_WORDS as usize + 8;
const PATH_MAX: usize = libc::PATH_MAX as usize;

#[repr(C)]
struct AdLibrary {
    pub_: ffi::OcerzApiLibrary,
    text: *mut c_char,
    path: *mut c_char,
    hash: *mut u32,
    hcap: u32,
}

#[repr(C)]
struct AdStructRec {
    line: c_int,
    export_name: *const c_char,
    argpos: c_int,
    shape: *const c_char,
}

#[repr(C)]
struct AdInplaceRec {
    line: c_int,
    export_name: *const c_char,
    argpos: c_int,
    offset: u32,
    sig: *const c_char,
}

struct AdParse {
    path: *const c_char,
    err: *mut c_char,
    errlen: usize,
    lib: *mut AdLibrary,
    entries: *mut ffi::OcerzApiEntry,
    entry_line: *mut c_int,
    nentries: c_int,
    centries: c_int,
    shapes: *mut ffi::OcerzApiShape,
    shape_line: *mut c_int,
    nshapes: c_int,
    cshapes: c_int,
    structs: *mut AdStructRec,
    nstructs: c_int,
    cstructs: c_int,
    inplace: *mut AdInplaceRec,
    ninplace: c_int,
    cinplace: c_int,
}

macro_rules! ad_refuse {
    ($p:expr, $line:expr, $fmt:literal $(, $arg:expr)* $(,)?) => {{
        let p = $p;
        if !p.err.is_null() && p.errlen != 0 {
            let n = unsafe {
                libc::snprintf(
                    p.err,
                    p.errlen,
                    b"%s:%d: \0".as_ptr().cast(),
                    p.path,
                    $line as c_int,
                )
            };
            if n >= 0 && (n as usize) < p.errlen {
                unsafe {
                    libc::snprintf(
                        p.err.add(n as usize),
                        p.errlen - n as usize,
                        concat!($fmt, "\0").as_ptr().cast(),
                        $($arg),*
                    );
                }
            }
        }
        0
    }};
}

unsafe fn ad_hash_name(mut s: *const c_char) -> u32 {
    let mut h = 2166136261u32;
    while unsafe { *s } != 0 {
        h ^= unsafe { *s } as u8 as u32;
        h = h.wrapping_mul(16777619);
        s = unsafe { s.add(1) };
    }
    h
}

unsafe fn ad_hash_find(
    lib: *const AdLibrary,
    entries: *const ffi::OcerzApiEntry,
    name: *const c_char,
) -> c_int {
    if unsafe { (*lib).hcap } == 0 {
        return -1;
    }
    let mask = unsafe { (*lib).hcap } - 1;
    let mut i = unsafe { ad_hash_name(name) } & mask;
    loop {
        let slot = unsafe { *(*lib).hash.add(i as usize) };
        if slot == 0 {
            return -1;
        }
        let e = unsafe { &*entries.add((slot - 1) as usize) };
        if unsafe { libc::strcmp(e.export_name, name) } == 0 {
            return (slot - 1) as c_int;
        }
        i = i.wrapping_add(1) & mask;
    }
}

unsafe fn ad_hash_grow(lib: *mut AdLibrary, entries: *const ffi::OcerzApiEntry, n: c_int) -> bool {
    let old = unsafe { (*lib).hcap };
    let mut cap = if old != 0 { old } else { 64 };
    while ((n + 1) as u64) * 2 > cap as u64 {
        cap = cap.wrapping_mul(2);
    }
    if cap == old {
        return true;
    }
    let h = unsafe { libc::calloc(cap as usize, mem::size_of::<u32>()) as *mut u32 };
    if h.is_null() {
        return false;
    }
    for k in 0..n as usize {
        let mut i = unsafe { ad_hash_name((*entries.add(k)).export_name) } & (cap - 1);
        while unsafe { *h.add(i as usize) } != 0 {
            i = i.wrapping_add(1) & (cap - 1);
        }
        unsafe { *h.add(i as usize) = (k as u32).wrapping_add(1) };
    }
    unsafe {
        libc::free((*lib).hash.cast());
        (*lib).hash = h;
        (*lib).hcap = cap;
    }
    true
}

unsafe fn ad_hash_put(lib: *mut AdLibrary, entries: *const ffi::OcerzApiEntry, k: c_int) {
    let mask = unsafe { (*lib).hcap } - 1;
    let mut i = unsafe { ad_hash_name((*entries.add(k as usize)).export_name) } & mask;
    while unsafe { *(*lib).hash.add(i as usize) } != 0 {
        i = i.wrapping_add(1) & mask;
    }
    unsafe { *(*lib).hash.add(i as usize) = (k as u32).wrapping_add(1) };
}

unsafe fn ad_grow(arr: *mut *mut u8, cap: *mut c_int, want: c_int, elem: usize) -> bool {
    if want <= unsafe { *cap } {
        return true;
    }
    let mut nc = if unsafe { *cap } != 0 {
        (unsafe { *cap }) * 2
    } else {
        64
    };
    while nc < want {
        nc *= 2;
    }
    let na = unsafe { libc::realloc(*arr as *mut _, nc as usize * elem) };
    if na.is_null() {
        return false;
    }
    unsafe {
        *arr = na.cast();
        *cap = nc;
    }
    true
}

unsafe fn ad_decimal(mut s: *const c_char, max: u64, out: *mut u64) -> bool {
    if unsafe { *s } == 0 || (unsafe { *s } == b'0' as c_char && unsafe { *s.add(1) } != 0) {
        return false;
    }
    let mut v = 0u64;
    while unsafe { *s } != 0 {
        let c = unsafe { *s };
        if !(b'0' as c_char..=b'9' as c_char).contains(&c) {
            return false;
        }
        let d = (c as u8 - b'0') as u64;
        if v > (u64::MAX - d) / 10 {
            return false;
        }
        v = v.wrapping_mul(10).wrapping_add(d);
        s = unsafe { s.add(1) };
    }
    if v > max {
        return false;
    }
    unsafe { *out = v };
    true
}

unsafe fn ad_version(s: *const c_char, packed: *mut u32) -> bool {
    let mut part = [0u32; 3];
    let limit = [0xffffu32, 0xff, 0xff];
    let mut n = 0usize;
    let mut c = s;
    loop {
        if n == 3 {
            return false;
        }
        let first = unsafe { *c };
        if !(b'0' as c_char..=b'9' as c_char).contains(&first) {
            return false;
        }
        let mut v = 0u32;
        while unsafe { *c } >= b'0' as c_char && unsafe { *c } <= b'9' as c_char {
            v = v
                .wrapping_mul(10)
                .wrapping_add((unsafe { *c } as u8 - b'0') as u32);
            if v > *limit.as_ptr().add(n) {
                return false;
            }
            c = unsafe { c.add(1) };
        }
        *part.as_mut_ptr().add(n) = v;
        n += 1;
        if unsafe { *c } == 0 {
            break;
        }
        if unsafe { *c } != b'.' as c_char {
            return false;
        }
        c = unsafe { c.add(1) };
    }
    unsafe {
        *packed = *part.as_ptr() << 16
            | *part.as_ptr().add(1) << 8
            | *part.as_ptr().add(2);
    }
    true
}

unsafe fn ad_kind_name(k: ffi::OcerzApiKind) -> *const c_char {
    match k {
        ffi::OCERZ_API_FN => b"fn\0".as_ptr().cast(),
        ffi::OCERZ_API_DATA => b"data\0".as_ptr().cast(),
        ffi::OCERZ_API_VAR => b"var\0".as_ptr().cast(),
        ffi::OCERZ_API_SPECIAL => b"special\0".as_ptr().cast(),
        ffi::OCERZ_API_STUB => b"stub\0".as_ptr().cast(),
        _ => b"unknown\0".as_ptr().cast(),
    }
}

unsafe fn ad_callback_sig(s: *const c_char) -> bool {
    if unsafe { libc::strlen(s) } >= ffi::OCERZ_ABI_CB_MAX as usize {
        return false;
    }
    let mut sig = mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
    if unsafe { ffi::ocerz_abi_parse(s, sig.as_mut_ptr()) } != 0 {
        return false;
    }
    let sig = unsafe { sig.assume_init() };
    for i in 0..sig.nargs as usize {
        if *sig.arg.as_ptr().add(i) == b'c' as c_char {
            return false;
        }
    }
    true
}

unsafe fn ad_add_entry(
    p: *mut AdParse,
    line: c_int,
    kind: ffi::OcerzApiKind,
    name: *mut c_char,
    out: *mut *mut ffi::OcerzApiEntry,
) -> bool {
    let prior = unsafe { ad_hash_find((*p).lib, (*p).entries, name) };
    if prior >= 0 {
        let e = unsafe { &*(*p).entries.add(prior as usize) };
        return ad_refuse!(
            unsafe { &*p },
            line,
            "export %s is already declared, as %s, on line %d",
            name,
            unsafe { ad_kind_name(e.kind) },
            unsafe { *(*p).entry_line.add(prior as usize) }
        ) != 0;
    }
    if !unsafe {
        ad_grow(
            ptr::addr_of_mut!((*p).entries).cast(),
            &mut (*p).centries,
            (*p).nentries + 1,
            mem::size_of::<ffi::OcerzApiEntry>(),
        )
    } {
        return false;
    }
    let lcap = unsafe { (*p).centries };
    let nl = unsafe {
        libc::realloc(
            (*p).entry_line.cast(),
            lcap as usize * mem::size_of::<c_int>(),
        ) as *mut c_int
    };
    if nl.is_null() {
        return false;
    }
    unsafe { (*p).entry_line = nl };
    if !unsafe { ad_hash_grow((*p).lib, (*p).entries, (*p).nentries) } {
        return false;
    }
    let k = unsafe { (*p).nentries };
    unsafe {
        (*p).nentries += 1;
        let e = &mut *(*p).entries.add(k as usize);
        ptr::write_bytes(e, 0, 1);
        e.kind = kind;
        e.export_name = name;
        *(*p).entry_line.add(k as usize) = line;
        ad_hash_put((*p).lib, (*p).entries, k);
        *out = e;
    }
    true
}

unsafe fn ad_record(p: *mut AdParse, line: c_int, f: &[*mut c_char], nf: c_int) -> bool {
    let kind = f[0];
    let mut e: *mut ffi::OcerzApiEntry = ptr::null_mut();
    let mut num = 0u64;
    if unsafe { libc::strcmp(kind, b"fn\0".as_ptr().cast()) } == 0 {
        if nf != 4 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a fn record has %d fields, want 4: fn <export> <host-symbol> <signature>",
                nf
            );
            return false;
        }
        let mut sig = mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
        if unsafe { ffi::ocerz_abi_parse(f[3], sig.as_mut_ptr()) } != 0 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "fn %s is declared %s, which is not a signature in the notation abi.h defines",
                f[1],
                f[3]
            );
            return false;
        }
        if !unsafe { ad_add_entry(p, line, ffi::OCERZ_API_FN, f[1], &mut e) } {
            return false;
        }
        unsafe {
            (*e).host = f[2];
            (*e).sig = f[3];
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"data\0".as_ptr().cast()) } == 0 {
        if nf != 3 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a data record has %d fields, want 3: data <export> <host-symbol>",
                nf
            );
            return false;
        }
        if !unsafe { ad_add_entry(p, line, ffi::OCERZ_API_DATA, f[1], &mut e) } {
            return false;
        }
        unsafe {
            (*e).host = f[2];
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"var\0".as_ptr().cast()) } == 0 {
        if nf != 4 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a var record has %d fields, want 4: var <export> <bytes> <filler>",
                nf
            );
            return false;
        }
        if !unsafe { ad_decimal(f[2], AD_VAR_BYTES_MAX, &mut num) } || num == 0 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "var %s is %s bytes, want a decimal count from 1 to %u",
                f[1],
                f[2],
                AD_VAR_BYTES_MAX as c_uint
            );
            return false;
        }
        if !unsafe { ad_add_entry(p, line, ffi::OCERZ_API_VAR, f[1], &mut e) } {
            return false;
        }
        unsafe {
            (*e).bytes = num as u32;
            (*e).filler = f[3];
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"special\0".as_ptr().cast()) } == 0 {
        if nf != 3 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a special record has %d fields, want 3: special <export> <handler>",
                nf
            );
            return false;
        }
        if !unsafe { ad_add_entry(p, line, ffi::OCERZ_API_SPECIAL, f[1], &mut e) } {
            return false;
        }
        unsafe {
            (*e).handler = f[2];
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"stub\0".as_ptr().cast()) } == 0 {
        if nf != 3 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a stub record has %d fields, want 3: stub <export> <reason>",
                nf
            );
            return false;
        }
        if !unsafe { ad_add_entry(p, line, ffi::OCERZ_API_STUB, f[1], &mut e) } {
            return false;
        }
        unsafe {
            (*e).reason = f[2];
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"shape\0".as_ptr().cast()) } == 0 {
        if nf < 4 {
            ad_refuse!(unsafe { &*p }, line, "a shape record has %d fields, want at least 4: shape <name> <version> <words> <word>...", nf);
            return false;
        }
        let mut version = 0u64;
        if !unsafe { ad_decimal(f[2], u64::MAX, &mut version) } {
            ad_refuse!(
                unsafe { &*p },
                line,
                "shape %s has version %s, want a decimal number",
                f[1],
                f[2]
            );
            return false;
        }
        if !unsafe { ad_decimal(f[3], ffi::OCERZ_APIDB_SHAPE_WORDS as u64, &mut num) } || num == 0 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "shape %s version %s is %s words long, want a decimal count from 1 to %d",
                f[1],
                f[2],
                f[3],
                ffi::OCERZ_APIDB_SHAPE_WORDS as c_int
            );
            return false;
        }
        if (nf - 4) as u64 != num {
            ad_refuse!(
                unsafe { &*p },
                line,
                "shape %s version %s declares %s words and lists %d",
                f[1],
                f[2],
                f[3],
                nf - 4
            );
            return false;
        }
        for w in 0..num as usize {
            if unsafe { libc::strcmp(f[4 + w], b"-\0".as_ptr().cast()) } != 0
                && !unsafe { ad_callback_sig(f[4 + w]) }
            {
                ad_refuse!(unsafe { &*p }, line, "word %d of shape %s version %s is %s, which is neither - nor a signature abi.h can call back through", w as c_int, f[1], f[2], f[4 + w]);
                return false;
            }
        }
        for s in 0..unsafe { (*p).nshapes } {
            let sh = unsafe { &*(*p).shapes.add(s as usize) };
            if sh.version == version && unsafe { libc::strcmp(sh.name, f[1]) } == 0 {
                ad_refuse!(
                    unsafe { &*p },
                    line,
                    "shape %s version %s is already declared on line %d",
                    f[1],
                    f[2],
                    unsafe { *(*p).shape_line.add(s as usize) }
                );
                return false;
            }
        }
        if !unsafe {
            ad_grow(
                ptr::addr_of_mut!((*p).shapes).cast(),
                &mut (*p).cshapes,
                (*p).nshapes + 1,
                mem::size_of::<ffi::OcerzApiShape>(),
            )
        } {
            return false;
        }
        let nl = unsafe {
            libc::realloc(
                (*p).shape_line.cast(),
                (*p).cshapes as usize * mem::size_of::<c_int>(),
            ) as *mut c_int
        };
        if nl.is_null() {
            return false;
        }
        unsafe {
            (*p).shape_line = nl;
            let sh = &mut *(*p).shapes.add((*p).nshapes as usize);
            ptr::write_bytes(sh, 0, 1);
            sh.name = f[1];
            sh.version = version;
            sh.words = num as c_int;
            for w in 0..num as usize {
                *sh.word.as_mut_ptr().add(w) = if libc::strcmp(f[4 + w], b"-\0".as_ptr().cast()) == 0 {
                    ptr::null()
                } else {
                    f[4 + w]
                };
            }
            *(*p).shape_line.add((*p).nshapes as usize) = line;
            (*p).nshapes += 1;
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"struct\0".as_ptr().cast()) } == 0 {
        if nf != 4 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "a struct record has %d fields, want 4: struct <export> <argpos> <shape-name>",
                nf
            );
            return false;
        }
        if !unsafe { ad_decimal(f[2], (ffi::OCERZ_ABI_MAX_ARGS - 1) as u64, &mut num) } {
            ad_refuse!(
                unsafe { &*p },
                line,
                "struct %s names argument %s, want a decimal position from 0 to %d",
                f[1],
                f[2],
                (ffi::OCERZ_ABI_MAX_ARGS - 1) as c_int
            );
            return false;
        }
        if !unsafe {
            ad_grow(
                ptr::addr_of_mut!((*p).structs).cast(),
                &mut (*p).cstructs,
                (*p).nstructs + 1,
                mem::size_of::<AdStructRec>(),
            )
        } {
            return false;
        }
        unsafe {
            let r = &mut *(*p).structs.add((*p).nstructs as usize);
            r.line = line;
            r.export_name = f[1];
            r.argpos = num as c_int;
            r.shape = f[3];
            (*p).nstructs += 1;
        }
        return true;
    }
    if unsafe { libc::strcmp(kind, b"inplace\0".as_ptr().cast()) } == 0 {
        let mut off = 0u64;
        if nf != 5 {
            ad_refuse!(unsafe { &*p }, line, "an inplace record has %d fields, want 5: inplace <export> <argpos> <offset> <signature>", nf);
            return false;
        }
        if !unsafe { ad_decimal(f[2], (ffi::OCERZ_ABI_MAX_ARGS - 1) as u64, &mut num) } {
            ad_refuse!(
                unsafe { &*p },
                line,
                "inplace %s names argument %s, want a decimal position from 0 to %d",
                f[1],
                f[2],
                (ffi::OCERZ_ABI_MAX_ARGS - 1) as c_int
            );
            return false;
        }
        if !unsafe { ad_decimal(f[3], ffi::OCERZ_APIDB_INPLACE_MAX_OFFSET as u64, &mut off) }
            || (off & 7) != 0
        {
            ad_refuse!(
                unsafe { &*p },
                line,
                "inplace %s names offset %s, want a multiple of 8 from 0 to %d",
                f[1],
                f[3],
                ffi::OCERZ_APIDB_INPLACE_MAX_OFFSET as c_int
            );
            return false;
        }
        let mut sig = mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
        if unsafe { ffi::ocerz_abi_parse(f[4], sig.as_mut_ptr()) } != 0 {
            ad_refuse!(
                unsafe { &*p },
                line,
                "inplace %s gives the signature %s, which is not notation",
                f[1],
                f[4]
            );
            return false;
        }
        if !unsafe {
            ad_grow(
                ptr::addr_of_mut!((*p).inplace).cast(),
                &mut (*p).cinplace,
                (*p).ninplace + 1,
                mem::size_of::<AdInplaceRec>(),
            )
        } {
            return false;
        }
        unsafe {
            let r = &mut *(*p).inplace.add((*p).ninplace as usize);
            r.line = line;
            r.export_name = f[1];
            r.argpos = num as c_int;
            r.offset = off as u32;
            r.sig = f[4];
            (*p).ninplace += 1;
        }
        return true;
    }
    if unsafe {
        libc::strcmp(kind, b"ocerz-apidb\0".as_ptr().cast()) == 0
            || libc::strcmp(kind, b"library\0".as_ptr().cast()) == 0
            || libc::strcmp(kind, b"sdk\0".as_ptr().cast()) == 0
    } {
        ad_refuse!(
            unsafe { &*p },
            line,
            "a %s record may appear only once, in the header",
            kind
        );
        return false;
    }
    ad_refuse!(
        unsafe { &*p },
        line,
        "%s is not a record kind; want fn, data, var, special, stub, shape, struct or inplace",
        kind
    );
    false
}

unsafe fn ad_resolve_inplace(p: *mut AdParse) -> bool {
    for i in 0..unsafe { (*p).ninplace } {
        let r = unsafe { &*(*p).inplace.add(i as usize) };
        let k = unsafe { ad_hash_find((*p).lib, (*p).entries, r.export_name) };
        if k < 0 {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "inplace names %s, which no fn record declares",
                r.export_name
            );
            return false;
        }
        let e = unsafe { &mut *(*p).entries.add(k as usize) };
        if e.kind != ffi::OCERZ_API_FN {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "inplace names %s, which is a %s record on line %d, not a fn record",
                r.export_name,
                ad_kind_name(e.kind),
                *(*p).entry_line.add(k as usize)
            );
            return false;
        }
        let mut sig = mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
        if unsafe { ffi::ocerz_abi_parse(e.sig, sig.as_mut_ptr()) } != 0 {
            return false;
        }
        let sig = unsafe { sig.assume_init() };
        if r.argpos >= sig.nargs
            || *sig.arg.as_ptr().add(r.argpos as usize) != b'p' as c_char
        {
            ad_refuse!(unsafe{&*p},r.line,"inplace binds argument %d of %s, which its signature %s does not declare as a pointer",r.argpos,r.export_name,e.sig);
            return false;
        }
        for j in 0..e.ninplace {
            let x = &*e.inplace.as_ptr().add(j as usize);
            if x.argpos == r.argpos && x.offset == r.offset {
                ad_refuse!(
                    unsafe { &*p },
                    r.line,
                    "offset %u of argument %d of %s is already converted in place",
                    r.offset,
                    r.argpos,
                    r.export_name
                );
                return false;
            }
        }
        for j in 0..e.nstructs {
            if (*e).structs.as_ptr().add(j as usize).read().argpos == r.argpos {
                ad_refuse!(unsafe{&*p},r.line,"argument %d of %s is bound to a shape, which copies it, and cannot also be converted in place",r.argpos,r.export_name);
                return false;
            }
        }
        if e.ninplace >= ffi::OCERZ_APIDB_INPLACE as c_int {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "%s already has the %d inplace records one fn may have",
                r.export_name,
                ffi::OCERZ_APIDB_INPLACE as c_int
            );
            return false;
        }
        *e.inplace.as_mut_ptr().add(e.ninplace as usize) = ffi::OcerzApiInplace {
            argpos: r.argpos,
            offset: r.offset,
            sig: r.sig,
        };
        e.ninplace += 1;
    }
    true
}

unsafe fn ad_resolve_structs(p: *mut AdParse) -> bool {
    for i in 0..unsafe { (*p).nstructs } {
        let r = unsafe { &*(*p).structs.add(i as usize) };
        let k = unsafe { ad_hash_find((*p).lib, (*p).entries, r.export_name) };
        if k < 0 {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "struct names %s, which no fn record declares",
                r.export_name
            );
            return false;
        }
        let e = unsafe { &mut *(*p).entries.add(k as usize) };
        if e.kind != ffi::OCERZ_API_FN {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "struct names %s, which is a %s record on line %d, not a fn record",
                r.export_name,
                ad_kind_name(e.kind),
                *(*p).entry_line.add(k as usize)
            );
            return false;
        }
        let mut shaped = false;
        for s in 0..unsafe { (*p).nshapes } {
            if libc::strcmp((*(*p).shapes.add(s as usize)).name, r.shape) == 0 {
                shaped = true;
                break;
            }
        }
        if !shaped {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "struct names shape %s, which no shape record declares",
                r.shape
            );
            return false;
        }
        let mut sig = mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
        if ffi::ocerz_abi_parse(e.sig, sig.as_mut_ptr()) != 0 {
            return false;
        }
        let sig = sig.assume_init();
        if r.argpos >= sig.nargs
            || *sig.arg.as_ptr().add(r.argpos as usize) != b'p' as c_char
        {
            ad_refuse!(unsafe{&*p},r.line,"struct binds argument %d of %s, which its signature %s does not declare as a pointer",r.argpos,r.export_name,e.sig);
            return false;
        }
        for j in 0..i {
            let q = &*(*p).structs.add(j as usize);
            if q.argpos == r.argpos && libc::strcmp(q.export_name, r.export_name) == 0 {
                ad_refuse!(
                    unsafe { &*p },
                    r.line,
                    "argument %d of %s is already bound to a shape on line %d",
                    r.argpos,
                    r.export_name,
                    q.line
                );
                return false;
            }
        }
        if e.nstructs >= ffi::OCERZ_APIDB_STRUCT_ARGS as c_int {
            ad_refuse!(
                unsafe { &*p },
                r.line,
                "%s already has the %d struct records one fn may have",
                r.export_name,
                ffi::OCERZ_APIDB_STRUCT_ARGS as c_int
            );
            return false;
        }
        *e.structs.as_mut_ptr().add(e.nstructs as usize) = ffi::OcerzApiStructArg {
            argpos: r.argpos,
            shape: r.shape,
        };
        e.nstructs += 1;
    }
    true
}

unsafe fn ad_free_parse(p: *mut AdParse) {
    let p = &mut *p;
    libc::free(p.entries.cast());
    libc::free(p.entry_line.cast());
    libc::free(p.shapes.cast());
    libc::free(p.shape_line.cast());
    libc::free(p.structs.cast());
    libc::free(p.inplace.cast());
    if !p.lib.is_null() {
        libc::free((*p.lib).hash.cast());
        libc::free((*p.lib).text.cast());
        libc::free((*p.lib).path.cast());
        libc::free(p.lib.cast());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_parse(
    path: *const c_char,
    text: *const c_char,
    len: usize,
    err: *mut c_char,
    errlen: usize,
) -> *const ffi::OcerzApiLibrary {
    let mut p = AdParse {
        path: if path.is_null() {
            b"(unnamed)\0".as_ptr().cast()
        } else {
            path
        },
        err,
        errlen,
        lib: ptr::null_mut(),
        entries: ptr::null_mut(),
        entry_line: ptr::null_mut(),
        nentries: 0,
        centries: 0,
        shapes: ptr::null_mut(),
        shape_line: ptr::null_mut(),
        nshapes: 0,
        cshapes: 0,
        structs: ptr::null_mut(),
        nstructs: 0,
        cstructs: 0,
        inplace: ptr::null_mut(),
        ninplace: 0,
        cinplace: 0,
    };
    if !err.is_null() && errlen != 0 {
        *err = 0;
    }
    p.lib = libc::calloc(1, mem::size_of::<AdLibrary>()) as *mut AdLibrary;
    let buf = libc::malloc(len.wrapping_add(1)) as *mut c_char;
    let pathcopy = libc::strdup(p.path);
    if p.lib.is_null() || buf.is_null() || pathcopy.is_null() || text.is_null() && len != 0 {
        libc::free(buf.cast());
        libc::free(pathcopy.cast());
        ad_refuse!(
            &p,
            0,
            "%s",
            if text.is_null() && len != 0 {
                b"no text\0".as_ptr() as *const c_char
            } else {
                b"out of memory\0".as_ptr() as *const c_char
            }
        );
        ad_free_parse(&mut p);
        return ptr::null();
    }
    if len != 0 {
        ptr::copy_nonoverlapping(text, buf, len);
    }
    *buf.add(len) = 0;
    (*p.lib).text = buf;
    (*p.lib).path = pathcopy;
    let mut stage = 0;
    let mut line = 0;
    let mut pos = 0;
    while pos < len {
        line += 1;
        let s = buf.add(pos);
        let nl = libc::memchr(s.cast(), b'\n' as c_int, len - pos) as *mut c_char;
        let ll = if nl.is_null() {
            len - pos
        } else {
            nl.offset_from(s) as usize
        };
        pos = pos.wrapping_add(ll).wrapping_add(if nl.is_null() { 0 } else { 1 });
        if !libc::memchr(s.cast(), 0, ll).is_null() {
            ad_refuse!(&p, line, "the line contains a NUL byte");
            ad_free_parse(&mut p);
            return ptr::null();
        }
        *s.add(ll) = 0;
        if ll == 0 || *s == b'#' as c_char {
            continue;
        }
        for i in 0..ll {
            let c = *s.add(i) as u8;
            if c < 0x20 || c == 0x7f {
                ad_refuse!(&p,line,"the line contains the control character 0x%02x; fields are separated by single spaces",c as c_uint);
                ad_free_parse(&mut p);
                return ptr::null();
            }
        }
        if *s == b' ' as c_char {
            ad_refuse!(&p, line, "the line starts with a space");
            ad_free_parse(&mut p);
            return ptr::null();
        }
        if *s.add(ll - 1) == b' ' as c_char {
            ad_refuse!(
                &p,
                line,
                "the line ends with a space, and nothing may follow the last field"
            );
            ad_free_parse(&mut p);
            return ptr::null();
        }
        if !libc::strstr(s, b"  \0".as_ptr().cast()).is_null() {
            ad_refuse!(&p, line, "two fields are separated by more than one space");
            ad_free_parse(&mut p);
            return ptr::null();
        }
        let mut f = [ptr::null_mut(); AD_FIELDS_MAX];
        let mut nf = 0;
        let mut c = s;
        loop {
            if nf < AD_FIELDS_MAX {
                f[nf] = c;
            }
            nf += 1;
            let sp = libc::strchr(c, b' ' as c_int);
            if sp.is_null() {
                break;
            }
            *sp = 0;
            c = sp.add(1);
        }
        if nf > AD_FIELDS_MAX {
            if libc::strcmp(f[0], b"shape\0".as_ptr().cast()) == 0 {
                ad_refuse!(
                    &p,
                    line,
                    "a shape record has %d fields, and a shape may have at most %d words",
                    nf as c_int,
                    ffi::OCERZ_APIDB_SHAPE_WORDS as c_int
                );
            } else {
                ad_refuse!(
                    &p,
                    line,
                    "a %s record has %d fields, far more than any record has",
                    f[0],
                    nf as c_int
                );
            }
            ad_free_parse(&mut p);
            return ptr::null();
        }
        if stage == 0 {
            if libc::strcmp(f[0], b"ocerz-apidb\0".as_ptr().cast()) != 0 || nf != 2 {
                ad_refuse!(
                    &p,
                    line,
                    "the first record is not the header 'ocerz-apidb 1'"
                );
                ad_free_parse(&mut p);
                return ptr::null();
            }
            if libc::strcmp(f[1], b"1\0".as_ptr().cast()) != 0 {
                ad_refuse!(
                    &p,
                    line,
                    "the header declares format %s, and this ocerz reads format 1",
                    f[1]
                );
                ad_free_parse(&mut p);
                return ptr::null();
            }
            stage = 1;
        } else if stage == 1 {
            if libc::strcmp(f[0], b"library\0".as_ptr().cast()) != 0 || nf != 2 {
                ad_refuse!(
                    &p,
                    line,
                    "the second record is not 'library <install-name>'"
                );
                ad_free_parse(&mut p);
                return ptr::null();
            }
            (*p.lib).pub_.install_name = f[1];
            stage = 2;
        } else if stage == 2 {
            let mut packed = 0;
            if libc::strcmp(f[0], b"sdk\0".as_ptr().cast()) != 0
                || nf != 3
                || libc::strcmp(f[1], b"macos\0".as_ptr().cast()) != 0
            {
                ad_refuse!(&p, line, "the third record is not 'sdk macos <version>'");
                ad_free_parse(&mut p);
                return ptr::null();
            }
            if !ad_version(f[2], &mut packed) {
                ad_refuse!(
                    &p,
                    line,
                    "the sdk version %s is not X, X.Y or X.Y.Z in decimal",
                    f[2]
                );
                ad_free_parse(&mut p);
                return ptr::null();
            }
            (*p.lib).pub_.sdk_version = f[2];
            stage = 3;
        } else if !ad_record(&mut p, line, &f, nf as c_int) {
            ad_free_parse(&mut p);
            return ptr::null();
        }
    }
    if stage < 3 {
        ad_refuse!(
            &p,
            if line > 0 { line } else { 1 },
            "the file ends before its %s record",
            if stage == 0 {
                b"ocerz-apidb header\0".as_ptr() as *const c_char
            } else if stage == 1 {
                b"library\0".as_ptr() as *const c_char
            } else {
                b"sdk\0".as_ptr() as *const c_char
            }
        );
        ad_free_parse(&mut p);
        return ptr::null();
    }
    if !ad_resolve_structs(&mut p) || !ad_resolve_inplace(&mut p) {
        ad_free_parse(&mut p);
        return ptr::null();
    }
    (*p.lib).pub_.path = (*p.lib).path;
    (*p.lib).pub_.entries = p.entries;
    (*p.lib).pub_.nentries = p.nentries;
    (*p.lib).pub_.shapes = p.shapes;
    (*p.lib).pub_.nshapes = p.nshapes;
    libc::free(p.entry_line.cast());
    libc::free(p.shape_line.cast());
    libc::free(p.structs.cast());
    libc::free(p.inplace.cast());
    &(*p.lib).pub_
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_find(
    lib: *const ffi::OcerzApiLibrary,
    export_name: *const c_char,
) -> *const ffi::OcerzApiEntry {
    if lib.is_null() || export_name.is_null() {
        return ptr::null();
    }
    let al = lib.cast::<AdLibrary>();
    let k = ad_hash_find(al, (*lib).entries, export_name);
    if k < 0 {
        ptr::null()
    } else {
        (*lib).entries.add(k as usize)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_shape(
    lib: *const ffi::OcerzApiLibrary,
    name: *const c_char,
    version: u64,
) -> *const ffi::OcerzApiShape {
    if lib.is_null() || name.is_null() {
        return ptr::null();
    }
    for i in 0..(*lib).nshapes {
        let s = &*(*lib).shapes.add(i as usize);
        if s.version == version && libc::strcmp(s.name, name) == 0 {
            return s;
        }
    }
    ptr::null()
}

#[repr(C)]
struct AdSlot {
    install_name: *mut c_char,
    lib: *const ffi::OcerzApiLibrary,
    next: *mut AdSlot,
}
static G_AD_SLOTS: AtomicPtr<AdSlot> = AtomicPtr::new(ptr::null_mut());
static G_AD_CHOSEN: AtomicI32 = AtomicI32::new(0);
static mut G_AD_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_AD_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;
static mut G_AD_MINOS: u32 = 0;
static mut G_AD_DIR: [c_char; PATH_MAX] = [0; PATH_MAX];
static mut G_AD_NAMES: *mut *mut c_char = ptr::null_mut();
static mut G_AD_NAMES_N: c_int = 0;

unsafe fn ad_lock() {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_AD_LOCK));
}
unsafe fn ad_unlock() {
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_AD_LOCK));
}
extern "C" fn ad_preload_once() {
    unsafe {
        let mut n = 0;
        let names = ocerz_apidb_install_names(&mut n);
        for i in 0..n {
            ocerz_apidb_library(*names.add(i as usize));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_preload() {
    libc::pthread_once(ptr::addr_of_mut!(G_AD_ONCE), Some(ad_preload_once));
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_postfork_child() {
    ptr::write(
        ptr::addr_of_mut!(G_AD_LOCK),
        libc::PTHREAD_MUTEX_INITIALIZER,
    );
}

unsafe fn ad_default_root(out: *mut c_char, outlen: usize) -> bool {
    let mut small = [0 as c_char; PATH_MAX];
    let mut exe = small.as_mut_ptr();
    let mut sz = PATH_MAX as u32;
    if libc::_NSGetExecutablePath(exe, &mut sz) != 0 {
        exe = libc::malloc(sz as usize) as *mut c_char;
        if exe.is_null() || libc::_NSGetExecutablePath(exe, &mut sz) != 0 {
            libc::free(exe.cast());
            return false;
        }
    }
    let mut real = [0 as c_char; PATH_MAX];
    let ok = !libc::realpath(exe, real.as_mut_ptr()).is_null();
    if exe != small.as_mut_ptr() {
        libc::free(exe.cast());
    }
    if !ok {
        return false;
    }
    let slash = libc::strrchr(real.as_mut_ptr(), b'/' as c_int);
    if slash.is_null() {
        return false;
    }
    *slash = 0;
    let n = libc::snprintf(
        out,
        outlen,
        b"%s/runtime/apis\0".as_ptr().cast(),
        if real[0] == 0 {
            b"/\0".as_ptr().cast()
        } else {
            real.as_ptr()
        },
    );
    n > 0 && (n as usize) < outlen
}

unsafe fn ad_choose_locked() {
    if G_AD_CHOSEN.load(Ordering::SeqCst) != 0 {
        return;
    }
    *ptr::addr_of_mut!(G_AD_DIR) = [0; PATH_MAX];
    let mut root = [0 as c_char; PATH_MAX];
    let env = libc::getenv(b"OCERZ_APIDB\0".as_ptr().cast());
    let have = if !env.is_null() && *env != 0 {
        let n = libc::snprintf(root.as_mut_ptr(), PATH_MAX, b"%s\0".as_ptr().cast(), env);
        n > 0 && (n as usize) < PATH_MAX
    } else {
        ad_default_root(root.as_mut_ptr(), PATH_MAX)
    };
    let mut rr = [0 as c_char; PATH_MAX];
    let mut mac = [0 as c_char; PATH_MAX];
    let mut d = ptr::null_mut();
    if have && !libc::realpath(root.as_ptr(), rr.as_mut_ptr()).is_null() {
        let n = libc::snprintf(
            mac.as_mut_ptr(),
            PATH_MAX,
            b"%s/macos\0".as_ptr().cast(),
            rr.as_ptr(),
        );
        if n > 0 && (n as usize) < PATH_MAX {
            d = libc::opendir(mac.as_ptr());
        }
    }
    if d.is_null() {
        crate::ocerz_log!(
            "apidb: no API database at %s/macos\n",
            if have {
                root.as_ptr()
            } else {
                b"(no root)\0".as_ptr().cast()
            }
        );
        G_AD_CHOSEN.store(1, Ordering::SeqCst);
        return;
    }
    let minos = *ptr::addr_of!(G_AD_MINOS);
    let mut best = [0 as c_char; 256];
    let mut oldest = [0 as c_char; 256];
    let mut bv = 0;
    let mut ov = 0;
    let mut hb = false;
    let mut ho = false;
    loop {
        let de = libc::readdir(d);
        if de.is_null() {
            break;
        }
        let name = (*de).d_name.as_ptr();
        let l = libc::strlen(name);
        let mut v = 0;
        if *name == b'.' as c_char || l >= 256 || !ad_version(name, &mut v) {
            continue;
        }
        let mut full = [0 as c_char; PATH_MAX];
        let mut st = mem::zeroed();
        let n = libc::snprintf(
            full.as_mut_ptr(),
            PATH_MAX,
            b"%s/%s\0".as_ptr().cast(),
            mac.as_ptr(),
            name,
        );
        if n <= 0
            || (n as usize) >= PATH_MAX
            || libc::stat(full.as_ptr(), &mut st) != 0
            || (st.st_mode & libc::S_IFMT) != libc::S_IFDIR
        {
            continue;
        }
        if !ho || v < ov || (v == ov && libc::strcmp(name, oldest.as_ptr()) < 0) {
            libc::snprintf(oldest.as_mut_ptr(), 256, b"%s\0".as_ptr().cast(), name);
            ov = v;
            ho = true;
        }
        if minos != 0 && v > minos {
            continue;
        }
        if !hb || v > bv || (v == bv && libc::strcmp(name, best.as_ptr()) < 0) {
            libc::snprintf(best.as_mut_ptr(), 256, b"%s\0".as_ptr().cast(), name);
            bv = v;
            hb = true;
        }
    }
    libc::closedir(d);
    let pick = if hb {
        best.as_ptr()
    } else if ho {
        oldest.as_ptr()
    } else {
        ptr::null()
    };
    if !pick.is_null() {
        let gd = ptr::addr_of_mut!(G_AD_DIR) as *mut c_char;
        let n = libc::snprintf(gd, PATH_MAX, b"%s/%s\0".as_ptr().cast(), mac.as_ptr(), pick);
        if n <= 0 || (n as usize) >= PATH_MAX {
            *gd = 0;
        }
    }
    let gd = ptr::addr_of!(G_AD_DIR) as *const c_char;
    if *gd != 0 {
        crate::ocerz_log!(
            "apidb: using %s for a guest declaring macOS %u.%u.%u\n",
            gd,
            minos >> 16,
            (minos >> 8) & 0xff,
            minos & 0xff
        );
    } else {
        crate::ocerz_log!("apidb: %s holds no version directory\n", mac.as_ptr());
    }
    G_AD_CHOSEN.store(1, Ordering::SeqCst);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_dir() -> *const c_char {
    if G_AD_CHOSEN.load(Ordering::SeqCst) == 0 {
        ad_lock();
        ad_choose_locked();
        ad_unlock();
    }
    let gd = ptr::addr_of!(G_AD_DIR) as *const c_char;
    if *gd != 0 {
        gd
    } else {
        ptr::null()
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_set_minos(minos: u32) {
    ad_lock();
    if G_AD_CHOSEN.load(Ordering::SeqCst) == 0 {
        *ptr::addr_of_mut!(G_AD_MINOS) = minos;
    } else if minos != *ptr::addr_of!(G_AD_MINOS) {
        crate::ocerz_log!("apidb: minimum macOS %#x arrived after the version directory was chosen, which stands\n",minos);
    }
    ad_unlock();
}

unsafe fn ad_read_file(
    path: *const c_char,
    len_out: *mut usize,
    missing: *mut c_int,
) -> *mut c_char {
    *missing = 0;
    let fd = libc::open(path, libc::O_RDONLY | libc::O_CLOEXEC);
    if fd < 0 {
        let e = *libc::__error();
        *missing = (e == libc::ENOENT || e == libc::ENOTDIR) as c_int;
        return ptr::null_mut();
    }
    let mut st = mem::zeroed::<libc::stat>();
    if libc::fstat(fd, &mut st) != 0 || (st.st_mode & libc::S_IFMT) != libc::S_IFREG {
        libc::close(fd);
        return ptr::null_mut();
    }
    let mut cap = (st.st_size as usize).wrapping_add(1);
    let mut len = 0;
    let mut buf = libc::malloc(cap) as *mut c_char;
    while !buf.is_null() {
        if len == cap {
            let nb = libc::realloc(buf.cast(), cap.wrapping_mul(2)) as *mut c_char;
            if nb.is_null() {
                libc::free(buf.cast());
                buf = ptr::null_mut();
                break;
            }
            buf = nb;
            cap = cap.wrapping_mul(2);
        }
        let r = libc::read(fd, buf.add(len).cast(), cap - len);
        if r < 0 && *libc::__error() == libc::EINTR {
            continue;
        }
        if r < 0 {
            libc::free(buf.cast());
            buf = ptr::null_mut();
            break;
        }
        if r == 0 {
            break;
        }
        len = len.wrapping_add(r as usize);
    }
    libc::close(fd);
    *len_out = len;
    buf
}

unsafe fn ad_leaf(name: *const c_char) -> *const c_char {
    let x = libc::strrchr(name, b'/' as c_int);
    if x.is_null() {
        name
    } else {
        x.add(1)
    }
}
unsafe fn ad_find_leaf_locked(name: *const c_char) -> *mut AdSlot {
    let leaf = ad_leaf(name);
    let mut s = G_AD_SLOTS.load(Ordering::SeqCst);
    while !s.is_null() {
        if libc::strcmp(ad_leaf((*s).install_name), leaf) == 0 {
            return s;
        }
        s = (*s).next;
    }
    ptr::null_mut()
}
unsafe fn ad_publish_locked(name: *const c_char, lib: *const ffi::OcerzApiLibrary) -> *mut AdSlot {
    let s = libc::calloc(1, mem::size_of::<AdSlot>()) as *mut AdSlot;
    let n = libc::strdup(name);
    if s.is_null() || n.is_null() {
        libc::free(s.cast());
        libc::free(n.cast());
        return ptr::null_mut();
    }
    (*s).install_name = n;
    (*s).lib = lib;
    (*s).next = G_AD_SLOTS.load(Ordering::SeqCst);
    G_AD_SLOTS.store(s, Ordering::SeqCst);
    s
}

unsafe fn ad_load_locked(name: *const c_char) -> *const ffi::OcerzApiLibrary {
    ad_choose_locked();
    let gd = ptr::addr_of!(G_AD_DIR) as *const c_char;
    if *gd == 0 {
        return ptr::null();
    }
    let leaf = ad_leaf(name);
    if *leaf == 0 {
        return ptr::null();
    }
    let mut path = [0 as c_char; PATH_MAX];
    let n = libc::snprintf(
        path.as_mut_ptr(),
        PATH_MAX,
        b"%s/%s.api\0".as_ptr().cast(),
        gd,
        leaf,
    );
    if n <= 0 || (n as usize) >= PATH_MAX {
        return ptr::null();
    }
    let mut len = 0;
    let mut missing = 0;
    let text = ad_read_file(path.as_ptr(), &mut len, &mut missing);
    if text.is_null() {
        let why = *libc::__error();
        if missing == 0 {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: apidb: cannot read %s: %s\n\0".as_ptr().cast(),
                path.as_ptr(),
                libc::strerror(why),
            );
        }
        return ptr::null();
    }
    let mut err = [0 as c_char; 512];
    let lib = ocerz_apidb_parse(path.as_ptr(), text, len, err.as_mut_ptr(), 512);
    libc::free(text.cast());
    if lib.is_null() {
        libc::fprintf(
            crate::log::stderr(),
            b"ocerz: apidb: refusing %s\n\0".as_ptr().cast(),
            err.as_ptr(),
        );
        return ptr::null();
    }
    if libc::strcmp((*lib).install_name, name) != 0 {
        crate::ocerz_log!(
            "apidb: %s describes %s, not %s\n",
            path.as_ptr(),
            (*lib).install_name,
            name
        );
        ad_publish_locked((*lib).install_name, lib);
        return ptr::null();
    }
    crate::ocerz_log!(
        "apidb: loaded %s, %d exports and %d shapes\n",
        path.as_ptr(),
        (*lib).nentries,
        (*lib).nshapes
    );
    lib
}
unsafe fn ad_find_slot(name: *const c_char) -> *mut AdSlot {
    let mut s = G_AD_SLOTS.load(Ordering::SeqCst);
    while !s.is_null() {
        if libc::strcmp((*s).install_name, name) == 0 {
            return s;
        }
        s = (*s).next;
    }
    ptr::null_mut()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_library(name: *const c_char) -> *const ffi::OcerzApiLibrary {
    if name.is_null() || *name == 0 {
        return ptr::null();
    }
    let mut s = ad_find_slot(name);
    if !s.is_null() {
        return (*s).lib;
    }
    ad_lock();
    s = ad_find_slot(name);
    if s.is_null() {
        let lib = if ad_find_leaf_locked(name).is_null() {
            ad_load_locked(name)
        } else {
            ptr::null()
        };
        s = ad_publish_locked(name, lib);
        if s.is_null() {
            ad_unlock();
            return lib;
        }
    }
    ad_unlock();
    (*s).lib
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_apidb_install_names(count: *mut c_int) -> *const *const c_char {
    if !count.is_null() {
        *count = 0;
    }
    ad_lock();
    ad_choose_locked();
    let names_ptr = ptr::addr_of_mut!(G_AD_NAMES);
    if (*names_ptr).is_null() && *(ptr::addr_of!(G_AD_DIR) as *const c_char) != 0 {
        let d = libc::opendir(ptr::addr_of!(G_AD_DIR) as *const c_char);
        if !d.is_null() {
            let mut names: *mut *mut c_char = ptr::null_mut();
            let mut cap: c_int = 0;
            let mut n = 0;
            loop {
                let de = libc::readdir(d);
                if de.is_null() {
                    break;
                }
                let dn = (*de).d_name.as_ptr();
                let l = libc::strlen(dn);
                if l < 5 || libc::strcmp(dn.add(l - 4), b".api\0".as_ptr().cast()) != 0 {
                    continue;
                }
                let mut path = [0 as c_char; PATH_MAX];
                let m = libc::snprintf(
                    path.as_mut_ptr(),
                    PATH_MAX,
                    b"%s/%s\0".as_ptr().cast(),
                    ptr::addr_of!(G_AD_DIR) as *const c_char,
                    dn,
                );
                if m <= 0 || (m as usize) >= PATH_MAX {
                    continue;
                }
                let mut len = 0;
                let mut missing = 0;
                let text = ad_read_file(path.as_ptr(), &mut len, &mut missing);
                if text.is_null() {
                    continue;
                }
                let mut line = text;
                let end = text.add(len);
                let mut rec = 0;
                let mut first = ptr::null_mut();
                let mut second = ptr::null_mut();
                let mut send = end;
                while line < end && rec < 2 {
                    let q =
                        libc::memchr(line.cast(), b'\n' as c_int, end.offset_from(line) as usize)
                            as *mut c_char;
                    let lend = if q.is_null() { end } else { q };
                    if lend != line && *line != b'#' as c_char {
                        rec += 1;
                        if rec == 1 {
                            first = line;
                        } else {
                            second = line;
                            send = lend;
                        }
                    }
                    line = if lend < end { lend.add(1) } else { end };
                }
                if rec < 2
                    || first.is_null()
                    || second.is_null()
                    || (send.offset_from(second) as usize) < 8
                    || libc::memcmp(
                        first as *const libc::c_void,
                        b"ocerz-apidb 1\0".as_ptr().cast(),
                        13,
                    ) != 0
                    || libc::memcmp(
                        second as *const libc::c_void,
                        b"library \0".as_ptr().cast(),
                        8,
                    ) != 0
                {
                    libc::free(text.cast());
                    continue;
                }
                let ilen = send.offset_from(second) as usize - 8;
                let name = libc::malloc(ilen.wrapping_add(1)) as *mut c_char;
                if !name.is_null() {
                    ptr::copy_nonoverlapping(second.add(8), name, ilen);
                    *name.add(ilen) = 0;
                    if n >= cap {
                        let nc = if cap != 0 { cap.wrapping_mul(2) } else { 32 };
                        let nn = libc::realloc(
                            names.cast(),
                            (nc as usize).wrapping_mul(mem::size_of::<*mut c_char>()),
                        ) as *mut *mut c_char;
                        if nn.is_null() {
                            libc::free(name.cast());
                            libc::free(text.cast());
                            break;
                        }
                        names = nn;
                        cap = nc;
                    }
                    *names.add(n as usize) = name;
                    n += 1;
                }
                libc::free(text.cast());
            }
            libc::closedir(d);
            *names_ptr = names;
            *ptr::addr_of_mut!(G_AD_NAMES_N) = n;
        }
    }
    if !count.is_null() {
        *count = *ptr::addr_of!(G_AD_NAMES_N);
    }
    let ret = *names_ptr as *const *const c_char;
    ad_unlock();
    ret
}
