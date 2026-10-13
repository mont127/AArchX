/* scandir and alphasort from i386 code, in both dirent layouts: the legacy one (_scandir, 32-bit inodes), which Left 4
 * Dead 2's file system lists its sound folders with, and $INODE64. The entries come back in the guest's layout, kept by
 * the guest's select, ordered by alphasort or the guest's own compar, and freed with free(). The expected output is
 * written by hand: x86_64 has no legacy layout to compare against. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ent32 { uint32_t d_ino; uint16_t d_reclen; uint8_t d_type, d_namlen; char d_name[256]; };
struct ent64 { uint64_t d_ino, d_seekoff; uint16_t d_reclen, d_namlen; uint8_t d_type; char d_name[1024]; };

int scandir32(const char *, struct ent32 ***, int (*)(const struct ent32 *),
              int (*)(const struct ent32 **, const struct ent32 **)) __asm("_scandir");
int alphasort32(const struct ent32 **, const struct ent32 **) __asm("_alphasort");
int scandir64(const char *, struct ent64 ***, int (*)(const struct ent64 *),
              int (*)(const struct ent64 **, const struct ent64 **)) __asm("_scandir$INODE64");
int alphasort64(const struct ent64 **, const struct ent64 **) __asm("_alphasort$INODE64");

static int visible32(const struct ent32 *e) { return e->d_name[0] != '.'; }
static int visible64(const struct ent64 *e) { return e->d_name[0] != '.'; }
static int reverse64(const struct ent64 **a, const struct ent64 **b) { return strcmp((*b)->d_name, (*a)->d_name); }

int main(void)
{
    char dir[] = "/tmp/m32-scandir-XXXXXX", path[64];
    if (!mkdtemp(dir))
        return 1;
    const char *files[] = { "b.wav", "a.wav", "c.txt" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        fclose(fopen(path, "w"));
    }
    snprintf(path, sizeof path, "%s/sub", dir);
    mkdir(path, 0755);

    struct ent32 **l32;
    int n = scandir32(dir, &l32, visible32, alphasort32);
    printf("legacy %d:", n);
    for (int i = 0; i < n; i++) {
        printf(" %s(%u,%s)", l32[i]->d_name, l32[i]->d_namlen, l32[i]->d_type == 4 ? "dir" : "file");
        free(l32[i]);
    }
    free(l32);
    struct ent64 **l64;
    n = scandir64(dir, &l64, visible64, reverse64);
    printf("\ninode64 %d:", n);
    for (int i = 0; i < n; i++) {
        printf(" %s(%u)", l64[i]->d_name, l64[i]->d_namlen);
        free(l64[i]);
    }
    free(l64);
    n = scandir64(dir, &l64, NULL, alphasort64);
    printf("\nall %d: %s %s %s\n", n, l64[0]->d_name, l64[1]->d_name, l64[2]->d_name);
    for (int i = 0; i < n; i++)
        free(l64[i]);
    free(l64);
    printf("missing %d\n", scandir32("/nonexistent-m32", &l32, NULL, NULL));

    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/sub", dir);
    rmdir(path);
    rmdir(dir);
    return 0;
}
