/*
 * The on-disk translation cache's store.
 *
 * ocerz/tcache.h states the interface; what follows is how records are kept.
 *
 * ---- where ----
 * Records live in tc-<fingerprint> under $HOME/Library/Caches/ocerz, or under
 * OCERZ_TCACHE_DIR when that is set.  The fingerprint is a hash of everything
 * that changes what a translation looks like without changing the guest bytes
 * it came from: this ocerz binary's LC_UUID, the memory mode and the host
 * bases it maps the guest at, the machine model, and every OCERZ_ variable in
 * the environment apart from the few that only steer logging, the loader or
 * this cache.  A different build or a different knob therefore opens a
 * different directory, and nothing inside one directory needs a version check
 * of its own.  Opening a store touches its directory.  Directories of other
 * fingerprints are removed once they have not been touched for a day, and the
 * least recently touched go sooner while they hold more than 2 GB together, so
 * rebuilding ocerz does not pile up stores; one touched within the last hour
 * is never removed, because a Wine session uses several fingerprints at once.
 *
 * ---- one index, many data files ----
 * The directory holds one index, shared by every process that uses it, and
 * one data file per process that has written anything, d-<n>.td.  The index
 * is a header and an open-addressed table of (key, location) slots that every
 * process maps shared and changes with atomic operations alone: a location
 * names a data file and an offset in it, and a key is claimed with a
 * compare-and-swap before its location is stored.  A process appends to its
 * own data file only, and publishes a record's location only after the record
 * is written, so a reader that finds a location finds the whole record behind
 * it; a record written again, because the guest bytes under it changed, gets
 * a new location in the same slot, and the newest one wins.  What one process
 * translates is therefore loaded, not translated again, by every process that
 * starts after it, in the same session as well as in the next one.
 *
 * A record is kept LZ4-compressed, which makes it about 1.65 times smaller for
 * 1.6 us when it is written and 0.3 us when it is read, and carries a checksum
 * of what is stored, checked on every load, so a record torn by a crash is
 * refused rather than run.  The index is created under a temporary name and
 * linked into place, so no process ever maps half an index.
 *
 * Nothing is ever taken out of a store, so it only fills.  Writing stops, and
 * a note is printed once, when the data files add up to OCERZ_TCACHE_MAX_MB
 * (4096 by default) or the table runs out of room; reading goes on.  Every
 * process holds a shared flock on the directory's users file for as long as it
 * lives, and one that finds nobody else there (an exclusive flock succeeds)
 * and the store full removes it and starts it again, so a full store costs one
 * cold start rather than every translation from then on.
 *
 * The disk comes first.  The cache never writes while its volume has less than
 * OCERZ_TCACHE_MIN_FREE_MB free (10240 by default), checked before every
 * append, and a process takes at most half of what was free above that floor
 * when it opened the store.  A lone process that finds the volume already
 * under the floor removes the store, and the prune then removes every other
 * store that is more than an hour old.  A tester's Mac with a few GB free
 * filled up under a Steam session, stopped being able to swap, and panicked
 * on the watchdog; with the floor the cache gives up its speed instead.
 *
 * ---- in a process ----
 * A record put is copied into a 256 KB buffer and nothing more, so storing
 * costs the translator almost nothing; a full buffer goes to a writer thread,
 * which compresses its records, appends them to the data file and publishes
 * them.  When the process exits, execs or leaves through the non-main-thread
 * exit path, the last buffer is handed over too and the writer is given up to
 * three seconds to finish; a process killed outright loses what was still
 * queued, never half a record, since nothing is published before it is
 * written.  A data file
 * is mapped when a location first points into it and mapped again, larger,
 * when a location points past what is mapped.  A forked child keeps its
 * parent's mappings but not its parent's data file: it drops the buffer and
 * opens a data file of its own on its first write.
 *
 * The index is 64 MB, and the volume allocates all of it when the file is
 * created, so a page of it that no process has touched since is read from the
 * disk the first time a probe lands on it: about 70 us a fault.  A store that
 * -aot (src/jit.c) has just filled with a program's own code is exactly that,
 * and the first launch's misses on shared-cache blocks made 1300 such faults,
 * 0.1 s of xbench_dyn's 0.14.  So the thread that prunes other stores first
 * asks whether a stretch of the index is in memory (mincore) and, when it is
 * not, has the kernel read the whole index ahead (MADV_WILLNEED), while the
 * translator goes on: that launch now takes 0.03 s, a cold python3 start went
 * from 0.40 s to 0.33, and a warm one, whose index is in memory, is unchanged.
 * OCERZ_NO_TCACHE_PREFETCH=1 leaves the index to fault.  ocerz_tcache_has
 * probes the index for a key without reading the record, and ocerz_tcache_room
 * says whether the store is still under half of its size limit, which is where
 * -aot stops.
 *
 * ---- profiles ----
 * -aot cannot know which shared-cache code a program runs: ruby's and tclsh's
 * interpreters are themselves cache images, and the static closure of either
 * is 342 images and 800k function starts.  A launch knows, though, so every
 * process remembers the shared-cache rips it loaded or kept (each with the
 * memory model it ran them under) in a set of its own, and adds the new ones
 * to the program's profile when it flushes, execs or exits.  A profile lives
 * in profiles/ beside the tc- directories, named for the main executable's
 * identity (src/dyld.c, ocerz_dyld_file_ident) and the shared cache's UUID,
 * so a rebuilt ocerz or a changed knob, which open a new store, keep it, and
 * an OS update starts a new one.  It is a file of 8-byte entries appended
 * under an exclusive flock, after reading what is already there so an entry
 * is written once; a torn tail is cut off by the next writer, and the file
 * stops growing at OCERZ_TC_PROF_MAX entries.  A second file, named for the
 * identity alone (.execs), lists the programs this one was seen to execute
 * (python3's xcrun shim runs python3.9, which runs Python.app; ruby runs
 * /bin/sh and uname), one path a line.  A writer that finds all of its own
 * entries already there writes nothing, and keeping profiles costs a warm
 * launch nothing measurable (python3 0.187 s with and without, tclsh 0.109
 * and 0.108).  src/jit.c ("Ahead of time") says how -aot uses them.
 * OCERZ_NO_TCACHE_PROFILE=1 writes and reads none and, like the profiles
 * themselves, stays out of the fingerprint.
 */
