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
 * dlopen/dlsym/dlclose/dlerror (slots 0x68/0x80/0x70/0x78). The libdyld dl*
 * wrappers tail-call these vtable slots after shuffling args down by one, so the
 * handlers see dlopen(path=rsi, mode=rdx), dlsym(handle=rsi, symbol=rdx),
 * dlclose(handle=rsi), dlerror() respectively. Slot 0x2e0 is the caller-aware
 * dlopen entry (the public dlopen() forwards path=rsi, mode=rdx, caller=rcx); it
 * shares the 0x68 handler since the loader seam ignores the caller address.
 * SoftLinking.framework (used by CoreFoundation/SkyLight soft-links) calls dlopen
 * through 0x2e0, so without it a soft-link dlopen returned 0 and SoftLinking ran
 * strlen() on a NULL dlerror() string. They marshal the guest pointers
 * through ocerz_g2h and call the loader seam in dyld.c (ocerz_dlopen / ocerz_
 * dlsym / ocerz_dlclose / ocerz_dlerror), which owns the disk-image registry and
 * fixup engines. dlopen runs AFTER main on the guest's live stack, and loading a
 * new image runs its initializers via a nested ocerz_vm_call (on a fresh scratch
 * stack chosen inside dyld.c) — that call clobbers the whole guest CPU, so the
 * dlopen handler snapshots *cpu before the seam call and restores it afterwards,
 * then api_returns the handle to the original caller. dlclose returns 0 without
 * unmapping (disk images and their export-trie buffers stay alive). dlerror
 * returns the guest pointer to the last error string or 0 (perl's XSLoader
 * tolerates 0, falling back to its own "Unknown dlopen error").
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
 * gets only the libSystem closure — both correct. Non-cache disk dylibs (loaded
 * by the mini-dyld, see dyld.c) register through ocerz_dyldapi_register_image,
 * which records {mach_header, guest-copy of the install path} and appends the
 * header to the closure (compute_closure re-appends them after its cache BFS),
 * so disk images appear in image lists (slots 0x10/0x18/0x20/0x28), resolve
 * their path (cache_path_for_mh consults the disk table first), drive objc
 * map_images when they carry __objc_imageinfo, and resolve unwind sections
 * (slot 0x170). Of that closure only images carrying __objc_imageinfo are
 * handed to objc. map_images_nolock then asks
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
 *
 * Preoptimized class count (slot 0x308 = _dyld_objc_class_count). libobjc sizes
 * the safety bound of its subclass-tree integrity walk
 * (foreach_realized_class_and_subclass) as
 * ((_dyld_objc_class_count() + realizedClassCount) << 4) | 0xf. Returning 0
 * here collapses the bound to 0xf=15, so a legitimate Swift-overlay class
 * subtree (e.g. __SwiftNativeNSArrayBase, dragged in by libswiftCore's
 * array-bridge lazy init under localizedCaseInsensitiveCompare:) with more than
 * 15 nodes exhausts the budget and objc false-fatals "Memory corruption in
 * class list." The class data is correct (objc realizes it normally); only the
 * bound is wrong. Ocerz answers with the cache's preoptimized class count = the
 * ClassHashTable `occupied` field at g_clsopt+8 (186065 on this cache), matching
 * real dyld, so the bound is ~2.97M and the walk completes.
 *
 * C++ exception unwinding (slot 0x170 = _dyld_find_unwind_sections(addr,
 * dyld_unwind_sections* out)). libunwind/libc++abi call this once per frame to
 * locate that frame's image and its compact-unwind (__TEXT,__unwind_info) and
 * DWARF CFI (__TEXT,__eh_frame) sections. The libdyld trampoline swaps the args
 * before dispatch, so the handler sees rsi=addr(pc), rdx=out-struct pointer.
 * Returning 0 with the struct empty makes libunwind believe no frame has unwind
 * info, so a throw never reaches its catch: it spins its no-info fallback (the
 * objc terminate handler rethrows back into __cxa_throw forever), exhausting the
 * stack until __chkstk_darwin reads off the bottom into the guard page (a SIGBUS
 * masquerading as a libunwind/pthread fault). image_for_pc() resolves the pc to
 * its image (closure first, then any shared-cache image so dylibs on the unwind
 * path resolve too) and find_section_sz() reads the real (slid) section ranges
 * into the 5-field out-struct {mh, eh_frame ptr/len, unwind_info ptr/len}.
 */
#include "ocerz/dyldapi.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/interp.h"
#include "ocerz/dyld.h"

#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#define DYLDAPI_VTABLE_SIZE 0x2000

#define DYLDAPI_CLOSURE_MAX 1024

static uint64_t g_apis_global;
static struct OcerzCache *g_cache;
static uint64_t g_headeropt_rw;
static uint64_t g_headeropt_ro;
static uint64_t g_sel_pool;
static uint64_t g_selopt;
static uint64_t g_clsopt;
static uint64_t g_protoopt;
static uint64_t g_block_scratch;
static uint64_t g_objc_make_mutable;
static uint64_t g_cache_start;
static uint64_t g_cache_size;
uint64_t g_main_path;

#define DYLDAPI_NOOP_OFF 0x2000
static uint64_t g_closure_mh[DYLDAPI_CLOSURE_MAX];
static int g_closure_n;

/* The objc-runtime _dyld_objc_notify_mapped callback, captured when the guest
 * objc registers it at boot (api_objc_register_callbacks). It is driven once
 * over the boot closure there; ocerz_dyldapi_objc_map_one re-drives it for ONE
 * image so a framework dlopen'd AFTER boot (e.g. AppKit soft-linking
 * ViewBridge.framework for NSServiceViewController) gets its objc classes
 * realized and its header-info "loaded" bit set -- exactly as dyld's
 * notifyObjCMapped does on every dlopen. Without this a lazily-loaded cache
 * framework's classes are found in the preopt table but reported isLoaded=0,
 * so NSClassFromString returns nil and AppKit asserts. */
