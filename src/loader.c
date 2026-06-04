/*
 * src/loader.c
 *
 * Mach-O image loading for the guest, implementing ocerz_load_image() from
 * loader.h. The file is parsed entirely from its on-disk bytes: the system
 * <mach-o/loader.h> and <mach-o/fat.h> headers are used only for their struct
 * layouts and constant values (the on-disk format is identical regardless of
 * host architecture); no dyld runtime API is touched, because Ocerz must be
 * able to load an x86_64 image on an arm64 host with Rosetta absent.
 *
 * Flow:
 *   1. open the file, read the leading magic.
 *   2. if it is a fat archive (FAT_MAGIC/FAT_CIGAM or the _64 variants),
 *      iterate the fat_arch (or fat_arch_64) table — whose fields are always
 *      big-endian on disk and are byte-swapped here — and select the slice
 *      whose cputype is CPU_TYPE_X86_64. The selected slice's file offset is
 *      remembered and every subsequent file read is relative to it. A thin
 *      file has slice offset 0.
 *   3. parse the slice mach_header_64: magic must be MH_MAGIC_64 (the format
 *      stored is the architecture's native little-endian, so no swapping of
 *      the slice itself), cputype must be x86_64, filetype must be MH_EXECUTE
 *      (MH_DYLINKER is accepted too, for the future dyld-loading phase).
 *   4. walk the load commands within sizeofcmds, bounds-checking every step.
 *      LC_SEGMENT_64 commands are collected to compute the mapped guest range
 *      [vmaddr_lo, vmaddr_hi); __PAGEZERO (vmaddr 0, initprot 0) is skipped.
 *      The fileoff==0 segment (normally __TEXT) gives mh_gaddr. Entry comes
 *      from LC_UNIXTHREAD (raw x86_THREAD_STATE64 rip, entry_is_unixthread=1)
 *      or LC_MAIN (mh_gaddr + entryoff, entry_is_unixthread=0).
 *   5. map the union range ONCE PROT_READ|PROT_WRITE (the mem layer rounds to
 *      16KB and refuses a second anonymous MAP_FIXED into a shared host page),
 *      pread each segment's filesize bytes from the slice into g2h(vmaddr)
 *      (the bss tail is already zero from the anonymous mapping), then apply
 *      each segment's initprot. VM_PROT_* bit values equal PROT_* values, so
 *      initprot passes straight through; the mem layer drops guest PROT_EXEC
 *      to host PROT_READ.
 *
 * slide is 0 in this phase: the arena reservation in mem.c guarantees the
 * canonical [0x100000000, ...) range is free, so images load at their
 * preferred vmaddr. It is stored in the image anyway for the PIE-relocation
 * phase. The thread-state register order for x86_THREAD_STATE64 is
 * rax rbx rcx rdx rdi rsi rbp rsp r8..r15 rip rflags cs fs gs, so rip is the
 * 17th uint64_t (index 16) of the register block that follows flavor/count.
 *
 * Bounds checking is defensive throughout: a malformed file returns
 * OCERZ_EFORMAT (or OCERZ_EIO for read failures) with a clear message rather
 * than reading out of range.
 */
#include "ocerz/loader.h"
#include "ocerz/mem.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach/machine.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#define OCERZ_X86_THREAD_STATE64 4
#define OCERZ_X86_THREAD_STATE64_RIP_INDEX 16

static uint32_t swap32(uint32_t v)
{
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static uint64_t swap64(uint64_t v)
{
    return ((uint64_t)swap32((uint32_t)v) << 32) | swap32((uint32_t)(v >> 32));
}

static int read_exact(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, p + got, len - got, off + (off_t)got);
        if (n < 0)
            return OCERZ_EIO;
        if (n == 0)
            return OCERZ_EFORMAT;
        got += (size_t)n;
    }
    return OCERZ_OK;
}

