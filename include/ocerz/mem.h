/* Guest memory: an affine guest to host mapping plus a reserved arena. */
#ifndef OCERZ_MEM_H
#define OCERZ_MEM_H

#include "ocerz/types.h"

extern uint64_t ocerz_guest_base;
extern uint64_t ocerz_arena_lo;
extern uint64_t ocerz_arena_hi;
extern uint64_t ocerz_low_base;
extern uint64_t ocerz_top_base;
extern uint8_t *ocerz_commpage;

#define OCERZ_COMMPAGE_LO 0x00007fffffe00000ull
#define OCERZ_COMMPAGE_HI 0x00007fffffe04000ull
#define OCERZ_LOW_LIMIT   0x0000000300000000ull
#define OCERZ_TOP_LO      0x00007ffffe000000ull
#define OCERZ_TOP_HI      0x00007fffffe00000ull
#define OCERZ_GUEST_PAGE_SIZE 0x1000ull
#define OCERZ_HOST_PAGE_SIZE  0x4000ull

static inline void *ocerz_g2h(uint64_t gaddr)
{
    if (ocerz_commpage && gaddr >= OCERZ_COMMPAGE_LO && gaddr < OCERZ_COMMPAGE_HI)
        return ocerz_commpage + (gaddr - OCERZ_COMMPAGE_LO);
    if (ocerz_low_base) {
        if (gaddr < OCERZ_LOW_LIMIT)
            return (void *)(uintptr_t)(gaddr + ocerz_low_base);
        if (gaddr - OCERZ_TOP_LO < OCERZ_TOP_HI - OCERZ_TOP_LO)
            return (void *)(uintptr_t)(gaddr - OCERZ_TOP_LO + ocerz_top_base);
    }
    return (void *)(uintptr_t)(gaddr + ocerz_guest_base);
}

void ocerz_commpage_init(void);

static inline uint64_t ocerz_h2g(const void *haddr)
{
    uint64_t h = (uint64_t)(uintptr_t)haddr;
    if (ocerz_low_base) {
        if (h - ocerz_low_base < OCERZ_LOW_LIMIT)
            return h - ocerz_low_base;
        if (h - ocerz_top_base < OCERZ_TOP_HI - OCERZ_TOP_LO)
            return h - ocerz_top_base + OCERZ_TOP_LO;
    }
    return h - ocerz_guest_base;
}

static inline int ocerz_host_in_guest_space(const void *haddr)
{
    uint64_t h = (uint64_t)(uintptr_t)haddr;
    if (ocerz_commpage) {
        uint64_t c = (uint64_t)(uintptr_t)ocerz_commpage;
        if (h - c < OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO)
            return 1;
        /* identity mode: a plain-form JIT access to the guest commpage range
         * faults on the unmappable host address; the fault handler resolves it */
        if (ocerz_guest_base == 0 && h - OCERZ_COMMPAGE_LO < OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO)
            return 1;
    }
    if (ocerz_low_base) {
        if (h - ocerz_low_base < OCERZ_LOW_LIMIT)
            return 1;
        if (h - ocerz_top_base < OCERZ_TOP_HI - OCERZ_TOP_LO)
            return 1;
    }
    return h - ocerz_guest_base < ocerz_arena_hi;
}

int ocerz_mem_init(uint64_t lo, uint64_t hi);
int ocerz_mem_init_identity(uint64_t size);
int ocerz_mem_init_low_shadow(void);
int ocerz_mem_register_range(uint64_t glo, uint64_t ghi);
int ocerz_map_fixed(uint64_t gaddr, uint64_t len, int prot);
int ocerz_map_shared_anon(uint64_t gaddr, uint64_t len, int prot);
int ocerz_map_shared_file(uint64_t gaddr, uint64_t len, int prot, int fd, uint64_t off);
int ocerz_map_shared_file_padded(uint64_t gaddr, uint64_t len, int prot,
                                 int fd, uint64_t off);
int ocerz_map_hint(uint64_t gaddr, uint64_t len, int prot);
int ocerz_map_claim_fixed(uint64_t gaddr, uint64_t len, int prot);
int ocerz_map_claim_region(uint64_t gaddr, uint64_t len, int prot);
uint64_t ocerz_map_donate(uint64_t len);
void ocerz_mem_prefork(void);
void ocerz_mem_postfork(void);
uint64_t ocerz_map_anywhere(uint64_t len, int prot);
uint64_t ocerz_map_anywhere_aligned(uint64_t len, int prot, uint64_t align);
int ocerz_protect(uint64_t gaddr, uint64_t len, int prot);
int ocerz_unmap(uint64_t gaddr, uint64_t len);
int ocerz_addr_committed(uint64_t gaddr);
int ocerz_addr_prot(uint64_t gaddr);
int ocerz_addr_readable(uint64_t gaddr);
int ocerz_commit_fault_page(uint64_t gaddr);
unsigned ocerz_host_region_prot(uint64_t gaddr, uint64_t *base, uint64_t *size);
int ocerz_guest_vm_region(uint64_t *addr, uint64_t *size, unsigned *prot,
                          unsigned *max_prot);

