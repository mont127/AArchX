/*
 * include/ocerz/mem.h
 *
 * Guest memory: an affine guest->host mapping plus a reserved arena.
 *
 * On arm64 macOS the kernel refuses any mapping below the (ASLR-slid) main
 * executable, so the classic x86_64 process layout starting at 0x100000000
 * cannot be recreated at identical host addresses. Ocerz instead uses the
 * QEMU guest_base technique: every guest virtual address G corresponds to
 * host address G + ocerz_guest_base, a constant chosen once at startup.
 * ocerz_g2h()/ocerz_h2g() convert in each direction; all guest loads,
 * stores, fetches, and syscall pointer arguments go through this mapping.
 *
 * ocerz_mem_init() reserves one large PROT_NONE arena covering the guest
 * range [lo, hi) (default 0x100000000..0x900000000) and derives guest_base
 * from wherever the kernel placed it. The arena is reserved up front as ONE
 * MAP_ANON PROT_NONE region and is never re-mmap'd afterwards: pages are
 * committed and uncommitted purely with mprotect(), backed by a per-16KB-page
 * commit bitmap and a single map_lock that serializes every commit, uncommit,
 * protect, and bump reservation. mprotect() changes protection atomically
 * without a tear-down/re-map window, so a concurrent commit of one sub-page
 * allocation can never SIGBUS or zero a live neighbor that shares the same
 * 16KB host page (the raw-pthread thread-startup race). First mprotect-to-RW
 * of a never-touched reserved page yields a kernel-zeroed page, so guest
 * mmap(NULL) and the loaders' union map still see fresh zero memory.
 *
 * Apple Silicon pages are 16KB while x86_64 binaries assume 4KB. Commits
 * (ocerz_map_fixed/ocerz_protect) round OUTWARD to 16KB; an already-committed
 * shared page is only re-protected, never re-zeroed, except on an explicit
 * guest MAP_FIXED re-map of an occupied address, which memsets that range so
 * the guest still sees the fresh zero memory the kernel would give. Protection
 * is therefore slightly coarser than a real x86_64 kernel would give, which is
 * harmless for emulation correctness (guest code never executes natively, so
 * guest PROT_EXEC is dropped to PROT_READ).
 *
 * ocerz_map_anywhere() is a bump allocator inside the arena for guest
 * mmap(NULL) requests and the guest stack; ocerz_map_anywhere_aligned()
 * is the same allocator but rounds the result up to a power-of-two
 * boundary, which libmalloc's magazine allocator depends on when it asks
 * mach_vm_map() for an 8MB-aligned region and then recovers the region
 * header by masking any interior pointer down to that boundary. Both commit
 * their pages under map_lock together with the bump advance.
 * ocerz_unmap() returns pages to PROT_NONE (and zeroes them) instead of
 * unmapping so the arena reservation stays intact, and rounds INWARD: only
 * host pages wholly contained in the freed range are released, so a partially
 * freed 16KB page shared with a live neighbor (the libpthread post-spawn
 * stack-slack deallocate) is left committed and the neighbor survives, exactly
 * as a faithful 16KB kernel must behave. ocerz_map_claim_fixed() serves guest
 * VM_FLAGS_FIXED allocations: it grants the exact requested range only when
 * the range provably overlaps nothing (at or above the bump waterline) and
 * advances the waterline past it; otherwise it fails so the syscall layer can
 * report the honest KERN_NO_SPACE of an occupied range instead of silently
 * relocating an allocation the guest addressed by exact position.
 *
 * The sized load/store helpers use memcpy so unaligned guest accesses are
 * always safe regardless of host alignment checking.
 *
 * ocerz_watch_addr is a software store-watchpoint (set from OCERZ_WATCH in
 * vm.c): when nonzero, any ocerz_st/ocerz_st128 whose range covers it calls
 * ocerz_watch_hit, which logs value/rip/icount. The hot-path cost is one
 * always-false compare against a zero global.
 *
 * The low shadow window: fixed-address guests (the Wine x86_64-unix loader is
 * non-PIE with zero rebase records — its WINE_RESERVE segment claims guest
 * [0x1000, 0x200000000) and its __TEXT must sit at exactly 0x200000000) need
 * guest addresses an arm64 host process can never own: the kernel SIGKILLs any
 * arm64 binary whose __PAGEZERO is under 4GB, and the host's own arm64e shared
 * cache occupies the band around 0x200000000. So guest VA below
 * OCERZ_LOW_LIMIT is backed by a separate 12GB host reservation at
 * ocerz_low_base (probed at startup from a fixed candidate list, inherited by
 * child emulators through the OCERZ_LOWBASE environment variable so every
 * cooperating ocerz process shares one offset): g2h(G) = G + ocerz_low_base for
 * G < OCERZ_LOW_LIMIT, the plain affine mapping above it. The branch costs one
 * always-false compare for programs that never enable the window
 * (ocerz_low_base stays 0). ocerz_mem_init_low_shadow() reserves the block
 * lazily — only loaders that meet a fixed-address image call it.
 * ocerz_mem_register_range() reserves an additional identity-mapped guest
 * range outside the arena (the Wine loader's WINE_TOP_DOWN at 0x7ff000000000
 * is host-mappable as-is) so guest MAP_FIXED inside it is accepted. Commit
 * bookkeeping is region-based: the arena, the low window, and each registered
 * range carry their own commit bitmap.
 */
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

