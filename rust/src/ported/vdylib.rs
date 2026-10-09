//!
//! Building the synthesized x86_64 system libraries.
//!
//! ---- what an image is made of ----
//! Nothing about a library is compiled in here.  ocerz_apidb_library hands back
//! the parsed database file for an install name, and the builder walks its
//! entries in file order: a fn, special or stub record becomes a function stub,
//! a var record becomes a slot in __DATA filled by a filler this file knows by
//! name, and a data record becomes an absolute export naming the host's own
//! variable.  shape and struct records are the bridge's business and never reach
//! the image.  ocerz_vdylib_have is exactly the question whether native mode has
//! a database file for the install name, so a library is synthesized precisely
//! when there is a file describing it.
//!
//! ---- the file that is never a file ----
//! An image here is laid out exactly as a linker would lay it on disk: header,
//! load commands and stubs filling __TEXT from offset zero, then __DATA with the
//! jump slots and after them the data exports, then the export trie past the
//! end of both.  The loader never reopens anything - map_segments memcpys out
//! of DynImage.slice and the trie walker reads that same buffer - so the buffer
//! IS the file, and keeping the file layout honest is what lets the image flow
//! through mapping, binding, dlopen and dladdr with no special case anywhere.
//! __TEXT has to be the segment with fileoff 0 and a non-zero filesize, because
//! that pair is how map_segments picks the text segment out and derives the
//! image's base from it.  __TEXT starts at vmaddr zero and every segment is 4 KB
//! aligned in both file and memory, so a symbol's offset in the trie is also its
//! offset in the buffer, and the loader's slide is the whole of the image's load
//! address.
//!
//! Every size is computed from the entries.  __TEXT is as many pages as the
//! header, the load commands and one stub per function take, __DATA as many as
//! the jump slots and the var slots take and never less than one, and the trie
//! as many bytes as its nodes need, so a library of five thousand exports is
//! built the same way as one of fifty.  The only bound is the one the
//! formats impose: an image is addressed with 32-bit file offsets, and an export
//! id carries its entry index in twenty bits.
//!
//! __TEXT is emitted with read+execute in BOTH maxprot and initprot.  Only one
//! of the two is really consulted - protect_ro_segments reads the word at
//! segment offset 56, which is maxprot rather than the initprot its local is
//! named for - and a segment named __TEXT is re-protected only when that word
//! has read and execute and not write.  Writing 5 into both means the stubs end
//! up executable-not-writable whichever word is meant, instead of staying in
//! the writable state every image is mapped in.
//!
//! The trie deliberately sits outside every segment.  Nothing maps it into
//! guest memory and nothing needs to: the only reader is the host-side walker,
//! which indexes the host buffer by the LC_DYLD_EXPORTS_TRIE dataoff.  Giving
//! it a __LINKEDIT segment would only cost the guest address space.
//!
//! ---- why the trie's numbers are fixed width ----
//! Every child edge in an export trie carries the absolute offset of the child
//! node from the start of the trie, so a node's size depends on how wide its
//! children's offsets encode, and those offsets depend on the sizes of the
//! nodes before them.  Encoding offsets minimally therefore means iterating to
//! a fixed point that is not guaranteed to settle.  Instead every offset
//! and every terminal address is emitted as a padded ULEB128 of fixed width,
//! high bit set on every byte but the last: redundant padding is legal ULEB and
//! the walker decodes it correctly because it keeps shifting while the high bit
//! is set.  Offsets, and the terminals of stubs and data slots, are five bytes,
//! which carry 35 bits, more than a 32-bit file offset can reach.  A native
//! data terminal carries a host address instead, and the image's size puts no
//! bound on that: the host's libraries sit wherever the kernel mapped them, and
//! an address past 2^35 written into five bytes would lose its top bits without
//! a word of complaint.  Those terminals are ten bytes, which is every bit a
//! 64-bit value has, the tenth byte holding bit 63 alone.  With every width
//! known up front - a terminal's kind comes from the record, and no width
//! depends on the value written into it - a node's size is known before
//! anything is placed, so layout is one pass over the nodes and emission is a
//! second, with no back-patching at all.  The same choice fixes a terminal at
//! one flags byte plus its address bytes, so the terminal size a node declares
//! is 6 for a stub or a slot and 11 for native data, a single ULEB byte either
//! way, and an image's layout does not move whatever addresses the host hands
//! back.
//!
//! The trie is a real radix trie, not a flat list of full names.  The walker
//! takes the FIRST child whose edge matches under strncmp, so with a flat root
//! the edge _write would swallow a lookup of _writev, consume six characters
//! and dead-end on a childless node.  Children of a node are therefore keyed by
//! distinct first characters, a shared prefix becomes an edge of its own, and a
//! symbol that is a proper prefix of another - _read inside _readv and _readdir
//! - becomes an interior node that also carries a terminal.  An edge is a span
//! of the name it came from rather than a copy, so no name is too long to be an
//! edge, and a trie over n names has at most 2n + 1 nodes, which is what is
//! allocated for it.
//!
//! ---- why twelve bytes at a stride of sixteen ----
//! A stub is mov r11d, imm32 (six bytes) then jmp qword [rip + rel32] (six
//! more).  Twelve is all the instructions need, but they are placed every
//! sixteen so a stub's address is its index shifted, which keeps the trie
//! addresses, the jump displacements and the __DATA slot for a given export all
//! derivable from one number; the four bytes of slack are filled with int3 so a
//! stray branch into the padding stops instead of quietly executing the zeros
//! that would otherwise be there.  The jump is rip-relative and the slot it
//! reads holds a constant, the one address in the dyld-API trap window that
//! means "bridge", so nothing in a synthesized image is position-dependent and
//! the image needs no rebases, no binds and no chained fixups: __TEXT and
//! __DATA slide together, and a displacement between them computed at build
//! time is still correct at every load address.
//!
//! ---- the exports that are not functions ----
//! Not every name a program imports from libSystem is one it calls.  A
//! stack-protected function copies the value at ___stack_chk_guard onto its
//! frame in the prologue and compares the copy against it again before it
//! returns, and both are loads through the pointer the loader bound, not
//! calls.  clang turns the protector on by default, so almost every ordinary
//! program imports that symbol whether its author asked for it or not, and an
//! image that lacks it refuses to bind the program at all.
//!
//! So a var record is a name, a size and a filler, given a slot in __DATA after
//! the last jump slot, each slot starting on an 8-byte boundary, with __DATA's
//! size taking the slots in and the trie moving past them.  Its trie terminal
//! is the slot's offset from the image start, the same thing a function's
//! terminal is for its stub, so the trie walker and the loader cannot tell the
//! two kinds apart and neither needed a change: the guest's GOT entry is bound
//! to the slot's address and the guest reads through it.  A var export has no
//! stub and no export id, because nothing ever branches to it.  The filler is
//! named in the record and looked up in a table here, because what fills a slot
//! is code; a name the table does not have refuses the image rather than
//! leaving the slot zero, since a record that asked for a canary and got zeroes
//! would be quietly weaker than it says.
//!
//! ---- the canary ----
//! The stack_guard filler draws ___stack_chk_guard from arc4random_buf every
//! time an image is built.  It does not have to agree with the host's own guard
//! or with anything else: the only thing a guest ever compares it with is the
//! copy its own prologue took, and the handler behind ___stack_chk_fail stops
//! the run without comparing anything.  The loader builds a given install name
//! once, so a running guest only ever sees one value.
//!
//! ---- the default floating-point environments ----
//! The fe_dfl_env and fe_dfl_daz_env fillers write the sixteen bytes of x86's
//! _FE_DFL_ENV and _FE_DFL_DISABLE_SSE_DENORMS_ENV: control word 0x037f, status
//! 0, and MXCSR 0x1f80, or 0x9fc0 with flush-to-zero and denormals-are-zero,
//! the values x86 libm's own copies hold under Rosetta.  The host's are arm64
//! fenv_t objects, a different structure, and fesetenv here is ocerz's
//! (src/sysbridge.c), which reads the guest's copy.
//!
//! ---- GSS object identifiers ----
//! On x86 the GSS and Kerberos headers lay their structures out under
//! pack(2), so a gss_OID_desc, a 32-bit length and a pointer to the
//! identifier's bytes, is twelve bytes with the pointer at offset four, where
//! arm64 has sixteen with it at eight.  A program names the descriptors GSS
//! exports, __gss_krb5_mechanism_oid_desc and the rest, through macros such as
//! GSS_KRB5_MECHANISM, and Kerberos also exports pointers to them and to
//! descriptor sets.  The gss_oid_desc filler writes the x86 descriptor of the
//! host's one, its pointer leading to the host's bytes, which have no layout;
//! gss_oid_ptr writes a pointer to such a descriptor in guest memory, and
//! gss_oid_set_ptr one to a gss_OID_set_desc whose elements are an array of
//! them at x86's twelve-byte stride.  A converted copy is made once per host
//! descriptor and never freed, as the host's are constants.
//!
//! ---- the page size ----
//! The page_size, page_mask and page_shift fillers write 4096, 4095 and 12.
//! They stand behind vm_page_size and its relatives, which on this host hold
//! 16384: an Intel Mac has 4 KB pages, x86 programs are compiled knowing it, and
//! one that reads the host's value in one place and its own constant in another
//! fails its own consistency checks.  ocerz's memory layer already gives the
//! guest 4 KB pages over the host's.
//!
//! Its lowest byte is always zero.  x86 is little-endian and the copy sits above
//! the locals it guards, so the lowest byte is the first one an overrun climbing
//! out of a buffer reaches.  A string copy can write a zero only as its
//! terminator, so it cannot rewrite that byte and carry on to the saved frame
//! pointer and return address with the guard still matching; and a string read
//! that runs off the end of an unterminated buffer, which is how a guard usually
//! leaks into output, stops there before it has shown any of the other seven.
//! The host's own guard zeroes its second byte instead, which stops a copy just
//! as well, gives up one byte to a read, and in exchange notices an off-by-one
//! that writes nothing but a terminator; either is sound, and nothing ever
//! compares the two.  The other seven bytes are drawn again in the rare case
//! they all come back zero, since an all-zero guard is reproduced exactly by
//! any overrun that writes zeroes.
//!
//! ---- the data symbols that must not be slots ----
//! libSystem has other data that compiled programs reach for directly: _environ,
//! ___progname, __DefaultRuneLocale, and ___stdoutp and ___stderrp, which are
//! what stdout and stderr expand to.  None of them can be a var record, because
//! each is a variable native code also reads or writes on its own account, and
//! a guest slot beside it is a second copy that goes stale the first time either
//! side writes.  _environ is what native getenv, setenv and execvp walk, and it
//! points at the host's environment, not at the one on the guest's initial
//! stack.  ___progname is what native getprogname, err and warn print, and ocerz
//! points it at the last component of the guest's argv[0] before the guest runs,
//! as it does NXArgv and NXArgc (bridge.c).  ___stdoutp and ___stderrp are the
//! streams native printf, puts
//! and perror write through; a program that assigns stdout expects printf to
//! follow it, and a slot holding anything but the native stream hands native
//! stdio a FILE it never opened.  __DefaultRuneLocale is a 3208-byte table that
//! carries pointers into host memory and has to agree with native __maskrune,
//! so a copy is wrong on both counts, and a zeroed one silently answers false to
//! every ctype question.  A database that does not export one of them leaves a
//! program importing it failing to bind and saying which name it wanted, and a
//! refusal that names itself is better than a value that is wrong without
//! saying so.
//!
//! ---- the exports that are the host's own variables ----
//! CoreFoundation exports data a guest reaches by address, and the address is
//! the part that matters.  A CFSTR literal is a structure the compiler lays down
//! in the guest's own __cfstring section, and its first word is bound to
//! ___CFConstantStringClassReference: that word is the isa by which native
//! CoreFoundation and the Objective-C runtime recognize the literal as a string
//! at all.  kCFTypeArrayCallBacks and its dictionary siblings are handed over by
//! address, and CoreFoundation may compare the pointer it is given against the
//! address of its own.  A slot here holding a copy of either is a second object
//! at a second address: an isa naming a class nobody registered, and a callbacks
//! structure whose address nothing recognizes.  Some such names would survive a
//! copy - kCFBooleanTrue and kCFRunLoopDefaultMode are constant pointers to
//! objects that never move - but one rule for every native name is simpler than
//! deciding name by name, and costs nothing.
//!
//! So a data record's trie terminal is the host variable's own address with
//! EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE set.  The address is what the host lookup,
//! ocerz_bridge_host_symbol unless the caller supplies another, answers for the
//! record's host symbol in the library whose install name the file names.  The
//! trie walker returns an absolute terminal's value as it stands instead of
//! adding the load address, so every bind path that asks it - bind opcodes and
//! chained fixups alike - writes the native address into the guest's GOT entry
//! or isa word.  That address is usable from guest code only because the loader
//! asks for a virtual image in native mode alone, and native mode runs in the
//! identity map, where a host address is a guest address.  A native export
//! takes no stub, no slot, no export id and no byte of either segment: its trie
//! entry is all of it.
//!
//! A name the host lookup does not find is left out of the trie, and said once
//! per process through OCERZ_LOG however many times the image is built.  An
//! absolute export of zero would bind the guest's reference to a null pointer
//! that faults somewhere far from the import that caused it; an export that is
//! absent puts the name in the loader's unresolved-import report, and the run
//! stops with 71 before the guest has executed an instruction.
//!
//! ---- export ids ----
//! An export id is the library's ordinal in its top twelve bits and the entry's
//! index in its file in the low twenty.  The ordinal is the position of the
//! library's file among the .api files of the version directory in use, sorted
//! by name, read once per process.  That makes an id a function of the database
//! alone: numbering libraries as they were first loaded would number them by
//! whichever one a guest happened to name first - a program linking
//! CoreFoundation lists it ahead of libSystem - so the same libSystem would
//! carry different ids, and be different bytes, from one guest to the next.
//! Numbered by the directory, libSystem's stubs are the same in every process
//! that reads the same database, and an id means the same export in every run.
//! Indices of var and data entries are simply never minted, since neither has a
//! stub.
//!
//! ---- what happens after the trap ----
//! The trap lands in ocerz_vdylib_dispatch, which splits the export id into the
//! library and the entry it was minted from and asks the bridge whether it
//! knows how to perform that call for real.  If it does, the call is made there
//! and the guest goes on with the result in the registers x86 code expects.  If
//! it does not - a stub record, or a fn whose host symbol will not resolve - the
//! export names itself and the run stops with 72.
//!
//! The answer is cached in the library's own table, one atomic word per entry,
//! with a marker for an answer of no.  The bridge resolves by name, and this
//! dispatch is on the path of every call a guest makes into a virtual library,
//! so resolving per call would put a hash lookup in front of memcpy - a cost
//! that hides in a microbenchmark and does not hide in a program that calls
//! memcpy a million times.  Two threads that race to fill the same word store
//! the same pointer, because the bridge makes each descriptor once.
//!
//! Translated code reaches the same dispatch without the trap, through
//! ocerz_vdylib_fastcall.  What it has to decide on top is whether translated
//! code may carry on as though a function had returned.  It notes rsp and the
//! JIT's retirement count, performs the dispatch, and answers zero only if the
//! result is an ordinary step, rip is the word just below the new rsp and rsp
//! rose by eight or, for the r11-keeping stubs, sixteen, nothing asks the thread
//! to stop or to interpret its next instruction, and no translation was retired
//! in the meantime.  Everything else, a delivered signal among it, goes back
//! through the dispatcher exactly as the trap path would have.  The xmm contract
//! the translator asks for comes from the same database record: a fn record's
//! signature names the argument and result registers, a special reads the eight
//! argument registers and writes the two result ones, and __tlv_bootstrap and
//! ___chkstk_darwin, whose callers rely on every register surviving, have no
//! contract.
//!
//! ---- the stubs that keep r11 ----
//! An ordinary export's stub loads its id into r11, which is free to do: r11 is
//! a scratch register in the System V ABI and every linker stub on the platform
//! clobbers it.  __tlv_bootstrap is not an ordinary function.  It is the thunk a
//! thread-local variable's descriptor calls, and that calling convention keeps
//! every register but rax, so clang does hold live values in r11 across a
//! thread-local access; a program that loaded one thread-local into r11 and then
//! touched another read back garbage.  The stack probe ___chkstk_darwin has the
//! same contract for the same reason: clang calls it from a function prologue
//! and expects every register back.  The stub of a special record whose handler
//! is tlv_bootstrap or chkstk therefore pushes r11 before loading the id, which
//! still fits the sixteen-byte stride, and those handlers put r11 back from the
//! stack before they return.  Keying the push on the handler rather than on the
//! export's name keeps the two halves of that convention in one record.
//!
//! ---- ocerz's own trampolines ----
//! A native block the guest holds needs an invoke word the guest can call, and
//! that has to be x86 code that traps, with nothing any library exports behind
//! it.  So the last library ordinal belongs to no file: vd_lib refuses a
//! database file that would take it, and its entry indices name handlers in a
//! table here.  ocerz_vdylib_trampoline writes one page of guest memory the
//! first time it is asked, a stub of an export's shape per handler with its
//! jump slot at the page's middle, and makes the page read-only and executable,
//! so the dispatcher, the JIT's fast call and the xmm contract meet an id they
//! know how to treat, the contract being a special's.
//!
//! ---- dyld_stub_binder ----
//! A binary linked with classic lazy binding, which is every Intel binary built
//! for a macOS older than 12, imports dyld_stub_binder from libSystem whether or
//! not it ever reaches it: its __stub_helper entries jump there to bind a lazy
//! pointer on first call.  The loader applies lazy binds eagerly, so no helper
//! ever runs, but the import still has to bind or the whole program is refused
//! with 71 before its first instruction.  Its name is the one symbol with no
//! leading underscore, because dyld defines it in assembly rather than in C, and
//! libSystem's database exports it as a stub record; a guest that did reach it
//! would name it and stop, which is the honest answer to a lazy bind the loader
//! failed to make.
//!

