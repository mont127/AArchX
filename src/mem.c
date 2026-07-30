/* Guest memory arena management. */
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
static uint64_t alloc_floor;

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

static uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + (align - 1)) & ~(align - 1);
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

static int host_make_writable(void *hp)
{
    if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE) == 0)
        return 0;
    return mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == hp ? 0 : -1;
}

extern uint64_t ocerz_current_guest_rip(void);
extern uint64_t ocerz_current_guest_rsp(void);

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
        lowlog = l ? strtoull(l, NULL, 0) : 0;
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

    int sane = iname && (rip - ibase) < 0x8000000ull;
    uint64_t cmp = hit_probe ? probe : gaddr;
    fprintf(stderr, "ocerz: MEMLOG[%d/%04lx] %-7s gaddr=%#llx len=%#llx prot=%d rip=%#llx (%s+%#llx) -> probe(%#llx)committed=%d\n",
            (int)getpid(), (unsigned long)((uintptr_t)pthread_self() >> 8) & 0xffff,
            op, (unsigned long long)gaddr, (unsigned long long)len, prot,
            (unsigned long long)rip,
            sane ? iname : "<shared-cache>", (unsigned long long)(sane ? rip - ibase : 0),
            (unsigned long long)cmp, ocerz_addr_committed(cmp));

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

int ocerz_commit_fault_page(uint64_t gaddr)
{
    uint64_t p = gaddr & ~((uint64_t)OCERZ_HOST_PAGE - 1);
    MemRegion *r = region_for_range(p, p + OCERZ_HOST_PAGE);
    if (!r)
        return 0;
    size_t i = pg_index(r, p);
    if (bit_test(r, i))
        return 1;
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
    alloc_floor = lo + (len / 4);
    bump_next = alloc_floor;
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

    uint64_t loader_reserve = size / 4;
    if (loader_reserve > (1ull << 30))
        loader_reserve = 1ull << 30;
    alloc_floor = lo + loader_reserve;
    bump_next = alloc_floor;
    if (!region_add(lo, lo + size))
        return OCERZ_ENOMEM;
    OCERZ_LOG("identity guest arena [%#llx, %#llx), bump %#llx, guest_base 0\n",
              (unsigned long long)lo, (unsigned long long)ocerz_arena_hi, (unsigned long long)bump_next);
    ocerz_init_gate_arm();
    return OCERZ_OK;
}

static uint64_t find_free_span_locked(uint64_t start, uint64_t end,
                                      uint64_t glen, uint64_t align)
{
    if (start < alloc_floor)
        start = alloc_floor;
    if (end > ocerz_arena_hi)
        end = ocerz_arena_hi;
    if (glen == 0 || glen > UINT64_MAX - OCERZ_HOST_PAGE)
        return 0;
    uint64_t need = glen + OCERZ_HOST_PAGE;
    if (start >= end || need > end - start)
        return 0;

    const MemRegion *r = region_for_range(alloc_floor, ocerz_arena_hi);
    if (!r)
        return 0;

    uint64_t candidate = align_up(start, align);
    while (candidate < end && need <= end - candidate) {
        candidate = bump_skip_islands(candidate, need, align);
        if (candidate >= end || need > end - candidate)
            break;

        uint64_t occupied = 0;
        for (uint64_t p = candidate; p < candidate + need; p += OCERZ_HOST_PAGE) {
            if (bit_test(r, pg_index(r, p))) {
                occupied = p;
                break;
            }
        }
        if (!occupied)
            return candidate;

        if (occupied > UINT64_MAX - 2 * OCERZ_HOST_PAGE)
            break;
        candidate = align_up(occupied + 2 * OCERZ_HOST_PAGE, align);
    }
    return 0;
}

static uint64_t find_anywhere_locked(uint64_t glen, uint64_t align)
{
    uint64_t start = bump_next;
    if (start < alloc_floor || start >= ocerz_arena_hi)
        start = alloc_floor;

    uint64_t gaddr = find_free_span_locked(start, ocerz_arena_hi, glen, align);
    if (!gaddr && start > alloc_floor)
        gaddr = find_free_span_locked(alloc_floor, start, glen, align);
    return gaddr;
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
    uint64_t gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
    if (!gaddr) {
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
    int rc = map_fixed_locked(gaddr, glen, prot, 0);
    if (rc == OCERZ_OK)
        bump_next = gaddr + glen + OCERZ_HOST_PAGE;
    pthread_mutex_unlock(&map_lock);
    return rc == OCERZ_OK ? gaddr : 0;
}

uint64_t ocerz_map_anywhere_aligned(uint64_t len, int prot, uint64_t align)
{
    if (align < OCERZ_HOST_PAGE)
        align = OCERZ_HOST_PAGE;
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = find_anywhere_locked(glen, align);
    if (!gaddr) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    int rc = map_fixed_locked(gaddr, glen, prot, 0);
    if (rc == OCERZ_OK)
        bump_next = gaddr + glen + OCERZ_HOST_PAGE;
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

int ocerz_map_hint(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo = gaddr & ~(OCERZ_HOST_PAGE - 1);
    uint64_t hi = round_up(gaddr + len);
    if (lo == 0 || hi <= lo || lo < OCERZ_LOW_LIMIT)
        return OCERZ_ENOMEM;
    pthread_mutex_lock(&map_lock);
    if (region_for_range(lo, hi)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    if (reserve_host_fixed(lo, hi - lo) != lo) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    if (!region_add(lo, hi)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    int rc = map_fixed_locked(lo, hi - lo, prot, 0);
    pthread_mutex_unlock(&map_lock);
    return rc;
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
    uint64_t gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
    if (!gaddr) {
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
