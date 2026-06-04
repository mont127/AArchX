/*
 * include/ocerz/loader.h
 *
 * Mach-O image loading and process-image bootstrap for the guest.
 *
 * ocerz_load_image() opens an x86_64 Mach-O executable (selecting the
 * x86_64 slice of a fat binary), maps every LC_SEGMENT_64 into the guest
 * arena at its vmaddr (plus a slide for PIE images when the preferred range
 * is unavailable), zero-fills bss tails, applies segment protections with
 * guest PROT_EXEC dropped (guest code is data to Ocerz), and extracts the
 * entry point. Entry comes from LC_UNIXTHREAD (raw x86_THREAD_STATE64 rip;
 * static binaries and dyld itself) or LC_MAIN (entryoff relative to __TEXT;
 * requires dyld, reported via entry_is_unixthread=0 so the caller can decide
 * to load dyld instead).
 *
 * ocerz_setup_stack() builds the exec-time stack exactly the way XNU does
 * for an LC_UNIXTHREAD binary: from the top of a fresh stack mapping it
 * lays out the executable-path string, apple[] strings, envp strings and
 * argv strings, then the apple/envp/argv pointer vectors (each
 * NULL-terminated), then argc, leaving rsp pointing at argc, 16-byte
 * aligned. For a dyld-style entry the first stack word is instead the guest
 * address of the main executable's mach_header (mh_gaddr), which is what
 * dyld's _dyld_start expects; that path activates in the dynamic-binary
 * phase.
 *
 * vmaddr_lo/vmaddr_hi describe the image's mapped guest range (post-slide);
 * mh_gaddr is the guest address of the mach_header itself.
 */
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
