/*
 * i386 programs that load themselves (include/ocerz/m32.h).
 *
 * A UPX-packed executable has one segment, an LC_UNIXTHREAD entry, no libraries and no dylinker: the kernel
 * starts its stub, which makes raw system calls (sysenter, or int 0x80) to map the program it decompresses,
 * then reads that program's LC_LOAD_DYLINKER, opens /usr/lib/dyld, maps the i386 slice and jumps to its entry
 * with the program's mach header pushed above argc, as the kernel would have.  macOS has no i386 dyld any more,
 * and m32 is its own dyld, so the open is answered with a dyld of m32's making: a file holding one empty
 * segment and a thread state whose eip is the M32_ID_DYLD trap.  The stub maps it and jumps there, and
 * m32_dyld_entry takes the program from the mach header the stub pushed (m32_adopt_program) and starts it as
 * m32_run starts any other: its libraries, its fixups, its initializers, its entry.
 *
 * The raw calls are the few a loader stub makes, with darwin's i386 convention: the number in eax, the
 * arguments above the return address at the user stack (ecx for sysenter, which returns to edx), the result in
 * eax, and on failure the carry flag set and errno in eax.  m32_run keeps the addresses below the packed image
 * free until then, since the program unpacks there.  Kega Fusion 3.63's Mac build is packed this way.
 * OCERZ_M32_NO_SELFLOAD=1 starts such a program as any other, as before 2026-10-16.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

static char g_path[1024];

void m32_selfload_begin(const char *path)
{
    snprintf(g_path, sizeof g_path, "%s", path);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* A file descriptor for m32's dyld: an i386 MH_DYLINKER with one empty 16 KB segment at a free address of the
 * window and an LC_UNIXTHREAD whose eip is the M32_ID_DYLD trap.  The file is unlinked as soon as it is open. */