#include "ocerz/tcache.h"
#include "ocerz/mem.h"
#include "ocerz/mode.h"
#include "ocerz/types.h"

#include <arm_acle.h>
#include <compression.h>
#include <dirent.h>
#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TC_IDX_MAGIC 0x32494354u
#define TC_DAT_MAGIC 0x32444354u
#define TC_ZREC_MAGIC 0x315a4354u
#define TC_SLOTS (1ull << 22)
#define TC_PROBE 64
#define TC_BUF_BYTES (256u << 10)
#define TC_OUT_BYTES (512u << 10)
#define TC_QMAX 4
#define TC_REC_MAX (256u << 10)
#define TC_LOC_OFF_BITS 40

typedef struct {
    uint32_t magic, version;
    uint64_t fp, slots, next_file, bytes, pad[3];
} TcIdxHdr;

typedef struct {
    uint64_t key, loc;
} TcSlot;

typedef struct {
    uint32_t magic, version;
    uint64_t fp;
} TcDatHdr;

typedef struct {
    const uint8_t *p;
    size_t len;
    int fd;
} TcFile;

typedef struct {
    uint32_t magic, size;
    uint64_t key, sum;
    uint32_t raw, zlen;
} TcStored;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mode = -1;
static uint64_t g_fp;
static char g_dir[1024];
static int g_opened;
static TcIdxHdr *g_hdr;
static TcSlot *g_slot;
static uint64_t g_mask;
static TcFile *g_file;
static uint64_t g_nfile;
static uint64_t g_cap_bytes, g_floor_bytes, g_room_end;
static int g_full;
static uint64_t g_dno;
static int g_dfd = -1;
static uint64_t g_doff;
static uint8_t *g_buf;
static size_t g_buf_n;
static int g_log = -1;
static int g_ufd = -1;
static uint8_t *g_zbuf, *g_obuf, *g_rbuf, *g_zscratch, *g_dscratch;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qwork = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_qdone = PTHREAD_COND_INITIALIZER;
static struct { uint8_t *p; size_t n; } g_q[TC_QMAX];
static int g_qn, g_busy, g_writer, g_nspare;
static uint8_t *g_spare[TC_QMAX];
static pthread_t g_writer_tid;

int ocerz_tcache_mode(void)
{
    if (g_mode < 0) {
        const char *e = getenv("OCERZ_TCACHE");
        int m = OCERZ_TC_ON;
        if (e && (!strcmp(e, "0") || !strcmp(e, "off")))
            m = OCERZ_TC_OFF;
        else if (e && !strcmp(e, "verify"))
            m = OCERZ_TC_VERIFY;
        else if (e && !strcmp(e, "roundtrip"))
            m = OCERZ_TC_ROUNDTRIP;
        g_mode = m;
    }
    return g_mode;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t mix(uint64_t h, uint64_t w)
{
    h = (h ^ w) * 0x9e3779b97f4a7c15ull;
    return h ^ (h >> 29);
}

/*
 * Every record loaded is summed before it is trusted.  A mix per word is a
 * chain of dependent multiplies, about 2 ms of a CoreFoundation program's
 * launch for the 9 MB it loads; the sum is CRC32C instead, in three lanes so
 * the instructions overlap, the lanes and the header then mixed together.  It
 * only has to catch a torn or damaged record, and each build of ocerz keeps
 * its own store, so no record written with the old sum is ever read with this.
 */
static uint64_t stored_sum(const TcStored *z)
{
    const uint8_t *p = (const uint8_t *)(z + 1);
    size_t n = z->size - sizeof *z;
    uint32_t c0 = 0x452821e6u, c1 = 0x38d01377u, c2 = 0xbe5466cfu;
    uint64_t w0, w1, w2;
    for (; n >= 24; p += 24, n -= 24) {
        memcpy(&w0, p, 8);
        memcpy(&w1, p + 8, 8);
        memcpy(&w2, p + 16, 8);
        c0 = __crc32cd(c0, w0);
        c1 = __crc32cd(c1, w1);
        c2 = __crc32cd(c2, w2);
    }
    for (; n >= 8; p += 8, n -= 8) {
        memcpy(&w0, p, 8);
        c0 = __crc32cd(c0, w0);
    }
    uint64_t h = mix(mix(0x452821e638d01377ull, z->key), ((uint64_t)z->raw << 32) | z->zlen);
    return mix(mix(h, ((uint64_t)c0 << 32) | c1), c2);
}

static int env_ignored(const char *kv)
{
    static const char *const skip[] = {
        "OCERZ_TCACHE=", "OCERZ_TCACHE_LOG=", "OCERZ_TCACHE_DIR=", "OCERZ_TCACHE_TRACE=",
        "OCERZ_TCACHE_MAX_MB=", "OCERZ_TCACHE_MIN_FREE_MB=", "OCERZ_GUESTPROF=", "OCERZ_GUESTPROF_PERIOD=", "OCERZ_LOWBASE=",
        "OCERZ_GUEST_DYLD_INSERT_LIBRARIES=", "OCERZ_PRELOAD_OBJC=", "OCERZ_GUEST_SLIDE=",
        "OCERZ_NO_TCACHE_PROFILE=", "OCERZ_AOT_NO_EXECS=", "OCERZ_NO_TCACHE_FOLD=", "OCERZ_NO_TCACHE_SLAB=",
    };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (!strncmp(kv, skip[i], strlen(skip[i])))
            return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static uint64_t fingerprint(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, "ocerz-tc-2", 10);
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (mh) {
        const uint8_t *lc = (const uint8_t *)(mh + 1);
        for (uint32_t i = 0; i < mh->ncmds; i++) {
            const struct load_command *c = (const struct load_command *)lc;
            if (c->cmd == LC_UUID)
                h = fnv(h, ((const struct uuid_command *)c)->uuid, 16);
            lc += c->cmdsize;
        }
    }
    uint64_t v[4] = { (uint64_t)ocerz_mode, ocerz_low_base, ocerz_top_base, ocerz_guest_base };
    h = fnv(h, v, sizeof v);
    char model[128] = "";
    size_t ml = sizeof model - 1;
    if (sysctlbyname("hw.model", model, &ml, NULL, 0) == 0)
        h = fnv(h, model, strnlen(model, sizeof model));
    size_t n = 0;
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "OCERZ_", 6) && !env_ignored(*e))
            n++;
    const char **kv = n ? (const char **)malloc(n * sizeof *kv) : NULL;
    if (kv) {
        size_t k = 0;
        for (char **e = environ; e && *e && k < n; e++)
            if (!strncmp(*e, "OCERZ_", 6) && !env_ignored(*e))
                kv[k++] = *e;
        qsort(kv, k, sizeof *kv, cmp_str);
        for (size_t i = 0; i < k; i++)
            h = fnv(h, kv[i], strlen(kv[i]) + 1);
        free(kv);
    }
    return h;
}

