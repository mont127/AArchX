/*
 * src/dyld.c
 *
 * Ocerz's mini-dyld implementation (dyld.h): load, link and launch a
 * dynamically-linked x86_64 LC_MAIN executable against the shared cache,
 * doing the work Apple's dyld would, so no real dyld runs.
 *
 * Flow of ocerz_dyld_run:
 *  1. Identity memory mode (guest_base = 0) with a reserved low arena for the
 *     executable, stack and heap; the shared cache is mapped separately at its
 *     own absolute addresses by ocerz_cache_map.
 *  2. Read the executable, require MH_PIE, and choose a slide so its lowest
 *     real segment (the fileoff==0 __TEXT) lands at LOAD_BASE. Map every
 *     LC_SEGMENT_64 (skipping __PAGEZERO) at vmaddr+slide, pread its file
 *     bytes, and apply its protections (guest exec dropped to read).
 *  3. Apply fixups. Modern binaries carry LC_DYLD_CHAINED_FIXUPS: the fixups
 *     metadata (header, starts, imports, symbol strings) is read straight from
 *     the file image and the chains themselves are walked in the now-mapped
 *     segments. For the DYLD_CHAINED_PTR_64 / _64_OFFSET formats each chain slot
 *     is either a rebase (an internal pointer: load_base + target for _OFFSET,
 *     or vmaddr + slide for the plain _64 form, OR'd with any high8 byte) or a
 *     bind (an import: the 24-bit ordinal selects an entry in the imports table
 *     whose symbol name is resolved with ocerz_cache_resolve and stored with its
 *     addend). The 12-bit next field strides the chain by 4 bytes.
 *
 *     Classic binaries (the BSD coreutils — /bin/ls, /bin/cp, /usr/bin/du, ...)
 *     instead carry LC_DYLD_INFO[_ONLY] opcode streams and __la_symbol_ptr lazy
 *     stubs, NO chained fixups. apply_classic_fixups interprets those streams
 *     exactly as dyld would: classic_rebase walks the REBASE opcodes and adds
 *     the slide to every internal pointer (SET_SEGMENT_AND_OFFSET resolves a
 *     segment index to seg_vmaddr[index]+slide); classic_bind_stream walks the
 *     BIND opcodes (SET_DYLIB_ORDINAL / SET_SYMBOL_TRAILING_FLAGS giving the
 *     name and weak bit / SET_ADDEND_SLEB / SET_SEGMENT_AND_OFFSET / ADD_ADDR /
 *     DO_BIND[_ADD_ADDR/_IMM_SCALED/_ULEB_TIMES_SKIPPING]) resolving each symbol
 *     through ocerz_cache_resolve and storing value+addend. The lazy_bind stream
 *     is bound EAGERLY at load (faithful: lazy binding is deferred eager binding,
 *     and dyld itself binds eagerly under bind-at-launch). Eagerly binding the
 *     lazy stream overwrites each __la_symbol_ptr slot — which on disk holds a
 *     preferred-base (unslid) pointer into __stub_helper — with the real symbol
 *     address, so the first `jmp *la_symbol_ptr[n]` lands on the resolved target
 *     instead of an unmapped unslid address, and no dyld_stub_binder dance is
 *     needed. Without this the BSD coreutils SIGSEGV on their first lazy call.
 *  4. Build the entry frame: a guest stack with argc/argv/envp/apple and the
 *     C string data, plus a tiny raw-_exit trampoline used as main's return
 *     address on the freestanding path.
 *  5. Bring up libSystem and run the program the way dyld's start does. When
 *     the executable links a dylib (any LC_LOAD_DYLIB, the normal case) Ocerz
 *     runs libSystem.B.dylib's single __init_offsets orchestrator first — this
 *     boots libpthread (main-thread TSD at gs_base, the kernel commpage),
 *     libmalloc, libc, and libobjc, the last of which drives the objc<->dyld
 *     map_images handshake through the dyld-API shim (see dyldapi.c). Then,
 *     under OCERZ_INITPHASE, Ocerz runs the +load phase and the image
 *     initializers in dependency order, the way dyld4 does. Which images get
 *     their +load/initializers run is the "eager set". dyld runs them for the
 *     whole dependency closure; Ocerz computes a sound approximation: it seeds
 *     the set with the main executable's DIRECT LC_LOAD_DYLIB dependencies
 *     (every dylib dyld is guaranteed to bring up — these always have their
 *     initializers run) plus the libSystem/usr-lib-system roots, then expands
 *     by following data pointers (scan_uses) to the transitive dependencies the
 *     program actually references. Seeding the direct deps is what fixes a
 *     binary that reaches Foundation only indirectly — e.g. a pure-Swift main
 *     whose bridge to Foundation runs through libswiftCore and has no
 *     __objc_classrefs of its own: Foundation is still a direct LC_LOAD_DYLIB,
 *     so its initializer now runs and caches NSString's concrete class, and
 *     +[NSString allocWithZone:] hands back a real __NSPlaceholderString instead
 *     of class_createInstance'ing a bare abstract NSString. Then it calls
 *     main(argc,argv,envp,apple) and, on a normal return, libc exit(rv) so
 *     atexit handlers run and stdio is flushed (a bare _exit would lose buffered
 *     printf output). A freestanding executable with no dylibs skips init and
 *     uses the raw _exit trampoline. OCERZ_INIT forces init on, OCERZ_NOINIT
 *     forces it off.
 *
 * libc/Objective-C executables run: imports resolve through ocerz_cache_resolve
 * (which now follows re-export terminals, e.g. _memset -> libsystem_platform
 * __platform_memset, see cache.c), the dependency closure of objc images is
 * mapped (see dyldapi.c), the cache images' +load/initializers run in
 * dependency order (step 5), and exit() flushes stdio. Foundation/Swift apps
 * (e.g. NSString bridging through libswiftCore) run end-to-end.
 *
 * Thread-local variables (TLV). C/C++ thread_local / __thread storage works the
 * way real dyld makes it work, which Ocerz must drive itself because it is the
 * mini-dyld. A `thread_local X` access compiles to `lea rdi,[desc]; call *[desc]`
 * where desc is a 24-byte tlv_descriptor in the image's __thread_vars
 * (S_THREAD_LOCAL_VARIABLES, type 0x13). On disk that descriptor is
 * {thunk, key, offset}: the thunk is a chained/classic BIND to the libdyld
 * placeholder import __tlv_bootstrap, which is a 5-byte `jmp <fatal>` to dyld's
 * "thread locals not initialized" -> abort path. Real dyld never leaves it that
 * way: at image load it (a) calls pthread_key_create once per image to get a TSD
 * key, (b) writes that key into every descriptor, and (c) overwrites every
 * descriptor's thunk to the REAL worker tlv_get_addr, which is the body right
 * after the placeholder jmp (__tlv_bootstrap + 8). The worker's fast path is
 * `key=u32[desc+8]; base=gs:[key*8]; if base!=0 return base + u32[desc+0xc]`;
 * its slow path (base==0, the thread's first access) allocates a per-thread
 * block sized to the image's TLV template, memmoves the __thread_data initial
 * bytes (or calloc-zeroes a pure __thread_bss), pthread_setspecific(key, block),
 * and returns block + offset. The worker reads dyld's INTERNAL descriptor layout
 * — NOT the on-disk one — a repacked 24 bytes:
 *   {thunk@0(8), key@8(u32), offset@0xc(u32), template_self_rel@0x10(s32),
 *    block_size@0x14(u32)},
 * where template_self_rel reconstructs the template address as
 * (desc+0x10)+(s32)[desc+0x10]. ocerz_tlv_register_image performs dyld's job for
 * every loaded image that has a __thread_vars section: it locates the thread
 * template region (__thread_data 0x11 + __thread_bss 0x12), creates one real
 * guest pthread key by calling _pthread_key_create through ocerz_vm_call, then
 * rewrites each descriptor in place into the internal layout — thunk =
 * cache(__tlv_bootstrap)+8, the image's key, the descriptor's own (original)
 * offset, the self-relative pointer to the runtime (already-rebased) template,
 * and the block size. After that the first `call *[desc]` on a thread runs the
 * real worker: it sees gs:[key*8]==0 (a fresh TSD slot in THIS host thread's
 * gs_base), takes the guest slow path that mallocs+memmoves+setspecifics the
 * block, and returns the address of this thread's own copy; later accesses on
 * the same thread hit the fast path. Because every guest thread is a real host
 * pthread with its own gs_base TSD (syscall.c bsdthread_create), a second guest
 * thread's gs:[key*8] is independently 0 and it allocates its OWN block — true
 * per-thread storage with no extra work. Registration runs after the libSystem
 * initializer (so libpthread/pthread_key_create are up) and before the C++
 * static-init phase, for the main exe, every dependency with TLVs, and dlopen'd
 * images at their fixup time. The key dtor is left 0 (the per-thread block leaks
 * at thread exit — process-lifetime data, sound for correctness; real dyld
 * passes tlv_free, an upgrade for later).
 *
 * Non-cache disk dylibs. Some executables link a dylib that is a real file on
 * disk rather than a member of the shared cache — e.g. /usr/bin/perl links
 * .../CORE/libperl.dylib by absolute path. DynImage is therefore the element
 * of a small global registry (g_dimgs), the main exe being a transient entry-0
 * and every disk dylib a registered entry. Before the main exe's fixups,
 * load_disk_deps walks its LC_LOAD_DYLIB list and, for each install name NOT
 * resolvable in the cache, load_disk_dylib opens the file, selects the x86_64
 * slice, maps its segments into an ocerz_map_anywhere region (map_segments with
 * is_main=0 chooses load_base from that region; the main exe keeps its byte-
 * identical ocerz_arena_lo path), recurses into THAT image's own non-cache deps
 * deps-first, then applies its fixups. Both fixup engines resolve imports
 * through resolve_import: a positive two-level library ordinal maps to the
 * loader image's Nth LC_LOAD_DYLIB install name and, when that names a loaded
 * disk image, resolves in its export trie first; otherwise the existing cache
 * resolve runs, with a flat sweep of the disk images as a final fallback (so a
 * binary whose every dep is a cache path takes the unchanged cache path — zero
 * regression). Each disk image's mach_header is registered in the dyld-API
 * closure (ocerz_dyldapi_register_image) so image lists / unwind / objc see it
 * and its path, and the init/load phases descend into it via dep_mh (cache OR
 * disk) so its initializers run deps-first; it joins the eager set as a direct
 * dep. The image's file buffer (owned_buf) is kept alive for the whole run
 * because its export trie is read at resolve time. The slide-from-segment
 * helpers (image_slide_d here, image_slide/find_section_sz in dyldapi.c) key
 * off the first fileoff==0 segment with non-zero filesize, which is the header-
 * bearing __TEXT for BOTH a cache image, the main exe (skipping __PAGEZERO) and
 * a disk dylib whose __TEXT sits at vmaddr 0.
 *
 * @rpath/@loader_path/@executable_path expansion. Homebrew binaries and
 * relocatable bundles reference their dylibs by a relocatable install name
 * rather than an absolute path. expand_install_name reproduces dyld's @path
 * semantics: @executable_path/STEM resolves against dirname(main exe host path)
 * (g_main_hostpath); @loader_path/STEM against dirname of the REFERENCING image's
 * on-disk path (DynImage.path; the main exe's host path for its own direct deps);
 * @rpath/STEM iterates the accumulated rpath search list and takes the first
 * candidate that access(F_OK)'s. Non-@ names pass through verbatim, so the entire
 * existing absolute-path suite is byte-identical. The rpath search list is the
 * ordered concatenation of the LC_RPATH entries of the main executable and of
 * every dylib on the load chain that reached this image, inherited down the load
 * graph (RpathList threaded through load_disk_deps -> load_disk_dylib): at each
 * load collect_rpaths appends THIS image's own LC_RPATH (each entry itself
 * @loader_path/@executable_path-expanded relative to that image; an absolute
 * rpath entry passes through) after the inherited list. So cwebp's main-exe rpath
 * "@loader_path/../lib" (=> /usr/local/lib) resolves @rpath/libwebp.7.dylib, and
 * when libwebp later pulls @rpath/libsharpyuv.0.dylib the inherited list still
 * carries that entry so the leaf is found. Dedup keys on the RESOLVED on-disk
 * path so a lib reached via two install names loads once. dyld's two-level
 * namespace binding records the TARGET IMAGE, not a path string: DynImage stores
 * both the resolved path and the requesting install_name, and the ordinal->image
 * lookup (resolve_import) and the init/load-phase descent (dep_mh) match the
 * Nth-dependency install name (e.g. "@rpath/libwebp.7.dylib") against
 * install_name first, falling back to path — so an @rpath dep registered under
 * its resolved path still binds and runs its initializers deps-first.
 *
 * Runtime dlopen/dlsym. After main starts, a program (perl's XSLoader) calls
 * dlopen()/dlsym() to bring up MH_BUNDLE plug-ins — perl's XS .bundle modules.
 * Those land in the dyld-API shim's vtable slots (dyldapi.c +0x68 dlopen, +0x80
 * dlsym, +0x70 dlclose, +0x78 dlerror), which marshal the System V args and
 * call the loader seam exported here: ocerz_dlopen / ocerz_dlsym / ocerz_dlclose
 * / ocerz_dlerror. ocerz_dlopen dedups the path against the cache and g_dimgs;
 * an unloaded disk path is brought up by dlopen_load_image, the non-fatal twin
 * of load_disk_dylib (read_file -> select_slice(x86_64) -> map_segments(is_main=
 * 0) -> load_disk_deps -> apply_fixups||apply_classic_fixups -> ocerz_dyldapi_
 * register_image), then its initializers (and any newly-loaded deps', deps-
 * first) run INLINE on the live guest via run_init_phase — the guest is mid-
 * execution, NOT in the pre-main init phase, so ocerz_dlopen runs the inits on
 * their own scratch stack (a fresh ocerz_map_anywhere region) so perl's live
 * frames are untouched, and the dispatch handler in dyldapi.c snapshots and
 * restores the full guest CPU around the call. Failures are non-fatal: a
 * missing/bad bundle records a dlerror string and returns a NULL handle, exactly
 * as real dlopen does, so the guest (XSLoader) reports its own error instead of
 * Ocerz dying. The handle IS the image's mach_header guest address (load_base);
 * a re-dlopen of a loaded path returns the same handle. ocerz_dlsym prepends '_'
 * to the caller's symbol (dlsym strips the underscore the Mach-O name carries),
 * searches the handle image's export trie (ocerz_image_self_resolve) then its
 * LC_SYMTAB nlist table (N_SECT|N_EXT defined symbols), and honors the special
 * RTLD_DEFAULT/RTLD_SELF/RTLD_MAIN_ONLY/RTLD_NEXT handles with a flat search of
 * every disk image plus the cache. perl's boot_<Module> XS entry is found in the
 * bundle's export trie, so `use Fcntl` and shasum's Digest::SHA load run.
 */
