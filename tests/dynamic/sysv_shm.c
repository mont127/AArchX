/*
 * System V shared memory really shared, not privately copied.
 *
 * shmat is the one mapping call whose address the guest cannot choose for
 * itself: the host kernel refuses a fixed address, even over ground the arena
 * has already reserved, so the segment lands wherever the host puts it and is
 * carried into guest space afterwards.  Carrying it with a copy instead of a
 * share would pass every single-process check and still be wrong, because the
 * point of the segment is that another process writes into it - Steam's client
 * talks to its games this way, and Brawlhalla was the game that noticed
 * syscall 262 was not there at all.
 *
 * So the test spends a child process.  The parent writes through its
 * attachment, the child attaches the same id by itself and writes a second
 * word, and the parent then reads both back: the child's word can only appear
 * if the two attachments name the same pages.  A second attachment in the
 * parent checks the same thing within one address space, where a copy would
 * also show up as a stale read.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>

#define SEG_SIZE 0x4000

int main(void)
{
    int id = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
    if (id < 0) {
        printf("sysv_shm bad: shmget: %s\n", strerror(errno));
        return 1;
    }

    int bad = 0;
    volatile unsigned *a = shmat(id, NULL, 0);
    if (a == (void *)-1) {
        printf("sysv_shm bad: shmat: %s\n", strerror(errno));
        shmctl(id, IPC_RMID, NULL);
        return 1;
    }
    a[0] = 0x11111111u;
    a[1] = 0;
    a[2] = 0;

    volatile unsigned *b = shmat(id, NULL, 0);
    if (b == (void *)-1) {
        printf("sysv_shm bad: second shmat: %s\n", strerror(errno));
        bad = 1;
    } else {
        if (b == a) {
            printf("sysv_shm bad: two attachments share one address %p\n", (void *)b);
            bad = 1;
        }
        if (b[0] != 0x11111111u) {
            printf("sysv_shm bad: alias reads %#x, not the write through the first\n", b[0]);
            bad = 1;
        }
        b[1] = 0x22222222u;
        if (a[1] != 0x22222222u) {
            printf("sysv_shm bad: first attachment reads %#x after the alias wrote\n", a[1]);
            bad = 1;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        volatile unsigned *c = shmat(id, NULL, 0);
        if (c == (void *)-1)
            _exit(2);
        if (c[0] != 0x11111111u)
            _exit(3);
        c[2] = 0x33333333u;
        shmdt((void *)c);
        _exit(0);
    }
    if (pid < 0) {
        printf("sysv_shm bad: fork: %s\n", strerror(errno));
        bad = 1;
    } else {
        int st = 0;
        if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("sysv_shm bad: child status %d\n", st);
            bad = 1;
        } else if (a[2] != 0x33333333u) {
            printf("sysv_shm bad: parent reads %#x where the child wrote\n", a[2]);
            bad = 1;
        }
    }

    struct shmid_ds ds;
    memset(&ds, 0, sizeof ds);
    if (shmctl(id, IPC_STAT, &ds) != 0) {
        printf("sysv_shm bad: shmctl: %s\n", strerror(errno));
        bad = 1;
    } else if ((size_t)ds.shm_segsz < (size_t)SEG_SIZE) {
        printf("sysv_shm bad: segment is %llu bytes\n", (unsigned long long)ds.shm_segsz);
        bad = 1;
    }

    if (b != (void *)-1 && shmdt((void *)b) != 0) {
        printf("sysv_shm bad: shmdt alias: %s\n", strerror(errno));
        bad = 1;
    }
    if (shmdt((void *)a) != 0) {
        printf("sysv_shm bad: shmdt: %s\n", strerror(errno));
        bad = 1;
    }
    if (shmdt((void *)a) == 0) {
        printf("sysv_shm bad: detaching twice succeeded\n");
        bad = 1;
    }
    shmctl(id, IPC_RMID, NULL);

    if (!bad)
        printf("OK\n");
    return bad;
}
