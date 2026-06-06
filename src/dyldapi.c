/*
 * src/dyldapi.c
 *
 * The dyld runtime API shim (dyldapi.h): install a process-wide "dyld APIs"
 * object so the genuine libdyld.dylib trampolines work, and implement the
 * dyld services they invoke natively in Ocerz.
 *
 * Setup. The libdyld trampolines all begin `mov rdi,[apis]; mov rax,[rdi];
 * jmp [rax+slot]`, loading one global. ocerz_dyldapi_setup finds that global
 * robustly by resolving _dyld_get_active_platform and decoding its first
 * rip-relative load (`48 8b 3d <disp32>` after the `55 48 89 e5` prologue) to
 * recover the global's address, then builds two guest objects: a vtable of
 * 0x2000 bytes whose every 8-byte slot at offset OFF holds the native-thunk
 * address OCERZ_DYLDAPI_LO+OFF, and an APIs object whose first qword points at
 * that vtable. The object pointer is written into the libdyld global. From
 * then on any `jmp [vtable+OFF]` lands on a native-thunk address.
 *
 * Dispatch. The interpreter recognises an rip in [OCERZ_DYLDAPI_LO,
 * OCERZ_DYLDAPI_HI) and calls ocerz_dyldapi_dispatch, where the vtable byte
 * offset is rip-OCERZ_DYLDAPI_LO. Because the trampolines shift arguments down
 * by one (the object becomes `this`=rdi), a method sees this=rdi and its own
 * arguments in rsi/rdx/rcx/r8/r9. Each handler computes a result into rax and
 * returns to the guest by popping the caller's return address off the stack
 * (the trampoline tail-called us, so [rsp] is the original caller). Only the
 * slots a running program actually reaches are implemented; an unimplemented
 * slot logs its offset and returns 0, which is enough to surface the next slot
 * without derailing — slots that matter are then given real behaviour.
 *
 * The platform is reported as PLATFORM_MACOS (1). Image/dlopen/TLV services
 * are added here as programs reach them.
 *
 * The objc<->dyld handshake. libobjc's _objc_init registers callbacks through
 * slot 0x358 (_dyld_objc_register_callbacks) passing a version-4 struct
 * {version@0, mapped@8, init@0x10, unmapped@0x18, patches@0x20}. Real dyld
 * answers by synchronously invoking the `mapped` callback with the
 * already-loaded objc images, which is what drives objc's _read_images /
 * preopt_init / class realization; without it objc later dereferences a NULL
 * shared-cache selector table. Ocerz replicates this: it builds an array of
 * 32-byte _dyld_objc_notify_mapped_info entries {mach_header@0, objcImageInfo@8,
 * path@0x10, sectionLocations@0x18} for every loaded objc image and calls
 * `mapped` via a nested ocerz_vm_call. The "loaded" set is the real dependency
 * closure of the main executable: compute_closure() BFS-walks the executable's
 * LC_LOAD_DYLIB / weak / reexport / upward / lazy dependencies transitively,
 * resolving each install path to its cache image, so a binary that links
 * Foundation gets CoreFoundation/Foundation processed and a plain libc binary
 * gets only the libSystem closure — both correct. Of that closure only images
 * carrying __objc_imageinfo are handed to objc. map_images_nolock then asks
 * dyld for the
 * shared-cache objc-optimization roots, which Ocerz derives from libobjc's
 * __TEXT,__objc_opt_ro (an objc_opt_t v16 whose i32 fields at +0x0c/+0x18 give
 * the header-info RO/RW tables): slot 0x3a8 (header_opt_rw) and 0x3b0
 * (header_opt_ro) return those, slot 0x1f8 returns the shared-cache range, and
 * slot 0x378 (per-image section getter) returns 0 so objc walks the Mach-O.
 *
 * Class lookup by name (slot 0x2a8 = _dyld_for_each_objc_class). Modern objc
 * does NOT hash cache classes itself: getPreoptimizedClass delegates to dyld,
 * passing the class name and a block invoked as (block, classPtr, isLoaded,
 * stop*). Without this slot every objc_getClass of an unrealized cache class
 * misses and CoreFoundation's __CFInitialize bridge registration allocates a
 * hollow "future" class — whose isa then never matches the static class the
 * virtual-default-zone proxy carries, sending every typed malloc into the
 * infinite proxy redispatch. Ocerz answers from the cache's real table: the
 * objc_opt_t v16 i32 field at +0x20 (largeSharedCachesClassOffset) locates
 * dyld's objc::ClassHashTable {u32 version,capacity,occupied,shift,mask; u64
 * salt@+0x18; u32 scramble[256]@+0x20; u8 tab[mask+1]; u8 check[capacity];
 * i32 nameoffs[capacity] (table-relative); u64 objects[capacity]; u32 dupcnt;
 * u64 dups[]}. The perfect hash is Jenkins lookup8(name, salt) with index =
 * (val>>shift) ^ scramble[tab[val & mask]], validated by a checkbyte
 * (((name[0]&7)<<5)|len&0x1f) plus strcmp. Each object word packs {bit0
 * isDuplicate, bits1..47 offset-from-cache-base, bits48..63 headerInfoRW
 * dylib index}; duplicates point into the trailing dups run instead. The
 * block is invoked once per hit via nested ocerz_vm_call, isLoaded read from
 * the headerInfoRW entry's bit0 (objc itself marks images loaded there during
 * map_images), and a tiny guest scratch holds the stop flag.
 */
