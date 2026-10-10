/*
 * m32's hand-written libSystem entry points (include/ocerz/m32.h): the ones whose 32-bit and 64-bit
 * forms differ in layout, that allocate for the caller, that take variadic arguments, or that exist only on i386.
 */
#include <dirent.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <wchar.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_objc.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

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
 * Host pages are 16 KB and i386 ones 4 KB: lengths round up to 16 KB, and mprotect/madvise are accepted without
 * effect (ponytail: a guard page an allocator protects stays writable; nothing we run relies on the fault). */
#define M32_MAP_FAILED 0xffffffffu
static uint32_t map_len(uint32_t len) { return (len + 0x3fffu) & ~0x3fffu; }

static int sp_mmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t want = m32_arg(cpu, 0), len = m32_arg(cpu, 1);
    int prot = (int)m32_arg(cpu, 2), flags = (int)m32_arg(cpu, 3), fd = (int)m32_arg(cpu, 4);
    uint64_t off = m32_arg64(cpu, 5);
    if (!len) {
        errno = EINVAL;
        RETV(M32_MAP_FAILED);
    }
    uint32_t size = map_len(len), g;
    if (flags & MAP_FIXED) {
        g = want;
        if ((g & 0x3fff) || (uint64_t)g + size > M32_HANDLE_LO) {
            errno = EINVAL;
            RETV(M32_MAP_FAILED);
        }
        ocerz_unmap(g, size);
        if (ocerz_map_fixed(g, size, PROT_READ | PROT_WRITE) != OCERZ_OK) {
            errno = ENOMEM;
            RETV(M32_MAP_FAILED);
        }
    } else {
        g = (uint32_t)ocerz_map_anywhere(size, PROT_READ | PROT_WRITE);
        if (!g || (uint64_t)g + size > M32_HANDLE_LO) {
            errno = ENOMEM;
            RETV(M32_MAP_FAILED);
        }
    }
    if (!(flags & MAP_ANON) && fd >= 0) {
        /* a file: shared mappings overlay the file where the offset allows it, anything else is a copy */
        if ((flags & MAP_SHARED) && !(off & 0x3fff) && ocerz_map_shared_file(g, size, prot | PROT_READ, fd, off) == OCERZ_OK)
            RETV(g);
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
    (void)prot;
    RETV(g);
}
static int sp_munmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t g = m32_arg(cpu, 0), len = m32_arg(cpu, 1);
    if (!(g & 0x3fff))
        ocerz_unmap(g, map_len(len));   /* ponytail: a partial 4 KB unmap keeps the host page */
    RETV(0);
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
        g = (uint32_t)ocerz_map_anywhere(size, PROT_READ | PROT_WRITE);
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

/* ---- signals: handlers are recorded, not delivered (no i386 signal frames yet) ---- */
static uint32_t g_sig_handler[NSIG];
static int sp_signal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t sig = m32_arg(cpu, 0), h = m32_arg(cpu, 1);
    if (sig == 0 || sig >= NSIG)
        RETV(0xffffffffu);   /* SIG_ERR */
    uint32_t old = g_sig_handler[sig];
    g_sig_handler[sig] = h;
    if (h > 1)
        m32_log_once("signal handler recorded (not delivered) for signal", sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "another");
    if (h == 1)   /* SIG_IGN is honored: SIGPIPE */
        signal((int)sig, SIG_IGN);
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
        m32_wr(old + 8, 0);
    }
    if (act) {
        uint32_t h = m32_rd(act);
        g_sig_handler[sig] = h;
        if (h == 1)
            signal((int)sig, SIG_IGN);
        else if (h > 1)
            m32_log_once("sigaction handler recorded (not delivered) for signal", sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "another");
    }
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