#include "ocerz/dyld.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/dyldapi.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#define DYN_ARENA_SIZE (4ull << 30)
#define DYN_STACK_SIZE (8ull << 20)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        close(fd);
        return NULL;
    }
    if (pread(fd, buf, (size_t)sz, 0) != (ssize_t)sz) {
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);
    *len_out = (size_t)sz;
    return buf;
}

static const uint8_t *select_slice(const uint8_t *buf, size_t len)
{
    uint32_t magic = rd32(buf);
    if (magic == MH_MAGIC_64)
        return buf;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        int swap = (magic == FAT_CIGAM || magic == FAT_CIGAM_64);
        int is64 = (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64);
        uint32_t nfat = rd32(buf + 4);
        if (swap)
            nfat = __builtin_bswap32(nfat);
        const uint8_t *fa = buf + 8;
        size_t stride = is64 ? 32 : 20;
        for (uint32_t i = 0; i < nfat; i++) {
            const uint8_t *e = fa + i * stride;
            uint32_t cputype = rd32(e);
            uint64_t off = is64 ? rd64(e + 8) : rd32(e + 8);
            if (swap) {
                cputype = __builtin_bswap32(cputype);
                off = is64 ? __builtin_bswap64(off) : __builtin_bswap32((uint32_t)off);
            }
            if (cputype == CPU_TYPE_X86_64 && off < len)
                return buf + off;
        }
    }
    return NULL;
}

int ocerz_peek_dynamic(const char *path)
{
    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf)
        return -1;
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        free(buf);
        return -1;
    }
    uint32_t ncmds = rd32(slice + 16);
    const uint8_t *lc = slice + sizeof(struct mach_header_64);
    int dynamic = -1;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000028) {
            dynamic = 1;
            break;
        }
        if (cmd == LC_UNIXTHREAD) {
            dynamic = 0;
            break;
        }
        lc += rd32(lc + 4);
    }
    free(buf);
    return dynamic;
}