static int select_fat_slice(int fd, uint32_t magic, uint64_t *slice_off)
{
    int is_64 = (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64);
    struct fat_header fh;
    if (read_exact(fd, &fh, sizeof fh, 0) != OCERZ_OK) {
        OCERZ_FATAL("truncated fat header\n");
        return OCERZ_EFORMAT;
    }
    uint32_t nfat = swap32(fh.nfat_arch);
    if (nfat == 0 || nfat > 64) {
        OCERZ_FATAL("implausible fat arch count %u\n", nfat);
        return OCERZ_EFORMAT;
    }
    off_t off = (off_t)sizeof fh;
    for (uint32_t i = 0; i < nfat; i++) {
        if (is_64) {
            struct fat_arch_64 fa;
            if (read_exact(fd, &fa, sizeof fa, off) != OCERZ_OK)
                return OCERZ_EFORMAT;
            off += (off_t)sizeof fa;
            if ((int32_t)swap32((uint32_t)fa.cputype) == (int32_t)CPU_TYPE_X86_64) {
                *slice_off = swap64(fa.offset);
                return OCERZ_OK;
            }
        } else {
            struct fat_arch fa;
            if (read_exact(fd, &fa, sizeof fa, off) != OCERZ_OK)
                return OCERZ_EFORMAT;
            off += (off_t)sizeof fa;
            if ((int32_t)swap32((uint32_t)fa.cputype) == (int32_t)CPU_TYPE_X86_64) {
                *slice_off = swap32(fa.offset);
                return OCERZ_OK;
            }
        }
    }
    OCERZ_FATAL("fat binary contains no x86_64 slice\n");
    return OCERZ_EFORMAT;
}

