/*
 * The x86_64 dyld shared cache: every system dylib pre-linked at a fixed
 * address.
 *
 * Three of the entry points here are fault-handler callbacks rather than
 * ordinary API: one unpacks a lazily-slid cache page and asks for a retry, one
 * handles a store into a page the guest patched and that was re-armed once code
 * was translated out of it, and one re-arms a range whose code has just been
 * translated.  Symbol resolution comes in a flat form and a two-level one that
 * resolves within the specific dylib a binary named, which is what keeps two
 * versions of the same library in the cache from being confused.
 */
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
int ocerz_cache_lazy_fault(uintptr_t addr);
int ocerz_cache_lazy_region(uintptr_t addr);
int ocerz_cache_region(uintptr_t addr);
int ocerz_cache_protect(uintptr_t addr, uint64_t len, int prot);
int ocerz_cache_write_fault(uintptr_t addr);
void ocerz_cache_arm_exec(uint64_t lo, uint64_t hi);
uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol);

uint64_t ocerz_cache_resolve_ex(OcerzCache *c, const char *symbol, int *found);
uint64_t ocerz_cache_resolve_in_image(OcerzCache *c, const char *path,
                                      const char *symbol, int *found);
uint64_t ocerz_cache_image_addr(OcerzCache *c, uint32_t i, const char **path_out);

void ocerz_cache_prefork(void);
void ocerz_cache_postfork(void);

#endif
