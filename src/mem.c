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
 * one reservation.
 *
 * Apple Silicon enforces 16KB page granularity while x86_64 guests assume 4KB,
 * so two guest allocations less than 16KB apart can share a single host page.
 * The arena is reserved up front as ONE PROT_NONE MAP_ANON region; pages are
 * committed and uncommitted purely with mprotect(), never with a second
 * MAP_FIXED mmap. This is the crux of thread safety: mprotect() changes a
 * page's protection atomically WITHOUT a tear-down/re-map window, so a worker
 * thread reading a live stack page never sees the transient SIGBUS that a
 * concurrent MAP_FIXED unmap->remap on a neighboring 4KB region would cause.
 * mprotect() on a still-zero-filled reserved page also does not zero the
 * page's bytes, so committing one sub-page allocation never destroys a live
 * neighbor that happens to share the same 16KB host page.
 *
 * A single map_lock serializes every commit, uncommit, protect, and bump
 * reservation, and a commit bitmap tracks which 16KB host pages have been
 * faulted in. A page that is already committed is only re-protected, never
 * re-zeroed, except on an explicit guest MAP_FIXED re-map of an occupied
 * address, where the guest legitimately expects fresh zero memory and
 * commit_range() makes the page writable (the shared neighbor may currently be
 * a read-only code segment) before it memsets, then sets the requested prot.
 * Because the reservation is
 * MAP_ANON PROT_NONE, the first mprotect-to-RW of a never-touched page yields
 * a kernel-zeroed page, so the loaders' union-map-then-protect pattern and
 * guest mmap(NULL) both still see zeroed fresh memory.
 *
 * Commit (map_fixed) rounds OUTWARD to 16KB so a sub-page guest map fully
 * covers its bytes, which is safe because a shared page is only ever
 * (re-)protected. Uncommit (unmap) rounds INWARD: only host pages wholly
 * contained in the freed range are returned to PROT_NONE, so a partially-freed
 * 16KB page whose other half is a live neighbor (the libpthread post-spawn
 * stack-slack deallocate) is left committed and the neighbor survives. This
 * matches a faithful 16KB kernel, which likewise cannot release a page it does
 * not wholly own.
 *
 * Guest PROT_EXEC is translated to host PROT_READ: guest code is never
 * executed natively, it is only read by the decoder and JIT translator.
 *
 * The bump allocator for ocerz_map_anywhere() starts high above any sane
 * executable image and leaves a 16KB guard gap between allocations so runaway
 * guest writes fault instead of bleeding into the next allocation; the commit
 * happens under map_lock together with the bump advance so reservation and
 * page state stay consistent across threads.
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
 *
 * Regions generalize the bookkeeping beyond the single arena: the low shadow
 * window (guest [0, OCERZ_LOW_LIMIT) backed at ocerz_low_base; see mem.h) and
 * identity-mapped registered ranges (ocerz_mem_register_range, e.g. the Wine
 * loader's WINE_TOP_DOWN reservation) each get their own commit bitmap, and
 * every commit/protect/unmap path resolves its guest range to one region
 * before touching page state. Address translation stays entirely in
 * ocerz_g2h — regions never carry an offset of their own. Both reservation
 * primitives claim their host range with mach_vm_allocate(VM_FLAGS_FIXED),
 * which fails on an occupied range instead of silently clobbering it the way
 * mmap MAP_FIXED would; Darwin has no MAP_FIXED_NOREPLACE. The low-shadow base
 * is chosen once by the first emulator in a process tree and pinned for every
 * descendant via OCERZ_LOWBASE, because a child whose low window sat at a
 * different host base could not service cross-process address translation
 * (wineserver reading a sibling's guest memory) with a constant offset.
 */
#include "ocerz/mem.h"

#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define OCERZ_HOST_PAGE 0x4000ull

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ocerz_guest_base;
uint64_t ocerz_arena_lo;
uint64_t ocerz_arena_hi;
uint64_t ocerz_low_base;
uint64_t ocerz_top_base;
uint8_t *ocerz_commpage;

static uint64_t bump_next;

/* Reserved islands: ranges inside the bump pool that a guest claimed FIXED at an
 * address ABOVE the bump waterline (e.g. Wine reserves a specific high address that
 * happens to fall in ocerz's identity arena, or libmalloc grows a block in place).
 * Recording the claim as an island -- instead of jumping bump_next past it -- keeps
 * bump_next low so ocerz_map_anywhere fills the (potentially huge) gap below the
 * island contiguously and only steps over the island once it reaches it. Jumping
 * bump_next would strand every byte between the waterline and the claim (observed:
 * a single 0x3fff40000 claim near arena_hi stranded ~1.7GB, then the vprot-table
 * mmap that Wine needs to service its own page-guard faults failed with ENOMEM). */
#define MEM_ISLAND_MAX 64
static struct { uint64_t lo, hi; } islands[MEM_ISLAND_MAX];
static int island_n;

static uint64_t bump_skip_islands(uint64_t start, uint64_t glen, uint64_t align)
{
    for (int guard = 0; guard <= island_n; guard++) {
        uint64_t end = start + glen;
        int hit = 0;
        for (int i = 0; i < island_n; i++) {
            if (start < islands[i].hi && end > islands[i].lo) {
                start = (islands[i].hi + (align - 1)) & ~(align - 1);
                hit = 1;
                break;
            }
        }
        if (!hit)
            break;
    }
    return start;
}

typedef struct {
    uint64_t glo;
    uint64_t ghi;
    uint8_t *bm;
} MemRegion;

#define MEM_REGION_MAX 16
static MemRegion regions[MEM_REGION_MAX];
static int region_n;

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

static MemRegion *region_for_range(uint64_t lo, uint64_t hi)
{
    for (int k = 0; k < region_n; k++)
        if (lo >= regions[k].glo && hi <= regions[k].ghi)
            return &regions[k];
    return NULL;
}

static size_t pg_index(const MemRegion *r, uint64_t gaddr)
{
    return (size_t)((round_down(gaddr) - r->glo) / OCERZ_HOST_PAGE);
}

static int bit_test(const MemRegion *r, size_t i)
{
    return r->bm && (r->bm[i >> 3] & (uint8_t)(1u << (i & 7)));
}

static void bit_set(const MemRegion *r, size_t i)
{
    if (r->bm)
        r->bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void bit_clr(const MemRegion *r, size_t i)
{
    if (r->bm)
        r->bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static MemRegion *region_add(uint64_t glo, uint64_t ghi)
{
    if (region_n >= MEM_REGION_MAX)
        return NULL;
    uint64_t npages = (ghi - glo) / OCERZ_HOST_PAGE;
    uint8_t *bm = (uint8_t *)calloc(1, (size_t)((npages + 7) / 8));
    if (!bm)
        return NULL;
    regions[region_n].glo = glo;
    regions[region_n].ghi = ghi;
    regions[region_n].bm = bm;
    return &regions[region_n++];
}

/* Make a host page anonymous-writable. A guest MAP_SHARED mapping of a read-only
 * fd (an SxS manifest the guest mapped then unmapped, a wineserver tmpmap) cannot
 * be mprotect'd writable nor turned back into reservation by mprotect; when
 * mprotect fails, replace the page with a fresh zero-filled anonymous page, which
 * drops the file overlay. A file overlay always covers a whole host page
 * (ocerz_map_shared_file rounds to host pages), so the replace never clobbers a
 * live anonymous 4KB neighbor sharing the same 16KB host page. Returns 0 on ok. */
static int host_make_writable(void *hp)
{
    if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE) == 0)
        return 0;
    return mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == hp ? 0 : -1;
}

static int commit_range(const MemRegion *r, uint64_t lo, uint64_t hi, int hprot,
                        uint64_t zlo, uint64_t zhi)
{
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE) {
        size_t i = pg_index(r, p);
        void *hp = ocerz_g2h(p);
        int committed = bit_test(r, i) ? 1 : 0;
        uint64_t mlo = p > zlo ? p : zlo;
        uint64_t mhi = p + OCERZ_HOST_PAGE < zhi ? p + OCERZ_HOST_PAGE : zhi;
        if (committed && mlo < mhi) {
            if (host_make_writable(hp) != 0)
                return OCERZ_ENOMEM;
            memset(ocerz_g2h(mlo), 0, (size_t)(mhi - mlo));
        }
        if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, hprot) != 0 &&
            mmap(hp, (size_t)OCERZ_HOST_PAGE, hprot,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
            return OCERZ_ENOMEM;
        if (!committed)
            bit_set(r, i);
    }
    return OCERZ_OK;
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
    if (!region_add(lo, hi))
        return OCERZ_ENOMEM;
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
    if (lo < OCERZ_LOW_LIMIT) {
        OCERZ_FATAL("identity arena landed at %#llx, below the low-shadow limit\n",
                    (unsigned long long)lo);
        return OCERZ_ENOMEM;
    }
    ocerz_guest_base = 0;
    ocerz_arena_lo = lo;
    ocerz_arena_hi = lo + size;
    bump_next = lo + size / 2;
    if (!region_add(lo, lo + size))
        return OCERZ_ENOMEM;
    OCERZ_LOG("identity guest arena [%#llx, %#llx), bump %#llx, guest_base 0\n",
              (unsigned long long)lo, (unsigned long long)ocerz_arena_hi, (unsigned long long)bump_next);
    return OCERZ_OK;
}

static uint64_t reserve_host_fixed(uint64_t base, uint64_t size)
{
    mach_vm_address_t addr = base;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS)
        return 0;
    if (mprotect((void *)(uintptr_t)addr, (size_t)size, PROT_NONE) != 0) {
        mach_vm_deallocate(mach_task_self(), addr, size);
        return 0;
    }
    return (uint64_t)addr;
}