#define DYN_SEG_MAX 16

typedef struct DynImage {
    const uint8_t *slice;
    uint8_t *owned_buf;
    char path[1024];
    char install_name[1024];
    uint64_t slide;
    uint64_t load_base;
    uint64_t main_entry;
    uint32_t cf_off;
    uint32_t cf_size;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    int has_dyld_info;
    uint64_t seg_vmaddr[DYN_SEG_MAX];
    int seg_count;
    int is_pie;
    int links_dylib;
} DynImage;

#define DYN_DIMG_MAX 64
static DynImage g_dimgs[DYN_DIMG_MAX];
static int g_dimgs_n;
static char g_main_hostpath[1024];

static OcerzCache *g_run_cache;
static struct OcerzVM *g_run_vm;
static uint64_t g_run_init_args[5];
static int g_run_init_ready;
static uint64_t g_dlerror_g;

static DynImage *dimg_find_by_path(const char *path)
{
    for (int i = 0; i < g_dimgs_n; i++)
        if (g_dimgs[i].path[0] && strcmp(g_dimgs[i].path, path) == 0)
            return &g_dimgs[i];
    return NULL;
}

static DynImage *dimg_find_by_install_name(const char *iname)
{
    for (int i = 0; i < g_dimgs_n; i++)
        if (g_dimgs[i].install_name[0] && strcmp(g_dimgs[i].install_name, iname) == 0)
            return &g_dimgs[i];
    return NULL;
}

static int map_segments(DynImage *img, int is_main)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    img->is_pie = (rd32(mh + 24) & MH_PIE) != 0;
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint64_t text_vmaddr = 0;
    int have_text = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64 && rd64(lc + 40) == 0 && rd64(lc + 48) != 0) {
            text_vmaddr = rd64(lc + 24);
            have_text = 1;
            break;
        }
        lc += rd32(lc + 4);
    }
    if (!have_text)
        return OCERZ_EFORMAT;

    uint64_t vmlo = ~0ull, vmhi = 0;
    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 56);
            if (!(vmaddr == 0 && initprot == 0)) {
                if (vmaddr < vmlo)
                    vmlo = vmaddr;
                if (vmaddr + vmsize > vmhi)
                    vmhi = vmaddr + vmsize;
            }
        }
        lc += rd32(lc + 4);
    }
    if (vmhi <= vmlo)
        return OCERZ_EFORMAT;

    if (is_main) {
        img->load_base = ocerz_arena_lo;
        img->slide = img->load_base - text_vmaddr;
        if (ocerz_map_fixed(vmlo + img->slide, vmhi - vmlo, PROT_READ | PROT_WRITE) != OCERZ_OK)
            return OCERZ_ENOMEM;
    } else {
        uint64_t region = ocerz_map_anywhere(vmhi - vmlo, PROT_READ | PROT_WRITE);
        if (region == 0)
            return OCERZ_ENOMEM;
        img->slide = region - vmlo;
        img->load_base = text_vmaddr + img->slide;
    }

    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t fileoff = rd64(lc + 40);
            uint64_t filesize = rd64(lc + 48);
            uint32_t initprot = rd32(lc + 56);
            if (img->seg_count < DYN_SEG_MAX)
                img->seg_vmaddr[img->seg_count] = vmaddr;
            img->seg_count++;
            if (!(vmaddr == 0 && initprot == 0) && filesize)
                memcpy(ocerz_g2h(vmaddr + img->slide), img->slice + fileoff, (size_t)filesize);
        } else if (cmd == 0x80000028) {
            img->main_entry = text_vmaddr + rd64(lc + 8) + img->slide;
        } else if (cmd == 0x80000034) {
            img->cf_off = rd32(lc + 8);
            img->cf_size = rd32(lc + 12);
        } else if (cmd == 0x22 || cmd == 0x80000022) {
            img->has_dyld_info = 1;
            img->rebase_off = rd32(lc + 8);
            img->rebase_size = rd32(lc + 12);
            img->bind_off = rd32(lc + 0x10);
            img->bind_size = rd32(lc + 0x14);
            img->weak_bind_off = rd32(lc + 0x18);
            img->weak_bind_size = rd32(lc + 0x1c);
            img->lazy_bind_off = rd32(lc + 0x20);
            img->lazy_bind_size = rd32(lc + 0x24);
        } else if (cmd == 0x0c || cmd == 0x8000001f || cmd == 0x80000018) {
            img->links_dylib = 1;
        }
        lc += csize;
    }
    return OCERZ_OK;
}

static uint64_t self_uleb(const uint8_t **pp, const uint8_t *end)
{
    uint64_t r = 0;
    int sh = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80))
            break;
        sh += 7;
    }
    return r;
}

static uint64_t image_export_trie(DynImage *img, uint32_t *size_out)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000033) {
            *size_out = rd32(lc + 12);
            return rd32(lc + 8);
        }
        if (cmd == 0x22 || cmd == 0x80000022) {
            *size_out = rd32(lc + 0x2c);
            return rd32(lc + 0x28);
        }
        lc += rd32(lc + 4);
    }
    *size_out = 0;
    return 0;
}

static uint64_t ocerz_image_self_resolve(DynImage *img, const char *sym)
{
    uint32_t tsize = 0;
    uint64_t toff = image_export_trie(img, &tsize);
    if (!toff || !tsize)
        return 0;
    const uint8_t *start = img->slice + toff;
    const uint8_t *end = start + tsize;
    const uint8_t *p = start;
    const char *s = sym;
    while (p < end) {
        uint64_t term = self_uleb(&p, end);
        if (*s == '\0') {
            if (term == 0)
                return 0;
            const uint8_t *tp = p;
            uint64_t flags = self_uleb(&tp, end);
            if (flags & 0x08)
                return 0;
            return img->load_base + self_uleb(&tp, end);
        }
        p += term;
        if (p >= end)
            return 0;
        uint8_t children = *p++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < children; i++) {
            const char *edge = (const char *)p;
            size_t elen = strlen(edge);
            p += elen + 1;
            uint64_t child_off = self_uleb(&p, end);
            if (next == NULL && strncmp(s, edge, elen) == 0) {
                s += elen;
                next = start + child_off;
            }
        }
        if (next == NULL)
            return 0;
        p = next;
    }
    return 0;
}

static const char *dimg_ordinal_name(DynImage *img, int ord)
{
    if (ord <= 0)
        return NULL;
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    int n = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            if (++n == ord)
                return (const char *)(lc + rd32(lc + 8));
        }
        lc += rd32(lc + 4);
    }
    return NULL;
}

static uint64_t disk_flat_resolve(const char *name)
{
    for (int i = 0; i < g_dimgs_n; i++) {
        uint64_t v = ocerz_image_self_resolve(&g_dimgs[i], name);
        if (v)
            return v;
    }
    return 0;
}

static uint64_t resolve_import(OcerzCache *cache, DynImage *img, const char *name,
                               int libord, int weak)
{
    uint64_t value = 0;
    if (libord > 0) {
        const char *tgt = dimg_ordinal_name(img, libord);
        if (tgt) {
            DynImage *dep = dimg_find_by_install_name(tgt);
            if (!dep)
                dep = dimg_find_by_path(tgt);
            if (dep)
                value = ocerz_image_self_resolve(dep, name);
        }
    }
    if (value == 0)
        value = ocerz_cache_resolve(cache, name);
    if (value == 0 && (libord == -3 || libord == 0 || libord == -2))
        value = ocerz_image_self_resolve(img, name);
    if (value == 0)
        value = disk_flat_resolve(name);
    if (value == 0 && !weak)
        OCERZ_FATAL("unresolved import: %s\n", name);
    return value;
}