int ocerz_load_image(const char *path, OcerzImage *img)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        OCERZ_FATAL("cannot open %s\n", path);
        return OCERZ_EIO;
    }

    memset(img, 0, sizeof *img);

    char real[1024];
    if (realpath(path, real) != NULL) {
        size_t n = strlen(real);
        if (n >= sizeof img->path)
            n = sizeof img->path - 1;
        memcpy(img->path, real, n);
        img->path[n] = '\0';
    } else {
        size_t n = strlen(path);
        if (n >= sizeof img->path)
            n = sizeof img->path - 1;
        memcpy(img->path, path, n);
        img->path[n] = '\0';
    }

    uint32_t magic = 0;
    if (read_exact(fd, &magic, sizeof magic, 0) != OCERZ_OK) {
        OCERZ_FATAL("cannot read magic from %s\n", path);
        close(fd);
        return OCERZ_EFORMAT;
    }

    uint64_t slice_off = 0;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM ||
        magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        int r = select_fat_slice(fd, magic, &slice_off);
        if (r != OCERZ_OK) {
            close(fd);
            return r;
        }
    }

    struct mach_header_64 mh;
    if (read_exact(fd, &mh, sizeof mh, (off_t)slice_off) != OCERZ_OK) {
        OCERZ_FATAL("truncated mach_header_64\n");
        close(fd);
        return OCERZ_EFORMAT;
    }
    if (mh.magic != MH_MAGIC_64) {
        OCERZ_FATAL("not a 64-bit Mach-O (magic %#x)\n", mh.magic);
        close(fd);
        return OCERZ_EFORMAT;
    }
    if (mh.cputype != (cpu_type_t)CPU_TYPE_X86_64) {
        OCERZ_FATAL("not an x86_64 image (cputype %#x)\n", (unsigned)mh.cputype);
        close(fd);
        return OCERZ_EFORMAT;
    }
    if (mh.filetype != MH_EXECUTE && mh.filetype != MH_DYLINKER) {
        OCERZ_FATAL("unsupported filetype %#x (need MH_EXECUTE or MH_DYLINKER)\n",
                    mh.filetype);
        close(fd);
        return OCERZ_EFORMAT;
    }

    img->is_pie = (mh.flags & MH_PIE) != 0;
    img->slide = 0;

    uint32_t sizeofcmds = mh.sizeofcmds;
    if (sizeofcmds == 0 || sizeofcmds > (16u << 20)) {
        OCERZ_FATAL("implausible sizeofcmds %u\n", sizeofcmds);
        close(fd);
        return OCERZ_EFORMAT;
    }
    uint8_t *cmds = (uint8_t *)malloc(sizeofcmds);
    if (cmds == NULL) {
        OCERZ_FATAL("out of memory reading load commands\n");
        close(fd);
        return OCERZ_ENOMEM;
    }
    if (read_exact(fd, cmds, sizeofcmds, (off_t)(slice_off + sizeof mh)) != OCERZ_OK) {
        OCERZ_FATAL("truncated load commands\n");
        free(cmds);
        close(fd);
        return OCERZ_EFORMAT;
    }

    uint64_t vmaddr_lo = ~(uint64_t)0;
    uint64_t vmaddr_hi = 0;
    int have_range = 0;
    int have_mh = 0;
    int entry_found = 0;
    uint64_t entry = 0;
    int entry_is_unixthread = 0;
    uint64_t main_entryoff = 0;
    int have_main = 0;

    uint32_t off = 0;
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        if (off + sizeof(struct load_command) > sizeofcmds) {
            OCERZ_FATAL("load command %u runs past sizeofcmds\n", i);
            free(cmds);
            close(fd);
            return OCERZ_EFORMAT;
        }
        struct load_command lc;
        memcpy(&lc, cmds + off, sizeof lc);
        if (lc.cmdsize < sizeof(struct load_command) ||
            (uint64_t)off + lc.cmdsize > sizeofcmds) {
            OCERZ_FATAL("load command %u has bad cmdsize %u\n", i, lc.cmdsize);
            free(cmds);
            close(fd);
            return OCERZ_EFORMAT;
        }

        if (lc.cmd == LC_SEGMENT_64) {
            if (lc.cmdsize < sizeof(struct segment_command_64)) {
                OCERZ_FATAL("LC_SEGMENT_64 too small\n");
                free(cmds);
                close(fd);
                return OCERZ_EFORMAT;
            }
            struct segment_command_64 sc;
            memcpy(&sc, cmds + off, sizeof sc);
            if (sc.vmaddr == 0 && sc.initprot == 0) {
                off += lc.cmdsize;
                continue;
            }
            if (sc.vmaddr < vmaddr_lo)
                vmaddr_lo = sc.vmaddr;
            if (sc.vmaddr + sc.vmsize > vmaddr_hi)
                vmaddr_hi = sc.vmaddr + sc.vmsize;
            have_range = 1;
            if (sc.fileoff == 0) {
                img->mh_gaddr = sc.vmaddr;
                have_mh = 1;
            }
        } else if (lc.cmd == LC_UNIXTHREAD) {
            uint32_t p = off + (uint32_t)sizeof(struct thread_command);
            while (p + 8 <= off + lc.cmdsize) {
                uint32_t flavor, count;
                memcpy(&flavor, cmds + p, 4);
                memcpy(&count, cmds + p + 4, 4);
                uint64_t body = (uint64_t)p + 8;
                uint64_t regbytes = (uint64_t)count * 4;
                if (body + regbytes > (uint64_t)off + lc.cmdsize) {
                    OCERZ_FATAL("LC_UNIXTHREAD flavor block runs past command\n");
                    free(cmds);
                    close(fd);
                    return OCERZ_EFORMAT;
                }
                if (flavor == OCERZ_X86_THREAD_STATE64) {
                    uint64_t rip_off = body +
                        (uint64_t)OCERZ_X86_THREAD_STATE64_RIP_INDEX * 8;
                    if (rip_off + 8 > (uint64_t)off + lc.cmdsize) {
                        OCERZ_FATAL("x86_THREAD_STATE64 too short for rip\n");
                        free(cmds);
                        close(fd);
                        return OCERZ_EFORMAT;
                    }
                    memcpy(&entry, cmds + rip_off, 8);
                    entry_found = 1;
                    entry_is_unixthread = 1;
                }
                p = (uint32_t)(body + regbytes);
            }
        } else if (lc.cmd == LC_MAIN) {
            if (lc.cmdsize < sizeof(struct entry_point_command)) {
                OCERZ_FATAL("LC_MAIN too small\n");
                free(cmds);
                close(fd);
                return OCERZ_EFORMAT;
            }
            struct entry_point_command ec;
            memcpy(&ec, cmds + off, sizeof ec);
            main_entryoff = ec.entryoff;
            have_main = 1;
        }
        off += lc.cmdsize;
    }

    free(cmds);

    if (!have_range || !have_mh) {
        OCERZ_FATAL("image has no mappable segments or no __TEXT at fileoff 0\n");
        close(fd);
        return OCERZ_EFORMAT;
    }

    if (!entry_found) {
        if (have_main) {
            entry = img->mh_gaddr + main_entryoff;
            entry_is_unixthread = 0;
        } else {
            OCERZ_FATAL("image has neither LC_UNIXTHREAD nor LC_MAIN entry\n");
            close(fd);
            return OCERZ_EFORMAT;
        }
    }

    img->vmaddr_lo = vmaddr_lo + img->slide;
    img->vmaddr_hi = vmaddr_hi + img->slide;
    img->mh_gaddr += img->slide;
    img->entry = entry + img->slide;
    img->entry_is_unixthread = entry_is_unixthread;

    if (ocerz_map_fixed(img->vmaddr_lo, img->vmaddr_hi - img->vmaddr_lo,
                        PROT_READ | PROT_WRITE) != OCERZ_OK) {
        OCERZ_FATAL("cannot map guest range [%#llx, %#llx)\n",
                    (unsigned long long)img->vmaddr_lo,
                    (unsigned long long)img->vmaddr_hi);
        close(fd);
        return OCERZ_ENOMEM;
    }

    uint8_t *cmds2 = (uint8_t *)malloc(sizeofcmds);
    if (cmds2 == NULL) {
        OCERZ_FATAL("out of memory rereading load commands\n");
        close(fd);
        return OCERZ_ENOMEM;
    }
    if (read_exact(fd, cmds2, sizeofcmds, (off_t)(slice_off + sizeof mh)) != OCERZ_OK) {
        free(cmds2);
        close(fd);
        return OCERZ_EFORMAT;
    }

    off = 0;
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        struct load_command lc;
        memcpy(&lc, cmds2 + off, sizeof lc);
        if (lc.cmd == LC_SEGMENT_64) {
            struct segment_command_64 sc;
            memcpy(&sc, cmds2 + off, sizeof sc);
            if (!(sc.vmaddr == 0 && sc.initprot == 0)) {
                uint64_t vmaddr = sc.vmaddr + img->slide;
                if (sc.filesize > 0) {
                    if (read_exact(fd, ocerz_g2h(vmaddr), (size_t)sc.filesize,
                                   (off_t)(slice_off + sc.fileoff)) != OCERZ_OK) {
                        OCERZ_FATAL("cannot read segment data\n");
                        free(cmds2);
                        close(fd);
                        return OCERZ_EIO;
                    }
                }
                OCERZ_LOG("segment %-16.16s vmaddr=%#llx vmsize=%#llx "
                          "fileoff=%#llx filesize=%#llx prot=%c%c%c\n",
                          sc.segname,
                          (unsigned long long)vmaddr,
                          (unsigned long long)sc.vmsize,
                          (unsigned long long)sc.fileoff,
                          (unsigned long long)sc.filesize,
                          (sc.initprot & VM_PROT_READ) ? 'r' : '-',
                          (sc.initprot & VM_PROT_WRITE) ? 'w' : '-',
                          (sc.initprot & VM_PROT_EXECUTE) ? 'x' : '-');
            }
        }
        off += lc.cmdsize;
    }

    for (int writable = 0; writable <= 1; writable++) {
        off = 0;
        for (uint32_t i = 0; i < mh.ncmds; i++) {
            struct load_command lc;
            memcpy(&lc, cmds2 + off, sizeof lc);
            if (lc.cmd == LC_SEGMENT_64) {
                struct segment_command_64 sc;
                memcpy(&sc, cmds2 + off, sizeof sc);
                if (!(sc.vmaddr == 0 && sc.initprot == 0)) {
                    int is_w = (sc.initprot & VM_PROT_WRITE) != 0;
                    if (is_w == writable)
                        ocerz_protect(sc.vmaddr + img->slide, sc.vmsize, sc.initprot);
                }
            }
            off += lc.cmdsize;
        }
    }

    free(cmds2);
    close(fd);

    OCERZ_LOG("loaded %s entry=%#llx mh=%#llx range=[%#llx,%#llx) %s pie=%d\n",
              img->path,
              (unsigned long long)img->entry,
              (unsigned long long)img->mh_gaddr,
              (unsigned long long)img->vmaddr_lo,
              (unsigned long long)img->vmaddr_hi,
              entry_is_unixthread ? "unixthread" : "lc_main",
              img->is_pie);
    return OCERZ_OK;
}
