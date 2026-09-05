/* MAP_PRIVATE file mappings.  The emulator maps the 16 KB-aligned interior of
 * a private file mapping straight from the file when the file offset and the
 * guest address agree modulo 16 KB, and reads the rest in.  This pins what
 * the guest must not be able to tell: the bytes are the file's at every
 * offset (head, interior, tail, past EOF), a write is private to the mapping
 * and never reaches the file, an offset that does not line up still reads
 * right, a fixed mapping at a 4 KB address works, and unmapping one 4 KB page
 * leaves its neighbours intact.  Goldens come from the native binary. */
#include "gsys.h"

#define PROT_NONE 0
#define O_RDWR 0x0002
static inline g_i64 sys_pread(int fd, void *buf, g_u64 len, g_i64 off)
{
    return g_syscall6(0x2000000 | 153, fd, (g_i64)buf, (g_i64)len, off, 0, 0);
}

#define FILE_LEN (37 * 4096 + 1234)      /* 151 KB: interior pages, a partial tail */
#define PATH "/tmp/ocerz_filemap_private.bin"

static unsigned char expect_byte(g_u64 off) { return (unsigned char)((off * 7 + (off >> 12) * 13) & 0xff); }

static int check(const unsigned char *p, g_u64 off, g_u64 n)
{
    for (g_u64 i = 0; i < n; i++)
        if (p[i] != expect_byte(off + i)) return 0;
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    static unsigned char buf[4096];
    int fd = sys_open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { g_puts("open failed\n"); return 1; }
    for (g_u64 off = 0; off < FILE_LEN; ) {
        g_u64 n = FILE_LEN - off < 4096 ? FILE_LEN - off : 4096;
        for (g_u64 i = 0; i < n; i++) buf[i] = expect_byte(off + i);
        sys_write(fd, buf, n);
        off += n;
    }

    /* whole file, offset 0: interior pages come from the file itself */
    g_u64 maplen = (FILE_LEN + 0xfff) & ~(g_u64)0xfff;
    unsigned char *m = (unsigned char *)sys_mmap(0, maplen + 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    g_puts("whole "); g_putu64(check(m, 0, FILE_LEN));
    g_puts("past-eof-zero "); g_putu64(m[FILE_LEN] == 0 && m[maplen - 1] == 0);
    g_puts("beyond-file-zero "); g_putu64(m[maplen + 0x100] == 0);

    /* a private write stays private */
    m[0x5000] ^= 0xff; m[0x20010] ^= 0xff;
    unsigned char back[2];
    sys_pread(fd, back, 1, 0x5000); sys_pread(fd, back + 1, 1, 0x20010);
    g_puts("cow "); g_putu64(back[0] == expect_byte(0x5000) && back[1] == expect_byte(0x20010) &&
                             m[0x5000] == (unsigned char)(expect_byte(0x5000) ^ 0xff));

    /* unmap one 4 KB page in the middle: neighbours keep their bytes */
    sys_munmap(m + 0x10000, 0x1000);
    g_puts("neighbours "); g_putu64(check(m + 0xf000, 0xf000, 0x1000) && check(m + 0x11000, 0x11000, 0x1000));
    sys_munmap(m, maplen + 0x8000);

    /* offset 4 KB: does not line up with the address, must still read right */
    unsigned char *m2 = (unsigned char *)sys_mmap(0, 0x20000, PROT_READ, MAP_PRIVATE, fd, 0x1000);
    g_puts("offset4k "); g_putu64(check(m2, 0x1000, 0x20000));
    sys_munmap(m2, 0x20000);

    /* a fixed mapping at a 4 KB-aligned address with a matching 4 KB offset:
     * the head page is read in, the interior mapped */
    unsigned char *area = (unsigned char *)sys_mmap(0, 0x40000, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *m3 = (unsigned char *)sys_mmap(area + 0x1000, 0x20000, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0x1000);
    g_puts("fixed4k "); g_putu64(m3 == area + 0x1000 && check(m3, 0x1000, 0x20000));
    sys_munmap(area, 0x40000);

    /* offset 16 KB, mapping longer than the file: tail zero */
    unsigned char *m4 = (unsigned char *)sys_mmap(0, maplen, PROT_READ, MAP_PRIVATE, fd, 0x4000);
    g_puts("offset16k "); g_putu64(check(m4, 0x4000, FILE_LEN - 0x4000) && m4[FILE_LEN - 0x4000] == 0);
    sys_munmap(m4, maplen);

    sys_close(fd);
    sys_unlink(PATH);
    return 0;
}