int ocerz_mem_init_low_shadow(void)
{
    /* The low shadow window and the WINE_TOP_DOWN strip are reserved as ONE
     * contiguous host block (low window first, then the top strip): only the
     * block base (OCERZ_LOWBASE) has to be inherited by child emulators, and the
     * top base is derived as base + LOW_LIMIT. An earlier design reserved the
     * top strip with mmap(NULL), which lands at an arbitrary low host address
     * that is reliably free in the first process but collides in a re-exec'd
     * child (wineserver/services) — the child then died reserving the inherited
     * top base. A single fixed high block at a probed candidate is free in every
     * cooperating process. */
    static const uint64_t candidates[] = {
        0x8000000000ull, 0x10000000000ull, 0x500000000ull, 0x600000000000ull,
    };
    uint64_t topsz = OCERZ_TOP_HI - OCERZ_TOP_LO;
    uint64_t blocksz = OCERZ_LOW_LIMIT + topsz;
    pthread_mutex_lock(&map_lock);
    if (ocerz_low_base) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_OK;
    }
    uint64_t base = 0;
    const char *env = getenv("OCERZ_LOWBASE");
    if (env && env[0]) {
        uint64_t want = strtoull(env, NULL, 0);
        base = reserve_host_fixed(want, blocksz);
        if (base != want) {
            pthread_mutex_unlock(&map_lock);
            OCERZ_FATAL("cannot reserve inherited low-shadow base %#llx\n",
                        (unsigned long long)want);
            return OCERZ_ENOMEM;
        }
    } else {
        for (size_t k = 0; k < sizeof candidates / sizeof candidates[0] && !base; k++)
            base = reserve_host_fixed(candidates[k], blocksz);
        if (!base) {
            pthread_mutex_unlock(&map_lock);
            OCERZ_FATAL("no host base accepts the %#llx-byte shadow block\n",
                        (unsigned long long)blocksz);
            return OCERZ_ENOMEM;
        }
        char buf[24];
        snprintf(buf, sizeof buf, "%#llx", (unsigned long long)base);
        setenv("OCERZ_LOWBASE", buf, 1);
    }
    if (!region_add(0, OCERZ_LOW_LIMIT) || !region_add(OCERZ_TOP_LO, OCERZ_TOP_HI)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    ocerz_top_base = base + OCERZ_LOW_LIMIT;
    ocerz_low_base = base;
    pthread_mutex_unlock(&map_lock);
    OCERZ_LOG("low shadow window guest [0, %#llx) -> host %#llx; top strip [%#llx, %#llx) -> host %#llx\n",
              (unsigned long long)OCERZ_LOW_LIMIT, (unsigned long long)base,
              (unsigned long long)OCERZ_TOP_LO, (unsigned long long)OCERZ_TOP_HI,
              (unsigned long long)ocerz_top_base);
    return OCERZ_OK;
}