/* True when a host address lies inside one of the windows ocerz maps for guest
 * memory: the low-shadow block, the top block, the commpage, or the affine
 * arena. Because the emulator only ever touches guest memory through ocerz_g2h,
 * a genuine guest memory fault always lands in one of these; a fault outside
 * them is an ocerz host-code bug (a wild/NULL host pointer) and must surface as
 * a real crash, not be delivered to the guest as a signal. The affine window is
 * tested from guest 0 (not arena_lo) so a guest access to the null guard page
 * below the arena still counts as a guest fault; ocerz's own image and heap sit
 * below guest_base, so h - guest_base wraps and excludes them. */
static inline int ocerz_host_in_guest_space(const void *haddr)
{
    uint64_t h = (uint64_t)(uintptr_t)haddr;
    if (ocerz_commpage) {
        uint64_t c = (uint64_t)(uintptr_t)ocerz_commpage;
        if (h - c < OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO)
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
int ocerz_map_shared_file(uint64_t gaddr, uint64_t len, int prot, int fd, uint64_t off);
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
unsigned ocerz_host_region_prot(uint64_t gaddr, uint64_t *base, uint64_t *size);

static inline uint64_t ocerz_ld(uint64_t gaddr, int size)
{
    const void *p = ocerz_g2h(gaddr);
    uint16_t v2; uint32_t v4; uint64_t v8;
    switch (size) {
    case 1: return *(const uint8_t *)p;
    case 2: __builtin_memcpy(&v2, p, 2); return v2;
    case 4: __builtin_memcpy(&v4, p, 4); return v4;
    case 8: __builtin_memcpy(&v8, p, 8); return v8;
    default: { uint64_t v = 0; __builtin_memcpy(&v, p, (size_t)size); return v; }
    }
}

extern uint64_t ocerz_watch_addr;
extern uint64_t ocerz_watch_val;
void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi);

static inline void ocerz_st(uint64_t gaddr, int size, uint64_t v)
{
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < (uint64_t)size)
        ocerz_watch_hit(gaddr, size, v, 0);
    if (ocerz_watch_val && v == ocerz_watch_val)
        ocerz_watch_hit(gaddr, size, v, 0);
    void *p = ocerz_g2h(gaddr);
    switch (size) {
    case 1: *(uint8_t *)p = (uint8_t)v; break;
    case 2: { uint16_t t = (uint16_t)v; __builtin_memcpy(p, &t, 2); break; }
    case 4: { uint32_t t = (uint32_t)v; __builtin_memcpy(p, &t, 4); break; }
    case 8: __builtin_memcpy(p, &v, 8); break;
    default: __builtin_memcpy(p, &v, (size_t)size); break;
    }
}

static inline Ocerz128 ocerz_ld128(uint64_t gaddr)
{
    Ocerz128 v;
    memcpy(&v, ocerz_g2h(gaddr), 16);
    return v;
}

static inline void ocerz_st128(uint64_t gaddr, Ocerz128 v)
{
    if (ocerz_watch_addr && ocerz_watch_addr - gaddr < 16)
        ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
    if (ocerz_watch_val && (v.lo == ocerz_watch_val || v.hi == ocerz_watch_val))
        ocerz_watch_hit(gaddr, 16, v.lo, v.hi);
    memcpy(ocerz_g2h(gaddr), &v, 16);
}

#endif