void ocerz_init_gate_arm(void);
void ocerz_init_gate_release(void);
void ocerz_init_gate_wait(void);
void ocerz_init_gate_prefork(void);
void ocerz_init_gate_postfork_parent(void);
void ocerz_init_gate_postfork_child(void);

static inline uint64_t ocerz_ld(uint64_t gaddr, int size)
{
    const void *p = ocerz_g2h(gaddr);
    switch (size) {
    case 1:
        return __atomic_load_n((const uint8_t *)p, __ATOMIC_ACQUIRE);
    case 2:
        if (((uintptr_t)p & 1) == 0)
            return __atomic_load_n((const uint16_t *)p, __ATOMIC_ACQUIRE);
        break;
    case 4:
        if (((uintptr_t)p & 3) == 0)
            return __atomic_load_n((const uint32_t *)p, __ATOMIC_ACQUIRE);
        break;
    case 8:
        if (((uintptr_t)p & 7) == 0)
            return __atomic_load_n((const uint64_t *)p, __ATOMIC_ACQUIRE);
        break;
    default:
        break;
    }
    uint64_t v = 0;
    __builtin_memcpy(&v, p, (size_t)size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return v;
}

extern uint64_t ocerz_watch_addr;
extern uint64_t ocerz_watch_val;
extern uint64_t ocerz_watch_shadow;
void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi);

static inline void ocerz_st(uint64_t gaddr, int size, uint64_t v)
{
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < (uint64_t)size)
        ocerz_watch_hit(gaddr, size, v, 0);
    if (ocerz_watch_val && v == ocerz_watch_val)
        ocerz_watch_hit(gaddr, size, v, 0);
    if (ocerz_watch_shadow && ocerz_low_base && size == 8 &&
        v - ocerz_low_base < OCERZ_LOW_LIMIT)
        ocerz_watch_hit(gaddr, size, v, 0);
    void *p = ocerz_g2h(gaddr);
    switch (size) {
    case 1:
        __atomic_store_n((uint8_t *)p, (uint8_t)v, __ATOMIC_RELEASE);
        return;
    case 2:
        if (((uintptr_t)p & 1) == 0) {
            __atomic_store_n((uint16_t *)p, (uint16_t)v, __ATOMIC_RELEASE);
            return;
        }
        break;
    case 4:
        if (((uintptr_t)p & 3) == 0) {
            __atomic_store_n((uint32_t *)p, (uint32_t)v, __ATOMIC_RELEASE);
            return;
        }
        break;
    case 8:
        if (((uintptr_t)p & 7) == 0) {
            __atomic_store_n((uint64_t *)p, v, __ATOMIC_RELEASE);
            return;
        }
        break;
    default:
        break;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __builtin_memcpy(p, &v, (size_t)size);
}

static inline Ocerz128 ocerz_ld128(uint64_t gaddr)
{
    const void *p = ocerz_g2h(gaddr);
    Ocerz128 v;
    if (((uintptr_t)p & 7) == 0) {
        v.lo = __atomic_load_n((const uint64_t *)p, __ATOMIC_ACQUIRE);
        v.hi = __atomic_load_n((const uint64_t *)p + 1, __ATOMIC_ACQUIRE);
    } else {
        __builtin_memcpy(&v, p, 16);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
    return v;
}

static inline void ocerz_st128(uint64_t gaddr, Ocerz128 v)
{
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < 16)
        ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
    if (ocerz_watch_val && (v.lo == ocerz_watch_val || v.hi == ocerz_watch_val))
        ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
    if (ocerz_watch_shadow && ocerz_low_base &&
        (v.lo - ocerz_low_base < OCERZ_LOW_LIMIT ||
         v.hi - ocerz_low_base < OCERZ_LOW_LIMIT))
        ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
    void *p = ocerz_g2h(gaddr);
    if (((uintptr_t)p & 7) == 0) {
        __atomic_store_n((uint64_t *)p, v.lo, __ATOMIC_RELEASE);
        __atomic_store_n((uint64_t *)p + 1, v.hi, __ATOMIC_RELEASE);
    } else {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __builtin_memcpy(p, &v, 16);
    }
}

#endif
