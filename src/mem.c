/* Guest memory arena management. */
#include "ocerz/mem.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define OCERZ_GUEST_PAGE OCERZ_GUEST_PAGE_SIZE
#define OCERZ_HOST_PAGE  OCERZ_HOST_PAGE_SIZE

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

void ocerz_init_gate_prefork(void)
{
    pthread_mutex_lock(&g_initgate_m);
}

void ocerz_init_gate_postfork_parent(void)
{
    pthread_mutex_unlock(&g_initgate_m);
}

void ocerz_init_gate_postfork_child(void)
{
    g_init_released = 1;
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

static uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + (align - 1)) & ~(align - 1);
}

#define MEM_SLOT_OWNER_MASK 0x00ffffffu
#define MEM_SLOT_PROT_SHIFT 24
#define MEM_SLOT_PROT_MASK  0x07000000u
#define MEM_SLOT_GUARD      0x80000000u

#define MEM_SHARED_SLOT_MASK 0x0fu
#define MEM_SHARED_PADDED    0x40u
#define MEM_SHARED_PHYSICAL  0x80u

typedef struct {
    uint64_t guard_lo;
    uint64_t guard_hi;
    uint32_t live_slots;
    uint32_t next_free;
    uint16_t region;
    uint8_t active;
} MemOwner;

typedef struct {
    uint64_t glo;
    uint64_t ghi;
    uint8_t *bm;
    uint8_t *shared;
    uint32_t *slots;
} MemRegion;

#define MEM_REGION_MAX 16
static MemRegion regions[MEM_REGION_MAX];
static int region_n;

static MemOwner *owners;
static uint32_t owner_n;
static uint32_t owner_cap;
static uint32_t owner_free;

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

static uint64_t guest_round_down(uint64_t v)
{
    return v & ~(OCERZ_GUEST_PAGE - 1);
}

static uint64_t guest_round_up(uint64_t v)
{
    return (v + OCERZ_GUEST_PAGE - 1) & ~(OCERZ_GUEST_PAGE - 1);
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
    if (hi <= lo)
        return NULL;
    int n = __atomic_load_n(&region_n, __ATOMIC_ACQUIRE);
    for (int k = 0; k < n; k++)
        if (lo >= regions[k].glo && hi <= regions[k].ghi)
            return &regions[k];
    return NULL;
}

static size_t pg_index(const MemRegion *r, uint64_t gaddr)
{
    return (size_t)((round_down(gaddr) - r->glo) / OCERZ_HOST_PAGE);
}

static size_t slot_index(const MemRegion *r, uint64_t gaddr)
{
    return (size_t)((guest_round_down(gaddr) - r->glo) / OCERZ_GUEST_PAGE);
}

static uint32_t slot_load(const MemRegion *r, size_t i)
{
    return r->slots ? __atomic_load_n(&r->slots[i], __ATOMIC_ACQUIRE) : 0;
}

static void slot_store(const MemRegion *r, size_t i, uint32_t value)
{
    if (r->slots)
        __atomic_store_n(&r->slots[i], value, __ATOMIC_RELEASE);
}

static uint32_t slot_owner(uint32_t state)
{
    return state & MEM_SLOT_OWNER_MASK;
}

static int slot_is_data(uint32_t state)
{
    return slot_owner(state) != 0 && !(state & MEM_SLOT_GUARD);
}

static uint32_t slot_data_state(uint32_t owner, int prot)
{
    return owner | ((uint32_t)(prot & 7) << MEM_SLOT_PROT_SHIFT);
}

static int bit_test(const MemRegion *r, size_t i)
{
    return r->bm && (__atomic_load_n(&r->bm[i >> 3], __ATOMIC_ACQUIRE) &
                     (uint8_t)(1u << (i & 7)));
}

static void bit_set(const MemRegion *r, size_t i)
{
    if (r->bm)
        __atomic_fetch_or(&r->bm[i >> 3], (uint8_t)(1u << (i & 7)),
                          __ATOMIC_RELEASE);
}

static void bit_clr(const MemRegion *r, size_t i)
{
    if (r->bm)
        __atomic_fetch_and(&r->bm[i >> 3], (uint8_t)~(1u << (i & 7)),
                           __ATOMIC_RELEASE);
}

static uint8_t shared_load(const MemRegion *r, size_t i)
{
    return r->shared ? r->shared[i] : 0;
}

static void shared_store(const MemRegion *r, size_t i, uint8_t value)
{
    if (r->shared)
        r->shared[i] = value;
}

static uint8_t shared_slot_bit(uint64_t gaddr)
{
    return (uint8_t)(1u << ((gaddr & (OCERZ_HOST_PAGE - 1)) /
                             OCERZ_GUEST_PAGE));
}

static MemRegion *region_add(uint64_t glo, uint64_t ghi)
{
    if (region_n >= MEM_REGION_MAX)
        return NULL;
    uint64_t npages = (ghi - glo) / OCERZ_HOST_PAGE;
    uint64_t nslots = (ghi - glo) / OCERZ_GUEST_PAGE;
    uint8_t *bm = (uint8_t *)calloc(1, (size_t)((npages + 7) / 8));
    uint8_t *shared = (uint8_t *)calloc((size_t)npages, sizeof(*shared));
    uint32_t *slots = (uint32_t *)calloc((size_t)nslots, sizeof(*slots));
    if (!bm || !shared || !slots) {
        free(bm);
        free(shared);
        free(slots);
        return NULL;
    }
    regions[region_n].glo = glo;
    regions[region_n].ghi = ghi;
    regions[region_n].bm = bm;
    regions[region_n].shared = shared;
    regions[region_n].slots = slots;
    MemRegion *result = &regions[region_n];
    __atomic_store_n(&region_n, region_n + 1, __ATOMIC_RELEASE);
    return result;
}