static int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* Bytes free to this user on the volume that holds the store. */
static uint64_t free_bytes(void)
{
    struct statfs s;
    return statfs(g_dir, &s) == 0 ? (uint64_t)s.f_bavail * s.f_bsize : UINT64_MAX;
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static uint64_t dir_bytes(const char *dir)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char path[1400];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) == 0)
            total += (uint64_t)st.st_blocks * 512;
    }
    closedir(d);
    return total;
}

static void *prune_other_dirs(void *arg)
{
    (void)arg;
    size_t len = sizeof(TcIdxHdr) + TC_SLOTS * sizeof(TcSlot), pg = (size_t)getpagesize(), out = 0;
    char vec[64];
    if (!getenv("OCERZ_NO_TCACHE_PREFETCH") && mincore((char *)g_hdr + len / 2, 64 * pg, vec) == 0) {
        for (size_t i = 0; i < 64; i++)
            out += !(vec[i] & MINCORE_INCORE);
        if (out > 4)
            madvise(g_hdr, len, MADV_WILLNEED);
    }
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", g_dir);
    char *slash = strrchr(parent, '/');
    if (!slash || strncmp(slash + 1, "tc-", 3))
        return NULL;
    *slash = 0;
    DIR *d = opendir(parent);
    if (!d)
        return NULL;
    struct { char path[1200]; time_t mt; uint64_t bytes; } keep[64];
    int nk = 0;
    uint64_t total = 0;
    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "tc-", 3) || strlen(de->d_name) != 19)
            continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", parent, de->d_name);
        if (!strcmp(path, g_dir))
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (now - st.st_mtime < 3600)
            continue;
        if (now - st.st_mtime > 86400 || nk == 64) {
            remove_dir(path);
            continue;
        }
        snprintf(keep[nk].path, sizeof keep[nk].path, "%s", path);
        keep[nk].mt = st.st_mtime;
        keep[nk].bytes = dir_bytes(path);
        total += keep[nk].bytes;
        nk++;
    }
    closedir(d);
    uint64_t budget = free_bytes() < g_floor_bytes ? 0 : 2ull << 30;
    while (nk > 0 && total > budget) {
        int oldest = 0;
        for (int i = 1; i < nk; i++)
            if (keep[i].mt < keep[oldest].mt) oldest = i;
        remove_dir(keep[oldest].path);
        total -= keep[oldest].bytes;
        keep[oldest] = keep[--nk];
    }
    return NULL;
}

static uint64_t slot_hash(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 29;
    return key;
}