static int dyld_fd(void)
{
    uint32_t at = m32_map(0x4000);
    if (!at)
        return -1;
    static uint8_t img[0x4000];
    memset(img, 0, sizeof img);
    put32(img + 0, MH_MAGIC);
    put32(img + 4, CPU_TYPE_I386);
    put32(img + 8, CPU_SUBTYPE_I386_ALL);
    put32(img + 12, MH_DYLINKER);
    put32(img + 16, 2);
    put32(img + 20, 56 + 80);
    uint8_t *seg = img + 28;
    put32(seg + 0, LC_SEGMENT);
    put32(seg + 4, 56);
    memcpy(seg + 8, "__TEXT", 6);
    put32(seg + 24, at);
    put32(seg + 28, sizeof img);
    put32(seg + 32, 0);
    put32(seg + 36, sizeof img);
    put32(seg + 40, VM_PROT_READ | VM_PROT_EXECUTE);
    put32(seg + 44, VM_PROT_READ | VM_PROT_EXECUTE);
    uint8_t *thr = seg + 56;
    put32(thr + 0, LC_UNIXTHREAD);
    put32(thr + 4, 80);
    put32(thr + 8, 1);    /* x86_THREAD_STATE32 */
    put32(thr + 12, 16);
    put32(thr + 16 + 4 * 10, M32_TRAP(M32_ID_DYLD));   /* eip */
    char dir[1024], path[1100];
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir))
        snprintf(dir, sizeof dir, "/tmp/");
    snprintf(path, sizeof path, "%socerz-m32-dyld.XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (write(fd, img, sizeof img) != (ssize_t)sizeof img || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int is_dyld(const char *path)
{
    const char *base = strrchr(path, '/');
    return base && !strcmp(base, "/dyld");
}

int m32_raw_syscall(struct OcerzVM *vm, OcerzCPU *cpu, int sysenter)
{
    (void)vm;
    uint32_t nr = (uint32_t)cpu->gpr[OCERZ_RAX] & 0xffff;   /* above it, the arguments' size in bytes */
    uint32_t sp = (uint32_t)cpu->gpr[sysenter ? OCERZ_RCX : OCERZ_RSP];
    uint32_t back = sysenter ? (uint32_t)cpu->gpr[OCERZ_RDX] : (uint32_t)cpu->rip;
#define A(n) m32_rd(sp + 4 + 4 * (n))
    int64_t r = -1;
    errno = 0;
    switch (nr) {
    case 1:     /* exit */
        fflush(NULL);
        exit((int)(A(0) & 0xff));
    case 3:     /* read */
        r = read((int)A(0), m32_h(A(1)), A(2));
        break;
    case 4:     /* write */
        r = write((int)A(0), m32_h(A(1)), A(2));
        break;
    case 5: {   /* open */
        const char *path = (const char *)m32_h(A(0));
        if (!ocerz_addr_readable(A(0))) {
            errno = EFAULT;
            break;
        }
        r = is_dyld(path) ? dyld_fd() : open(path, (int)A(1), (int)A(2));
        if (r < 0 && !errno)
            errno = ENOENT;
        break;
    }
    case 6:     /* close */
        r = close((int)A(0));
        break;
    case 20:    /* getpid */
        r = getpid();
        break;
    case 73:    /* munmap: a partial 4 KB unmap keeps the host page, as the bridged call does */
        if (!(A(0) & 0x3fff))
            ocerz_unmap(A(0), (A(1) + 0x3fffu) & ~0x3fffu);
        r = 0;
        break;
    case 74:    /* mprotect */
    case 75:    /* madvise: accepted without effect, as the bridged calls are */
        r = 0;
        break;
    case 78: {  /* mincore: a page with a mapping in the window is in core, and a range with none is not mapped */
        uint32_t lo = A(0) & ~0xfffu, n = (uint32_t)(((uint64_t)A(1) + (A(0) & 0xfffu) + 0xfffu) >> 12), vec = A(2);
        int any = 0;
        for (uint32_t i = 0; i < n; i++) {
            int in = ocerz_addr_mapped(lo + (uint64_t)i * 0x1000) == 1;
            any |= in;
            if (vec)
                *(uint8_t *)m32_h(vec + i) = (uint8_t)in;
        }
        if (any)
            r = 0;
        else
            errno = ENOMEM;
        break;
    }
    case 153:   /* pread */
        r = pread((int)A(0), m32_h(A(1)), A(2), (off_t)(A(3) | (uint64_t)A(4) << 32));
        break;
    case 197: { /* mmap */
        uint32_t g = m32_mmap(A(0), A(1), (int)A(2), (int)A(3), (int)A(4), A(5) | (uint64_t)A(6) << 32);
        r = g == 0xffffffffu ? -1 : (int64_t)g;
        break;
    }
    case 199:   /* lseek */
        r = lseek((int)A(0), (off_t)(A(1) | (uint64_t)A(2) << 32), (int)A(3));
        break;
    default: {
        char what[64];
        snprintf(what, sizeof what, "%u", (unsigned)nr);
        m32_log_once("raw i386 system call not handled, failing with ENOSYS:", what);
        errno = ENOSYS;
        break;
    }
    }
#undef A
    ocerz_flags_materialize(cpu);
    if (r < 0) {
        cpu->rflags |= OCERZ_CF;
        cpu->gpr[OCERZ_RAX] = (uint32_t)(errno ? errno : EINVAL);
    } else {
        cpu->rflags &= ~OCERZ_CF;
        cpu->gpr[OCERZ_RAX] = (uint32_t)r;
        if (nr == 199)
            cpu->gpr[OCERZ_RDX] = (uint32_t)((uint64_t)r >> 32);
    }
    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = back;
    return OCERZ_STEP_OK;
}

int m32_dyld_entry(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP], mh = m32_rd(esp), sp = esp + 4;
    M32Image *img = g_path[0] ? m32_adopt_program(mh, g_path) : NULL;
    if (!img) {
        fprintf(stderr, "ocerz: m32: %s: what it unpacked at %#x is not an i386 program m32 can load\n",
                g_path[0] ? g_path : "the program", (unsigned)mh);
        ocerz_vm_request_exit(vm, 65);
        return OCERZ_STEP_EXIT;
    }
    OCERZ_LOG("m32: %s unpacked itself: mach header %#x, %d images\n", g_path, (unsigned)mh, m32_nimages);
    if (m32_fixup_all(0)) {
        fprintf(stderr, "ocerz: m32: %s: required imports are unresolved\n", g_path);
        if (!getenv("OCERZ_M32_KEEP_GOING")) {
            ocerz_vm_request_exit(vm, 71);
            return OCERZ_STEP_EXIT;
        }
    }
    if (!img->entry_main && !img->entry_thread) {
        fprintf(stderr, "ocerz: m32: %s has no entry point\n", g_path);
        ocerz_vm_request_exit(vm, 65);
        return OCERZ_STEP_EXIT;
    }
    int rc = m32_start_program(vm, img, sp, g_path);
    if (rc >= 0) {
        ocerz_vm_request_exit(vm, rc);
        return OCERZ_STEP_EXIT;
    }
    return OCERZ_STEP_OK;
}