static int guest_range(uint64_t gaddr, uint64_t len, uint64_t *lo, uint64_t *hi)
{
    if (len == 0 || gaddr > UINT64_MAX - len)
        return 0;
    uint64_t end = gaddr + len;
    if (end > UINT64_MAX - (OCERZ_GUEST_PAGE - 1))
        return 0;
    *lo = guest_round_down(gaddr);
    *hi = guest_round_up(end);
    return *lo < *hi;
}

static int allocation_guard_end(uint64_t data_hi, uint64_t *guard_hi)
{
    if (data_hi > UINT64_MAX - (OCERZ_HOST_PAGE - 1))
        return 0;
    uint64_t aligned = round_up(data_hi);
    if (aligned > UINT64_MAX - OCERZ_HOST_PAGE)
        return 0;
    *guard_hi = aligned + OCERZ_HOST_PAGE;
    return 1;
}

static uint32_t owner_create_locked(const MemRegion *r, uint32_t live_slots,
                                    uint64_t guard_lo, uint64_t guard_hi)
{
    uint32_t id;
    if (owner_free) {
        id = owner_free;
        owner_free = owners[id - 1].next_free;
    } else {
        if (owner_n == MEM_SLOT_OWNER_MASK)
            return 0;
        if (owner_n == owner_cap) {
            uint32_t new_cap = owner_cap ? owner_cap * 2 : 256;
            if (new_cap < owner_cap || new_cap > MEM_SLOT_OWNER_MASK)
                new_cap = MEM_SLOT_OWNER_MASK;
            MemOwner *grown = (MemOwner *)realloc(owners,
                                                  (size_t)new_cap * sizeof(*grown));
            if (!grown)
                return 0;
            memset(grown + owner_cap, 0,
                   (size_t)(new_cap - owner_cap) * sizeof(*grown));
            owners = grown;
            owner_cap = new_cap;
        }
        id = ++owner_n;
    }

    MemOwner *owner = &owners[id - 1];
    owner->guard_lo = guard_lo;
    owner->guard_hi = guard_hi;
    owner->live_slots = live_slots;
    owner->next_free = 0;
    owner->region = (uint16_t)(r - regions);
    owner->active = 1;
    return id;
}

static void owner_cancel_locked(uint32_t id)
{
    if (!id || id > owner_n || !owners[id - 1].active)
        return;
    owners[id - 1].active = 0;
    owners[id - 1].next_free = owner_free;
    owner_free = id;
}

static void affected_include(uint64_t lo, uint64_t hi,
                             uint64_t *affected_lo, uint64_t *affected_hi)
{
    if (lo >= hi)
        return;
    if (*affected_lo > lo)
        *affected_lo = lo;
    if (*affected_hi < hi)
        *affected_hi = hi;
}

static void owner_retire_locked(uint32_t id, uint64_t *affected_lo,
                                uint64_t *affected_hi)
{
    if (!id || id > owner_n)
        return;
    MemOwner *owner = &owners[id - 1];
    if (!owner->active)
        return;
    MemRegion *r = &regions[owner->region];
    for (uint64_t p = owner->guard_lo; p < owner->guard_hi;
         p += OCERZ_GUEST_PAGE) {
        size_t i = slot_index(r, p);
        uint32_t state = slot_load(r, i);
        if (slot_owner(state) == id && (state & MEM_SLOT_GUARD))
            slot_store(r, i, 0);
    }
    affected_include(owner->guard_lo, owner->guard_hi,
                     affected_lo, affected_hi);
    owner->active = 0;
    owner->next_free = owner_free;
    owner_free = id;
}

static void release_slot_locked(MemRegion *r, uint64_t gaddr,
                                uint64_t *affected_lo, uint64_t *affected_hi)
{
    size_t i = slot_index(r, gaddr);
    uint32_t state = slot_load(r, i);
    uint32_t id = slot_owner(state);
    if (!id || (state & MEM_SLOT_GUARD))
        return;
    slot_store(r, i, 0);
    size_t page_i = pg_index(r, gaddr);
    uint8_t shared = shared_load(r, page_i);
    if (shared & shared_slot_bit(gaddr))
        shared_store(r, page_i,
                     (uint8_t)(shared & ~shared_slot_bit(gaddr)));
    affected_include(gaddr, gaddr + OCERZ_GUEST_PAGE,
                     affected_lo, affected_hi);
    if (id <= owner_n) {
        MemOwner *owner = &owners[id - 1];
        if (owner->active && owner->live_slots && --owner->live_slots == 0)
            owner_retire_locked(id, affected_lo, affected_hi);
    }
}

static int slots_are_free(const MemRegion *r, uint64_t lo, uint64_t hi)
{
    for (uint64_t p = lo; p < hi; p += OCERZ_GUEST_PAGE)
        if (slot_load(r, slot_index(r, p)) != 0)
            return 0;
    return 1;
}