int ocerz_mem_register_range(uint64_t glo, uint64_t ghi)
{
    uint64_t lo = round_down(glo);
    uint64_t hi = round_up(ghi);
    pthread_mutex_lock(&map_lock);
    if (region_for_range(lo, hi)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_OK;
    }
    if (lo < OCERZ_LOW_LIMIT || reserve_host_fixed(lo, hi - lo) != lo) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    int ok = region_add(lo, hi) != NULL;
    pthread_mutex_unlock(&map_lock);
    if (ok)
        OCERZ_LOG("registered identity guest range [%#llx, %#llx)\n",
                  (unsigned long long)lo, (unsigned long long)hi);
    return ok ? OCERZ_OK : OCERZ_ENOMEM;
}

static int map_fixed_locked(uint64_t gaddr, uint64_t len, int prot, int zero_overlap)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    const MemRegion *r = region_for_range(lo, hi);
    if (!r)
        return OCERZ_ENOMEM;
    return commit_range(r, lo, hi, host_prot(prot),
                        zero_overlap ? gaddr : 0,
                        zero_overlap ? gaddr + len : 0);
}

int ocerz_map_fixed(uint64_t gaddr, uint64_t len, int prot)
{
    pthread_mutex_lock(&map_lock);
    int rc = map_fixed_locked(gaddr, len, prot, 1);
    pthread_mutex_unlock(&map_lock);
    return rc;
}