#include "ocerz/dyldapi.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/interp.h"

#include <sys/mman.h>
#include <string.h>
#include <mach-o/loader.h>

#define DYLDAPI_VTABLE_SIZE 0x2000

#define DYLDAPI_CLOSURE_MAX 1024

static uint64_t g_apis_global;
static struct OcerzCache *g_cache;
static uint64_t g_headeropt_rw;
static uint64_t g_headeropt_ro;
static uint64_t g_sel_pool;
static uint64_t g_clsopt;
static uint64_t g_block_scratch;
static uint64_t g_objc_make_mutable;
static uint64_t g_cache_start;
static uint64_t g_cache_size;
uint64_t g_main_path;

#define DYLDAPI_NOOP_OFF 0x2000
static uint64_t g_closure_mh[DYLDAPI_CLOSURE_MAX];
static int g_closure_n;

static uint64_t find_section_sz(uint64_t mh, const char *sect, uint64_t *size_out)
{
    const struct mach_header_64 *h = (const void *)(uintptr_t)mh;
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    uint64_t slide = 0;
    const uint8_t *q = lc;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)q;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)q;
            if (s->fileoff == 0 && s->vmaddr != 0) {
                slide = mh - s->vmaddr;
                break;
            }
        }
        q += l->cmdsize;
    }
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++)
                if (strncmp(sc[j].sectname, sect, 16) == 0) {
                    if (size_out)
                        *size_out = sc[j].size;
                    return sc[j].addr + slide;
                }
        }
        lc += l->cmdsize;
    }
    return 0;
}

static uint64_t find_section_any(uint64_t mh, const char *sect)
{
    return find_section_sz(mh, sect, NULL);
}

static uint64_t image_slide(uint64_t mh)
{
    const struct mach_header_64 *h = (const void *)(uintptr_t)mh;
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (s->fileoff == 0 && s->vmaddr != 0)
                return mh - s->vmaddr;
        }
        lc += l->cmdsize;
    }
    return 0;
}

static uint64_t cache_find_path(struct OcerzCache *cache, const char *path)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (mh && p && strcmp(p, path) == 0)
            return mh;
    }
    return 0;
}

static const char *cache_path_for_mh(struct OcerzCache *cache, uint64_t mh)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p = NULL;
        if (ocerz_cache_image_addr(cache, i, &p) == mh)
            return p;
    }
    return NULL;
}

static int closure_seen(uint64_t mh)
{
    for (int i = 0; i < g_closure_n; i++)
        if (g_closure_mh[i] == mh)
            return 1;
    return 0;
}

static void compute_closure(struct OcerzCache *cache, uint64_t main_mh)
{
    uint64_t queue[DYLDAPI_CLOSURE_MAX];
    int qn = 0;
    g_closure_n = 0;
    if (main_mh)
        queue[qn++] = main_mh;
    while (qn > 0 && g_closure_n < DYLDAPI_CLOSURE_MAX) {
        uint64_t mh = queue[--qn];
        if (closure_seen(mh))
            continue;
        const struct mach_header_64 *h = (const void *)(uintptr_t)mh;
        if (h->magic != MH_MAGIC_64)
            continue;
        g_closure_mh[g_closure_n++] = mh;
        const uint8_t *lc = (const uint8_t *)(h + 1);
        for (uint32_t i = 0; i < h->ncmds; i++) {
            const struct load_command *l = (const void *)lc;
            if (l->cmd == LC_LOAD_DYLIB || l->cmd == LC_LOAD_WEAK_DYLIB ||
                l->cmd == LC_REEXPORT_DYLIB || l->cmd == LC_LOAD_UPWARD_DYLIB) {
                uint32_t noff;
                memcpy(&noff, lc + 8, 4);
                if (noff < l->cmdsize) {
                    uint64_t dep = cache_find_path(cache, (const char *)lc + noff);
                    if (dep && qn < DYLDAPI_CLOSURE_MAX && !closure_seen(dep))
                        queue[qn++] = dep;
                }
            }
            lc += l->cmdsize;
        }
    }
}

