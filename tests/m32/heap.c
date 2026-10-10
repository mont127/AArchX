#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <malloc/malloc.h>
int main(void) {
    char *a = malloc(100); strcpy(a, "heap-a"); puts(a);
    int *z = calloc(1000, sizeof *z); int sum = 0; for (int i = 0; i < 1000; i++) sum += z[i];
    printf("calloc-zero %d\n", sum);
    a = realloc(a, 100000); printf("realloc keeps %s\n", a);
    printf("in-window %d\n", (unsigned)(uintptr_t)a < 0xE0000000u && (uintptr_t)a > 0x1000);
    printf("msize-ok %d\n", malloc_size(a) >= 100000);
    void *al; printf("memalign %d %d\n", posix_memalign(&al, 64, 256), ((uintptr_t)al & 63) == 0);
    char *d = strdup("dup'd"); printf("%s %d\n", d, (unsigned)(uintptr_t)d < 0xE0000000u);
    char *s; int n = asprintf(&s, "%s-%d", "fmt", 7); printf("%s %d\n", s, n);
    free(a); free(z); free(al); free(d); free(s);
    for (int i = 0; i < 100000; i++) free(malloc((i * 37) % 4096 + 1));
    puts("churn-ok");
    return 0;
}
