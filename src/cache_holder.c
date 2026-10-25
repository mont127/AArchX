/*
 * The cache holder.  Mapping the x86_64 shared cache's files costs a launch 0.05 ms while some other process has
 * them mapped and 1.5-4 ms once none has (a busy Mac: the kernel builds each file's memory object anew), where
 * Rosetta's shared region costs a launch nothing: uname -m 11.6 ms against 8.7, perl -e 1 15.3 against 12.0.  So a
 * cache-mode launch sees that a holder runs: `ocerz -hold-cache`, a process that maps every file of the cache
 * read-only, touches none of their pages, and exits once no launch has come for ten minutes or the cache's file
 * is not the one it holds any more.  It holds the translation cache's store too, its index and its data files,
 * whose 64 MB index alone costs a launch 0.25-0.4 ms to map when nothing else has it: every 30 seconds it maps
 * the files that came since and lets go of those deleted or replaced, so a store emptied for space is not kept
 * on the disk.  It keeps a lock on a file in the user's temporary directory named for the cache's UUID and the
 * store's path, which a launch tries: held means a holder runs, and the launch only sets the file's time, which
 * is how the holder knows launches still come; free means none does, and the launch starts one.  The holder is
 * never the launch's child, or a guest waiting for all its children would wait ten minutes for it: the launch
 * spawns a first process, which forks the holder off in a session of its own and exits, and reaps that one at
 * once.  OCERZ_NO_CACHE_HOLDER=1 starts none (the tests set it), OCERZ_CACHE_HOLDER_IDLE=<seconds> is how long
 * a holder waits for a launch (600), OCERZ_CACHE_HOLDER_DIR=<dir> puts its lock there and not in the user's
 * temporary directory.
 */
#include "ocerz/cache.h"
#include "ocerz/tcache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HOLDER_STEM "dyld_shared_cache_x86_64"
#define HOLDER_MAX_FILES 16

static int holder_lock_path(const uint8_t *uuid, const char *store, char *out, size_t n)
{
    char tmp[PATH_MAX];
    const char *d = getenv("OCERZ_CACHE_HOLDER_DIR");
    size_t l = d && d[0] ? strlcpy(tmp, d, sizeof tmp) + 1 : confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
    if (l < 2 || l > sizeof tmp)
        return 0;
    int k = snprintf(out, n, "%s%socerz-cache-holder-", tmp, tmp[l - 2] == '/' ? "" : "/");
    for (int i = 0; i < 16 && k > 0 && (size_t)k + 3 < n; i++)
        k += snprintf(out + k, n - (size_t)k, "%02x", uuid[i]);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = store; *p; p++)
        h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    if (k > 0 && (size_t)k + 18 < n)
        k += snprintf(out + k, n - (size_t)k, "-%016llx", (unsigned long long)h);
    return k > 0 && (size_t)k < n;
}

void ocerz_cache_holder_note(const uint8_t *uuid, const char *dir)
{
    if (!uuid || !dir || getenv("OCERZ_NO_CACHE_HOLDER"))
        return;
    char lock[PATH_MAX], store[PATH_MAX];
    if (!ocerz_tcache_store_path(store, sizeof store))
        strlcpy(store, "-", sizeof store);
    if (!holder_lock_path(uuid, store, lock, sizeof lock))
        return;
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        futimes(fd, NULL);   /* a holder runs: a launch came */
        close(fd);
        return;
    }
    close(fd);
    char self[PATH_MAX];
    uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz) != 0)
        return;
    const char *ie = getenv("OCERZ_CACHE_HOLDER_IDLE");
    char idle[24];
    snprintf(idle, sizeof idle, "%d", ie && atoi(ie) > 0 ? atoi(ie) : 600);
    char *args[] = { self, (char *)"-hold-cache", lock, (char *)dir, idle, store, NULL };
    char *env[] = { NULL };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (posix_spawn_file_actions_init(&fa) != 0)
        return;
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return;
    }
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    if (posix_spawn(&pid, self, &fa, &at, args, env) == 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
}