use core::ffi::{c_char, c_int, c_void};
use core::{mem, ptr};
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU8, AtomicU64, Ordering};

use crate::ffi;
use crate::inline::{ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};

const VD_PAGE: u64 = 4096;
const VD_STUB_STRIDE: u64 = 16;
const VD_SLOT_BYTES: u64 = 8;
const VD_ULEB_WIDTH: u32 = 5;
const VD_ULEB_ABS_WIDTH: u32 = 10;
const VD_INDEX_BITS: u32 = 20;
const VD_INDEX_MASK: u32 = (1 << VD_INDEX_BITS) - 1;
const VD_LIBS_MAX: usize = 1 << (32 - VD_INDEX_BITS);
const VD_INTERNAL_ORD: u32 = VD_LIBS_MAX as u32 - 1;
const VD_TRAMP_SLOTS: u64 = 0x800;
const VD_GSS_OID_X86: u32 = 12;
const VD_LEGACY_MAX: usize = 8;
const VD_LEAF_WRITE_MAX: u64 = 16384;
const VD_LEAF_LIBRARY: &[u8] = b"/usr/lib/libSystem.B.dylib\0";
const VD_NONE: usize = 1;

type HostSym = ffi::OcerzVdylibHostSym;
type InternalHandler = unsafe extern "C" fn(*mut ffi::OcerzVM, *mut ffi::OcerzCPU) -> c_int;
type Filler = unsafe fn(*mut u8, u32);
type LateFiller = unsafe extern "C" fn(u64, u32, *const c_char, *const c_char, HostSym);

#[repr(C)]
struct VdInternal {
    name: *const c_char,
    handler: InternalHandler,
}
unsafe impl Sync for VdInternal {}