static int apply_fixups(DynImage *img, OcerzCache *cache)
{
    if (img->cf_off == 0)
        return OCERZ_OK;
    const uint8_t *cf = img->slice + img->cf_off;
    uint32_t starts_off = rd32(cf + 4);
    uint32_t imports_off = rd32(cf + 8);
    uint32_t symbols_off = rd32(cf + 12);
    uint32_t imports_cnt = rd32(cf + 16);

    const uint8_t *sii = cf + starts_off;
    uint32_t seg_count = rd32(sii);
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so = rd32(sii + 4 + s * 4);
        if (so == 0)
            continue;
        const uint8_t *sis = sii + so;
        uint16_t page_size = rd16(sis + 4);
        uint16_t ptr_format = rd16(sis + 6);
        uint64_t seg_off = rd64(sis + 8);
        uint16_t page_count = rd16(sis + 0x14);
        const uint8_t *page_start = sis + 0x16;
        if (ptr_format != 2 && ptr_format != 6) {
            OCERZ_FATAL("unsupported chained pointer format %u\n", ptr_format);
            return OCERZ_EUNSUP;
        }
        for (uint16_t pg = 0; pg < page_count; pg++) {
            uint16_t start = rd16(page_start + pg * 2);
            if (start == 0xffff)
                continue;
            uint64_t addr = img->load_base + seg_off + (uint64_t)pg * page_size + start;
            for (;;) {
                uint64_t raw = ocerz_ld(addr, 8);
                int bind = (int)(raw >> 63) & 1;
                uint32_t next = (uint32_t)((raw >> 51) & 0xfff);
                if (bind) {
                    uint32_t ordinal = (uint32_t)(raw & 0xffffff);
                    uint64_t addend = (raw >> 24) & 0xff;
                    uint64_t value = 0;
                    if (ordinal < imports_cnt) {
                        uint32_t imp = rd32(cf + imports_off + ordinal * 4);
                        uint32_t noff = imp >> 9;
                        int libord = (int8_t)(imp & 0xff);
                        int weakimp = (imp >> 8) & 1;
                        const char *name = (const char *)(cf + symbols_off + noff);
                        value = resolve_import(cache, img, name, libord, weakimp);
                    }
                    ocerz_st(addr, 8, value + addend);
                } else {
                    uint64_t target = raw & 0xfffffffffull;
                    uint64_t high8 = (raw >> 36) & 0xff;
                    uint64_t value = (ptr_format == 6)
                        ? img->load_base + target
                        : target + img->slide;
                    value |= high8 << 56;
                    ocerz_st(addr, 8, value);
                }
                if (next == 0)
                    break;
                addr += (uint64_t)next * 4;
            }
        }
    }
    return OCERZ_OK;
}

static int64_t self_sleb(const uint8_t **pp, const uint8_t *end)
{
    int64_t r = 0;
    int sh = 0;
    uint8_t b = 0;
    while (*pp < end) {
        b = *(*pp)++;
        r |= (int64_t)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80))
            break;
    }
    if (sh < 64 && (b & 0x40))
        r |= -(int64_t)1 << sh;
    return r;
}

static uint64_t classic_resolve(DynImage *img, OcerzCache *cache, const char *name,
                                int libord, int weak)
{
    return resolve_import(cache, img, name, libord, weak);
}

static void classic_rebase(DynImage *img)
{
    if (img->rebase_size == 0)
        return;
    const uint8_t *p = img->slice + img->rebase_off;
    const uint8_t *end = p + img->rebase_size;
    uint64_t addr = 0;
    int done = 0;
    while (p < end && !done) {
        uint8_t op = *p & 0xf0;
        uint8_t imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00:
            done = 1;
            break;
        case 0x10:
            break;
        case 0x20: {
            uint64_t off = self_uleb(&p, end);
            if (imm < (uint8_t)img->seg_count)
                addr = img->seg_vmaddr[imm] + img->slide + off;
            break;
        }
        case 0x30:
            addr += self_uleb(&p, end);
            break;
        case 0x40:
            addr += (uint64_t)imm * 8;
            break;
        case 0x50:
            for (uint8_t i = 0; i < imm; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8;
            }
            break;
        case 0x60: {
            uint64_t cnt = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8;
            }
            break;
        }
        case 0x70:
            ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
            addr += 8 + self_uleb(&p, end);
            break;
        case 0x80: {
            uint64_t cnt = self_uleb(&p, end);
            uint64_t skip = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8 + skip;
            }
            break;
        }
        default:
            done = 1;
            break;
        }
    }
}

static void classic_bind_stream(DynImage *img, OcerzCache *cache,
                                const uint8_t *p, const uint8_t *end, int is_lazy)
{
    uint64_t addr = 0;
    const char *name = "";
    int64_t addend = 0;
    int libord = 0;
    int weak = 0;
    int done = 0;
    while (p < end && !done) {
        uint8_t op = *p & 0xf0;
        uint8_t imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00:
            if (is_lazy) {
                addr = 0;
                addend = 0;
                weak = 0;
            } else {
                done = 1;
            }
            break;
        case 0x10:
            libord = imm;
            break;
        case 0x20:
            libord = (int)self_uleb(&p, end);
            break;
        case 0x30:
            libord = imm ? (int)(int8_t)(0xf0 | imm) : 0;
            break;
        case 0x40:
            weak = (imm & 0x1) != 0;
            name = (const char *)p;
            p += strlen((const char *)p) + 1;
            break;
        case 0x50:
            break;
        case 0x60:
            addend = self_sleb(&p, end);
            break;
        case 0x70: {
            uint64_t off = self_uleb(&p, end);
            if (imm < (uint8_t)img->seg_count)
                addr = img->seg_vmaddr[imm] + img->slide + off;
            break;
        }
        case 0x80:
            addr += self_uleb(&p, end);
            break;
        case 0x90: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8;
            break;
        }
        case 0xa0: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8 + self_uleb(&p, end);
            break;
        }
        case 0xb0: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8 + (uint64_t)imm * 8;
            break;
        }
        case 0xc0: {
            uint64_t cnt = self_uleb(&p, end);
            uint64_t skip = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t v = classic_resolve(img, cache, name, libord, weak);
                ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
                addr += 8 + skip;
            }
            break;
        }
        default:
            done = 1;
            break;
        }
    }
}

static int apply_classic_fixups(DynImage *img, OcerzCache *cache)
{
    if (!img->has_dyld_info)
        return OCERZ_OK;
    classic_rebase(img);
    if (img->bind_size)
        classic_bind_stream(img, cache, img->slice + img->bind_off,
                            img->slice + img->bind_off + img->bind_size, 0);
    if (img->weak_bind_size)
        classic_bind_stream(img, cache, img->slice + img->weak_bind_off,
                            img->slice + img->weak_bind_off + img->weak_bind_size, 0);
    if (img->lazy_bind_size)
        classic_bind_stream(img, cache, img->slice + img->lazy_bind_off,
                            img->slice + img->lazy_bind_off + img->lazy_bind_size, 1);
    return OCERZ_OK;
}

typedef struct DynFrame {
    uint64_t argc;
    uint64_t argv_arr;
    uint64_t envp_arr;
    uint64_t apple_arr;
    uint64_t progvars;
    uint64_t stack_top;
    uint64_t exit_stub;
} DynFrame;

static uint64_t put_str(uint64_t *sp, const char *s)
{
    size_t l = strlen(s) + 1;
    *sp -= l;
    memcpy(ocerz_g2h(*sp), s, l);
    return *sp;
}

