/*
 * m32's hand-written libSystem entry points (include/ocerz/m32.h): the ones whose 32-bit and 64-bit
 * forms differ in layout, that allocate for the caller, that take variadic arguments, or that exist only on i386.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <wchar.h>
#include <math.h>
#include <netdb.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "ocerz/decode.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_objc.h"
#include "ocerz/syscall.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"
#include "ocerz/x87.h"

/* OCERZ_M32LOG=calls or exit: who ended the process (games quit quietly when a check fails) */
static void log_exit(OcerzCPU *cpu, const char *how)
{
    const char *l = getenv("OCERZ_M32LOG");
    if (!l || (!strstr(l, "calls") && !strstr(l, "exit")))
        return;
    uint32_t bp = (uint32_t)cpu->gpr[OCERZ_RBP];
    fprintf(stderr, "ocerz: m32: guest %s(%d) <- %#x frames", how, (int)m32_arg(cpu, 0), m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    for (int i = 0; i < 12 && bp && bp < M32_HANDLE_LO && !(bp & 3); i++, bp = m32_rd(bp))
        fprintf(stderr, " %#x", m32_rd(bp + 4));
    fprintf(stderr, "\n");
    if (cpu->btrace) {   /* OCERZ_BTRACE=1: the blocks that led here, oldest first */
        uint32_t n = cpu->btrace_n, m = cpu->btrace_mask, k = n > 4000 ? 4000 : n;
        fprintf(stderr, "ocerz: m32: btrace");
        for (uint32_t i = n - k; i != n; i++)
            fprintf(stderr, " %llx", (unsigned long long)cpu->btrace[i & m]);
        fprintf(stderr, "\n");
    }
}

static int sp_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_exit(cpu, "exit");
    fflush(NULL);
    exit((int)(m32_arg(cpu, 0) & 0xff));
}

static int sp__exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    log_exit(cpu, "_exit");
    fflush(NULL);
    _exit((int)(m32_arg(cpu, 0) & 0xff));
}

/* SetFrontProcess(&psn) is how a game started by a script (Left 4 Dead 2 from Steam, through hl2.sh) came to the
 * front.  Today's cooperative activation ignores it while another app is active, so the game started unfocused:
 * throttled by engine_no_focus_sleep and without the mouse until the user switched away and back.  For the current
 * process the call still goes through, and the app also asks to activate once its run loop turns, which is honored. */
static int sp_SetFrontProcess(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static short (*set_front)(const void *), (*same)(const void *, const void *, unsigned char *);
    if (!set_front) {
        same = (short (*)(const void *, const void *, unsigned char *))dlsym(RTLD_DEFAULT, "SameProcess");
        set_front = (short (*)(const void *))dlsym(RTLD_DEFAULT, "SetFrontProcess");
    }
    const uint32_t *psn = m32_h(m32_arg(cpu, 0)), current[2] = { 0, 2 };   /* kCurrentProcess */
    unsigned char self = psn[0] == 0 && psn[1] == 2;
    if (!self && same && same(psn, current, &self) != 0)
        self = 0;
    if (self)
        m32_activate_soon();
    m32_ret(cpu, (uint32_t)(int32_t)(set_front ? set_front(psn) : 0), 0, 0);
    return OCERZ_STEP_OK;
}

/* CoreGraphics' private process calls SDL 1.2's main makes a program started outside a bundle a foreground app with:
 * CPSGetCurrentProcess is GetCurrentProcess (a CPSProcessSerNum is a ProcessSerialNumber), CPSEnableForegroundOperation
 * TransformProcessType to a foreground application, and CPSSetFrontProcess SetFrontProcess.  m32 had no declaration
 * for them and answered 0 without doing anything, so such a program stayed in the background without a menu bar. */
static int sp_CPSGetCurrentProcess(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static short (*current)(void *);
    if (!current)
        current = (short (*)(void *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
    uint32_t psn = m32_arg(cpu, 0);
    m32_ret(cpu, psn && current ? (uint32_t)(int32_t)current(m32_h(psn)) : (uint32_t)-50, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_CPSEnableForegroundOperation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static int32_t (*transform)(const void *, uint32_t);
    if (!transform)
        transform = (int32_t (*)(const void *, uint32_t))dlsym(RTLD_DEFAULT, "TransformProcessType");
    uint32_t psn = m32_arg(cpu, 0);
    if (psn && transform)
        transform(m32_h(psn), 1);   /* kProcessTransformToForegroundApplication; already one is no error to SDL */
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

static int sp_ok0(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

static int sp_stub_binder(struct OcerzVM *vm, OcerzCPU *cpu)
{
    fprintf(stderr, "ocerz: m32: dyld_stub_binder reached: an import was left lazy\n");
    ocerz_vm_request_exit(vm, 134);
    return OCERZ_STEP_EXIT;
}


/* ---- compiler-rt: i386 has no 64-bit divide, and these live in libSystem there ---- */
#define ARG64(n) m32_arg64(cpu, n)
static double argd(OcerzCPU *cpu, int n) { uint64_t v = ARG64(n); double d; memcpy(&d, &v, 8); return d; }
static float argf(OcerzCPU *cpu, int n) { uint32_t v = m32_arg(cpu, n); float f; memcpy(&f, &v, 4); return f; }
#define RET64(v) (m32_ret64(cpu, (uint64_t)(v)), OCERZ_STEP_OK)
#define RETV_PLAIN(v) (m32_ret(cpu, (uint32_t)(v), 0, 0), OCERZ_STEP_OK)
static int sp_udivdi3(struct OcerzVM *vm, OcerzCPU *cpu) { uint64_t b = ARG64(2); return RET64(b ? ARG64(0) / b : 0); }
static int sp_umoddi3(struct OcerzVM *vm, OcerzCPU *cpu) { uint64_t b = ARG64(2); return RET64(b ? ARG64(0) % b : 0); }
static int sp_divdi3(struct OcerzVM *vm, OcerzCPU *cpu) { int64_t b = (int64_t)ARG64(2); return RET64(b ? (int64_t)ARG64(0) / b : 0); }
static int sp_moddi3(struct OcerzVM *vm, OcerzCPU *cpu) { int64_t b = (int64_t)ARG64(2); return RET64(b ? (int64_t)ARG64(0) % b : 0); }
static int sp_muldi3(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64(ARG64(0) * ARG64(2)); }
static int sp_ashldi3(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64(ARG64(0) << (m32_arg(cpu, 2) & 63)); }
static int sp_ashrdi3(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64((int64_t)ARG64(0) >> (m32_arg(cpu, 2) & 63)); }
static int sp_lshrdi3(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64(ARG64(0) >> (m32_arg(cpu, 2) & 63)); }
static int sp_cmpdi2(struct OcerzVM *vm, OcerzCPU *cpu) { int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2); m32_ret(cpu, a < b ? 0 : a == b ? 1 : 2, 0, 0); return OCERZ_STEP_OK; }
static int sp_ucmpdi2(struct OcerzVM *vm, OcerzCPU *cpu) { uint64_t a = ARG64(0), b = ARG64(2); m32_ret(cpu, a < b ? 0 : a == b ? 1 : 2, 0, 0); return OCERZ_STEP_OK; }
static int sp_fixunsdfdi(struct OcerzVM *vm, OcerzCPU *cpu) { double d = argd(cpu, 0); return RET64(d <= 0 ? 0 : (uint64_t)d); }
static int sp_fixunssfdi(struct OcerzVM *vm, OcerzCPU *cpu) { float f = argf(cpu, 0); return RET64(f <= 0 ? 0 : (uint64_t)f); }
static int sp_fixdfdi(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64((int64_t)argd(cpu, 0)); }
static int sp_fixsfdi(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64((int64_t)argf(cpu, 0)); }
static int sp_floatdidf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, (double)(int64_t)ARG64(0)); return OCERZ_STEP_OK; }
static int sp_floatundidf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, (double)ARG64(0)); return OCERZ_STEP_OK; }
static int sp_floatdisf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, (float)(int64_t)ARG64(0)); return OCERZ_STEP_OK; }
static int sp_floatundisf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, (float)ARG64(0)); return OCERZ_STEP_OK; }
static int sp_bzero(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t p = m32_arg(cpu, 0), n = m32_arg(cpu, 1);
    if (p && n)
        memset(m32_h(p), 0, n);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* _FORTIFY_SOURCE entry points: the object-size argument only matters for the check, which the guest compiled in */
static int sp_memcpy_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    if (n)
        memmove(m32_h(d), m32_h(s), n);
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_memset_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), c = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    if (n)
        memset(m32_h(d), (int)c, n);
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strcpy_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1);
    memmove(m32_h(d), m32_h(s), strlen(m32_h(s)) + 1);
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_stpcpy_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1), n = (uint32_t)strlen(m32_h(s));
    memmove(m32_h(d), m32_h(s), n + 1);
    m32_ret(cpu, d + n, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strcat_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1);
    strcat(m32_h(d), m32_h(s));
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strncpy_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    strncpy(m32_h(d), m32_h(s), n);
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strncat_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t d = m32_arg(cpu, 0), s = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    strncat(m32_h(d), m32_h(s), n);
    m32_ret(cpu, d, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strlcpy_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_ret(cpu, (uint32_t)strlcpy(m32_h(m32_arg(cpu, 0)), m32_h(m32_arg(cpu, 1)), m32_arg(cpu, 2)), 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_strlcat_chk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_ret(cpu, (uint32_t)strlcat(m32_h(m32_arg(cpu, 0)), m32_h(m32_arg(cpu, 1)), m32_arg(cpu, 2)), 0, 0);
    return OCERZ_STEP_OK;
}

/* ---- 32-bit layouts of file and time structures (offsets measured with clang -arch i386, 10.8 SDK rules) ---- */

#define RETV(v) do { uint32_t r_ = (uint32_t)(v); m32_save_errno(cpu); m32_ret(cpu, r_, 0, 0); return OCERZ_STEP_OK; } while (0)

static void w16(uint32_t g, uint16_t v) { memcpy(m32_h(g), &v, 2); }
static void w64(uint32_t g, uint64_t v) { memcpy(m32_h(g), &v, 8); }
static void ts_out(uint32_t g, struct timespec t) { m32_wr(g, (uint32_t)t.tv_sec); m32_wr(g + 4, (uint32_t)t.tv_nsec); }

/* struct stat with 64-bit inodes ($INODE64, stat64): 108 bytes; the old i386 struct stat: 96 bytes */
static void stat_out(uint32_t g, const struct stat *h, int inode64)
{
    memset(m32_h(g), 0, inode64 ? 108 : 96);
    if (inode64) {
        m32_wr(g, (uint32_t)h->st_dev); w16(g + 4, h->st_mode); w16(g + 6, h->st_nlink); w64(g + 8, h->st_ino);
        m32_wr(g + 16, h->st_uid); m32_wr(g + 20, h->st_gid); m32_wr(g + 24, (uint32_t)h->st_rdev);
        ts_out(g + 28, h->st_atimespec); ts_out(g + 36, h->st_mtimespec); ts_out(g + 44, h->st_ctimespec);
        ts_out(g + 52, h->st_birthtimespec); w64(g + 60, (uint64_t)h->st_size); w64(g + 68, (uint64_t)h->st_blocks);
        m32_wr(g + 76, (uint32_t)h->st_blksize); m32_wr(g + 80, h->st_flags); m32_wr(g + 84, h->st_gen);
    } else {
        m32_wr(g, (uint32_t)h->st_dev); m32_wr(g + 4, (uint32_t)h->st_ino); w16(g + 8, h->st_mode); w16(g + 10, h->st_nlink);
        m32_wr(g + 12, h->st_uid); m32_wr(g + 16, h->st_gid); m32_wr(g + 20, (uint32_t)h->st_rdev);
        ts_out(g + 24, h->st_atimespec); ts_out(g + 32, h->st_mtimespec); ts_out(g + 40, h->st_ctimespec);
        w64(g + 48, (uint64_t)h->st_size); w64(g + 56, (uint64_t)h->st_blocks); m32_wr(g + 64, (uint32_t)h->st_blksize);
        m32_wr(g + 68, h->st_flags); m32_wr(g + 72, h->st_gen);
    }
}
static int stat_common(OcerzCPU *cpu, int which, int inode64)
{
    struct stat st;
    uint32_t a = m32_arg(cpu, 0), out = m32_arg(cpu, 1);
    int r = which == 2 ? fstat((int)a, &st) : which == 1 ? lstat(m32_h(a), &st) : stat(m32_h(a), &st);
    if (r == 0 && out)
        stat_out(out, &st, inode64);
    RETV(r);
}
static int sp_stat(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 0, 0); }
static int sp_lstat(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 1, 0); }
static int sp_fstat(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 2, 0); }
static int sp_stat64(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 0, 1); }
static int sp_lstat64(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 1, 1); }
static int sp_fstat64(struct OcerzVM *vm, OcerzCPU *cpu) { return stat_common(cpu, 2, 1); }

/* struct statfs: $INODE64's fixed-size layout is the host's (2168 bytes). The legacy one is 272 bytes: f_otype and
 * f_oflags shorts, seven longs from f_bsize @4, f_fsid @32, f_owner @40, f_type short @46, f_flags @48,
 * f_fstypename[15] @60, f_mntonname[90] @75, f_mntfromname[90] @165 */
static int statfs_common(OcerzCPU *cpu, int fd, int inode64)
{
    struct statfs h;
    uint32_t a = m32_arg(cpu, 0), g = m32_arg(cpu, 1);
    int r = fd ? fstatfs((int)a, &h) : statfs(m32_h(a), &h);
    if (r == 0 && g && inode64)
        memcpy(m32_h(g), &h, sizeof h);
    else if (r == 0 && g) {
        uint32_t v[7] = { h.f_bsize, (uint32_t)h.f_iosize, (uint32_t)h.f_blocks, (uint32_t)h.f_bfree,
                          (uint32_t)h.f_bavail, (uint32_t)h.f_files, (uint32_t)h.f_ffree };
        memset(m32_h(g), 0, 272);
        w16(g, (uint16_t)h.f_type);
        w16(g + 2, (uint16_t)h.f_flags);
        memcpy(m32_h(g + 4), v, sizeof v);
        memcpy(m32_h(g + 32), &h.f_fsid, 8);
        m32_wr(g + 40, h.f_owner);
        w16(g + 46, (uint16_t)h.f_type);
        m32_wr(g + 48, h.f_flags);
        strlcpy(m32_h(g + 60), h.f_fstypename, 15);
        strlcpy(m32_h(g + 75), h.f_mntonname, 90);
        strlcpy(m32_h(g + 165), h.f_mntfromname, 90);
    }
    RETV(r);
}
static int sp_statfs(struct OcerzVM *vm, OcerzCPU *cpu) { return statfs_common(cpu, 0, 0); }
static int sp_fstatfs(struct OcerzVM *vm, OcerzCPU *cpu) { return statfs_common(cpu, 1, 0); }
static int sp_statfs64(struct OcerzVM *vm, OcerzCPU *cpu) { return statfs_common(cpu, 0, 1); }
static int sp_fstatfs64(struct OcerzVM *vm, OcerzCPU *cpu) { return statfs_common(cpu, 1, 1); }

