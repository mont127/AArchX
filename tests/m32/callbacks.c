#include <stdio.h>
#include <stdlib.h>
static int order[4], n_order;
__attribute__((constructor)) static void early(void) { order[n_order++] = 1; }
__attribute__((constructor)) static void early2(void) { order[n_order++] = 2; }
static void bye(void) { puts("atexit ran"); }
static void bye2(void) { puts("atexit ran first"); }
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(void) {
    printf("ctors %d %d %d\n", n_order, order[0], order[1]);
    int v[8] = { 5, 3, 9, 1, 7, 2, 8, 4 }; qsort(v, 8, sizeof v[0], cmp);
    for (int i = 0; i < 8; i++) printf("%d%c", v[i], i == 7 ? '\n' : ' ');
    double d[4] = { 2.5, -1.0, 9.75, 0.0 }; qsort(d, 4, sizeof d[0], cmpd);
    printf("%.2f %.2f %.2f %.2f\n", d[0], d[1], d[2], d[3]);
    int key = 7; int *hit = bsearch(&key, v, 8, sizeof v[0], cmp); printf("bsearch %d\n", hit ? *hit : -1);
    atexit(bye); atexit(bye2);
    return 5;
}
