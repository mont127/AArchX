//! Mach-O image loading for the guest: pick the x86_64 slice out of a fat
//! binary, read the load commands, and lay the segments out in guest memory.
//! Reads are done at explicit offsets rather than by mapping the file, so a fat
//! slice needs no separate mapping and a truncated or malformed image fails the
//! read instead of faulting later.

use core::ffi::{c_char, c_int, c_ulonglong, c_void};
use core::{mem, ptr};

use crate::ffi::{
    OCERZ_EFORMAT, OCERZ_EIO, OCERZ_ENOMEM, OCERZ_OK, OcerzImage, ocerz_map_fixed, ocerz_protect,
};
use crate::inline::ocerz_g2h;
use crate::ported::dyldapi::macho::{
    LC_SEGMENT_64, LoadCommand, MH_MAGIC_64, MachHeader64, SegmentCommand64,
};

const CPU_TYPE_X86_64: i32 = 0x0100_0007;
const FAT_MAGIC: u32 = 0xcafe_babe;
const FAT_CIGAM: u32 = 0xbeba_feca;
const FAT_MAGIC_64: u32 = 0xcafe_babf;
const FAT_CIGAM_64: u32 = 0xbfba_feca;
const MH_EXECUTE: c_int = 2;
const MH_DYLINKER: c_int = 7;
const MH_PIE: c_int = 0x0020_0000;
const LC_UNIXTHREAD: c_int = 0x5;
const LC_MAIN: u32 = 0x8000_0028;
const X86_THREAD_STATE64: c_int = 4;
const X86_THREAD_STATE64_RIP_INDEX: c_int = 16;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_PROT_EXECUTE: c_int = 4;

#[repr(C)]
struct FatHeader {
    _magic: u32,
    nfat_arch: u32,
}

#[repr(C)]
struct FatArch {
    cputype: i32,
    _cpusubtype: i32,
    offset: u32,
    _size: u32,
    _align: u32,
}

#[repr(C)]
struct FatArch64 {
    cputype: i32,
    _cpusubtype: i32,
    offset: u64,
    _size: u64,
    _align: u32,
    _reserved: u32,
}

#[repr(C)]
struct EntryPointCommand {
    _cmd: u32,
    _cmdsize: u32,
    entryoff: u64,
    _stacksize: u64,
}

const _: () = assert!(mem::size_of::<FatHeader>() == 8);
const _: () = assert!(mem::size_of::<FatArch>() == 20);
const _: () = assert!(mem::size_of::<FatArch64>() == 32);
const _: () = assert!(mem::size_of::<EntryPointCommand>() == 24);

#[inline]
fn swap32(v: u32) -> u32 {
    ((v & 0x0000_00ff) << 24)
        | ((v & 0x0000_ff00) << 8)
        | ((v & 0x00ff_0000) >> 8)
        | ((v & 0xff00_0000) >> 24)
}

#[inline]
fn swap64(v: u64) -> u64 {
    ((swap32(v as u32) as u64) << 32) | swap32((v >> 32) as u32) as u64
}

unsafe fn read_exact(fd: c_int, buf: *mut c_void, len: usize, off: libc::off_t) -> c_int {
    unsafe {
        let p = buf.cast::<u8>();
        let mut got = 0usize;
        while got < len {
            let n = libc::pread(
                fd,
                p.add(got).cast(),
                len - got,
                off.wrapping_add(got as libc::off_t),
            );
            if n < 0 {
                return OCERZ_EIO;
            }
            if n == 0 {
                return OCERZ_EFORMAT;
            }
            got += n as usize;
        }
        OCERZ_OK
    }
}

unsafe fn close_return(fd: c_int, result: c_int) -> c_int {
    unsafe {
        libc::close(fd);
        result
    }
}

unsafe fn free_close_return(fd: c_int, buf: *mut u8, result: c_int) -> c_int {
    unsafe {
        libc::free(buf.cast());
        libc::close(fd);
        result
    }
}