/* readdir hands back a buffer that lives until the next readdir on the same DIR: one guest buffer per DIR */
static pthread_mutex_t g_dir_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *dir; uint32_t buf; } g_dirbufs[256];
static uint32_t dir_buffer(void *dir)
{
    pthread_mutex_lock(&g_dir_lock);
    uint32_t b = 0;
    int freeslot = -1;
    for (int i = 0; i < 256 && !b; i++) {
        if (g_dirbufs[i].dir == dir) b = g_dirbufs[i].buf;
        else if (!g_dirbufs[i].dir && freeslot < 0) freeslot = i;
    }
    if (!b && freeslot >= 0) {
        b = g_dirbufs[freeslot].buf ? g_dirbufs[freeslot].buf : m32_static_alloc(1048, 8);
        g_dirbufs[freeslot].dir = dir;
        g_dirbufs[freeslot].buf = b;
    }
    pthread_mutex_unlock(&g_dir_lock);
    return b;
}
static void dirent_out(uint32_t g, const struct dirent *e, int inode64)
{
    if (inode64) {   /* d_ino u64 @0, d_seekoff u64 @8, d_reclen @16, d_namlen @18, d_type @20, d_name[1024] @21 */
        w64(g, e->d_ino); w64(g + 8, e->d_seekoff); w16(g + 16, 1048); w16(g + 18, e->d_namlen);
        *(uint8_t *)m32_h(g + 20) = e->d_type;
        memcpy(m32_h(g + 21), e->d_name, (size_t)e->d_namlen + 1);
    } else {         /* d_ino u32 @0, d_reclen @4, d_type @6, d_namlen u8 @7, d_name[256] @8 */
        m32_wr(g, (uint32_t)e->d_ino); w16(g + 4, 264); *(uint8_t *)m32_h(g + 6) = e->d_type;
        uint8_t n = e->d_namlen > 255 ? 255 : (uint8_t)e->d_namlen;
        *(uint8_t *)m32_h(g + 7) = n;
        memcpy(m32_h(g + 8), e->d_name, n);
        *(char *)m32_h(g + 8 + n) = 0;
    }
}
static int readdir_common(OcerzCPU *cpu, int inode64)
{
    void *dir = m32_host(m32_arg(cpu, 0));
    struct dirent *e = dir ? readdir(dir) : NULL;
    uint32_t b = e ? dir_buffer(dir) : 0;
    if (b)
        dirent_out(b, e, inode64);
    RETV(b);
}
static int sp_readdir(struct OcerzVM *vm, OcerzCPU *cpu) { return readdir_common(cpu, 0); }
static int sp_readdir64(struct OcerzVM *vm, OcerzCPU *cpu) { return readdir_common(cpu, 1); }
static int readdir_r_common(OcerzCPU *cpu, int inode64)
{
    void *dir = m32_host(m32_arg(cpu, 0));
    uint32_t entry = m32_arg(cpu, 1), result = m32_arg(cpu, 2);
    struct dirent *e = dir ? readdir(dir) : NULL;   /* ponytail: one DIR per thread, as every caller uses it */
    if (e)
        dirent_out(entry, e, inode64);
    m32_wr(result, e ? entry : 0);
    RETV(0);
}
static int sp_readdir_r(struct OcerzVM *vm, OcerzCPU *cpu) { return readdir_r_common(cpu, 0); }
static int sp_readdir_r64(struct OcerzVM *vm, OcerzCPU *cpu) { return readdir_r_common(cpu, 1); }
/* scandir: the entries are guest dirents on the guest heap (the caller frees each one and the list), kept and
 * ordered by the guest's own select and compar; compar reads two dirent pointers from guest slots */
struct ScandirCmp { struct OcerzVM *vm; uint32_t fn, slots; };
static int scandir_cmp(void *t, const void *a, const void *b)
{
    struct ScandirCmp *c = t;
    m32_wr(c->slots, *(const uint32_t *)a);
    m32_wr(c->slots + 4, *(const uint32_t *)b);
    uint32_t args[2] = { c->slots, c->slots + 4 };
    return (int32_t)m32_call(c->vm, c->fn, args, 2, NULL, NULL);
}
static int scandir_common(struct OcerzVM *vm, OcerzCPU *cpu, int inode64)
{
    uint32_t path = m32_arg(cpu, 0), out = m32_arg(cpu, 1), sel = m32_arg(cpu, 2), cmp = m32_arg(cpu, 3);
    DIR *d = path ? opendir((const char *)m32_h(path)) : NULL;
    if (!d)
        RETV(-1);
    uint32_t *v = NULL, n = 0, cap = 0;
    for (struct dirent *e; (e = readdir(d));) {
        uint32_t g = m32_malloc(inode64 ? 1048 : 264);
        if (!g)
            break;
        dirent_out(g, e, inode64);
        if (sel && !m32_call(vm, sel, &g, 1, NULL, NULL)) {
            m32_free(g);
            continue;
        }
        if (n == cap && !(v = realloc(v, (cap = cap ? cap * 2 : 64) * sizeof *v)))
            break;
        v[n++] = g;
    }
    closedir(d);
    if (cmp && n > 1) {
        struct ScandirCmp c = { vm, cmp, m32_malloc(8) };
        qsort_r(v, n, sizeof *v, &c, scandir_cmp);
        m32_free(c.slots);
    }
    uint32_t list = m32_malloc(4 * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++)
        m32_wr(list + 4 * i, v[i]);
    free(v);
    m32_wr(out, list);
    errno = 0;
    RETV(n);
}
static int sp_scandir(struct OcerzVM *vm, OcerzCPU *cpu) { return scandir_common(vm, cpu, 0); }
static int sp_scandir64(struct OcerzVM *vm, OcerzCPU *cpu) { return scandir_common(vm, cpu, 1); }
static int alphasort_common(OcerzCPU *cpu, uint32_t name)
{
    const char *a = (const char *)m32_h(m32_rd(m32_arg(cpu, 0)) + name);
    const char *b = (const char *)m32_h(m32_rd(m32_arg(cpu, 1)) + name);
    return RETV_PLAIN(strcoll(a, b));
}
static int sp_alphasort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return alphasort_common(cpu, 8); }
static int sp_alphasort64(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return alphasort_common(cpu, 21); }
static int sp_closedir(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *dir = m32_host(m32_arg(cpu, 0));
    pthread_mutex_lock(&g_dir_lock);
    for (int i = 0; i < 256; i++)
        if (g_dirbufs[i].dir == dir)
            g_dirbufs[i].dir = NULL;   /* the buffer is reused by the next opendir */
    pthread_mutex_unlock(&g_dir_lock);
    RETV(dir ? closedir(dir) : -1);
}

/* struct tm: nine ints, long tm_gmtoff @36, char *tm_zone @40 (44 bytes) */
static void tm_out(uint32_t g, const struct tm *h)
{
    memcpy(m32_h(g), h, 36);
    m32_wr(g + 36, (uint32_t)h->tm_gmtoff);
    m32_wr(g + 40, m32_cstring(h->tm_zone));
}
static void tm_in(uint32_t g, struct tm *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h, m32_h(g), 36);
    h->tm_gmtoff = (int32_t)m32_rd(g + 36);
    uint32_t z = m32_rd(g + 40);
    h->tm_zone = z ? m32_h(z) : NULL;
}
static time_t time_in(uint32_t g) { return (time_t)(int32_t)m32_rd(g); }
static __thread uint32_t t_tm;   /* gmtime/localtime's static result, per thread */
static int sp_gmtime_r(struct OcerzVM *vm, OcerzCPU *cpu)
{
    time_t t = time_in(m32_arg(cpu, 0));
    struct tm h;
    uint32_t out = m32_arg(cpu, 1);
    if (!gmtime_r(&t, &h)) RETV(0);
    tm_out(out, &h);
    RETV(out);
}
static int sp_localtime_r(struct OcerzVM *vm, OcerzCPU *cpu)
{
    time_t t = time_in(m32_arg(cpu, 0));
    struct tm h;
    uint32_t out = m32_arg(cpu, 1);
    if (!localtime_r(&t, &h)) RETV(0);
    tm_out(out, &h);
    RETV(out);
}
static int sp_gmtime(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!t_tm) t_tm = m32_static_alloc(44, 4);
    time_t t = time_in(m32_arg(cpu, 0));
    struct tm h;
    if (!gmtime_r(&t, &h)) RETV(0);
    tm_out(t_tm, &h);
    RETV(t_tm);
}
static int sp_localtime(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!t_tm) t_tm = m32_static_alloc(44, 4);
    time_t t = time_in(m32_arg(cpu, 0));
    struct tm h;
    if (!localtime_r(&t, &h)) RETV(0);
    tm_out(t_tm, &h);
    RETV(t_tm);
}
static int mktime_common(OcerzCPU *cpu, int gm)
{
    struct tm h;
    uint32_t g = m32_arg(cpu, 0);
    tm_in(g, &h);
    time_t t = gm ? timegm(&h) : mktime(&h);
    tm_out(g, &h);   /* both normalize the structure */
    RETV((uint32_t)(int32_t)t);
}
static int sp_mktime(struct OcerzVM *vm, OcerzCPU *cpu) { return mktime_common(cpu, 0); }
static int sp_timegm(struct OcerzVM *vm, OcerzCPU *cpu) { return mktime_common(cpu, 1); }
static int sp_strftime(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct tm h;
    tm_in(m32_arg(cpu, 3), &h);
    RETV(strftime(m32_h(m32_arg(cpu, 0)), m32_arg(cpu, 1), m32_h(m32_arg(cpu, 2)), &h));
}
static int sp_asctime(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct tm h;
    tm_in(m32_arg(cpu, 0), &h);
    RETV(m32_cstring(asctime(&h)));
}
static int sp_ctime(struct OcerzVM *vm, OcerzCPU *cpu)
{
    time_t t = time_in(m32_arg(cpu, 0));
    RETV(m32_cstring(ctime(&t)));
}
static int sp_gettimeofday(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct timeval tv;
    int r = gettimeofday(&tv, NULL);
    uint32_t g = m32_arg(cpu, 0);
    if (!r && g) {
        m32_wr(g, (uint32_t)tv.tv_sec);
        m32_wr(g + 4, (uint32_t)tv.tv_usec);
    }
    RETV(r);
}
/* struct rusage: two 8-byte timevals, then fourteen 4-byte longs (72 bytes) */
static int sp_getrusage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct rusage ru;
    int r = getrusage((int)m32_arg(cpu, 0), &ru);
    uint32_t g = m32_arg(cpu, 1);
    if (!r && g) {
        const long *rest = &ru.ru_maxrss;
        m32_wr(g, (uint32_t)ru.ru_utime.tv_sec);
        m32_wr(g + 4, (uint32_t)ru.ru_utime.tv_usec);
        m32_wr(g + 8, (uint32_t)ru.ru_stime.tv_sec);
        m32_wr(g + 12, (uint32_t)ru.ru_stime.tv_usec);
        for (int i = 0; i < 14; i++)
            m32_wr(g + 16 + 4 * i, (uint32_t)rest[i]);
    }
    RETV(r);
}
static int sp_nanosleep(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t a = m32_arg(cpu, 0), b = m32_arg(cpu, 1);
    struct timespec want = { (int32_t)m32_rd(a), (int32_t)m32_rd(a + 4) }, left = { 0, 0 };
    int r = nanosleep(&want, &left);
    if (b)
        ts_out(b, left);
    RETV(r);
}
/* POSIX aio on the guest's i386 struct aiocb (48 bytes: fildes 0, off_t offset 4, buf 12, nbytes 16, reqprio 20,
 * sigevent 24, lio_opcode 44) through a host aiocb kept per guest one until aio_return: the host layout (80 bytes)
 * differs, and handing the guest's to the kernel had it read file data into whatever its buf/nbytes offsets held.
 * ponytail: completion notification is SIGEV_NONE (games poll with aio_error/aio_suspend); add signal or thread
 * delivery when one waits for it. */
#include <aio.h>
typedef struct AioSlot { uint32_t g; struct aiocb *h; } AioSlot;
static AioSlot g_aio[1024];
static pthread_mutex_t g_aio_lock = PTHREAD_MUTEX_INITIALIZER;

static struct aiocb *aio_host(uint32_t g, int create)
{
    struct aiocb *h = NULL;
    pthread_mutex_lock(&g_aio_lock);
    int free_at = -1;
    for (int i = 0; i < 1024 && !h; i++) {
        if (g_aio[i].g == g)
            h = g_aio[i].h;
        else if (!g_aio[i].g && free_at < 0)
            free_at = i;
    }
    if (!h && create && free_at >= 0) {
        h = calloc(1, sizeof *h);
        g_aio[free_at] = (AioSlot){ g, h };
    }
    if (h && create) {   /* (re)load the request from the guest's control block */
        memset(h, 0, sizeof *h);
        h->aio_fildes = (int)m32_rd(g);
        h->aio_offset = (off_t)((uint64_t)m32_rd(g + 4) | (uint64_t)m32_rd(g + 8) << 32);
        h->aio_buf = m32_h(m32_rd(g + 12));
        h->aio_nbytes = m32_rd(g + 16);
        h->aio_reqprio = (int)m32_rd(g + 20);
        h->aio_sigevent.sigev_notify = SIGEV_NONE;
        h->aio_lio_opcode = (int)m32_rd(g + 44);
    }
    pthread_mutex_unlock(&g_aio_lock);
    return h;
}
static void aio_forget(uint32_t g)
{
    pthread_mutex_lock(&g_aio_lock);
    for (int i = 0; i < 1024; i++)
        if (g_aio[i].g == g) {
            free(g_aio[i].h);
            g_aio[i] = (AioSlot){ 0, NULL };
        }
    pthread_mutex_unlock(&g_aio_lock);
}
static int aio_start(OcerzCPU *cpu, int (*f)(struct aiocb *))
{
    struct aiocb *h = aio_host(m32_arg(cpu, 0), 1);
    if (!h) {
        errno = EAGAIN;
        RETV(-1);
    }
    RETV(f(h));
}
static int sp_aio_read(struct OcerzVM *vm, OcerzCPU *cpu) { return aio_start(cpu, aio_read); }
static int sp_aio_write(struct OcerzVM *vm, OcerzCPU *cpu) { return aio_start(cpu, aio_write); }
static int sp_aio_error(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct aiocb *h = aio_host(m32_arg(cpu, 0), 0);
    if (!h) {
        errno = EINVAL;
        RETV(-1);
    }
    RETV(aio_error(h));
}
static int sp_aio_return(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0);
    struct aiocb *h = aio_host(g, 0);
    if (!h) {
        errno = EINVAL;
        RETV(-1);
    }
    ssize_t r = aio_return(h);
    aio_forget(g);
    RETV((uint32_t)r);
}
static int sp_aio_cancel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 1);
    RETV(aio_cancel((int)m32_arg(cpu, 0), g ? aio_host(g, 0) : NULL));
}
/* aio_suspend(const struct aiocb *const list[], int n, const struct timespec *timeout) */
static int sp_aio_suspend(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t list = m32_arg(cpu, 0), t = m32_arg(cpu, 2);
    int n = (int)m32_arg(cpu, 1);
    if (n < 0 || n > 1024) {
        errno = EINVAL;
        RETV(-1);
    }
    const struct aiocb *hl[1024];
    for (int i = 0; i < n; i++) {
        uint32_t g = m32_rd(list + 4u * (uint32_t)i);
        hl[i] = g ? aio_host(g, 0) : NULL;
    }
    struct timespec ts = { 0, 0 };
    if (t)
        ts = (struct timespec){ (int32_t)m32_rd(t), (int32_t)m32_rd(t + 4) };
    RETV(aio_suspend(hl, n, t ? &ts : NULL));
}

