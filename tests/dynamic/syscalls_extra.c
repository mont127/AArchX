#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <unistd.h>

static int fails;

static void bad(const char *what)
{
    printf("BAD %s errno=%d\n", what, errno);
    fails++;
}

static void no_enosys(const char *what, long r)
{
    if (r == -1 && errno == ENOSYS)
        bad(what);
}

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
}

static void *chdir_thread(void *arg)
{
    char cwd[4096], want[4096];
    if (syscall(348, (const char *)arg) != 0 || !getcwd(cwd, sizeof cwd) || !realpath(arg, want))
        return (void *)1;
    return strcmp(cwd, want) == 0 ? NULL : (void *)2;
}

static void check_aio(const char *path)
{
    static const char msg[] = "async-io";
    char back[16] = "";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    struct aiocb cb;
    const struct aiocb *list[1] = { &cb };
    memset(&cb, 0, sizeof cb);
    cb.aio_fildes = fd;
    cb.aio_buf = (void *)msg;
    cb.aio_nbytes = sizeof msg - 1;
    if (fd < 0 || aio_write(&cb) != 0) {
        bad("aio_write");
    } else {
        while (aio_error(&cb) == EINPROGRESS)
            aio_suspend(list, 1, NULL);
        if (aio_return(&cb) != (ssize_t)(sizeof msg - 1))
            bad("aio_return write");
        memset(&cb, 0, sizeof cb);
        cb.aio_fildes = fd;
        cb.aio_buf = back;
        cb.aio_nbytes = sizeof msg - 1;
        if (aio_read(&cb) != 0) {
            bad("aio_read");
        } else {
            while (aio_error(&cb) == EINPROGRESS)
                aio_suspend(list, 1, NULL);
            if (aio_return(&cb) != (ssize_t)(sizeof msg - 1) || memcmp(back, msg, sizeof msg - 1) != 0)
                bad("aio_read data");
        }
    }
    if (fd >= 0)
        close(fd);
}

static void check_msg_nocancel(void)
{
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (q < 0) {
        bad("msgget");
        return;
    }
    struct {
        long mtype;
        char mtext[16];
    } m = { 7, "ocerz" }, r;
    memset(&r, 0, sizeof r);
    if (syscall(418, q, &m, sizeof m.mtext, 0) != 0)
        bad("msgsnd_nocancel");
    if (syscall(419, q, &r, sizeof r.mtext, 0L, 0) != (long)sizeof r.mtext || r.mtype != 7 ||
        strcmp(r.mtext, "ocerz") != 0)
        bad("msgrcv_nocancel");
    msgctl(q, IPC_RMID, NULL);
}