int ocerz_dyldapi_setup(struct OcerzCache *cache)
{
    uint64_t fn = ocerz_cache_resolve(cache, "_dyld_get_active_platform");
    if (fn == 0)
        return OCERZ_EUNSUP;
    const uint8_t *p = (const uint8_t *)(uintptr_t)fn;
    if (p[0] != 0x55 || p[1] != 0x48 || p[2] != 0x89 || p[3] != 0xe5)
        return OCERZ_EUNSUP;
    if (p[4] != 0x48 || p[5] != 0x8b || p[6] != 0x3d)
        return OCERZ_EUNSUP;
    int32_t disp;
    memcpy(&disp, p + 7, 4);
    g_apis_global = fn + 11 + (int64_t)disp;

    uint64_t vtable = ocerz_map_anywhere(DYLDAPI_VTABLE_SIZE, PROT_READ | PROT_WRITE);
    uint64_t obj = ocerz_map_anywhere(0x400, PROT_READ | PROT_WRITE);
    if (vtable == 0 || obj == 0)
        return OCERZ_ENOMEM;
    for (uint64_t off = 0; off < DYLDAPI_VTABLE_SIZE; off += 8)
        ocerz_st(vtable + off, 8, OCERZ_DYLDAPI_LO + off);
    ocerz_st(obj, 8, vtable);
    ocerz_st(g_apis_global, 8, obj);

    g_cache = cache;
    g_cache_start = cache->base;
    g_cache_size = 0x40000000000ull;

    compute_closure(cache, ocerz_arena_lo);

    uint64_t objc_mh = 0;
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (mh && path && strcmp(path, "/usr/lib/libobjc.A.dylib") == 0) {
            objc_mh = mh;
            break;
        }
    }
    uint64_t opt = objc_mh ? find_section_any(objc_mh, "__objc_opt_ro") : 0;
    if (opt) {
        int32_t ro_off = 0, rw_off = 0, cls_off = 0;
        int64_t sel_off = 0;
        memcpy(&ro_off, (const void *)(uintptr_t)(opt + 0x0c), 4);
        memcpy(&rw_off, (const void *)(uintptr_t)(opt + 0x18), 4);
        memcpy(&cls_off, (const void *)(uintptr_t)(opt + 0x20), 4);
        memcpy(&sel_off, (const void *)(uintptr_t)(opt + 0x28), 8);
        g_headeropt_ro = ro_off ? opt + (int64_t)ro_off : 0;
        g_headeropt_rw = rw_off ? opt + (int64_t)rw_off : 0;
        g_clsopt = cls_off ? opt + (int64_t)cls_off : 0;
        g_sel_pool = sel_off ? opt + sel_off : 0;
    }

    g_block_scratch = ocerz_map_anywhere(0x40, PROT_READ | PROT_WRITE);

    g_objc_make_mutable = ocerz_map_anywhere(0x40, PROT_READ | PROT_WRITE);
    if (g_objc_make_mutable)
        for (uint64_t o = 0; o < 0x40; o += 8)
            ocerz_st(g_objc_make_mutable + o, 8, OCERZ_DYLDAPI_LO + DYLDAPI_NOOP_OFF);

    OCERZ_LOG("dyldapi: apis_global=%#llx obj=%#llx vtable=%#llx opt=%#llx rw=%#llx ro=%#llx\n",
              (unsigned long long)g_apis_global, (unsigned long long)obj,
              (unsigned long long)vtable, (unsigned long long)opt,
              (unsigned long long)g_headeropt_rw, (unsigned long long)g_headeropt_ro);
    return OCERZ_OK;
}

static void api_return(OcerzCPU *cpu, uint64_t result)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = result;
}