static int sp_select(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t t = m32_arg(cpu, 4);
    struct timeval tv, *tp = NULL;
    if (t) {
        tv.tv_sec = (int32_t)m32_rd(t);
        tv.tv_usec = (int32_t)m32_rd(t + 4);
        tp = &tv;
    }
    int r = select((int)m32_arg(cpu, 0), m32_host(m32_arg(cpu, 1)), m32_host(m32_arg(cpu, 2)), m32_host(m32_arg(cpu, 3)), tp);
    if (t) {
        m32_wr(t, (uint32_t)tv.tv_sec);
        m32_wr(t + 4, (uint32_t)tv.tv_usec);
    }
    RETV(r);
}

/* open(path, flags, ...) and fcntl(fd, cmd, ...): one optional int, or a flock pointer (same layout: off_t pairs) */
static int sp_open(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = open(m32_h(m32_arg(cpu, 0)), (int)m32_arg(cpu, 1), (int)m32_arg(cpu, 2));
    static int log = -1;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "files");
    if (log)
        fprintf(stderr, "ocerz: m32: file open %.300s -> %d\n", (const char *)m32_h(m32_arg(cpu, 0)), fd);
    RETV(fd);
}
static int sp_openat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    RETV(openat((int)m32_arg(cpu, 0), m32_h(m32_arg(cpu, 1)), (int)m32_arg(cpu, 2), (int)m32_arg(cpu, 3)));
}
static int sp_fcntl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)m32_arg(cpu, 0), cmd = (int)m32_arg(cpu, 1);
    uint32_t a = m32_arg(cpu, 2);
    switch (cmd) {
    case F_GETLK: case F_SETLK: case F_SETLKW: case F_GETPATH: case F_PREALLOCATE: case F_RDADVISE:
        RETV(fcntl(fd, cmd, m32_h(a)));   /* flock, a path buffer: no pointers or longs inside that differ */
    default:
        RETV(fcntl(fd, cmd, (int)a));
    }
}

/* ---- memory mappings: inside the window, never the host's own address space ----
 * Host pages are 16 KB and i386 ones 4 KB: lengths round up to 16 KB.  An mprotect that asks for write access gets
 * it for the host pages it touches (an image's __TEXT, which m32 maps read and execute, is one Wine writes after
 * unprotecting it); one that takes access away, and madvise, are accepted without effect (ponytail: a guard page an
 * allocator protects stays writable; nothing we run relies on the fault). */
#define M32_MAP_FAILED 0xffffffffu
static uint32_t map_len(uint32_t len) { return (len + 0x3fffu) & ~0x3fffu; }
/* [a, b) of the 16 KB host page at page, for a fixed anonymous mapping: zeroed, the page mapped if it was not and
 * made writable if it was not (Wine maps a builtin DLL's header page over the start of its read-only image) */
static int map_part(uint32_t page, uint32_t a, uint32_t b)
{
    if (!ocerz_addr_readable(page))
        return ocerz_map_fixed(page, 0x4000, PROT_READ | PROT_WRITE) != OCERZ_OK;
    if (ocerz_protect(page, 0x4000, PROT_READ | PROT_WRITE) != OCERZ_OK)
        return 1;
    memset(m32_h(a), 0, b - a);
    return 0;
}

static int sp_mprotect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0), len = m32_arg(cpu, 1);
    int prot = (int)m32_arg(cpu, 2);
    if ((g & 0xfff) || (uint64_t)g + len > M32_HANDLE_LO) {
        errno = EINVAL;
        RETV(0xffffffffu);
    }
    if ((prot & PROT_WRITE) && len) {
        uint32_t lo = g & ~0x3fffu, hi = (uint32_t)(((uint64_t)g + len + 0x3fff) & ~0x3fffull);
        for (uint32_t p = lo; p < hi; p += 0x4000)
            if (ocerz_addr_readable(p))
                ocerz_protect(p, 0x4000, PROT_READ | PROT_WRITE);
    }
    RETV(0);
}

/* mmap from i386 code, the bridged call's and a raw system call's (src/m32_selfload.c): the guest address, or
 * M32_MAP_FAILED with errno set */
uint32_t m32_mmap(uint32_t want, uint32_t len, int prot, int flags, int fd, uint64_t off)
{
    if (!len) {
        errno = EINVAL;
        return M32_MAP_FAILED;
    }
    uint32_t size = map_len(len), g;
    /* A hint is taken where the range is free, as macOS takes it: Wine's preloader reserves the address ranges
     * Windows programs need that way, and a range given out anywhere else is no use to it. */
    if (!(flags & MAP_FIXED) && want && !(want & 0xfff) && (uint64_t)want + len <= M32_HANDLE_LO) {
        int free_range = 1;
        for (uint64_t a = want; free_range && a < (uint64_t)want + len; a += 0x1000)
            free_range = ocerz_addr_mapped(a) == 0;
        if (free_range)
            flags |= MAP_FIXED;
    }
    if (flags & MAP_FIXED) {
        /* 4 KB pages, as on the Intel Macs (MW3's VirtualAlloc commits and decommits single pages): the 16 KB host
         * pages wholly inside are mapped afresh, and of a host page the range covers only partly, just the covered
         * part is zeroed, as a fresh anonymous page would be; its other bytes stay */
        g = want;
        uint64_t end = (uint64_t)g + ((len + 0xfffu) & ~0xfffu), lo = ((uint64_t)g + 0x3fff) & ~0x3fffull, hi = end & ~0x3fffull;
        if ((g & 0xfff) || end > M32_HANDLE_LO) {
            errno = EINVAL;
            return M32_MAP_FAILED;
        }
        int bad = 0;
        if (lo < hi) {
            ocerz_unmap(lo, hi - lo);
            bad |= ocerz_map_fixed(lo, hi - lo, PROT_READ | PROT_WRITE) != OCERZ_OK;
        }
        if (g < lo)
            bad |= map_part(g & ~0x3fffu, g, (uint32_t)(lo < end ? lo : end));
        if (hi < end && hi >= lo)
            bad |= map_part((uint32_t)hi, (uint32_t)hi, (uint32_t)end);
        if (bad) {
            errno = ENOMEM;
            return M32_MAP_FAILED;
        }
    } else {
        g = m32_map(size);
        if (!g) {
            errno = ENOMEM;
            return M32_MAP_FAILED;
        }
    }
    if (!(flags & MAP_ANON) && fd >= 0) {
        /* a file: shared mappings overlay the file where the offset allows it, anything else is a copy */
        if ((flags & MAP_SHARED) && !(off & 0x3fff) && !(g & 0x3fff) && ocerz_map_shared_file(g, size, prot | PROT_READ, fd, off) == OCERZ_OK)
            return g;
        uint64_t done = 0;
        while (done < len) {
            ssize_t r = pread(fd, (char *)m32_h(g) + done, len - done, (off_t)(off + done));
            if (r <= 0)
                break;
            done += (uint64_t)r;
        }
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
            m32_log_once("a writable shared file mapping at an unaligned offset is a copy (writes are not saved):", "mmap");
    }
    return g;
}

static int sp_mmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t want = m32_arg(cpu, 0), len = m32_arg(cpu, 1);
    int prot = (int)m32_arg(cpu, 2), flags = (int)m32_arg(cpu, 3), fd = (int)m32_arg(cpu, 4);
    RETV(m32_mmap(want, len, prot, flags, fd, m32_arg64(cpu, 5)));
}
static int sp_munmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0), len = m32_arg(cpu, 1);
    if (!(g & 0x3fff))
        ocerz_unmap(g, map_len(len));   /* ponytail: a partial 4 KB unmap keeps the host page */
    RETV(0);
}
/* Foundation's page calls hand out memory the program writes, which has to be in the window: SDL 1.2's main
 * converts its arguments to UTF-16 in NSAllocateMemoryPages' pages (Armagetron Advanced), and given host pages it
 * got a handle cell instead and wrote past it */
static int sp_NSAllocateMemoryPages(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t n = m32_arg(cpu, 0);
    uint32_t g = n ? m32_mmap(0, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0) : 0;
    m32_ret(cpu, g == M32_MAP_FAILED ? 0 : g, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_NSDeallocateMemoryPages(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0), n = m32_arg(cpu, 1);
    if (g && !(g & 0x3fff))
        ocerz_unmap(g, map_len(n));
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_NSCopyMemoryPages(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t s = m32_arg(cpu, 0), d = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    if (s && d && n)
        memmove(m32_h(d), m32_h(s), n);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_vm_allocate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t addrp = m32_arg(cpu, 1), size = map_len(m32_arg(cpu, 2)), flags = m32_arg(cpu, 3);
    uint32_t g = 0;
    if (!(flags & 1 /* VM_FLAGS_ANYWHERE */)) {
        g = m32_rd(addrp);
        if ((g & 0x3fff) || ocerz_map_fixed(g, size, PROT_READ | PROT_WRITE) != OCERZ_OK)
            g = 0;
    } else
        g = m32_map(size);
    if (!g)
        RETV(3);   /* KERN_NO_SPACE */
    m32_wr(addrp, g);
    RETV(0);
}
static int sp_vm_deallocate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 1), size = m32_arg(cpu, 2);
    if (g && !(g & 0x3fff))
        ocerz_unmap(g, map_len(size));
    RETV(0);
}
static int sp_sbrk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    errno = ENOMEM;
    RETV(M32_MAP_FAILED);
}

/* ---- setjmp/longjmp: the guest's own registers in its own buffer (i386 jmp_buf is 72 bytes, sigjmp_buf 76) ----
 * layout (ours): 0 ebx, 4 esi, 8 edi, 12 ebp, 16 esp after return, 20 return eip */
static int setjmp_common(OcerzCPU *cpu)
{
    uint32_t b = m32_arg(cpu, 0), esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    m32_wr(b, (uint32_t)cpu->gpr[OCERZ_RBX]);
    m32_wr(b + 4, (uint32_t)cpu->gpr[OCERZ_RSI]);
    m32_wr(b + 8, (uint32_t)cpu->gpr[OCERZ_RDI]);
    m32_wr(b + 12, (uint32_t)cpu->gpr[OCERZ_RBP]);
    m32_wr(b + 16, esp + 4);
    m32_wr(b + 20, m32_rd(esp));
    m32_ret(cpu, 0, (uint32_t)cpu->gpr[OCERZ_RDX], 0);
    return OCERZ_STEP_OK;
}
static int sp_setjmp(struct OcerzVM *vm, OcerzCPU *cpu) { return setjmp_common(cpu); }
static int sp_longjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t b = m32_arg(cpu, 0), v = m32_arg(cpu, 1);
    cpu->gpr[OCERZ_RBX] = m32_rd(b);
    cpu->gpr[OCERZ_RSI] = m32_rd(b + 4);
    cpu->gpr[OCERZ_RDI] = m32_rd(b + 8);
    cpu->gpr[OCERZ_RBP] = m32_rd(b + 12);
    cpu->gpr[OCERZ_RSP] = m32_rd(b + 16);
    cpu->rip = m32_rd(b + 20);
    cpu->gpr[OCERZ_RAX] = v ? v : 1;
    return OCERZ_STEP_OK;
}

/* ---- signals ----
 * A handler for an asynchronous signal (SIGINT, SIGTERM, SIGALRM, SIGCHLD, ...) runs at the guest's next crossing into
 * the host: the host's handler only marks the signal, for the whole process since the kernel picks any thread, and
 * m32_trap calls the guest's handler there as a nested call, before the call the guest was making.  A call blocked in
 * the host (nanosleep, pause, read) is interrupted first, as on a Mac.  SDL 1.2 turns SIGINT and SIGTERM into its
 * quit event this way, so a game asked to stop saves its settings and quits.  The handler gets (sig) or, with
 * SA_SIGINFO, (sig, siginfo, ucontext) with only si_signo filled in.  A fault's handler (SIGSEGV, SIGBUS, SIGFPE,
 * SIGILL, SIGTRAP) runs at the fault instead (below).  OCERZ_NO_M32_SIGNALS=1 records the asynchronous handlers
 * only; OCERZ_NO_M32_FAULT_HANDLERS=1 leaves a fault to ocerz's crash report, as a program's crash handler (SDL's
 * parachute) would otherwise take it. */
static uint32_t g_sig_handler[NSIG], g_sig_flags[NSIG];
volatile uint32_t m32_sig_pending;          /* signals caught for the guest, not yet run */
static __thread uint32_t t_sig_running;     /* signals whose handler is running on this thread */

static int fault_signal(uint32_t sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL || sig == SIGTRAP;
}
static int g_faults_off = -1;   /* OCERZ_NO_M32_FAULT_HANDLERS, read before any fault can ask */

static int async_signal(uint32_t sig)
{
    switch (sig) {
    case SIGHUP: case SIGINT: case SIGQUIT: case SIGTERM: case SIGALRM: case SIGVTALRM: case SIGPROF:
    case SIGCHLD: case SIGWINCH: case SIGUSR1: case SIGUSR2: case SIGPIPE: case SIGCONT: case SIGINFO:
    case SIGURG: case SIGIO: case SIGTSTP:
        return 1;
    }
    return 0;
}

static void host_signal(int sig) { __atomic_or_fetch(&m32_sig_pending, 1u << sig, __ATOMIC_SEQ_CST); }

static void set_handler(uint32_t sig, uint32_t h, uint32_t flags)
{
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_M32_SIGNALS") != NULL;
    if (g_faults_off < 0)
        g_faults_off = getenv("OCERZ_NO_M32_FAULT_HANDLERS") != NULL;
    g_sig_handler[sig] = h;
    g_sig_flags[sig] = flags;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (h == 1) {   /* SIG_IGN is honored: SIGPIPE */
        sa.sa_handler = SIG_IGN;
    } else if (h > 1 && async_signal(sig) && !off) {
        sa.sa_handler = host_signal;
        sa.sa_flags = (int)(flags & (SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT));
    } else {
        if (h > 1 && (!fault_signal(sig) || g_faults_off == 1))
            m32_log_once("signal handler recorded (not delivered) for signal",
                         sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "another");
        if (!async_signal(sig))
            return;
        sa.sa_handler = SIG_DFL;
    }
    sigaction((int)sig, &sa, NULL);
}

/* The guest's handlers for the signals caught since, each once, on the calling thread (m32_trap). */
void m32_signals_deliver(struct OcerzVM *vm)
{
    uint32_t pending = __atomic_load_n(&m32_sig_pending, __ATOMIC_SEQ_CST) & ~t_sig_running;
    while (pending) {
        uint32_t sig = (uint32_t)__builtin_ctz(pending), bit = 1u << sig;
        pending &= pending - 1;
        if (!(__atomic_fetch_and(&m32_sig_pending, ~bit, __ATOMIC_SEQ_CST) & bit))
            continue;   /* another thread ran it */
        uint32_t h = g_sig_handler[sig], flags = g_sig_flags[sig];
        if (h <= 1)
            continue;
        if (flags & SA_RESETHAND)
            set_handler(sig, 0, 0);
        uint32_t info = 0;
        if (flags & SA_SIGINFO) {   /* an i386 siginfo_t (64 bytes), then room for a ucontext nobody fills */
            info = m32_calloc(1, 64 + 512);
            if (info)
                m32_wr(info, sig);
        }
        uint32_t running = flags & SA_NODEFER ? 0 : bit;
        t_sig_running |= running;
        const uint32_t args[3] = { sig, info, info ? info + 64 : 0 };
        m32_call(vm, h, args, 3, NULL, NULL);
        t_sig_running &= ~running;
        if (info)
            m32_free(info);
    }
}

