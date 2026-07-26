/* The dyld runtime API shim. */
#ifndef OCERZ_DYLDAPI_H
#define OCERZ_DYLDAPI_H

#include "ocerz/cpu.h"

#define OCERZ_DYLDAPI_LO 0x00000000dda00000ull
#define OCERZ_DYLDAPI_HI 0x00000000dda10000ull

struct OcerzVM;
struct OcerzCache;

int ocerz_dyldapi_setup(struct OcerzCache *cache);
int ocerz_dyldapi_dispatch(struct OcerzVM *vm, OcerzCPU *cpu);
void ocerz_dyldapi_run_image_loads(struct OcerzVM *vm, uint64_t mh, uint64_t stack_top);
void ocerz_dyldapi_register_image(uint64_t mh, const char *path);
void ocerz_dyldapi_objc_map_one(struct OcerzVM *vm, uint64_t mh);
uint64_t ocerz_dyldapi_canonical_selector(const char *name);
void ocerz_dyldapi_dump_method(uint64_t cls, const char *sel);

#endif
