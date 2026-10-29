/*
 * Application bundles (src/bundle.c).
 */
#ifndef OCERZ_BUNDLE_H
#define OCERZ_BUNDLE_H

#include <stddef.h>

/* the executable of the bundle at dir: CFBundleExecutable under Contents/MacOS, or the bundle's own name there */
int ocerz_bundle_executable(const char *dir, char *out, size_t n);
/* the string under key in the bundle's Contents/Info.plist, XML or binary */
int ocerz_bundle_plist(const char *dir, const char *key, char *out, size_t n);

#endif
