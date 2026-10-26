/*
 * The on-disk translation cache: where translated blocks are kept between runs
 * and shared between processes.
 *
 * src/jit.c decides what a block's record holds and how a record becomes a
 * block again; src/tcache.c only stores records and finds them.  A record
 * starts with an OcerzTcRecHead whose key is the guest address with the
 * block's mode bits above it (OCERZ_TC_KEY_M32 for a 32-bit block,
 * OCERZ_TC_KEY_PLAIN for a block translated under the plain memory model;
 * for a block of a disk image, which moves from run to run, OCERZ_TC_KEY_IMAGE
 * and a hash of the image's identity and the block's offset in it stand in for
 * the address), whose size counts the whole record, head included, in a
 * multiple of eight bytes; sum is the store's, and a record handed to
 * ocerz_tcache_put may leave it zero.
 *
 * ocerz_tcache_mode answers what OCERZ_TCACHE asks for: OCERZ_TC_ON, the
 * default, which loads and records; OCERZ_TC_OFF, for "off" or "0";
 * OCERZ_TC_ROUNDTRIP, which src/jit.c handles alone and which touches no file;
 * and OCERZ_TC_VERIFY, which translates every block anyway and compares it
 * with the record it would have loaded.  ocerz_tcache_find returns the newest
 * stored record for a key, or NULL; the record stays readable until the next
 * call into this interface.  ocerz_tcache_has only answers whether there is
 * one, and ocerz_tcache_room whether the store, under OCERZ_TC_ON, has more than
 * half of its room left.
 * ocerz_tcache_put stores a record, ocerz_tcache_flush makes everything put so
 * far visible to other processes, and ocerz_tcache_child is called in a forked
 * child, which writes a data file of its own rather than its parent's.
 *
 * A profile is what -aot learns from a program's earlier runs.
 * ocerz_tcache_profile_open names the program (an identity of its main
 * executable, and its path) and the shared cache (its UUID), and starts a thread that reads
 * the records the profile lists ahead of ocerz_tcache_find; from then on
 * ocerz_tcache_profile_note collects shared-cache rips, each with
 * OCERZ_TC_PROF_PLAIN or OCERZ_TC_PROF_ORDERED for the memory model it was
 * kept under, and ocerz_tcache_flush and exit add the new ones to the
 * program's profile.  ocerz_tcache_profile_exec adds a program this one
 * executes.  ocerz_tcache_profile_read hands back a profile's entries (one
 * rip may come twice, once per model), and ocerz_tcache_profile_execs the
 * programs, one path a line, in a string for the caller to free.
 * ocerz_tcache_profile_image notes a disk image this process loaded, by
 * dlopen or as a dependency, and ocerz_tcache_profile_images hands back the
 * ones a program's launches loaded, the same way.  ocerz_tcache_profile_programs
 * lists every program with a profile, "<identity> <path>" a line, the most
 * recently launched first.
 */
#ifndef OCERZ_TCACHE_H
#define OCERZ_TCACHE_H

#include <stddef.h>
#include <stdint.h>

enum { OCERZ_TC_OFF = 0, OCERZ_TC_ROUNDTRIP, OCERZ_TC_ON, OCERZ_TC_VERIFY };

#define OCERZ_TC_KEY_M32 (1ull << 63)
#define OCERZ_TC_KEY_PLAIN (1ull << 62)
#define OCERZ_TC_KEY_IMAGE (1ull << 61)
#define OCERZ_TC_REC_MAGIC 0x32524354u
#define OCERZ_TC_PROF_PLAIN (1ull << 62)
#define OCERZ_TC_PROF_ORDERED (1ull << 61)
#define OCERZ_TC_PROF_RIP ((1ull << 48) - 1)
#define OCERZ_TC_PROF_MAX 100000

typedef struct OcerzTcRecHead {
    uint32_t magic;
    uint32_t size;
    uint64_t key;
    uint64_t sum;
} OcerzTcRecHead;

int ocerz_tcache_mode(void);
const OcerzTcRecHead *ocerz_tcache_find(uint64_t key);
int ocerz_tcache_has(uint64_t key);
void ocerz_tcache_put(const OcerzTcRecHead *rec);
void ocerz_tcache_flush(void);
void ocerz_tcache_child(void);
const char *ocerz_tcache_dir(int for_write);
int ocerz_tcache_room(void);
void ocerz_tcache_profile_open(uint64_t ident, const uint8_t *cache_uuid, const char *path);
void ocerz_tcache_profile_note(uint64_t v);
void ocerz_tcache_profile_note_key(uint64_t key);
int ocerz_tcache_warm_keys(void);
int ocerz_tcache_profile_ordered(void);
size_t ocerz_tcache_profile_entries(void);
uint64_t *ocerz_tcache_profile_lazy(size_t *n);
void ocerz_tcache_note_ordered(void);
void ocerz_tcache_profile_exec(const char *path);
size_t ocerz_tcache_profile_read(uint64_t ident, const uint8_t *cache_uuid, uint64_t **out);
void ocerz_tcache_profile_image(const char *path);
char *ocerz_tcache_profile_images(uint64_t ident);
char *ocerz_tcache_profile_programs(void);
char *ocerz_tcache_profile_execs(uint64_t ident);
OcerzTcRecHead *ocerz_tcache_warm_next(uint64_t *key, void **prep);
void ocerz_tcache_set_prep(void *(*prep)(const OcerzTcRecHead *rec, uint64_t key), void (*drop)(void *));
void *ocerz_tcache_take_prep(void);
int ocerz_tcache_found_warm(void);
int ocerz_tcache_warm_at(size_t *i, int take, OcerzTcRecHead **rec, uint64_t *key, void **prep);
void ocerz_tcache_warm_untake(size_t i, OcerzTcRecHead *rec);

int ocerz_tcache_store_path(char *out, size_t n);
int ocerz_tcache_native(void);
int ocerz_tcache_side_dir(const char *sub, int make, char *out, size_t n);

int ocerz_tcache_warm_has(uint64_t key);

#endif