static int sp_signal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t sig = m32_arg(cpu, 0), h = m32_arg(cpu, 1);
    if (sig == 0 || sig >= NSIG)
        RETV(0xffffffffu);   /* SIG_ERR */
    uint32_t old = g_sig_handler[sig];
    set_handler(sig, h, SA_RESTART);   /* signal() is BSD's: interrupted calls restart */
    RETV(old);
}
static int sp_sigaction(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t sig = m32_arg(cpu, 0), act = m32_arg(cpu, 1), old = m32_arg(cpu, 2);
    if (sig == 0 || sig >= NSIG)
        RETV(0xffffffffu);
    if (old) {   /* struct sigaction: handler, mask, flags */
        m32_wr(old, g_sig_handler[sig]);
        m32_wr(old + 4, 0);
        m32_wr(old + 8, g_sig_flags[sig]);
    }
    if (act)
        set_handler(sig, m32_rd(act), m32_rd(act + 8));
    RETV(0);
}

/* ---- faults ----
 * A fault in i386 code reaches the program's own handler for it: Mono's SIGSEGV handler turns a null access into a
 * NullReferenceException and its SIGFPE one a division by zero into a DivideByZeroException, 32-bit Wine turns either
 * into a Windows exception.  The host's fault handler (vm.c), and the interpreter for a division or an int3, hand it to
 * ocerz_signal_deliver, which sends a 32-bit cpu here.  The frame is the one XNU's sendsig builds for i386: a 64-byte
 * siginfo, a 32-byte ucontext and its 600-byte mcontext (exception state, thread state, and the float state, which is
 * eight reserved bytes then an FXSAVE image), on the thread's stack or on the alternate stack sigaltstack recorded when
 * the handler asked for SA_ONSTACK.  The handler is called as handler(sig, info, uc) and returns to a trap that loads
 * the mcontext back, whatever the handler changed in it, as sigreturn does.  A handler that faults again for its own
 * signal more than a few levels deep is not run again: the kernel would have killed the process. */
#define MC_SS 12            /* mcontext32: __es at 0, __ss at 12, __fs at 76 */
#define MC_FS 76
#define MC_SIZE 600
#define FS_SNAPSHOT 296     /* the float state's reserved bytes after xmm7 */
static __thread struct { uint32_t sp, size, flags; } t_altstack = { 0, 0, SS_DISABLE };
static __thread int t_fault_depth;

/* sigaltstack(const stack_t *ss, stack_t *old): {ss_sp, ss_size, ss_flags}, kept for the guest's fault handlers; the
 * host thread keeps its own */
static int sp_sigaltstack(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t ss = m32_arg(cpu, 0), old = m32_arg(cpu, 1), esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    int on = !(t_altstack.flags & SS_DISABLE) && esp - t_altstack.sp < t_altstack.size;
    if (old) {
        m32_wr(old, t_altstack.sp);
        m32_wr(old + 4, t_altstack.size);
        m32_wr(old + 8, (t_altstack.flags & SS_DISABLE) | (on ? SS_ONSTACK : 0));
    }
    if (ss) {
        uint32_t flags = m32_rd(ss + 8);
        if (on) {
            errno = EPERM;
            RETV(0xffffffffu);
        }
        if (flags & SS_DISABLE) {
            t_altstack.flags = SS_DISABLE;
        } else {
            t_altstack.sp = m32_rd(ss);
            t_altstack.size = m32_rd(ss + 4);
            t_altstack.flags = 0;
        }
    }
    RETV(0);
}

static const int g_thread_state_regs[8] = { OCERZ_RAX, OCERZ_RBX, OCERZ_RCX, OCERZ_RDX, OCERZ_RDI, OCERZ_RSI,
                                            OCERZ_RBP, OCERZ_RSP };

int m32_signal_fault(OcerzCPU *cpu, int sig, uint64_t addr, int code, uint32_t err)
{
    if (sig <= 0 || sig >= NSIG)
        return 0;
    uint32_t h = g_sig_handler[sig];
    if (h <= 1 && (sig == SIGSEGV || sig == SIGBUS) && g_sig_handler[sig == SIGSEGV ? SIGBUS : SIGSEGV] > 1) {
        sig = sig == SIGSEGV ? SIGBUS : SIGSEGV;   /* macOS raises either for a bad access */
        h = g_sig_handler[sig];
    }
    uint32_t flags = g_sig_flags[sig];
    if (h <= 1 || t_fault_depth >= 4 || g_faults_off == 1)
        return 0;
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    int alt = (flags & SA_ONSTACK) && !(t_altstack.flags & SS_DISABLE) && t_altstack.size &&
              !(esp - t_altstack.sp < t_altstack.size);
    uint32_t top = alt ? t_altstack.sp + t_altstack.size : esp;
    uint32_t mc = (top - MC_SIZE) & ~15u, uc = (mc - 32) & ~15u, si = (uc - 64) & ~15u;
    uint32_t sp = ((si - 16) & ~15u) - 4;   /* at the handler's entry esp + 4 is 16-byte aligned */
    if (top < MC_SIZE + 256 || !ocerz_addr_readable(sp) || !ocerz_addr_readable(top - 1))
        return 0;
    memset(m32_h(sp), 0, top - sp);

    m32_wr(mc + 0, sig == SIGSEGV || sig == SIGBUS ? 14 : sig == SIGFPE ? 0 : sig == SIGTRAP ? 3 : 6);   /* trapno */
    m32_wr(mc + 4, err);
    m32_wr(mc + 8, (uint32_t)addr);
    if (cpu->cc_op != OCERZ_CC_NONE)
        ocerz_flags_materialize(cpu);
    for (int i = 0; i < 8; i++)
        m32_wr(mc + MC_SS + 4 * (uint32_t)i, (uint32_t)cpu->gpr[g_thread_state_regs[i]]);
    m32_wr(mc + MC_SS + 32, cpu->seg_sel[OCERZ_SREG_SS]);
    m32_wr(mc + MC_SS + 36, (uint32_t)cpu->rflags);
    m32_wr(mc + MC_SS + 40, (uint32_t)cpu->rip);
    m32_wr(mc + MC_SS + 44, cpu->cs_sel);
    m32_wr(mc + MC_SS + 48, cpu->seg_sel[OCERZ_SREG_DS]);
    m32_wr(mc + MC_SS + 52, cpu->seg_sel[OCERZ_SREG_ES]);
    m32_wr(mc + MC_SS + 56, cpu->seg_sel[OCERZ_SREG_FS]);
    m32_wr(mc + MC_SS + 60, cpu->seg_sel[OCERZ_SREG_GS]);
    ocerz_x87_fxsave(cpu, mc + MC_FS + 8);
    m32_wr(mc + MC_FS + 32, cpu->mxcsr);
    m32_wr(mc + MC_FS + 36, 0xffbf);
    for (uint32_t i = 0; i < 8; i++) {
        memcpy(m32_h(mc + MC_FS + 168 + 16 * i), &cpu->xmm[i].lo, 8);
        memcpy(m32_h(mc + MC_FS + 176 + 16 * i), &cpu->xmm[i].hi, 8);
    }
    m32_wr(mc + MC_FS + FS_SNAPSHOT, cpu->fsw);   /* fxsave's FSW folds TOP in; the exact word comes back too */

    m32_wr(uc + 0, (uint32_t)alt);
    m32_wr(uc + 8, t_altstack.sp);
    m32_wr(uc + 12, t_altstack.size);
    m32_wr(uc + 16, t_altstack.flags | (alt ? SS_ONSTACK : 0));
    m32_wr(uc + 24, MC_SIZE);
    m32_wr(uc + 28, mc);

    m32_wr(si + 0, (uint32_t)sig);
    m32_wr(si + 8, (uint32_t)code);
    m32_wr(si + 24, (uint32_t)addr);

    m32_wr(sp, M32_TRAP(M32_ID_SIGRETURN));
    m32_wr(sp + 4, (uint32_t)sig);
    m32_wr(sp + 8, si);
    m32_wr(sp + 12, uc);
    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = h;
    t_fault_depth++;
    if (flags & SA_RESETHAND)
        g_sig_handler[sig] = 0;
    return 1;
}

/* the handler returned: its ret popped the trap's address, so uc is the third word at esp */
int m32_sigreturn(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t uc = m32_rd((uint32_t)cpu->gpr[OCERZ_RSP] + 8), mc = uc ? m32_rd(uc + 28) : 0;
    if (!mc || !ocerz_addr_readable(mc) || !ocerz_addr_readable(mc + MC_SIZE - 1)) {
        fprintf(stderr, "ocerz: m32: a signal handler returned without its context (esp %#x)\n",
                (uint32_t)cpu->gpr[OCERZ_RSP]);
        return OCERZ_STEP_FATAL;
    }
    for (int i = 0; i < 8; i++)
        cpu->gpr[g_thread_state_regs[i]] = m32_rd(mc + MC_SS + 4 * (uint32_t)i);
    cpu->rflags = (m32_rd(mc + MC_SS + 36) & 0x00ffffffu) | OCERZ_FLAG_FIXED1;
    cpu->cc_op = OCERZ_CC_NONE;
    cpu->rip = m32_rd(mc + MC_SS + 40);
    ocerz_x87_fxrstor(cpu, mc + MC_FS + 8);
    cpu->fsw = (uint16_t)m32_rd(mc + MC_FS + FS_SNAPSHOT);
    cpu->mxcsr = m32_rd(mc + MC_FS + 32);
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    for (uint32_t i = 0; i < 8; i++) {
        memcpy(&cpu->xmm[i].lo, m32_h(mc + MC_FS + 168 + 16 * i), 8);
        memcpy(&cpu->xmm[i].hi, m32_h(mc + MC_FS + 176 + 16 * i), 8);
    }
    if (t_fault_depth > 0)
        t_fault_depth--;
    return OCERZ_STEP_OK;
}

static int sp_getpagesize(struct OcerzVM *vm, OcerzCPU *cpu) { RETV(m32_page_size()); }
static int sp_sysconf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int name = (int)m32_arg(cpu, 0);
    RETV(name == _SC_PAGESIZE ? (long)m32_page_size() : sysconf(name));
}
/* host_page_size(host, vm_size_t *size): KERN_SUCCESS */
static int sp_host_page_size(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), m32_page_size());
    RETV(0);
}

