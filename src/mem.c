/*
 * src/mem.c
 *
 * Guest memory arena management implementing the contract in mem.h.
 *
 * ocerz_mem_init() reserves the whole guest range as one PROT_NONE mapping
 * and then derives guest_base from wherever the kernel placed it: the affine
 * mapping g2h(G) = G + guest_base works for ANY base, so there is no need to
 * land the reservation at a specific host address. This matters because
 * macOS (observed on 26.5) ignores non-MAP_FIXED placement hints and always
 * relocates anonymous reservations to its own chosen base; rather than fight
 * that, we ask for the reservation hintless and set guest_base = host_base -
 * arena_lo so that the guest's canonical low address maps exactly onto the
 * first reserved host page. Everything the guest ever maps lives inside this
 * reservation, which is why the fixed-placement paths below can use MAP_FIXED
 * safely: they only ever replace pages Ocerz already owns.
 *
 * Apple Silicon enforces 16KB page granularity, so all ranges are rounded
 * outward to 16KB. Callers that need several sub-16KB-aligned pieces inside
 * one region (the Mach-O loader with its 4KB-aligned segments) must map the
 * union once and then apply protections, because a second anonymous
 * MAP_FIXED into the same host page would zero its neighbor's bytes.
 *
 * Guest PROT_EXEC is translated to host PROT_READ: guest code is never
 * executed natively, it is only read by the decoder and JIT translator.
 *
 * The bump allocator for ocerz_map_anywhere() starts at guest 0x300000000,
 * far above any sane executable image, and leaves a 16KB guard gap between
 * allocations so runaway guest writes fault instead of bleeding into the
 * next allocation. ocerz_unmap() re-reserves PROT_NONE rather than
 * unmapping, keeping the arena contiguous forever.
 *
 * ocerz_map_claim_fixed() backs a guest VM_FLAGS_FIXED allocation: the kernel
 * contract is memory at EXACTLY the requested address or an error, never a
 * silent relocation (libmalloc extends large blocks in place this way and
 * trusts a success without re-reading the address). A range is granted only
 * when it provably collides with nothing — entirely at or above the bump
 * waterline and inside the arena — and the waterline then jumps past it; any
 * range below the waterline may overlap a live allocation, so the caller gets
 * the same KERN_NO_SPACE a real kernel gives for an occupied range, and the
 * guest allocator takes its normal allocate-new fallback.
 */
#include "ocerz/mem.h"

#include <sys/mman.h>
#include <stdlib.h>
#include <pthread.h>

#define OCERZ_HOST_PAGE 0x4000ull

static pthread_mutex_t bump_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ocerz_guest_base;
uint64_t ocerz_arena_lo;
uint64_t ocerz_arena_hi;
uint8_t *ocerz_commpage;

static uint64_t bump_next;

void ocerz_commpage_init(void)
{
    if (ocerz_commpage)
        return;
    uint8_t *cp = (uint8_t *)calloc(1, 0x4000);
    if (!cp)
        return;
    static const uint8_t tmpl[0x70] = {
        0x63, 0x6f, 0x6d, 0x6d, 0x70, 0x61, 0x67, 0x65, 0x20, 0x36, 0x34, 0x2d, 0x62, 0x69, 0x74, 0x00,
        0xaf, 0x1f, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00,
        0xaf, 0x1f, 0x0c, 0x00, 0x00, 0x00, 0x40, 0x00, 0xad, 0xdb, 0xba, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xe8, 0x03, 0x00, 0x00, 0x0c, 0x0c, 0x0c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0xec, 0x5e, 0x3b, 0x57, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x0c, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(cp, tmpl, sizeof tmpl);
    ocerz_commpage = cp;
}

static uint64_t round_down(uint64_t v)
{
    return v & ~(OCERZ_HOST_PAGE - 1);
}

static uint64_t round_up(uint64_t v)
{
    return (v + OCERZ_HOST_PAGE - 1) & ~(OCERZ_HOST_PAGE - 1);
}

static int host_prot(int prot)
{
    int p = 0;
    if (prot & PROT_READ)
        p |= PROT_READ;
    if (prot & PROT_WRITE)
        p |= PROT_READ | PROT_WRITE;
    if (prot & PROT_EXEC)
        p |= PROT_READ;
    return p;
}

int ocerz_mem_init(uint64_t lo, uint64_t hi)
{
    size_t len = (size_t)(hi - lo);
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        OCERZ_FATAL("could not reserve a %#zx-byte guest arena\n", len);
        return OCERZ_ENOMEM;
    }
    uint64_t host_base = (uint64_t)(uintptr_t)p;
    ocerz_guest_base = host_base - lo;
    ocerz_arena_lo = lo;
    ocerz_arena_hi = hi;
    bump_next = lo + (len / 4);
    OCERZ_LOG("guest arena [%#llx, %#llx) -> host %#llx, guest_base %#llx\n",
              (unsigned long long)lo, (unsigned long long)hi,
              (unsigned long long)host_base,
              (unsigned long long)ocerz_guest_base);
    return OCERZ_OK;
}