static uint64_t g_objc_mapped_cb;
static uint64_t g_objc_dlopen_mapped[DYLDAPI_CLOSURE_MAX];
static int g_objc_dlopen_mapped_n;

/* The main program's LC_BUILD_VERSION triple, parsed once at setup. The
 * dyld_build_version_t availability APIs (_dyld_program_sdk_at_least and
 * friends) compare against these; answering them with a blanket "false" (the
 * old unimplemented-slot default) told every @available check in CF/Foundation/
 * AppKit that the program was built against an ancient SDK and flipped the
 * frameworks into legacy-compat behavior process-wide. */
static uint32_t g_main_bv_platform, g_main_bv_minos, g_main_bv_sdk;

static void parse_build_version(uint64_t mh, uint32_t *plat, uint32_t *minos, uint32_t *sdk)
{
    *plat = 0;
    *minos = 0;
    *sdk = 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
    if (!mh || h->magic != MH_MAGIC_64)
        return;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_BUILD_VERSION) {
            memcpy(plat, lc + 8, 4);
            memcpy(minos, lc + 12, 4);
            memcpy(sdk, lc + 16, 4);
            return;
        }
        if (l->cmd == LC_VERSION_MIN_MACOSX) {
            *plat = 1;
            memcpy(minos, lc + 8, 4);
            memcpy(sdk, lc + 12, 4);
            return;
        }
        lc += l->cmdsize;
    }
}

/* dyld_build_version_t arrives packed in one register: low 32 = platform, high
 * 32 = packed version (X<<16|Y<<8|Z). platform 0xffffffff is a dated "version
 * set"; a binary built with the current SDK satisfies every shipped set, so
 * answer true rather than decode dyld's date table. */
static uint64_t build_version_at_least(uint32_t plat, uint32_t have, uint64_t q)
{
    uint32_t qplat = (uint32_t)q;
    uint32_t qver = (uint32_t)(q >> 32);
    if (qplat == 0xffffffffu)
        return 1;
    if (qplat != plat)
        return 0;
    return have >= qver;
}

#define DYLDAPI_DISK_MAX 64
static uint64_t g_disk_mh[DYLDAPI_DISK_MAX];
static uint64_t g_disk_path[DYLDAPI_DISK_MAX];
static int g_disk_n;

void ocerz_dyldapi_register_image(uint64_t mh, const char *path)
{
    if (getenv("OCERZ_IMGLOG"))
        fprintf(stderr, "ocerz: IMGREG mh=%#llx path=%s\n",
                (unsigned long long)mh, (path && path[0]) ? path : "<NULL>");
    uint64_t gpath = 0;
    if (path && path[0]) {
        uint64_t need = (uint64_t)strlen(path) + 1;
        gpath = ocerz_map_anywhere(need, PROT_READ | PROT_WRITE);
        if (gpath)
            memcpy(ocerz_g2h(gpath), path, need);
    }
    for (int i = 0; i < g_disk_n; i++)
        if (g_disk_mh[i] == mh)
            return;
    if (g_disk_n < DYLDAPI_DISK_MAX) {
        g_disk_mh[g_disk_n] = mh;
        g_disk_path[g_disk_n] = gpath;
        g_disk_n++;
    }
    for (int i = 0; i < g_closure_n; i++)
        if (g_closure_mh[i] == mh)
            return;
    if (g_closure_n < DYLDAPI_CLOSURE_MAX)
        g_closure_mh[g_closure_n++] = mh;
}

static uint64_t disk_gpath_for_mh(uint64_t mh)
{
    for (int i = 0; i < g_disk_n; i++)
        if (g_disk_mh[i] == mh)
            return g_disk_path[i];
    return 0;
}

static const char *disk_path_for_mh(uint64_t mh)
{
    uint64_t g = disk_gpath_for_mh(mh);
    return g ? (const char *)ocerz_g2h(g) : NULL;
}

static uint64_t find_section_sz(uint64_t mh, const char *sect, uint64_t *size_out)
{
    const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    uint64_t slide = 0;
    const uint8_t *q = lc;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)q;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)q;
            if (s->fileoff == 0 && s->filesize != 0) {
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
    const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (s->fileoff == 0 && s->filesize != 0)
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

static int image_covers(uint64_t mh, uint64_t addr)
{
    const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    uint64_t slide = image_slide(mh);
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            uint64_t lo = s->vmaddr + slide;
            if (s->vmsize && addr >= lo && addr < lo + s->vmsize)
                return 1;
        }
        lc += l->cmdsize;
    }
    return 0;
}

static uint64_t image_for_pc(uint64_t pc)
{
    for (int i = 0; i < g_closure_n; i++)
        if (image_covers(g_closure_mh[i], pc))
            return g_closure_mh[i];
    if (g_cache)
        for (uint32_t i = 0; i < g_cache->images_cnt; i++) {
            uint64_t mh = ocerz_cache_image_addr(g_cache, i, NULL);
            if (mh && image_covers(mh, pc))
                return mh;
        }
    return 0;
}

/* dladdr's dli_sname/dli_saddr: the defined symbol with the greatest value not
 * past addr. The symbol table lives in __LINKEDIT (symoff/stroff are file
 * offsets, converted to load addresses through that segment's fileoff/vmaddr +
 * slide). Cache dylibs keep only a stripped local table in their split
 * linkedit, so the bounds check below simply yields no symbol there — dladdr is
 * still valid with dli_sname == NULL, matching its contract when nothing
 * matches. Leading '_' is dropped to report the C name as dyld does. */
