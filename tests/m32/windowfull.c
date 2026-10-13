/* A full window: mmap answers ENOMEM, and what it handed out before comes back with munmap, so filling the window a
 * second time fits as many blocks as the first (nothing mapped on the way to the failure stays behind), and the heap
 * still serves malloc afterwards. Every allocation of guest memory goes through m32_map (src/m32_mem.c). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define BLOCK (128u << 20)

static int fill(void **blocks, int *err)
{
    int n = 0;
    for (; n < 64; n++) {
        blocks[n] = mmap(NULL, BLOCK, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (blocks[n] == MAP_FAILED) {
            *err = errno;
            break;
        }
    }
    for (int i = 0; i < n; i++)
        munmap(blocks[i], BLOCK);
    return n;
}

int main(void)
{
    void *blocks[64];
    int e1 = 0, e2 = 0, first = fill(blocks, &e1), second = fill(blocks, &e2);
    char *p = malloc(1 << 20);
    if (p)
        p[(1 << 20) - 1] = 1;
    printf("window full: at least 16 %d, ENOMEM %d, refilled the same %d, malloc after %d\n", first >= 16,
           e1 == ENOMEM && e2 == ENOMEM, first == second, p != NULL);
    return 0;
}