int ocerz_mem_init_identity(uint64_t size)
{
    void *p = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        OCERZ_FATAL("could not reserve a %#llx-byte identity arena\n", (unsigned long long)size);
        return OCERZ_ENOMEM;
    }
    uint64_t lo = (uint64_t)(uintptr_t)p;
    ocerz_guest_base = 0;
    ocerz_arena_lo = lo;
    ocerz_arena_hi = lo + size;
    bump_next = lo + size / 2;
    OCERZ_LOG("identity guest arena [%#llx, %#llx), bump %#llx, guest_base 0\n",
              (unsigned long long)lo, (unsigned long long)ocerz_arena_hi, (unsigned long long)bump_next);
    return OCERZ_OK;
}

int ocerz_map_fixed(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    if (lo < ocerz_arena_lo || hi > ocerz_arena_hi)
        return OCERZ_ENOMEM;
    void *want = ocerz_g2h(lo);
    void *p = mmap(want, (size_t)(hi - lo), host_prot(prot),
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return OCERZ_ENOMEM;
    return OCERZ_OK;
}

uint64_t ocerz_map_anywhere(uint64_t len, int prot)
{
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&bump_lock);
    uint64_t gaddr = bump_next;
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&bump_lock);
        return 0;
    }
    bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    pthread_mutex_unlock(&bump_lock);
    if (ocerz_map_fixed(gaddr, glen, prot) != OCERZ_OK)
        return 0;
    return gaddr;
}

uint64_t ocerz_map_anywhere_aligned(uint64_t len, int prot, uint64_t align)
{
    if (align < OCERZ_HOST_PAGE)
        align = OCERZ_HOST_PAGE;
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&bump_lock);
    uint64_t gaddr = (bump_next + (align - 1)) & ~(align - 1);
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&bump_lock);
        return 0;
    }
    bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    pthread_mutex_unlock(&bump_lock);
    if (ocerz_map_fixed(gaddr, glen, prot) != OCERZ_OK)
        return 0;
    return gaddr;
}

void ocerz_mem_prefork(void)
{
    pthread_mutex_lock(&bump_lock);
}

void ocerz_mem_postfork(void)
{
    pthread_mutex_unlock(&bump_lock);
}

int ocerz_map_claim_fixed(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = gaddr & ~(OCERZ_HOST_PAGE - 1);
    uint64_t hi = round_up(gaddr + len);
    pthread_mutex_lock(&bump_lock);
    if (lo < bump_next || hi + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&bump_lock);
        return OCERZ_ENOMEM;
    }
    bump_next = hi + OCERZ_HOST_PAGE;
    pthread_mutex_unlock(&bump_lock);
    return ocerz_map_fixed(lo, hi - lo, prot);
}

int ocerz_protect(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    if (mprotect(ocerz_g2h(lo), (size_t)(hi - lo), host_prot(prot)) != 0)
        return OCERZ_ENOMEM;
    return OCERZ_OK;
}

int ocerz_unmap(uint64_t gaddr, uint64_t len)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    if (lo < ocerz_arena_lo || hi > ocerz_arena_hi)
        return OCERZ_ENOMEM;
    void *p = mmap(ocerz_g2h(lo), (size_t)(hi - lo), PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return OCERZ_ENOMEM;
    return OCERZ_OK;
}
