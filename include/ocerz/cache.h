/* The x86_64 dyld shared cache: every system dylib pre-linked at a fixed address. */
#ifndef OCERZ_CACHE_H
#define OCERZ_CACHE_H

#include "ocerz/types.h"

typedef struct OcerzCache {
    int mapped;
    uint64_t base;
    const uint8_t *hdr;
    uint32_t images_off;
    uint32_t images_cnt;
} OcerzCache;

int ocerz_cache_map(OcerzCache *c);
uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol);

uint64_t ocerz_cache_resolve_ex(OcerzCache *c, const char *symbol, int *found);
uint64_t ocerz_cache_image_addr(OcerzCache *c, uint32_t i, const char **path_out);

#endif