static int build_frame(const char *path, int argc, char **argv, char **envp, DynFrame *out)
{
    uint64_t aux = ocerz_map_anywhere(1u << 20, PROT_READ | PROT_WRITE);
    uint64_t stack = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
    if (aux == 0 || stack == 0)
        return OCERZ_ENOMEM;

    static const uint8_t exit_stub[] = { 0x89, 0xc7, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05 };
    out->exit_stub = stack + DYN_STACK_SIZE - 64;
    memcpy(ocerz_g2h(out->exit_stub), exit_stub, sizeof exit_stub);
    out->stack_top = (out->exit_stub - 256) & ~0xfull;

    char apple0[2048];
    snprintf(apple0, sizeof apple0, "executable_path=%s", path);

    uint64_t sp = aux + (1u << 20);
    uint64_t argv_g[64], envp_g[64], apple_g[8];
    int envc = 0;
    while (envp && envp[envc] && envc < 60)
        envc++;

    for (int i = argc - 1; i >= 0; i--)
        argv_g[i] = put_str(&sp, argv[i]);
    for (int i = envc - 1; i >= 0; i--)
        envp_g[i] = put_str(&sp, envp[i]);
    char thbuf[64], stkbuf[160];
    snprintf(thbuf, sizeof thbuf, "th_port=0x%x", (unsigned)mach_thread_self());
    snprintf(stkbuf, sizeof stkbuf, "main_stack=0x%llx,0x%llx,0x%llx,0x%llx",
             (unsigned long long)(stack + DYN_STACK_SIZE), (unsigned long long)DYN_STACK_SIZE,
             (unsigned long long)0x4000, (unsigned long long)0x4000);
    apple_g[0] = put_str(&sp, apple0);
    apple_g[1] = put_str(&sp, "stack_guard=0x6f6365727a5f6773");
    apple_g[2] = put_str(&sp, "ptr_munge=0xa3f1c2b4d5e60718");
    apple_g[3] = put_str(&sp, "malloc_entropy=0x91827364a5b6c7d8,0x1f2e3d4c5b6a7988");
    apple_g[4] = put_str(&sp, stkbuf);
    apple_g[5] = put_str(&sp, thbuf);
    int applec = 6;

    sp &= ~0xfull;

    uint64_t argv_arr = sp - (uint64_t)(argc + 1) * 8;
    for (int i = 0; i < argc; i++)
        ocerz_st(argv_arr + (uint64_t)i * 8, 8, argv_g[i]);
    ocerz_st(argv_arr + (uint64_t)argc * 8, 8, 0);

    uint64_t envp_arr = argv_arr - (uint64_t)(envc + 1) * 8;
    for (int i = 0; i < envc; i++)
        ocerz_st(envp_arr + (uint64_t)i * 8, 8, envp_g[i]);
    ocerz_st(envp_arr + (uint64_t)envc * 8, 8, 0);

    uint64_t apple_arr = envp_arr - (uint64_t)(applec + 1) * 8;
    for (int i = 0; i < applec; i++)
        ocerz_st(apple_arr + (uint64_t)i * 8, 8, apple_g[i]);
    ocerz_st(apple_arr + (uint64_t)applec * 8, 8, 0);

    uint64_t cells = (apple_arr - 8 * 8) & ~0xfull;
    const char *slash = strrchr(argv[0], '/');
    uint64_t leaf = argv_g[0] + (slash ? (uint64_t)(slash - argv[0] + 1) : 0);
    ocerz_st(cells + 0, 4, (uint64_t)argc);
    ocerz_st(cells + 8, 8, argv_arr);
    ocerz_st(cells + 16, 8, envp_arr);
    ocerz_st(cells + 24, 8, leaf);

    uint64_t pv = cells - 48;
    ocerz_st(pv + 0, 8, ocerz_arena_lo);
    ocerz_st(pv + 8, 8, cells + 0);
    ocerz_st(pv + 16, 8, cells + 8);
    ocerz_st(pv + 24, 8, cells + 16);
    ocerz_st(pv + 32, 8, cells + 24);

    out->argc = (uint64_t)argc;
    out->argv_arr = argv_arr;
    out->envp_arr = envp_arr;
    out->apple_arr = apple_arr;
    out->progvars = pv;
    return OCERZ_OK;
}

static uint64_t find_dylib_init(OcerzCache *cache, const char *substr)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (!path || !strstr(path, substr) || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint32_t ns = rd32(lc + 64);
                const uint8_t *sec = lc + 72;
                for (uint32_t s = 0; s < ns; s++) {
                    uint8_t ty = rd32(sec + 64) & 0xff;
                    uint64_t sa = rd64(sec + 32), ssz = rd64(sec + 40);
                    if (ty == 0x16 && ssz >= 4)
                        return mh + rd32((const uint8_t *)(uintptr_t)sa);
                    if (ty == 0x09 && ssz >= 8)
                        return rd64((const uint8_t *)(uintptr_t)sa);
                    sec += 80;
                }
            }
            lc += rd32(lc + 4);
        }
        return 0;
    }
    return 0;
}

static uint64_t dep_find(OcerzCache *cache, const char *path)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (mh && p && strcmp(p, path) == 0)
            return mh;
    }
    return 0;
}

static uint64_t dep_mh(OcerzCache *cache, const char *path)
{
    uint64_t mh = dep_find(cache, path);
    if (mh)
        return mh;
    DynImage *d = dimg_find_by_install_name(path);
    if (!d)
        d = dimg_find_by_path(path);
    return d ? d->load_base : 0;
}

static int64_t image_slide_d(uint64_t mh)
{
    uint32_t ncmds = rd32((const uint8_t *)(uintptr_t)mh + 16);
    const uint8_t *lc = (const uint8_t *)(uintptr_t)mh + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (s->fileoff == 0 && s->filesize != 0)
                return (int64_t)mh - (int64_t)s->vmaddr;
        }
        lc += l->cmdsize;
    }
    return 0;
}

struct seg_ent { uint64_t lo, hi, mh; };
#define SEG_MAX 32768
static struct seg_ent g_segs[SEG_MAX];
static int g_segs_n;
static int seg_cmp(const void *a, const void *b)
{
    const struct seg_ent *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo ? 1 : 0;
}
static void build_segs(OcerzCache *cache)
{
    g_segs_n = 0;
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (!mh || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        int64_t slide = image_slide_d(mh);
        uint32_t ncmds = rd32((const uint8_t *)(uintptr_t)mh + 16);
        const uint8_t *lc = (const uint8_t *)(uintptr_t)mh + sizeof(struct mach_header_64);
        for (uint32_t n = 0; n < ncmds; n++) {
            const struct load_command *l = (const void *)lc;
            if (l->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *s = (const void *)lc;
                if (s->vmsize && g_segs_n < SEG_MAX) {
                    g_segs[g_segs_n].lo = s->vmaddr + slide;
                    g_segs[g_segs_n].hi = s->vmaddr + slide + s->vmsize;
                    g_segs[g_segs_n].mh = mh;
                    g_segs_n++;
                }
            }
            lc += l->cmdsize;
        }
    }
    qsort(g_segs, g_segs_n, sizeof(struct seg_ent), seg_cmp);
}
static uint64_t seg_owner(uint64_t addr)
{
    int lo = 0, hi = g_segs_n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_segs[mid].lo <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best >= 0 && addr < g_segs[best].hi)
        return g_segs[best].mh;
    return 0;
}

#define EAGER_MAX 4096
static uint64_t g_eager[EAGER_MAX];
static int g_eager_n;
static int eager_has(uint64_t mh)
{
    for (int i = 0; i < g_eager_n; i++)
        if (g_eager[i] == mh) return 1;
    return 0;
}
static void eager_add(uint64_t mh)
{
    if (mh && !eager_has(mh) && g_eager_n < EAGER_MAX)
        g_eager[g_eager_n++] = mh;
}
static void scan_uses(uint64_t mh)
{
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32((const uint8_t *)(uintptr_t)mh + 16);
    const uint8_t *lc = (const uint8_t *)(uintptr_t)mh + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++) {
                uint32_t type = sc[j].flags & 0xff;
                int isptr = type == 6 || type == 7 ||
                            strncmp(sc[j].sectname, "__got", 16) == 0 ||
                            strncmp(sc[j].sectname, "__la_symbol_ptr", 16) == 0 ||
                            strncmp(sc[j].sectname, "__auth_got", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_classrefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_superrefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_protorefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_nlclslist", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_catlist", 16) == 0 ||
                            strncmp(sc[j].sectname, "__cfstring", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_classlist", 16) == 0;
                if (isptr) {
                    uint64_t a = sc[j].addr + slide, e = a + sc[j].size;
                    for (uint64_t pp = a; pp + 8 <= e; pp += 8) {
                        uint64_t v = rd64((const uint8_t *)(uintptr_t)pp);
                        uint64_t o = seg_owner(v);
                        if (o && o != mh) eager_add(o);
                    }
                }
            }
        }
        lc += l->cmdsize;
    }
}
static void eager_add_direct_deps(OcerzCache *cache, uint64_t mh)
{
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    if (rd32(h) != MH_MAGIC_64)
        return;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                eager_add(dep_mh(cache, (const char *)(lc + noff)));
        }
        lc += rd32(lc + 4);
    }
}

static void compute_eager_set(OcerzCache *cache, uint64_t main_mh)
{
    build_segs(cache);
    g_eager_n = 0;
    eager_add(main_mh);
    eager_add_direct_deps(cache, main_mh);
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (mh && p && (strstr(p, "/usr/lib/system/") ||
                        strcmp(p, "/usr/lib/libSystem.B.dylib") == 0))
            eager_add(mh);
    }
    int root_n = g_eager_n;
    for (int i = 0; i < g_eager_n; i++)
        scan_uses(g_eager[i]);
    if (getenv("OCERZ_INITLOG"))
        fprintf(stderr, "dynamic: eager init set: root=%d eager=%d (of closure)\n", root_n, g_eager_n);
}