static void image_nearest_symbol(uint64_t mh, uint64_t addr,
                                 uint64_t *sname_out, uint64_t *saddr_out)
{
    *sname_out = 0;
    *saddr_out = 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
    if (!mh || h->magic != MH_MAGIC_64)
        return;
    uint64_t slide = image_slide(mh);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint64_t le_vmaddr = 0, le_fileoff = 0, le_filesize = 0;
    int have_le = 0, have_sym = 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SYMTAB) {
            const struct symtab_command *s = (const void *)lc;
            symoff = s->symoff;
            nsyms = s->nsyms;
            stroff = s->stroff;
            strsize = s->strsize;
            have_sym = 1;
        } else if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (strcmp(s->segname, "__LINKEDIT") == 0) {
                le_vmaddr = s->vmaddr;
                le_fileoff = s->fileoff;
                le_filesize = s->filesize;
                have_le = 1;
            }
        }
        lc += l->cmdsize;
    }
    if (!have_sym || !have_le || !nsyms)
        return;
    if (symoff < le_fileoff || stroff < le_fileoff ||
        symoff + (uint64_t)nsyms * sizeof(struct nlist_64) > le_fileoff + le_filesize ||
        (uint64_t)stroff + strsize > le_fileoff + le_filesize)
        return;
    uint64_t symtab = le_vmaddr + slide + (symoff - le_fileoff);
    uint64_t strtab = le_vmaddr + slide + (stroff - le_fileoff);
    uint64_t best_val = 0, best_strx = 0;
    int found = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        const struct nlist_64 *n =
            (const struct nlist_64 *)ocerz_g2h(symtab + (uint64_t)i * sizeof(struct nlist_64));
        if (n->n_type & N_STAB)
            continue;
        if ((n->n_type & N_TYPE) != N_SECT)
            continue;
        if (n->n_un.n_strx == 0 || (uint32_t)n->n_un.n_strx >= strsize)
            continue;
        uint64_t val = n->n_value + slide;
        if (val > addr)
            continue;
        if (!found || val > best_val) {
            best_val = val;
            best_strx = n->n_un.n_strx;
            found = 1;
        }
    }
    if (!found)
        return;
    uint64_t namep = strtab + best_strx;
    if (*(const char *)ocerz_g2h(namep) == '_')
        namep += 1;
    *saddr_out = best_val;
    *sname_out = namep;
}

static const char *cache_path_for_mh(struct OcerzCache *cache, uint64_t mh)
{
    const char *dp = disk_path_for_mh(mh);
    if (dp)
        return dp;
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
        const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
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
    for (int i = 0; i < g_disk_n; i++)
        if (!closure_seen(g_disk_mh[i]) && g_closure_n < DYLDAPI_CLOSURE_MAX)
            g_closure_mh[g_closure_n++] = g_disk_mh[i];
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

    compute_closure(cache, ocerz_main_mh);

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
        int32_t selopt_off = 0, ro_off = 0, rw_off = 0, cls_off = 0, proto_off = 0;
        int64_t sel_off = 0;
        memcpy(&selopt_off, (const void *)(uintptr_t)(opt + 0x08), 4);
        memcpy(&ro_off, (const void *)(uintptr_t)(opt + 0x0c), 4);
        memcpy(&rw_off, (const void *)(uintptr_t)(opt + 0x18), 4);
        memcpy(&cls_off, (const void *)(uintptr_t)(opt + 0x20), 4);
        memcpy(&proto_off, (const void *)(uintptr_t)(opt + 0x24), 4);
        memcpy(&sel_off, (const void *)(uintptr_t)(opt + 0x28), 8);
        g_selopt = selopt_off ? opt + (int64_t)selopt_off : 0;
        g_headeropt_ro = ro_off ? opt + (int64_t)ro_off : 0;
        g_headeropt_rw = rw_off ? opt + (int64_t)rw_off : 0;
        g_clsopt = cls_off ? opt + (int64_t)cls_off : 0;
        g_protoopt = proto_off ? opt + (int64_t)proto_off : 0;
        g_sel_pool = sel_off ? opt + sel_off : 0;
    }

    parse_build_version(ocerz_main_mh, &g_main_bv_platform, &g_main_bv_minos, &g_main_bv_sdk);

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
    g_objc_mapped_cb = mapped;

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

    /* Record the images whose objc classes were just registered, so a later
     * dlopen of a NON-boot disk dylib (winemac.drv) is NOT mistaken for already
     * registered. ocerz_dyldapi_register_image appends every disk image to the
     * image-list closure (g_closure_mh) for the dyld image/unwind APIs, but that
     * closure is not the objc-registration set: only the images driven through
     * map_images here (and later through objc_map_one) have realized classes. */
    for (int k = 0; k < n; k++)
        if (g_objc_dlopen_mapped_n < (int)(sizeof g_objc_dlopen_mapped / sizeof g_objc_dlopen_mapped[0]))
            g_objc_dlopen_mapped[g_objc_dlopen_mapped_n++] = mhs[k];

    cpu->rip = caller_ret;
    cpu->gpr[OCERZ_RSP] = ret_rsp;
    cpu->gpr[OCERZ_RAX] = 0;
    return OCERZ_STEP_OK;
}

static int objc_image_already_loaded(uint64_t mh)
{
    for (int i = 0; i < g_objc_dlopen_mapped_n; i++)
        if (g_objc_dlopen_mapped[i] == mh)
            return 1;
    return 0;
}

/* Drive libobjc's map_images for a batch of images in one call (the boot path
 * does this for the whole launch closure; this is the same call, factored). */