/* Map a file fd as genuinely shared memory at a guest address that has already
 * been reserved (ocerz_map_fixed / register_range / map_anywhere). The wineserver
 * coordinates with its clients through MAP_SHARED tmpmap files (KUSER_SHARED_DATA,
 * the session block, per-thread blocks); a private pread snapshot makes the
 * client miss every write the server makes after the snapshot (e.g. a new
 * thread's start context). MAP_FIXED overlays the shared file onto the region's
 * PROT_NONE reservation so writes from any cooperating ocerz process are seen by
 * all of them. The covered pages are marked committed in the bitmap. */
int ocerz_map_shared_file(uint64_t gaddr, uint64_t len, int prot, int fd, uint64_t off)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(lo, hi);
    if (!r) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    void *want = ocerz_g2h(lo);
    void *got = mmap(want, (size_t)(hi - lo), host_prot(prot),
                     MAP_SHARED | MAP_FIXED, fd, (off_t)off);
    if (got == MAP_FAILED || got != want) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE)
        bit_set(r, pg_index(r, p));
    pthread_mutex_unlock(&map_lock);
    return OCERZ_OK;
}

uint64_t ocerz_map_anywhere(uint64_t len, int prot)
{
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = bump_skip_islands(bump_next, glen, OCERZ_HOST_PAGE);
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    int rc = map_fixed_locked(gaddr, glen, prot, 0);
    pthread_mutex_unlock(&map_lock);
    return rc == OCERZ_OK ? gaddr : 0;
}

uint64_t ocerz_map_anywhere_aligned(uint64_t len, int prot, uint64_t align)
{
    if (align < OCERZ_HOST_PAGE)
        align = OCERZ_HOST_PAGE;
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = (bump_next + (align - 1)) & ~(align - 1);
    gaddr = bump_skip_islands(gaddr, glen, align);
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    int rc = map_fixed_locked(gaddr, glen, prot, 0);
    pthread_mutex_unlock(&map_lock);
    return rc == OCERZ_OK ? gaddr : 0;
}

void ocerz_mem_prefork(void)
{
    pthread_mutex_lock(&map_lock);
}

void ocerz_mem_postfork(void)
{
    pthread_mutex_unlock(&map_lock);
}

int ocerz_map_claim_fixed(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = gaddr & ~(OCERZ_HOST_PAGE - 1);
    uint64_t hi = round_up(gaddr + len);
    pthread_mutex_lock(&map_lock);
    if (lo < bump_next || hi + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (int i = 0; i < island_n; i++) {
        if (lo < islands[i].hi && hi > islands[i].lo) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_ENOMEM;
        }
    }
    if (lo == bump_next) {
        bump_next = hi + OCERZ_HOST_PAGE;
    } else if (island_n < MEM_ISLAND_MAX) {
        islands[island_n].lo = lo;
        islands[island_n].hi = hi + OCERZ_HOST_PAGE;
        island_n++;
    } else {
        bump_next = hi + OCERZ_HOST_PAGE;
    }
    int rc = map_fixed_locked(lo, hi - lo, prot, 0);
    pthread_mutex_unlock(&map_lock);
    return rc;
}

uint64_t ocerz_map_donate(uint64_t len)
{
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = bump_next;
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    const MemRegion *r = region_for_range(gaddr, gaddr + glen);
    if (r)
        for (uint64_t p = gaddr; p < gaddr + glen; p += OCERZ_HOST_PAGE)
            bit_set(r, pg_index(r, p));
    pthread_mutex_unlock(&map_lock);
    return gaddr;
}

