/* Kernel copyouts into a page that holds executed code.  The JIT write-traps
 * such pages to catch self-modifying code (ocerz_mem_arm_exec); a kernel
 * copy into a read-only page does not fault, it fails (EFAULT, or a mach
 * reply destroyed - wineboot hung on that).  read/readv/pread and a mach
 * receive must land in a buffer next to running code, and the code must
 * still be rewritable afterwards.  Prints OK. */
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

static void put_code(uint8_t *p, uint32_t imm)
{
    p[0] = 0xb8; memcpy(p + 1, &imm, 4);   /* mov eax, imm32 */
    p[5] = 0xc3;                           /* ret */
}

int main(void)
{
    uint8_t *page = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    put_code(page, 17);
    int (*fn)(void) = (int (*)(void))page;
    int a = fn();

    /* read(2) into the code page */
    int pfd[2]; pipe(pfd);
    write(pfd[1], "PIPEDATA", 8);
    char *buf1 = (char *)page + 0x800;
    memset(buf1, 0, 16);
    ssize_t n1 = read(pfd[0], buf1, 8);

    /* readv(2) */
    write(pfd[1], "ABCDEFGH", 8);
    char *buf2 = (char *)page + 0x900;
    memset(buf2, 0, 16);
    struct iovec iov[2] = { { buf2, 4 }, { buf2 + 4, 4 } };
    ssize_t n2 = readv(pfd[0], iov, 2);

    /* pread(2) from a file */
    char path[64]; snprintf(path, sizeof path, "/tmp/ocerz_smc_io_%d", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    write(fd, "xxxxFILEDATA", 12);
    char *buf3 = (char *)page + 0xa00;
    memset(buf3, 0, 16);
    ssize_t n3 = pread(fd, buf3, 8, 4);
    close(fd); unlink(path);

    /* mach receive into the code page */
    mach_port_t port = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    struct { mach_msg_header_t h; uint64_t body; } snd;
    memset(&snd, 0, sizeof snd);
    snd.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    snd.h.msgh_size = sizeof snd;
    snd.h.msgh_remote_port = port;
    snd.h.msgh_id = 4242;
    kern_return_t ks = mach_msg(&snd.h, MACH_SEND_MSG, sizeof snd, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    mach_msg_header_t *rcv = (mach_msg_header_t *)(page + 0xc00);
    memset(rcv, 0, 128);
    kern_return_t kr = mach_msg(rcv, MACH_RCV_MSG, 0, 128, port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    int id = kr == KERN_SUCCESS ? (int)rcv->msgh_id : -(int)kr;

    /* the code still runs, and can still be rewritten in place */
    int b = fn();
    put_code(page, 34);
    int c = fn();

    buf1[8] = 0; buf2[8] = 0; buf3[8] = 0;
    int ok = a == 17 && n1 == 8 && strcmp(buf1, "PIPEDATA") == 0 && n2 == 8 &&
             strcmp(buf2, "ABCDEFGH") == 0 && n3 == 8 && strcmp(buf3, "FILEDATA") == 0 &&
             ks == KERN_SUCCESS && id == 4242 && b == 17 && c == 34;
    if (!ok)
        printf("a=%d n1=%zd buf1=%s n2=%zd buf2=%s n3=%zd buf3=%s ks=%d id=%d b=%d c=%d\n",
               a, n1, buf1, n2, buf2, n3, buf3, (int)ks, id, b, c);
    printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
