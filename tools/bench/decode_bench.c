/*
 * Decoder throughput and equivalence bench.  Linear-sweeps a window of the
 * x86_64 dyld shared cache with ocerz_decode (64-bit) and again with
 * ocerz_decode_mode(..., 1) (i386), advancing by the decoded length on success
 * and one byte on error.  Reports best-of-N ns/byte and ns/insn and an FNV-1a
 * hash folded over every decoded instruction's fields and every return code,
 * so two builds can be compared bit-for-bit.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "ocerz/decode.h"

#define DEFAULT_PATH "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64"
#define DEFAULT_OFF (1UL << 20)
#define DEFAULT_LEN (64UL << 20)
#define NRUNS 5

static uint64_t h;

static void mix(uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xff;
        h *= 1099511628211ULL;
    }
}

static void mixop(const X86Operand *o)
{
    mix(o->kind);
    mix(o->reg);
    mix(o->size);
    mix(o->high8);
    mix(o->base);
    mix(o->index);
    mix(o->scale);
    mix(o->riprel);
    mix(o->disp);
    mix(o->imm);
}

static void mixinsn(const X86Insn *in, int rc)
{
    int i;
    mix((uint64_t)rc);
    if (rc != 0)
        return;
    mix(in->rip);
    mix(in->len);
    mix(in->op);
    mix(in->opsize);
    mix(in->addrsize);
    mix(in->rep);
    mix(in->lock);
    mix(in->seg);
    mix(in->cc);
    mix(in->nops);
    mix(in->mode32);
    mix(in->vex);
    mix(in->vvvv);
    for (i = 0; i < 3; i++)
        mixop(&in->ops[i]);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double sweep(const uint8_t *base, size_t len, int mode32,
                    unsigned long long *ninsn, unsigned long long *nerr,
                    int do_hash)
{
    size_t pos = 0;
    uint64_t t0 = now_ns();
    X86Insn insn;
    *ninsn = 0;
    *nerr = 0;
    while (pos < len) {
        int rc;
        memset(&insn, 0, sizeof(insn));
        rc = mode32 ? ocerz_decode_mode(base + pos, len - pos, pos, &insn, 1)
                    : ocerz_decode(base + pos, len - pos, pos, &insn);
        if (do_hash)
            mixinsn(&insn, rc);
        if (rc == 0) {
            (*ninsn)++;
            pos += insn.len ? insn.len : 1;
        } else {
            (*nerr)++;
            pos += 1;
        }
    }
    return (double)(now_ns() - t0);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : DEFAULT_PATH;
    size_t off = argc > 2 ? strtoull(argv[2], 0, 0) : DEFAULT_OFF;
    size_t len = argc > 3 ? strtoull(argv[3], 0, 0) : DEFAULT_LEN;
    int fd = open(path, O_RDONLY);
    struct stat st;
    const uint8_t *base;
    int mode;

    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "decode_bench: cannot open %s\n", path);
        return 2;
    }
    if (off + len > (size_t)st.st_size)
        len = (size_t)st.st_size - off;
    base = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "decode_bench: cannot mmap %s\n", path);
        return 2;
    }
    base += off;

    for (mode = 0; mode < 2; mode++) {
        double best = 0;
        unsigned long long ninsn = 0, nerr = 0;
        uint64_t hbest = 0;
        int r;
        for (r = 0; r < NRUNS; r++) {
            double dt;
            h = 1469598103934665603ULL;
            dt = sweep(base, len, mode, &ninsn, &nerr, 1);
            if (r == 0 || dt < best) {
                best = dt;
                hbest = h;
            }
        }
        printf("mode=%s bytes=%zu insns=%llu errors=%llu hash=%016llx ns_per_byte=%.3f ns_per_insn=%.1f\n",
               mode ? "i386" : "x64", len, ninsn, nerr,
               (unsigned long long)hbest, best / (double)len,
               best / (double)(ninsn ? ninsn : 1));
    }
    return 0;
}