static G_VD_INTERNAL: [VdInternal; 7] = [
    VdInternal {
        name: c"(native block invoke)".as_ptr(),
        handler: ffi::ocerz_block_invoke_trap,
    },
    VdInternal {
        name: c"(native IMP)".as_ptr(),
        handler: ffi::ocerz_objc_imp_trap,
    },
    VdInternal {
        name: c"(native function)".as_ptr(),
        handler: ffi::ocerz_bridge_thunk_trap,
    },
    VdInternal {
        name: c"(Objective-C exception type false)".as_ptr(),
        handler: ffi::ocerz_objc_eh_false,
    },
    VdInternal {
        name: c"(Objective-C exception type match)".as_ptr(),
        handler: ffi::ocerz_objc_eh_do_catch,
    },
    VdInternal {
        name: c"(Objective-C exception destructor)".as_ptr(),
        handler: ffi::ocerz_objc_eh_destroy,
    },
    VdInternal {
        name: c"(Objective-C uncaught exception)".as_ptr(),
        handler: ffi::ocerz_objc_eh_terminate,
    },
];
const VD_NINTERNAL: usize = G_VD_INTERNAL.len();

#[repr(C)]
struct VdFiller {
    name: *const c_char,
    fill: Option<Filler>,
    late: Option<LateFiller>,
}
unsafe impl Sync for VdFiller {}

unsafe fn vd_fill_stack_guard(slot: *mut u8, size: u32) {
    let mut live = false;
    while !live && size > 1 {
        unsafe { libc::arc4random_buf(slot.add(1).cast(), (size - 1) as usize) };
        for i in 1..size {
            live |= unsafe { *slot.add(i as usize) != 0 };
        }
    }
    unsafe { *slot = 0 };
}

unsafe fn vd_fill_value(slot: *mut u8, size: u32, value: u64) {
    for i in 0..size {
        unsafe {
            *slot.add(i as usize) = if i < 8 { (value >> (8 * i)) as u8 } else { 0 };
        }
    }
}

unsafe fn vd_fill_page_size(slot: *mut u8, size: u32) {
    unsafe { vd_fill_value(slot, size, ffi::OCERZ_GUEST_PAGE_SIZE as u64) };
}

unsafe fn vd_fill_page_mask(slot: *mut u8, size: u32) {
    unsafe { vd_fill_value(slot, size, ffi::OCERZ_GUEST_PAGE_SIZE as u64 - 1) };
}

unsafe fn vd_fill_page_shift(slot: *mut u8, size: u32) {
    unsafe { vd_fill_value(slot, size, 12) };
}

unsafe fn vd_fill_fe_env(slot: *mut u8, size: u32, mxcsr: u32) {
    unsafe { libc::memset(slot.cast(), 0, size as usize) };
    if size >= 8 {
        unsafe {
            *slot = 0x7f;
            *slot.add(1) = 0x03;
            libc::memcpy(slot.add(4).cast(), &mxcsr as *const u32 as *const c_void, 4);
        }
    }
}

unsafe fn vd_fill_fe_dfl_env(slot: *mut u8, size: u32) {
    unsafe { vd_fill_fe_env(slot, size, 0x1f80) };
}

unsafe fn vd_fill_fe_dfl_daz_env(slot: *mut u8, size: u32) {
    unsafe { vd_fill_fe_env(slot, size, 0x9fc0) };
}

static mut G_VD_GSS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_VD_GSS_PAGE: u64 = 0;
static mut G_VD_GSS_USED: u64 = 0;
static mut G_VD_GSS_HOST: [*const c_void; 256] = [ptr::null(); 256];
static mut G_VD_GSS_GUEST: [u64; 256] = [0; 256];
static mut G_VD_GSS_N: c_int = 0;

unsafe fn vd_gss_alloc_locked(size: u32) -> u64 {
    let size = ((size as u64 + 15) & !15) as u64;
    if unsafe { G_VD_GSS_PAGE == 0 || G_VD_GSS_USED + size > ffi::OCERZ_GUEST_PAGE_SIZE as u64 } {
        unsafe {
            G_VD_GSS_PAGE = ffi::ocerz_map_anywhere(
                ffi::OCERZ_GUEST_PAGE_SIZE as u64,
                libc::PROT_READ | libc::PROT_WRITE,
            );
            G_VD_GSS_USED = 0;
        }
        if unsafe { G_VD_GSS_PAGE == 0 } {
            return 0;
        }
    }
    let at = unsafe { G_VD_GSS_PAGE + G_VD_GSS_USED };
    unsafe { G_VD_GSS_USED += size };
    at
}

unsafe fn vd_gss_write_oid(to: *mut u8, host: *const u8) {
    let mut length = 0u32;
    let mut elements: *const c_void = ptr::null();
    unsafe {
        libc::memcpy(
            &mut length as *mut u32 as *mut c_void,
            host.cast(),
            mem::size_of::<u32>(),
        );
        libc::memcpy(
            &mut elements as *mut *const c_void as *mut c_void,
            host.add(8).cast(),
            mem::size_of::<*const c_void>(),
        );
        let g = if elements.is_null() {
            0
        } else {
            ocerz_h2g(elements)
        };
        libc::memcpy(to.cast(), &length as *const u32 as *const c_void, 4);
        libc::memcpy(to.add(4).cast(), &g as *const u64 as *const c_void, 8);
    }
}

unsafe fn vd_gss_oid_locked(host: *const c_void) -> u64 {
    if host.is_null() {
        return 0;
    }
    for k in 0..unsafe { G_VD_GSS_N as usize } {
        if unsafe { G_VD_GSS_HOST[k] == host } {
            return unsafe { G_VD_GSS_GUEST[k] };
        }
    }
    let at = unsafe { vd_gss_alloc_locked(VD_GSS_OID_X86) };
    if at == 0 {
        return 0;
    }
    unsafe { vd_gss_write_oid(ocerz_g2h(at).cast(), host.cast()) };
    if unsafe { G_VD_GSS_N < 256 } {
        let n = unsafe { G_VD_GSS_N as usize };
        unsafe {
            G_VD_GSS_HOST[n] = host;
            G_VD_GSS_GUEST[n] = at;
            G_VD_GSS_N += 1;
        }
    }
    at
}

unsafe fn vd_gss_host_var(
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) -> *const c_void {
    let at = host_sym.map_or(ptr::null_mut(), |f| unsafe {
        f(install_name, export_name.add(1))
    });
    if at.is_null() {
        crate::ocerz_log!(
            "vdylib: %s has no %s on this host; its x86 copy is zero\n",
            install_name,
            export_name
        );
    }
    at
}

unsafe fn vd_fill_gss_oid_desc(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    let host = unsafe { vd_gss_host_var(install_name, export_name, host_sym) };
    if !host.is_null() && size >= VD_GSS_OID_X86 {
        unsafe { vd_gss_write_oid(ocerz_g2h(slot).cast(), host.cast()) };
    }
}

unsafe fn vd_fill_gss_oid_ptr(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    let host =
        unsafe { vd_gss_host_var(install_name, export_name, host_sym) as *const *const c_void };
    let mut g = 0;
    unsafe { libc::pthread_mutex_lock(ptr::addr_of_mut!(G_VD_GSS_LOCK)) };
    if !host.is_null() {
        g = unsafe { vd_gss_oid_locked(*host) };
    }
    unsafe { libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_VD_GSS_LOCK)) };
    if size >= mem::size_of::<u64>() as u32 {
        unsafe { ocerz_st(slot, 8, g) };
    }
}

unsafe fn vd_fill_gss_oid_set_ptr(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    let host = unsafe { vd_gss_host_var(install_name, export_name, host_sym) as *const *const u8 };
    let set = if host.is_null() {
        ptr::null()
    } else {
        unsafe { *host }
    };
    let mut g = 0;
    unsafe { libc::pthread_mutex_lock(ptr::addr_of_mut!(G_VD_GSS_LOCK)) };
    if !set.is_null() {
        let mut count = 0u64;
        let mut elements: *const u8 = ptr::null();
        unsafe {
            libc::memcpy(
                &mut count as *mut u64 as *mut c_void,
                set.cast(),
                mem::size_of::<u64>(),
            );
            libc::memcpy(
                &mut elements as *mut *const u8 as *mut c_void,
                set.add(8).cast(),
                mem::size_of::<*const u8>(),
            );
        }
        let array = if count != 0 && !elements.is_null() && count < 64 {
            unsafe { vd_gss_alloc_locked(count as u32 * VD_GSS_OID_X86) }
        } else {
            0
        };
        let mut k = 0;
        while array != 0 && k < count {
            unsafe {
                vd_gss_write_oid(
                    ocerz_g2h(array + k * VD_GSS_OID_X86 as u64).cast(),
                    elements.add((k * 16) as usize),
                )
            };
            k += 1;
        }
        g = unsafe { vd_gss_alloc_locked(16) };
        if g != 0 {
            unsafe {
                ocerz_st(g, 8, if array != 0 { count } else { 0 });
                ocerz_st(g + 8, 8, array);
            }
        }
    }
    unsafe { libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_VD_GSS_LOCK)) };
    if size >= mem::size_of::<u64>() as u32 {
        unsafe { ocerz_st(slot, 8, g) };
    }
}

unsafe fn vd_fill_zero(slot: *mut u8, size: u32) {
    unsafe { libc::memset(slot.cast(), 0, size as usize) };
}