uint32_t m32_special_data(const char *symbol, void *host_var)
{
    (void)host_var;
    if (!strcmp(symbol, "__DefaultRuneLocale"))
        return rune_locale();
    /* object constants CoreFoundation exports but only Foundation's headers declare (so the database has no
     * record): the guest reads the object */
    if (host_var && (!strcmp(symbol, "_NSDefaultRunLoopMode") || !strcmp(symbol, "_NSRunLoopCommonModes"))) {
        uint32_t g = m32_static_alloc(4, 4);
        m32_wr(g, m32_objc_to_guest(*(void **)host_var));
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
static int window_region(uint32_t gaddr_cell, uint32_t gsize_cell, mach_vm_address_t a, mach_vm_size_t sz, kern_return_t kr)
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
    }
    return kr;
}
/* vm_region / vm_region_64(task, address*, size*, flavor, info, count*, object_name*) */
static int sp_vm_region(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t ga = A(1), gs = A(2), gcnt = A(5), gobj = A(6);
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
    kr = window_region(ga, gs, a, sz, kr);
    if (kr == KERN_SUCCESS) {
        m32_wr(gcnt, cnt);
        if (gobj)
            m32_wr(gobj, 0);
    }
    m32_ret(cpu, (uint32_t)kr, 0, 0);
    return OCERZ_STEP_OK;
}
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
    kr = window_region(ga, gs, a, sz, kr);
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

static const M32SpecialEntry g_specials[] = {
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
    { "___isnormald", sp_isnormald }, { "___signbitd", sp_signbitd },
    { "_aio_read", sp_aio_read }, { "_aio_write", sp_aio_write }, { "_aio_error", sp_aio_error },
    { "_aio_return", sp_aio_return }, { "_aio_cancel", sp_aio_cancel }, { "_aio_suspend", sp_aio_suspend },
    { "_aio_suspend$UNIX2003", sp_aio_suspend },
    { "_vm_region", sp_vm_region }, { "_vm_region_64", sp_vm_region }, { "_vm_region_recurse_64", sp_vm_region_recurse_64 },
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
    { "_stat", sp_stat }, { "_lstat", sp_lstat }, { "_fstat", sp_fstat },
    { "_stat$INODE64", sp_stat64 }, { "_lstat$INODE64", sp_lstat64 }, { "_fstat$INODE64", sp_fstat64 },
    { "_stat64", sp_stat64 }, { "_lstat64", sp_lstat64 }, { "_fstat64", sp_fstat64 },
    { "_readdir", sp_readdir }, { "_readdir$INODE64", sp_readdir64 },
    { "_readdir_r", sp_readdir_r }, { "_readdir_r$INODE64", sp_readdir_r64 }, { "_closedir", sp_closedir },
    { "_gmtime_r", sp_gmtime_r }, { "_localtime_r", sp_localtime_r }, { "_gmtime", sp_gmtime },
    { "_localtime", sp_localtime }, { "_mktime", sp_mktime }, { "_timegm", sp_timegm },
    { "_strftime", sp_strftime }, { "_asctime", sp_asctime }, { "_ctime", sp_ctime },
    { "_gettimeofday", sp_gettimeofday }, { "_nanosleep", sp_nanosleep }, { "_select", sp_select },
    { "_open", sp_open }, { "_openat", sp_openat }, { "_fcntl", sp_fcntl },
    { "_mmap", sp_mmap }, { "_munmap", sp_munmap }, { "_mprotect", sp_ok0 }, { "_madvise", sp_ok0 },
    { "_msync", sp_ok0 }, { "_vm_allocate", sp_vm_allocate }, { "_vm_deallocate", sp_vm_deallocate },
    { "_sbrk", sp_sbrk },
    { "_setjmp", sp_setjmp }, { "__setjmp", sp_setjmp }, { "_sigsetjmp", sp_setjmp },
    { "_longjmp", sp_longjmp }, { "__longjmp", sp_longjmp }, { "_siglongjmp", sp_longjmp },
    { "_signal", sp_signal }, { "_sigaction", sp_sigaction }, { "_sigaltstack", sp_ok0 },
    { "___bzero", sp_bzero }, { "_bzero", sp_bzero }, { "___stack_chk_fail", sp_stack_chk_fail },
    { "_exit", sp_exit },
    { "__exit", sp__exit },
    { "__Exit", sp__exit },
    { "___cxa_atexit", sp_ok0 },   /* ponytail: C++ static destructors do not run at exit */
    { "dyld_stub_binder", sp_stub_binder }, { "__tlv_bootstrap", m32_tlv_trap },
    { NULL, NULL }
};

static const M32SpecialEntry *const g_tables[] = { g_specials, m32_stdio_specials, m32_cf_specials, m32_thread_specials,
                                                   m32_dyld_specials, m32_objc_specials, m32_block_specials,
                                                   m32_exc_specials, m32_zlib_specials, m32_unwind_specials,
                                                   m32_gl_specials, m32_audio_specials };

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
    return f;
}