int ocerz_map_claim_region(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    pthread_mutex_lock(&map_lock);
    const MemRegion *r = region_for_range(lo, hi);
    if (!r || (r->glo == ocerz_arena_lo && r->ghi == ocerz_arena_hi)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE) {
        if (bit_test(r, pg_index(r, p))) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_ENOMEM;
        }
    }
    int rc = commit_range(r, lo, hi, host_prot(prot), 0, 0);
    pthread_mutex_unlock(&map_lock);
    return rc;
}

/* Apply a guest protection change to the host pages. A 16KB host page can back
 * up to four 4KB guest pages with DIFFERENT guest protections, so a protection
 * change is rounded by direction: a PERMISSIVE change (writable) rounds OUTWARD
 * and a RESTRICTIVE change (read-only / exec-only / none) rounds INWARD. Only the
 * 16KB host pages WHOLLY covered by the restricted guest range lose write access;
 * a host page the range only partly covers is shared with a more-permissive 4KB
 * neighbor and keeps its current protection. The net per-host-page protection is
 * thus the union (most permissive) of its 4KB guests, which is what keeps a PE's
 * writable BSS/data working when it abuts a read-only section inside one 16KB
 * page (Wine protects ntdll's read-only sections to PAGE_READONLY after load; an
 * outward round would strand the adjacent BSS read-only and fault the guest's own
 * writes). The leaked write access on a partly-restricted page is unfaithful only
 * to a guest that deliberately writes read-only memory to test the fault. */
int ocerz_protect(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo, hi;
    if (host_prot(prot) == (PROT_READ | PROT_WRITE)) {
        lo = round_down(gaddr);
        hi = round_up(gaddr + len);
    } else {
        lo = round_up(gaddr);
        hi = round_down(gaddr + len);
        if (lo >= hi)
            return OCERZ_OK;
    }
    pthread_mutex_lock(&map_lock);
    const MemRegion *r = region_for_range(lo, hi);
    int rc = r ? commit_range(r, lo, hi, host_prot(prot), 0, 0) : OCERZ_ENOMEM;
    pthread_mutex_unlock(&map_lock);
    return rc;
}

int ocerz_unmap(uint64_t gaddr, uint64_t len)
{
    uint64_t lo = round_up(gaddr);
    uint64_t hi = round_down(gaddr + len);
    if (lo >= hi)
        return OCERZ_OK;
    pthread_mutex_lock(&map_lock);
    const MemRegion *r = region_for_range(lo, hi);
    if (!r) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    int rc = OCERZ_OK;
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE) {
        if (!bit_test(r, pg_index(r, p)))
            continue;
        void *hp = ocerz_g2h(p);
        if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE) == 0) {
            memset(hp, 0, (size_t)OCERZ_HOST_PAGE);
            if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_NONE) != 0) {
                rc = OCERZ_ENOMEM;
                break;
            }
        } else if (mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_NONE,
                        MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp) {
            /* A read-only file overlay (e.g. an unmapped SxS manifest) that
             * mprotect cannot restore: replace it with a fresh anonymous PROT_NONE
             * reservation page so a later writable re-commit of this address (a
             * heap reusing the freed slot) succeeds instead of faulting RO. */
            rc = OCERZ_ENOMEM;
            break;
        }
        bit_clr(r, pg_index(r, p));
    }
    for (int i = 0; i < island_n; i++) {
        if (lo <= islands[i].lo && hi >= islands[i].hi) {
            islands[i] = islands[--island_n];
            i--;
        }
    }
    pthread_mutex_unlock(&map_lock);
    return rc;
}

int ocerz_addr_committed(uint64_t gaddr)
{
    const MemRegion *r = region_for_range(round_down(gaddr), round_up(gaddr + 1));
    if (!r)
        return -1;
    return bit_test(r, pg_index(r, gaddr)) ? 1 : 0;
}

/* Diagnostic: the live host VM protection of the page backing a guest address,
 * as the kernel sees it (cur in bits 0-7, max in bits 8-15, region base/size in
 * the out-params). Returns ~0u if no region maps the address. Used by the crash
 * dump to tell a real protection fault from a bitmap/host mismatch. */
unsigned ocerz_host_region_prot(uint64_t gaddr, uint64_t *base, uint64_t *size)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)ocerz_g2h(gaddr);
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz,
                                      VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &obj);
    if (kr != KERN_SUCCESS)
        return ~0u;
    if (base) *base = (uint64_t)a;
    if (size) *size = (uint64_t)sz;
    return (unsigned)(info.protection & 0xff) | ((unsigned)(info.max_protection & 0xff) << 8);
}
