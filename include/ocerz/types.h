/*
 * include/ocerz/types.h
 *
 * Shared primitive types, error codes, and logging for every Ocerz module.
 *
 * Ocerz is a user-space x86_64 -> arm64 dynamic binary translator for macOS:
 * a Rosetta-2-independent way to run x86_64 Mach-O binaries on Apple Silicon.
 * Guest CPU state, guest memory, instruction decoding, interpretation, JIT
 * translation, and syscall forwarding all build on the definitions here.
 *
 * Ocerz128 is the 128-bit value carrier used for SSE registers and 16-byte
 * memory operands; it is a pair of little-endian 64-bit halves (lo holds
 * bits 0..63, hi holds bits 64..127), matching x86 memory layout exactly.
 *
 * The error codes are negative so that functions can return either a
 * non-negative success value or one of these. OCERZ_EUNDEF means an x86
 * opcode that does not exist (or that Ocerz deliberately rejects),
 * OCERZ_ETRUNC means the instruction ran past the readable byte window,
 * OCERZ_ETOOLONG means more than 15 encoded bytes, OCERZ_EFORMAT is a
 * malformed Mach-O, OCERZ_EUNSUP is a recognized but unimplemented feature.
 *
 * ocerz_verbose gates diagnostics: 0 silent, 1 lifecycle logging (OCERZ_LOG),
 * 2 per-instruction tracing (OCERZ_TRACE). OCERZ_FATAL always prints.
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

#define OCERZ_LOG(...) do { if (ocerz_verbose >= 1) fprintf(stderr, "ocerz: " __VA_ARGS__); } while (0)
#define OCERZ_TRACE(...) do { if (ocerz_verbose >= 2) fprintf(stderr, "ocerz: " __VA_ARGS__); } while (0)
#define OCERZ_FATAL(...) do { fprintf(stderr, "ocerz: fatal: " __VA_ARGS__); } while (0)

#endif