static int api_objc_register_callbacks(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t cb = cpu->gpr[OCERZ_RSI];
    uint64_t mapped = cb ? ocerz_ld(cb + 0x08, 8) : 0;
    if (!mapped || !g_cache) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }

    uint64_t mhs[DYLDAPI_CLOSURE_MAX], paths[DYLDAPI_CLOSURE_MAX], iis[DYLDAPI_CLOSURE_MAX];
    int n = 0;
    for (int i = 0; i < g_closure_n && n < DYLDAPI_CLOSURE_MAX; i++) {
        uint64_t mh = g_closure_mh[i];
        uint64_t ii = find_section_any(mh, "__objc_imageinfo");
        if (!ii)
            continue;
        const char *path = cache_path_for_mh(g_cache, mh);
        mhs[n] = mh;
        paths[n] = path ? ocerz_h2g(path) : 0;
        iis[n] = ii;
        n++;
    }
    if (n == 0) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }

    uint64_t infos = ocerz_map_anywhere((uint64_t)n * 0x20, PROT_READ | PROT_WRITE);
    if (!infos) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
    for (int k = 0; k < n; k++) {
        uint64_t e = infos + (uint64_t)k * 0x20;
        ocerz_st(e + 0x00, 8, mhs[k]);
        ocerz_st(e + 0x08, 8, iis[k]);
        ocerz_st(e + 0x10, 8, paths[k]);
        ocerz_st(e + 0x18, 8, 0);
    }

    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    uint64_t caller_ret = ocerz_ld(rsp, 8);
    uint64_t ret_rsp = rsp + 8;

    OCERZ_LOG("dyldapi: objc map_images driving %d images (infos=%#llx)\n",
              n, (unsigned long long)infos);

    uint64_t args[3] = { (uint64_t)n, infos, g_objc_make_mutable };
    ocerz_vm_call(vm, mapped, args, 3, ret_rsp);

    cpu->rip = caller_ret;
    cpu->gpr[OCERZ_RSP] = ret_rsp;
    cpu->gpr[OCERZ_RAX] = 0;
    return OCERZ_STEP_OK;
}

#define CLSHASH_MIX(a, b, c) \
    do { \
        a -= b; a -= c; a ^= (c >> 43); b -= c; b -= a; b ^= (a << 9);  c -= a; c -= b; c ^= (b >> 8);  \
        a -= b; a -= c; a ^= (c >> 38); b -= c; b -= a; b ^= (a << 23); c -= a; c -= b; c ^= (b >> 5);  \
        a -= b; a -= c; a ^= (c >> 35); b -= c; b -= a; b ^= (a << 49); c -= a; c -= b; c ^= (b >> 11); \
        a -= b; a -= c; a ^= (c >> 12); b -= c; b -= a; b ^= (a << 18); c -= a; c -= b; c ^= (b >> 22); \
    } while (0)

static uint64_t clshash_lookup8(const uint8_t *k, size_t length, uint64_t level)
{
    uint64_t a = level, b = level, c = 0x9e3779b97f4a7c13ull;
    size_t len = length;
    while (len >= 24) {
        a += (uint64_t)k[0] + ((uint64_t)k[1] << 8) + ((uint64_t)k[2] << 16) + ((uint64_t)k[3] << 24)
           + ((uint64_t)k[4] << 32) + ((uint64_t)k[5] << 40) + ((uint64_t)k[6] << 48) + ((uint64_t)k[7] << 56);
        b += (uint64_t)k[8] + ((uint64_t)k[9] << 8) + ((uint64_t)k[10] << 16) + ((uint64_t)k[11] << 24)
           + ((uint64_t)k[12] << 32) + ((uint64_t)k[13] << 40) + ((uint64_t)k[14] << 48) + ((uint64_t)k[15] << 56);
        c += (uint64_t)k[16] + ((uint64_t)k[17] << 8) + ((uint64_t)k[18] << 16) + ((uint64_t)k[19] << 24)
           + ((uint64_t)k[20] << 32) + ((uint64_t)k[21] << 40) + ((uint64_t)k[22] << 48) + ((uint64_t)k[23] << 56);
        CLSHASH_MIX(a, b, c);
        k += 24;
        len -= 24;
    }
    c += length;
    switch (len) {
    case 23: c += ((uint64_t)k[22] << 56);
    case 22: c += ((uint64_t)k[21] << 48);
    case 21: c += ((uint64_t)k[20] << 40);
    case 20: c += ((uint64_t)k[19] << 32);
    case 19: c += ((uint64_t)k[18] << 24);
    case 18: c += ((uint64_t)k[17] << 16);
    case 17: c += ((uint64_t)k[16] << 8);
    case 16: b += ((uint64_t)k[15] << 56);
    case 15: b += ((uint64_t)k[14] << 48);
    case 14: b += ((uint64_t)k[13] << 40);
    case 13: b += ((uint64_t)k[12] << 32);
    case 12: b += ((uint64_t)k[11] << 24);
    case 11: b += ((uint64_t)k[10] << 16);
    case 10: b += ((uint64_t)k[9] << 8);
    case 9:  b += (uint64_t)k[8];
    case 8:  a += ((uint64_t)k[7] << 56);
    case 7:  a += ((uint64_t)k[6] << 48);
    case 6:  a += ((uint64_t)k[5] << 40);
    case 5:  a += ((uint64_t)k[4] << 32);
    case 4:  a += ((uint64_t)k[3] << 24);
    case 3:  a += ((uint64_t)k[2] << 16);
    case 2:  a += ((uint64_t)k[1] << 8);
    case 1:  a += (uint64_t)k[0];
    }
    CLSHASH_MIX(a, b, c);
    return c;
}

