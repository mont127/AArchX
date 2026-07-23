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

/* Thread-init start gate. Real macOS+Wine does not run a newly-created thread's
 * guest user code -- which would commit its 32-bit WoW64 stack or return into PE
 * -- until the main thread finishes loader-init, whose tail is the release of the
 * preloader low-space reservation (a guest munmap of [0x1000,0x200000000)). ocerz
 * workers (synthetic workq + the kernel HOSTWQ callbacks) otherwise run the
 * instant they are spawned, so a worker can commit a low 32-bit stack and another
 * unmap the old preloader stack BEFORE the main thread releases the reservation,
 * decommitting a live page another thread reads (the 0x8ffff8 fault). The gate
 * parks every worker until that release fires. Armed only for Wine (WINEARCH set),
 * so a native program that spawns libdispatch workers but never releases a
 * preloader reservation never waits and cannot hang. */
static pthread_mutex_t g_initgate_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_initgate_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_init_released = 1;

void ocerz_init_gate_arm(void)
{
    pthread_mutex_lock(&g_initgate_m);
    g_init_released = getenv("WINEARCH") ? 0 : 1;
    pthread_mutex_unlock(&g_initgate_m);
}

void ocerz_init_gate_release(void)
{
    pthread_mutex_lock(&g_initgate_m);
    if (!g_init_released) {
        g_init_released = 1;
        pthread_cond_broadcast(&g_initgate_cv);
        if (getenv("OCERZ_THRLOG"))
            fprintf(stderr, "ocerz: INITGATE released pid=%d\n", (int)getpid());
    }
    pthread_mutex_unlock(&g_initgate_m);
}

void ocerz_init_gate_wait(void)
{
    pthread_mutex_lock(&g_initgate_m);
    while (!g_init_released)
        pthread_cond_wait(&g_initgate_cv, &g_initgate_m);
    pthread_mutex_unlock(&g_initgate_m);
}

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

/* OCERZ_MEMLOG=<hexaddr>: trace every public map op whose 16KB host page covers
 * that probe address, with the post-op committed state and the calling thread, to
 * catch a decommit/restrictive-protect that clobbers a live 4KB neighbor sharing
 * the probe's 16KB host page. */
/* Defined in vm.c: the calling guest thread's current rip (0 if none). Used only
 * by the MEMLOG diagnostic to name the Wine function behind a low-range unmap. */
extern uint64_t ocerz_current_guest_rip(void);
extern uint64_t ocerz_current_guest_rsp(void);
/* Defined in dyld.c: the loaded image (Wine .so / shared-cache dylib) covering an
 * address, plus its load base, so MEMLOG can name the rip's image+offset. */
extern const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out);

static void memlog(const char *op, uint64_t gaddr, uint64_t len, int prot)
{
    static int init;
    static uint64_t probe, lowlog;
    if (!init) {
        init = 1;
        const char *e = getenv("OCERZ_MEMLOG");
        probe = e ? strtoull(e, NULL, 0) : 0;
        const char *l = getenv("OCERZ_LOWLOG");
        lowlog = l ? strtoull(l, NULL, 0) : 0;  /* ceiling: log all low ops < this */
    }
    if (!probe && !lowlog)
        return;
    uint64_t lo = round_down(gaddr);
    uint64_t hi = round_up(gaddr + len);
    uint64_t ppg = round_down(probe);
    int hit_probe = probe && ppg >= lo && ppg < hi;
    int hit_low   = lowlog && lo < lowlog && hi > 0x100000ull;
    if (!hit_probe && !hit_low)
        return;
    uint64_t rip = ocerz_current_guest_rip(), ibase = 0;
    const char *iname = rip ? ocerz_dyld_name_for_addr(rip, &ibase) : NULL;
    /* The shared-cache dylibs are not in the DynImage list, so a cache rip resolves
     * to the highest Wine .so with a nonsense offset; show <shared-cache> instead. */
    int sane = iname && (rip - ibase) < 0x8000000ull;
    uint64_t cmp = hit_probe ? probe : gaddr;  /* lowlog-only: report the op's own addr */
    fprintf(stderr, "ocerz: MEMLOG[%d/%04lx] %-7s gaddr=%#llx len=%#llx prot=%d rip=%#llx (%s+%#llx) -> probe(%#llx)committed=%d\n",
            (int)getpid(), (unsigned long)((uintptr_t)pthread_self() >> 8) & 0xffff,
            op, (unsigned long long)gaddr, (unsigned long long)len, prot,
            (unsigned long long)rip,
            sane ? iname : "<shared-cache>", (unsigned long long)(sane ? rip - ibase : 0),
            (unsigned long long)cmp, ocerz_addr_committed(cmp));
    /* For the residual-race unmap (a shared-cache libsystem syscall rip), walk the
     * guest stack for the Wine-side caller(s) -- return addresses in a Wine .so (which
     * IS in the DynImage list, so the offset is sane) name the function that asked for
     * this low-range free. The immediate caller is the lowest-offset Wine-.so hit. */
    if (op[0] == 'u' && !sane) {
        uint64_t sp = ocerz_current_guest_rsp();
        int shown = 0;
        for (int i = 0; sp && i < 160 && shown < 6; i++) {
            uint64_t a = sp + (uint64_t)i * 8;
            if (!ocerz_addr_committed(a)) break;
            uint64_t v = *(uint64_t *)ocerz_g2h(a);
            if (v < 0x300000000ull) continue;
            uint64_t b = 0; const char *n = ocerz_dyld_name_for_addr(v, &b);
            if (n && (v - b) < 0x8000000ull) {
                const char *bn = strrchr(n, '/');
                fprintf(stderr, "ocerz:   stk[+%#x]=%#llx (%s+%#llx)\n",
                        (unsigned)(i * 8), (unsigned long long)v,
                        bn ? bn + 1 : n, (unsigned long long)(v - b));
                shown++;
            }
        }
    }
}