static int sp_stack_chk_fail(struct OcerzVM *vm, OcerzCPU *cpu)
{
    fprintf(stderr, "ocerz: m32: stack protector tripped (called from %#x)\n", m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    ocerz_vm_request_exit(vm, 134);
    return OCERZ_STEP_EXIT;
}

/* _DefaultRuneLocale as i386 lays it out: magic[8], encoding[32], two function pointers, invalid_rune, then
 * runetype/maplower/mapupper[256] at 0x34/0x434/0x834 (what inline ctype macros read), the rest empty */
#include <runetype.h>
static uint32_t rune_locale(void)
{
    const _RuneLocale *h = &_DefaultRuneLocale;
    uint32_t g = m32_static_alloc(3164, 16);
    memcpy(m32_h(g), h->__magic, 8);
    memcpy(m32_h(g + 8), h->__encoding, 32);
    m32_wr(g + 48, (uint32_t)h->__invalid_rune);
    memcpy(m32_h(g + 52), h->__runetype, 256 * 4);
    memcpy(m32_h(g + 52 + 1024), h->__maplower, 256 * 4);
    memcpy(m32_h(g + 52 + 2048), h->__mapupper, 256 * 4);
    return g;
}

/* The page size an i386 program is told: 4 KB, as on the Intel Macs it was built for, where the host's are 16 KB.
 * m32's mmap already maps at 4 KB (a fixed range clears just the part of a host page it covers), its mprotect leaves
 * the window as it is and a partial munmap keeps the host page, so a 4 KB answer costs nothing; and a program that
 * lays out memory by it expects 4 KB.  Wine does: told 16 KB, wineserver flagged every PE DLL with 4 KB sections
 * as one to map flat and ntdll refused it (STATUS_INVALID_IMAGE_FORMAT, at kernelbase.dll).  getpagesize,
 * sysconf(_SC_PAGESIZE), host_page_size and vm_page_size, vm_page_mask, vm_page_shift and vm_kernel_page_size agree,
 * and valloc aligns to it.  OCERZ_M32_HOST_PAGES=1 tells the host's. */
uint32_t m32_page_size(void)
{
    static uint32_t size;
    if (!size)
        size = getenv("OCERZ_M32_HOST_PAGES") ? (uint32_t)getpagesize() : 4096;
    return size;
}

static uint32_t page_var(uint32_t v)
{
    uint32_t g = m32_static_alloc(4, 4);
    m32_wr(g, v);
    return g;
}

uint32_t m32_special_data(const char *symbol, void *host_var)
{
    (void)host_var;
    if (!strcmp(symbol, "__DefaultRuneLocale"))
        return rune_locale();
    if (!strcmp(symbol, "_vm_page_size") || !strcmp(symbol, "_vm_kernel_page_size"))
        return page_var(m32_page_size());
    if (!strcmp(symbol, "_vm_page_mask"))
        return page_var(m32_page_size() - 1);
    if (!strcmp(symbol, "_vm_page_shift"))
        return page_var((uint32_t)__builtin_ctz(m32_page_size()));
    /* object constants CoreFoundation exports but only Foundation's headers declare (so the database has no
     * record): the guest reads the object */
    if (host_var && (!strcmp(symbol, "_NSDefaultRunLoopMode") || !strcmp(symbol, "_NSRunLoopCommonModes"))) {
        uint32_t g = m32_static_alloc(4, 4);
        m32_wr(g, m32_objc_to_guest(*(void **)host_var));
        return g;
    }
    /* code built against the 10.4 SDK writes stdin, stdout and stderr as &__sF[0..2]: three i386 __sFILEs (88 bytes)
     * in a row, each aliased to the host's stream as m32_file's twins are (Firefox 3.6's setbuf(stdout) handed libc
     * an address in an array that was not there).  The host's own streams, not ocerz's stderr (include/ocerz/types.h) */
    if (!strcmp(symbol, "___sF")) {
        extern FILE *__stdinp, *__stdoutp, *__stderrp;
        FILE *std[3] = { __stdinp, __stdoutp, __stderrp };
        uint32_t g = m32_static_alloc(3 * 88, 4);
        for (uint32_t i = 0; i < 3; i++) {
            m32_alias(g + 88 * i, std[i]);
            *(int16_t *)m32_h(g + 88 * i + 14) = (int16_t)fileno(std[i]);
        }
        return g;
    }
    if (!strcmp(symbol, "___stack_chk_guard")) {
        uint32_t g = m32_static_alloc(4, 4);
        m32_wr(g, arc4random() | 0xff000000u);   /* a canary with a NUL-free top byte */
        return g;
    }
    return 0;
}

/* Hot pure calls straight to the host function: no signature walk, bridge frame or data refresh per call. */
#define FAST_I(name, expr) static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, (uint32_t)(expr), 0, 0); return OCERZ_STEP_OK; }
#define A(n) m32_arg(cpu, n)
#define H(n) m32_h(m32_arg(cpu, n))
/* The classifiers isinf/isnan/isfinite/signbit/isnormal compile to in i386 code (the 10.x SDK's math.h).
 * libSystem's declarations have none of them, so they resolved to "return 0": every float was neither NaN nor
 * infinite, and not finite either.  0 or 1, as Apple's answer. */
FAST_I(isinff, isinf(argf(cpu, 0)) != 0)
FAST_I(isnanf, isnan(argf(cpu, 0)) != 0)
FAST_I(isfinitef, isfinite(argf(cpu, 0)) != 0)
FAST_I(isnormalf, isnormal(argf(cpu, 0)) != 0)
FAST_I(signbitf, signbit(argf(cpu, 0)) != 0)
FAST_I(isinfd, isinf(argd(cpu, 0)) != 0)
FAST_I(isnand, isnan(argd(cpu, 0)) != 0)
FAST_I(isfinited, isfinite(argd(cpu, 0)) != 0)
FAST_I(isnormald, isnormal(argd(cpu, 0)) != 0)
FAST_I(signbitd, signbit(argd(cpu, 0)) != 0)
FAST_I(fast_strlen, strlen(H(0)))
FAST_I(fast_strcmp, strcmp(H(0), H(1)))
FAST_I(fast_strncmp, strncmp(H(0), H(1), A(2)))
FAST_I(fast_memcmp, A(2) ? memcmp(H(0), H(1), A(2)) : 0)
FAST_I(fast_wcscmp, wcscmp((const wchar_t *)H(0), (const wchar_t *)H(1)))   /* wchar_t is 4 bytes on both */
FAST_I(fast_wcslen, wcslen((const wchar_t *)H(0)))
FAST_I(fast_memcpy, (A(2) ? memmove(H(0), H(1), A(2)) : 0, A(0)))
FAST_I(fast_memset, (A(2) ? memset(H(0), (int)A(1), A(2)) : 0, A(0)))
FAST_I(fast_barrier, (__atomic_thread_fence(__ATOMIC_SEQ_CST), 0))
#define FAST_F(name, fn) static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, fn(argf(cpu, 0))); return OCERZ_STEP_OK; }
#define FAST_D(name, fn) static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, fn(argd(cpu, 0))); return OCERZ_STEP_OK; }
FAST_F(fast_sinf, sinf) FAST_F(fast_cosf, cosf) FAST_F(fast_tanf, tanf) FAST_F(fast_sqrtf, sqrtf)
FAST_F(fast_logf, logf) FAST_F(fast_expf, expf) FAST_F(fast_acosf, acosf) FAST_F(fast_asinf, asinf)
FAST_F(fast_atanf, atanf) FAST_F(fast_floorf, floorf) FAST_F(fast_ceilf, ceilf)
FAST_D(fast_sin, sin) FAST_D(fast_cos, cos) FAST_D(fast_tan, tan) FAST_D(fast_sqrt, sqrt) FAST_D(fast_log, log)
FAST_D(fast_exp, exp) FAST_D(fast_acos, acos) FAST_D(fast_asin, asin) FAST_D(fast_atan, atan)
FAST_D(fast_floor, floor) FAST_D(fast_ceil, ceil) FAST_D(fast_log10, log10)
static int sp_fast_powf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, powf(argf(cpu, 0), argf(cpu, 1))); return OCERZ_STEP_OK; }
static int sp_fast_atan2f(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, atan2f(argf(cpu, 0), argf(cpu, 1))); return OCERZ_STEP_OK; }
static int sp_fast_fmodf(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, fmodf(argf(cpu, 0), argf(cpu, 1))); return OCERZ_STEP_OK; }
static int sp_fast_pow(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, pow(argd(cpu, 0), argd(cpu, 2))); return OCERZ_STEP_OK; }
static int sp_fast_atan2(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, atan2(argd(cpu, 0), argd(cpu, 2))); return OCERZ_STEP_OK; }
static int sp_fast_fmod(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret_st0(cpu, fmod(argd(cpu, 0), argd(cpu, 2))); return OCERZ_STEP_OK; }
/* libkern atomics act on the guest's own 4- and 8-byte cells in place: pointers and longs are 32-bit on i386, and a
 * guest pointer value must not be converted (the generic crossing would hand the host a translated pointer) */
static uint32_t *cell32(OcerzCPU *cpu, int n) { return (uint32_t *)m32_h(m32_arg(cpu, n)); }
static int cas32(uint32_t *p, uint32_t old, uint32_t new)
{
    return __atomic_compare_exchange_n(p, &old, new, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
FAST_I(osa_cas32, cas32(cell32(cpu, 2), A(0), A(1)))   /* CompareAndSwap32/Int/Long/Ptr(old, new, cell) */
FAST_I(osa_add32, __atomic_add_fetch(cell32(cpu, 1), A(0), __ATOMIC_SEQ_CST))   /* Add32(amount, cell): new value */
FAST_I(osa_inc32, __atomic_add_fetch(cell32(cpu, 0), 1, __ATOMIC_SEQ_CST))
FAST_I(osa_dec32, __atomic_sub_fetch(cell32(cpu, 0), 1, __ATOMIC_SEQ_CST))
FAST_I(osa_or32, __atomic_or_fetch(cell32(cpu, 1), A(0), __ATOMIC_SEQ_CST))
FAST_I(osa_and32, __atomic_and_fetch(cell32(cpu, 1), A(0), __ATOMIC_SEQ_CST))
static int sp_osa_add64(struct OcerzVM *vm, OcerzCPU *cpu)   /* Add64(int64 amount, int64 *cell) */
{
    m32_ret64(cpu, __atomic_add_fetch((uint64_t *)m32_h(m32_arg(cpu, 2)), m32_arg64(cpu, 0), __ATOMIC_SEQ_CST));
    return OCERZ_STEP_OK;
}
static int sp_osa_cas64(struct OcerzVM *vm, OcerzCPU *cpu)   /* CompareAndSwap64(old, new, cell) */
{
    uint64_t old = m32_arg64(cpu, 0);
    m32_ret(cpu, __atomic_compare_exchange_n((uint64_t *)m32_h(m32_arg(cpu, 4)), &old, m32_arg64(cpu, 2), 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_spin_lock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t *l = cell32(cpu, 0);
    while (!cas32(l, 0, 1))
        sched_yield();   /* ponytail: yields instead of spinning; guests hold these locks only briefly */
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
FAST_I(spin_try, cas32(cell32(cpu, 0), 0, 1))
FAST_I(spin_unlock, (__atomic_store_n(cell32(cpu, 0), 0, __ATOMIC_RELEASE), 0))
/* Mach VM regions of the guest's own address space, which is the 4 GB window: regions are found from the guest
 * address up, reported in guest addresses, and none lies past the window (a host address would truncate to 32 bits and
 * send a guest's walk over its address space back to the start).  vm_address_t and vm_size_t cells are 32-bit; the
 * info structures carry no pointers, so they are written in place with the guest's count.  ponytail: every task is the
 * guest's own (mach_task_self); a game only looks at itself. */
static int window_region(uint32_t gaddr_cell, uint32_t gsize_cell, int wide, mach_vm_address_t a, mach_vm_size_t sz,
                         kern_return_t kr)
{
    uint64_t lo = ocerz_guest_base, hi = lo + M32_WINDOW;
    if (kr == KERN_SUCCESS && (a < lo || a >= hi))
        kr = KERN_INVALID_ADDRESS;
    if (kr == KERN_SUCCESS) {
        if (a + sz > hi)
            sz = hi - a;
        if (sz > 0xfffff000u)
            sz = 0xfffff000u;
        m32_wr(gaddr_cell, (uint32_t)(a - lo));
        m32_wr(gsize_cell, (uint32_t)sz);
        if (wide) {
            m32_wr(gaddr_cell + 4, 0);
            m32_wr(gsize_cell + 4, 0);
        }
    }
    return kr;
}
/* vm_region / vm_region_64 / mach_vm_region(task, address*, size*, flavor, info, count*, object_name*); wide: the
 * address and size cells are mach_vm_address_t and mach_vm_size_t, 64-bit */
static int region(struct OcerzVM *vm, OcerzCPU *cpu, int wide)
{
    uint32_t ga = A(1), gs = A(2), gcnt = A(5), gobj = A(6);
    if (wide && m32_rd(ga + 4)) {
        m32_ret(cpu, KERN_INVALID_ADDRESS, 0, 0);
        return OCERZ_STEP_OK;
    }
    mach_vm_address_t a = (uintptr_t)m32_h(m32_rd(ga));
    mach_vm_size_t sz = 0;
    mach_msg_type_number_t cnt = m32_rd(gcnt);
    mach_port_t obj = MACH_PORT_NULL;
    vm_region_flavor_t flavor = (vm_region_flavor_t)A(3);
    kern_return_t kr;
    for (;;) {
        cnt = m32_rd(gcnt);
        kr = mach_vm_region(mach_task_self(), &a, &sz, flavor, (vm_region_info_t)H(4), &cnt, &obj);
        if (kr != KERN_SUCCESS || (flavor != VM_REGION_BASIC_INFO && flavor != VM_REGION_BASIC_INFO_64) ||
            *(int *)H(4) != VM_PROT_NONE || a >= ocerz_guest_base + M32_WINDOW)
            break;
        a += sz;   /* as above: the unused window is free address space */
    }
    kr = window_region(ga, gs, wide, a, sz, kr);
    if (kr == KERN_SUCCESS) {
        m32_wr(gcnt, cnt);
        if (gobj)
            m32_wr(gobj, 0);
    }
    m32_ret(cpu, (uint32_t)kr, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_vm_region(struct OcerzVM *vm, OcerzCPU *cpu) { return region(vm, cpu, 0); }
static int sp_mach_vm_region(struct OcerzVM *vm, OcerzCPU *cpu) { return region(vm, cpu, 1); }
/* vm_region_recurse_64(task, address*, size*, nesting_depth*, info, count*) */
static int sp_vm_region_recurse_64(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t ga = A(1), gs = A(2), gd = A(3), gcnt = A(5);
    mach_vm_address_t a = (uintptr_t)m32_h(m32_rd(ga));
    mach_vm_size_t sz = 0;
    natural_t depth = m32_rd(gd);
    mach_msg_type_number_t cnt = m32_rd(gcnt);
    uint32_t asked = m32_rd(ga);
    kern_return_t kr;
    for (;;) {
        depth = m32_rd(gd);
        cnt = m32_rd(gcnt);
        kr = mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)H(4), &cnt);
        if (kr != KERN_SUCCESS || *(int *)H(4) != VM_PROT_NONE || a >= ocerz_guest_base + M32_WINDOW)
            break;
        a += sz;   /* unused window: free, as unmapped address space is in a real 32-bit process */
    }
    kr = window_region(ga, gs, 0, a, sz, kr);
    static int log = -1;
    static unsigned long n;
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "vmregion");
    if (log && (++n < 64 || n % 20000 == 0))
        fprintf(stderr, "ocerz: m32: vm_region_recurse_64 #%lu %#x -> %#x+%#x depth %u kr %d <- %#x\n", n, asked,
                m32_rd(ga), m32_rd(gs), depth, kr, m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    if (kr == KERN_SUCCESS) {
        m32_wr(gd, depth);
        m32_wr(gcnt, cnt);
    }
    m32_ret(cpu, (uint32_t)kr, 0, 0);
    return OCERZ_STEP_OK;
}
/* vm_read_overwrite(task, address, size, data, outsize*): a guest's safe read of its own memory */
static int sp_vm_read_overwrite(struct OcerzVM *vm, OcerzCPU *cpu)
{
    mach_vm_size_t out = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (uintptr_t)H(1), A(2), (uintptr_t)H(3), &out);
    if (A(4))
        m32_wr(A(4), (uint32_t)out);
    m32_ret(cpu, (uint32_t)kr, 0, 0);
    return OCERZ_STEP_OK;
}

#undef A
#undef H

/* ---- clocks: an Intel Mac's, as ocerz gives its 64-bit guests (syscall.c): mach_absolute_time counts nanoseconds
 * with a 1/1 timebase, which x86 code took for granted (Bink's timer runs on UpTime, frozen without this, and the
 * startup movies with it).  CoreServices' AbsoluteTime is the same count; UpTime and the conversions return their
 * 8-byte structs in EDX:EAX, as i386 Darwin returns small structs. */
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int sp_mach_absolute_time(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64(now_ns()); }
static int sp_mach_timebase_info(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_wr(m32_arg(cpu, 0), 1);
    m32_wr(m32_arg(cpu, 0) + 4, 1);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_AbsoluteToNanoseconds(struct OcerzVM *vm, OcerzCPU *cpu) { return RET64(ARG64(0)); }
/* a Duration: negative microseconds when they fit, else positive milliseconds (DriverServices.h) */
static int sp_AbsoluteToDuration(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t us = ARG64(0) / 1000, ms = us / 1000;
    m32_ret(cpu, us <= 0x7fffffff ? (uint32_t)-(int32_t)us : ms <= 0x7fffffff ? (uint32_t)ms : 0x7fffffffu, 0, 0);
    return OCERZ_STEP_OK;
}

/* bootstrap_look_up and bootstrap_check_in take the service as a name_t, a char[128] typedef the generator cannot
 * describe: the name is a C string in the window, and the port comes back as a 32-bit name in both modes, written
 * only on success as the host does.  Steam's libsteam_api finds the running client through
 * bootstrap_look_up("com.valvesoftware.steam.ipctool"). */
static int sp_bootstrap_look_up(struct OcerzVM *vm, OcerzCPU *cpu)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(m32_arg(cpu, 0), (const char *)m32_h(m32_arg(cpu, 1)), &port);
    if (kr == KERN_SUCCESS && m32_arg(cpu, 2))
        m32_wr(m32_arg(cpu, 2), port);
    return RETV_PLAIN(kr);
}
static int sp_bootstrap_check_in(struct OcerzVM *vm, OcerzCPU *cpu)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_check_in(m32_arg(cpu, 0), (const char *)m32_h(m32_arg(cpu, 1)), &port);
    if (kr == KERN_SUCCESS && m32_arg(cpu, 2))
        m32_wr(m32_arg(cpu, 2), port);
    return RETV_PLAIN(kr);
}

/* dispatch_once and dispatch_once_f on i386: the predicate is a 4-byte long the program's own inline check compares
 * with ~0, which the host's 8-byte one would overrun, and the block or function is guest code.  So they run here:
 * the predicate goes 0 -> 1 (running) -> ~0 (done) under one lock, a thread finding it running waits, and the block's
 * invoke (offset 12 of an i386 block) or the function is called as guest code. */
static pthread_mutex_t g_once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_once_done = PTHREAD_COND_INITIALIZER;
static void once_run(struct OcerzVM *vm, uint32_t pred, uint32_t fn, uint32_t arg)
{
    if (!pred || m32_rd(pred) == 0xffffffffu)
        return;
    pthread_mutex_lock(&g_once_lock);
    while (m32_rd(pred) == 1)
        pthread_cond_wait(&g_once_done, &g_once_lock);
    if (m32_rd(pred) == 0xffffffffu) {
        pthread_mutex_unlock(&g_once_lock);
        return;
    }
    m32_wr(pred, 1);
    pthread_mutex_unlock(&g_once_lock);
    m32_call(vm, fn, &arg, 1, NULL, NULL);
    pthread_mutex_lock(&g_once_lock);
    m32_wr(pred, 0xffffffffu);
    pthread_cond_broadcast(&g_once_done);
    pthread_mutex_unlock(&g_once_lock);
}
static int sp_dispatch_once(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t blk = m32_arg(cpu, 1);
    once_run(vm, m32_arg(cpu, 0), blk ? m32_rd(blk + 12) : 0, blk);
    return RETV_PLAIN(0);
}
static int sp_dispatch_once_f(struct OcerzVM *vm, OcerzCPU *cpu)
{
    once_run(vm, m32_arg(cpu, 0), m32_arg(cpu, 2), m32_arg(cpu, 1));
    return RETV_PLAIN(0);
}

/* getaddrinfo(node, service, hints, res*) and freeaddrinfo(list): the i386 struct addrinfo is {flags, family,
 * socktype, protocol, addrlen, canonname, addr, next}, 32 bytes, where the host's pointers are 8 bytes.  The host's
 * list is copied to guest memory, one block per entry holding its address and name, and freed at once; freeaddrinfo
 * frees the guest copy.  A sockaddr is the same bytes to both sides.  MW3 aborted in the host's freeaddrinfo. */
enum { AI_ADDRLEN = 16, AI_CANON = 20, AI_ADDR = 24, AI_NEXT = 28, AI_SIZE32 = 32 };
static void addrinfo_free(uint32_t g)
{
    for (uint32_t next; g; g = next) {
        next = m32_rd(g + AI_NEXT);
        m32_free(g);
    }
}
static int sp_getaddrinfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t node = m32_arg(cpu, 0), serv = m32_arg(cpu, 1), gh = m32_arg(cpu, 2), gres = m32_arg(cpu, 3);
    struct addrinfo hints = { 0 }, *list = NULL;
    if (gh)
        memcpy(&hints, m32_h(gh), 16);   /* flags, family, socktype, protocol */
    int r = getaddrinfo(node ? m32_h(node) : NULL, serv ? m32_h(serv) : NULL, gh ? &hints : NULL, &list);
    uint32_t head = 0, prev = 0;
    for (struct addrinfo *a = list; !r && a; a = a->ai_next) {
        uint32_t alen = a->ai_addr ? a->ai_addrlen : 0, nlen = a->ai_canonname ? (uint32_t)strlen(a->ai_canonname) + 1 : 0;
        uint32_t g = m32_malloc(AI_SIZE32 + alen + nlen);
        if (!g) {
            r = EAI_MEMORY;
            break;
        }
        memcpy(m32_h(g), a, 16);
        m32_wr(g + AI_ADDRLEN, alen);
        m32_wr(g + AI_ADDR, alen ? g + AI_SIZE32 : 0);
        m32_wr(g + AI_CANON, nlen ? g + AI_SIZE32 + alen : 0);
        m32_wr(g + AI_NEXT, 0);
        memcpy(m32_h(g + AI_SIZE32), a->ai_addr, alen);
        memcpy(m32_h(g + AI_SIZE32 + alen), a->ai_canonname, nlen);
        if (prev)
            m32_wr(prev + AI_NEXT, g);
        else
            head = g;
        prev = g;
    }
    if (list)
        freeaddrinfo(list);
    if (r) {
        addrinfo_free(head);
        head = 0;
    }
    if (gres)
        m32_wr(gres, head);
    RETV(r);
}
static int sp_freeaddrinfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    addrinfo_free(m32_arg(cpu, 0));
    return RETV_PLAIN(0);
}
/* gethostbyname(name): the i386 struct hostent {name, aliases, addrtype, length, addr_list}, 20 bytes, rebuilt in guest
 * memory with its arrays, names and addresses; like the host's static one, the next call replaces it */
static int sp_gethostbyname(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static uint32_t last;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t name = m32_arg(cpu, 0);
    pthread_mutex_lock(&lock);
    struct hostent *h = name ? gethostbyname(m32_h(name)) : NULL;
    uint32_t g = 0;
    if (h) {
        uint32_t na = 0, nd = 0, size = 20 + (uint32_t)strlen(h->h_name) + 1;
        for (; h->h_aliases && h->h_aliases[na]; na++)
            size += (uint32_t)strlen(h->h_aliases[na]) + 1;
        for (; h->h_addr_list && h->h_addr_list[nd]; nd++)
            size += (uint32_t)h->h_length;
        size += 4 * (na + 1 + nd + 1);
        g = m32_malloc(size);
    }
    if (g) {
        uint32_t aliases = g + 20, na = 0, nd = 0;
        while (h->h_aliases && h->h_aliases[na])
            na++;
        while (h->h_addr_list && h->h_addr_list[nd])
            nd++;
        uint32_t addrs = aliases + 4 * (na + 1), data = addrs + 4 * (nd + 1);
        m32_wr(g + 8, (uint32_t)h->h_addrtype);
        m32_wr(g + 12, (uint32_t)h->h_length);
        m32_wr(g + 4, aliases);
        m32_wr(g + 16, addrs);
        for (uint32_t i = 0; i < nd; i++, data += (uint32_t)h->h_length) {
            memcpy(m32_h(data), h->h_addr_list[i], (size_t)h->h_length);
            m32_wr(addrs + 4 * i, data);
        }
        m32_wr(addrs + 4 * nd, 0);
        for (uint32_t i = 0; i <= na; i++) {
            const char *str = i < na ? h->h_aliases[i] : h->h_name;
            memcpy(m32_h(data), str, strlen(str) + 1);
            m32_wr(i < na ? aliases + 4 * i : g, data);
            data += (uint32_t)strlen(str) + 1;
        }
        m32_wr(aliases + 4 * na, 0);
        m32_free(last);
        last = g;
    }
    pthread_mutex_unlock(&lock);
    return RETV_PLAIN(g);
}

/* pipe(int fds[2]): an array the generator cannot describe, the same bytes to both sides (libevent's wakeup pipe in
 * Left 4 Dead 2's Chromium) */
static int sp_pipe(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t fds = m32_arg(cpu, 0);
    RETV(fds ? pipe((int *)m32_h(fds)) : (errno = EFAULT, -1));
}

/* ioctl(fd, request, ...): one argument, guest memory when the request carries data (IOC_IN/IOC_OUT), else a number */
static int sp_ioctl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    unsigned long req = m32_arg(cpu, 1);
    uint32_t a = m32_arg(cpu, 2);
    RETV(ioctl((int)m32_arg(cpu, 0), req, (req & (IOC_IN | IOC_OUT)) && a ? m32_h(a) : (void *)(uintptr_t)a));
}