unsafe fn select_fat_slice(fd: c_int, magic: u32, slice_off: &mut u64) -> c_int {
    unsafe {
        let is_64 = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
        let mut fh: FatHeader = mem::zeroed();
        if read_exact(
            fd,
            (&mut fh as *mut FatHeader).cast(),
            mem::size_of::<FatHeader>(),
            0,
        ) != OCERZ_OK
        {
            crate::ocerz_fatal!("truncated fat header\n");
            return OCERZ_EFORMAT;
        }
        let nfat = swap32(fh.nfat_arch);
        if nfat == 0 || nfat > 64 {
            crate::ocerz_fatal!("implausible fat arch count %u\n", nfat);
            return OCERZ_EFORMAT;
        }
        let mut off = mem::size_of::<FatHeader>() as libc::off_t;
        for _ in 0..nfat {
            if is_64 {
                let mut fa: FatArch64 = mem::zeroed();
                if read_exact(
                    fd,
                    (&mut fa as *mut FatArch64).cast(),
                    mem::size_of::<FatArch64>(),
                    off,
                ) != OCERZ_OK
                {
                    return OCERZ_EFORMAT;
                }
                off = off.wrapping_add(mem::size_of::<FatArch64>() as libc::off_t);
                if swap32(fa.cputype as u32) as i32 == CPU_TYPE_X86_64 {
                    *slice_off = swap64(fa.offset);
                    return OCERZ_OK;
                }
            } else {
                let mut fa: FatArch = mem::zeroed();
                if read_exact(
                    fd,
                    (&mut fa as *mut FatArch).cast(),
                    mem::size_of::<FatArch>(),
                    off,
                ) != OCERZ_OK
                {
                    return OCERZ_EFORMAT;
                }
                off = off.wrapping_add(mem::size_of::<FatArch>() as libc::off_t);
                if swap32(fa.cputype as u32) as i32 == CPU_TYPE_X86_64 {
                    *slice_off = swap32(fa.offset) as u64;
                    return OCERZ_OK;
                }
            }
        }
        crate::ocerz_fatal!("fat binary contains no x86_64 slice\n");
        OCERZ_EFORMAT
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_load_image(path: *const c_char, img: *mut OcerzImage) -> c_int {
    unsafe {
        let fd = libc::open(path, libc::O_RDONLY);
        if fd < 0 {
            crate::ocerz_fatal!("cannot open %s\n", path);
            return OCERZ_EIO;
        }

        libc::memset(img.cast(), 0, mem::size_of::<OcerzImage>());

        let mut real = [0 as c_char; 1024];
        let path_copy = if libc::realpath(path, real.as_mut_ptr()).is_null() {
            path
        } else {
            real.as_ptr()
        };
        let path_len = libc::strlen(path_copy).min((*img).path.len() - 1);
        libc::memcpy((*img).path.as_mut_ptr().cast(), path_copy.cast(), path_len);
        *(*img).path.as_mut_ptr().add(path_len) = 0;

        let mut magic = 0u32;
        if read_exact(
            fd,
            (&mut magic as *mut u32).cast(),
            mem::size_of::<u32>(),
            0,
        ) != OCERZ_OK
        {
            crate::ocerz_fatal!("cannot read magic from %s\n", path);
            return close_return(fd, OCERZ_EFORMAT);
        }

        let mut slice_off = 0u64;
        if magic == FAT_MAGIC
            || magic == FAT_CIGAM
            || magic == FAT_MAGIC_64
            || magic == FAT_CIGAM_64
        {
            let result = select_fat_slice(fd, magic, &mut slice_off);
            if result != OCERZ_OK {
                return close_return(fd, result);
            }
        }

        let mut mh: MachHeader64 = mem::zeroed();
        if read_exact(
            fd,
            (&mut mh as *mut MachHeader64).cast(),
            mem::size_of::<MachHeader64>(),
            slice_off as libc::off_t,
        ) != OCERZ_OK
        {
            crate::ocerz_fatal!("truncated mach_header_64\n");
            return close_return(fd, OCERZ_EFORMAT);
        }
        if mh.magic != MH_MAGIC_64 {
            crate::ocerz_fatal!("not a 64-bit Mach-O (magic %#x)\n", mh.magic);
            return close_return(fd, OCERZ_EFORMAT);
        }
        if mh.cputype != CPU_TYPE_X86_64 {
            crate::ocerz_fatal!("not an x86_64 image (cputype %#x)\n", mh.cputype as u32);
            return close_return(fd, OCERZ_EFORMAT);
        }
        if mh.filetype != MH_EXECUTE as u32 && mh.filetype != MH_DYLINKER as u32 {
            crate::ocerz_fatal!(
                "unsupported filetype %#x (need MH_EXECUTE or MH_DYLINKER)\n",
                mh.filetype
            );
            return close_return(fd, OCERZ_EFORMAT);
        }

        (*img).is_pie = (mh.flags & MH_PIE as u32 != 0) as c_int;
        (*img).slide = 0;

        let sizeofcmds = mh.sizeofcmds;
        if sizeofcmds == 0 || sizeofcmds > (16u32 << 20) {
            crate::ocerz_fatal!("implausible sizeofcmds %u\n", sizeofcmds);
            return close_return(fd, OCERZ_EFORMAT);
        }
        let cmds = libc::malloc(sizeofcmds as usize).cast::<u8>();
        if cmds.is_null() {
            crate::ocerz_fatal!("out of memory reading load commands\n");
            return close_return(fd, OCERZ_ENOMEM);
        }
        if read_exact(
            fd,
            cmds.cast(),
            sizeofcmds as usize,
            slice_off.wrapping_add(mem::size_of::<MachHeader64>() as u64) as libc::off_t,
        ) != OCERZ_OK
        {
            crate::ocerz_fatal!("truncated load commands\n");
            return free_close_return(fd, cmds, OCERZ_EFORMAT);
        }

        let mut vmaddr_lo = u64::MAX;
        let mut vmaddr_hi = 0u64;
        let mut have_range = false;
        let mut have_mh = false;
        let mut entry_found = false;
        let mut entry = 0u64;
        let mut entry_is_unixthread = false;
        let mut main_entryoff = 0u64;
        let mut have_main = false;

        let mut off = 0u32;
        for i in 0..mh.ncmds {
            if (off as u64).wrapping_add(mem::size_of::<LoadCommand>() as u64) > sizeofcmds as u64 {
                crate::ocerz_fatal!("load command %u runs past sizeofcmds\n", i);
                return free_close_return(fd, cmds, OCERZ_EFORMAT);
            }
            let lc = ptr::read_unaligned(cmds.add(off as usize).cast::<LoadCommand>());
            if lc.cmdsize < mem::size_of::<LoadCommand>() as u32
                || (off as u64).wrapping_add(lc.cmdsize as u64) > sizeofcmds as u64
            {
                crate::ocerz_fatal!("load command %u has bad cmdsize %u\n", i, lc.cmdsize);
                return free_close_return(fd, cmds, OCERZ_EFORMAT);
            }

            if lc.cmd == LC_SEGMENT_64 {
                if lc.cmdsize < mem::size_of::<SegmentCommand64>() as u32 {
                    crate::ocerz_fatal!("LC_SEGMENT_64 too small\n");
                    return free_close_return(fd, cmds, OCERZ_EFORMAT);
                }
                let sc = ptr::read_unaligned(cmds.add(off as usize).cast::<SegmentCommand64>());
                if sc.vmaddr == 0 && sc.initprot == 0 {
                    off = off.wrapping_add(lc.cmdsize);
                    continue;
                }
                if sc.vmaddr < vmaddr_lo {
                    vmaddr_lo = sc.vmaddr;
                }
                let segment_hi = sc.vmaddr.wrapping_add(sc.vmsize);
                if segment_hi > vmaddr_hi {
                    vmaddr_hi = segment_hi;
                }
                have_range = true;
                if sc.fileoff == 0 {
                    (*img).mh_gaddr = sc.vmaddr;
                    have_mh = true;
                }
            } else if lc.cmd == LC_UNIXTHREAD as u32 {
                let mut p = off.wrapping_add(mem::size_of::<LoadCommand>() as u32);
                while p.wrapping_add(8) <= off.wrapping_add(lc.cmdsize) {
                    let flavor = ptr::read_unaligned(cmds.add(p as usize).cast::<u32>());
                    let count =
                        ptr::read_unaligned(cmds.add(p.wrapping_add(4) as usize).cast::<u32>());
                    let body = (p as u64).wrapping_add(8);
                    let regbytes = (count as u64).wrapping_mul(4);
                    if body.wrapping_add(regbytes) > (off as u64).wrapping_add(lc.cmdsize as u64) {
                        crate::ocerz_fatal!("LC_UNIXTHREAD flavor block runs past command\n");
                        return free_close_return(fd, cmds, OCERZ_EFORMAT);
                    }
                    if flavor == X86_THREAD_STATE64 as u32 {
                        let rip_off = body
                            .wrapping_add((X86_THREAD_STATE64_RIP_INDEX as u64).wrapping_mul(8));
                        if rip_off.wrapping_add(8) > (off as u64).wrapping_add(lc.cmdsize as u64) {
                            crate::ocerz_fatal!("x86_THREAD_STATE64 too short for rip\n");
                            return free_close_return(fd, cmds, OCERZ_EFORMAT);
                        }
                        entry = ptr::read_unaligned(cmds.add(rip_off as usize).cast::<u64>());
                        entry_found = true;
                        entry_is_unixthread = true;
                    }
                    p = body.wrapping_add(regbytes) as u32;
                }
            } else if lc.cmd == LC_MAIN {
                if lc.cmdsize < mem::size_of::<EntryPointCommand>() as u32 {
                    crate::ocerz_fatal!("LC_MAIN too small\n");
                    return free_close_return(fd, cmds, OCERZ_EFORMAT);
                }
                let ec = ptr::read_unaligned(cmds.add(off as usize).cast::<EntryPointCommand>());
                main_entryoff = ec.entryoff;
                have_main = true;
            }
            off = off.wrapping_add(lc.cmdsize);
        }

        libc::free(cmds.cast());

        if !have_range || !have_mh {
            crate::ocerz_fatal!("image has no mappable segments or no __TEXT at fileoff 0\n");
            return close_return(fd, OCERZ_EFORMAT);
        }

        if !entry_found {
            if have_main {
                entry = (*img).mh_gaddr.wrapping_add(main_entryoff);
                entry_is_unixthread = false;
            } else {
                crate::ocerz_fatal!("image has neither LC_UNIXTHREAD nor LC_MAIN entry\n");
                return close_return(fd, OCERZ_EFORMAT);
            }
        }

        (*img).vmaddr_lo = vmaddr_lo.wrapping_add((*img).slide);
        (*img).vmaddr_hi = vmaddr_hi.wrapping_add((*img).slide);
        (*img).mh_gaddr = (*img).mh_gaddr.wrapping_add((*img).slide);
        (*img).entry = entry.wrapping_add((*img).slide);
        (*img).entry_is_unixthread = entry_is_unixthread as c_int;

        if ocerz_map_fixed(
            (*img).vmaddr_lo,
            (*img).vmaddr_hi.wrapping_sub((*img).vmaddr_lo),
            libc::PROT_READ | libc::PROT_WRITE,
        ) != OCERZ_OK
        {
            crate::ocerz_fatal!(
                "cannot map guest range [%#llx, %#llx)\n",
                (*img).vmaddr_lo as c_ulonglong,
                (*img).vmaddr_hi as c_ulonglong
            );
            return close_return(fd, OCERZ_ENOMEM);
        }

        let cmds2 = libc::malloc(sizeofcmds as usize).cast::<u8>();
        if cmds2.is_null() {
            crate::ocerz_fatal!("out of memory rereading load commands\n");
            return close_return(fd, OCERZ_ENOMEM);
        }
        if read_exact(
            fd,
            cmds2.cast(),
            sizeofcmds as usize,
            slice_off.wrapping_add(mem::size_of::<MachHeader64>() as u64) as libc::off_t,
        ) != OCERZ_OK
        {
            return free_close_return(fd, cmds2, OCERZ_EFORMAT);
        }

        off = 0;
        for _ in 0..mh.ncmds {
            let lc = ptr::read_unaligned(cmds2.add(off as usize).cast::<LoadCommand>());
            if lc.cmd == LC_SEGMENT_64 {
                let sc = ptr::read_unaligned(cmds2.add(off as usize).cast::<SegmentCommand64>());
                if !(sc.vmaddr == 0 && sc.initprot == 0) {
                    let vmaddr = sc.vmaddr.wrapping_add((*img).slide);
                    if sc.filesize > 0
                        && read_exact(
                            fd,
                            ocerz_g2h(vmaddr),
                            sc.filesize as usize,
                            slice_off.wrapping_add(sc.fileoff) as libc::off_t,
                        ) != OCERZ_OK
                    {
                        crate::ocerz_fatal!("cannot read segment data\n");
                        return free_close_return(fd, cmds2, OCERZ_EIO);
                    }
                    crate::ocerz_log!(
                        "segment %-16.16s vmaddr=%#llx vmsize=%#llx fileoff=%#llx filesize=%#llx prot=%c%c%c\n",
                        sc.segname.as_ptr(),
                        vmaddr as c_ulonglong,
                        sc.vmsize as c_ulonglong,
                        sc.fileoff as c_ulonglong,
                        sc.filesize as c_ulonglong,
                        if sc.initprot & VM_PROT_READ != 0 {
                            b'r' as c_int
                        } else {
                            b'-' as c_int
                        },
                        if sc.initprot & VM_PROT_WRITE != 0 {
                            b'w' as c_int
                        } else {
                            b'-' as c_int
                        },
                        if sc.initprot & VM_PROT_EXECUTE != 0 {
                            b'x' as c_int
                        } else {
                            b'-' as c_int
                        }
                    );
                }
            }
            off = off.wrapping_add(lc.cmdsize);
        }

        for writable in 0..=1 {
            off = 0;
            for _ in 0..mh.ncmds {
                let lc = ptr::read_unaligned(cmds2.add(off as usize).cast::<LoadCommand>());
                if lc.cmd == LC_SEGMENT_64 {
                    let sc =
                        ptr::read_unaligned(cmds2.add(off as usize).cast::<SegmentCommand64>());
                    if !(sc.vmaddr == 0 && sc.initprot == 0) {
                        let is_writable = (sc.initprot & VM_PROT_WRITE != 0) as c_int;
                        if is_writable == writable {
                            ocerz_protect(
                                sc.vmaddr.wrapping_add((*img).slide),
                                sc.vmsize,
                                sc.initprot,
                            );
                        }
                    }
                }
                off = off.wrapping_add(lc.cmdsize);
            }
        }

        libc::free(cmds2.cast());
        libc::close(fd);

        crate::ocerz_log!(
            "loaded %s entry=%#llx mh=%#llx range=[%#llx,%#llx) %s pie=%d\n",
            (*img).path.as_ptr(),
            (*img).entry as c_ulonglong,
            (*img).mh_gaddr as c_ulonglong,
            (*img).vmaddr_lo as c_ulonglong,
            (*img).vmaddr_hi as c_ulonglong,
            if entry_is_unixthread {
                c"unixthread".as_ptr()
            } else {
                c"lc_main".as_ptr()
            },
            (*img).is_pie
        );
        OCERZ_OK
    }
}