static G_VD_FILLERS: [VdFiller; 12] = [
    VdFiller {
        name: c"stack_guard".as_ptr(),
        fill: Some(vd_fill_stack_guard),
        late: None,
    },
    VdFiller {
        name: c"zero".as_ptr(),
        fill: Some(vd_fill_zero),
        late: None,
    },
    VdFiller {
        name: c"page_size".as_ptr(),
        fill: Some(vd_fill_page_size),
        late: None,
    },
    VdFiller {
        name: c"page_mask".as_ptr(),
        fill: Some(vd_fill_page_mask),
        late: None,
    },
    VdFiller {
        name: c"page_shift".as_ptr(),
        fill: Some(vd_fill_page_shift),
        late: None,
    },
    VdFiller {
        name: c"fe_dfl_env".as_ptr(),
        fill: Some(vd_fill_fe_dfl_env),
        late: None,
    },
    VdFiller {
        name: c"fe_dfl_daz_env".as_ptr(),
        fill: Some(vd_fill_fe_dfl_daz_env),
        late: None,
    },
    VdFiller {
        name: c"objc_ehtype_vtable".as_ptr(),
        fill: None,
        late: Some(ffi::ocerz_objc_fill_ehtype_vtable),
    },
    VdFiller {
        name: c"objc_ehtype".as_ptr(),
        fill: None,
        late: Some(ffi::ocerz_objc_fill_ehtype),
    },
    VdFiller {
        name: c"gss_oid_desc".as_ptr(),
        fill: None,
        late: Some(vd_fill_gss_oid_desc_late),
    },
    VdFiller {
        name: c"gss_oid_ptr".as_ptr(),
        fill: None,
        late: Some(vd_fill_gss_oid_ptr_late),
    },
    VdFiller {
        name: c"gss_oid_set_ptr".as_ptr(),
        fill: None,
        late: Some(vd_fill_gss_oid_set_ptr_late),
    },
];

unsafe extern "C" fn vd_fill_gss_oid_desc_late(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    unsafe { vd_fill_gss_oid_desc(slot, size, install_name, export_name, host_sym) };
}

unsafe extern "C" fn vd_fill_gss_oid_ptr_late(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    unsafe { vd_fill_gss_oid_ptr(slot, size, install_name, export_name, host_sym) };
}

unsafe extern "C" fn vd_fill_gss_oid_set_ptr_late(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: HostSym,
) {
    unsafe { vd_fill_gss_oid_set_ptr(slot, size, install_name, export_name, host_sym) };
}

unsafe fn vd_filler(name: *const c_char) -> *const VdFiller {
    for f in &G_VD_FILLERS {
        if unsafe { libc::strcmp(f.name, name) == 0 } {
            return f;
        }
    }
    ptr::null()
}

struct VdLegacy {
    lib: *const c_char,
    export_name: *const c_char,
    host: *const c_char,
}
unsafe impl Sync for VdLegacy {}

static G_VD_LEGACY: [VdLegacy; 2] = [
    VdLegacy {
        lib: c"/System/Library/Frameworks/CoreLocation.framework/Versions/A/CoreLocation".as_ptr(),
        export_name: c"_kCLLocationAccuracyBest".as_ptr(),
        host: c"kCLLocationAccuracyBest".as_ptr(),
    },
    VdLegacy {
        lib: c"/System/Library/Frameworks/CoreLocation.framework/Versions/A/CoreLocation".as_ptr(),
        export_name: c"_kCLLocationAccuracyHundredMeters".as_ptr(),
        host: c"kCLLocationAccuracyHundredMeters".as_ptr(),
    },
];

#[repr(C)]
struct VdLib {
    api: *const ffi::OcerzApiLibrary,
    ordinal: u32,
    fn_cache: *mut AtomicPtr<ffi::OcerzBridgeFn>,
    native_missed: *mut AtomicU8,
}

static G_VD_LIBS: [AtomicPtr<VdLib>; VD_LIBS_MAX] =
    [const { AtomicPtr::new(ptr::null_mut()) }; VD_LIBS_MAX];
static mut G_VD_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_VD_FILES: *mut *mut c_char = ptr::null_mut();
static mut G_VD_NFILES: c_int = 0;
static G_VD_LISTED: AtomicI32 = AtomicI32::new(0);
static G_VD_TRAMP_PAGE: AtomicU64 = AtomicU64::new(0);
static mut G_VD_TRAMP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_VD_HOOKED: c_int = -1;

extern "C" fn vd_bridge_report() {
    unsafe { ffi::ocerz_bridge_report() };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_postfork_child() {
    unsafe {
        ptr::write(
            ptr::addr_of_mut!(G_VD_LOCK),
            libc::PTHREAD_MUTEX_INITIALIZER,
        );
    }
}

unsafe extern "C" fn vd_name_cmp(a: *const c_void, b: *const c_void) -> c_int {
    let aa = unsafe { *(a as *const *mut c_char) };
    let bb = unsafe { *(b as *const *mut c_char) };
    unsafe { libc::strcmp(aa, bb) }
}

unsafe fn vd_list_files_locked() {
    if G_VD_LISTED.load(Ordering::SeqCst) != 0 {
        return;
    }
    let dir = unsafe { ffi::ocerz_apidb_dir() };
    let d = if dir.is_null() {
        ptr::null_mut()
    } else {
        unsafe { libc::opendir(dir) }
    };
    let mut cap = 0;
    if !d.is_null() {
        loop {
            let de = unsafe { libc::readdir(d) };
            if de.is_null() {
                break;
            }
            let name = unsafe { (*de).d_name.as_ptr() };
            let n = unsafe { libc::strlen(name) };
            if unsafe { *name == b'.' as c_char }
                || n <= 4
                || unsafe { libc::strcmp(name.add(n - 4), c".api".as_ptr()) != 0 }
            {
                continue;
            }
            let count = unsafe { G_VD_NFILES };
            if count == cap {
                let nc = if cap != 0 { cap * 2 } else { 64 };
                let nf = unsafe {
                    libc::realloc(
                        G_VD_FILES.cast(),
                        nc as usize * mem::size_of::<*mut c_char>(),
                    ) as *mut *mut c_char
                };
                if nf.is_null() {
                    break;
                }
                unsafe { G_VD_FILES = nf };
                cap = nc;
            }
            let copy = unsafe { libc::strdup(name) };
            if copy.is_null() {
                break;
            }
            unsafe {
                *G_VD_FILES.add(G_VD_NFILES as usize) = copy;
                G_VD_NFILES += 1;
            }
        }
        unsafe { libc::closedir(d) };
    }
    let nfiles = unsafe { G_VD_NFILES };
    if nfiles > 1 {
        unsafe {
            libc::qsort(
                G_VD_FILES.cast(),
                nfiles as usize,
                mem::size_of::<*mut c_char>(),
                Some(vd_name_cmp),
            )
        };
    }
    G_VD_LISTED.store(1, Ordering::SeqCst);
}

unsafe fn vd_ordinal(api: *const ffi::OcerzApiLibrary) -> c_int {
    let path = unsafe { (*api).path };
    let mut base = unsafe { libc::strrchr(path, b'/' as c_int) };
    if base.is_null() {
        base = path as *mut c_char;
    } else {
        base = unsafe { base.add(1) };
    }
    if G_VD_LISTED.load(Ordering::SeqCst) == 0 {
        unsafe { libc::pthread_mutex_lock(ptr::addr_of_mut!(G_VD_LOCK)) };
        unsafe { vd_list_files_locked() };
        unsafe { libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_VD_LOCK)) };
    }
    let nfiles = unsafe { G_VD_NFILES };
    if nfiles == 0 {
        return -1;
    }
    let mut key = base;
    let hit = unsafe {
        libc::bsearch(
            &mut key as *mut *mut c_char as *const c_void,
            G_VD_FILES.cast(),
            nfiles as usize,
            mem::size_of::<*mut c_char>(),
            Some(vd_name_cmp),
        )
    };
    if hit.is_null() {
        -1
    } else {
        unsafe { (hit as *const *mut c_char).offset_from(G_VD_FILES) as c_int }
    }
}

unsafe fn vd_lib(install_name: *const c_char) -> *mut VdLib {
    let api = unsafe { ffi::ocerz_apidb_library(install_name) };
    if api.is_null() {
        return ptr::null_mut();
    }
    let ord = unsafe { vd_ordinal(api) };
    if ord < 0 || ord as u32 >= VD_INTERNAL_ORD {
        crate::ocerz_fatal!(
            "virtual %s: %s is not one of the first %u files of %s\n",
            install_name,
            unsafe { (*api).path },
            VD_INTERNAL_ORD,
            unsafe {
                let dir = ffi::ocerz_apidb_dir();
                if dir.is_null() {
                    c"(none)".as_ptr()
                } else {
                    dir
                }
            }
        );
        return ptr::null_mut();
    }
    if unsafe { (*api).nentries as u64 } > VD_INDEX_MASK as u64 + 1 {
        crate::ocerz_fatal!(
            "virtual %s declares %d exports, and an export id has room for %u\n",
            install_name,
            unsafe { (*api).nentries },
            VD_INDEX_MASK + 1
        );
        return ptr::null_mut();
    }
    let mut lib = G_VD_LIBS[ord as usize].load(Ordering::SeqCst);
    if !lib.is_null() {
        return lib;
    }

    unsafe { libc::pthread_mutex_lock(ptr::addr_of_mut!(G_VD_LOCK)) };
    lib = G_VD_LIBS[ord as usize].load(Ordering::SeqCst);
    if lib.is_null() {
        let n = if unsafe { (*api).nentries > 0 } {
            unsafe { (*api).nentries as usize }
        } else {
            1
        };
        let nl = unsafe { libc::calloc(1, mem::size_of::<VdLib>()) as *mut VdLib };
        let fn_cache = unsafe {
            libc::calloc(n, mem::size_of::<AtomicPtr<ffi::OcerzBridgeFn>>())
                as *mut AtomicPtr<ffi::OcerzBridgeFn>
        };
        let missed = unsafe { libc::calloc(n, mem::size_of::<AtomicU8>()) as *mut AtomicU8 };
        if !nl.is_null() && !fn_cache.is_null() && !missed.is_null() {
            for k in 0..n {
                unsafe {
                    fn_cache.add(k).write(AtomicPtr::new(ptr::null_mut()));
                    missed.add(k).write(AtomicU8::new(0));
                }
            }
            unsafe {
                nl.write(VdLib {
                    api,
                    ordinal: ord as u32,
                    fn_cache,
                    native_missed: missed,
                });
            }
            G_VD_LIBS[ord as usize].store(nl, Ordering::SeqCst);
            lib = nl;
        } else {
            unsafe {
                libc::free(nl.cast());
                libc::free(fn_cache.cast());
                libc::free(missed.cast());
            }
            crate::ocerz_fatal!("out of memory registering virtual %s\n", install_name);
        }
    }
    unsafe { libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_VD_LOCK)) };
    lib
}

