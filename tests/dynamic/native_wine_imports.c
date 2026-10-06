/*
 * Calls MacNdCheese's Wine makes that native mode had no answer for, or the
 * wrong one.  Each line is the same on the host and under ocerz:
 *
 * - __ulock_wait2, which msync waits in, ends with EINTR when a signal arrives
 *   and the handler has run by the time it returns.
 * - readv, writev, preadv, pwritev, sendmsg and recvmsg, whose iovec and
 *   msghdr hold pointers (kernel32 failed on writev's EFAULT).
 * - __res_9_state, the resolver state dnsapi.so reads in place.
 * - malloc_zone_statistics, which winemetal.so's statically linked LLVM calls.
 * - SCDynamicStoreCopyDHCPInfo and DHCPInfoGetOptionData, from mountmgr.so.
 * - _dyld_get_image_uuid, which D3DMetal asks about its own image.
 * - CoreAnalytics, the private framework D3DMetal links, opens and binds its
 *   two event calls (looked up, not called: they would send events).
 */
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <resolv.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <uuid/uuid.h>

#define UL_COMPARE_AND_WAIT 1
#define ULF_NO_ERRNO 0x01000000
extern bool _dyld_get_image_uuid(const struct mach_header *mh, uuid_t uuid);
extern int __ulock_wait2(uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2);

static _Atomic int g_handled;
static uint32_t g_word;
static _Atomic int g_waiting;

static void on_usr1(int sig)
{
    (void)sig;
    atomic_store(&g_handled, 1);
}

static void *waiter(void *arg)
{
    atomic_store(&g_waiting, 1);
    int r = __ulock_wait2(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &g_word, 0, 0, 0);
    *(int *)arg = r;
    return NULL;
}

static void ulock_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    int r = 1;
    pthread_t t;
    pthread_create(&t, NULL, waiter, &r);
    while (!atomic_load(&g_waiting))
        usleep(1000);
    usleep(50000);
    pthread_kill(t, SIGUSR1);
    pthread_join(t, NULL);
    printf("__ulock_wait2 interrupted: %s, handler ran: %d\n", r == -EINTR ? "EINTR" : "other",
           atomic_load(&g_handled));
}

static void iovecs(void)
{
    char path[] = "/tmp/ocerz-wine-imports.XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    char a[] = "vect", b[] = "ored";
    struct iovec out[2] = { { a, 4 }, { b, 4 } };
    ssize_t w = writev(fd, out, 2);
    ssize_t pw = pwritev(fd, out, 2, 8);
    char x[5] = { 0 }, y[5] = { 0 };
    struct iovec in[2] = { { x, 4 }, { y, 4 } };
    lseek(fd, 0, SEEK_SET);
    ssize_t rd = readv(fd, in, 2);
    int first = strcmp(x, "vect") == 0 && strcmp(y, "ored") == 0;
    memset(x, 0, sizeof x);
    memset(y, 0, sizeof y);
    ssize_t prd = preadv(fd, in, 2, 8);
    int second = strcmp(x, "vect") == 0 && strcmp(y, "ored") == 0;
    close(fd);
    printf("writev %zd pwritev %zd readv %zd %d preadv %zd %d\n", w, pw, rd, first, prd, second);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = out;
    m.msg_iovlen = 2;
    ssize_t sent = sendmsg(sv[0], &m, 0);
    char buf[16] = { 0 };
    struct iovec rin = { buf, sizeof buf - 1 };
    struct msghdr r;
    memset(&r, 0, sizeof r);
    r.msg_iov = &rin;
    r.msg_iovlen = 1;
    ssize_t got = recvmsg(sv[1], &r, 0);
    close(sv[0]);
    close(sv[1]);
    printf("sendmsg %zd recvmsg %zd '%s' flags %d\n", sent, got, buf, r.msg_flags);
}

static void image_uuid(void)
{
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    uuid_t got;
    memset(got, 0, sizeof got);
    int ok = _dyld_get_image_uuid((const struct mach_header *)mh, got);
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    int same = 0;
    for (uint32_t k = 0; k < mh->ncmds; k++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_UUID) {
            same = memcmp(((const struct uuid_command *)c)->uuid, got, sizeof got) == 0;
            break;
        }
        lc += c->cmdsize;
    }
    printf("main image uuid answered: %d, matches its LC_UUID: %d\n", ok, same);
}

int main(void)
{
    ulock_signal();
    iovecs();

    res_init();
    printf("resolver initialized: %d, servers: %d\n", (_res.options & RES_INIT) != 0, _res.nscount > 0);

    malloc_statistics_t all, def;
    memset(&all, 0xff, sizeof all);
    memset(&def, 0xff, sizeof def);
    malloc_zone_statistics(NULL, &all);
    malloc_zone_statistics(malloc_default_zone(), &def);
    printf("malloc statistics: all zones %d, default zone %d\n",
           all.blocks_in_use > 0 && all.size_in_use > 0 && all.size_in_use <= all.size_allocated,
           def.blocks_in_use > 0 && def.size_in_use <= def.size_allocated);

    CFDictionaryRef dhcp = SCDynamicStoreCopyDHCPInfo(NULL, NULL);
    CFDataRef mask = dhcp ? DHCPInfoGetOptionData(dhcp, 1) : NULL;
    printf("DHCP info: %d, subnet mask option: %d\n", dhcp != NULL, mask ? (int)CFDataGetLength(mask) : -1);
    if (dhcp)
        CFRelease(dhcp);
    image_uuid();
    void *ca = dlopen("/System/Library/PrivateFrameworks/CoreAnalytics.framework/CoreAnalytics", RTLD_LAZY);
    printf("CoreAnalytics opened: %d, event calls bound: %d\n", ca != NULL,
           ca && dlsym(ca, "AnalyticsSendEvent") && dlsym(ca, "AnalyticsSendEventLazy"));
    return 0;
}