/* OCERZ_NO_BATCH_VM: force the pre-batch per-page mprotect/mmap behavior, for A/B.
 * Default: range batching ON. */
static int ocerz_no_batch_vm(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("OCERZ_NO_BATCH_VM") ? 1 : 0;
    return v;
}

static int commit_range(const MemRegion *r, uint64_t lo, uint64_t hi, int hprot,
                        uint64_t zlo, uint64_t zhi)
{
    /* FAST PATH: when there is no zero-overlap to memset (the common case --
     * map_anywhere / ocerz_protect / claim_region all pass zlo>=zhi; only
     * ocerz_map_fixed zeroes), commit the whole contiguous run with ONE range
     * mprotect instead of one host syscall per 16KB page. [lo,hi) is a single
     * region (region_for_range proved it) and g2h is affine within a region, so
     * the host range [g2h(lo), g2h(lo)+(hi-lo)) is contiguous. Faithful: a range
     * mprotect is identical to per-page mprotect -- pages stay zero-fill-on-demand,
     * and the commit bitmap + 16KB-neighbor-union model are unchanged. On failure
     * (e.g. a read-only file-overlay page in the run) fall through to the per-page
     * loop, which keeps the mprotect-or-mmap fallback intact. */
    if (!ocerz_no_batch_vm() && zlo >= zhi && hi > lo &&
        mprotect(ocerz_g2h(lo), (size_t)(hi - lo), hprot) == 0) {
        for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE)
            bit_set(r, pg_index(r, p));
        return OCERZ_OK;
    }
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

/* Signal-safe commit-on-demand of the single guest page containing gaddr (to RW), WITHOUT
 * map_lock -- callable from the SIGSEGV/SIGBUS handler when ocerz's OWN signal-frame build
 * writes onto a reserved-but-uncommitted stack page (a no-altstack thread builds the frame
 * on its rsp). mprotect/mmap/bit_set are async-signal-safe; the only race is a concurrent
 * unmap of a page this thread is actively faulting on, which does not occur for a live
 * stack. Returns 1 if the page is now committed (caller re-runs the faulting write), 0 if
 * gaddr is not a reserved arena page (genuinely bad -> caller must not retry). */
int ocerz_commit_fault_page(uint64_t gaddr)
{
    uint64_t p = gaddr & ~((uint64_t)OCERZ_HOST_PAGE - 1);
    MemRegion *r = region_for_range(p, p + OCERZ_HOST_PAGE);
    if (!r)
        return 0;
    size_t i = pg_index(r, p);
    if (bit_test(r, i))
        return 1; /* already committed (lost a race -- fine) */
    void *hp = ocerz_g2h(p);
    if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE) != 0 &&
        mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
        return 0;
    bit_set(r, i);
    return 1;
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
    ocerz_init_gate_arm();
    return OCERZ_OK;
}

