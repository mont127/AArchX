/* Mini-dyld: runs a dynamically-linked x86_64 Mach-O without Apple's dyld. */
#ifndef OCERZ_DYLD_H
#define OCERZ_DYLD_H

#include "ocerz/types.h"

struct OcerzVM;

extern uint64_t ocerz_main_mh;

int ocerz_peek_dynamic(const char *path);
int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp);

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode);
uint64_t ocerz_dlsym(uint64_t handle, const char *sym);
int ocerz_dlclose(uint64_t handle);
uint64_t ocerz_dlerror(void);

void ocerz_dyld_dump_images(void);
const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out);

#endif
