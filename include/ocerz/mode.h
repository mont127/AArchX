/*
 * Which universe a guest process binds against.
 *
 * OCERZ_MODE_CACHE is the original arrangement: Apple's x86_64 dyld shared
 * cache is mapped and the guest's system libraries are the real Intel ones,
 * translated like any other guest code.
 *
 * OCERZ_MODE_NATIVE maps no cache at all.  The guest's system libraries are
 * synthesized x86_64 images whose exports are bridge stubs into the host's
 * own arm64 frameworks, so the process keeps an x86 personality while the
 * work happens natively.  Apple's Intel frameworks stop shipping, and this is
 * the mode that survives that; it is selected explicitly until it is ready to
 * be the default.
 *
 * The mode is process-wide and fixed before the VM starts, because the JIT
 * materializes the trap-window bounds once.  Children inherit it through
 * OCERZ_MODE, injected on the spawn and exec paths.
 */
#ifndef OCERZ_MODE_H
#define OCERZ_MODE_H

enum {
    OCERZ_MODE_CACHE = 0,
    OCERZ_MODE_NATIVE = 1,
};

extern int ocerz_mode;

#endif
