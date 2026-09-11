/* fileport_makeport / fileport_makefd: an fd turned into a Mach port and
 * back, the way XPC hands files between processes.  Photos receives its
 * library files like this and asserted -- then aborted -- when
 * fileport_makefd came back ENOSYS. */
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int fileport_makeport(int fd, mach_port_t *port);
int fileport_makefd(mach_port_t port);

int main(void)
{
    int p[2];
    if (pipe(p) != 0)
        return 1;
    mach_port_t port = MACH_PORT_NULL;
    if (fileport_makeport(p[1], &port) != 0 || port == MACH_PORT_NULL) {
        printf("BAD fileport_makeport\n");
        return 2;
    }
    int fd = fileport_makefd(port);
    if (fd < 0) {
        printf("BAD fileport_makefd\n");
        return 3;
    }
    if (fd == p[1] || write(fd, "fp", 2) != 2) {
        printf("BAD write through the new fd %d\n", fd);
        return 4;
    }
    char buf[4] = { 0 };
    if (read(p[0], buf, 2) != 2 || memcmp(buf, "fp", 2) != 0) {
        printf("BAD read back '%s'\n", buf);
        return 5;
    }
    mach_port_deallocate(mach_task_self(), port);
    printf("OK\n");
    return 0;
}
