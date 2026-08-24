#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    if (signal(SIGUSR1, SIG_IGN) == SIG_ERR)
        return 1;

    pid_t pid = fork();
    if (pid < 0)
        return 2;
    if (pid == 0) {
        int rc = pthread_kill(pthread_self(), SIGUSR1);
        _exit(rc == 0 ? 0 : 3);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited != pid)
        return 4;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 5;

    static const char marker[] = "fork signal ok\n";
    return write(STDOUT_FILENO, marker, sizeof(marker) - 1) == sizeof(marker) - 1
               ? 0 : 6;
}