unsafe fn vd_has_stub(kind: ffi::OcerzApiKind) -> bool {
    kind == ffi::OCERZ_API_FN || kind == ffi::OCERZ_API_SPECIAL || kind == ffi::OCERZ_API_STUB
}

unsafe fn vd_lib_of_id(id: u64, entry_out: *mut *const ffi::OcerzApiEntry) -> *mut VdLib {
    if id > u32::MAX as u64 {
        return ptr::null_mut();
    }
    let id = id as u32;
    let ord = id >> VD_INDEX_BITS;
    let idx = id & VD_INDEX_MASK;
    let lib = G_VD_LIBS[ord as usize].load(Ordering::SeqCst);
    if lib.is_null() {
        return ptr::null_mut();
    }
    let api = unsafe { (*lib).api };
    if idx >= unsafe { (*api).nentries as u32 } {
        return ptr::null_mut();
    }
    let entry = unsafe { (*api).entries.add(idx as usize) };
    if !unsafe { vd_has_stub((*entry).kind) } {
        return ptr::null_mut();
    }
    if !entry_out.is_null() {
        unsafe { *entry_out = entry };
    }
    lib
}

unsafe fn vd_internal_of_id(id: u64) -> *const VdInternal {
    if id > u32::MAX as u64 || ((id as u32) >> VD_INDEX_BITS) != VD_INTERNAL_ORD {
        return ptr::null();
    }
    let idx = (id as u32 & VD_INDEX_MASK) as usize;
    if idx < VD_NINTERNAL {
        unsafe { G_VD_INTERNAL.as_ptr().add(idx) }
    } else {
        ptr::null()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_have(install_name: *const c_char) -> c_int {
    (!unsafe { ffi::ocerz_apidb_library(install_name) }.is_null()) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_export_name(
    id: u64,
    lib_out: *mut *const c_char,
    sym_out: *mut *const c_char,
) -> c_int {
    let internal = unsafe { vd_internal_of_id(id) };
    if !internal.is_null() {
        if !lib_out.is_null() {
            unsafe { *lib_out = c"ocerz".as_ptr() };
        }
        if !sym_out.is_null() {
            unsafe { *sym_out = (*internal).name };
        }
        return 1;
    }
    let mut entry = ptr::null();
    let lib = unsafe { vd_lib_of_id(id, &mut entry) };
    if lib.is_null() {
        return 0;
    }
    if !lib_out.is_null() {
        unsafe { *lib_out = (*(*lib).api).install_name };
    }
    if !sym_out.is_null() {
        unsafe { *sym_out = (*entry).export_name };
    }
    1
}

#[inline(always)]
unsafe fn wr32(p: *mut u8, v: u32) {
    unsafe {
        *p = v as u8;
        *p.add(1) = (v >> 8) as u8;
        *p.add(2) = (v >> 16) as u8;
        *p.add(3) = (v >> 24) as u8;
    }
}

#[inline(always)]
unsafe fn wr64(p: *mut u8, v: u64) {
    unsafe {
        wr32(p, v as u32);
        wr32(p.add(4), (v >> 32) as u32);
    }
}

#[inline(always)]
fn vd_round_up(v: u64, align: u64) -> u64 {
    (v + align - 1) & !(align - 1)
}

unsafe fn vd_uleb_fixed(p: *mut u8, v: u64, width: u32) {
    for i in 0..width - 1 {
        unsafe { *p.add(i as usize) = (((v >> (7 * i)) & 0x7f) | 0x80) as u8 };
    }
    unsafe { *p.add((width - 1) as usize) = ((v >> (7 * (width - 1))) & 0x7f) as u8 };
}

fn vd_term_width(absolute: bool) -> u32 {
    if absolute {
        VD_ULEB_ABS_WIDTH
    } else {
        VD_ULEB_WIDTH
    }
}

#[derive(Clone, Copy)]
struct VdSym {
    name: *const c_char,
    addr: u64,
    absolute: c_int,
}

#[derive(Clone, Copy)]
struct VdNode {
    edge: *const c_char,
    elen: usize,
    first_child: c_int,
    next_sibling: c_int,
    terminal: c_int,
    absolute: c_int,
    addr: u64,
    off: u64,
    size: u64,
}

struct VdTrie {
    node: *mut VdNode,
    n: c_int,
    cap: c_int,
    overflow: c_int,
    sym: *const VdSym,
}

unsafe extern "C" fn vd_sym_cmp(a: *const c_void, b: *const c_void) -> c_int {
    let x = a as *const VdSym;
    let y = b as *const VdSym;
    unsafe { libc::strcmp((*x).name, (*y).name) }
}

unsafe fn vd_node_new(t: *mut VdTrie) -> c_int {
    if unsafe { (*t).n >= (*t).cap } {
        unsafe { (*t).overflow = 1 };
        return -1;
    }
    let i = unsafe { (*t).n };
    unsafe {
        (*t).n += 1;
        (*(*t).node.add(i as usize) = VdNode {
            edge: ptr::null(),
            elen: 0,
            first_child: -1,
            next_sibling: -1,
            terminal: 0,
            absolute: 0,
            addr: 0,
            off: 0,
            size: 0,
        });
    }
    i
}

unsafe fn vd_trie_build(t: *mut VdTrie, lo: c_int, hi: c_int, depth: usize) -> c_int {
    let me = unsafe { vd_node_new(t) };
    if me < 0 {
        return -1;
    }
    let mut i = lo;
    if i < hi && unsafe { libc::strlen((*(*t).sym.add(lo as usize)).name) == depth } {
        let sym = unsafe { *(*t).sym.add(lo as usize) };
        unsafe {
            (*(*t).node.add(me as usize)).terminal = 1;
            (*(*t).node.add(me as usize)).absolute = sym.absolute;
            (*(*t).node.add(me as usize)).addr = sym.addr;
        }
        i = lo + 1;
    }

    let mut last = -1;
    while i < hi {
        let first_name = unsafe { (*(*t).sym.add(i as usize)).name };
        let first_char = unsafe { *first_name.add(depth) };
        let mut j = i + 1;
        while j < hi && unsafe { *(*(*t).sym.add(j as usize)).name.add(depth) == first_char } {
            j += 1;
        }
        let a = first_name;
        let b = unsafe { (*(*t).sym.add((j - 1) as usize)).name };
        let mut lcp = depth;
        while unsafe { *a.add(lcp) != 0 && *a.add(lcp) == *b.add(lcp) } {
            lcp += 1;
        }
        let elen = lcp - depth;
        if elen == 0 {
            unsafe { (*t).overflow = 1 };
            return -1;
        }
        let kid = unsafe { vd_trie_build(t, i, j, lcp) };
        if kid < 0 {
            return -1;
        }
        unsafe {
            let child = (*t).node.add(kid as usize);
            (*child).edge = a.add(depth);
            (*child).elen = elen;
            if last < 0 {
                (*(*t).node.add(me as usize)).first_child = kid;
            } else {
                (*(*t).node.add(last as usize)).next_sibling = kid;
            }
        }
        last = kid;
        i = j;
    }
    me
}

unsafe fn vd_trie_layout(t: *mut VdTrie) -> u64 {
    for i in 0..unsafe { (*t).n } {
        let node = unsafe { (*t).node.add(i as usize) };
        let mut sz = 1u64;
        let mut kids = 0;
        if unsafe { (*node).terminal != 0 } {
            sz += 1 + vd_term_width(unsafe { (*node).absolute != 0 }) as u64;
        }
        sz += 1;
        let mut child = unsafe { (*node).first_child };
        while child >= 0 {
            let c = unsafe { (*t).node.add(child as usize) };
            sz += unsafe { (*c).elen as u64 + 1 + VD_ULEB_WIDTH as u64 };
            kids += 1;
            child = unsafe { (*c).next_sibling };
        }
        if kids > 255 {
            unsafe { (*t).overflow = 1 };
            return 0;
        }
        unsafe { (*node).size = sz };
    }
    let mut off = 0u64;
    for i in 0..unsafe { (*t).n } {
        let node = unsafe { (*t).node.add(i as usize) };
        unsafe {
            (*node).off = off;
            off += (*node).size;
        }
    }
    off
}

unsafe fn vd_trie_emit(t: *const VdTrie, out: *mut u8) {
    for i in 0..unsafe { (*t).n } {
        let node = unsafe { (*t).node.add(i as usize) };
        let mut p = unsafe { out.add((*node).off as usize) };
        if unsafe { (*node).terminal != 0 } {
            let width = vd_term_width(unsafe { (*node).absolute != 0 });
            unsafe {
                *p = (1 + width) as u8;
                p = p.add(1);
                *p = if (*node).absolute != 0 { 2 } else { 0 };
                p = p.add(1);
                vd_uleb_fixed(p, (*node).addr, width);
                p = p.add(width as usize);
            }
        } else {
            unsafe {
                *p = 0;
                p = p.add(1);
            }
        }
        let mut kids = 0u8;
        let mut child = unsafe { (*node).first_child };
        while child >= 0 {
            kids = kids.wrapping_add(1);
            child = unsafe { (*(*t).node.add(child as usize)).next_sibling };
        }
        unsafe { *p = kids };
        p = unsafe { p.add(1) };
        child = unsafe { (*node).first_child };
        while child >= 0 {
            let c = unsafe { (*t).node.add(child as usize) };
            unsafe {
                libc::memcpy(p.cast(), (*c).edge.cast(), (*c).elen);
                p = p.add((*c).elen);
                *p = 0;
                p = p.add(1);
                vd_uleb_fixed(p, (*c).off, VD_ULEB_WIDTH);
                p = p.add(VD_ULEB_WIDTH as usize);
                child = (*c).next_sibling;
            }
        }
    }
}

unsafe fn vd_write_segment(
    p: *mut u8,
    cmdsize: u32,
    name: *const c_char,
    vmaddr: u64,
    vmsize: u64,
    fileoff: u64,
    filesize: u64,
    prot: u32,
    nsects: u32,
) {
    unsafe {
        wr32(p, 0x19);
        wr32(p.add(4), cmdsize);
        libc::memcpy(p.add(8).cast(), name.cast(), libc::strlen(name));
        wr64(p.add(24), vmaddr);
        wr64(p.add(32), vmsize);
        wr64(p.add(40), fileoff);
        wr64(p.add(48), filesize);
        wr32(p.add(56), prot);
        wr32(p.add(60), prot);
        wr32(p.add(64), nsects);
        wr32(p.add(68), 0);
    }
}

unsafe fn vd_write_section(
    p: *mut u8,
    sect: *const c_char,
    seg: *const c_char,
    addr: u64,
    size: u64,
    off: u32,
    align: u32,
    flags: u32,
) {
    unsafe {
        libc::memcpy(p.cast(), sect.cast(), libc::strlen(sect));
        libc::memcpy(p.add(16).cast(), seg.cast(), libc::strlen(seg));
        wr64(p.add(32), addr);
        wr64(p.add(40), size);
        wr32(p.add(48), off);
        wr32(p.add(52), align);
        wr32(p.add(56), 0);
        wr32(p.add(60), 0);
        wr32(p.add(64), flags);
        wr32(p.add(68), 0);
        wr32(p.add(72), 0);
        wr32(p.add(76), 0);
    }
}

unsafe fn vd_keeps_r11(e: *const ffi::OcerzApiEntry) -> bool {
    unsafe {
        (*e).kind == ffi::OCERZ_API_SPECIAL
            && !(*e).handler.is_null()
            && (libc::strcmp((*e).handler, c"tlv_bootstrap".as_ptr()) == 0
                || libc::strcmp((*e).handler, c"chkstk".as_ptr()) == 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_image_with(
    install_name: *const c_char,
    mut host_sym: HostSym,
    len_out: *mut usize,
) -> *mut u8 {
    let lib = unsafe { vd_lib(install_name) };
    if lib.is_null() {
        return ptr::null_mut();
    }
    if host_sym.is_none() {
        host_sym = Some(ffi::ocerz_bridge_host_symbol);
    }
    let api = unsafe { (*lib).api };
    let name = unsafe { (*api).install_name };
    let ne = unsafe { (*api).nentries };
    let mut n = 0;
    let mut nv = 0;
    for k in 0..ne {
        let kind = unsafe { (*(*api).entries.add(k as usize)).kind };
        n += unsafe { vd_has_stub(kind) } as c_int;
        nv += (kind == ffi::OCERZ_API_VAR) as c_int;
    }
    let name_len = unsafe { libc::strlen(name) as u32 + 1 };
    let id_cmdsize = vd_round_up(24 + name_len as u64, 8) as u32;
    let seg_cmdsize = (72 + 80) as u32;
    let trie_cmdsize = 16u32;
    let sizeofcmds = 2 * seg_cmdsize + id_cmdsize + trie_cmdsize;
    let hdr_size = 32u32;
    let stubs_off = vd_round_up(hdr_size as u64 + sizeofcmds as u64, VD_STUB_STRIDE);
    let stubs_size = n as u64 * VD_STUB_STRIDE;
    let text_size = vd_round_up(stubs_off + stubs_size, VD_PAGE);
    let slots_off = text_size;
    let slots_size = n as u64 * VD_SLOT_BYTES;
    let vars_off = vd_round_up(slots_off + slots_size, VD_SLOT_BYTES);
    let ne_alloc = if ne > 0 { ne as usize } else { 0 };
    let var_addr = unsafe { libc::calloc(ne_alloc + 1, mem::size_of::<u64>()) as *mut u64 };
    let sym_cap = ne_alloc + 1 + VD_LEGACY_MAX;
    let syms = unsafe { libc::calloc(sym_cap, mem::size_of::<VdSym>()) as *mut VdSym };
    let node_cap = 2 * (ne_alloc + VD_LEGACY_MAX) + 2;
    let nodes = unsafe { libc::calloc(node_cap, mem::size_of::<VdNode>()) as *mut VdNode };
    let mut buf: *mut u8 = ptr::null_mut();

    macro_rules! image_fail {
        () => {{
            unsafe {
                libc::free(var_addr.cast());
                libc::free(syms.cast());
                libc::free(nodes.cast());
                libc::free(buf.cast());
            }
            return ptr::null_mut();
        }};
    }

    if var_addr.is_null() || syms.is_null() || nodes.is_null() {
        crate::ocerz_fatal!("out of memory building virtual %s\n", name);
        image_fail!();
    }

    let mut m = 0;
    let mut missed = 0;
    let mut vars_end = vars_off;
    let mut stub_index = 0u32;
    for k in 0..ne {
        let e = unsafe { (*api).entries.add(k as usize) };
        let kind = unsafe { (*e).kind };
        if unsafe { vd_has_stub(kind) } {
            unsafe {
                (*syms.add(m as usize)).name = (*e).export_name;
                (*syms.add(m as usize)).addr = stubs_off + stub_index as u64 * VD_STUB_STRIDE;
                (*syms.add(m as usize)).absolute = 0;
            }
            stub_index += 1;
            m += 1;
        } else if kind == ffi::OCERZ_API_VAR {
            let filler = unsafe { vd_filler((*e).filler) };
            if filler.is_null() {
                crate::ocerz_fatal!(
                    "virtual %s: %s asks for the filler %s, which ocerz does not have\n",
                    name,
                    unsafe { (*e).export_name },
                    unsafe { (*e).filler }
                );
                image_fail!();
            }
            if unsafe { (*e).bytes } == 0 {
                crate::ocerz_fatal!(
                    "virtual %s: %s is a data slot of no bytes\n",
                    name,
                    unsafe { (*e).export_name }
                );
                image_fail!();
            }
            unsafe { *var_addr.add(k as usize) = vars_end };
            vars_end += vd_round_up(unsafe { (*e).bytes as u64 }, VD_SLOT_BYTES);
            unsafe {
                (*syms.add(m as usize)).name = (*e).export_name;
                (*syms.add(m as usize)).addr = *var_addr.add(k as usize);
                (*syms.add(m as usize)).absolute = 0;
            }
            m += 1;
        } else if kind == ffi::OCERZ_API_DATA {
            let host = unsafe { host_sym.unwrap()(name, (*e).host) };
            if host.is_null() {
                let idx = k as usize;
                if unsafe { (*(*lib).native_missed.add(idx)).swap(1, Ordering::SeqCst) == 0 } {
                    crate::ocerz_log!(
                        "vdylib: host %s has no %s, so virtual %s does not export %s\n",
                        name,
                        unsafe { (*e).host },
                        name,
                        unsafe { (*e).export_name }
                    );
                }
                missed += 1;
                continue;
            }
            unsafe {
                (*syms.add(m as usize)).name = (*e).export_name;
                (*syms.add(m as usize)).addr = host as u64;
                (*syms.add(m as usize)).absolute = 1;
            }
            m += 1;
        }
    }
    for legacy in &G_VD_LEGACY {
        if unsafe { libc::strcmp(legacy.lib, name) != 0 } {
            continue;
        }
        let host = unsafe { host_sym.unwrap()(name, legacy.host) };
        if host.is_null() {
            crate::ocerz_log!(
                "vdylib: host %s has no %s, so virtual %s does not export %s\n",
                name,
                legacy.host,
                name,
                legacy.export_name
            );
            continue;
        }
        unsafe {
            (*syms.add(m as usize)).name = legacy.export_name;
            (*syms.add(m as usize)).addr = host as u64;
            (*syms.add(m as usize)).absolute = 1;
        }
        m += 1;
    }

    let data_used = vars_end - slots_off;
    let data_size = vd_round_up(if data_used != 0 { data_used } else { 1 }, VD_PAGE);
    let trie_off = slots_off + data_size;
    unsafe {
        libc::qsort(
            syms.cast(),
            m as usize,
            mem::size_of::<VdSym>(),
            Some(vd_sym_cmp),
        )
    };
    for i in 1..m {
        if unsafe {
            libc::strcmp(
                (*syms.add((i - 1) as usize)).name,
                (*syms.add(i as usize)).name,
            ) == 0
        } {
            crate::ocerz_fatal!("virtual %s exports %s twice\n", name, unsafe {
                (*syms.add(i as usize)).name
            });
            image_fail!();
        }
    }

    let mut trie = VdTrie {
        node: nodes,
        n: 0,
        cap: node_cap as c_int,
        overflow: 0,
        sym: syms,
    };
    if unsafe { vd_trie_build(&mut trie, 0, m, 0) } != 0 || trie.overflow != 0 {
        crate::ocerz_fatal!("virtual %s cannot be laid out as an export trie\n", name);
        image_fail!();
    }
    let trie_size = unsafe { vd_trie_layout(&mut trie) };
    if trie.overflow != 0 || trie_size == 0 {
        crate::ocerz_fatal!(
            "virtual %s has a trie node with more than 255 children\n",
            name
        );
        image_fail!();
    }
    let total = trie_off + trie_size;
    if total > u32::MAX as u64 {
        crate::ocerz_fatal!(
            "virtual %s would be %llu bytes, past what 32-bit file offsets reach\n",
            name,
            total
        );
        image_fail!();
    }
    buf = unsafe { libc::calloc(1, total as usize) as *mut u8 };
    if buf.is_null() {
        crate::ocerz_fatal!("out of memory building virtual %s\n", name);
        image_fail!();
    }

    unsafe {
        wr32(buf, 0xfeed_facf);
        wr32(buf.add(4), 0x0100_0007);
        wr32(buf.add(8), 3);
        wr32(buf.add(12), 6);
        wr32(buf.add(16), 4);
        wr32(buf.add(20), sizeofcmds);
        wr32(buf.add(24), 1 | 4 | 0x80);
        wr32(buf.add(28), 0);

        let mut lc = buf.add(hdr_size as usize);
        vd_write_segment(
            lc,
            seg_cmdsize,
            c"__TEXT".as_ptr(),
            0,
            text_size,
            0,
            text_size,
            1 | 4,
            1,
        );
        vd_write_section(
            lc.add(72),
            c"__text".as_ptr(),
            c"__TEXT".as_ptr(),
            stubs_off,
            stubs_size,
            stubs_off as u32,
            4,
            0x8000_0000 | 0x400,
        );
        lc = lc.add(seg_cmdsize as usize);
        vd_write_segment(
            lc,
            seg_cmdsize,
            c"__DATA".as_ptr(),
            slots_off,
            data_size,
            slots_off,
            data_size,
            1 | 2,
            1,
        );
        vd_write_section(
            lc.add(72),
            c"__data".as_ptr(),
            c"__DATA".as_ptr(),
            slots_off,
            data_used,
            slots_off as u32,
            3,
            0,
        );
        lc = lc.add(seg_cmdsize as usize);
        wr32(lc, 0x0d);
        wr32(lc.add(4), id_cmdsize);
        wr32(lc.add(8), 24);
        wr32(lc.add(12), 1);
        wr32(lc.add(16), 0x10000);
        wr32(lc.add(20), 0x10000);
        libc::memcpy(lc.add(24).cast(), name.cast(), name_len as usize);
        lc = lc.add(id_cmdsize as usize);
        wr32(lc, 0x8000_0033);
        wr32(lc.add(4), trie_cmdsize);
        wr32(lc.add(8), trie_off as u32);
        wr32(lc.add(12), trie_size as u32);

        vd_trie_emit(&trie, buf.add(trie_off as usize));
    }

    let base = unsafe { (*lib).ordinal } << VD_INDEX_BITS;
    stub_index = 0;
    for k in 0..ne {
        let e = unsafe { (*api).entries.add(k as usize) };
        if unsafe { (*e).kind } == ffi::OCERZ_API_VAR {
            let filler = unsafe { vd_filler((*e).filler) };
            let f = unsafe { &*filler };
            if f.late.is_some() {
                unsafe {
                    libc::memset(
                        buf.add(*var_addr.add(k as usize) as usize).cast(),
                        0,
                        (*e).bytes as usize,
                    )
                };
            } else {
                unsafe { f.fill.unwrap()(buf.add(*var_addr.add(k as usize) as usize), (*e).bytes) };
            }
            continue;
        }
        if !unsafe { vd_has_stub((*e).kind) } {
            continue;
        }
        let stub_addr = stubs_off + stub_index as u64 * VD_STUB_STRIDE;
        let slot_addr = slots_off + stub_index as u64 * VD_SLOT_BYTES;
        stub_index += 1;
        let s = unsafe { buf.add(stub_addr as usize) };
        let mut at = 0usize;
        if unsafe { vd_keeps_r11(e) } {
            unsafe {
                *s.add(at) = 0x41;
                *s.add(at + 1) = 0x53;
            }
            at += 2;
        }
        unsafe {
            *s.add(at) = 0x41;
            *s.add(at + 1) = 0xbb;
            wr32(s.add(at + 2), base | k as u32);
        }
        at += 6;
        unsafe {
            *s.add(at) = 0xff;
            *s.add(at + 1) = 0x25;
        }
        let rel = slot_addr as i64 - (stub_addr as i64 + at as i64 + 6);
        unsafe { wr32(s.add(at + 2), rel as i32 as u32) };
        at += 6;
        for pad in at..VD_STUB_STRIDE as usize {
            unsafe { *s.add(pad) = 0xcc };
        }
        unsafe {
            wr64(
                buf.add(slot_addr as usize),
                ffi::OCERZ_DYLDAPI_LO as u64 + ffi::OCERZ_BRIDGE_OFF as u64,
            )
        };
    }

    crate::ocerz_log!(
        "vdylib: built %s from %s with %d exports (%d functions, %d data, %d native, %d native missing), %llu bytes (text %llu data %llu trie %llu at %llu)\n",
        name,
        unsafe { (*api).path },
        m,
        n,
        nv,
        m - n - nv,
        missed,
        total,
        text_size,
        data_size,
        trie_size,
        trie_off
    );
    unsafe {
        libc::free(var_addr.cast());
        libc::free(syms.cast());
        libc::free(nodes.cast());
    }
    if !len_out.is_null() {
        unsafe { *len_out = total as usize };
    }
    buf
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_image(
    install_name: *const c_char,
    len_out: *mut usize,
) -> *mut u8 {
    unsafe { ocerz_vdylib_image_with(install_name, Some(ffi::ocerz_bridge_host_symbol), len_out) }
}

#[inline(always)]
unsafe fn vd_errno_enter(cpu: *const ffi::OcerzCPU) {
    if unsafe { (*cpu).gs_base } != 0 {
        unsafe {
            *libc::__error() = ocerz_ld((*cpu).gs_base + ffi::OCERZ_ERRNO_SLOT as u64, 4) as c_int;
        }
    }
}

#[inline(always)]
unsafe fn vd_errno_leave(cpu: *const ffi::OcerzCPU) {
    if unsafe { (*cpu).gs_base } != 0 {
        unsafe {
            ocerz_st(
                (*cpu).gs_base + ffi::OCERZ_ERRNO_SLOT as u64,
                4,
                *libc::__error() as u32 as u64,
            );
        }
    }
}

#[inline(always)]
unsafe fn vd_dispatch(vm: *mut ffi::OcerzVM, cpu: *mut ffi::OcerzCPU) -> c_int {
    unsafe {
        if G_VD_HOOKED < 0 {
            G_VD_HOOKED = 0;
            if !libc::getenv(c"OCERZ_BRIDGESTAT".as_ptr()).is_null() {
                libc::atexit(vd_bridge_report);
            }
        }
    }
    let id = unsafe { (*cpu).gpr[ffi::OCERZ_R11 as usize] & 0xffff_ffff };
    let internal = unsafe { vd_internal_of_id(id) };
    if !internal.is_null() {
        unsafe { vd_errno_enter(cpu) };
        let rc = unsafe { ((*internal).handler)(vm, cpu) };
        unsafe { vd_errno_leave(cpu) };
        return rc;
    }
    let mut entry = ptr::null();
    let lib = unsafe { vd_lib_of_id(id, &mut entry) };
    if lib.is_null() {
        unsafe {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: export id %#llx is not one a synthesized library minted\n"
                    .as_ptr(),
                id,
            );
            libc::exit(ffi::OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
    }

    let idx = unsafe { entry.offset_from((*(*lib).api).entries) as usize };
    let slot = unsafe { (*lib).fn_cache.add(idx) };
    let mut fn_ = unsafe { (*slot).load(Ordering::SeqCst) };
    if fn_.is_null() {
        fn_ = unsafe { ffi::ocerz_bridge_lookup((*(*lib).api).install_name, (*entry).export_name) }
            as *mut ffi::OcerzBridgeFn;
        if fn_.is_null() {
            fn_ = VD_NONE as *mut ffi::OcerzBridgeFn;
        }
        unsafe { (*slot).store(fn_, Ordering::SeqCst) };
    }
    if fn_ as usize != VD_NONE {
        unsafe { vd_errno_enter(cpu) };
        let rc = unsafe { ffi::ocerz_bridge_invoke(vm, cpu, fn_) };
        unsafe { vd_errno_leave(cpu) };
        return rc;
    }

    unsafe extern "C" {
        static mut ocerz_cmdline_summary: c_char;
    }
    let caller = unsafe { ocerz_ld((*cpu).gpr[ffi::OCERZ_RSP as usize], 8) };
    let mut base = 0u64;
    let image = unsafe { ffi::ocerz_dyld_name_for_addr(caller, &mut base) };
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: %s %s not implemented\n".as_ptr(),
            (*(*lib).api).install_name,
            (*entry).export_name,
        );
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz:   called from %#llx, %s+%#llx, in '%s'\n".as_ptr(),
            caller,
            if image.is_null() {
                c"an unknown image".as_ptr()
            } else {
                image
            },
            caller.wrapping_sub(base),
            ptr::addr_of_mut!(ocerz_cmdline_summary),
        );
        libc::exit(ffi::OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
    }
    ffi::OCERZ_STEP_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_dispatch(
    vm: *mut ffi::OcerzVM,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    unsafe { vd_dispatch(vm, cpu) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_xmm_contract(id: u64, in_: *mut u16, out: *mut u16) -> c_int {
    let internal = unsafe { vd_internal_of_id(id) };
    if !in_.is_null() && !out.is_null() && !internal.is_null() {
        unsafe {
            *in_ = 0xff;
            *out = 0x3;
        }
        return 1;
    }
    let mut entry = ptr::null();
    if in_.is_null() || out.is_null() || unsafe { vd_lib_of_id(id, &mut entry) }.is_null() {
        return 0;
    }
    if unsafe { (*entry).kind == ffi::OCERZ_API_FN } {
        if unsafe { (*entry).sig.is_null() } {
            return 0;
        }
        let mut sig = ffi::OcerzAbiSig::default();
        if unsafe { ffi::ocerz_abi_parse((*entry).sig, &mut sig) } != ffi::OCERZ_OK as c_int {
            return 0;
        }
        unsafe { ffi::ocerz_abi_xmm_contract(&sig, in_, out) };
        return 1;
    }
    if unsafe { (*entry).kind == ffi::OCERZ_API_SPECIAL }
        && unsafe { !(*entry).handler.is_null() }
        && unsafe {
            libc::strcmp((*entry).handler, c"tlv_bootstrap".as_ptr()) != 0
                && libc::strcmp((*entry).handler, c"chkstk".as_ptr()) != 0
        }
    {
        unsafe {
            *in_ = 0xff;
            *out = 0x3;
        }
        return 1;
    }
    0
}

unsafe extern "C" {
    fn ocerz_leaf_strlen();
    fn ocerz_leaf_strnlen();
    fn ocerz_leaf_strcmp();
    fn ocerz_leaf_strncmp();
    fn ocerz_leaf_memcmp();
    fn ocerz_leaf_strchr();
    fn ocerz_leaf_memchr();
    fn ocerz_leaf_memmove();
    fn ocerz_leaf_memset();
}

struct VdLeaf {
    export_name: *const c_char,
    host: *const c_char,
    sig: *const c_char,
    routine: unsafe extern "C" fn(),
    rdx_limit: u64,
}
unsafe impl Sync for VdLeaf {}

static G_VD_LEAF: [VdLeaf; 11] = [
    VdLeaf {
        export_name: c"_strlen".as_ptr(),
        host: c"strlen".as_ptr(),
        sig: c"L(p)".as_ptr(),
        routine: ocerz_leaf_strlen,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_strnlen".as_ptr(),
        host: c"strnlen".as_ptr(),
        sig: c"L(pL)".as_ptr(),
        routine: ocerz_leaf_strnlen,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_strcmp".as_ptr(),
        host: c"strcmp".as_ptr(),
        sig: c"i(pp)".as_ptr(),
        routine: ocerz_leaf_strcmp,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_strncmp".as_ptr(),
        host: c"strncmp".as_ptr(),
        sig: c"i(ppL)".as_ptr(),
        routine: ocerz_leaf_strncmp,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_memcmp".as_ptr(),
        host: c"memcmp".as_ptr(),
        sig: c"i(ppL)".as_ptr(),
        routine: ocerz_leaf_memcmp,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_bcmp".as_ptr(),
        host: c"bcmp".as_ptr(),
        sig: c"i(ppL)".as_ptr(),
        routine: ocerz_leaf_memcmp,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_strchr".as_ptr(),
        host: c"strchr".as_ptr(),
        sig: c"p(pi)".as_ptr(),
        routine: ocerz_leaf_strchr,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_memchr".as_ptr(),
        host: c"memchr".as_ptr(),
        sig: c"p(piL)".as_ptr(),
        routine: ocerz_leaf_memchr,
        rdx_limit: 0,
    },
    VdLeaf {
        export_name: c"_memcpy".as_ptr(),
        host: c"memcpy".as_ptr(),
        sig: c"p(ppL)".as_ptr(),
        routine: ocerz_leaf_memmove,
        rdx_limit: VD_LEAF_WRITE_MAX,
    },
    VdLeaf {
        export_name: c"_memmove".as_ptr(),
        host: c"memmove".as_ptr(),
        sig: c"p(ppL)".as_ptr(),
        routine: ocerz_leaf_memmove,
        rdx_limit: VD_LEAF_WRITE_MAX,
    },
    VdLeaf {
        export_name: c"_memset".as_ptr(),
        host: c"memset".as_ptr(),
        sig: c"p(piL)".as_ptr(),
        routine: ocerz_leaf_memset,
        rdx_limit: VD_LEAF_WRITE_MAX,
    },
];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_leaf(id: u64, rdx_limit: *mut u64) -> *const c_void {
    let mut entry = ptr::null();
    let lib = unsafe { vd_lib_of_id(id, &mut entry) };
    if lib.is_null()
        || rdx_limit.is_null()
        || unsafe {
            (*entry).kind != ffi::OCERZ_API_FN
                || (*entry).host.is_null()
                || (*entry).sig.is_null()
                || (*entry).nstructs != 0
                || (*entry).ninplace != 0
                || libc::strcmp((*(*lib).api).install_name, VD_LEAF_LIBRARY.as_ptr().cast()) != 0
        }
    {
        return ptr::null();
    }
    for leaf in &G_VD_LEAF {
        if unsafe { libc::strcmp((*entry).export_name, leaf.export_name) == 0 } {
            if unsafe {
                libc::strcmp((*entry).host, leaf.host) != 0
                    || libc::strcmp((*entry).sig, leaf.sig) != 0
            } {
                return ptr::null();
            }
            unsafe { *rdx_limit = leaf.rdx_limit };
            return leaf.routine as *const () as *const c_void;
        }
    }
    ptr::null()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_trap_only(id: u64) -> c_int {
    let mut entry = ptr::null();
    (!unsafe { vd_lib_of_id(id, &mut entry) }.is_null()
        && unsafe {
            (*entry).kind == ffi::OCERZ_API_SPECIAL
                && !(*entry).handler.is_null()
                && libc::strcmp((*entry).handler, c"fork".as_ptr()) == 0
        }) as c_int
}

#[inline(always)]
unsafe fn ocerz_jit_retire_epoch() -> u64 {
    unsafe {
        (&*ptr::addr_of!(ffi::ocerz_jit_retire_count).cast::<AtomicU64>()).load(Ordering::Acquire)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_fastcall(
    vm: *mut ffi::OcerzVM,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    let epoch = unsafe { ocerz_jit_retire_epoch() };
    let rsp0 = unsafe { (*cpu).gpr[ffi::OCERZ_RSP as usize] };
    unsafe {
        (*cpu).rip = ffi::OCERZ_DYLDAPI_LO as u64 + ffi::OCERZ_BRIDGE_OFF as u64;
        if (*cpu).cc_op != ffi::OCERZ_CC_NONE {
            ffi::ocerz_flags_materialize(cpu);
        }
    }
    let rc = unsafe { vd_dispatch(vm, cpu) };
    let rsp = unsafe { (*cpu).gpr[ffi::OCERZ_RSP as usize] };
    if rc == ffi::OCERZ_STEP_OK as c_int
        && rsp.wrapping_sub(rsp0).wrapping_sub(8) <= 8
        && unsafe { (*cpu).rip == ocerz_ld(rsp.wrapping_sub(8), 8) }
        && unsafe {
            (*cpu).interrupt == 0
                && (*cpu).terminated == 0
                && (*vm).exited == 0
                && (*cpu).suspend_count == 0
                && (*cpu).interp_once == 0
        }
        && unsafe { ocerz_jit_retire_epoch() == epoch }
    {
        return 0;
    }
    if rc == ffi::OCERZ_STEP_EXIT as c_int || rc == ffi::OCERZ_STEP_FATAL as c_int {
        return rc + 1;
    }
    ffi::OCERZ_STEP_OK as c_int + 1
}

type SlotOf = Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> u64>;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_late_fill(
    install_name: *const c_char,
    slot_of: SlotOf,
    ctx: *mut c_void,
) {
    let lib = unsafe { vd_lib(install_name) };
    if lib.is_null() {
        return;
    }
    let api = unsafe { (*lib).api };
    for k in 0..unsafe { (*api).nentries } {
        let e = unsafe { (*api).entries.add(k as usize) };
        if unsafe { (*e).kind != ffi::OCERZ_API_VAR } {
            continue;
        }
        let f = unsafe { vd_filler((*e).filler) };
        if f.is_null() || unsafe { (*f).late.is_none() } {
            continue;
        }
        let slot = slot_of.map_or(0, |f| unsafe { f(ctx, (*e).export_name) });
        if slot != 0 {
            unsafe {
                ((*f).late.unwrap())(
                    slot,
                    (*e).bytes,
                    (*api).install_name,
                    (*e).export_name,
                    Some(ffi::ocerz_bridge_host_symbol),
                )
            };
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vdylib_trampoline(which: u32) -> u64 {
    if which as usize >= VD_NINTERNAL {
        return 0;
    }
    let mut page = G_VD_TRAMP_PAGE.load(Ordering::SeqCst);
    if page != 0 {
        return page + which as u64 * VD_STUB_STRIDE;
    }
    unsafe { libc::pthread_mutex_lock(ptr::addr_of_mut!(G_VD_TRAMP_LOCK)) };
    page = G_VD_TRAMP_PAGE.load(Ordering::SeqCst);
    if page == 0 {
        let made = unsafe {
            ffi::ocerz_map_anywhere(
                ffi::OCERZ_GUEST_PAGE_SIZE as u64,
                libc::PROT_READ | libc::PROT_WRITE,
            )
        };
        if made != 0 {
            let buf = unsafe { ocerz_g2h(made).cast::<u8>() };
            unsafe { libc::memset(buf.cast(), 0xcc, ffi::OCERZ_GUEST_PAGE_SIZE as usize) };
            for k in 0..VD_NINTERNAL as u32 {
                let s = unsafe { buf.add(k as usize * VD_STUB_STRIDE as usize) };
                let slot = VD_TRAMP_SLOTS + k as u64 * VD_SLOT_BYTES;
                unsafe {
                    *s = 0x41;
                    *s.add(1) = 0xbb;
                    wr32(s.add(2), (VD_INTERNAL_ORD << VD_INDEX_BITS) | k);
                    *s.add(6) = 0xff;
                    *s.add(7) = 0x25;
                    wr32(
                        s.add(8),
                        (slot as i64 - (k as u64 * VD_STUB_STRIDE + 12) as i64) as i32 as u32,
                    );
                    wr64(
                        buf.add(slot as usize),
                        ffi::OCERZ_DYLDAPI_LO as u64 + ffi::OCERZ_BRIDGE_OFF as u64,
                    );
                }
            }
            if unsafe {
                ffi::ocerz_protect(
                    made,
                    ffi::OCERZ_GUEST_PAGE_SIZE as u64,
                    libc::PROT_READ | libc::PROT_EXEC,
                )
            } == ffi::OCERZ_OK as c_int
            {
                page = made;
                G_VD_TRAMP_PAGE.store(page, Ordering::SeqCst);
            } else {
                unsafe { ffi::ocerz_unmap(made, ffi::OCERZ_GUEST_PAGE_SIZE as u64) };
            }
        }
        if page == 0 {
            unsafe {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: vdylib: no read-only guest page could be set up for ocerz's own trampolines\n"
                        .as_ptr(),
                )
            };
        }
    }
    unsafe { libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_VD_TRAMP_LOCK)) };
    if page != 0 {
        page + which as u64 * VD_STUB_STRIDE
    } else {
        0
    }
}
