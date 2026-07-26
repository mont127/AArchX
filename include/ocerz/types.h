/* Shared primitive types, error codes and logging. */
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