/* execv/execve: the vectors hold 4-byte pointers; the new program runs under ocerz as well (an i386 one by m32) */
static int exec_guest(struct OcerzVM *vm, OcerzCPU *cpu, uint32_t genv)
{
    extern char **environ;
    char *v[2][512];
    uint32_t gv[2] = { m32_arg(cpu, 1), genv };
    for (int k = 0; k < 2; k++) {
        int n = 0;
        for (uint32_t p; gv[k] && n < 511 && (p = m32_rd(gv[k] + 4 * (uint32_t)n)); n++)
            v[k][n] = (char *)m32_h(p);
        v[k][n] = NULL;
    }
    errno = ocerz_guest_execve(vm, cpu, (const char *)m32_h(m32_arg(cpu, 0)), v[0], genv ? v[1] : environ);
    RETV(-1);
}
static int sp_execv(struct OcerzVM *vm, OcerzCPU *cpu) { return exec_guest(vm, cpu, 0); }
static int sp_execve(struct OcerzVM *vm, OcerzCPU *cpu) { return exec_guest(vm, cpu, m32_arg(cpu, 2)); }

/* posix_spawn(pid_t *pid, path, file_actions *, attr *, argv, envp) and posix_spawnp: the vectors hold 4-byte
 * pointers, and the file actions and attributes are the host's own, behind the handle their _init calls left in the
 * guest's slot (the database crosses those as one pointer written back).  The program starts under ocerz as the
 * syscall's does (ocerz_guest_posix_spawn), an i386 one by m32 and POSIX_SPAWN_SETEXEC in place of this one.  The
 * database had no shape for the vectors and answered 0 without starting anything: Wine fell back to execv.
 * posix_spawnp looks a name without a slash up in PATH, as libc does. */
static int spawn_guest(struct OcerzVM *vm, OcerzCPU *cpu, int search)
{
    extern char **environ;
    char *v[2][512];
    uint32_t gv[2] = { m32_arg(cpu, 4), m32_arg(cpu, 5) };
    for (int k = 0; k < 2; k++) {
        int n = 0;
        for (uint32_t p; gv[k] && n < 511 && (p = m32_rd(gv[k] + 4 * (uint32_t)n)); n++)
            v[k][n] = (char *)m32_h(p);
        v[k][n] = NULL;
    }
    uint32_t gpid = m32_arg(cpu, 0), gpath = m32_arg(cpu, 1), gfa = m32_arg(cpu, 2), gat = m32_arg(cpu, 3);
    posix_spawn_file_actions_t fa = gfa ? (posix_spawn_file_actions_t)m32_host(m32_rd(gfa)) : NULL;
    posix_spawnattr_t at = gat ? (posix_spawnattr_t)m32_host(m32_rd(gat)) : NULL;
    char *const *argv = gv[0] ? v[0] : NULL, *const *envp = gv[1] ? v[1] : environ;
    const char *path = gpath ? (const char *)m32_h(gpath) : NULL;
    int pid = 0, e;
    if (!path || !*path) {
        e = path ? ENOENT : EINVAL;
    } else if (search && !strchr(path, '/')) {
        const char *dirs = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
        e = ENOENT;
        for (const char *d = dirs; d; d = strchr(d, ':') ? strchr(d, ':') + 1 : NULL) {
            char full[PATH_MAX];
            size_t n = strcspn(d, ":");
            snprintf(full, sizeof full, "%.*s/%s", (int)n, n ? d : ".", path);
            e = ocerz_guest_posix_spawn(vm, cpu, &pid, full, gfa ? &fa : NULL, gat ? &at : NULL, argv, envp);
            if (e != ENOENT && e != ENOTDIR && e != EACCES)
                break;
        }
    } else {
        e = ocerz_guest_posix_spawn(vm, cpu, &pid, path, gfa ? &fa : NULL, gat ? &at : NULL, argv, envp);
    }
    if (!e && gpid)
        m32_wr(gpid, (uint32_t)pid);
    m32_ret(cpu, (uint32_t)e, 0, 0);   /* the error number itself, errno untouched */
    return OCERZ_STEP_OK;
}
/* syscall(number, ...): the raw system calls m32 answers (src/m32_selfload.c), in libc's convention: -1 and errno where
 * the kernel's sets the carry flag, a 64-bit result (lseek, thread_selfid) in edx:eax.  Being variadic it had no
 * crossing and answered 0, so syscall(SYS_thread_selfid) gave every thread the same id. */
static int sp_syscall(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP], back = m32_rd(esp);
    cpu->gpr[OCERZ_RAX] = m32_arg(cpu, 0);
    cpu->gpr[OCERZ_RSP] = esp + 4;   /* the number's slot stands for the return address: arguments at [sp + 4] */
    cpu->rip = back;
    int rc = m32_raw_syscall(vm, cpu, 0);
    if (rc != OCERZ_STEP_OK)
        return rc;
    cpu->gpr[OCERZ_RSP] = esp + 4;   /* as ret leaves it */
    if (cpu->rflags & OCERZ_CF) {
        errno = (int)(uint32_t)cpu->gpr[OCERZ_RAX];
        m32_save_errno(cpu);
        cpu->gpr[OCERZ_RAX] = 0xffffffffu;
        cpu->gpr[OCERZ_RDX] = 0xffffffffu;
    }
    return OCERZ_STEP_OK;
}
static int sp_posix_spawn(struct OcerzVM *vm, OcerzCPU *cpu) { return spawn_guest(vm, cpu, 0); }
static int sp_posix_spawnp(struct OcerzVM *vm, OcerzCPU *cpu) { return spawn_guest(vm, cpu, 1); }

/* keymgr: the process-wide pointers GCC 3 and 4's runtime support keeps under fixed keys, one lock each (crt3.o's
 * atexit list, libgcc's unwinder object list).  libkeymgr has no header in today's SDK, so its calls had no
 * declaration and answered 0 without writing the pointer back: Kega Fusion's crt3.o then read whatever the stack
 * held.  Here every key's pointer starts NULL, as keymgr's did, and one recursive lock stands for all the keys. */
static pthread_mutex_t g_keymgr_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static struct { uint32_t key, ptr; } g_keymgr[64];
static int g_nkeymgr;
static uint32_t *keymgr_slot(uint32_t key)
{
    for (int i = 0; i < g_nkeymgr; i++)
        if (g_keymgr[i].key == key)
            return &g_keymgr[i].ptr;
    if (g_nkeymgr == (int)(sizeof g_keymgr / sizeof g_keymgr[0]))
        return NULL;
    g_keymgr[g_nkeymgr].key = key;
    g_keymgr[g_nkeymgr].ptr = 0;
    return &g_keymgr[g_nkeymgr++].ptr;
}
/* int _keymgr_get_and_lock_processwide_ptr_2(unsigned key, void **result) */
static int sp_keymgr_get_lock_2(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_mutex_lock(&g_keymgr_lock);
    uint32_t *slot = keymgr_slot(m32_arg(cpu, 0));
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), slot ? *slot : 0);
    return RETV_PLAIN(slot ? 0 : ENOMEM);
}
/* void *_keymgr_get_and_lock_processwide_ptr(unsigned key) */
static int sp_keymgr_get_lock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_mutex_lock(&g_keymgr_lock);
    uint32_t *slot = keymgr_slot(m32_arg(cpu, 0));
    return RETV_PLAIN(slot ? *slot : 0);
}
/* int _keymgr_set_and_unlock_processwide_ptr(unsigned key, void *ptr) */
static int sp_keymgr_set_unlock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t *slot = keymgr_slot(m32_arg(cpu, 0));
    if (slot)
        *slot = m32_arg(cpu, 1);
    pthread_mutex_unlock(&g_keymgr_lock);
    return RETV_PLAIN(slot ? 0 : ENOMEM);
}
/* int _keymgr_unlock_processwide_ptr(unsigned key) */
static int sp_keymgr_unlock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pthread_mutex_unlock(&g_keymgr_lock);
    return RETV_PLAIN(0);
}