static int open_index(void)
{
    char path[1200], tmp[1200];
    snprintf(path, sizeof path, "%s/index", g_dir);
    size_t len = sizeof(TcIdxHdr) + TC_SLOTS * sizeof(TcSlot);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        snprintf(tmp, sizeof tmp, "%s/index.%d", g_dir, (int)getpid());
        int t = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (t < 0)
            return 0;
        TcIdxHdr h = { TC_IDX_MAGIC, 1, g_fp, TC_SLOTS, 1, 0, { 0, 0, 0 } };
        if (ftruncate(t, (off_t)len) != 0 || pwrite(t, &h, sizeof h, 0) != (ssize_t)sizeof h) {
            close(t);
            unlink(tmp);
            return 0;
        }
        if (link(tmp, path) == 0) {
            fd = t;
        } else {
            close(t);
            fd = open(path, O_RDWR | O_CLOEXEC);
        }
        unlink(tmp);
        if (fd < 0)
            return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != len) {
        close(fd);
        return 0;
    }
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return 0;
    TcIdxHdr *h = (TcIdxHdr *)p;
    if (h->magic != TC_IDX_MAGIC || h->version != 1 || h->fp != g_fp || h->slots != TC_SLOTS) {
        munmap(p, len);
        return 0;
    }
    g_hdr = h;
    g_slot = (TcSlot *)(h + 1);
    g_mask = TC_SLOTS - 1;
    return 1;
}

static void reset_store(void)
{
    DIR *d = opendir(g_dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "d-", 2) && strcmp(de->d_name, "index"))
            continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", g_dir, de->d_name);
        unlink(path);
    }
    closedir(d);
}

static int claim_store(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/users", g_dir);
    g_ufd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g_ufd < 0)
        return 0;
    if (flock(g_ufd, LOCK_EX | LOCK_NB) == 0) {
        snprintf(path, sizeof path, "%s/index", g_dir);
        TcIdxHdr h;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h && (h.pad[0] || h.bytes > g_cap_bytes)) {
                if (g_log > 0)
                    fprintf(stderr, "ocerz: TCACHE[%d] %s is full; starting it again\n", (int)getpid(), g_dir);
                reset_store();
            } else if (free_bytes() < g_floor_bytes) {
                if (g_log > 0)
                    fprintf(stderr, "ocerz: TCACHE[%d] %s: the disk is under the floor; removing the store\n",
                            (int)getpid(), g_dir);
                reset_store();
            }
            close(fd);
        }
    }
    return flock(g_ufd, LOCK_SH) == 0;
}

static int open_store(void)
{
    if (g_opened)
        return g_hdr != NULL;
    g_opened = 1;
    if (g_log < 0)
        g_log = getenv("OCERZ_TCACHE_LOG") ? 1 : 0;
    const char *mx = getenv("OCERZ_TCACHE_MAX_MB");
    g_cap_bytes = (uint64_t)(mx && atoi(mx) > 0 ? atoi(mx) : 4096) << 20;
    const char *mf = getenv("OCERZ_TCACHE_MIN_FREE_MB");
    g_floor_bytes = (mf && *mf ? strtoull(mf, NULL, 10) : 10240) << 20;
    g_fp = fingerprint();
    const char *dir = getenv("OCERZ_TCACHE_DIR");
    const char *home = getenv("HOME");
    if (dir && dir[0])
        snprintf(g_dir, sizeof g_dir, "%s/tc-%016llx", dir, (unsigned long long)g_fp);
    else if (home && home[0])
        snprintf(g_dir, sizeof g_dir, "%s/Library/Caches/ocerz/tc-%016llx", home, (unsigned long long)g_fp);
    else
        return 0;
    if (mkdirs(g_dir) != 0 || !claim_store() || !open_index())
        return 0;
    utimes(g_dir, NULL);
    uint64_t fr = free_bytes();
    g_room_end = __atomic_load_n(&g_hdr->bytes, __ATOMIC_RELAXED) + (fr > g_floor_bytes ? (fr - g_floor_bytes) / 2 : 0);
    if (fr < g_floor_bytes) {
        g_full = 1;
        if (g_log > 0)
            fprintf(stderr, "ocerz: TCACHE[%d] %s: %llu MB free, under the %llu MB floor; not writing\n", (int)getpid(),
                    g_dir, (unsigned long long)(fr >> 20), (unsigned long long)(g_floor_bytes >> 20));
    }
    size_t ds = compression_decode_scratch_buffer_size(COMPRESSION_LZ4_RAW);
    g_rbuf = (uint8_t *)malloc(TC_REC_MAX);
    g_dscratch = (uint8_t *)malloc(ds ? ds : 1);
    if (!g_rbuf || !g_dscratch)
        return 0;
    if (g_log > 0)
        fprintf(stderr, "ocerz: TCACHE[%d] store %s mode=%d low=%#llx top=%#llx base=%#llx\n", (int)getpid(),
                g_dir, ocerz_mode, (unsigned long long)ocerz_low_base, (unsigned long long)ocerz_top_base,
                (unsigned long long)ocerz_guest_base);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, prune_other_dirs, NULL);
    pthread_attr_destroy(&at);
    return 1;
}

static const uint8_t *file_at(uint64_t no, uint64_t off, uint64_t need)
{
    if (!no || no >= (1ull << 24))
        return NULL;
    if (no >= g_nfile) {
        uint64_t n = g_nfile ? g_nfile : 64;
        while (n <= no) n *= 2;
        TcFile *nv = (TcFile *)realloc(g_file, n * sizeof *nv);
        if (!nv)
            return NULL;
        for (uint64_t i = g_nfile; i < n; i++) {
            nv[i].p = NULL;
            nv[i].len = 0;
            nv[i].fd = -1;
        }
        g_file = nv;
        g_nfile = n;
    }
    TcFile *f = &g_file[no];
    if (off + need <= f->len)
        return f->p + off;
    if (f->fd < 0) {
        char path[1200];
        snprintf(path, sizeof path, "%s/d-%llu.td", g_dir, (unsigned long long)no);
        f->fd = open(path, O_RDONLY | O_CLOEXEC);
        if (f->fd < 0)
            return NULL;
    }
    struct stat st;
    if (fstat(f->fd, &st) != 0 || (uint64_t)st.st_size < off + need)
        return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, f->fd, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (f->p)
        munmap((void *)f->p, f->len);
    f->p = (const uint8_t *)p;
    f->len = (size_t)st.st_size;
    return f->p + off;
}