#define TLV_REG_MAX 4096
static uint64_t g_tlv_registered[TLV_REG_MAX];
static int g_tlv_registered_n;

static int tlv_is_registered(uint64_t mh)
{
    for (int i = 0; i < g_tlv_registered_n; i++)
        if (g_tlv_registered[i] == mh)
            return 1;
    return 0;
}

static void ocerz_tlv_register_image(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                                     uint64_t stack_top)
{
    if (!mh || tlv_is_registered(mh))
        return;
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    if (rd32(h) != MH_MAGIC_64)
        return;
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    uint64_t vars_addr = 0, vars_size = 0;
    uint64_t tmpl_lo = ~0ull, tmpl_hi = 0, data_lo = ~0ull;
    for (uint32_t j = 0; j < ncmds; j++) {
        if (rd32(lc) == LC_SEGMENT_64) {
            uint32_t ns = rd32(lc + 64);
            const uint8_t *sec = lc + 72;
            for (uint32_t s = 0; s < ns; s++) {
                uint8_t ty = rd32(sec + 64) & 0xff;
                uint64_t sa = rd64(sec + 32), ssz = rd64(sec + 40);
                if (ty == 0x13) {
                    vars_addr = sa;
                    vars_size = ssz;
                } else if (ty == 0x11 || ty == 0x12) {
                    if (sa < tmpl_lo)
                        tmpl_lo = sa;
                    if (sa + ssz > tmpl_hi)
                        tmpl_hi = sa + ssz;
                    if (ty == 0x11 && sa < data_lo)
                        data_lo = sa;
                }
                sec += 80;
            }
        }
        lc += rd32(lc + 4);
    }
    if (g_tlv_registered_n < TLV_REG_MAX)
        g_tlv_registered[g_tlv_registered_n++] = mh;
    if (vars_addr == 0 || vars_size < 24)
        return;

    uint64_t boot = ocerz_cache_resolve(cache, "__tlv_bootstrap");
    if (boot == 0) {
        OCERZ_LOG("dynamic: TLV: __tlv_bootstrap unresolved, skipping mh=%#llx\n",
                  (unsigned long long)mh);
        return;
    }
    uint64_t tlv_get_addr = boot + 8;

    uint64_t keycreate = ocerz_cache_resolve(cache, "_pthread_key_create");
    if (keycreate == 0) {
        OCERZ_LOG("dynamic: TLV: _pthread_key_create unresolved, skipping mh=%#llx\n",
                  (unsigned long long)mh);
        return;
    }
    uint64_t scratch = ocerz_map_anywhere(16, PROT_READ | PROT_WRITE);
    if (scratch == 0)
        return;
    ocerz_st(scratch, 8, 0);
    uint64_t ka[2] = { scratch, 0 };
    uint64_t krc = ocerz_vm_call(vm, keycreate, ka, 2, stack_top);
    if (vm->exited)
        return;
    uint32_t key = (uint32_t)ocerz_ld(scratch, 4);
    if (krc != 0 || key < 0xa || key > 0x2ff) {
        OCERZ_LOG("dynamic: TLV: pthread_key_create failed (rc=%llu key=%u) mh=%#llx\n",
                  (unsigned long long)krc, key, (unsigned long long)mh);
        return;
    }

    uint64_t block_size = (tmpl_hi > tmpl_lo) ? (tmpl_hi - tmpl_lo) : 0;
    int has_data = (data_lo != ~0ull);
    uint64_t tmpl_runtime = (uint64_t)((int64_t)tmpl_lo + slide);

    uint64_t descs_rt = (uint64_t)((int64_t)vars_addr + slide);
    for (uint64_t off = 0; off + 24 <= vars_size; off += 24) {
        uint64_t desc = descs_rt + off;
        uint32_t var_off = (uint32_t)ocerz_ld(desc + 16, 8);
        int32_t self_rel = has_data
            ? (int32_t)((int64_t)tmpl_runtime - (int64_t)(desc + 0x10))
            : 0;
        ocerz_st(desc + 0, 8, tlv_get_addr);
        ocerz_st(desc + 8, 4, key);
        ocerz_st(desc + 0xc, 4, var_off);
        ocerz_st(desc + 0x10, 4, (uint32_t)self_rel);
        ocerz_st(desc + 0x14, 4, (uint32_t)block_size);
    }
    OCERZ_LOG("dynamic: TLV: registered mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
              (unsigned long long)mh, key, (unsigned long long)block_size,
              (unsigned long long)descs_rt, (unsigned long long)vars_size);
}

static void ocerz_tlv_register_closure(OcerzVM *vm, OcerzCache *cache, uint64_t main_mh,
                                       uint64_t stack_top)
{
    ocerz_tlv_register_image(vm, cache, main_mh, stack_top);
    for (int i = 0; i < g_eager_n && !vm->exited; i++)
        ocerz_tlv_register_image(vm, cache, g_eager[i], stack_top);
    for (int i = 0; i < g_dimgs_n && !vm->exited; i++)
        if (g_dimgs[i].load_base)
            ocerz_tlv_register_image(vm, cache, g_dimgs[i].load_base, stack_top);
}

static void run_image_inits(OcerzVM *vm, uint64_t mh, const uint64_t *ia, uint64_t stack_top)
{
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        if (rd32(lc) == LC_SEGMENT_64) {
            uint32_t ns = rd32(lc + 64);
            const uint8_t *sec = lc + 72;
            for (uint32_t s = 0; s < ns; s++) {
                uint8_t ty = rd32(sec + 64) & 0xff;
                uint64_t sa = (uint64_t)((int64_t)rd64(sec + 32) + slide), ssz = rd64(sec + 40);
                if (ty == 0x16) {
                    for (uint64_t o = 0; o + 4 <= ssz; o += 4) {
                        uint64_t fn = mh + rd32((const uint8_t *)(uintptr_t)(sa + o));
                        if (getenv("OCERZ_INITLOG"))
                            fprintf(stderr, "INIT mh=%#llx fn=%#llx\n",
                                    (unsigned long long)mh, (unsigned long long)fn);
                        ocerz_vm_call(vm, fn, ia, 5, stack_top);
                        if (vm->exited)
                            return;
                    }
                } else if (ty == 0x09) {
                    for (uint64_t o = 0; o + 8 <= ssz; o += 8) {
                        uint64_t fn = rd64((const uint8_t *)(uintptr_t)(sa + o));
                        if (fn) {
                            if (getenv("OCERZ_INITLOG"))
                                fprintf(stderr, "INIT mh=%#llx fn=%#llx\n",
                                        (unsigned long long)mh, (unsigned long long)fn);
                            ocerz_vm_call(vm, fn, ia, 5, stack_top);
                        }
                        if (vm->exited)
                            return;
                    }
                }
                sec += 80;
            }
        }
        lc += rd32(lc + 4);
    }
}

#define INIT_VISITED_MAX 4096
static uint64_t g_init_visited[INIT_VISITED_MAX];
static int g_init_visited_n;

static void run_init_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                           const uint64_t *ia, uint64_t stack_top, uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    if (rd32(h) != MH_MAGIC_64)
        return;
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return;
    if (g_init_visited_n < INIT_VISITED_MAX)
        g_init_visited[g_init_visited_n++] = mh;

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                run_init_phase(vm, cache, dep_mh(cache, (const char *)(lc + noff)),
                               ia, stack_top, skip_mh);
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_eager_n == 0 || eager_has(mh)))
        run_image_inits(vm, mh, ia, stack_top);
}

static void run_load_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh, uint64_t stack_top,
                           uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    if (rd32(h) != MH_MAGIC_64)
        return;
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return;
    if (g_init_visited_n < INIT_VISITED_MAX)
        g_init_visited[g_init_visited_n++] = mh;

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                run_load_phase(vm, cache, dep_mh(cache, (const char *)(lc + noff)),
                               stack_top, skip_mh);
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_eager_n == 0 || eager_has(mh)))
        ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
}

#define RPATH_MAX 64

typedef struct RpathList {
    char entry[RPATH_MAX][1024];
    int n;
} RpathList;

static void path_dirname(const char *in, char *out, size_t n)
{
    if (!in || !in[0]) {
        snprintf(out, n, ".");
        return;
    }
    const char *slash = strrchr(in, '/');
    if (!slash) {
        snprintf(out, n, ".");
        return;
    }
    if (slash == in) {
        snprintf(out, n, "/");
        return;
    }
    size_t len = (size_t)(slash - in);
    if (len >= n)
        len = n - 1;
    memcpy(out, in, len);
    out[len] = '\0';
}