static void objc_drive_map_images(struct OcerzVM *vm, const uint64_t *mhs, int n)
{
    if (!g_objc_mapped_cb || n <= 0)
        return;
    uint64_t infos = ocerz_map_anywhere((uint64_t)n * 0x20, PROT_READ | PROT_WRITE);
    uint64_t istk = ocerz_map_anywhere(0x100000, PROT_READ | PROT_WRITE);
    if (!infos || !istk)
        return;
    for (int k = 0; k < n; k++) {
        uint64_t e = infos + (uint64_t)k * 0x20;
        const char *path = cache_path_for_mh(g_cache, mhs[k]);
        ocerz_st(e + 0x00, 8, mhs[k]);
        ocerz_st(e + 0x08, 8, find_section_any(mhs[k], "__objc_imageinfo"));
        ocerz_st(e + 0x10, 8, path ? ocerz_h2g(path) : 0);
        ocerz_st(e + 0x18, 8, 0);
    }
    if (getenv("OCERZ_DLOPENLOG"))
        for (int k = 0; k < n; k++)
            fprintf(stderr, "ocerz: MAPIMG %s\n",
                    cache_path_for_mh(g_cache, mhs[k]) ? cache_path_for_mh(g_cache, mhs[k]) : "?");
    OCERZ_LOG("dyldapi: objc map_images (dlopen) %d image(s), first %s\n",
              n, cache_path_for_mh(g_cache, mhs[0]) ? cache_path_for_mh(g_cache, mhs[0]) : "?");
    uint64_t args[3] = { (uint64_t)n, infos, g_objc_make_mutable };
    ocerz_vm_call(vm, g_objc_mapped_cb, args, 3, istk + 0x100000 - 64);
}

/* When a cache framework is dlopen'd after boot, dyld brings up its WHOLE
 * not-yet-loaded dependency sub-closure and drives objc map_images over all of
 * them together -- so categories a private dependency vends (e.g. AppIntents'
 * +[NSXPCConnection ln_applicationServiceWithError:]) get attached, not just
 * the leaf framework's own classes. Reproduce that: BFS the LC_LOAD_DYLIB tree
 * from mh, collect every objc-bearing cache image not already loaded, and
 * map_images the batch once. An already-loaded image's subtree is itself
 * already loaded, so it is not re-walked. */