/*
 * The store's directory, for other per-build caches kept beside it: NULL when
 * the store is off or did not open, and for writing when it is under the
 * free-space floor too.
 */
const char *ocerz_tcache_dir(int for_write)
{
    int m = ocerz_tcache_mode();
    if (m != OCERZ_TC_ON && m != OCERZ_TC_VERIFY)
        return NULL;
    pthread_mutex_lock(&g_lock);
    int ok = open_store();
    pthread_mutex_unlock(&g_lock);
    return ok && !(for_write && g_full) ? g_dir : NULL;
}

int ocerz_tcache_has(uint64_t key)
{
    int has = 0;
    pthread_mutex_lock(&g_lock);
    if (open_store()) {
        uint64_t i = slot_hash(key) & g_mask;
        for (int k = 0; k < TC_PROBE; k++, i = (i + 1) & g_mask) {
            uint64_t sk = __atomic_load_n(&g_slot[i].key, __ATOMIC_ACQUIRE);
            if (!sk || sk == key) {
                has = sk && __atomic_load_n(&g_slot[i].loc, __ATOMIC_ACQUIRE);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return has;
}

int ocerz_tcache_room(void)
{
    int m = ocerz_tcache_mode();
    if (m != OCERZ_TC_ON)
        return 0;
    pthread_mutex_lock(&g_lock);
    int ok = open_store() && !g_full && __atomic_load_n(&g_hdr->bytes, __ATOMIC_RELAXED) < g_cap_bytes / 2;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

const OcerzTcRecHead *ocerz_tcache_find(uint64_t key)
{
    const OcerzTcRecHead *found = NULL;
    pthread_mutex_lock(&g_lock);
    if (open_store()) {
        uint64_t i = slot_hash(key) & g_mask;
        for (int k = 0; k < TC_PROBE; k++, i = (i + 1) & g_mask) {
            uint64_t sk = __atomic_load_n(&g_slot[i].key, __ATOMIC_ACQUIRE);
            if (!sk)
                break;
            if (sk != key)
                continue;
            uint64_t loc = __atomic_load_n(&g_slot[i].loc, __ATOMIC_ACQUIRE);
            if (!loc)
                break;
            uint64_t no = loc >> TC_LOC_OFF_BITS, off = loc & ((1ull << TC_LOC_OFF_BITS) - 1);
            const TcStored *z = (const TcStored *)(const void *)file_at(no, off, sizeof *z);
            if (!z || z->magic != TC_ZREC_MAGIC || z->key != key || z->size < sizeof *z ||
                z->size > TC_REC_MAX || z->size % 8 || z->zlen > z->size - sizeof *z ||
                z->raw > TC_REC_MAX - sizeof(OcerzTcRecHead) || z->raw % 8)
                break;
            z = (const TcStored *)(const void *)file_at(no, off, z->size);
            if (!z || z->sum != stored_sum(z))
                break;
            OcerzTcRecHead *r = (OcerzTcRecHead *)(void *)g_rbuf;
            size_t got = z->zlen == z->raw
                ? (memcpy(r + 1, z + 1, z->raw), (size_t)z->raw)
                : compression_decode_buffer((uint8_t *)(r + 1), TC_REC_MAX - sizeof *r,
                                            (const uint8_t *)(z + 1), z->zlen, g_dscratch,
                                            COMPRESSION_LZ4_RAW);
            if (got != z->raw)
                break;
            r->magic = OCERZ_TC_REC_MAGIC;
            r->size = (uint32_t)(sizeof *r + z->raw);
            r->key = key;
            r->sum = z->sum;
            found = r;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void publish(uint64_t key, uint64_t loc)
{
    uint64_t i = slot_hash(key) & g_mask;
    for (int k = 0; k < TC_PROBE; k++, i = (i + 1) & g_mask) {
        uint64_t sk = __atomic_load_n(&g_slot[i].key, __ATOMIC_ACQUIRE);
        if (!sk) {
            uint64_t zero = 0;
            if (__atomic_compare_exchange_n(&g_slot[i].key, &zero, key, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                sk = key;
            else
                sk = zero;
        }
        if (sk == key) {
            __atomic_store_n(&g_slot[i].loc, loc, __ATOMIC_RELEASE);
            return;
        }
    }
    if (!g_full && g_log > 0)
        fprintf(stderr, "ocerz: TCACHE[%d] index has no room near key %#llx\n", (int)getpid(),
                (unsigned long long)key);
    g_full = 1;
    __atomic_store_n(&g_hdr->pad[0], 1, __ATOMIC_RELAXED);
}

static int open_data(void)
{
    if (g_dfd >= 0)
        return 1;
    for (int tries = 0; tries < 8; tries++) {
        uint64_t no = __atomic_fetch_add(&g_hdr->next_file, 1, __ATOMIC_RELAXED);
        if (!no || no >= (1ull << 24))
            return 0;
        char path[1200];
        snprintf(path, sizeof path, "%s/d-%llu.td", g_dir, (unsigned long long)no);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            continue;
        TcDatHdr h = { TC_DAT_MAGIC, 1, g_fp };
        if (pwrite(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) {
            close(fd);
            unlink(path);
            return 0;
        }
        g_dno = no;
        g_dfd = fd;
        g_doff = sizeof h;
        return 1;
    }
    return 0;
}

static void write_stored(const uint8_t *p, size_t n)
{
    if (!n || !g_hdr || g_full)
        return;
    if (__atomic_load_n(&g_hdr->bytes, __ATOMIC_RELAXED) + n > g_room_end || free_bytes() < g_floor_bytes + n) {
        if (g_log > 0)
            fprintf(stderr, "ocerz: TCACHE[%d] %s: out of disk room (%llu MB free); no longer writing\n",
                    (int)getpid(), g_dir, (unsigned long long)(free_bytes() >> 20));
        g_full = 1;
        return;
    }
    if (!open_data())
        return;
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(g_dfd, p + done, n - done, (off_t)(g_doff + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            close(g_dfd);
            g_dfd = -1;
            return;
        }
        done += (size_t)w;
    }
    for (size_t at = 0; at < n; ) {
        const TcStored *z = (const TcStored *)(const void *)(p + at);
        publish(z->key, (g_dno << TC_LOC_OFF_BITS) | (g_doff + at));
        at += z->size;
    }
    g_doff += n;
    uint64_t total = __atomic_add_fetch(&g_hdr->bytes, n, __ATOMIC_RELAXED);
    if (total > g_cap_bytes) {
        if (!g_full && g_log > 0)
            fprintf(stderr, "ocerz: TCACHE[%d] %s holds %llu MB; no longer writing\n", (int)getpid(), g_dir,
                    (unsigned long long)(total >> 20));
        g_full = 1;
        __atomic_store_n(&g_hdr->pad[0], 1, __ATOMIC_RELAXED);
    }
}

static void store_buffer(const uint8_t *raw_buf, size_t n)
{
    size_t o = 0;
    for (size_t at = 0; at < n; ) {
        const OcerzTcRecHead *rec = (const OcerzTcRecHead *)(const void *)(raw_buf + at);
        at += rec->size;
        uint32_t raw = rec->size - (uint32_t)sizeof *rec;
        size_t zl = compression_encode_buffer(g_zbuf, TC_REC_MAX, (const uint8_t *)(rec + 1), raw, g_zscratch,
                                              COMPRESSION_LZ4_RAW);
        const uint8_t *payload = g_zbuf;
        if (!zl || zl >= raw) {
            zl = raw;
            payload = (const uint8_t *)(rec + 1);
        }
        uint32_t size = (uint32_t)((sizeof(TcStored) + zl + 7) & ~(size_t)7);
        if (o + size > TC_OUT_BYTES) {
            write_stored(g_obuf, o);
            o = 0;
        }
        TcStored *z = (TcStored *)(void *)(g_obuf + o);
        z->magic = TC_ZREC_MAGIC;
        z->size = size;
        z->key = rec->key;
        z->raw = raw;
        z->zlen = (uint32_t)zl;
        memcpy(z + 1, payload, zl);
        memset((uint8_t *)(z + 1) + zl, 0, size - sizeof *z - zl);
        z->sum = stored_sum(z);
        o += size;
    }
    write_stored(g_obuf, o);
}

static void *writer_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_qlock);
    for (;;) {
        while (!g_qn)
            pthread_cond_wait(&g_qwork, &g_qlock);
        uint8_t *b = g_q[0].p;
        size_t n = g_q[0].n;
        g_busy = 1;
        pthread_mutex_unlock(&g_qlock);
        store_buffer(b, n);
        pthread_mutex_lock(&g_qlock);
        g_busy = 0;
        g_qn--;
        memmove(&g_q[0], &g_q[1], (size_t)g_qn * sizeof g_q[0]);
        if (g_nspare < TC_QMAX)
            g_spare[g_nspare++] = b;
        else
            free(b);
        pthread_cond_broadcast(&g_qdone);
    }
    return NULL;
}

static uint8_t *take_buffer(void)
{
    uint8_t *b = NULL;
    pthread_mutex_lock(&g_qlock);
    if (g_nspare)
        b = g_spare[--g_nspare];
    pthread_mutex_unlock(&g_qlock);
    return b ? b : (uint8_t *)malloc(TC_BUF_BYTES);
}

static void hand_off_locked(void)
{
    if (!g_buf || !g_buf_n)
        return;
    pthread_mutex_lock(&g_qlock);
    if (!g_writer) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        g_writer = pthread_create(&g_writer_tid, &at, writer_main, NULL) == 0;
        pthread_attr_destroy(&at);
    }
    if (!g_writer) {
        pthread_mutex_unlock(&g_qlock);
        store_buffer(g_buf, g_buf_n);
        g_buf_n = 0;
        return;
    }
    while (g_qn == TC_QMAX)
        pthread_cond_wait(&g_qdone, &g_qlock);
    g_q[g_qn].p = g_buf;
    g_q[g_qn].n = g_buf_n;
    g_qn++;
    pthread_cond_signal(&g_qwork);
    pthread_mutex_unlock(&g_qlock);
    g_buf = take_buffer();
    g_buf_n = 0;
}

static int profile_off(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_TCACHE_PROFILE") || ocerz_tcache_mode() != OCERZ_TC_ON;
    return off;
}

static int profile_path(char *out, size_t n, const char *leaf)
{
    const char *dir = getenv("OCERZ_TCACHE_DIR");
    const char *home = getenv("HOME");
    if (dir && dir[0])
        snprintf(out, n, "%s/profiles%s%s", dir, leaf ? "/" : "", leaf ? leaf : "");
    else if (home && home[0])
        snprintf(out, n, "%s/Library/Caches/ocerz/profiles%s%s", home, leaf ? "/" : "", leaf ? leaf : "");
    else
        return 0;
    return 1;
}

static void profile_rips_name(char *out, size_t n, uint64_t ident, const uint8_t *uuid)
{
    char leaf[80];
    int k = snprintf(leaf, sizeof leaf, "%016llx-", (unsigned long long)ident);
    for (int i = 0; i < 16; i++)
        k += snprintf(leaf + k, sizeof leaf - (size_t)k, "%02x", uuid[i]);
    snprintf(leaf + k, sizeof leaf - (size_t)k, ".rips");
    if (!profile_path(out, n, leaf))
        out[0] = 0;
}

static void profile_execs_name(char *out, size_t n, uint64_t ident)
{
    char leaf[40];
    snprintf(leaf, sizeof leaf, "%016llx.execs", (unsigned long long)ident);
    if (!profile_path(out, n, leaf))
        out[0] = 0;
}

static os_unfair_lock g_plock = OS_UNFAIR_LOCK_INIT;
static uint64_t *g_pset;
static size_t g_pcap, g_pn, g_pwritten;
static uint64_t g_pident;
static char g_prips[1200];

static int pset_add(uint64_t **set, size_t *cap, size_t *n, uint64_t v)
{
    if (2 * (*n + 1) > *cap) {
        size_t nc = *cap ? *cap * 2 : 8192;
        uint64_t *ns = (uint64_t *)calloc(nc, sizeof *ns);
        if (!ns)
            return 0;
        for (size_t i = 0; i < *cap; i++)
            if ((*set)[i]) {
                size_t j = (size_t)(slot_hash((*set)[i]) & (nc - 1));
                while (ns[j]) j = (j + 1) & (nc - 1);
                ns[j] = (*set)[i];
            }
        free(*set);
        *set = ns;
        *cap = nc;
    }
    size_t j = (size_t)(slot_hash(v) & (*cap - 1));
    while ((*set)[j] && (*set)[j] != v) j = (j + 1) & (*cap - 1);
    if ((*set)[j])
        return 0;
    (*set)[j] = v;
    (*n)++;
    return 1;
}

static int pset_has(const uint64_t *set, size_t cap, uint64_t v)
{
    if (!v || !cap)
        return 0;
    size_t j = (size_t)(slot_hash(v) & (cap - 1));
    while (set[j] && set[j] != v) j = (j + 1) & (cap - 1);
    return set[j] != 0;
}

static void profile_write(void)
{
    os_unfair_lock_lock(&g_plock);
    if (!g_prips[0] || g_pwritten == g_pn) {
        os_unfair_lock_unlock(&g_plock);
        return;
    }
    char dir[1200];
    int fd = -1;
    if (profile_path(dir, sizeof dir, NULL) && mkdirs(dir) == 0)
        fd = open(g_prips, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    struct stat st;
    if (fd >= 0 && flock(fd, LOCK_EX) == 0 && fstat(fd, &st) == 0) {
        size_t have = (size_t)st.st_size / 8, fcap = 0, fn = 0, nout = 0;
        uint64_t *old = have && have < OCERZ_TC_PROF_MAX ? (uint64_t *)malloc(have * 8) : NULL;
        uint64_t *fset = NULL, *out = have < OCERZ_TC_PROF_MAX ? (uint64_t *)malloc(g_pn * 8) : NULL;
        size_t held = 0;
        if (old && pread(fd, old, have * 8, 0) == (ssize_t)(have * 8))
            for (size_t i = 0; i < have; i++)
                held += pset_has(g_pset, g_pcap, old[i]);
        for (size_t i = 0; held < g_pn && old && i < have; i++)
            if (old[i])
                pset_add(&fset, &fcap, &fn, old[i]);
        if (held >= g_pn) {
            free(out);
            out = NULL;
        }
        for (size_t i = 0; out && i < g_pcap && have + nout < OCERZ_TC_PROF_MAX; i++)
            if (g_pset[i] && !pset_has(fset, fcap, g_pset[i]))
                out[nout++] = g_pset[i];
        if (nout)
            (void)!pwrite(fd, out, nout * 8, (off_t)(have * 8));
        if (g_log > 0)
            fprintf(stderr, "ocerz: TCACHE[%d] profile %s: %zu held, %zu added\n", (int)getpid(), g_prips, have, nout);
        free(old);
        free(fset);
        free(out);
        g_pwritten = g_pn;
    }
    if (fd >= 0)
        close(fd);
    os_unfair_lock_unlock(&g_plock);
}

static void profile_atexit(void)
{
    profile_write();
}

void ocerz_tcache_profile_open(uint64_t ident, const uint8_t *cache_uuid)
{
    if (!ident || !cache_uuid || profile_off())
        return;
    if (g_log < 0)
        g_log = getenv("OCERZ_TCACHE_LOG") ? 1 : 0;
    os_unfair_lock_lock(&g_plock);
    int first = !g_prips[0];
    profile_rips_name(g_prips, sizeof g_prips, ident, cache_uuid);
    g_pident = ident;
    os_unfair_lock_unlock(&g_plock);
    if (first && g_prips[0])
        atexit(profile_atexit);
}

void ocerz_tcache_profile_note(uint64_t v)
{
    if (!g_prips[0])
        return;
    os_unfair_lock_lock(&g_plock);
    if (g_pn < OCERZ_TC_PROF_MAX)
        pset_add(&g_pset, &g_pcap, &g_pn, v);
    os_unfair_lock_unlock(&g_plock);
}

void ocerz_tcache_profile_exec(const char *path)
{
    char name[1200], dir[1200];
    if (!g_pident || !path || !path[0] || strchr(path, '\n'))
        return;
    profile_execs_name(name, sizeof name, g_pident);
    if (!name[0] || !profile_path(dir, sizeof dir, NULL) || mkdirs(dir) != 0)
        return;
    int fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    struct stat st;
    if (flock(fd, LOCK_EX) == 0 && fstat(fd, &st) == 0 && st.st_size < (64 << 10)) {
        size_t have = (size_t)st.st_size, pl = strlen(path);
        char *buf = (char *)malloc(have + 2), *line = NULL;
        if (buf && pread(fd, buf + 1, have, 0) == (ssize_t)have) {
            buf[0] = '\n';
            buf[have + 1] = 0;
            for (char *p = strstr(buf, path); p && !line; p = strstr(p + 1, path))
                if (p[-1] == '\n' && p[pl] == '\n')
                    line = p;
            if (!line) {
                char *rec = (char *)malloc(pl + 1);
                if (rec) {
                    memcpy(rec, path, pl);
                    rec[pl] = '\n';
                    (void)!pwrite(fd, rec, pl + 1, (off_t)have);
                    free(rec);
                }
            }
        }
        free(buf);
    }
    close(fd);
}

size_t ocerz_tcache_profile_read(uint64_t ident, const uint8_t *cache_uuid, uint64_t **out)
{
    char name[1200];
    *out = NULL;
    if (!ident || !cache_uuid || profile_off())
        return 0;
    profile_rips_name(name, sizeof name, ident, cache_uuid);
    int fd = name[0] ? open(name, O_RDONLY | O_CLOEXEC) : -1;
    struct stat st;
    size_t n = 0;
    if (fd >= 0 && flock(fd, LOCK_SH) == 0 && fstat(fd, &st) == 0 && st.st_size >= 8) {
        n = (size_t)st.st_size / 8;
        if (n > OCERZ_TC_PROF_MAX)
            n = OCERZ_TC_PROF_MAX;
        *out = (uint64_t *)malloc(n * 8);
        if (!*out || pread(fd, *out, n * 8, 0) != (ssize_t)(n * 8)) {
            free(*out);
            *out = NULL;
            n = 0;
        }
    }
    if (fd >= 0)
        close(fd);
    return n;
}

char *ocerz_tcache_profile_execs(uint64_t ident)
{
    char name[1200];
    if (!ident || profile_off())
        return NULL;
    profile_execs_name(name, sizeof name, ident);
    int fd = name[0] ? open(name, O_RDONLY | O_CLOEXEC) : -1;
    struct stat st;
    char *buf = NULL;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (64 << 10) &&
        (buf = (char *)malloc((size_t)st.st_size + 1)) != NULL) {
        ssize_t got = pread(fd, buf, (size_t)st.st_size, 0);
        buf[got > 0 ? got : 0] = 0;
    }
    if (fd >= 0)
        close(fd);
    return buf;
}

static void flush_atexit(void)
{
    ocerz_tcache_flush();
}

void ocerz_tcache_put(const OcerzTcRecHead *rec)
{
    if (!rec || rec->size > TC_REC_MAX || rec->size % 8 || rec->size < sizeof *rec)
        return;
    pthread_mutex_lock(&g_lock);
    if (!open_store() || g_full) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!g_zbuf) {
        size_t zs = compression_encode_scratch_buffer_size(COMPRESSION_LZ4_RAW);
        g_zbuf = (uint8_t *)malloc(TC_REC_MAX);
        g_obuf = (uint8_t *)malloc(TC_OUT_BYTES);
        g_zscratch = (uint8_t *)malloc(zs ? zs : 1);
        if (!g_zbuf || !g_obuf || !g_zscratch) {
            free(g_zbuf);
            g_zbuf = NULL;
            pthread_mutex_unlock(&g_lock);
            return;
        }
        atexit(flush_atexit);
    }
    if (g_buf && g_buf_n + rec->size > TC_BUF_BYTES)
        hand_off_locked();
    if (!g_buf)
        g_buf = take_buffer();
    if (g_buf) {
        memcpy(g_buf + g_buf_n, rec, rec->size);
        g_buf_n += rec->size;
    }
    pthread_mutex_unlock(&g_lock);
}

void ocerz_tcache_flush(void)
{
    profile_write();
    if (!g_zbuf)
        return;
    pthread_mutex_lock(&g_lock);
    hand_off_locked();
    pthread_mutex_unlock(&g_lock);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&g_qlock);
    while ((g_qn || g_busy) &&
           pthread_cond_timedwait(&g_qdone, &g_qlock, &until) == 0)
        ;
    pthread_mutex_unlock(&g_qlock);
}

void ocerz_tcache_child(void)
{
    pthread_mutex_init(&g_lock, NULL);
    pthread_mutex_init(&g_qlock, NULL);
    pthread_cond_init(&g_qwork, NULL);
    pthread_cond_init(&g_qdone, NULL);
    g_plock = OS_UNFAIR_LOCK_INIT;
    g_writer = 0;
    g_qn = 0;
    g_busy = 0;
    if (g_dfd >= 0)
        close(g_dfd);
    g_dfd = -1;
    g_buf_n = 0;
}
