/* statfs and fstatfs from i386 code, in both layouts: the legacy 272-byte one (_statfs, _fstatfs), which Left 4 Dead
 * 2's Chromium asks for at map start, and $INODE64's fixed-size one. Both describe the volume holding /, so the
 * file system name, mount point and block size must agree. Expected output written by hand. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct statfs32 {
    short f_otype, f_oflags;
    int32_t f_bsize, f_iosize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint32_t f_owner;
    short f_reserved1, f_type;
    int32_t f_flags, f_reserved2[2];
    char f_fstypename[15], f_mntonname[90], f_mntfromname[90], f_reserved3;
    int32_t f_reserved4[4];
};
struct statfs64 {
    uint32_t f_bsize;
    int32_t f_iosize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint32_t f_owner, f_type, f_flags, f_fssubtype;
    char f_fstypename[16], f_mntonname[1024], f_mntfromname[1024];
    uint32_t f_flags_ext, f_reserved[7];
};

int statfs32(const char *, struct statfs32 *) __asm("_statfs");
int fstatfs32(int, struct statfs32 *) __asm("_fstatfs");
int statfs64(const char *, struct statfs64 *) __asm("_statfs$INODE64");
int fstatfs64(int, struct statfs64 *) __asm("_fstatfs$INODE64");

int main(void)
{
    struct statfs32 a, b;
    struct statfs64 c, d;
    memset(&a, 0xee, sizeof a);
    memset(&b, 0xee, sizeof b);
    int fd = open("/", O_RDONLY);
    int r = statfs32("/", &a) | fstatfs32(fd, &b) | statfs64("/", &c) | fstatfs64(fd, &d);
    close(fd);
    printf("sizes %u %u\n", (unsigned)sizeof a, (unsigned)sizeof c);
    printf("results %d\n", r);
    printf("legacy %s %s type-shadow %d\n", a.f_fstypename, a.f_mntonname, a.f_otype == a.f_type);
    printf("inode64 %s %s\n", c.f_fstypename, c.f_mntonname);
    printf("agree %d %d %d\n", a.f_bsize == (int32_t)c.f_bsize && a.f_bsize > 0, !strcmp(a.f_fstypename, b.f_fstypename),
           !strcmp(c.f_mntonname, d.f_mntonname) && a.f_fsid[0] == c.f_fsid[0]);
    printf("missing %d\n", statfs32("/nonexistent-m32", &a));
    return 0;
}