int ocerz_mem_init_identity(uint64_t size)
{
    /* Hint the arena at OCERZ_LOW_LIMIT so a LARGER arena still lands at the same low base
     * (0x300000000) the 4GB arena got from mmap(NULL); without the hint the kernel places an
     * 8GB+ reservation tens of GB high, which shifts every arena address and was observed to
     * coincide with a deterministic unmapped-fetch crash. Advisory (no MAP_FIXED) so it falls
     * back to the kernel's choice if that range is taken. */
    void *p = mmap((void *)OCERZ_LOW_LIMIT, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
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
    /* Give ocerz_map_anywhere() the upper 3/4 of the arena (was 1/2): Wine legitimately
     * reserves ~2GB of arena address space, which exhausted a 2GB bump pool and failed the
     * 1MB vprot-table mmap ("no driver could be loaded"). The lower 1/4 still holds guest
     * FIXED claims; any FIXED claim above bump_next is recorded as an island, so shrinking
     * the FIXED region only costs island bookkeeping, not correctness. (A bigger arena that
     * lands tens of GB high instead shifts addresses and triggers an unmapped-fetch crash, so
     * keep the arena at its low 4GB base and just widen the bump split.) */
    bump_next = lo + size / 4;
    if (!region_add(lo, lo + size))
        return OCERZ_ENOMEM;
    OCERZ_LOG("identity guest arena [%#llx, %#llx), bump %#llx, guest_base 0\n",
              (unsigned long long)lo, (unsigned long long)ocerz_arena_hi, (unsigned long long)bump_next);
    ocerz_init_gate_arm();
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
    memlog(prot == 0 ? "reserve" : "commit", gaddr, len, prot);
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
static int map_shared_overlay(uint64_t gaddr, uint64_t len, int prot,
                              int fd, uint64_t off, const char *op)
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
    int flags = MAP_SHARED | MAP_FIXED | (fd < 0 ? MAP_ANON : 0);
    void *got = mmap(want, (size_t)(hi - lo), host_prot(prot),
                     flags, fd, (off_t)off);
    if (got == MAP_FAILED || got != want) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE)
        bit_set(r, pg_index(r, p));
    pthread_mutex_unlock(&map_lock);
    memlog(op, gaddr, len, prot);
    return OCERZ_OK;
}

int ocerz_map_shared_anon(uint64_t gaddr, uint64_t len, int prot)
{
    return map_shared_overlay(gaddr, len, prot, -1, 0, "shared-anon");
}

int ocerz_map_shared_file(uint64_t gaddr, uint64_t len, int prot, int fd, uint64_t off)
{
    return map_shared_overlay(gaddr, len, prot, fd, off, "shared-file");
}

uint64_t ocerz_map_anywhere(uint64_t len, int prot)
{
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = bump_skip_islands(bump_next, glen, OCERZ_HOST_PAGE);
    if (gaddr + glen + OCERZ_HOST_PAGE > ocerz_arena_hi) {
        if (getenv("OCERZ_OOMLOG")) {
            uint64_t isl = 0;
            for (int i = 0; i < island_n; i++) isl += islands[i].hi - islands[i].lo;
            fprintf(stderr,
                    "ocerz: MAPOOM[%d] len=%#llx bump_next=%#llx arena=[%#llx,%#llx) free_above_bump=%#llx islands=%d isl_bytes=%#llx skipped_to=%#llx\n",
                    (int)getpid(), (unsigned long long)len, (unsigned long long)bump_next,
                    (unsigned long long)ocerz_arena_lo, (unsigned long long)ocerz_arena_hi,
                    (unsigned long long)(ocerz_arena_hi - bump_next), island_n,
                    (unsigned long long)isl, (unsigned long long)gaddr);
        }
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
    memlog(host_prot(prot) == (PROT_READ | PROT_WRITE) ? "prot-rw" : "prot-ro",
           gaddr, len, prot);
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
        /* Replace the committed host page with a FRESH private PROT_NONE reservation,
         * exactly as the macOS kernel does for munmap followed by an anonymous re-map.
         * This discards the old physical page (so the next commit reads zero, the
         * zero-fill-on-demand Wine's heap/loader rely on) AND -- crucially -- converts
         * a MAP_SHARED wineserver/file overlay back to PRIVATE. A plain mprotect+memset
         * zeroes the bytes but leaves a read-write shared page SHARED, so when the heap
         * later reuses the freed low address its metadata (e.g. a SUBHEAP data_size) is
         * silently shared with another process whose wineserver writes corrupt it --
         * the source of the 0x8FFFF8 "walk past commit_end on a garbage data_size" fault.
         * It also subsumes the old read-only-file-overlay (SxS manifest) fallback. */
        if (mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_NONE,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp) {
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
    memlog("unmap", gaddr, len, 0);
    /* The main thread's preloader-reservation release (a guest munmap covering the
     * whole low 32-bit space from ~0 up past 4GB) is the end of loader-init: let
     * the parked workers run now (they ran their guest code mid-air before this,
     * racing the release -> the 0x8ffff8 decommit). See ocerz_init_gate_*. */
    if (gaddr <= 0x10000ull && gaddr + len >= 0x100000000ull)
        ocerz_init_gate_release();
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
