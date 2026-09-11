/* SysV semaphores: semget/semop/semctl.  Steam's tier0 threading is built on
 * them; unimplemented, every "thread synchronization object is unuseable". */
#include <errno.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>

int main(void)
{
    int id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    if (id < 0) {
        printf("BAD semget\n");
        return 1;
    }
    union semun u;
    unsigned short init[2] = { 1, 5 };
    u.array = init;
    if (semctl(id, 0, SETALL, u) < 0) {
        printf("BAD SETALL\n");
        return 2;
    }
    struct sembuf op = { 0, -1, 0 };      /* take sem 0 (1 -> 0) */
    if (semop(id, &op, 1) < 0) {
        printf("BAD semop\n");
        return 3;
    }
    if (semctl(id, 0, GETVAL) != 0 || semctl(id, 1, GETVAL) != 5) {
        printf("BAD values %d %d\n", semctl(id, 0, GETVAL), semctl(id, 1, GETVAL));
        semctl(id, 0, IPC_RMID);
        return 4;
    }
    unsigned short got[2] = { 0, 0 };
    u.array = got;
    if (semctl(id, 0, GETALL, u) < 0 || got[0] != 0 || got[1] != 5) {
        printf("BAD GETALL %u %u\n", got[0], got[1]);
        semctl(id, 0, IPC_RMID);
        return 5;
    }
    struct semid_ds ds;
    u.buf = &ds;
    if (semctl(id, 0, IPC_STAT, u) < 0 || ds.sem_nsems != 2) {
        printf("BAD IPC_STAT nsems=%u\n", (unsigned)ds.sem_nsems);
        semctl(id, 0, IPC_RMID);
        return 6;
    }
    semctl(id, 0, IPC_RMID);
    printf("OK\n");
    return 0;
}