void ocerz_dyldapi_objc_map_one(struct OcerzVM *vm, uint64_t mh)
{
    if (getenv("OCERZ_OBJCLOG"))
        fprintf(stderr, "ocerz: OBJCMAP1 mh=%#llx cb=%d cache=%d already=%d\n",
                (unsigned long long)mh, g_objc_mapped_cb ? 1 : 0, g_cache ? 1 : 0,
                mh ? objc_image_already_loaded(mh) : -1);
    if (!g_objc_mapped_cb || !mh || !g_cache || objc_image_already_loaded(mh))
        return;
    uint64_t queue[DYLDAPI_CLOSURE_MAX], visited[DYLDAPI_CLOSURE_MAX], batch[DYLDAPI_CLOSURE_MAX];
    int qn = 0, vn = 0, bn = 0;
    queue[qn++] = mh;
    while (qn > 0 && bn < DYLDAPI_CLOSURE_MAX) {
        uint64_t cur = queue[--qn];
        int seen = 0;
        for (int i = 0; i < vn; i++)
            if (visited[i] == cur) { seen = 1; break; }
        if (seen)
            continue;
        if (vn < DYLDAPI_CLOSURE_MAX)
            visited[vn++] = cur;
        if (objc_image_already_loaded(cur))
            continue;
        const struct mach_header_64 *h = (const void *)(uintptr_t)cur;
        if (h->magic != MH_MAGIC_64)
            continue;
        if (find_section_any(cur, "__objc_imageinfo") && bn < DYLDAPI_CLOSURE_MAX)
            batch[bn++] = cur;
        const uint8_t *lc = (const uint8_t *)(h + 1);
        for (uint32_t i = 0; i < h->ncmds; i++) {
            const struct load_command *l = (const void *)lc;
            if (l->cmd == LC_LOAD_DYLIB || l->cmd == LC_LOAD_WEAK_DYLIB ||
                l->cmd == LC_REEXPORT_DYLIB || l->cmd == LC_LOAD_UPWARD_DYLIB) {
                uint32_t noff;
                memcpy(&noff, lc + 8, 4);
                if (noff < l->cmdsize) {
                    uint64_t dep = cache_find_path(g_cache, (const char *)lc + noff);
                    if (dep && qn < DYLDAPI_CLOSURE_MAX)
                        queue[qn++] = dep;
                }
            }
            lc += l->cmdsize;
        }
    }
    if (getenv("OCERZ_OBJCLOG")) {
        fprintf(stderr, "ocerz: OBJCMAP mh=%#llx batch=%d path=%s\n",
                (unsigned long long)mh, bn,
                cache_path_for_mh(g_cache, mh) ? cache_path_for_mh(g_cache, mh) : "?");
        for (int k = 0; k < bn; k++) {
            const char *p = cache_path_for_mh(g_cache, batch[k]);
            fprintf(stderr, "ocerz:   batch[%d] mh=%#llx %s\n", k,
                    (unsigned long long)batch[k], p ? p : "?");
        }
    }
    if (bn == 0)
        return;
    for (int k = 0; k < bn; k++)
        if (g_objc_dlopen_mapped_n < (int)(sizeof g_objc_dlopen_mapped / sizeof g_objc_dlopen_mapped[0]))
            g_objc_dlopen_mapped[g_objc_dlopen_mapped_n++] = batch[k];
    objc_drive_map_images(vm, batch, bn);
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

/* Perfect-hash lookup over a shared-cache objc_stringhash_t table (the layout
 * both the class table and the protocol table use: header, salt, scramble[256],
 * tab[mask+1], checkbytes[capacity], string offsets, 64-bit object words, then
 * a duplicates array). Returns the raw object words; the caller decodes them
 * ((value<<1|dup-flag) with the owning dylib's header index in the top bits). */
static int stringhash_find_raw(uint64_t table, const char *key, uint64_t *od_out, uint64_t *dups_out)
{
    if (!table || !key || !key[0])
        return 0;
    const uint8_t *t = (const uint8_t *)(uintptr_t)table;
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
    memcpy(od_out, t + off_obj + (uint64_t)h * 8, 8);
    *dups_out = table + off_dups;
    return 1;
}

static int stringhash_lookup(uint64_t table, const char *key, uint64_t *out, int max)
{
    uint64_t od = 0, dups = 0;
    if (!stringhash_find_raw(table, key, &od, &dups))
        return 0;
    if (!(od & 1)) {
        out[0] = od;
        return 1;
    }
    uint64_t didx = (od >> 1) & ((1ull << 47) - 1);
    uint32_t dcnt = (uint32_t)(od >> 48);
    int n = 0;
    for (uint32_t i = 0; i < dcnt && n < max; i++) {
        uint64_t d;
        memcpy(&d, (const void *)(uintptr_t)(dups + (didx + i) * 8), 8);
        if (!(d & 1))
            out[n++] = d;
    }
    return n;
}

static int clsopt_lookup(const char *key, uint64_t *out, int max)
{
    return stringhash_lookup(g_clsopt, key, out, max);
}

/* _dyld_get_objc_selector must return the cache's CANONICAL pointer for a
 * selector name so it pointer-equals the (relative) selector references baked
 * into cache method lists; otherwise sel_registerName mints a fresh selector
 * and protocol/method lookups by SEL silently miss (the NSXPCInterface protocol
 * method-signature path, e.g. -[...WMXPCServerInterface retrieveStagesWithReply:]).
 * The selector string pool at relativeMethodSelectorBase holds every canonical
 * selector once; a linear scan with a fixed cap missed selectors stored past
 * the cap, so build a one-shot open-addressing string->pointer index over the
 * whole pool and look up O(1).
 *
 * Building is serialized by g_selidx_lock and double-checked: a real process
 * resolves selectors from several guest threads at once (the first ObjC use in a
 * re-exec'd Wine child races worker threads), and without the lock each entrant
 * sees g_selidx still NULL, builds its own table, and stomps the half-built
 * shared one — readers then walk a torn table and the open-addressing probe
 * never terminates (a 100% CPU hang). g_selidx is published last, after the
 * table is fully populated, so a thread that observes it non-NULL sees a
 * complete index. The probe bound is belt-and-suspenders against a full table. */
static const char **g_selidx;
static uint32_t g_selidx_cap;
static pthread_mutex_t g_selidx_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void selpool_build(void)
{
    if (g_selidx || !g_sel_pool)
        return;
    pthread_mutex_lock(&g_selidx_lock);
    if (g_selidx || !g_sel_pool) {
        pthread_mutex_unlock(&g_selidx_lock);
        return;
    }
    const char *base = (const char *)(uintptr_t)g_sel_pool;
    const char *cap_end = base + 0x10000000;
    uint64_t count = 0;
    int zeros = 0;
    const char *p = base, *pool_end = base;
    while (p < cap_end) {
        if (*p == 0) {
            if (++zeros > 256)
                break;
            p++;
            continue;
        }
        zeros = 0;
        count++;
        p += strlen(p) + 1;
        pool_end = p;
    }
    uint32_t cap = 1;
    while ((uint64_t)cap < count * 2 + 16)
        cap <<= 1;
    const char **idx = (const char **)calloc(cap, sizeof(const char *));
    if (!idx) {
        pthread_mutex_unlock(&g_selidx_lock);
        return;
    }
    p = base;
    while (p < pool_end) {
        if (*p == 0) {
            p++;
            continue;
        }
        size_t l = strlen(p);
        uint32_t h = (uint32_t)(fnv1a(p, l) & (cap - 1));
        uint32_t probes = 0;
        while (idx[h] && ++probes < cap)
            h = (h + 1) & (cap - 1);
        idx[h] = p;
        p += l + 1;
    }
    g_selidx_cap = cap;
    g_selidx = idx;
    pthread_mutex_unlock(&g_selidx_lock);
}

/* Fast _dyld_get_objc_selector: look the name up in the shared cache's
 * precomputed objc selector hash (objc_selopt_t at objc_opt_t+0x08). It is the
 * same perfect-hash format the class table uses (stringhash_find_raw), but the
 * selector table ends at the string-offset array -- no object-word/duplicate
 * arrays -- and the canonical selector pointer IS the hashed string itself,
 * table + offsets[h]. O(1), needs no build. This replaces the ~11s open-
 * addressing index over the 2.5M-entry relativeMethodSelectorBase pool, which
 * was blowing past winemac.drv's 5s Cocoa-startup deadline. The returned pointer
 * is the cache's canonical selector (deduped into one pool), so it pointer-equals
 * the selectors baked into the cache's relative method lists, which is what the
 * caller needs. A name absent from the cache returns 0 (objc then mints its own). */
static uint64_t selopt_canonical(const char *want)
{
    if (!g_selopt || !want || !want[0])
        return 0;
    const uint8_t *t = (const uint8_t *)(uintptr_t)g_selopt;
    uint32_t capacity, shift, mask;
    uint64_t salt;
    memcpy(&capacity, t + 4, 4);
    memcpy(&shift, t + 12, 4);
    memcpy(&mask, t + 16, 4);
    memcpy(&salt, t + 24, 8);
    if (capacity == 0 || capacity > 0x1000000)
        return 0;
    size_t kl = strlen(want);
    uint64_t val = clshash_lookup8((const uint8_t *)want, kl, salt);
    uint64_t off_tab = 0x420;
    uint64_t off_check = off_tab + (uint64_t)mask + 1;
    uint64_t off_stroffs = off_check + capacity;
    uint32_t scr;
    memcpy(&scr, t + 0x20 + (uint64_t)t[off_tab + (val & mask)] * 4, 4);
    uint32_t h = (uint32_t)(shift >= 64 ? 0 : (val >> shift)) ^ scr;
    if (h >= capacity)
        return 0;
    uint8_t cb = (uint8_t)(((want[0] & 0x7) << 5) | ((uint8_t)kl & 0x1f));
    if (t[off_check + h] != cb)
        return 0;
    int32_t so;
    memcpy(&so, t + off_stroffs + (uint64_t)h * 4, 4);
    if (so == 0)
        return 0;
    uint64_t canon = g_selopt + (uint64_t)(int64_t)so;
    if (strcmp((const char *)(uintptr_t)canon, want) != 0)
        return 0;
    return canon;
}

static uint64_t selpool_canonical(const char *want)
{
    if (!want || !want[0])
        return 0;
    if (g_selopt) {
        uint64_t r = selopt_canonical(want);
        if (getenv("OCERZ_SELVERIFY") && g_sel_pool) {
            if (!g_selidx) selpool_build();
            uint64_t b = 0;
            if (g_selidx) {
                size_t wl = strlen(want);
                uint32_t hh = (uint32_t)(fnv1a(want, wl) & (g_selidx_cap - 1));
                while (g_selidx[hh]) {
                    if (strcmp(g_selidx[hh], want) == 0) { b = (uint64_t)(uintptr_t)g_selidx[hh]; break; }
                    hh = (hh + 1) & (g_selidx_cap - 1);
                }
            }
            if (b != r)
                fprintf(stderr, "ocerz: SELVERIFY MISMATCH \"%s\" selopt=%#llx build=%#llx\n",
                        want, (unsigned long long)r, (unsigned long long)b);
        }
        return r;
    }
    if (!g_sel_pool)
        return 0;
    if (!g_selidx)
        selpool_build();
    if (!g_selidx)
        return 0;
    size_t wl = strlen(want);
    uint32_t h = (uint32_t)(fnv1a(want, wl) & (g_selidx_cap - 1));
    while (g_selidx[h]) {
        if (strcmp(g_selidx[h], want) == 0)
            return (uint64_t)(uintptr_t)g_selidx[h];
        h = (h + 1) & (g_selidx_cap - 1);
    }
    return 0;
}

/* Public: the shared cache's canonical selector pointer for a name string (host
 * pointer), or 0 if the cache has no such selector. The mini-dyld uses this to
 * canonicalize an arena dylib's __objc_selrefs at load -- modern objc, on a
 * shared-cache launch, assumes dyld already uniqued every selector reference and
 * skips its own per-image selref fixup, so a selref left pointing at the image's
 * local __objc_methname string is a DISTINCT pointer from the cache's canonical
 * selector and fails objc_msgSend's pointer-equality method match (the
 * "selector ... does not match selector known to Objective C runtime" abort). */
uint64_t ocerz_dyldapi_canonical_selector(const char *name)
{
    return selpool_canonical(name);
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
        if (getenv("OCERZ_CLSLOG") && name && strstr((const char *)ocerz_g2h(name), "NSString"))
            fprintf(stderr, "ocerz: CLSLOOKUP[%d] \"%s\" cls=%#llx imgidx=%u loaded=%llu\n",
                    (int)getpid(), (const char *)ocerz_g2h(name), (unsigned long long)cls,
                    (unsigned)(hits[i] >> 48), (unsigned long long)loaded);
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

/* _dyld_for_each_objc_protocol: the protocol analog of for_each_objc_class
 * (libobjc's getPreoptimizedProtocol drives NSProtocolFromString,
 * protocol_getMethodDescription, NSXPCInterface...). Same stringhash table
 * shape, but the duplicate runs are huge (NSObject appears in thousands of
 * images), so iterate the run inline invoking the block per entry instead of
 * collecting into a fixed array; libobjc's block stops at the first entry
 * whose image is loaded. */
static int api_for_each_objc_protocol(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t name = cpu->gpr[OCERZ_RSI];
    uint64_t block = cpu->gpr[OCERZ_RDX];
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    uint64_t caller_ret = ocerz_ld(rsp, 8);
    uint64_t ret_rsp = rsp + 8;
    uint64_t invoke = block ? ocerz_ld(block + 0x10, 8) : 0;
    uint64_t od = 0, dups = 0;
    int found = 0;
    if (name && invoke && g_block_scratch)
        found = stringhash_find_raw(g_protoopt, (const char *)ocerz_g2h(name), &od, &dups);
    OCERZ_LOG("dyldapi: for_each_objc_protocol \"%s\" -> %s\n",
              name ? (const char *)ocerz_g2h(name) : "(null)", found ? "hit" : "miss");
    if (!found) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
    ocerz_st(g_block_scratch, 8, 0);
    uint64_t run_base = 0, run_cnt = 1;
    if (od & 1) {
        run_base = dups + ((od >> 1) & ((1ull << 47) - 1)) * 8;
        run_cnt = od >> 48;
    }
    for (uint64_t i = 0; i < run_cnt; i++) {
        uint64_t d = od;
        if (od & 1) {
            memcpy(&d, (const void *)(uintptr_t)(run_base + i * 8), 8);
            if (d & 1)
                continue;
        }
        uint64_t proto = g_cache_start + ((d >> 1) & ((1ull << 47) - 1));
        uint64_t loaded = objc_index_loaded((uint32_t)(d >> 48));
        uint64_t args[4] = { block, proto, loaded, g_block_scratch };
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

static int meth_list_has(uint64_t ml, const char *want)
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
        uint64_t nameptr;
        if (rel) {
            int32_t noff = (int32_t)(uint32_t)ocerz_ld(ent, 4);
            nameptr = g_sel_pool + (int64_t)noff;
        } else {
            nameptr = ocerz_ld(ent, 8);
        }
        if (nameptr && strcmp((const char *)ocerz_g2h(nameptr), want) == 0)
            return 1;
    }
    return 0;
}

static void meth_list_print_substr(uint64_t ml, const char *substr)
{
    if (!in_cache(ml))
        return;
    uint32_t ef = (uint32_t)ocerz_ld(ml, 4);
    uint32_t count = (uint32_t)ocerz_ld(ml + 4, 4);
    uint32_t entsize = ef & 0xfffcu;
    int rel = (ef & 0x80000000u) != 0;
    if (count == 0 || count > 65536 || entsize == 0 || entsize > 64)
        return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t ent = ml + 8 + (uint64_t)i * entsize;
        uint64_t nameptr = rel ? g_sel_pool + (int64_t)(int32_t)(uint32_t)ocerz_ld(ent, 4)
                               : ocerz_ld(ent, 8);
        if (!nameptr) continue;
        const char *nm = (const char *)ocerz_g2h(nameptr);
        if (strstr(nm, substr)) {
            uint64_t imp = rel ? (uint64_t)((int64_t)(ent + 8) + (int64_t)(int32_t)(uint32_t)ocerz_ld(ent + 8, 4))
                               : ocerz_ld(ent + 16, 8);
            fprintf(stderr, "ocerz:       method \"%s\" imp=%#llx\n", nm, (unsigned long long)imp);
        }
    }
}

/* OCERZ_METHDUMP diagnostic: for class `cls`, walk its METACLASS's static
 * baseMethods (a shared-cache relative-list-list of per-image sub-lists) and
 * report, for each sub-list, its image index, that image's objc "loaded" bit
 * (objc excludes a sub-list whose image is not loaded when it realizes the
 * class), and whether the sub-list defines `sel`. Pins down whether a cluster
 * override (e.g. +allocWithZone:) is being dropped because its image is unloaded
 * at realize time. */
void ocerz_dyldapi_dump_method(uint64_t cls, const char *sel)
{
    if (!in_cache(cls) || !g_sel_pool) {
        fprintf(stderr, "ocerz: METHDUMP cls=%#llx not-in-cache or no selpool\n",
                (unsigned long long)cls);
        return;
    }
    uint64_t meta = ocerz_ld(cls, 8) & 0x00007ffffffffff8ull;
    uint64_t data = ocerz_ld(meta + 0x20, 8) & ~0x7ull;
    uint64_t ro = data;
    uint32_t dflags = (uint32_t)ocerz_ld(data, 4);
    int realized = (dflags & 0x80000000u) != 0;
    if (realized) {
        uint64_t roe = ocerz_ld(data + 0x08, 8);
        ro = (roe & 1) ? ocerz_ld((roe & ~1ull), 8) : roe;
    }
    uint64_t raw = ocerz_ld(ro + 0x20, 8);
    fprintf(stderr, "ocerz: METHDUMP cls=%#llx meta=%#llx realized=%d ro=%#llx baseMethods=%#llx sel=%s\n",
            (unsigned long long)cls, (unsigned long long)meta, realized,
            (unsigned long long)ro, (unsigned long long)raw, sel);
    uint64_t ptr = raw & ~0x7ull;
    if (!in_cache(ptr)) {
        fprintf(stderr, "ocerz:   baseMethods not in cache\n");
        return;
    }
    if (raw & 1) {
        uint32_t count = (uint32_t)ocerz_ld(ptr + 4, 4);
        if (count > 4096) count = 4096;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t ent = ptr + 8 + (uint64_t)i * 8;
            uint64_t rawent = ocerz_ld(ent, 8);
            int64_t off = (int64_t)rawent >> 16;
            uint32_t imgidx = (uint32_t)(rawent & 0xffff);
            uint64_t sublist = (uint64_t)((int64_t)ent + off);
            uint64_t loaded = objc_index_loaded(imgidx);
            int has = meth_list_has(sublist, sel);
            fprintf(stderr, "ocerz:   sub[%u] list=%#llx imgidx=%u loaded=%llu has(%s)=%d%s\n",
                    i, (unsigned long long)sublist, imgidx, (unsigned long long)loaded,
                    sel, has, (has && !loaded) ? "  <== DROPPED (has sel but image unloaded)" : "");
            meth_list_print_substr(sublist, "allocWithZone:");
        }
    } else {
        fprintf(stderr, "ocerz:   single list has(%s)=%d\n", sel, meth_list_has(ptr, sel));
    }
    /* +[NSString allocWithZone:] (Foundation IMP 0x7ff8040b450a) returns the
     * placeholder only when (self == *0x7ff8436dad48); that global is a runtime-
     * fixed-up NSString class reference (0 in the static cache, == NSString in an
     * isolated ocerz run). Read it here to see if the Wine process fixed it up. */
    uint64_t g = 0x7ff8436dad48ull;
    fprintf(stderr, "ocerz:   ALLOCGLOBAL[%#llx]=%#llx  cls=%#llx  match=%d\n",
            (unsigned long long)g, (unsigned long long)ocerz_ld(g, 8),
            (unsigned long long)cls, ocerz_ld(g, 8) == cls);
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
    case 0x238:
        api_return(cpu, build_version_at_least(g_main_bv_platform, g_main_bv_sdk,
                                               cpu->gpr[OCERZ_RSI]));
        return OCERZ_STEP_OK;
    case 0x240:
        api_return(cpu, build_version_at_least(g_main_bv_platform, g_main_bv_minos,
                                               cpu->gpr[OCERZ_RSI]));
        return OCERZ_STEP_OK;
    case 0x228:
    case 0x230: {
        uint32_t plat, minos, sdk;
        parse_build_version(cpu->gpr[OCERZ_RSI], &plat, &minos, &sdk);
        api_return(cpu, build_version_at_least(plat, off == 0x228 ? sdk : minos,
                                               cpu->gpr[OCERZ_RDX]));
        return OCERZ_STEP_OK;
    }
    case 0x198:
    case 0x1a0:
    case 0x328:
    case 0x360:
    case 0x3c8:
    case 0x3d0:
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    case 0x1e8: {
        uint64_t addr = cpu->gpr[OCERZ_RSI];
        uint64_t len = cpu->gpr[OCERZ_RDX];
        api_return(cpu, in_cache(addr) && in_cache(addr + len));
        return OCERZ_STEP_OK;
    }
    case 0x318:
    case 0x320:
    case 0x368:
    case 0x370:
        cpu->gpr[OCERZ_RDX] = 0;
        api_return(cpu, 2);
        return OCERZ_STEP_OK;
    case 0x2b0:
        return api_for_each_objc_protocol(vm, cpu);
    case 0x2d8:
        api_return(cpu, cpu->gpr[OCERZ_RSI] && g_cache &&
                        cache_find_path(g_cache, (const char *)ocerz_g2h(cpu->gpr[OCERZ_RSI])) != 0);
        return OCERZ_STEP_OK;
    case 0x68:
    case 0x2e0: {
        uint64_t pathg = cpu->gpr[OCERZ_RSI];
        uint64_t mode = cpu->gpr[OCERZ_RDX];
        const char *host = pathg ? (const char *)ocerz_g2h(pathg) : NULL;
        if (host && getenv("OCERZ_DLBT") && strstr(host, getenv("OCERZ_DLBT"))) {
            fprintf(stderr, "ocerz: DLBT dlopen(\"%s\") caller-chain:", host);
            uint64_t fp = cpu->gpr[OCERZ_RBP];
            for (int d = 0; d < 14 && fp >= 0x300000000ull; d++) {
                fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
                uint64_t nf = ocerz_ld(fp, 8);
                if (nf <= fp) break;
                fp = nf;
            }
            fprintf(stderr, "\n");
        }
        OcerzCPU saved = *cpu;
        uint64_t h = ocerz_dlopen(vm, host, (int)mode);
        *cpu = saved;
        if (vm->exited)
            return OCERZ_STEP_OK;
        api_return(cpu, h);
        return OCERZ_STEP_OK;
    }
    case 0x80: {
        uint64_t handle = cpu->gpr[OCERZ_RSI];
        uint64_t symg = cpu->gpr[OCERZ_RDX];
        uint64_t addr = 0;
        if (symg)
            addr = ocerz_dlsym(handle, (const char *)ocerz_g2h(symg));
        api_return(cpu, addr);
        return OCERZ_STEP_OK;
    }
    case 0x70:
        api_return(cpu, (uint64_t)ocerz_dlclose(cpu->gpr[OCERZ_RSI]));
        return OCERZ_STEP_OK;
    case 0x78:
        api_return(cpu, ocerz_dlerror());
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
            const struct mach_header_64 *h = (const struct mach_header_64 *)ocerz_g2h(mh);
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
                path = mh == ocerz_main_mh ? g_main_path
                                           : (uint64_t)(uintptr_t)cache_path_for_mh(g_cache, mh);
                break;
            }
        }
        api_return(cpu, path);
        return OCERZ_STEP_OK;
    }
    case 0x170: {
        uint64_t pc = cpu->gpr[OCERZ_RSI];
        uint64_t info = cpu->gpr[OCERZ_RDX];
        uint64_t mh = image_for_pc(pc);
        if (mh && info) {
            uint64_t eh_sz = 0, cu_sz = 0;
            uint64_t eh = find_section_sz(mh, "__eh_frame", &eh_sz);
            uint64_t cu = find_section_sz(mh, "__unwind_info", &cu_sz);
            ocerz_st(info + 0x00, 8, mh);
            ocerz_st(info + 0x08, 8, eh);
            ocerz_st(info + 0x10, 8, eh ? eh_sz : 0);
            ocerz_st(info + 0x18, 8, cu);
            ocerz_st(info + 0x20, 8, cu ? cu_sz : 0);
            if (getenv("OCERZ_UNWLOG"))
                fprintf(stderr, "ocerz: UNWLOG pc=%#llx mh=%#llx eh=%#llx eh_sz=%#llx cu=%#llx cu_sz=%#llx\n",
                        (unsigned long long)pc, (unsigned long long)mh,
                        (unsigned long long)eh, (unsigned long long)eh_sz,
                        (unsigned long long)cu, (unsigned long long)cu_sz);
        } else if (getenv("OCERZ_UNWLOG")) {
            fprintf(stderr, "ocerz: UNWLOG pc=%#llx mh=0 (no image)\n", (unsigned long long)pc);
        }
        api_return(cpu, mh ? 1 : 0);
        return OCERZ_STEP_OK;
    }
    case 0x1b0:
        api_return(cpu, image_for_pc(cpu->gpr[OCERZ_RSI]));
        return OCERZ_STEP_OK;
    case 0x60: {
        uint64_t addr = cpu->gpr[OCERZ_RSI];
        uint64_t info = cpu->gpr[OCERZ_RDX];
        uint64_t mh = image_for_pc(addr);
        if (!mh || !info) {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        uint64_t fname = mh == ocerz_main_mh
                             ? g_main_path
                             : (uint64_t)(uintptr_t)cache_path_for_mh(g_cache, mh);
        uint64_t sname = 0, saddr = 0;
        image_nearest_symbol(mh, addr, &sname, &saddr);
        ocerz_st(info + 0x00, 8, fname);
        ocerz_st(info + 0x08, 8, mh);
        ocerz_st(info + 0x10, 8, sname);
        ocerz_st(info + 0x18, 8, saddr);
        api_return(cpu, 1);
        return OCERZ_STEP_OK;
    }
    case 0x2f0:
        api_return(cpu, ocerz_main_mh);
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
    case 0x308: {
        uint32_t occupied = 0;
        if (g_clsopt)
            memcpy(&occupied, (const void *)(uintptr_t)(g_clsopt + 8), 4);
        api_return(cpu, occupied);
        return OCERZ_STEP_OK;
    }
    case 0x3a8:
        api_return(cpu, g_headeropt_rw);
        return OCERZ_STEP_OK;
    case 0x2a0: {
        uint64_t name = cpu->gpr[OCERZ_RSI];
        uint64_t result = name ? selpool_canonical((const char *)ocerz_g2h(name)) : 0;
        if (!result && name && getenv("OCERZ_SELLOG"))
            fprintf(stderr, "ocerz: SELMISS \"%s\"\n", (const char *)ocerz_g2h(name));
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