#define CLSOPT_MAX_HITS 16

static int clsopt_lookup(const char *key, uint64_t *out, int max)
{
    if (!g_clsopt || !key || !key[0])
        return 0;
    const uint8_t *t = (const uint8_t *)(uintptr_t)g_clsopt;
    uint32_t capacity, shift, mask;
    uint64_t salt;
    memcpy(&capacity, t + 4, 4);
    memcpy(&shift, t + 12, 4);
    memcpy(&mask, t + 16, 4);
    memcpy(&salt, t + 24, 8);
    if (capacity == 0 || capacity > 0x1000000)
        return 0;
    size_t kl = strlen(key);
    uint64_t val = clshash_lookup8((const uint8_t *)key, kl, salt);
    uint64_t off_tab = 0x420;
    uint64_t off_check = off_tab + (uint64_t)mask + 1;
    uint64_t off_stroffs = off_check + capacity;
    uint64_t off_obj = off_stroffs + (uint64_t)capacity * 4;
    uint64_t off_dups = off_obj + (uint64_t)capacity * 8 + 4;
    uint32_t scr;
    memcpy(&scr, t + 0x20 + (uint64_t)t[off_tab + (val & mask)] * 4, 4);
    uint32_t h = (uint32_t)(shift >= 64 ? 0 : (val >> shift)) ^ scr;
    if (h >= capacity)
        return 0;
    uint8_t cb = (uint8_t)(((key[0] & 0x7) << 5) | ((uint8_t)kl & 0x1f));
    if (t[off_check + h] != cb)
        return 0;
    int32_t so;
    memcpy(&so, t + off_stroffs + (uint64_t)h * 4, 4);
    if (so == 0 || strcmp((const char *)t + so, key) != 0)
        return 0;
    uint64_t od;
    memcpy(&od, t + off_obj + (uint64_t)h * 8, 8);
    if (!(od & 1)) {
        out[0] = od;
        return 1;
    }
    uint64_t didx = (od >> 1) & ((1ull << 47) - 1);
    uint32_t dcnt = (uint32_t)(od >> 48);
    int n = 0;
    for (uint32_t i = 0; i < dcnt && n < max; i++) {
        uint64_t d;
        memcpy(&d, t + off_dups + (didx + i) * 8, 8);
        if (!(d & 1))
            out[n++] = d;
    }
    return n;
}

static uint64_t objc_index_loaded(uint32_t idx)
{
    if (!g_headeropt_rw)
        return 1;
    uint32_t count, entsize;
    memcpy(&count, (const void *)(uintptr_t)g_headeropt_rw, 4);
    memcpy(&entsize, (const void *)(uintptr_t)(g_headeropt_rw + 4), 4);
    if (entsize == 0 || entsize > 0x40 || idx >= count)
        return 0;
    return ocerz_ld(g_headeropt_rw + 8 + (uint64_t)idx * entsize, 8) & 1;
}