static int expand_at_prefix(DynImage *loader, const char *name, char *out, size_t n)
{
    if (strncmp(name, "@executable_path/", 17) == 0) {
        char dir[1024];
        path_dirname(g_main_hostpath, dir, sizeof dir);
        snprintf(out, n, "%s/%s", dir, name + 17);
        return 1;
    }
    if (strncmp(name, "@loader_path/", 13) == 0) {
        char dir[1024];
        path_dirname(loader ? loader->path : g_main_hostpath, dir, sizeof dir);
        snprintf(out, n, "%s/%s", dir, name + 13);
        return 1;
    }
    return 0;
}

static int expand_rpath_entry(const char *entry, DynImage *loader, char *out, size_t n)
{
    if (expand_at_prefix(loader, entry, out, n))
        return 1;
    snprintf(out, n, "%s", entry);
    return 1;
}

static void collect_rpaths(DynImage *img, const RpathList *inherited, RpathList *merged)
{
    merged->n = 0;
    if (inherited) {
        for (int i = 0; i < inherited->n && merged->n < RPATH_MAX; i++)
            snprintf(merged->entry[merged->n++], 1024, "%s", inherited->entry[i]);
    }
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_RPATH) {
            uint32_t off = rd32(lc + 8);
            if (off < rd32(lc + 4) && merged->n < RPATH_MAX) {
                char exp[1024];
                expand_rpath_entry((const char *)(lc + off), img, exp, sizeof exp);
                snprintf(merged->entry[merged->n++], 1024, "%s", exp);
            }
        }
        lc += rd32(lc + 4);
    }
}