/* libcrypto 0.9.7's MD5_Init, MD5_Update and MD5_Final: the library is gone from Apple silicon, and CommonCrypto's
 * CC_MD5 calls do the same with a context of the same layout (92 bytes: four state words, the bit count's two, a
 * 64-byte block, its fill), so the guest's context is used where it is.  Firefox 3.6 links them. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
static int sp_MD5_Init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    return RETV_PLAIN(m32_arg(cpu, 0) ? CC_MD5_Init(m32_h(m32_arg(cpu, 0))) : 0);
}
static int sp_MD5_Update(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t c = m32_arg(cpu, 0), d = m32_arg(cpu, 1), n = m32_arg(cpu, 2);
    return RETV_PLAIN(c && (d || !n) ? CC_MD5_Update(m32_h(c), d ? m32_h(d) : "", n) : 0);
}
static int sp_MD5_Final(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t md = m32_arg(cpu, 0), c = m32_arg(cpu, 1);
    return RETV_PLAIN(md && c ? CC_MD5_Final(m32_h(md), m32_h(c)) : 0);
}
#pragma clang diagnostic pop

/* clCreateContext(props, n, devices, notify, data, errcode_ret) and clCreateContextFromType(props, type, notify, data,
 * errcode_ret): m32 crosses no OpenCL, and answered both with a NULL context and *errcode_ret as it was, which a
 * program that tests the error code took for CL_SUCCESS (GRID 2 calls clCreateContextFromType).  They answer NULL
 * and CL_DEVICE_NOT_FOUND (-1), as on a Mac with no OpenCL device. */
static int cl_no_context(OcerzCPU *cpu, int err_arg)
{
    uint32_t err = m32_arg(cpu, err_arg);
    if (err)
        m32_wr(err, (uint32_t)-1);
    return RETV_PLAIN(0);
}
static int sp_clCreateContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return cl_no_context(cpu, 5); }
static int sp_clCreateContextFromType(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return cl_no_context(cpu, 4); }

/* localeconv() and localeconv_l(locale): a struct lconv is ten string pointers and fourteen chars, 56 bytes on i386
 * (the chars at 40) and 96 on arm64 (at 80, clang -arch i386 and -arch arm64).  Crossed, the guest read the host's
 * structure as its own, and GLib's g_ascii_strtod found an empty decimal point and asserted (Wesnoth).  The guest
 * gets an i386 copy, kept per thread, with the strings in guest memory. */
#include <locale.h>
#include <xlocale.h>
static uint32_t lconv_guest(const struct lconv *l)
{
    static __thread uint32_t g;
    if (!g)
        g = m32_static_alloc(56, 4);
    if (!g || !l)
        return 0;
    const char *const *s = (const char *const *)(const void *)l;
    for (uint32_t i = 0; i < 10; i++)
        m32_wr(g + 4 * i, m32_cstring(s[i]));
    memcpy(m32_h(g + 40), (const char *)l + 80, 14);
    return g;
}
static int sp_localeconv(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    return RETV_PLAIN(lconv_guest(localeconv()));
}
static int sp_localeconv_l(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *loc = m32_host(m32_arg(cpu, 0));
    return RETV_PLAIN(lconv_guest(localeconv_l(loc ? (locale_t)loc : LC_GLOBAL_LOCALE)));
}

/* getopt, getopt_long and getopt_long_only: argv is an array of guest pointers and a long option a 16-byte
 * {name, has_arg, flag, val}, and the state lives in variables the guest has its own copies of.  The host's
 * getopt runs on a host argv over the same strings, with the guest's optind, opterr and optreset copied in; optind,
 * optarg and optopt come back, and so does argv's order, which getopt_long permutes. */