static int api_for_each_objc_class(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t name = cpu->gpr[OCERZ_RSI];
    uint64_t block = cpu->gpr[OCERZ_RDX];
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    uint64_t caller_ret = ocerz_ld(rsp, 8);
    uint64_t ret_rsp = rsp + 8;
    uint64_t invoke = block ? ocerz_ld(block + 0x10, 8) : 0;
    uint64_t hits[CLSOPT_MAX_HITS];
    int n = 0;
    if (name && invoke && g_block_scratch)
        n = clsopt_lookup((const char *)ocerz_g2h(name), hits, CLSOPT_MAX_HITS);
    OCERZ_LOG("dyldapi: for_each_objc_class \"%s\" -> %d hit(s)\n",
              name ? (const char *)ocerz_g2h(name) : "(null)", n);
    if (n == 0) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
    ocerz_st(g_block_scratch, 8, 0);
    for (int i = 0; i < n; i++) {
        uint64_t cls = g_cache_start + ((hits[i] >> 1) & ((1ull << 47) - 1));
        uint64_t loaded = objc_index_loaded((uint32_t)(hits[i] >> 48));
        uint64_t args[4] = { block, cls, loaded, g_block_scratch };
        ocerz_vm_call(vm, invoke, args, 4, ret_rsp);
        if (vm->exited)
            return OCERZ_STEP_OK;
        if (ocerz_ld(g_block_scratch, 1) & 1)
            break;
    }
    cpu->rip = caller_ret;
    cpu->gpr[OCERZ_RSP] = ret_rsp;
    cpu->gpr[OCERZ_RAX] = 0;
    return OCERZ_STEP_OK;
}

static int in_cache(uint64_t a)
{
    return a >= g_cache_start && a < g_cache_start + g_cache_size;
}

static uint64_t load_in_plain_list(uint64_t ml)
{
    if (!in_cache(ml))
        return 0;
    uint32_t ef = (uint32_t)ocerz_ld(ml, 4);
    uint32_t count = (uint32_t)ocerz_ld(ml + 4, 4);
    uint32_t entsize = ef & 0xfffcu;
    int rel = (ef & 0x80000000u) != 0;
    if (count == 0 || count > 65536 || entsize == 0 || entsize > 64)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t ent = ml + 8 + (uint64_t)i * entsize;
        uint64_t nameptr, imp;
        if (rel) {
            int32_t noff = (int32_t)(uint32_t)ocerz_ld(ent, 4);
            int32_t ioff = (int32_t)(uint32_t)ocerz_ld(ent + 8, 4);
            nameptr = g_sel_pool + (int64_t)noff;
            imp = ent + 8 + (int64_t)ioff;
        } else {
            nameptr = ocerz_ld(ent, 8);
            imp = ocerz_ld(ent + 16, 8);
        }
        if (nameptr && strcmp((const char *)ocerz_g2h(nameptr), "load") == 0)
            return imp;
    }
    return 0;
}

static uint64_t load_in_method_list(uint64_t raw)
{
    if (!raw)
        return 0;
    uint64_t ptr = raw & ~0x7ull;
    if (!in_cache(ptr))
        return 0;
    if (raw & 1) {
        uint32_t count = (uint32_t)ocerz_ld(ptr + 4, 4);
        if (count > 4096)
            return 0;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t ent = ptr + 8 + (uint64_t)i * 8;
            int64_t off = (int64_t)ocerz_ld(ent, 8) >> 16;
            uint64_t imp = load_in_plain_list((uint64_t)((int64_t)ent + off));
            if (imp)
                return imp;
        }
        return 0;
    }
    return load_in_plain_list(ptr);
}

static uint64_t metaclass_load_imp(uint64_t cls)
{
    if (!in_cache(cls))
        return 0;
    uint64_t meta = ocerz_ld(cls, 8) & 0x00007ffffffffff8ull;
    if (!in_cache(meta))
        return 0;
    uint64_t data = ocerz_ld(meta + 0x20, 8) & ~0x7ull;
    if (!in_cache(data))
        return 0;
    return load_in_method_list(ocerz_ld(data + 0x20, 8));
}