/* A file the holder keeps mapped, known by its inode so that a replaced one is let go. */
typedef struct { dev_t dev; ino_t ino; void *p; size_t len; char name[24]; } HeldFile;
static HeldFile *g_held;
static size_t g_nheld, g_capheld;

static int held_map(const char *path, const char *name)
{
    int f = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (f < 0)
        return 0;
    int ok = 0;
    if (fstat(f, &st) == 0 && st.st_size > 0 && strlen(name) < sizeof g_held[0].name) {
        for (size_t i = 0; i < g_nheld; i++)
            if (g_held[i].dev == st.st_dev && g_held[i].ino == st.st_ino) {
                close(f);
                return 1;
            }
        if (g_nheld == g_capheld) {
            size_t cap = g_capheld ? 2 * g_capheld : 64;
            HeldFile *v = (HeldFile *)realloc(g_held, cap * sizeof *v);
            if (!v) {
                close(f);
                return 0;
            }
            g_held = v;
            g_capheld = cap;
        }
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, f, 0);
        if (p != MAP_FAILED) {
            HeldFile *h = &g_held[g_nheld++];
            h->dev = st.st_dev;
            h->ino = st.st_ino;
            h->p = p;
            h->len = (size_t)st.st_size;
            strlcpy(h->name, name, sizeof h->name);
            ok = 1;
        }
    }
    close(f);
    return ok;
}

/* The store's index and data files that came since the last look; those deleted or replaced let go. */
static void held_store(const char *store)
{
    char path[PATH_MAX];
    struct stat st;
    for (size_t i = 0; i < g_nheld;) {
        snprintf(path, sizeof path, "%s/%s", store, g_held[i].name);
        if (stat(path, &st) != 0 || st.st_ino != g_held[i].ino || st.st_dev != g_held[i].dev) {
            munmap(g_held[i].p, g_held[i].len);
            g_held[i] = g_held[--g_nheld];
        } else {
            i++;
        }
    }
    DIR *d = opendir(store);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (strcmp(e->d_name, "index") && !(l > 5 && !strncmp(e->d_name, "d-", 2) && !strcmp(e->d_name + l - 3, ".td")))
            continue;
        snprintf(path, sizeof path, "%s/%s", store, e->d_name);
        held_map(path, e->d_name);
    }
    if (d)
        closedir(d);
}

int ocerz_cache_holder_main(int argc, char **argv)
{
    if (argc < 5)
        return 64;
    const char *lock = argv[2], *dir = argv[3], *store = argc > 5 && strcmp(argv[5], "-") ? argv[5] : NULL;
    int idle = atoi(argv[4]);
    if (idle <= 0)
        idle = 600;
    pid_t pid = fork();
    if (pid != 0)
        _exit(pid < 0);
    setsid();
    setpriority(PRIO_PROCESS, 0, 10);
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0)
        _exit(0);
    int held = 0;
    struct stat first = { 0 };
    for (int i = 0; i < HOLDER_MAX_FILES; i++) {
        char path[PATH_MAX];
        if (i)
            snprintf(path, sizeof path, "%s%s.%02d", dir, HOLDER_STEM, i);
        else
            snprintf(path, sizeof path, "%s%s", dir, HOLDER_STEM);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (f < 0)
            break;
        if (fstat(f, &st) == 0 && st.st_size > 0 &&
            mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, f, 0) != MAP_FAILED) {
            if (!held)
                first = st;
            held++;
        }
        close(f);
    }
    if (!held)
        _exit(0);
    if (store)
        held_store(store);
    char line[32];
    int l = snprintf(line, sizeof line, "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0)
        (void)pwrite(fd, line, (size_t)l, 0);
    futimes(fd, NULL);
    for (;;) {
        sleep(idle < 60 ? (unsigned)(idle > 1 ? idle / 2 : 1) : 30u);
        struct stat st, now;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s%s", dir, HOLDER_STEM);
        if (fstat(fd, &st) != 0 || time(NULL) - st.st_mtime >= idle)
            _exit(0);
        if (stat(path, &now) != 0 || now.st_ino != first.st_ino || now.st_dev != first.st_dev)
            _exit(0);
        if (store)
            held_store(store);
    }
}
