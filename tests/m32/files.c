#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
int main(void) {
    const char *p = "/tmp/m32-files-test.txt";
    FILE *f = fopen(p, "w"); fputs("line one\n", f); fwrite("line two\n", 1, 9, f); fclose(f);
    struct stat st; printf("stat %d size %lld reg %d\n", stat(p, &st), (long long)st.st_size, S_ISREG(st.st_mode));
    char buf[64] = {0}; f = fopen(p, "r"); fgets(buf, sizeof buf, f); printf("fgets %s", buf);
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; printf("fread %zu %s", n, buf); printf("eof %d\n", feof(f) != 0); fclose(f);
    int fd = open(p, O_RDONLY); printf("lseek %lld\n", (long long)lseek(fd, 0, SEEK_END));
    struct stat fst; printf("fstat %d %lld\n", fstat(fd, &fst), (long long)fst.st_size); close(fd);
    int bad = open("/nonexistent/x", O_RDONLY), err = errno; printf("missing %d errno %d %s\n", bad, err, strerror(err));
    DIR *d = opendir("/tmp"); int seen = 0; struct dirent *e;
    while ((e = readdir(d))) if (!strcmp(e->d_name, "m32-files-test.txt")) seen = e->d_type == DT_REG ? 1 : 2;
    closedir(d); printf("readdir %d\n", seen);
    unlink(p); printf("gone %d\n", access(p, F_OK));
    time_t t = time(NULL); struct timeval tv; gettimeofday(&tv, NULL); printf("time-sane %d\n", t > 1700000000 && tv.tv_sec >= t - 1);
    struct tm tm; time_t fixed = 86400 * 365; gmtime_r(&fixed, &tm); char out[64]; strftime(out, sizeof out, "%Y-%m-%d %H:%M", &tm); puts(out);
    printf("mktime-roundtrip %d\n", timegm(&tm) == fixed);
    struct timespec ts = { 0, 1000000 }; printf("nanosleep %d\n", nanosleep(&ts, NULL));
    printf("getenv %s\n", getenv("M32_TEST_ENV") ? "set" : "unset");
    fprintf(stderr, "stderr ok\n");
    return 0;
}