void ocerz_dyldapi_run_image_loads(struct OcerzVM *vm, uint64_t mh, uint64_t stack_top)
{
    if (!g_sel_pool)
        return;
    const char *imgpath = g_cache ? cache_path_for_mh(g_cache, mh) : NULL;
    OCERZ_LOG("loadphase: image %s (mh=%#llx)\n", imgpath ? imgpath : "?", (unsigned long long)mh);
    uint64_t size = 0;
    uint64_t nl = find_section_sz(mh, "__objc_nlclslist", &size);
    for (uint32_t i = 0; nl && i < (uint32_t)(size / 8); i++) {
        uint64_t cls = ocerz_ld(nl + (uint64_t)i * 8, 8) & ~0x1ull;
        uint64_t imp = metaclass_load_imp(cls);
        if (imp) {
            uint64_t a[2] = { cls, 0 };
            ocerz_vm_call(vm, imp, a, 2, stack_top);
            if (vm->exited)
                return;
        }
    }
    uint64_t csize = 0;
    uint64_t ncl = find_section_sz(mh, "__objc_nlcatlist", &csize);
    for (uint32_t i = 0; ncl && i < (uint32_t)(csize / 8); i++) {
        uint64_t cat = ocerz_ld(ncl + (uint64_t)i * 8, 8) & ~0x1ull;
        if (!in_cache(cat))
            continue;
        uint64_t imp = load_in_method_list(ocerz_ld(cat + 0x18, 8));
        if (imp) {
            uint64_t cls = ocerz_ld(cat + 0x08, 8);
            uint64_t a[2] = { cls, 0 };
            OCERZ_LOG("loadphase:   +load category imp=%#llx cls=%#llx\n",
                      (unsigned long long)imp, (unsigned long long)cls);
            ocerz_vm_call(vm, imp, a, 2, stack_top);
            if (vm->exited)
                return;
        }
    }
}