static uint32_t claim_slots_locked(MemRegion *r, uint64_t lo, uint64_t hi,
                                   uint64_t guard_hi, int prot,
                                   uint64_t *affected_lo, uint64_t *affected_hi)
{
    if (!slots_are_free(r, lo, guard_hi))
        return 0;
    uint64_t nslots64 = (hi - lo) / OCERZ_GUEST_PAGE;
    if (nslots64 == 0 || nslots64 > UINT32_MAX)
        return 0;
    uint32_t id = owner_create_locked(r, (uint32_t)nslots64, hi, guard_hi);
    if (!id)
        return 0;

    for (uint64_t p = lo; p < hi; p += OCERZ_GUEST_PAGE)
        slot_store(r, slot_index(r, p), slot_data_state(id, prot));
    for (uint64_t p = hi; p < guard_hi; p += OCERZ_GUEST_PAGE)
        slot_store(r, slot_index(r, p), id | MEM_SLOT_GUARD);
    affected_include(lo, guard_hi, affected_lo, affected_hi);
    return id;
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
            if (!ocerz_addr_readable(a)) break;
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
    int needs_overlap_zero = 0;
    if (zlo < zhi)
        for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE)
            if (bit_test(r, pg_index(r, p))) {
                needs_overlap_zero = 1;
                break;
            }
    if (!ocerz_no_batch_vm() && !needs_overlap_zero && hi > lo &&
        mprotect(ocerz_g2h(lo), (size_t)(hi - lo), hprot) == 0) {
        for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE)
            bit_set(r, pg_index(r, p));
        return OCERZ_OK;
    }
    /* Wine's PE loader re-commits over live pages constantly, and the loop
     * below costs a memset plus two mprotects for every 16 KB page.  A run of
     * whole pages that all have to end up zeroed is cheaper to replace with
     * one fresh anonymous mapping.  Pages shared with another process are
     * excluded: remapping them would break the sharing. */
    uint64_t blo = round_up(zlo > lo ? zlo : lo);
    uint64_t bhi = round_down(zhi < hi ? zhi : hi);
    if (ocerz_no_batch_vm())
        bhi = blo;
    for (uint64_t p = blo; p < bhi; p += OCERZ_HOST_PAGE)
        if (shared_load(r, pg_index(r, p)) & MEM_SHARED_PHYSICAL) {
            bhi = blo;
            break;
        }
    if (blo < bhi) {
        void *bp = ocerz_g2h(blo);
        if (mmap(bp, (size_t)(bhi - blo), hprot,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != bp)
            return OCERZ_ENOMEM;
        for (uint64_t p = blo; p < bhi; p += OCERZ_HOST_PAGE)
            bit_set(r, pg_index(r, p));
    }

    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE) {
        if (p >= blo && p < bhi)
            continue;
        size_t i = pg_index(r, p);
        void *hp = ocerz_g2h(p);
        int committed = bit_test(r, i) ? 1 : 0;
        int physical = (shared_load(r, i) & MEM_SHARED_PHYSICAL) != 0;
        uint64_t mlo = p > zlo ? p : zlo;
        uint64_t mhi = p + OCERZ_HOST_PAGE < zhi ? p + OCERZ_HOST_PAGE : zhi;
        if (committed && mlo < mhi) {
            if (physical
                    ? mprotect(hp, (size_t)OCERZ_HOST_PAGE,
                               PROT_READ | PROT_WRITE) != 0
                    : host_make_writable(hp) != 0)
                return OCERZ_ENOMEM;
            memset(ocerz_g2h(mlo), 0, (size_t)(mhi - mlo));
        }
        if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, hprot) != 0) {
            if (physical ||
                mmap(hp, (size_t)OCERZ_HOST_PAGE, hprot,
                     MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
                return OCERZ_ENOMEM;
        }
        if (!committed)
            bit_set(r, i);
    }
    return OCERZ_OK;
}