/* the guest's copy of a libSystem variable, or 0 where the import is not one (no database: a trap address) */
static uint32_t data_cell(const char *name)
{
    uint32_t a = m32_export(NULL, name);
    return a - (uint32_t)OCERZ_DYLDAPI_LO < (uint32_t)(OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO) ? 0 : a;
}
static int getopt_common(OcerzCPU *cpu, int kind)
{
    int argc = (int)m32_arg(cpu, 0);
    uint32_t gargv = m32_arg(cpu, 1), gopts = m32_arg(cpu, 2), glong = kind ? m32_arg(cpu, 3) : 0;
    uint32_t gindex = kind ? m32_arg(cpu, 4) : 0;
    uint32_t c_optind = data_cell("_optind"), c_optarg = data_cell("_optarg");
    uint32_t c_opterr = data_cell("_opterr"), c_optopt = data_cell("_optopt");
    uint32_t c_optreset = data_cell("_optreset");
    if (argc < 0 || argc > 4096 || !gargv || !gopts) {
        m32_ret(cpu, (uint32_t)-1, 0, 0);
        return OCERZ_STEP_OK;
    }
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    for (int i = 0; i < argc; i++) {
        uint32_t s = m32_rd(gargv + 4 * (uint32_t)i);
        argv[i] = s ? (char *)m32_h(s) : NULL;
    }
    int nlong = 0;
    while (glong && m32_rd(glong + 16 * (uint32_t)nlong) && nlong < 1024)
        nlong++;
    struct option *lo = calloc((size_t)nlong + 1, sizeof *lo);
    for (int i = 0; i < nlong; i++) {
        uint32_t at = glong + 16 * (uint32_t)i, flag = m32_rd(at + 8);
        lo[i].name = (const char *)m32_h(m32_rd(at));
        lo[i].has_arg = (int)m32_rd(at + 4);
        lo[i].flag = flag ? (int *)m32_h(flag) : NULL;
        lo[i].val = (int)m32_rd(at + 12);
    }
    if (c_optind) optind = (int)m32_rd(c_optind);
    if (c_opterr) opterr = (int)m32_rd(c_opterr);
    if (c_optreset) optreset = (int)m32_rd(c_optreset);
    int longindex = gindex ? (int)m32_rd(gindex) : 0;
    const char *opts = (const char *)m32_h(gopts);
    int r = kind == 0 ? getopt(argc, argv, opts)
          : kind == 1 ? getopt_long(argc, argv, opts, lo, gindex ? &longindex : NULL)
                      : getopt_long_only(argc, argv, opts, lo, gindex ? &longindex : NULL);
    for (int i = 0; i < argc; i++)
        m32_wr(gargv + 4 * (uint32_t)i, argv[i] ? (uint32_t)ocerz_h2g(argv[i]) : 0);
    if (gindex)
        m32_wr(gindex, (uint32_t)longindex);
    if (c_optind) m32_wr(c_optind, (uint32_t)optind);
    if (c_optarg) m32_wr(c_optarg, optarg ? (uint32_t)ocerz_h2g(optarg) : 0);
    if (c_optopt) m32_wr(c_optopt, (uint32_t)optopt);
    if (c_optreset) m32_wr(c_optreset, (uint32_t)optreset);
    free(argv);
    free(lo);
    m32_ret(cpu, (uint32_t)r, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_getopt(struct OcerzVM *vm, OcerzCPU *cpu) { return getopt_common(cpu, 0); }
static int sp_getopt_long(struct OcerzVM *vm, OcerzCPU *cpu) { return getopt_common(cpu, 1); }
static int sp_getopt_long_only(struct OcerzVM *vm, OcerzCPU *cpu) { return getopt_common(cpu, 2); }

/* kevent and kevent64 take arrays, nchanges structures in and up to nevents out, where the database's r{} carries
 * one.  i386's struct kevent is 20 bytes, {ident, filter, flags, fflags, data, udata} with 4-byte ident, data and
 * udata, against 32 on the host; kevent64_s is the same 48 bytes on both; a timeout's two longs are 4 bytes each.
 * udata comes back as the guest stored it.  Wine's server waits on its clients' sockets this way. */
static int kevent_timeout(uint32_t g, struct timespec *ts)
{
    if (!g)
        return 0;
    ts->tv_sec = (int32_t)m32_rd(g);
    ts->tv_nsec = (int32_t)m32_rd(g + 4);
    return 1;
}
static int sp_kevent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int kq = (int)m32_arg(cpu, 0), nch = (int)m32_arg(cpu, 2), nev = (int)m32_arg(cpu, 4);
    uint32_t gch = m32_arg(cpu, 1), gev = m32_arg(cpu, 3);
    struct timespec ts;
    int timed = kevent_timeout(m32_arg(cpu, 5), &ts);
    if (nch < 0 || nev < 0 || nch > 65536 || nev > 65536) {
        errno = EINVAL;
        RETV(-1);
    }
    struct kevent *ch = calloc((size_t)nch + 1, sizeof *ch), *ev = calloc((size_t)nev + 1, sizeof *ev);
    for (int i = 0; i < nch; i++) {
        const uint8_t *g = m32_h(gch + 20 * (uint32_t)i);
        int16_t filter; uint16_t flags; uint32_t ident, fflags, udata; int32_t data;
        memcpy(&ident, g, 4); memcpy(&filter, g + 4, 2); memcpy(&flags, g + 6, 2);
        memcpy(&fflags, g + 8, 4); memcpy(&data, g + 12, 4); memcpy(&udata, g + 16, 4);
        EV_SET(&ch[i], ident, filter, flags, fflags, data, (void *)(uintptr_t)udata);
    }
    int r = kevent(kq, ch, nch, ev, nev, timed ? &ts : NULL);
    int err = errno;
    for (int i = 0; i < r; i++) {
        uint8_t *g = m32_h(gev + 20 * (uint32_t)i);
        uint32_t ident = (uint32_t)ev[i].ident, fflags = ev[i].fflags, udata = (uint32_t)(uintptr_t)ev[i].udata;
        int32_t data = (int32_t)ev[i].data;
        memcpy(g, &ident, 4); memcpy(g + 4, &ev[i].filter, 2); memcpy(g + 6, &ev[i].flags, 2);
        memcpy(g + 8, &fflags, 4); memcpy(g + 12, &data, 4); memcpy(g + 16, &udata, 4);
    }
    free(ch);
    free(ev);
    errno = err;
    RETV(r);
}
static int sp_kevent64(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int kq = (int)m32_arg(cpu, 0), nch = (int)m32_arg(cpu, 2), nev = (int)m32_arg(cpu, 4);
    uint32_t gch = m32_arg(cpu, 1), gev = m32_arg(cpu, 3), flags = m32_arg(cpu, 5);
    struct timespec ts;
    int timed = kevent_timeout(m32_arg(cpu, 6), &ts);
    RETV(kevent64(kq, nch > 0 ? m32_h(gch) : NULL, nch, nev > 0 ? m32_h(gev) : NULL, nev, flags,
                  timed ? &ts : NULL));
}

/* bootstrap_register takes a name_t, a char[128], which crosses as the pointer C passes (look_up and check_in
 * are above); the ports are names */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static int sp_bootstrap_register(struct OcerzVM *vm, OcerzCPU *cpu)
{
    RETV(bootstrap_register(m32_arg(cpu, 0), (char *)m32_h(m32_arg(cpu, 1)), m32_arg(cpu, 2)));
}
#pragma clang diagnostic pop

/* fork and vfork: in the child libSystem sets mach_task_self_ and bootstrap_port afresh, since the names can
 * differ there, and so are the guest's copies of them (Wine's server forks to run as a daemon and then looks for
 * the bootstrap port).  vfork runs as fork, the child having its own copy of the window. */
static int sp_fork(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pid_t pid = fork();
    int err = errno;
    if (pid == 0) {
        m32_refill_data("_mach_task_self_");
        m32_refill_data("_bootstrap_port");
    }
    errno = err;
    RETV(pid);
}

/* sendmsg and recvmsg: i386's struct msghdr is {name, namelen, iov, iovlen, control, controllen, flags} in 28 bytes
 * and an iovec {base, len} 8, where the database converts the header and hands the host an i386 iovec array.  The
 * control data, the descriptors SCM_RIGHTS passes among it, has one layout on both (4-byte cmsghdr fields and
 * alignment), and so do the names.  Wine's client and server talk this way. */
#define M32_MSG_IOV_MAX 1024
static int msg_in(uint32_t g, struct msghdr *m, struct iovec *iov)
{
    uint32_t name = m32_rd(g), giov = m32_rd(g + 8), n = m32_rd(g + 12), control = m32_rd(g + 16);
    if (n > M32_MSG_IOV_MAX)
        return -1;
    memset(m, 0, sizeof *m);
    m->msg_name = name ? m32_h(name) : NULL;
    m->msg_namelen = m32_rd(g + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t base = m32_rd(giov + 8 * i);
        iov[i].iov_base = base ? m32_h(base) : NULL;
        iov[i].iov_len = m32_rd(giov + 8 * i + 4);
    }
    m->msg_iov = n ? iov : NULL;
    m->msg_iovlen = (int)n;
    m->msg_control = control ? m32_h(control) : NULL;
    m->msg_controllen = m32_rd(g + 20);
    m->msg_flags = (int)m32_rd(g + 24);
    return 0;
}
static int sp_sendmsg(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct msghdr m;
    static __thread struct iovec iov[M32_MSG_IOV_MAX];
    if (msg_in(m32_arg(cpu, 1), &m, iov) != 0) {
        errno = EMSGSIZE;
        RETV(-1);
    }
    RETV(sendmsg((int)m32_arg(cpu, 0), &m, (int)m32_arg(cpu, 2)));
}
static int sp_recvmsg(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct msghdr m;
    static __thread struct iovec iov[M32_MSG_IOV_MAX];
    uint32_t g = m32_arg(cpu, 1);
    if (msg_in(g, &m, iov) != 0) {
        errno = EMSGSIZE;
        RETV(-1);
    }
    ssize_t r = recvmsg((int)m32_arg(cpu, 0), &m, (int)m32_arg(cpu, 2));
    int err = errno;
    if (r >= 0) {
        m32_wr(g + 4, m.msg_namelen);
        m32_wr(g + 20, m.msg_controllen);
        m32_wr(g + 24, (uint32_t)m.msg_flags);
    }
    errno = err;
    RETV(r);
}

/* i386_set_ldt and i386_get_ldt on ocerz's one table (ocerz_ldt_call), which a segment register load reads: Wine
 * gives each thread an %fs selector for its TEB this way, LDT_AUTO_ALLOC (-1) choosing the index.  The 8-byte
 * descriptors are read and written in the window. */
static int ldt_common(OcerzCPU *cpu, int num)
{
    long r = ocerz_ldt_call(num, (int32_t)m32_arg(cpu, 0), m32_arg(cpu, 1), (int32_t)m32_arg(cpu, 2));
    if (r < 0)
        errno = (int)-r;
    RETV(r < 0 ? -1 : r);
}
static int sp_i386_set_ldt(struct OcerzVM *vm, OcerzCPU *cpu) { return ldt_common(cpu, 5); }
static int sp_i386_get_ldt(struct OcerzVM *vm, OcerzCPU *cpu) { return ldt_common(cpu, 6); }

/* readv and writev, and the positioned preadv and pwritev: an array of i386 iovecs, as in a msghdr above (Wine's
 * client sends each request to its server with writev) */
static int iov_in(uint32_t giov, int n, struct iovec *iov)
{
    if (n < 0 || n > M32_MSG_IOV_MAX)
        return -1;
    for (int i = 0; i < n; i++) {
        uint32_t base = m32_rd(giov + 8 * (uint32_t)i);
        iov[i].iov_base = base ? m32_h(base) : NULL;
        iov[i].iov_len = m32_rd(giov + 8 * (uint32_t)i + 4);
    }
    return 0;
}
static int iov_call(OcerzCPU *cpu, int kind)
{
    static __thread struct iovec iov[M32_MSG_IOV_MAX];
    int fd = (int)m32_arg(cpu, 0), n = (int)m32_arg(cpu, 2);
    if (iov_in(m32_arg(cpu, 1), n, iov) != 0) {
        errno = EINVAL;
        RETV(-1);
    }
    off_t off = (off_t)(m32_arg(cpu, 3) | (uint64_t)m32_arg(cpu, 4) << 32);
    switch (kind) {
    case 0: RETV(readv(fd, iov, n));
    case 1: RETV(writev(fd, iov, n));
    case 2: RETV(preadv(fd, iov, n, off));
    default: RETV(pwritev(fd, iov, n, off));
    }
}
static int sp_readv(struct OcerzVM *vm, OcerzCPU *cpu) { return iov_call(cpu, 0); }
static int sp_writev(struct OcerzVM *vm, OcerzCPU *cpu) { return iov_call(cpu, 1); }
static int sp_preadv(struct OcerzVM *vm, OcerzCPU *cpu) { return iov_call(cpu, 2); }
static int sp_pwritev(struct OcerzVM *vm, OcerzCPU *cpu) { return iov_call(cpu, 3); }

static const M32SpecialEntry g_specials[] = {
    { "_readv", sp_readv }, { "_writev", sp_writev }, { "_preadv", sp_preadv }, { "_pwritev", sp_pwritev },
    { "_i386_set_ldt", sp_i386_set_ldt }, { "_i386_get_ldt", sp_i386_get_ldt },
    { "_sendmsg", sp_sendmsg }, { "_recvmsg", sp_recvmsg },
    { "_fork", sp_fork }, { "_vfork", sp_fork },
    { "_kevent", sp_kevent }, { "_kevent64", sp_kevent64 },
    { "_bootstrap_register", sp_bootstrap_register },
    { "_getopt", sp_getopt }, { "_getopt_long", sp_getopt_long }, { "_getopt_long_only", sp_getopt_long_only },
    { "_localeconv", sp_localeconv }, { "_localeconv_l", sp_localeconv_l },
    { "_MD5_Init", sp_MD5_Init }, { "_MD5_Update", sp_MD5_Update }, { "_MD5_Final", sp_MD5_Final },
    { "_clCreateContext", sp_clCreateContext }, { "_clCreateContextFromType", sp_clCreateContextFromType },
    { "_dispatch_once", sp_dispatch_once }, { "_dispatch_once_f", sp_dispatch_once_f },
    { "__keymgr_get_and_lock_processwide_ptr_2", sp_keymgr_get_lock_2 },
    { "__keymgr_get_and_lock_processwide_ptr", sp_keymgr_get_lock },
    { "__keymgr_set_and_unlock_processwide_ptr", sp_keymgr_set_unlock },
    { "__keymgr_unlock_processwide_ptr", sp_keymgr_unlock },
    { "_execv", sp_execv }, { "_execve", sp_execve }, { "_posix_spawn", sp_posix_spawn },
    { "_posix_spawnp", sp_posix_spawnp }, { "_syscall", sp_syscall },
    { "_ioctl", sp_ioctl }, { "_pipe", sp_pipe },
    { "_getaddrinfo", sp_getaddrinfo }, { "_freeaddrinfo", sp_freeaddrinfo }, { "_gethostbyname", sp_gethostbyname },
    { "_bootstrap_look_up", sp_bootstrap_look_up }, { "_bootstrap_check_in", sp_bootstrap_check_in },
    { "_mach_absolute_time", sp_mach_absolute_time }, { "_mach_timebase_info", sp_mach_timebase_info },
    { "_UpTime", sp_mach_absolute_time }, { "_AbsoluteToNanoseconds", sp_AbsoluteToNanoseconds },
    { "_AbsoluteToDuration", sp_AbsoluteToDuration },
    { "_strlen", sp_fast_strlen }, { "_strcmp", sp_fast_strcmp }, { "_strncmp", sp_fast_strncmp },
    { "_memcmp", sp_fast_memcmp }, { "_wcscmp", sp_fast_wcscmp }, { "_wcslen", sp_fast_wcslen },
    { "_memcpy", sp_fast_memcpy }, { "_memmove", sp_fast_memcpy }, { "_memset", sp_fast_memset },
    { "_OSMemoryBarrier", sp_fast_barrier },
    { "___isinff", sp_isinff }, { "___isnanf", sp_isnanf }, { "___isfinitef", sp_isfinitef },
    { "___isnormalf", sp_isnormalf }, { "___signbitf", sp_signbitf },
    { "___isinfd", sp_isinfd }, { "___isnand", sp_isnand }, { "___isfinited", sp_isfinited },
    { "_finite", sp_isfinited },   /* the old BSD name, which the SDK no longer declares (Left 4 Dead 2's vphysics) */
    { "___isnormald", sp_isnormald }, { "___signbitd", sp_signbitd },
    { "_aio_read", sp_aio_read }, { "_aio_write", sp_aio_write }, { "_aio_error", sp_aio_error },
    { "_aio_return", sp_aio_return }, { "_aio_cancel", sp_aio_cancel }, { "_aio_suspend", sp_aio_suspend },
    { "_aio_suspend$UNIX2003", sp_aio_suspend },
    { "_vm_region", sp_vm_region }, { "_vm_region_64", sp_vm_region }, { "_mach_vm_region", sp_mach_vm_region }, { "_vm_region_recurse_64", sp_vm_region_recurse_64 },
    { "_vm_read_overwrite", sp_vm_read_overwrite },
    { "_OSAtomicCompareAndSwap32", sp_osa_cas32 }, { "_OSAtomicCompareAndSwap32Barrier", sp_osa_cas32 },
    { "_OSAtomicCompareAndSwapInt", sp_osa_cas32 }, { "_OSAtomicCompareAndSwapIntBarrier", sp_osa_cas32 },
    { "_OSAtomicCompareAndSwapLong", sp_osa_cas32 }, { "_OSAtomicCompareAndSwapLongBarrier", sp_osa_cas32 },
    { "_OSAtomicCompareAndSwapPtr", sp_osa_cas32 }, { "_OSAtomicCompareAndSwapPtrBarrier", sp_osa_cas32 },
    { "_OSAtomicCompareAndSwap64", sp_osa_cas64 }, { "_OSAtomicCompareAndSwap64Barrier", sp_osa_cas64 },
    { "_OSAtomicAdd32", sp_osa_add32 }, { "_OSAtomicAdd32Barrier", sp_osa_add32 },
    { "_OSAtomicIncrement32", sp_osa_inc32 }, { "_OSAtomicIncrement32Barrier", sp_osa_inc32 },
    { "_OSAtomicDecrement32", sp_osa_dec32 }, { "_OSAtomicDecrement32Barrier", sp_osa_dec32 },
    { "_OSAtomicOr32", sp_osa_or32 }, { "_OSAtomicOr32Barrier", sp_osa_or32 },
    { "_OSAtomicAnd32", sp_osa_and32 }, { "_OSAtomicAnd32Barrier", sp_osa_and32 },
    { "_OSAtomicAdd64", sp_osa_add64 }, { "_OSAtomicAdd64Barrier", sp_osa_add64 },
    { "_OSSpinLockLock", sp_spin_lock }, { "_OSSpinLockTry", sp_spin_try }, { "_OSSpinLockUnlock", sp_spin_unlock },
    { "_sinf", sp_fast_sinf }, { "_cosf", sp_fast_cosf }, { "_tanf", sp_fast_tanf }, { "_sqrtf", sp_fast_sqrtf },
    { "_logf", sp_fast_logf }, { "_expf", sp_fast_expf }, { "_acosf", sp_fast_acosf }, { "_asinf", sp_fast_asinf },
    { "_atanf", sp_fast_atanf }, { "_floorf", sp_fast_floorf }, { "_ceilf", sp_fast_ceilf },
    { "_powf", sp_fast_powf }, { "_atan2f", sp_fast_atan2f }, { "_fmodf", sp_fast_fmodf },
    { "_sin", sp_fast_sin }, { "_cos", sp_fast_cos }, { "_tan", sp_fast_tan }, { "_sqrt", sp_fast_sqrt },
    { "_log", sp_fast_log }, { "_exp", sp_fast_exp }, { "_acos", sp_fast_acos }, { "_asin", sp_fast_asin },
    { "_atan", sp_fast_atan }, { "_floor", sp_fast_floor }, { "_ceil", sp_fast_ceil }, { "_log10", sp_fast_log10 },
    { "_pow", sp_fast_pow }, { "_atan2", sp_fast_atan2 }, { "_fmod", sp_fast_fmod },
    { "___udivdi3", sp_udivdi3 }, { "___umoddi3", sp_umoddi3 }, { "___divdi3", sp_divdi3 },
    { "___moddi3", sp_moddi3 }, { "___muldi3", sp_muldi3 }, { "___ashldi3", sp_ashldi3 },
    { "___ashrdi3", sp_ashrdi3 }, { "___lshrdi3", sp_lshrdi3 }, { "___cmpdi2", sp_cmpdi2 },
    { "___ucmpdi2", sp_ucmpdi2 }, { "___fixunsdfdi", sp_fixunsdfdi }, { "___fixunssfdi", sp_fixunssfdi },
    { "___fixdfdi", sp_fixdfdi }, { "___fixsfdi", sp_fixsfdi }, { "___floatdidf", sp_floatdidf },
    { "___floatundidf", sp_floatundidf }, { "___floatdisf", sp_floatdisf }, { "___floatundisf", sp_floatundisf },
    { "___memcpy_chk", sp_memcpy_chk }, { "___memmove_chk", sp_memcpy_chk }, { "___memset_chk", sp_memset_chk },
    { "___strcpy_chk", sp_strcpy_chk }, { "___stpcpy_chk", sp_stpcpy_chk }, { "___strcat_chk", sp_strcat_chk },
    { "___strncpy_chk", sp_strncpy_chk }, { "___strncat_chk", sp_strncat_chk },
    { "___strlcpy_chk", sp_strlcpy_chk }, { "___strlcat_chk", sp_strlcat_chk },
    { "_stat", sp_stat }, { "_lstat", sp_lstat }, { "_fstat", sp_fstat },
    { "_stat$INODE64", sp_stat64 }, { "_lstat$INODE64", sp_lstat64 }, { "_fstat$INODE64", sp_fstat64 },
    { "_stat64", sp_stat64 }, { "_lstat64", sp_lstat64 }, { "_fstat64", sp_fstat64 },
    { "_statfs", sp_statfs }, { "_fstatfs", sp_fstatfs }, { "_statfs$INODE64", sp_statfs64 },
    { "_fstatfs$INODE64", sp_fstatfs64 }, { "_statfs64", sp_statfs64 }, { "_fstatfs64", sp_fstatfs64 },
    { "_readdir", sp_readdir }, { "_readdir$INODE64", sp_readdir64 },
    { "_scandir", sp_scandir }, { "_scandir$INODE64", sp_scandir64 }, { "_alphasort", sp_alphasort },
    { "_alphasort$INODE64", sp_alphasort64 },
    { "_readdir_r", sp_readdir_r }, { "_readdir_r$INODE64", sp_readdir_r64 }, { "_closedir", sp_closedir },
    { "_gmtime_r", sp_gmtime_r }, { "_localtime_r", sp_localtime_r }, { "_gmtime", sp_gmtime },
    { "_localtime", sp_localtime }, { "_mktime", sp_mktime }, { "_timegm", sp_timegm },
    { "_strftime", sp_strftime }, { "_asctime", sp_asctime }, { "_ctime", sp_ctime },
    { "_getrusage", sp_getrusage }, { "_gettimeofday", sp_gettimeofday }, { "_nanosleep", sp_nanosleep }, { "_select", sp_select },
    { "_open", sp_open }, { "_openat", sp_openat }, { "_fcntl", sp_fcntl },
    { "_NSAllocateMemoryPages", sp_NSAllocateMemoryPages }, { "_NSDeallocateMemoryPages", sp_NSDeallocateMemoryPages },
    { "_NSCopyMemoryPages", sp_NSCopyMemoryPages },
    { "_mmap", sp_mmap }, { "_munmap", sp_munmap }, { "_mprotect", sp_mprotect }, { "_madvise", sp_ok0 },
    { "_getpagesize", sp_getpagesize }, { "_sysconf", sp_sysconf }, { "_host_page_size", sp_host_page_size },
    { "__host_page_size", sp_host_page_size },
    { "_msync", sp_ok0 }, { "_vm_allocate", sp_vm_allocate }, { "_vm_deallocate", sp_vm_deallocate },
    { "_sbrk", sp_sbrk },
    { "_setjmp", sp_setjmp }, { "__setjmp", sp_setjmp }, { "_sigsetjmp", sp_setjmp },
    { "_longjmp", sp_longjmp }, { "__longjmp", sp_longjmp }, { "_siglongjmp", sp_longjmp },
    { "_signal", sp_signal }, { "_sigaction", sp_sigaction }, { "_sigaltstack", sp_sigaltstack },
    { "___bzero", sp_bzero }, { "_bzero", sp_bzero }, { "___stack_chk_fail", sp_stack_chk_fail },
    { "_exit", sp_exit },
    { "_SetFrontProcess", sp_SetFrontProcess }, { "_CPSSetFrontProcess", sp_SetFrontProcess },
    { "_CPSGetCurrentProcess", sp_CPSGetCurrentProcess }, { "__CPSGetCurrentProcess", sp_CPSGetCurrentProcess },
    { "_CPSEnableForegroundOperation", sp_CPSEnableForegroundOperation },
    { "__exit", sp__exit },
    { "__Exit", sp__exit },
    { "___cxa_atexit", sp_ok0 },   /* ponytail: C++ static destructors do not run at exit */
    { "___cxa_finalize", sp_ok0 },   /* nor at an image's unloading: nothing was registered */
    { "___keymgr_dwarf2_register_sections", sp_ok0 },   /* GCC's crt3 offering its EH frames; m32_unwind finds them */
    { "dyld_stub_binder", sp_stub_binder }, { "__tlv_bootstrap", m32_tlv_trap },
    { NULL, NULL }
};

static const M32SpecialEntry *const g_tables[] = { g_specials, m32_stdio_specials, m32_cf_specials, m32_thread_specials,
                                                   m32_dyld_specials, m32_objc_specials, m32_block_specials,
                                                   m32_exc_specials, m32_zlib_specials, m32_unwind_specials,
                                                   m32_gl_specials, m32_audio_specials, m32_atsu_specials,
                                                   m32_qd_specials, m32_agl_specials, m32_carbon_specials,
                                                   m32_controls_specials, m32_tsearch_specials,
                                                   m32_cg_specials, m32_iokit_specials };

static M32Special find_special(const char *name)
{
    for (size_t t = 0; t < sizeof g_tables / sizeof g_tables[0]; t++)
        for (const M32SpecialEntry *e = g_tables[t]; e->name; e++)
            if (!strcmp(e->name, name))
                return e->fn;
    return NULL;
}

/* the exact import name first ($INODE64 variants can differ), then without its variant suffix */
M32Special m32_special(const char *symbol)
{
    M32Special f = find_special(symbol);
    const char *dollar = strchr(symbol, '$');
    if (!f && dollar) {
        char base[256];
        snprintf(base, sizeof base, "%.*s", (int)(dollar - symbol), symbol);
        f = find_special(base);
    }
    return f ? f : m32_carbon_special(symbol);
}
