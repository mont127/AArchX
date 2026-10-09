#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    int id = shmget(IPC_PRIVATE, sizeof(uint32_t), IPC_CREAT | 0600);
    if (id < 0) {
        puts("bad");
        return 1;
    }

    volatile uint32_t *value = shmat(id, NULL, 0);
    if (value == (void *)-1) {
        shmctl(id, IPC_RMID, NULL);
        puts("bad");
        return 1;
    }
    *value = 0x11223344;

    pid_t pid = fork();
    if (pid == 0) {
        *value = 0x55667788;
        _exit(0);
    }

    int status = 0;
    pid_t waited = -1;
    if (pid > 0) {
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    int ok = pid > 0 && waited == pid && WIFEXITED(status)
        && WEXITSTATUS(status) == 0 && *value == 0x55667788;
    if (shmdt((void *)value) != 0)
        ok = 0;
    if (shmctl(id, IPC_RMID, NULL) != 0)
        ok = 0;

    puts(ok ? "ok" : "bad");
    return ok ? 0 : 1;
}