int ocerz_dyldapi_dispatch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t off = cpu->rip - OCERZ_DYLDAPI_LO;

    switch (off) {
    case DYLDAPI_NOOP_OFF:
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    case 0x210:
        api_return(cpu, 1);
        return OCERZ_STEP_OK;
    case 0x10:
        api_return(cpu, (uint64_t)g_closure_n);
        return OCERZ_STEP_OK;
    case 0x18: {
        uint32_t idx = (uint32_t)cpu->gpr[OCERZ_RSI];
        api_return(cpu, idx < (uint32_t)g_closure_n ? g_closure_mh[idx] : 0);
        return OCERZ_STEP_OK;
    }
    case 0x20: {
        uint32_t idx = (uint32_t)cpu->gpr[OCERZ_RSI];
        api_return(cpu, idx < (uint32_t)g_closure_n ? image_slide(g_closure_mh[idx]) : 0);
        return OCERZ_STEP_OK;
    }
    case 0x28: {
        uint32_t idx = (uint32_t)cpu->gpr[OCERZ_RSI];
        uint64_t name = 0;
        if (idx == 0 && g_main_path)
            name = g_main_path;
        else if (idx < (uint32_t)g_closure_n)
            name = (uint64_t)(uintptr_t)cache_path_for_mh(g_cache, g_closure_mh[idx]);
        api_return(cpu, name);
        return OCERZ_STEP_OK;
    }
    case 0x50: {
        uint64_t buf = cpu->gpr[OCERZ_RSI];
        uint64_t szp = cpu->gpr[OCERZ_RDX];
        if (!g_main_path) {
            api_return(cpu, (uint64_t)-1);
            return OCERZ_STEP_OK;
        }
        const char *host = (const char *)ocerz_g2h(g_main_path);
        uint32_t need = (uint32_t)strlen(host) + 1;
        uint32_t have = szp ? (uint32_t)ocerz_ld(szp, 4) : 0;
        if (buf && have >= need) {
            memcpy(ocerz_g2h(buf), host, need);
            if (szp)
                ocerz_st(szp, 4, need);
            api_return(cpu, 0);
        } else {
            if (szp)
                ocerz_st(szp, 4, need);
            api_return(cpu, (uint64_t)-1);
        }
        return OCERZ_STEP_OK;
    }
    case 0x160:
        api_return(cpu, image_slide(cpu->gpr[OCERZ_RSI]));
        return OCERZ_STEP_OK;
    case 0x168: {
        uint64_t addr = cpu->gpr[OCERZ_RSI];
        uint64_t path = 0;
        for (int i = 0; i < g_closure_n; i++) {
            uint64_t mh = g_closure_mh[i];
            const struct mach_header_64 *h = (const void *)(uintptr_t)mh;
            if (h->magic != MH_MAGIC_64)
                continue;
            uint64_t slide = image_slide(mh);
            const uint8_t *lc = (const uint8_t *)(h + 1);
            int hit = 0;
            for (uint32_t j = 0; j < h->ncmds && !hit; j++) {
                const struct load_command *l = (const void *)lc;
                if (l->cmd == LC_SEGMENT_64) {
                    const struct segment_command_64 *s = (const void *)lc;
                    uint64_t lo = s->vmaddr + slide;
                    if (s->vmsize && addr >= lo && addr < lo + s->vmsize)
                        hit = 1;
                }
                lc += l->cmdsize;
            }
            if (hit) {
                path = mh == ocerz_arena_lo ? g_main_path
                                            : (uint64_t)(uintptr_t)cache_path_for_mh(g_cache, mh);
                break;
            }
        }
        api_return(cpu, path);
        return OCERZ_STEP_OK;
    }
    case 0x2f0:
        api_return(cpu, ocerz_arena_lo);
        return OCERZ_STEP_OK;
    case 0x1f8: {
        uint64_t size_out = cpu->gpr[OCERZ_RSI];
        if (size_out)
            ocerz_st(size_out, 8, g_cache_size);
        api_return(cpu, g_cache_start);
        return OCERZ_STEP_OK;
    }
    case 0x2a8:
        return api_for_each_objc_class(vm, cpu);
    case 0x358:
        return api_objc_register_callbacks(vm, cpu);
    case 0x378: {
        /* _dyld_lookup_section_info(mach_header*, info_out, kind): returns the
         * named __TEXT/__DATA section's {addr in rax, size in rdx}. Kinds 0..5
         * are the Swift metadata sections, 6..17 the objc sections, in the
         * order of dyld's _dyld_section_location_kind enum. The Swift runtime's
         * conformance scanner walks every loaded image (including shared-cache
         * dylibs) asking for kind 1 (__swift5_proto, the protocol-conformance
         * records); without it `String: Hashable` is never found and generic
         * metadata such as _DictionaryStorage<String,Data> instantiates to NULL.
         * The objc kinds keep their pre-existing behaviour of reporting nothing
         * for shared-cache images so objc takes its preoptimized path instead of
         * re-walking the Mach-O. */
        static const char *const kindsect[] = {
            [0] = "__swift5_protos", [1] = "__swift5_proto",
            [2] = "__swift5_types",  [3] = "__swift5_replace",
            [4] = "__swift5_replac2", [5] = "__swift5_acfuncs",
            [6] = "__objc_imageinfo", [7] = "__objc_selrefs",
            [8] = "__objc_msgrefs",   [9] = "__objc_classrefs",
            [10] = "__objc_superrefs", [11] = "__objc_protorefs",
            [12] = "__objc_classlist", [13] = "__objc_nlclslist",
            [14] = "__objc_catlist",   [15] = "__objc_catlist2",
            [16] = "__objc_nlcatlist", [17] = "__objc_protolist",
        };
        uint64_t mh = cpu->gpr[OCERZ_RSI];
        uint64_t kind = cpu->gpr[OCERZ_RCX];
        uint64_t addr = 0, size = 0;
        int is_swift_kind = kind <= 5;
        if (mh && (is_swift_kind || mh < g_cache_start) &&
            kind < (sizeof kindsect / sizeof kindsect[0]) && kindsect[kind])
            addr = find_section_sz(mh, kindsect[kind], &size);
        cpu->gpr[OCERZ_RDX] = size;
        api_return(cpu, addr);
        return OCERZ_STEP_OK;
    }
    case 0x3a8:
        api_return(cpu, g_headeropt_rw);
        return OCERZ_STEP_OK;
    case 0x2a0: {
        uint64_t name = cpu->gpr[OCERZ_RSI];
        uint64_t result = 0;
        if (name && g_sel_pool) {
            const char *want = (const char *)ocerz_g2h(name);
            const char *p = (const char *)(uintptr_t)g_sel_pool;
            const char *end = p + 0x400000;
            while (p < end) {
                size_t l = strlen(p);
                if (l && strcmp(p, want) == 0) {
                    result = (uint64_t)(uintptr_t)p;
                    break;
                }
                p += l + 1;
            }
        }
        api_return(cpu, result);
        return OCERZ_STEP_OK;
    }
    case 0x3b0:
        api_return(cpu, g_headeropt_ro);
        return OCERZ_STEP_OK;
    default:
        OCERZ_LOG("dyldapi: unimplemented vtable slot +%#llx (this=%#llx a0=%#llx a1=%#llx a2=%#llx caller=%#llx)\n",
                  (unsigned long long)off, (unsigned long long)cpu->gpr[OCERZ_RDI],
                  (unsigned long long)cpu->gpr[OCERZ_RSI], (unsigned long long)cpu->gpr[OCERZ_RDX],
                  (unsigned long long)cpu->gpr[OCERZ_RCX],
                  (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8));
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
}