static int expand_install_name(DynImage *loader, const char *name,
                               const RpathList *rpaths, char *out, size_t n)
{
    if (!name)
        return 0;
    if (name[0] != '@') {
        snprintf(out, n, "%s", name);
        return 1;
    }
    if (expand_at_prefix(loader, name, out, n))
        return 1;
    if (strncmp(name, "@rpath/", 7) == 0) {
        const char *stem = name + 7;
        if (rpaths) {
            for (int i = 0; i < rpaths->n; i++) {
                char cand[1024];
                snprintf(cand, sizeof cand, "%s/%s", rpaths->entry[i], stem);
                if (access(cand, F_OK) == 0) {
                    snprintf(out, n, "%s", cand);
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

static void load_disk_deps(OcerzCache *cache, DynImage *loader, const RpathList *rpaths);

static DynImage *load_disk_dylib(OcerzCache *cache, const char *install_name, DynImage *loader,
                                 const RpathList *rpaths)
{
    char resolved[1024];
    if (!expand_install_name(loader, install_name, rpaths, resolved, sizeof resolved))
        return NULL;
    if (resolved[0] == '@')
        return NULL;
    if (dep_find(cache, resolved) != 0)
        return NULL;
    DynImage *existing = dimg_find_by_path(resolved);
    if (existing)
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
        return NULL;
    }

    size_t flen = 0;
    uint8_t *buf = read_file(resolved, &flen);
    if (!buf) {
        OCERZ_FATAL("Library not loaded: %s (no such file)\n", resolved);
        return NULL;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        OCERZ_FATAL("incompatible architecture: %s has no x86_64 slice\n", resolved);
        free(buf);
        return NULL;
    }

    DynImage *d = &g_dimgs[g_dimgs_n++];
    memset(d, 0, sizeof *d);
    d->slice = slice;
    d->owned_buf = buf;
    snprintf(d->path, sizeof d->path, "%s", resolved);
    snprintf(d->install_name, sizeof d->install_name, "%s", install_name);

    if (map_segments(d, 0) != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", resolved);
        g_dimgs_n--;
        free(buf);
        return NULL;
    }

    RpathList merged;
    collect_rpaths(d, rpaths, &merged);
    load_disk_deps(cache, d, &merged);

    if (apply_fixups(d, cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot apply fixups of %s\n", resolved);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);

    ocerz_dyldapi_register_image(d->load_base, d->path);
    OCERZ_LOG("dynamic: loaded disk dylib %s at load_base=%#llx slide=%#llx\n",
              resolved, (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

static void load_disk_deps(OcerzCache *cache, DynImage *loader, const RpathList *rpaths)
{
    const uint8_t *mh = loader->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                load_disk_dylib(cache, (const char *)(lc + noff), loader, rpaths);
        }
        lc += rd32(lc + 4);
    }
}

static void dlerror_set(const char *fmt, const char *arg)
{
    char host[1280];
    snprintf(host, sizeof host, fmt, arg ? arg : "");
    uint64_t need = (uint64_t)strlen(host) + 1;
    if (g_dlerror_g == 0)
        g_dlerror_g = ocerz_map_anywhere(2048, PROT_READ | PROT_WRITE);
    if (g_dlerror_g) {
        if (need > 2048)
            need = 2048;
        memcpy(ocerz_g2h(g_dlerror_g), host, (size_t)need);
        ((char *)ocerz_g2h(g_dlerror_g))[need - 1] = '\0';
    }
}

static DynImage *dlopen_load_image(OcerzCache *cache, const char *install_path)
{
    if (dep_find(cache, install_path) != 0)
        return NULL;
    DynImage *existing = dimg_find_by_path(install_path);
    if (existing)
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        dlerror_set("dlopen(%s): image registry full", install_path);
        return NULL;
    }
    size_t flen = 0;
    uint8_t *buf = read_file(install_path, &flen);
    if (!buf) {
        dlerror_set("dlopen(%s): image not found", install_path);
        return NULL;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        dlerror_set("dlopen(%s): no compatible x86_64 slice", install_path);
        free(buf);
        return NULL;
    }
    DynImage *d = &g_dimgs[g_dimgs_n++];
    memset(d, 0, sizeof *d);
    d->slice = slice;
    d->owned_buf = buf;
    snprintf(d->path, sizeof d->path, "%s", install_path);
    snprintf(d->install_name, sizeof d->install_name, "%s", install_path);
    if (map_segments(d, 0) != OCERZ_OK) {
        dlerror_set("dlopen(%s): cannot map segments", install_path);
        g_dimgs_n--;
        free(buf);
        return NULL;
    }
    RpathList merged;
    collect_rpaths(d, NULL, &merged);
    load_disk_deps(cache, d, &merged);
    if (apply_fixups(d, cache) != OCERZ_OK) {
        dlerror_set("dlopen(%s): cannot apply fixups", install_path);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);
    ocerz_dyldapi_register_image(d->load_base, d->path);
    OCERZ_LOG("dynamic: dlopen loaded %s at load_base=%#llx slide=%#llx\n",
              install_path, (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode)
{
    if (!g_run_cache) {
        dlerror_set("dlopen: runtime loader not initialized", NULL);
        return 0;
    }
    if (!hostpath) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return ocerz_arena_lo;
    }
    DynImage *already = dimg_find_by_path(hostpath);
    if (already) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return already->load_base;
    }
    uint64_t cmh = dep_find(g_run_cache, hostpath);
    if (cmh) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return cmh;
    }
    if (mode & 0x10) {
        dlerror_set("dlopen(%s): not already loaded (RTLD_NOLOAD)", hostpath);
        return 0;
    }
    int before = g_dimgs_n;
    DynImage *d = dlopen_load_image(g_run_cache, hostpath);
    if (!d)
        return 0;
    if (g_run_init_ready && g_run_vm && !vm->exited && g_dimgs_n > before) {
        uint64_t istk = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
        if (istk) {
            uint64_t itop = istk + DYN_STACK_SIZE - 64;
            for (int i = g_dimgs_n - 1; i >= before; i--) {
                ocerz_tlv_register_image(g_run_vm, g_run_cache, g_dimgs[i].load_base, itop);
                if (vm->exited)
                    break;
            }
            for (int i = g_dimgs_n - 1; i >= before && !vm->exited; i--) {
                run_image_inits(g_run_vm, g_dimgs[i].load_base, g_run_init_args, itop);
                if (vm->exited)
                    break;
            }
        }
    }
    if (g_dlerror_g)
        ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
    return d->load_base;
}

static uint64_t image_symtab_resolve(DynImage *img, const char *sym)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (rd32(lc) == LC_SYMTAB) {
            symoff = rd32(lc + 8);
            nsyms = rd32(lc + 12);
            stroff = rd32(lc + 16);
            strsize = rd32(lc + 20);
            break;
        }
        lc += rd32(lc + 4);
    }
    if (!symoff || !nsyms || !stroff)
        return 0;
    const uint8_t *nl = mh + symoff;
    const char *strs = (const char *)(mh + stroff);
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = nl + (uint64_t)i * 16;
        uint32_t strx = rd32(e);
        uint8_t ntype = e[4];
        if (strx == 0 || strx >= strsize)
            continue;
        if ((ntype & 0x0e) != 0x0e || !(ntype & 0x01))
            continue;
        if (strcmp(strs + strx, sym) == 0)
            return rd64(e + 8) + img->slide;
    }
    return 0;
}

uint64_t ocerz_dlsym(uint64_t handle, const char *sym)
{
    if (!sym || !sym[0])
        return 0;
    char buf[1024];
    buf[0] = '_';
    snprintf(buf + 1, sizeof buf - 1, "%s", sym);

    int64_t sh = (int64_t)handle;
    if (sh == -2 || sh == -3) {
        uint64_t v = disk_flat_resolve(buf);
        if (v)
            return v;
        if (g_run_cache)
            v = ocerz_cache_resolve(g_run_cache, buf);
        return v;
    }
    if (sh == -5) {
        DynImage *m = g_dimgs_n > 0 ? &g_dimgs[0] : NULL;
        return m ? ocerz_image_self_resolve(m, buf) : 0;
    }
    if (sh == -1) {
        uint64_t v = disk_flat_resolve(buf);
        if (v)
            return v;
        if (g_run_cache)
            v = ocerz_cache_resolve(g_run_cache, buf);
        return v;
    }

    for (int i = 0; i < g_dimgs_n; i++) {
        if (g_dimgs[i].load_base == handle) {
            uint64_t v = ocerz_image_self_resolve(&g_dimgs[i], buf);
            if (!v)
                v = image_symtab_resolve(&g_dimgs[i], buf);
            return v;
        }
    }
    if (g_run_cache) {
        uint64_t v = ocerz_cache_resolve(g_run_cache, buf);
        if (v)
            return v;
    }
    return disk_flat_resolve(buf);
}

int ocerz_dlclose(uint64_t handle)
{
    (void)handle;
    return 0;
}

uint64_t ocerz_dlerror(void)
{
    if (!g_dlerror_g)
        return 0;
    char *s = (char *)ocerz_g2h(g_dlerror_g);
    if (s[0] == '\0')
        return 0;
    return g_dlerror_g;
}

int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    if (ocerz_mem_init_identity(DYN_ARENA_SIZE) != OCERZ_OK)
        return OCERZ_ENOMEM;

    static OcerzCache cache;
    if (ocerz_cache_map(&cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot map shared cache for dynamic loading\n");
        return OCERZ_EIO;
    }
    g_run_cache = &cache;
    g_run_vm = vm;

    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf) {
        OCERZ_FATAL("cannot read %s\n", path);
        return OCERZ_EIO;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        OCERZ_FATAL("%s has no x86_64 slice\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    DynImage img;
    memset(&img, 0, sizeof img);
    img.slice = slice;
    snprintf(img.path, sizeof img.path, "%s", path);
    snprintf(img.install_name, sizeof img.install_name, "%s", path);
    snprintf(g_main_hostpath, sizeof g_main_hostpath, "%s", path);
    int r = map_segments(&img, 1);
    if (r != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", path);
        free(buf);
        return r;
    }
    if (!img.is_pie) {
        OCERZ_FATAL("%s is not PIE; dynamic loading needs a slidable image\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }
    if (img.main_entry == 0) {
        OCERZ_FATAL("%s has no LC_MAIN entry\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    RpathList main_rpaths;
    collect_rpaths(&img, NULL, &main_rpaths);
    load_disk_deps(&cache, &img, &main_rpaths);

    r = apply_fixups(&img, &cache);
    if (r != OCERZ_OK) {
        free(buf);
        return r;
    }
    if (img.cf_off == 0) {
        r = apply_classic_fixups(&img, &cache);
        if (r != OCERZ_OK) {
            free(buf);
            return r;
        }
    }

    DynFrame fr;
    memset(&fr, 0, sizeof fr);
    if (build_frame(path, argc, argv, envp, &fr) != OCERZ_OK) {
        OCERZ_FATAL("cannot build dynamic entry frame\n");
        free(buf);
        return OCERZ_ENOMEM;
    }

    uint64_t tsd = ocerz_map_anywhere(0x8000, PROT_READ | PROT_WRITE);
    if (tsd == 0) {
        free(buf);
        return OCERZ_ENOMEM;
    }
    uint64_t self = tsd + 0x4000;
    uint64_t gs = self + 0xe0;
    vm->cpu.gs_base = gs;
    ocerz_st(gs, 8, self);

    free(buf);

    ocerz_vm_install_handlers(vm);
    ocerz_commpage_init();
    if (ocerz_dyldapi_setup(&cache) != OCERZ_OK)
        OCERZ_LOG("dynamic: dyld API shim not installed\n");

    uint64_t environ_addr = ocerz_cache_resolve(&cache, "_environ");
    if (environ_addr) {
        ocerz_st(environ_addr, 8, fr.envp_arr);
        OCERZ_LOG("dynamic: environ=%#llx set to %#llx\n",
                  (unsigned long long)environ_addr, (unsigned long long)fr.envp_arr);
    }

    extern uint64_t g_main_path;
    if (fr.argv_arr)
        g_main_path = ocerz_ld(fr.argv_arr, 8);

    int ran_init = 0;
    int want_init = !getenv("OCERZ_NOINIT") && (img.links_dylib || getenv("OCERZ_INIT"));
    if (want_init) {
        uint64_t ia[5] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars };
        for (int i = 0; i < 5; i++)
            g_run_init_args[i] = ia[i];
        uint64_t init = find_dylib_init(&cache, "libSystem.B.dylib");
        if (init) {
            OCERZ_LOG("dynamic: running libSystem_initializer at %#llx\n", (unsigned long long)init);
            ocerz_vm_call(vm, init, ia, 5, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: libSystem_initializer returned\n");
            ran_init = 1;
        } else {
            OCERZ_LOG("dynamic: no libSystem initializer found\n");
        }
        if (ran_init && getenv("OCERZ_INITPHASE")) {
            uint64_t libsys = dep_find(&cache, "/usr/lib/libSystem.B.dylib");
            compute_eager_set(&cache, img.load_base);
            OCERZ_LOG("dynamic: eager init set = %d images (of closure)\n", g_eager_n);
            ocerz_tlv_register_closure(vm, &cache, img.load_base, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            g_init_visited_n = 0;
            run_load_phase(vm, &cache, img.load_base, fr.stack_top, libsys);
            if (vm->exited)
                return vm->exit_code;
            g_init_visited_n = 0;
            OCERZ_LOG("dynamic: running dependency-ordered initializer phase\n");
            run_init_phase(vm, &cache, img.load_base, ia, fr.stack_top, libsys);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: initializer phase complete\n");
        }
        if (ran_init)
            g_run_init_ready = 1;
    }

    OCERZ_LOG("dynamic: load_base=%#llx slide=%#llx main=%#llx\n",
              (unsigned long long)img.load_base, (unsigned long long)img.slide,
              (unsigned long long)img.main_entry);

    if (ran_init) {
        uint64_t margs[4] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr };
        uint64_t rv = ocerz_vm_call(vm, img.main_entry, margs, 4, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
        uint64_t exit_fn = ocerz_cache_resolve(&cache, "_exit");
        if (exit_fn) {
            uint64_t ea[1] = { rv & 0xff };
            ocerz_vm_call(vm, exit_fn, ea, 1, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
        }
        return (int)(rv & 0xff);
    }

    vm->cpu.gpr[OCERZ_RDI] = fr.argc;
    vm->cpu.gpr[OCERZ_RSI] = fr.argv_arr;
    vm->cpu.gpr[OCERZ_RDX] = fr.envp_arr;
    vm->cpu.gpr[OCERZ_RCX] = fr.apple_arr;
    vm->cpu.gpr[OCERZ_R8] = fr.progvars;
    uint64_t rsp = (fr.stack_top & ~0xfull) - 8;
    ocerz_st(rsp, 8, fr.exit_stub);
    vm->cpu.gpr[OCERZ_RSP] = rsp;
    vm->cpu.rip = img.main_entry;
    return ocerz_vm_run(vm);
}