static int host_page_guest_prot(const MemRegion *r, uint64_t page,
                                int *has_data)
{
    int prot = 0;
    *has_data = 0;
    for (uint64_t p = page; p < page + OCERZ_HOST_PAGE;
         p += OCERZ_GUEST_PAGE) {
        uint32_t state = slot_load(r, slot_index(r, p));
        if (!slot_is_data(state))
            continue;
        *has_data = 1;
        prot |= (int)((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT);
    }
    return prot;
}

static int sync_host_page_locked(const MemRegion *r, uint64_t page)
{
    size_t i = pg_index(r, page);
    int has_data;
    int prot = host_prot(host_page_guest_prot(r, page, &has_data));
    void *hp = ocerz_g2h(page);
    uint8_t shared = shared_load(r, i);
    if ((shared & MEM_SHARED_PHYSICAL) &&
        !(shared & MEM_SHARED_SLOT_MASK)) {
        if (mmap(hp, (size_t)OCERZ_HOST_PAGE, prot,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
            return OCERZ_ENOMEM;
        shared_store(r, i, 0);
        if (has_data)
            bit_set(r, i);
        else
            bit_clr(r, i);
        return OCERZ_OK;
    }
    if (!has_data) {
        if (!bit_test(r, i))
            return OCERZ_OK;
        if (mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_NONE,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
            return OCERZ_ENOMEM;
        bit_clr(r, i);
        return OCERZ_OK;
    }
    if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, prot) != 0) {
        if (shared & MEM_SHARED_PHYSICAL)
            return OCERZ_ENOMEM;
        if (mmap(hp, (size_t)OCERZ_HOST_PAGE, prot,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
            return OCERZ_ENOMEM;
    }
    bit_set(r, i);
    return OCERZ_OK;
}

static int sync_host_range_locked(const MemRegion *r, uint64_t lo, uint64_t hi)
{
    if (lo >= hi)
        return OCERZ_OK;
    lo = round_down(lo);
    hi = round_up(hi);
    int rc = OCERZ_OK;
    for (uint64_t p = lo; p < hi; p += OCERZ_HOST_PAGE) {
        uint8_t shared = shared_load(r, pg_index(r, p));
        if ((shared & MEM_SHARED_PHYSICAL) &&
            !(shared & MEM_SHARED_SLOT_MASK) &&
            sync_host_page_locked(r, p) != OCERZ_OK)
            rc = OCERZ_ENOMEM;
    }
    uint64_t run_lo = 0;
    int run_prot = 0;
    int have_run = 0;
    for (uint64_t p = lo; p <= hi; p += OCERZ_HOST_PAGE) {
        int has_data = 0;
        int prot = 0;
        if (p < hi)
            prot = host_prot(host_page_guest_prot(r, p, &has_data));
        if (have_run && (p == hi || !has_data || prot != run_prot)) {
            if (mprotect(ocerz_g2h(run_lo), (size_t)(p - run_lo),
                         run_prot) == 0) {
                for (uint64_t q = run_lo; q < p; q += OCERZ_HOST_PAGE)
                    bit_set(r, pg_index(r, q));
            } else {
                for (uint64_t q = run_lo; q < p; q += OCERZ_HOST_PAGE)
                    if (sync_host_page_locked(r, q) != OCERZ_OK)
                        rc = OCERZ_ENOMEM;
            }
            have_run = 0;
        }
        if (p == hi)
            break;
        if (has_data) {
            if (!have_run) {
                run_lo = p;
                run_prot = prot;
                have_run = 1;
            }
            continue;
        }
        if (bit_test(r, pg_index(r, p)) &&
            sync_host_page_locked(r, p) != OCERZ_OK)
            rc = OCERZ_ENOMEM;
    }
    return rc;
}

static int shared_replacement_allowed_locked(const MemRegion *r,
                                             uint64_t lo, uint64_t hi)
{
    for (uint64_t page = round_down(lo); page < round_up(hi);
         page += OCERZ_HOST_PAGE) {
        uint8_t shared = shared_load(r, pg_index(r, page));
        if (!(shared & MEM_SHARED_PHYSICAL))
            continue;
        if (lo <= page && hi >= page + OCERZ_HOST_PAGE)
            continue;
        if (!(shared & MEM_SHARED_PADDED))
            return 0;
        uint64_t first = lo > page ? lo : page;
        uint64_t end = hi < page + OCERZ_HOST_PAGE
                     ? hi : page + OCERZ_HOST_PAGE;
        for (uint64_t p = first; p < end; p += OCERZ_GUEST_PAGE)
            if (shared & shared_slot_bit(p))
                return 0;
    }
    return 1;
}

static int preserves_padded_page_locked(const MemRegion *r, uint64_t page,
                                        uint64_t lo, uint64_t hi)
{
    uint8_t shared = shared_load(r, pg_index(r, page));
    return (shared & (MEM_SHARED_PHYSICAL | MEM_SHARED_PADDED)) ==
               (MEM_SHARED_PHYSICAL | MEM_SHARED_PADDED) &&
           (lo > page || hi < page + OCERZ_HOST_PAGE);
}

static void update_padded_slots_locked(const MemRegion *r, uint64_t lo,
                                       uint64_t hi, int add)
{
    for (uint64_t page = round_down(lo); page < round_up(hi);
         page += OCERZ_HOST_PAGE) {
        if (!preserves_padded_page_locked(r, page, lo, hi))
            continue;
        size_t i = pg_index(r, page);
        uint8_t shared = shared_load(r, i);
        uint64_t first = lo > page ? lo : page;
        uint64_t end = hi < page + OCERZ_HOST_PAGE
                     ? hi : page + OCERZ_HOST_PAGE;
        uint8_t mask = 0;
        for (uint64_t p = first; p < end; p += OCERZ_GUEST_PAGE)
            mask |= shared_slot_bit(p);
        shared_store(r, i, add ? (uint8_t)(shared | mask)
                               : (uint8_t)(shared & ~mask));
    }
}

static int detach_replaced_shared_pages_locked(const MemRegion *r,
                                               uint64_t lo, uint64_t hi)
{
    for (uint64_t page = round_down(lo); page < round_up(hi);
         page += OCERZ_HOST_PAGE) {
        size_t i = pg_index(r, page);
        if (!(shared_load(r, i) & MEM_SHARED_PHYSICAL))
            continue;
        if (preserves_padded_page_locked(r, page, lo, hi))
            continue;
        void *hp = ocerz_g2h(page);
        if (mmap(hp, (size_t)OCERZ_HOST_PAGE,
                 PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp)
            return OCERZ_ENOMEM;
        shared_store(r, i, 0);
        bit_set(r, i);
    }
    return OCERZ_OK;
}

static int install_mapping_locked(MemRegion *r, uint64_t lo, uint64_t hi,
                                  uint64_t guard_hi, int prot, int replace,
                                  uint64_t zero_lo, uint64_t zero_hi,
                                  uint32_t *owner_out)
{
    if (!r || lo < r->glo || lo >= hi || guard_hi < hi ||
        guard_hi > r->ghi)
        return OCERZ_ENOMEM;
    uint64_t nslots64 = (hi - lo) / OCERZ_GUEST_PAGE;
    if (nslots64 == 0 || nslots64 > UINT32_MAX)
        return OCERZ_ENOMEM;
    if ((!replace && !slots_are_free(r, lo, guard_hi)) ||
        (replace && guard_hi > hi && !slots_are_free(r, hi, guard_hi)))
        return OCERZ_ENOMEM;
    if (!shared_replacement_allowed_locked(r, lo, hi))
        return OCERZ_EUNSUP;

    uint32_t *old_states = NULL;
    if (replace) {
        old_states = (uint32_t *)malloc((size_t)nslots64 * sizeof(*old_states));
        if (!old_states)
            return OCERZ_ENOMEM;
        for (uint64_t slot = 0; slot < nslots64; slot++)
            old_states[slot] = slot_load(
                r, slot_index(r, lo + slot * OCERZ_GUEST_PAGE));
    }

    uint32_t owner = owner_create_locked(r, (uint32_t)nslots64, hi, guard_hi);
    if (!owner) {
        free(old_states);
        return OCERZ_ENOMEM;
    }

    int rc = detach_replaced_shared_pages_locked(r, lo, hi);
    if (rc != OCERZ_OK) {
        owner_cancel_locked(owner);
        free(old_states);
        return rc;
    }

    int prepare_prot = host_prot(prot);
    for (uint64_t page = round_down(lo); page < round_up(hi);
         page += OCERZ_HOST_PAGE) {
        int has_data;
        prepare_prot |= host_prot(host_page_guest_prot(r, page, &has_data));
    }
    rc = commit_range(r, round_down(lo), round_up(hi), prepare_prot,
                      zero_lo, zero_hi);
    if (rc != OCERZ_OK) {
        owner_cancel_locked(owner);
        (void)sync_host_range_locked(r, lo, hi);
        free(old_states);
        return rc;
    }

    uint64_t affected_lo = UINT64_MAX;
    uint64_t affected_hi = 0;
    affected_include(lo, guard_hi, &affected_lo, &affected_hi);
    for (uint64_t slot = 0; slot < nslots64; slot++) {
        uint64_t p = lo + slot * OCERZ_GUEST_PAGE;
        if (replace && slot_is_data(old_states[slot])) {
            uint32_t old_owner = slot_owner(old_states[slot]);
            if (old_owner <= owner_n && owners[old_owner - 1].active &&
                owners[old_owner - 1].live_slots)
                owners[old_owner - 1].live_slots--;
        }
        slot_store(r, slot_index(r, p), slot_data_state(owner, prot));
    }
    for (uint64_t p = hi; p < guard_hi; p += OCERZ_GUEST_PAGE)
        slot_store(r, slot_index(r, p), owner | MEM_SLOT_GUARD);

    update_padded_slots_locked(r, lo, hi, 1);

    rc = sync_host_range_locked(r, affected_lo, affected_hi);
    if (rc != OCERZ_OK) {
        update_padded_slots_locked(r, lo, hi, 0);
        for (uint64_t p = hi; p < guard_hi; p += OCERZ_GUEST_PAGE) {
            size_t i = slot_index(r, p);
            if (slot_owner(slot_load(r, i)) == owner)
                slot_store(r, i, 0);
        }
        for (uint64_t slot = 0; slot < nslots64; slot++) {
            uint64_t p = lo + slot * OCERZ_GUEST_PAGE;
            if (replace) {
                slot_store(r, slot_index(r, p), old_states[slot]);
                if (slot_is_data(old_states[slot])) {
                    uint32_t old_owner = slot_owner(old_states[slot]);
                    if (old_owner <= owner_n && owners[old_owner - 1].active)
                        owners[old_owner - 1].live_slots++;
                }
            } else {
                slot_store(r, slot_index(r, p), 0);
            }
        }
        owner_cancel_locked(owner);
        (void)sync_host_range_locked(r, affected_lo, affected_hi);
        free(old_states);
        return rc;
    }

    if (replace) {
        for (uint64_t slot = 0; slot < nslots64; slot++) {
            uint32_t old_owner = slot_owner(old_states[slot]);
            if (slot_is_data(old_states[slot]) && old_owner <= owner_n &&
                owners[old_owner - 1].active &&
                owners[old_owner - 1].live_slots == 0)
                owner_retire_locked(old_owner, &affected_lo, &affected_hi);
        }
        (void)sync_host_range_locked(r, affected_lo, affected_hi);
    }
    free(old_states);
    if (owner_out)
        *owner_out = owner;
    return OCERZ_OK;
}

int ocerz_commit_fault_page(uint64_t gaddr)
{
    uint64_t p = gaddr & ~((uint64_t)OCERZ_HOST_PAGE - 1);
    MemRegion *r = region_for_range(p, p + OCERZ_HOST_PAGE);
    if (!r)
        return 0;
    uint32_t state = slot_load(r, slot_index(r, gaddr));
    if (!slot_is_data(state))
        return 0;
    size_t i = pg_index(r, p);
    if (bit_test(r, i))
        return 1;
    void *hp = ocerz_g2h(p);
    /* Publish backing first so a racing unmap will reset any page we activate. */
    bit_set(r, i);
    if (mprotect(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE) != 0 &&
        mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != hp) {
        bit_clr(r, i);
        return 0;
    }
    if (slot_load(r, slot_index(r, gaddr)) != state) {
        int has_data;
        int prot = host_prot(host_page_guest_prot(r, p, &has_data));
        if (!has_data) {
            (void)mmap(hp, (size_t)OCERZ_HOST_PAGE, PROT_NONE,
                       MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
            bit_clr(r, i);
        } else {
            (void)mprotect(hp, (size_t)OCERZ_HOST_PAGE, prot);
            bit_set(r, i);
        }
        return 0;
    }
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
    if (start >= end)
        return 0;

    const MemRegion *r = region_for_range(alloc_floor, ocerz_arena_hi);
    if (!r)
        return 0;

    if (start > UINT64_MAX - (align - 1))
        return 0;
    uint64_t candidate = align_up(start, align);
    while (candidate < end) {
        if (glen > end - candidate)
            break;
        uint64_t data_hi = candidate + glen;
        uint64_t guard_hi;
        if (!allocation_guard_end(data_hi, &guard_hi) || guard_hi > end)
            break;

        uint64_t occupied = 0;
        for (uint64_t p = candidate; p < guard_hi; p += OCERZ_GUEST_PAGE) {
            if (slot_load(r, slot_index(r, p)) != 0) {
                occupied = p;
                break;
            }
        }
        if (!occupied)
            return candidate;

        if (occupied > UINT64_MAX - OCERZ_GUEST_PAGE - (align - 1))
            break;
        candidate = align_up(occupied + OCERZ_GUEST_PAGE, align);
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

int ocerz_guest_vm_region(uint64_t *addr, uint64_t *size, unsigned *prot,
                          unsigned *max_prot)
{
    uint64_t query = guest_round_down(*addr);
    const uint64_t tail_end = 0x1000000000000ull;
    pthread_mutex_lock(&map_lock);
    for (int guard = 0; guard <= MEM_REGION_MAX; guard++) {
        const MemRegion *cls = NULL;
        const MemRegion *next = NULL;
        for (int k = 0; k < region_n; k++) {
            if (!cls && query >= regions[k].glo && query < regions[k].ghi)
                cls = &regions[k];
            else if (regions[k].glo > query &&
                     (!next || regions[k].glo < next->glo))
                next = &regions[k];
        }
        if (cls) {
            uint64_t pos = query;
            uint32_t state = slot_load(cls, slot_index(cls, pos));
            if (!slot_is_data(state)) {
                do {
                    pos += OCERZ_GUEST_PAGE;
                    if (pos >= cls->ghi)
                        break;
                    state = slot_load(cls, slot_index(cls, pos));
                } while (!slot_is_data(state));
            }
            if (pos < cls->ghi && slot_is_data(state)) {
                unsigned run_prot =
                    (state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT;
                uint64_t base = pos;
                if (pos == query) {
                    while (base > cls->glo) {
                        uint32_t before = slot_load(
                            cls, slot_index(cls, base - OCERZ_GUEST_PAGE));
                        if (!slot_is_data(before) ||
                            ((before & MEM_SLOT_PROT_MASK) >>
                             MEM_SLOT_PROT_SHIFT) != run_prot)
                            break;
                        base -= OCERZ_GUEST_PAGE;
                    }
                }
                uint64_t end = pos + OCERZ_GUEST_PAGE;
                while (end < cls->ghi) {
                    uint32_t after = slot_load(cls, slot_index(cls, end));
                    if (!slot_is_data(after) ||
                        ((after & MEM_SLOT_PROT_MASK) >>
                         MEM_SLOT_PROT_SHIFT) != run_prot)
                        break;
                    end += OCERZ_GUEST_PAGE;
                }
                *addr = base;
                *size = end - base;
                *prot = run_prot;
                *max_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
                pthread_mutex_unlock(&map_lock);
                return 1;
            }
            query = cls->ghi;
            continue;
        }
        if (next && query < next->glo) {
            query = next->glo;
            continue;
        }
        break;
    }
    pthread_mutex_unlock(&map_lock);
    *addr = query;
    *size = query < tail_end ? tail_end - query : OCERZ_HOST_PAGE;
    *prot = 0;
    *max_prot = 0;
    return 1;
}

static int map_fixed_locked(uint64_t gaddr, uint64_t len, int prot, int zero_overlap)
{
    uint64_t lo, hi;
    if (!guest_range(gaddr, len, &lo, &hi))
        return OCERZ_ENOMEM;
    MemRegion *r = region_for_range(round_down(lo), round_up(hi));
    if (!r)
        return OCERZ_ENOMEM;
    return install_mapping_locked(r, lo, hi, hi, prot, 1,
                                  zero_overlap ? gaddr : 0,
                                  zero_overlap ? gaddr + len : 0, NULL);
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
                              int fd, uint64_t off, int padded,
                              const char *op)
{
    uint64_t data_lo, data_hi;
    if (!guest_range(gaddr, len, &data_lo, &data_hi))
        return OCERZ_ENOMEM;
    if (gaddr != data_lo || (fd >= 0 && (off & (OCERZ_GUEST_PAGE - 1))))
        return OCERZ_EUNSUP;
    uint64_t lo = round_down(data_lo);
    uint64_t hi = round_up(data_hi);
    uint64_t map_off = 0;
    if (fd >= 0) {
        uint64_t prefix = data_lo - lo;
        if (off < prefix)
            return OCERZ_EUNSUP;
        map_off = off - prefix;
        if ((map_off & (OCERZ_HOST_PAGE - 1)) || map_off > INT64_MAX)
            return OCERZ_EUNSUP;
    }
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(lo, hi);
    if (!r) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (uint64_t p = data_lo; p < data_hi; p += OCERZ_GUEST_PAGE) {
        if (!slot_is_data(slot_load(r, slot_index(r, p)))) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_ENOMEM;
        }
    }
    for (uint64_t page = lo; page < hi; page += OCERZ_HOST_PAGE) {
        if (shared_load(r, pg_index(r, page)) & MEM_SHARED_PHYSICAL) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_EUNSUP;
        }
        for (uint64_t p = page; p < page + OCERZ_HOST_PAGE;
             p += OCERZ_GUEST_PAGE) {
            if (p >= data_lo && p < data_hi)
                continue;
            uint32_t state = slot_load(r, slot_index(r, p));
            unsigned sibling_prot =
                (state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT;
            if (slot_is_data(state) && sibling_prot != PROT_NONE) {
                pthread_mutex_unlock(&map_lock);
                return OCERZ_EUNSUP;
            }
        }
    }
    if (padded) {
        int fd_flags = fcntl(fd, F_GETFL);
        struct stat st;
        uint64_t map_len = hi - lo;
        if (fd < 0 || fd_flags < 0 || (fd_flags & O_ACCMODE) == O_RDONLY ||
            map_off > INT64_MAX - map_len || fstat(fd, &st) != 0 ||
            st.st_size < 0) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_EUNSUP;
        }
        uint64_t need = map_off + map_len;
        if ((uint64_t)st.st_size < need &&
            ftruncate(fd, (off_t)need) != 0) {
            pthread_mutex_unlock(&map_lock);
            return OCERZ_EUNSUP;
        }
    }
    void *want = ocerz_g2h(lo);
    int flags = MAP_SHARED | MAP_FIXED | (fd < 0 ? MAP_ANON : 0);
    void *got = mmap(want, (size_t)(hi - lo), host_prot(prot),
                     flags, fd, (off_t)map_off);
    if (got == MAP_FAILED || got != want) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    for (uint64_t page = lo; page < hi; page += OCERZ_HOST_PAGE) {
        uint8_t mask = 0;
        uint64_t first = page > data_lo ? page : data_lo;
        uint64_t end = page + OCERZ_HOST_PAGE < data_hi
                     ? page + OCERZ_HOST_PAGE : data_hi;
        for (uint64_t p = first; p < end; p += OCERZ_GUEST_PAGE)
            mask |= shared_slot_bit(p);
        shared_store(r, pg_index(r, page),
                     (uint8_t)(MEM_SHARED_PHYSICAL |
                               (padded ? MEM_SHARED_PADDED : 0) | mask));
        bit_set(r, pg_index(r, page));
    }
    for (uint64_t p = data_lo; p < data_hi; p += OCERZ_GUEST_PAGE) {
        size_t i = slot_index(r, p);
        uint32_t state = slot_load(r, i);
        slot_store(r, i, slot_data_state(slot_owner(state), prot));
    }
    int rc = sync_host_range_locked(r, lo, hi);
    pthread_mutex_unlock(&map_lock);
    memlog(op, gaddr, len, prot);
    return rc;
}

int ocerz_map_shared_anon(uint64_t gaddr, uint64_t len, int prot)
{
    return map_shared_overlay(gaddr, len, prot, -1, 0, 0, "shared-anon");
}

int ocerz_map_shared_file(uint64_t gaddr, uint64_t len, int prot, int fd, uint64_t off)
{
    return map_shared_overlay(gaddr, len, prot, fd, off, 0, "shared-file");
}

int ocerz_map_shared_file_padded(uint64_t gaddr, uint64_t len, int prot,
                                 int fd, uint64_t off)
{
    return map_shared_overlay(gaddr, len, prot, fd, off, 1,
                              "shared-file-padded");
}

uint64_t ocerz_map_anywhere(uint64_t len, int prot)
{
    if (len == 0 || len > UINT64_MAX - (OCERZ_GUEST_PAGE - 1))
        return 0;
    uint64_t glen = guest_round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
    if (!gaddr) {
        if (getenv("OCERZ_OOMLOG")) {
            fprintf(stderr,
                    "ocerz: MAPOOM[%d] len=%#llx bump_next=%#llx arena=[%#llx,%#llx) free_above_bump=%#llx\n",
                    (int)getpid(), (unsigned long long)len, (unsigned long long)bump_next,
                    (unsigned long long)ocerz_arena_lo, (unsigned long long)ocerz_arena_hi,
                    (unsigned long long)(ocerz_arena_hi - bump_next));
        }
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    uint64_t data_hi = gaddr + glen;
    uint64_t guard_hi;
    int rc = allocation_guard_end(data_hi, &guard_hi)
        ? install_mapping_locked(region_for_range(gaddr, guard_hi),
                                 gaddr, data_hi, guard_hi, prot, 0,
                                 gaddr, data_hi, NULL)
        : OCERZ_ENOMEM;
    if (rc == OCERZ_OK)
        bump_next = guard_hi;
    pthread_mutex_unlock(&map_lock);
    return rc == OCERZ_OK ? gaddr : 0;
}

uint64_t ocerz_map_anywhere_aligned(uint64_t len, int prot, uint64_t align)
{
    if (align < OCERZ_HOST_PAGE)
        align = OCERZ_HOST_PAGE;
    if ((align & (align - 1)) != 0 || len == 0 ||
        len > UINT64_MAX - (OCERZ_GUEST_PAGE - 1))
        return 0;
    uint64_t glen = guest_round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = find_anywhere_locked(glen, align);
    if (!gaddr) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    uint64_t data_hi = gaddr + glen;
    uint64_t guard_hi;
    int rc = allocation_guard_end(data_hi, &guard_hi)
        ? install_mapping_locked(region_for_range(gaddr, guard_hi),
                                 gaddr, data_hi, guard_hi, prot, 0,
                                 gaddr, data_hi, NULL)
        : OCERZ_ENOMEM;
    if (rc == OCERZ_OK)
        bump_next = guard_hi;
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
    if (len == 0 || gaddr > UINT64_MAX - len ||
        gaddr + len > UINT64_MAX - (OCERZ_HOST_PAGE - 1))
        return OCERZ_ENOMEM;
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
    uint64_t lo, hi;
    if (!guest_range(gaddr, len, &lo, &hi))
        return OCERZ_ENOMEM;
    uint64_t guard_hi;
    if (!allocation_guard_end(hi, &guard_hi))
        return OCERZ_ENOMEM;
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(round_down(lo), guard_hi);
    if (!r || lo < alloc_floor || guard_hi > ocerz_arena_hi) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    int rc = install_mapping_locked(r, lo, hi, guard_hi, prot, 0,
                                    lo, hi, NULL);
    if (rc == OCERZ_OK && lo == bump_next)
        bump_next = guard_hi;
    pthread_mutex_unlock(&map_lock);
    return rc;
}

uint64_t ocerz_map_donate(uint64_t len)
{
    if (len == 0 || len > UINT64_MAX - (OCERZ_HOST_PAGE - 1))
        return 0;
    uint64_t glen = round_up(len);
    pthread_mutex_lock(&map_lock);
    uint64_t gaddr = find_anywhere_locked(glen, OCERZ_HOST_PAGE);
    if (!gaddr) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    uint64_t data_hi = gaddr + glen;
    uint64_t guard_hi;
    MemRegion *r = NULL;
    uint64_t affected_lo = UINT64_MAX, affected_hi = 0;
    uint32_t owner = 0;
    if (allocation_guard_end(data_hi, &guard_hi)) {
        r = region_for_range(gaddr, guard_hi);
        if (r)
            owner = claim_slots_locked(r, gaddr, data_hi, guard_hi,
                                       PROT_READ | PROT_WRITE,
                                       &affected_lo, &affected_hi);
    }
    if (!owner) {
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    bump_next = guard_hi;
    for (uint64_t p = gaddr; p < data_hi; p += OCERZ_HOST_PAGE)
        bit_set(r, pg_index(r, p));
    pthread_mutex_unlock(&map_lock);
    return gaddr;
}

int ocerz_map_claim_region(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo, hi;
    if (!guest_range(gaddr, len, &lo, &hi))
        return OCERZ_ENOMEM;
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(round_down(lo), round_up(hi));
    if (!r || (r->glo == ocerz_arena_lo && r->ghi == ocerz_arena_hi)) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    int rc = install_mapping_locked(r, lo, hi, hi, prot, 0,
                                    lo, hi, NULL);
    pthread_mutex_unlock(&map_lock);
    return rc;
}

int ocerz_protect(uint64_t gaddr, uint64_t len, int prot)
{
    uint64_t lo, hi;
    if (!guest_range(gaddr, len, &lo, &hi))
        return OCERZ_ENOMEM;
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(round_down(lo), round_up(hi));
    int rc = r ? OCERZ_OK : OCERZ_ENOMEM;
    for (uint64_t p = lo; rc == OCERZ_OK && p < hi;
         p += OCERZ_GUEST_PAGE) {
        uint32_t state = slot_load(r, slot_index(r, p));
        if (!slot_is_data(state))
            rc = OCERZ_ENOMEM;
        uint8_t shared = shared_load(r, pg_index(r, p));
        if ((shared & MEM_SHARED_PHYSICAL) &&
            !(shared & shared_slot_bit(p)) && prot != PROT_NONE)
            rc = OCERZ_EUNSUP;
    }
    if (rc == OCERZ_OK) {
        for (uint64_t p = lo; p < hi; p += OCERZ_GUEST_PAGE) {
            size_t i = slot_index(r, p);
            uint32_t state = slot_load(r, i);
            slot_store(r, i, slot_data_state(slot_owner(state), prot));
        }
        rc = sync_host_range_locked(r, lo, hi);
    }
    pthread_mutex_unlock(&map_lock);
    memlog(host_prot(prot) == (PROT_READ | PROT_WRITE) ? "prot-rw" : "prot-ro",
           gaddr, len, prot);
    return rc;
}

int ocerz_unmap(uint64_t gaddr, uint64_t len)
{
    uint64_t lo, hi;
    if (!guest_range(gaddr, len, &lo, &hi))
        return OCERZ_ENOMEM;
    pthread_mutex_lock(&map_lock);
    MemRegion *r = region_for_range(round_down(lo), round_up(hi));
    if (!r) {
        pthread_mutex_unlock(&map_lock);
        return OCERZ_ENOMEM;
    }
    uint64_t affected_lo = UINT64_MAX, affected_hi = 0;
    for (uint64_t p = lo; p < hi; p += OCERZ_GUEST_PAGE)
        release_slot_locked(r, p, &affected_lo, &affected_hi);
    int rc = sync_host_range_locked(r, affected_lo, affected_hi);
    pthread_mutex_unlock(&map_lock);
    memlog("unmap", gaddr, len, 0);

    if (gaddr <= 0x10000ull && hi >= 0x100000000ull)
        ocerz_init_gate_release();
    return rc;
}

int ocerz_addr_committed(uint64_t gaddr)
{
    if (gaddr == UINT64_MAX)
        return -1;
    const MemRegion *r = region_for_range(round_down(gaddr), round_up(gaddr + 1));
    if (!r)
        return -1;
    return slot_is_data(slot_load(r, slot_index(r, gaddr))) ? 1 : 0;
}

int ocerz_addr_prot(uint64_t gaddr)
{
    if (gaddr == UINT64_MAX)
        return -1;
    const MemRegion *r = region_for_range(round_down(gaddr),
                                          round_up(gaddr + 1));
    if (!r)
        return -1;
    uint32_t state = slot_load(r, slot_index(r, gaddr));
    if (!slot_is_data(state))
        return -1;
    return (int)((state & MEM_SLOT_PROT_MASK) >> MEM_SLOT_PROT_SHIFT);
}

int ocerz_addr_readable(uint64_t gaddr)
{
    int prot = ocerz_addr_prot(gaddr);
    return prot >= 0 && (prot & PROT_READ) != 0;
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
