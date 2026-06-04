/*
 * include/ocerz/cache.h
 *
 * The x86_64 dyld shared cache: the file (split across a main file plus
 * .01..NN subcaches in the OS cryptex) that holds every system dylib
 * pre-linked at a fixed address. On macOS the individual dylibs
 * (libsystem_c.dylib, libSystem.B.dylib, ...) do NOT exist as standalone
 * files — they live only inside this cache — so any loader that wants to run
 * a normal dynamically-linked x86_64 program must map the cache itself, the
 * way Rosetta and the real dyld do. Ocerz maps it manually and resolves
 * symbols out of it (a "mini-dyld"), rather than running Apple's dyld.
 *
 * The cache's mappings have a fixed preferred base (0x7FF800000000 on this
 * machine) and that exact host address is free on Apple Silicon, so the
 * cache is mapped identity (guest address == host address == preferred
 * address) and Ocerz's dynamic memory mode runs with guest_base = 0 so the
 * guest's pointers into the cache need no translation. Every subcache
 * mapping is mmap'd MAP_FIXED at its preferred address from the subcache
 * file at its file offset; executable mappings are mapped READ (guest code
 * is interpreted/JITed, never run natively) and writable mappings are mapped
 * private so guest writes to __DATA stay process-local.
 *
 * ocerz_cache_map() maps all subcaches and locates the image table (the
 * array of dyld_cache_image_info at imagesOffset/imagesCount in the modern
 * header at 0x1C0/0x1C4, each entry carrying a dylib's mach_header address).
 * ocerz_cache_resolve() finds an exported symbol's absolute cache address by
 * walking each cache dylib's export trie (LC_DYLD_EXPORTS_TRIE, or the
 * export half of LC_DYLD_INFO) — a deliberately simple search-all-images
 * pass that sidesteps two-level-namespace re-export indirection: a symbol
 * like _printf is found directly in the dylib that truly exports it
 * (libsystem_c), which is correct for the flat set of libSystem symbols a
 * normal program imports. Re-export trie nodes are skipped for now.
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
uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol);
uint64_t ocerz_cache_image_addr(OcerzCache *c, uint32_t i, const char **path_out);

#endif
