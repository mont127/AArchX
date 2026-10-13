/* A FILE's fields as i386 code reads them inline.  Valve's file system (Left 4 Dead 2) flocks fp->_file of every file
 * it opens for writing and gives up when that fails, after fopen has truncated it: settings and config.cfg came out
 * empty while _file read 0.  _file must be the stream's descriptor, also for a FILE libc reuses after fclose and for
 * fdopen and tmpfile.  Expected output written by hand. */
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

FILE *fopen_plain(const char *, const char *) __asm("_fopen");

static int same(FILE *f) { return f && *(short *)((char *)f + 14) == fileno(f); }

int main(void)
{
    const char *p = "/tmp/m32-filefd.txt";
    FILE *a = fopen_plain(p, "w"), *b = fopen("/dev/null", "r");
    int locked = flock(*(short *)((char *)a + 14), LOCK_EX | LOCK_NB) == 0;
    fputs("\"config\"\n{\n}\n", a);
    printf("fopen %d %d flock %d\n", same(a), same(b), locked);
    fclose(a);
    struct stat st;
    stat(p, &st);
    printf("written %lld\n", (long long)st.st_size);
    FILE *c = fopen(p, "r");   /* libc hands out a's FILE again */
    printf("reused %d\n", same(c));
    FILE *d = fdopen(dup(fileno(b)), "r"), *t = tmpfile();
    printf("fdopen %d tmpfile %d\n", same(d), same(t));
    fclose(b), fclose(c), fclose(d), fclose(t);
    unlink(p);
    return 0;
}
