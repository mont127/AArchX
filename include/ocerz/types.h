/*
 * Shared primitive types, error codes and logging.
 */
#ifndef OCERZ_TYPES_H
#define OCERZ_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

typedef struct Ocerz128 {
    uint64_t lo;
    uint64_t hi;
} Ocerz128;

enum {
    OCERZ_OK = 0,
    OCERZ_EUNDEF = -1,
    OCERZ_ETRUNC = -2,
    OCERZ_ETOOLONG = -3,
    OCERZ_EIO = -4,
    OCERZ_EFORMAT = -5,
    OCERZ_ENOMEM = -6,
    OCERZ_EUNSUP = -7,
};

extern int ocerz_verbose;
extern __thread int ocerz_critical_depth;

/* ocerz's own diagnostics go through a FILE of its own on descriptor 2, which
   ocerz_diag_init opens unbuffered as the real one is.  In native mode the
   guest's stdio is the host's: a program that points stderr's _write at its
   own x86 function (HandBrake) or holds its lock had ocerz's logging run that
   function, or wait on that lock, from inside the loader, the JIT or the
   bridge.  Before ocerz_diag_init, and in a test that never calls it, this is
   the process's stderr. */
extern FILE *ocerz_stderr;
static inline FILE *ocerz_stderr_stream(void) { return ocerz_stderr ? ocerz_stderr : __stderrp; }
#undef stderr
#define stderr ocerz_stderr_stream()

#define OCERZ_LOG(...) do { if (ocerz_verbose >= 1) fprintf(stderr, "ocerz: " __VA_ARGS__); } while (0)
#define OCERZ_TRACE(...) do { if (ocerz_verbose >= 2) fprintf(stderr, "ocerz: " __VA_ARGS__); } while (0)
#define OCERZ_FATAL(...) do { fprintf(stderr, "ocerz: fatal: " __VA_ARGS__); } while (0)

#endif
