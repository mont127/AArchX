/* Mach-O image loading and process-image bootstrap. */
#ifndef OCERZ_LOADER_H
#define OCERZ_LOADER_H

#include "ocerz/types.h"

struct OcerzVM;

typedef struct OcerzImage {
    uint64_t entry;
    int entry_is_unixthread;
    uint64_t mh_gaddr;
    uint64_t vmaddr_lo;
    uint64_t vmaddr_hi;
    uint64_t slide;
    int is_pie;
    char path[1024];
} OcerzImage;

int ocerz_load_image(const char *path, OcerzImage *img);
int ocerz_setup_stack(struct OcerzVM *vm, const OcerzImage *img, int argc, char **argv, char **envp);

#endif
