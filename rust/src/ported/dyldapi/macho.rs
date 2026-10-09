//! Mach-O load-command records and constants used to inspect mapped dyld images.

use core::ffi::c_char;

#[repr(C)]
pub(crate) struct MachHeader64 {
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
pub(crate) struct LoadCommand {
    pub cmd: u32,
    pub cmdsize: u32,
}

#[repr(C)]
pub(crate) struct SegmentCommand64 {
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
pub(crate) struct Section64 {
    pub sectname: [c_char; 16],
    pub segname: [c_char; 16],
    pub addr: u64,
    pub size: u64,
    pub offset: u32,
    pub align: u32,
    pub reloff: u32,
    pub nreloc: u32,
    pub flags: u32,
    pub reserved1: u32,
    pub reserved2: u32,
    pub reserved3: u32,
}

#[repr(C)]
pub(crate) struct Dylib {
    pub name_offset: u32,
    pub timestamp: u32,
    pub current_version: u32,
    pub compatibility_version: u32,
}

#[repr(C)]
pub(crate) struct DylibCommand {
    pub cmd: u32,
    pub cmdsize: u32,
    pub dylib: Dylib,
}

#[repr(C)]
pub(crate) struct SymtabCommand {
    pub cmd: u32,
    pub cmdsize: u32,
    pub symoff: u32,
    pub nsyms: u32,
    pub stroff: u32,
    pub strsize: u32,
}

#[repr(C)]
pub(crate) struct LinkeditDataCommand {
    pub cmd: u32,
    pub cmdsize: u32,
    pub dataoff: u32,
    pub datasize: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) union NlistUn {
    pub n_strx: u32,
    pub n_value32: u32,
}

#[repr(C)]
pub(crate) struct Nlist64 {
    pub n_un: NlistUn,
    pub n_type: u8,
    pub n_sect: u8,
    pub n_desc: i16,
    pub n_value: u64,
}

const _: () = assert!(core::mem::size_of::<MachHeader64>() == 32);
const _: () = assert!(core::mem::size_of::<LoadCommand>() == 8);
const _: () = assert!(core::mem::size_of::<SegmentCommand64>() == 72);
const _: () = assert!(core::mem::size_of::<Section64>() == 80);
const _: () = assert!(core::mem::size_of::<DylibCommand>() == 24);
const _: () = assert!(core::mem::size_of::<SymtabCommand>() == 24);
const _: () = assert!(core::mem::size_of::<LinkeditDataCommand>() == 16);
const _: () = assert!(core::mem::size_of::<Nlist64>() == 16);

pub(crate) const MH_MAGIC_64: u32 = 0xfeedfacf;
pub(crate) const LC_SEGMENT_64: u32 = 0x19;
pub(crate) const LC_SYMTAB: u32 = 0x2;
pub(crate) const LC_ID_DYLIB: u32 = 0xd;
pub(crate) const LC_LOAD_DYLIB: u32 = 0xc;
pub(crate) const LC_LOAD_WEAK_DYLIB: u32 = 0x80000018;
pub(crate) const LC_REEXPORT_DYLIB: u32 = 0x8000001f;
pub(crate) const LC_LOAD_UPWARD_DYLIB: u32 = 0x80000023;
pub(crate) const LC_VERSION_MIN_MACOSX: u32 = 0x24;
pub(crate) const LC_BUILD_VERSION: u32 = 0x32;
pub(crate) const LC_LAZY_LOAD_DYLIB_INFO: u32 = 0x3a;
pub(crate) const LC_FUNCTION_STARTS: u32 = 0x26;
pub(crate) const N_STAB: u8 = 0xe0;
pub(crate) const N_TYPE: u8 = 0x0e;
pub(crate) const N_SECT: u8 = 0x0e;