int main(void)
{
    char dir[] = "/tmp/ocerz_sysc.XXXXXX";
    if (!mkdtemp(dir)) {
        printf("BAD mkdtemp\n");
        return 1;
    }
    char a[256], b[256], c[256], d[256], e[256], lnk[256], sub[256], buf[4096];
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    snprintf(c, sizeof c, "%s/c", dir);
    snprintf(d, sizeof d, "%s/d", dir);
    snprintf(e, sizeof e, "%s/e", dir);
    snprintf(lnk, sizeof lnk, "%s/link", dir);
    snprintf(sub, sizeof sub, "%s/sub", dir);
    if (write_file(a, "alpha") || write_file(b, "beta"))
        bad("setup");

    struct stat st, st2;
    if (fchmodat(AT_FDCWD, a, 0640, 0) != 0 || stat(a, &st) != 0 || (st.st_mode & 0777) != 0640)
        bad("fchmodat");
    if (fchownat(AT_FDCWD, a, getuid(), getgid(), 0) != 0)
        bad("fchownat");
    if (renameatx_np(AT_FDCWD, a, AT_FDCWD, b, RENAME_SWAP) != 0 || read_file(a, buf, sizeof buf) ||
        strcmp(buf, "beta") != 0)
        bad("renameatx_np swap");
    if (renameatx_np(AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCL) == 0 || errno != EEXIST)
        bad("renameatx_np excl");
    if (renameat(AT_FDCWD, a, AT_FDCWD, c) != 0 || access(c, F_OK) != 0)
        bad("renameat");
    if (clonefileat(AT_FDCWD, b, AT_FDCWD, d, 0) != 0 || read_file(d, buf, sizeof buf) || strcmp(buf, "alpha") != 0)
        bad("clonefileat");
    int fd = open(c, O_RDONLY);
    if (fd < 0 || fclonefileat(fd, AT_FDCWD, e, 0) != 0 || read_file(e, buf, sizeof buf) || strcmp(buf, "beta") != 0)
        bad("fclonefileat");
    if (fd >= 0)
        close(fd);

    if (symlink("target-of-link", lnk) != 0)
        bad("symlink");
    fd = open(lnk, O_RDONLY | O_SYMLINK);
    long n = fd >= 0 ? syscall(551, fd, buf, sizeof buf) : -1;
    if (n != 14 || memcmp(buf, "target-of-link", 14) != 0)
        bad("freadlink");
    if (fd >= 0)
        close(fd);

    if (syscall(279, c, &st2, NULL, NULL) != 0)
        bad("stat_extended");
    if (syscall(292, sub, (uid_t)-1, (gid_t)-1, 0700, NULL) != 0 || stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
        bad("mkdir_extended");

    struct statfs sfs;
    char tail[256];
    const char *leaf = strrchr(dir, '/');
    snprintf(tail, sizeof tail, "%s/c", leaf ? leaf : dir);
    memset(buf, 0, sizeof buf);
    if (statfs(c, &sfs) != 0 || stat(c, &st) != 0 ||
        syscall(217, buf, sizeof buf, &sfs.f_fsid, (uint64_t)st.st_ino, 0) <= 0 ||
        strlen(buf) < strlen(tail) || strcmp(buf + strlen(buf) - strlen(tail), tail) != 0)
        bad("fsgetpath_ext");

    char p1[PROC_PIDPATHINFO_MAXSIZE] = "", p2[PROC_PIDPATHINFO_MAXSIZE] = "";
    proc_pidpath(getpid(), p1, sizeof p1);
    if (syscall(545, 2, getpid(), 11, 0, (uint64_t)0, (uint64_t)0, p2, (int)sizeof p2) < 0 || p1[0] == 0 ||
        strcmp(p1, p2) != 0)
        bad("proc_info_extended_id");

    check_aio(e);
    check_msg_nocancel();

    char before[4096], after[4096];
    pthread_t t;
    void *tr = (void *)9;
    if (!getcwd(before, sizeof before) || pthread_create(&t, NULL, chdir_thread, dir) != 0 ||
        pthread_join(t, &tr) != 0 || tr != NULL)
        bad("pthread_chdir");
    if (!getcwd(after, sizeof after) || strcmp(before, after) != 0)
        bad("pthread_chdir leaked to the process");

    struct ntptimeval ntv;
    if (ntp_gettime(&ntv) < 0)
        bad("ntp_gettime");
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    if (ntp_adjtime(&tx) < 0)
        bad("ntp_adjtime");

    no_enosys("thread_selfusage", syscall(482));
    no_enosys("kdebug_trace64", syscall(179, 0, 0, 0, 0, 0));
    int level = 0;
    no_enosys("memorystatus_get_level", syscall(453, &level));
    long s = syscall(450, AF_INET, SOCK_DGRAM, 0, getpid());
    no_enosys("socket_delegate", s);
    if (s >= 0)
        close((int)s);
    char label[64] = "sandbox";
    struct {
        size_t len;
        char *str;
    } mac = { sizeof label, label };
    no_enosys("mac_get_proc", syscall(386, &mac));
    no_enosys("revoke", syscall(56, c));
    no_enosys("fsctl", syscall(242, c, 0UL, NULL, 0));
    long o = syscall(218, AT_FDCWD, c, O_RDONLY, 0, 0, 0, -1);
    no_enosys("openat_dprotected_np", o);
    if (o >= 0)
        close((int)o);
    int ngroups = 0;
    no_enosys("getsgroups", syscall(288, &ngroups, NULL));
    no_enosys("exchangedata", exchangedata(c, d, 0));

    unlink(a);
    unlink(b);
    unlink(c);
    unlink(d);
    unlink(e);
    unlink(lnk);
    rmdir(sub);
    rmdir(dir);
    if (!fails)
        printf("OK\n");
    return fails != 0;
}
