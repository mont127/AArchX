/*
 * The on-disk translation cache: where translated blocks are kept between runs
 * and shared between processes.
 *
 * src/jit.c decides what a block's record holds and how a record becomes a
 * block again; src/tcache.c only stores records and finds them.  A record
 * starts with an OcerzTcRecHead whose key is the guest address with the
 * block's mode bits above it (OCERZ_TC_KEY_M32 for a 32-bit block,
 * OCERZ_TC_KEY_PLAIN for a block translated under the plain memory model),
 * whose size counts the whole record, head included, in a multiple of eight
 * bytes; sum is the store's, and a record handed to ocerz_tcache_put may leave
 * it zero.
 *
 * ocerz_tcache_mode answers what OCERZ_TCACHE asks for: OCERZ_TC_ON, the
 * default, which loads and records; OCERZ_TC_OFF, for "off" or "0";
 * OCERZ_TC_ROUNDTRIP, which src/jit.c handles alone and which touches no file;
 * and OCERZ_TC_VERIFY, which translates every block anyway and compares it
 * with the record it would have loaded.  ocerz_tcache_find returns the newest
 * stored record for a key, or NULL; the record stays readable until the next
 * call into this interface.
 * ocerz_tcache_put stores a record, ocerz_tcache_flush makes everything put so
 * far visible to other processes, and ocerz_tcache_child is called in a forked
 * child, which writes a data file of its own rather than its parent's.
 */
#ifndef OCERZ_TCACHE_H
#define OCERZ_TCACHE_H

#include <stdint.h>

enum { OCERZ_TC_OFF = 0, OCERZ_TC_ROUNDTRIP, OCERZ_TC_ON, OCERZ_TC_VERIFY };

#define OCERZ_TC_KEY_M32 (1ull << 63)
#define OCERZ_TC_KEY_PLAIN (1ull << 62)
#define OCERZ_TC_REC_MAGIC 0x32524354u

typedef struct OcerzTcRecHead {
    uint32_t magic;
    uint32_t size;
    uint64_t key;
    uint64_t sum;
} OcerzTcRecHead;

int ocerz_tcache_mode(void);
const OcerzTcRecHead *ocerz_tcache_find(uint64_t key);
void ocerz_tcache_put(const OcerzTcRecHead *rec);
void ocerz_tcache_flush(void);
void ocerz_tcache_child(void);

#endif
