/*
 * Mini-dyld: loads, links and launches a dynamic x86_64 executable against the
 * shared cache.
 *
 * ---- binding ----
 * A cache dependency is bound in the SPECIFIC dylib the image linked, not by a
 * flat search across every cache image: openssl links libcrypto.46.dylib
 * (3.3.6) while the cache also holds libcrypto.44 (2.8.3) exporting the same
 * names, and the flat fallback bound OpenSSL_version to .44 and reported the
 * wrong version.  In the export trie, a terminal size of 0 with the name fully
 * consumed is not a miss: the node carries an empty edge to the terminal child,
 * which happens whenever a symbol is a strict prefix of others (_libiconv
 * versus _libiconv_open), so the search falls through to the child.
 *
 * Real dyld maps each segment with its initprot; this one maps every image
 * read-write so the copy and the fixups can land, then gives __TEXT its real
 * protection once fixups are done.  Leaving it writable is not merely untidy:
 * code sitting in a writable slot is treated as possibly self-modifying, and a
 * write-trapped page costs a fault per store to its data neighbours.
 *
 * ---- the initial stack ----
 * Every argument and environment entry goes onto the guest stack, counted first
 * and sized to the real need.  The arrays were once fixed at 64 with the
 * environment cut at 60, which silently dropped the last variables of a large
 * environment - and Wine's loader marks its one-time re-exec by appending
 * WINELOADERNOEXEC=1, so a launch with one variable too many re-exec'd every
 * Wine process forever (2026-09-06, Steam).  The three vectors are one
 * contiguous ascending run - argv NULL envp NULL apple NULL - because that is
 * what XNU's exec path produces and libSystem relies on it: apple is not passed
 * anywhere, it is found by walking off the end of envp.  Stacking them downward
 * instead put the argv vector where apple belongs, so that walk ran past argv's
 * terminator into the string area and handed strlen() the bytes of
 * "th_port=0x..." as a pointer, and python3 died there.
 *
 * The ProgramVars block handed to libSystem's initializer and to every image
 * initializer points at the real NXArgc, NXArgv, environ and __progname -
 * libdyld's in cache mode, the host's in native mode - and not at cells of its
 * own beside the vectors.  libc's _NSGetEnviron and _NSGetArgv answer whatever
 * that block points at, while the environ a program reads is libdyld's variable,
 * so with cells of their own the two came apart at the first setenv: setenv moved
 * *_NSGetEnviron() to a new array and environ went on naming the old one, and a
 * program that called setenv and then execve with environ handed its child an
 * environment without the variable.  On a real system they are one variable,
 * which the app_bundle native case checks against an arm64 build and cache mode.
 *
 * ---- initializers ----
 * Getting this phase right is most of the file.  Whether a program pulls in
 * CoreFoundation/Foundation/AppKit cannot be asked of the main executable's own
 * load commands, because a .app is a small stub linking one umbrella framework
 * and libSystem - Safari, a Cocoa app by any measure, read as "not a CF
 * program" and skipped the dependency-ordered phase entirely.  But libSystem's
 * own closure is deliberately not followed either: libxpc weak-links
 * XPCSupport, which links Foundation, so descending there makes every
 * dynamically linked program look like a Cocoa app, /usr/bin/sort included.
 *
 * The eager set is closed under dependencies as well as over data references,
 * because the walk recurses into every dependency but only RUNS the
 * initializers of images in the set - so an image in it whose dependency is
 * missing gets initialized on top of an uninitialized library.  That is how
 * Safari aborted in an Engram static initializer that called operator new
 * before libc++abi's own initializer had run.
 *
 * The order is dyld's: per image and bottom-up, that image's objc load
 * notification and then its own initializers, with every dependency already
 * finished.  Neither global order works - all +load first runs SiriTTSService's
 * ahead of libc++'s initializer, and all initializers first recurses
 * libsystem_malloc into its own zone setup.  Pruning on the done flag is what
 * keeps the walk small: libSystem's closure is marked done the moment
 * libSystem_initializer returns, so a library whose only dependency is
 * libSystem stops right there instead of descending through libxpc into
 * Foundation and dragging the whole system into its subtree.  Its objc load
 * notifications are therefore delivered before that prune can hide them.
 * LC_LOAD_UPWARD_DYLIB is skipped on purpose: an upward link is how a library
 * declares the back edge of a dependency cycle and is an ordering edge for
 * nothing.  Following it made CoreFoundation's upward link to
 * CoreServicesInternal a real edge, which put QuickLookThumbnailing, SiriTTS
 * and CoreML inside libc++abi's subtree, and libc++abi then sat unfinished on
 * the recursion stack while MLAssetIO's initializer called operator new into a
 * libc++ that had not been initialized yet.
 *
 * The dlopen closure (init_closure) collects its images in dependency
 * post-order and runs them in that order, never sorted by load address: a
 * dlopen'd image is mapped below the dependencies it pulls in, so address
 * order ran a dependent ahead of its dependency.  steamui initialized before
 * libtier0_s, and its first CUtlMemory growth called through g_pMemAlloc while
 * tier0's allocator singleton still had a null vtable.
 *
 * ---- thread-local variables ----
 * Two descriptor layouts share the same 24 bytes.  A static linker emits the
 * classic tlv_descriptor { thunk, key:u64, offset:u64 }, so the offset is at
 * +0x10; the shared cache ships the packed form dyld uses now - { thunk,
 * key:u32, offset:u32, initialContentDelta:i32, initialContentSize:u32 } -
 * where the offset is the u32 at +0xc and +0x10 is the delta.  We always WRITE
 * the packed form, so reading +0x10 unconditionally took the delta (0) for a
 * cache image and stored it over the real offset: every thread-local in the
 * image collapsed onto offset 0 and they all aliased each other.  SwiftUI reads
 * a thread-local holding its current PropertyList element that way, got the
 * small integer living at block offset 0, and cast it unconditionally to a
 * class - "Could not cast value of type 'NSIndirectTaggedPointerString'".  A
 * key is small enough that a classic descriptor's u64 leaves +0xc zero, so a
 * non-zero +0xc means the packed form is already there.
 *
 * ---- the hand-built main thread ----
 * libpthread caches __thread_selfid() at TSD base - 8, and guest libpthread
 * only fills it in _pthread_set_self_internal, which the hand-built main thread
 * never runs - so pthread_threadid_np() returned 0 on it.  The pthread firstfit
 * mutex protocol stores that tid as the lock owner, and owner 0 looks UNLOCKED,
 * so any mutex taken by the main thread had no mutual exclusion at all against
 * other threads (CFRunLoopSource locks among them): lost psynch wakes,
 * corrupted signaled flags, and the explorer sync freeze.
 *
 * OCERZ_DLOPEN_PIGGYBACK loads a chosen dylib, guest initializers and all,
 * right after the first dlopen whose path matches - a probe placed inside the
 * real process, on the same thread, at the same point in its life.
 *
 * ---- the executable's own identity ----
 * The main-executable path is realpath()'d so the guest always sees an
 * ABSOLUTE exec path - it feeds executable_path=, the DynFrame exec_path and
 * the host path used for matching.  Real macOS always resolves argv[0] to
 * absolute, and without it a `./prog` launch gave a relative
 * _dyld_get_image_name(0) and _NSGetExecutablePath: Steam's tier1 built
 * "/../Steam.AppBundle/..." out of one and V_RemoveDotSlashes asserted.
 * Pinned by the dynamic test exec_abspath.
 *
 * A dlopen of the main executable's own path returns the already-loaded main
 * image rather than mapping a second copy, matched against that host path raw
 * or realpath'd.  Native dyld never loads a second copy of the running
 * executable; deduping only against the loaded-image list was not enough, so
 * Steam's bootstrapper dlopening its own steam_osx mapped a duplicate, which
 * gave duplicate GURLHelper and UpdateEventHandlers objc classes and crashed
 * steamui on a null vtable.  Pinned by the dynamic test dlopen_self.
 *
 * ---- native mode ----
 * OCERZ_MODE_NATIVE maps no shared cache at all: the guest's system libraries
 * are to become synthesized x86 images whose exports bridge into the host's own
 * arm64 frameworks, and until one of those exists there is nothing for a system
 * import to bind to.  The loader is not forked for it.  Every consumer still
 * receives the same static OcerzCache, simply left zeroed, because a zeroed
 * cache already answers the way this mode needs: mapped is 0, so each resolve
 * reports not-found and the has-image test says no, and images_cnt is 0, so the
 * dependency map and the initializer search walk nothing, ran_init stays clear
 * and control reaches the cache-free process start at the tail of
 * ocerz_dyld_run.  A null pointer would say exactly the same thing at the price
 * of a null check in every one of those callers and a second path to keep
 * honest, so the zeroed struct is the one that travels.
 *
 * Unresolved imports are collected rather than announced one at a time.  In
 * cache mode a miss is a real failure and prints where it happens; in native
 * mode a program commonly imports several things no virtual library exports
 * yet, and the report is only useful if it names all of them, so a miss is
 * recorded by (library, symbol) pair in a fixed table, deduplicated, and the set
 * is reported once the main image's fixups are done - after which the process
 * stops with 71 rather than running a program with imports bound to zero.  The
 * library is the two-level ordinal's target install name, or (flat) when the
 * import names no ordinal.
 *
 * An import whose ordinal names a virtual image is resolved in that image and
 * nowhere else.  The flat search over every loaded image that follows a
 * two-level miss is there for disk dylibs whose re-exports the resolver does not
 * follow, and a virtual image re-exports nothing, so falling back would only let
 * a CoreFoundation import CoreFoundation does not export bind silently to a
 * libSystem export of the same name.
 *
 * A system library that a dependency names is not on disk at all on a modern
 * macOS - /usr/lib/libSystem.B.dylib exists only inside the cache - so in
 * native mode failing to read one is the ordinary case rather than a fault,
 * and it is logged rather than announced as fatal.  The collected import
 * report is what names the consequence, symbol by symbol.
 *
 * Which libraries ocerz synthesizes, and what each exports, comes from the API
 * database (apidb.h), whose version directory is chosen by the minimum macOS
 * the main image declares.  So before any dependency is loaded, native mode
 * reads that version out of the main image - the minos of an LC_BUILD_VERSION
 * whose platform is macOS, or else the version of an LC_VERSION_MIN_MACOSX -
 * and hands it to ocerz_apidb_set_minos; an image declaring neither, or only
 * another platform's version, hands over zero, which chooses the newest
 * directory.
 *
 * At the same point, before any framework can be opened and run its
 * initializers, native mode hands the guest's arguments to
 * ocerz_bridge_set_process_args, which makes them the host's own argv, argc and
 * program name, and it hands over the vectors on the guest's stack once the
 * frame is built, so that the host's variables name the argv main is given.  The
 * process path CoreFoundation takes the main bundle from is the main image's real
 * path, the one ocerz_dyld_main_path answers, which is set before the first
 * dependency is looked at; bridge.c hands it to CoreFoundation when it first opens
 * a framework.
 *
 * A name that ocerz synthesizes is answered rather than missed.  Before an
 * install name is expanded at all, native mode asks ocerz_vdylib_have whether
 * it has an image for it, and if it does the image is built in memory and
 * registered as an ordinary DynImage whose slice points at that buffer instead
 * of at the bytes of a file - which is the only difference, since every disk
 * image is already mapped and read out of a host buffer, so the segment copy,
 * the trie walker, the import resolver, dlopen and dladdr all carry on
 * unchanged.  Asking before the expansion keeps @-resolution off a path that
 * was never going to exist, and asking before the dep_find veto - which
 * refuses anything the cache already owns - leaves that veto dead in native
 * mode by position rather than by an exception written into it.
 *
 * A virtual image then returns right there instead of falling through the rest
 * of the disk loader.  It names no dependencies, it is built needing no fixups
 * because its one data slot holds a constant and its stubs reach that slot
 * rip-relative, it carries no Objective-C, and the dyld-API image list is a
 * cache-mode structure: ocerz_dyldapi_register_image ends in closure_add, and
 * the closure is allocated by ocerz_dyldapi_setup, which native mode does not
 * run.  Both of its call sites are therefore confined to cache mode, so that a
 * registration that would do nothing is not made at all.  Mapping the segments
 * and handing __TEXT back its protection is the whole of the work.
 *
 * Native mode runs no libSystem initializer, so the initializer phase that
 * cache mode gates on it never runs either, and for a while nothing ran a guest
 * image's own initializers at all: a C constructor or a C++ static object's
 * constructor was silently skipped.  Each guest image's __mod_init_func and
 * __init_offsets entries now run just before main, after every +load, the
 * dylibs in the reverse of the order they were loaded, so a library's
 * dependencies are initialized before it, and the main image last.  dyld
 * interleaves the two per image, +load then constructors, image by image; ocerz
 * runs all +load methods first, a difference only a constructor that sends a
 * message to a class in a later image could see.  Virtual images carry no
 * initializers, so walking them costs a header scan.
 *
 * A guest's main returns into a few bytes ocerz writes at the top of its stack.
 * In cache mode they make the exit syscall with main's result, since dyld's own
 * start has already been bypassed.  In native mode they call the virtual
 * libSystem's _exit export instead, the C library's exit, because that is what
 * start does on a real system and it is what runs the guest's atexit handlers
 * and flushes its stdio; the exit syscall stays behind the call only as the path
 * taken if that export is missing.
 *
 * No x86 Objective-C runtime runs in native mode, so nothing canonicalizes a
 * guest image's selectors, or reads its classes, the way the translated runtime
 * does in cache mode.  canonicalize_objc_selrefs hands each disk dylib, after
 * its fixups, to ocerz_objcbridge_fix_selrefs instead, which makes every
 * selector reference the host runtime's own SEL, and then to
 * ocerz_objcbridge_define_image, which makes the image's protocols, classes and
 * categories the host runtime's own and queues its +load methods; a dylib's
 * dependencies are loaded, and so defined, before it.  The main image gets the
 * same two passes right after the unresolved-import report, before any guest
 * code runs.  When the guest links libobjc, ocerz's uncaught-exception handler
 * is installed at that same point, once every host framework the guest links
 * has been opened and has installed its own handler for ocerz's to chain to
 * (objcbridge.h).  The queued +load methods run through
 * ocerz_objcbridge_run_loads once the guest's thread block is in place and the
 * handlers are installed, just before main, which is the first point guest code
 * can run at all.
 *
 * ---- thread-local variables in native mode ----
 * Descriptors are rewritten into the same packed form as in cache mode, but the
 * thunk word is left exactly as the fixups bound it.  In cache mode the import
 * binds to __tlv_bootstrap, which is not the code that answers, so the rewrite
 * points the word at tlv_get_addr beside it; in native mode the same import binds
 * to the synthesized libSystem's __tlv_bootstrap stub, which is itself the entry
 * that answers, so the binding already says the right thing and resolving the stub
 * a second time could only disagree with it.  The fixups have landed by the time
 * registration looks: each disk dylib is bound inside its own load, and the main
 * image before the unresolved-import report.
 *
 * The key is ocerz's own, a small integer per image counted from 1, not a pthread
 * key.  Cache mode's key is read by the guest's tlv_get_addr through the guest's
 * pthread_getspecific; here nothing reads it but ocerz_tlv_address, and there is no
 * x86 libpthread to create one in.  A dense key makes each thread's table a flat
 * array indexed by it, and leaves 0 to mean a descriptor registration never
 * rewrote - a classic descriptor's key word is 0 on disk - so such a descriptor is
 * refused instead of resolved into some other image's block.  The table has an
 * entry for every image the loader can hold, DYN_DIMG_MAX dylibs and the main
 * image, so no key can outgrow it and it never has to be reallocated under a
 * thread that is reading it.
 *
 * The table lives in guest memory, found through the slot OCERZ_TLV_TABLE_SLOT
 * past gs in the thread block ocerz built, and not in host thread-local storage.
 * An attached thread is torn down by a pthread key destructor, and by then the
 * host's own __thread storage has already been released and reads back as zero
 * (see the attach section of vm.c): a table reached through a host thread variable
 * would be invisible at the one moment it has to be freed, and every block behind
 * it would leak.  The slot also travels with the cpu.  A guest call runs a copy of
 * the thread's cpu and the copy carries the same gs, so a variable touched inside a
 * native callback is the same variable as outside it.  Nothing else in ocerz
 * writes that slot, and in native mode no x86 libpthread is there to use it.
 *
 * A thread's block for an image is made the first time that thread touches one of
 * the image's variables - copied from the template when the image has initialized
 * thread data, left zeroed when it has only __thread_bss - and from then on an
 * access is the descriptor, the table pointer and one entry.  An attached thread's
 * blocks and its table are freed when its personality is torn down, at thread exit
 * or detach, before its region is unmapped.  Each block is unmapped by the length
 * recorded when its key was given out, never by a length read back out of guest
 * memory, and a descriptor whose size disagrees with its key's is refused for the
 * same reason.  The main thread's blocks live as long as the process, and since
 * dlclose unloads nothing, a key always names the image it was given to.
 *
 * Registration runs for the main image and everything loaded with it right after
 * the unresolved-import report, before any guest code can run, and as soon as a
 * dlopen has loaded, before anything in the new images runs, over every image the
 * loader holds rather than only the new ones: a dependency
 * mapped by a dlopen that then failed is handed out by path to the next dlopen
 * without coming back through here.  An image already registered is skipped, and
 * that is not a formality - once a descriptor for a variable at offset 0 has been
 * packed it no longer reads as packed, so packing it again would store the
 * template delta as its offset.
 */
#include "ocerz/dyld.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/dyldapi.h"
#include "ocerz/mode.h"
#include "ocerz/vdylib.h"
#include "ocerz/apidb.h"
#include "ocerz/objcbridge.h"
#include "ocerz/bridge.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <crt_externs.h>

#define DYN_ARENA_SIZE (256ull << 30)
#define DYN_STACK_SIZE (8ull << 20)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
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
        return len >= 8 && rd32(buf + 4) == CPU_TYPE_X86_64 ? buf : NULL;
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
    int dynamic = -1, has_thread = 0, has_dylinker = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000028) {
            dynamic = 1;
            break;
        }
        if (cmd == LC_UNIXTHREAD)
            has_thread = 1;
        if (cmd == LC_LOAD_DYLINKER)
            has_dylinker = 1;
        lc += rd32(lc + 4);
    }
    if (dynamic < 0 && has_thread)
        dynamic = has_dylinker;
    free(buf);
    return dynamic;
}

#define DYN_SEG_MAX 16

typedef struct DynImage {
    const uint8_t *slice;
    uint8_t *owned_buf;
    char path[1024];
    char install_name[1024];
    char id_name[1024];
    uint64_t slide;
    uint64_t load_base;
    uint64_t main_entry;
    uint64_t thread_entry;
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
    int links_cf;
    uint64_t file_dev;
    uint64_t file_ino;
} DynImage;

#define DYN_DIMG_MAX 256
static DynImage g_dimgs[DYN_DIMG_MAX];
static int g_dimgs_n;
static DynImage g_main_dimg;
static int g_main_dimg_valid;
static char g_main_hostpath[1024];
static uint64_t g_main_dev, g_main_ino;

uint64_t ocerz_main_mh;

static OcerzCache *g_run_cache;

uint64_t ocerz_dyld_resolve_guest_sym(const char *name)
{
    if (!g_run_cache || !name) return 0;
    return ocerz_cache_resolve(g_run_cache, name);
}
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
        if ((g_dimgs[i].install_name[0] && strcmp(g_dimgs[i].install_name, iname) == 0) ||
            (g_dimgs[i].id_name[0] && strcmp(g_dimgs[i].id_name, iname) == 0))
            return &g_dimgs[i];
    return NULL;
}

static void dimg_record_id(DynImage *d)
{
    const uint8_t *mh = d->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t csize = rd32(lc + 4);
        uint32_t noff = rd32(lc + 8);
        if (rd32(lc) == LC_ID_DYLIB && noff < csize) {
            snprintf(d->id_name, sizeof d->id_name, "%.*s", (int)(csize - noff),
                     (const char *)(lc + noff));
            return;
        }
        lc += csize;
    }
}

static int file_identity(const char *path, uint64_t *dev, uint64_t *ino)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && st.st_ino != 0;
    close(fd);
    if (!ok)
        return 0;
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return 1;
}

static DynImage *dimg_find_by_identity(uint64_t dev, uint64_t ino)
{
    for (int i = 0; i < g_dimgs_n; i++)
        if (g_dimgs[i].file_ino == ino && g_dimgs[i].file_dev == dev)
            return &g_dimgs[i];
    return NULL;
}

void ocerz_dyld_dump_images(void)
{
    fprintf(stderr, "ocerz: IMGMAP[%d] n=%d arena_lo=%#llx\n",
            (int)getpid(), g_dimgs_n, (unsigned long long)ocerz_arena_lo);
    for (int i = 0; i < g_dimgs_n; i++) {
        DynImage *im = &g_dimgs[i];
        uint64_t lo = im->load_base, hi = lo;
        for (int s = 0; s < im->seg_count; s++) {
            uint64_t v = im->seg_vmaddr[s] + im->slide;
            if (v > hi) hi = v;
        }
        fprintf(stderr, "ocerz:   img[%d] base=%#llx slide=%#llx segs=%d hi~=%#llx %s\n",
                i, (unsigned long long)lo, (unsigned long long)im->slide,
                im->seg_count, (unsigned long long)hi,
                im->install_name[0] ? im->install_name : im->path);
    }
}

const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out)
{
    DynImage *best = NULL;
    for (int i = 0; i < g_dimgs_n; i++) {
        if (g_dimgs[i].load_base <= addr &&
            (!best || g_dimgs[i].load_base > best->load_base))
            best = &g_dimgs[i];
    }
    if (!best) return NULL;
    if (base_out) *base_out = best->load_base;
    return best->install_name[0] ? best->install_name : best->path;
}

const char *ocerz_dyld_main_path(void)
{
    return g_main_hostpath[0] ? g_main_hostpath : NULL;
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

    if (is_main && !img->is_pie) {
        img->load_base = text_vmaddr;
        img->slide = 0;
        lc = mh + sizeof(struct mach_header_64);
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint64_t vmaddr = rd64(lc + 24);
                uint64_t vmsize = rd64(lc + 32);
                uint32_t initprot = rd32(lc + 56);
                if (!(vmaddr == 0 && initprot == 0) && vmsize) {
                    if (vmaddr < OCERZ_LOW_LIMIT) {
                        if (vmaddr + vmsize > OCERZ_LOW_LIMIT)
                            return OCERZ_ENOMEM;
                        if (ocerz_mem_init_low_shadow() != OCERZ_OK)
                            return OCERZ_ENOMEM;
                    } else if (!(vmaddr >= ocerz_arena_lo && vmaddr + vmsize <= ocerz_arena_hi)) {
                        if (ocerz_mem_register_range(vmaddr, vmaddr + vmsize) != OCERZ_OK)
                            return OCERZ_ENOMEM;
                    }
                    if (ocerz_map_fixed(vmaddr, vmsize,
                                        PROT_READ | PROT_WRITE) != OCERZ_OK)
                        return OCERZ_ENOMEM;
                }
            }
            lc += rd32(lc + 4);
        }
    } else if (is_main) {
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
    if (is_main)
        ocerz_main_mh = img->load_base;

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
                img->seg_vmaddr[img->seg_count++] = vmaddr;
            if (!(vmaddr == 0 && initprot == 0) && filesize)
                memcpy(ocerz_g2h(vmaddr + img->slide), img->slice + fileoff, (size_t)filesize);
        } else if (cmd == 0x80000028) {
            img->main_entry = text_vmaddr + rd64(lc + 8) + img->slide;
        } else if (cmd == LC_UNIXTHREAD && csize >= 152 && rd32(lc + 8) == 4) {
            img->thread_entry = rd64(lc + 144) + img->slide;
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
            uint32_t noff = rd32(lc + 8);
            const char *dp = (const char *)(lc + noff);
            if (noff < rd32(lc + 4) &&
                (strstr(dp, "/CoreFoundation.framework/") ||
                 strstr(dp, "/Foundation.framework/") ||
                 strstr(dp, "/AppKit.framework/")))
                img->links_cf = 1;
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

static uint64_t image_export_trie(const uint8_t *slice, uint32_t *size_out)
{
    const uint8_t *mh = slice;
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

uint64_t ocerz_dyld_trie_resolve(const uint8_t *slice, uint64_t load_base,
                                 const char *sym, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    uint32_t tsize = 0;
    uint64_t toff = image_export_trie(slice, &tsize);
    if (!toff || !tsize)
        return 0;
    const uint8_t *start = slice + toff;
    const uint8_t *end = start + tsize;
    const uint8_t *p = start;
    const char *s = sym;
    while (p < end) {
        uint64_t term = self_uleb(&p, end);
        if (*s == '\0' && term != 0) {
            const uint8_t *tp = p;
            uint64_t flags = self_uleb(&tp, end);
            if (flags & 0x08)
                return 0;
            *found = 1;

            if ((flags & 0x03) == 0x02)
                return self_uleb(&tp, end);
            return load_base + self_uleb(&tp, end);
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

static uint64_t ocerz_image_self_resolve_ex(DynImage *img, const char *sym, int *found)
{
    return ocerz_dyld_trie_resolve(img->slice, img->load_base, sym, found);
}

static uint64_t ocerz_image_self_resolve(DynImage *img, const char *sym)
{
    return ocerz_image_self_resolve_ex(img, sym, NULL);
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

static uint64_t disk_flat_resolve_ex(const char *name, int *found)
{
    for (int i = 0; i < g_dimgs_n; i++) {
        int f = 0;
        uint64_t v = ocerz_image_self_resolve_ex(&g_dimgs[i], name, &f);
        if (f) {
            *found = 1;
            return v;
        }
    }
    return 0;
}

static uint64_t disk_flat_resolve(const char *name)
{
    int f = 0;
    return disk_flat_resolve_ex(name, &f);
}

static int expand_at_prefix(DynImage *loader, const char *name, char *out, size_t n);

#define NATIVE_MISS_MAX 256
struct native_miss { char lib[256]; char sym[256]; };
static struct native_miss g_native_miss[NATIVE_MISS_MAX];
static int g_native_miss_n;
static int g_native_miss_dropped;

static void native_miss_add(const char *lib, const char *sym)
{
    if (!lib || !lib[0])
        lib = "(flat)";
    for (int i = 0; i < g_native_miss_n; i++)
        if (strcmp(g_native_miss[i].sym, sym) == 0 && strcmp(g_native_miss[i].lib, lib) == 0)
            return;
    if (g_native_miss_n >= NATIVE_MISS_MAX) {
        g_native_miss_dropped++;
        return;
    }
    snprintf(g_native_miss[g_native_miss_n].lib, sizeof g_native_miss[0].lib, "%s", lib);
    snprintf(g_native_miss[g_native_miss_n].sym, sizeof g_native_miss[0].sym, "%s", sym);
    g_native_miss_n++;
}

static uint64_t resolve_import(OcerzCache *cache, DynImage *img, const char *name,
                               int libord, int weak)
{

    uint64_t value = 0;
    int found = 0;
    int virtual_dep = 0;
    const char *tgt = NULL;
    if (libord > 0) {
        tgt = dimg_ordinal_name(img, libord);
        if (tgt) {
            DynImage *dep = dimg_find_by_install_name(tgt);
            if (!dep)
                dep = dimg_find_by_path(tgt);
            if (!dep && tgt[0] == '@') {
                char ex[1024];
                if (expand_at_prefix(img, tgt, ex, sizeof ex)) {
                    dep = dimg_find_by_path(ex);
                    if (!dep)
                        dep = dimg_find_by_install_name(ex);
                }
            }
            if (dep) {
                value = ocerz_image_self_resolve_ex(dep, name, &found);
                virtual_dep = ocerz_mode == OCERZ_MODE_NATIVE && ocerz_vdylib_have(tgt);
            }
            if (!found && !dep && tgt[0] != '@')
                value = ocerz_cache_resolve_in_image(cache, tgt, name, &found);
        }
    }
    if (!found)
        value = ocerz_cache_resolve_ex(cache, name, &found);
    if (!found && (libord == -3 || libord == 0 || libord == -2))
        value = ocerz_image_self_resolve_ex(img, name, &found);
    if (!found && !virtual_dep)
        value = disk_flat_resolve_ex(name, &found);
    if (!found && !weak) {
        if (ocerz_mode == OCERZ_MODE_NATIVE)
            native_miss_add(tgt, name);
        else
            OCERZ_FATAL("unresolved import: %s\n", name);
    }
    return value;
}

static void protect_ro_segments(DynImage *img)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_TEXT_RO") ? 1 : 0;
    if (dis || !img->slice) return;
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 56);
            if (vmsize && !(vmaddr == 0 && initprot == 0) && !(initprot & 2) && (initprot & 4) &&
                memcmp(lc + 8, "__TEXT", 7) == 0)
                ocerz_protect(vmaddr + img->slide, vmsize, PROT_READ | PROT_EXEC);
        }
        lc += rd32(lc + 4);
    }
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
    uint64_t exec_path;
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
    int envc = 0;
    while (envp && envp[envc])
        envc++;
    char apple0[2048];
    snprintf(apple0, sizeof apple0, "executable_path=%s", path);
    size_t need = strlen(apple0) + 1 + 512;
    for (int i = 0; i < argc; i++)
        need += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++)
        need += strlen(envp[i]) + 1;
    need += ((size_t)argc + (size_t)envc + 16) * 8 + 256;
    uint64_t aux_size = 1u << 20;
    if (need + 65536 > aux_size)
        aux_size = (need + 65536 + 0x3fff) & ~(uint64_t)0x3fff;
    uint64_t aux = ocerz_map_anywhere(aux_size, PROT_READ | PROT_WRITE);
    uint64_t stack = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
    if (aux == 0 || stack == 0)
        return OCERZ_ENOMEM;
    uint64_t *argv_g = (uint64_t *)calloc((size_t)argc + 1, sizeof *argv_g);
    uint64_t *envp_g = (uint64_t *)calloc((size_t)envc + 1, sizeof *envp_g);
    if (!argv_g || !envp_g) {
        free(argv_g);
        free(envp_g);
        return OCERZ_ENOMEM;
    }

    static const uint8_t exit_stub[] = { 0x89, 0xc7, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05 };
    out->exit_stub = stack + DYN_STACK_SIZE - 64;
    memcpy(ocerz_g2h(out->exit_stub), exit_stub, sizeof exit_stub);
    out->stack_top = (out->exit_stub - 256) & ~0xfull;

    uint64_t sp = aux + aux_size;
    uint64_t apple_g[8];

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

    apple_g[1] = put_str(&sp, "stack_guard=0x6f6365727a5f6700");
    apple_g[2] = put_str(&sp, "ptr_munge=0xa3f1c2b4d5e60718");
    apple_g[3] = put_str(&sp, "malloc_entropy=0x91827364a5b6c7d8,0x1f2e3d4c5b6a7988");
    apple_g[4] = put_str(&sp, stkbuf);
    apple_g[5] = put_str(&sp, thbuf);
    out->exec_path = put_str(&sp, path);
    int applec = 6;

    sp &= ~0xfull;

    uint64_t vec_bytes = ((uint64_t)argc + 1 + (uint64_t)envc + 1 +
                          (uint64_t)applec + 1) * 8;
    uint64_t argv_arr = (sp - vec_bytes) & ~0xfull;
    for (int i = 0; i < argc; i++)
        ocerz_st(argv_arr + (uint64_t)i * 8, 8, argv_g[i]);
    ocerz_st(argv_arr + (uint64_t)argc * 8, 8, 0);

    uint64_t envp_arr = argv_arr + ((uint64_t)argc + 1) * 8;
    for (int i = 0; i < envc; i++)
        ocerz_st(envp_arr + (uint64_t)i * 8, 8, envp_g[i]);
    ocerz_st(envp_arr + (uint64_t)envc * 8, 8, 0);

    uint64_t apple_arr = envp_arr + ((uint64_t)envc + 1) * 8;
    for (int i = 0; i < applec; i++)
        ocerz_st(apple_arr + (uint64_t)i * 8, 8, apple_g[i]);
    ocerz_st(apple_arr + (uint64_t)applec * 8, 8, 0);

    uint64_t cells = (argv_arr - 8 * 8) & ~0xfull;
    const char *slash = strrchr(argv[0], '/');
    uint64_t leaf = argv_g[0] + (slash ? (uint64_t)(slash - argv[0] + 1) : 0);
    ocerz_st(cells + 0, 4, (uint64_t)argc);
    ocerz_st(cells + 8, 8, argv_arr);
    ocerz_st(cells + 16, 8, envp_arr);
    ocerz_st(cells + 24, 8, leaf);

    uint64_t pv = cells - 48;
    ocerz_st(pv + 0, 8, ocerz_main_mh ? ocerz_main_mh : ocerz_arena_lo);
    ocerz_st(pv + 8, 8, cells + 0);
    ocerz_st(pv + 16, 8, cells + 8);
    ocerz_st(pv + 24, 8, cells + 16);
    ocerz_st(pv + 32, 8, cells + 24);

    out->argc = (uint64_t)argc;
    out->argv_arr = argv_arr;
    out->envp_arr = envp_arr;
    out->apple_arr = apple_arr;
    out->progvars = pv;
    free(argv_g);
    free(envp_g);
    return OCERZ_OK;
}

static uint64_t find_dylib_init(OcerzCache *cache, const char *substr)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (!path || !strstr(path, substr) || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
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

#define DEPMAP_BITS 13
static struct { const char *path; uint64_t mh; } g_depmap[1u << DEPMAP_BITS];
static int g_depmap_built;
static uint32_t depmap_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static void depmap_build(OcerzCache *cache)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (!mh || !p) continue;
        uint32_t h = depmap_hash(p) & ((1u << DEPMAP_BITS) - 1);
        while (g_depmap[h].path) {
            if (strcmp(g_depmap[h].path, p) == 0) break;
            h = (h + 1) & ((1u << DEPMAP_BITS) - 1);
        }
        if (!g_depmap[h].path) { g_depmap[h].path = p; g_depmap[h].mh = mh; }
    }
    g_depmap_built = 1;
}
static uint64_t dep_find(OcerzCache *cache, const char *path)
{
    if (!g_depmap_built) depmap_build(cache);
    uint32_t h = depmap_hash(path) & ((1u << DEPMAP_BITS) - 1);
    for (unsigned n = 0; n < (1u << DEPMAP_BITS) && g_depmap[h].path; n++) {
        if (strcmp(g_depmap[h].path, path) == 0) return g_depmap[h].mh;
        h = (h + 1) & ((1u << DEPMAP_BITS) - 1);
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
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
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
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
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
                        uint64_t v = rd64((const uint8_t *)ocerz_g2h(pp));
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
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
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

static int is_libsystem_path(const char *p)
{
    return strstr(p, "/usr/lib/system/") != NULL ||
           strcmp(p, "/usr/lib/libSystem.B.dylib") == 0;
}

static int closure_links_cf(OcerzCache *cache, uint64_t main_mh)
{
    static uint64_t seen[EAGER_MAX];
    int n = 0;
    if (main_mh)
        seen[n++] = main_mh;
    for (int i = 0; i < n; i++) {
        const uint8_t *h = (const uint8_t *)ocerz_g2h(seen[i]);
        if (rd32(h) != MH_MAGIC_64)
            continue;
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
                uint32_t noff = rd32(lc + 8);
                if (noff < rd32(lc + 4)) {
                    const char *dp = (const char *)(lc + noff);
                    if (strstr(dp, "/CoreFoundation.framework/") ||
                        strstr(dp, "/Foundation.framework/") ||
                        strstr(dp, "/AppKit.framework/"))
                        return 1;
                    if (is_libsystem_path(dp))
                        continue;
                    uint64_t dmh = dep_mh(cache, dp);
                    if (dmh && n < EAGER_MAX) {
                        int dup = 0;
                        for (int k = 0; k < n; k++)
                            if (seen[k] == dmh) { dup = 1; break; }
                        if (!dup)
                            seen[n++] = dmh;
                    }
                }
            }
            lc += rd32(lc + 4);
        }
    }
    return 0;
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
    for (int i = 0; i < g_eager_n; i++) {
        eager_add_direct_deps(cache, g_eager[i]);
        scan_uses(g_eager[i]);
    }
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

typedef struct TlvSections {
    uint64_t descs;
    uint64_t descs_size;
    uint64_t tmpl;
    uint64_t block_size;
    int has_data;
} TlvSections;

static int tlv_find_sections(uint64_t mh, TlvSections *ts)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return 0;
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
    ts->descs = (uint64_t)((int64_t)vars_addr + slide);
    ts->descs_size = vars_addr ? vars_size : 0;
    ts->tmpl = (uint64_t)((int64_t)tmpl_lo + slide);
    ts->block_size = (tmpl_hi > tmpl_lo) ? (tmpl_hi - tmpl_lo) : 0;
    ts->has_data = (data_lo != ~0ull);
    return 1;
}

static void tlv_pack_descriptors(const TlvSections *ts, uint64_t thunk, uint32_t key)
{
    for (uint64_t off = 0; off + 24 <= ts->descs_size; off += 24) {
        uint64_t desc = ts->descs + off;
        uint32_t packed_off = (uint32_t)ocerz_ld(desc + 0xc, 4);
        uint32_t var_off = packed_off ? packed_off
                                      : (uint32_t)ocerz_ld(desc + 0x10, 8);
        int32_t self_rel = ts->has_data
            ? (int32_t)((int64_t)ts->tmpl - (int64_t)(desc + 0x10))
            : 0;
        if (thunk)
            ocerz_st(desc + 0, 8, thunk);
        ocerz_st(desc + 8, 4, key);
        ocerz_st(desc + 0xc, 4, var_off);
        ocerz_st(desc + 0x10, 4, (uint32_t)self_rel);
        ocerz_st(desc + 0x14, 4, (uint32_t)ts->block_size);
    }
}

static void ocerz_tlv_register_image(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                                     uint64_t stack_top)
{
    if (!mh || tlv_is_registered(mh))
        return;
    TlvSections ts;
    if (!tlv_find_sections(mh, &ts))
        return;
    if (g_tlv_registered_n < TLV_REG_MAX)
        g_tlv_registered[g_tlv_registered_n++] = mh;
    if (ts.descs_size < 24)
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

    tlv_pack_descriptors(&ts, tlv_get_addr, key);
    OCERZ_LOG("dynamic: TLV: registered mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
              (unsigned long long)mh, key, (unsigned long long)ts.block_size,
              (unsigned long long)ts.descs, (unsigned long long)ts.descs_size);
}

#define NATIVE_TLV_KEYS (DYN_DIMG_MAX + 1)
#define NATIVE_TLV_TABLE_BYTES ((uint64_t)(NATIVE_TLV_KEYS + 1) * 8)

static uint32_t g_native_tlv_size[NATIVE_TLV_KEYS + 1];
static _Atomic uint32_t g_native_tlv_keys;

static void native_tlv_register_image(uint64_t mh)
{
    if (!mh || tlv_is_registered(mh))
        return;
    TlvSections ts;
    if (!tlv_find_sections(mh, &ts))
        return;
    if (g_tlv_registered_n < TLV_REG_MAX)
        g_tlv_registered[g_tlv_registered_n++] = mh;
    if (ts.descs_size < 24)
        return;
    uint32_t key = g_native_tlv_keys + 1;
    if (key > NATIVE_TLV_KEYS) {
        OCERZ_LOG("dynamic: TLV: no native key left for mh=%#llx\n", (unsigned long long)mh);
        return;
    }
    g_native_tlv_size[key] = (uint32_t)ts.block_size;
    g_native_tlv_keys = key;
    tlv_pack_descriptors(&ts, 0, key);
    OCERZ_LOG("dynamic: TLV: native mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
              (unsigned long long)mh, key, (unsigned long long)ts.block_size,
              (unsigned long long)ts.descs, (unsigned long long)ts.descs_size);
}

static void native_tlv_register_loaded(uint64_t main_mh)
{
    native_tlv_register_image(main_mh);
    for (int i = 0; i < g_dimgs_n; i++)
        native_tlv_register_image(g_dimgs[i].load_base);
}

static uint64_t native_tlv_first_touch(uint64_t gs_base, uint64_t desc, uint32_t key,
                                       uint32_t off, uint32_t size)
{
    if (key > g_native_tlv_keys || size != g_native_tlv_size[key]) {
        OCERZ_LOG("dynamic: TLV: descriptor %#llx names key %u with a %u-byte block, which no registered image has\n",
                  (unsigned long long)desc, key, size);
        return 0;
    }
    uint64_t slot = gs_base + OCERZ_TLV_TABLE_SLOT;
    uint64_t table = ocerz_ld(slot, 8);
    if (!table) {
        table = ocerz_map_anywhere(NATIVE_TLV_TABLE_BYTES, PROT_READ | PROT_WRITE);
        if (!table) {
            OCERZ_LOG("dynamic: TLV: no guest memory for the thread table of gs=%#llx\n",
                      (unsigned long long)gs_base);
            return 0;
        }
        ocerz_st(slot, 8, table);
    }
    uint64_t block = ocerz_map_anywhere(size, PROT_READ | PROT_WRITE);
    if (!block) {
        OCERZ_LOG("dynamic: TLV: no guest memory for a %u-byte block of key %u\n", size, key);
        return 0;
    }
    int32_t delta = (int32_t)rd32((const uint8_t *)ocerz_g2h(desc + 0x10));
    if (delta)
        memcpy(ocerz_g2h(block), ocerz_g2h((uint64_t)((int64_t)desc + 0x10 + delta)), size);
    ocerz_st(table + (uint64_t)key * 8, 8, block);
    return block + off;
}

uint64_t ocerz_tlv_address(OcerzCPU *cpu, uint64_t desc)
{
    const uint8_t *d = (const uint8_t *)ocerz_g2h(desc);
    uint32_t key = rd32(d + 8);
    uint32_t off = rd32(d + 0xc);
    uint32_t size = rd32(d + 0x14);
    uint64_t gs_base = cpu->gs_base;
    if (key == 0 || key > NATIVE_TLV_KEYS || size == 0 || off > size || gs_base == 0)
        return 0;
    uint64_t table = ocerz_ld(gs_base + OCERZ_TLV_TABLE_SLOT, 8);
    uint64_t block = table ? ocerz_ld(table + (uint64_t)key * 8, 8) : 0;
    if (block)
        return block + off;
    return native_tlv_first_touch(gs_base, desc, key, off, size);
}

void ocerz_tlv_release_thread(uint64_t gs_base)
{
    if (ocerz_mode != OCERZ_MODE_NATIVE || gs_base == 0)
        return;
    uint64_t slot = gs_base + OCERZ_TLV_TABLE_SLOT;
    uint64_t table = ocerz_ld(slot, 8);
    if (!table)
        return;
    ocerz_st(slot, 8, 0);
    uint32_t keys = g_native_tlv_keys;
    for (uint32_t key = 1; key <= keys && key <= NATIVE_TLV_KEYS; key++) {
        uint64_t block = ocerz_ld(table + (uint64_t)key * 8, 8);
        if (block && g_native_tlv_size[key])
            ocerz_unmap(block, g_native_tlv_size[key]);
    }
    ocerz_unmap(table, NATIVE_TLV_TABLE_BYTES);
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

static const char *image_id_name(uint64_t mh)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return NULL;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        if (rd32(lc) == LC_ID_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                return (const char *)(lc + noff);
        }
        lc += rd32(lc + 4);
    }
    return NULL;
}

static int g_init_dlopen_restricted;
static int g_foundation_inited;
static int image_is_objc_core(uint64_t mh)
{
    const char *id = image_id_name(mh);
    return id && (strstr(id, "/Foundation.framework/") ||
                  strstr(id, "/CoreFoundation.framework/") ||
                  strstr(id, "/libobjc.A.dylib"));
}
static int image_is_foundation(uint64_t mh)
{
    const char *id = image_id_name(mh);
    return id && strstr(id, "/Foundation.framework/") != NULL;
}

static void run_image_inits(OcerzVM *vm, uint64_t mh, const uint64_t *ia, uint64_t stack_top)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    const char *iscan = getenv("OCERZ_INITSCAN");
    int do_scan = iscan && (iscan[0] == '*' || strtoull(iscan, NULL, 0) == mh);
    if (do_scan) {
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint32_t ns = rd32(lc + 64);
                const uint8_t *sec = lc + 72;
                for (uint32_t s = 0; s < ns; s++) {
                    uint32_t fl = rd32(sec + 64);
                    fprintf(stderr, "INITSCAN mh=%#llx seg=%.16s sect=%.16s type=%#x addr=%#llx sz=%#llx\n",
                            (unsigned long long)mh, (const char *)(sec + 16), (const char *)sec,
                            fl & 0xff, (unsigned long long)rd64(sec + 32), (unsigned long long)rd64(sec + 40));
                    sec += 80;
                }
            } else {
                fprintf(stderr, "INITSCAN mh=%#llx LC cmd=%#x\n", (unsigned long long)mh, cmd);
            }
            lc += rd32(lc + 4);
        }
        lc = h + sizeof(struct mach_header_64);
    }
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
    if (image_is_foundation(mh)) {
        if (!g_foundation_inited && getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "ocerz: FOUNDATION-INITED mh=%#llx\n", (unsigned long long)mh);
        g_foundation_inited = 1;
    }
}

#define INIT_VISITED_MAX 8192
static uint64_t g_init_visited[INIT_VISITED_MAX];
static uint32_t g_init_gen[INIT_VISITED_MAX];
static uint8_t g_init_done[INIT_VISITED_MAX];
static uint8_t g_load_done[INIT_VISITED_MAX];
static uint8_t g_init_being[INIT_VISITED_MAX];
static int g_init_visited_n;
static uint32_t g_init_cur_gen;
static int g_init_force;
static int g_init_collect_depth;
static uint64_t g_libsys_mh;

static int init_mark(uint64_t mh)
{
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return i;
    if (g_init_visited_n >= INIT_VISITED_MAX)
        return -1;
    int i = g_init_visited_n++;
    g_init_visited[i] = mh;
    g_init_gen[i] = 0;
    g_init_done[i] = 0;
    g_init_being[i] = 0;
    return i;
}

static int init_is_done(uint64_t mh)
{
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return g_init_done[i];
    return 0;
}

static int is_umbrella_path(const char *p)
{
    return p && strncmp(p, "/usr/lib/system/", 16) == 0;
}

static void init_mark_done_closure(OcerzCache *cache, uint64_t mh)
{
    if (!mh)
        return;
    int idx = init_mark(mh);
    if (idx < 0 || g_init_done[idx] || g_init_being[idx])
        return;
    g_init_being[idx] = 1;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) == MH_MAGIC_64) {
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                cmd == LC_REEXPORT_DYLIB) {
                uint32_t noff = rd32(lc + 8);
                const char *dpath = (const char *)(lc + noff);
                if (noff < rd32(lc + 4) && is_umbrella_path(dpath))
                    init_mark_done_closure(cache, dep_mh(cache, dpath));
            }
            lc += rd32(lc + 4);
        }
    }
    g_init_being[idx] = 0;
    g_init_done[idx] = 1;
}

static int dylib_lc_is_init_dep(const uint8_t *lc)
{
    uint32_t cmd = rd32(lc);
    if (cmd != LC_LOAD_DYLIB && cmd != LC_LOAD_WEAK_DYLIB && cmd != LC_REEXPORT_DYLIB)
        return 0;
    if (cmd != LC_REEXPORT_DYLIB && rd32(lc + 4) >= sizeof(struct dylib_use_command) &&
        rd32(lc + 8) == sizeof(struct dylib_use_command) && rd32(lc + 12) == DYLIB_USE_MARKER)
        return (rd32(lc + 24) & DYLIB_USE_UPWARD) == 0;
    return 1;
}

static void init_collect(OcerzCache *cache, uint64_t mh, uint64_t *list, int *n, int cap)
{
    if (!mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx < 0 || g_init_done[idx] || g_init_being[idx])
        return;
    g_init_being[idx] = 1;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        if (dylib_lc_is_init_dep(lc)) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                init_collect(cache, dep_mh(cache, (const char *)(lc + noff)), list, n, cap);
        }
        lc += rd32(lc + 4);
    }
    if (*n < cap)
        list[(*n)++] = mh;
}

#define INIT_CLOSURE_CAP 4096

static void init_closure(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                         const uint64_t *ia, uint64_t stack_top)
{
    if (vm->exited || !mh)
        return;
    static uint64_t list[INIT_CLOSURE_CAP];
    uint64_t *l = list;
    int reentrant = (g_init_collect_depth > 0);
    if (reentrant)
        l = (uint64_t *)malloc(sizeof(uint64_t) * INIT_CLOSURE_CAP);
    if (!l)
        return;
    g_init_collect_depth++;
    int n = 0;
    init_collect(cache, mh, l, &n, INIT_CLOSURE_CAP);
    for (int i = 0; i < n; i++) {
        int idx = init_mark(l[i]);
        if (idx >= 0)
            g_init_being[idx] = 0;
    }
    for (int i = 0; i < n && !vm->exited; i++) {
        uint64_t m = l[i];
        int idx = init_mark(m);
        if (idx < 0 || g_init_done[idx])
            continue;
        if (g_init_dlopen_restricted && !image_is_objc_core(m)) {
            if (getenv("OCERZ_INITTRACE"))
                fprintf(stderr, "INITCLOSURE skip-restricted mh=%#llx\n", (unsigned long long)m);
            continue;
        }
        if (getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "INITCLOSURE run mh=%#llx\n", (unsigned long long)m);
        int prev_tol = ocerz_init_tolerant;
        ocerz_init_tolerant = 1;
        run_image_inits(vm, m, ia, stack_top);
        ocerz_init_tolerant = prev_tol;
        g_init_done[idx] = 1;
    }
    g_init_collect_depth--;
    if (reentrant)
        free(l);
}

static void run_init_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                           const uint64_t *ia, uint64_t stack_top, uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx >= 0 && (g_init_done[idx] || g_init_gen[idx] == g_init_cur_gen)) {
        if (getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "INITTRACE skip mh=%#llx done=%d gen=%u cur=%u\n",
                    (unsigned long long)mh, g_init_done[idx], g_init_gen[idx], g_init_cur_gen);
        return;
    }
    if (idx >= 0)
        g_init_gen[idx] = g_init_cur_gen;
    if (getenv("OCERZ_INITTRACE"))
        fprintf(stderr, "INITTRACE enter mh=%#llx force=%d\n",
                (unsigned long long)mh, g_init_force);

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    const char *cfdump = getenv("OCERZ_CFDUMP");
    if (cfdump && mh == strtoull(cfdump, NULL, 0)) {
        fprintf(stderr, "CFDUMP mh=%#llx ncmds=%u (h=%p)\n",
                (unsigned long long)mh, ncmds, (const void *)h);
        const uint8_t *p = lc;
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t c = rd32(p), sz = rd32(p + 4);
            if (c == 0xc || c == 0x8000001f || c == 0x80000018 || c == 0x80000022)
                fprintf(stderr, "  CFDUMP [%u] cmd=%#x sz=%u name=%s\n",
                        j, c, sz, (const char *)(p + rd32(p + 8)));
            p += sz;
        }
    }
    for (uint32_t j = 0; j < ncmds; j++) {
        if (dylib_lc_is_init_dep(lc)) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4)) {
                uint64_t dmh = dep_mh(cache, (const char *)(lc + noff));
                if (getenv("OCERZ_INITEDGE"))
                    fprintf(stderr, "INITEDGE %#llx -> %#llx \"%s\"\n",
                            (unsigned long long)mh, (unsigned long long)dmh,
                            (const char *)(lc + noff));
                if (!dmh && getenv("OCERZ_INITTRACE"))
                    fprintf(stderr, "INITTRACE dep-unresolved mh=%#llx dep=\"%s\"\n",
                            (unsigned long long)mh, (const char *)(lc + noff));
                run_init_phase(vm, cache, dmh, ia, stack_top, skip_mh);
            }
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_init_force || g_eager_n == 0 || eager_has(mh))) {
        if (idx < 0 || !g_load_done[idx]) {
            ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
            if (idx >= 0)
                g_load_done[idx] = 1;
            if (vm->exited)
                return;
        }
        run_image_inits(vm, mh, ia, stack_top);
        if (idx >= 0)
            g_init_done[idx] = 1;
    } else if (getenv("OCERZ_INITLOG")) {
        fprintf(stderr, "INITSKIP mh=%#llx eager=%d is_libsystem=%d\n",
                (unsigned long long)mh, eager_has(mh), mh == skip_mh);
    }
}

static void run_load_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh, uint64_t stack_top,
                           uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx >= 0 && (g_load_done[idx] || g_init_gen[idx] == g_init_cur_gen))
        return;
    if (idx >= 0)
        g_init_gen[idx] = g_init_cur_gen;

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        if (dylib_lc_is_init_dep(lc)) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                run_load_phase(vm, cache, dep_mh(cache, (const char *)(lc + noff)),
                               stack_top, skip_mh);
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_init_force || g_eager_n == 0 || eager_has(mh))) {
        ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
        if (idx >= 0)
            g_load_done[idx] = 1;
    }
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
    const char *base, *rest;
    if (strncmp(name, "@executable_path", 16) == 0 && (name[16] == '/' || name[16] == '\0')) {
        base = g_main_hostpath;
        rest = name + 16;
    } else if (strncmp(name, "@loader_path", 12) == 0 && (name[12] == '/' || name[12] == '\0')) {
        base = loader ? loader->path : g_main_hostpath;
        rest = name + 12;
    } else {
        return 0;
    }
    char dir[1024];
    path_dirname(base, dir, sizeof dir);
    snprintf(out, n, "%s%s", dir, rest);
    return 1;
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

static void canonicalize_objc_selrefs(DynImage *img)
{
    int64_t slide = img->slide;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(img->load_base);
    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        ocerz_objcbridge_fix_selrefs(h, slide);
        ocerz_objcbridge_define_image(h, slide);
        return;
    }
    if (rd32(h) != MH_MAGIC_64)
        return;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++) {
                if (strncmp(sc[j].sectname, "__objc_selrefs", 16) != 0)
                    continue;
                uint64_t a = sc[j].addr + slide, e = a + sc[j].size;
                for (uint64_t pp = a; pp + 8 <= e; pp += 8) {
                    uint64_t name = rd64((const uint8_t *)ocerz_g2h(pp));
                    if (!name)
                        continue;
                    uint64_t canon =
                        ocerz_dyldapi_canonical_selector((const char *)ocerz_g2h(name));
                    if (canon && canon != name)
                        wr64((uint8_t *)ocerz_g2h(pp), canon);
                }
            }
        }
        lc += l->cmdsize;
    }
}

static DynImage *load_disk_dylib(OcerzCache *cache, const char *install_name, DynImage *loader,
                                 const RpathList *rpaths)
{
    DynImage *by_name = dimg_find_by_install_name(install_name);
    if (by_name)
        return by_name;
    if (ocerz_mode == OCERZ_MODE_NATIVE && ocerz_vdylib_have(install_name)) {
        if (g_dimgs_n >= DYN_DIMG_MAX) {
            OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
            return NULL;
        }
        size_t vlen = 0;
        uint8_t *vbuf = ocerz_vdylib_image(install_name, &vlen);
        if (!vbuf || vlen == 0) {
            OCERZ_FATAL("cannot synthesize %s\n", install_name);
            free(vbuf);
            return NULL;
        }
        DynImage *v = &g_dimgs[g_dimgs_n++];
        memset(v, 0, sizeof *v);
        v->slice = vbuf;
        v->owned_buf = vbuf;
        snprintf(v->path, sizeof v->path, "%s", install_name);
        snprintf(v->install_name, sizeof v->install_name, "%s", install_name);
        dimg_record_id(v);
        if (map_segments(v, 0) != OCERZ_OK) {
            OCERZ_FATAL("cannot map segments of virtual %s\n", install_name);
            g_dimgs_n--;
            free(vbuf);
            return NULL;
        }
        protect_ro_segments(v);
        OCERZ_LOG("dynamic: registered virtual dylib %s at load_base=%#llx slide=%#llx\n",
                  install_name, (unsigned long long)v->load_base,
                  (unsigned long long)v->slide);
        return v;
    }
    char resolved[1024];
    if (!expand_install_name(loader, install_name, rpaths, resolved, sizeof resolved))
        return NULL;
    if (resolved[0] == '@')
        return NULL;
    if (dep_find(cache, resolved) != 0)
        return NULL;

    char canon[1024];
    if (ocerz_canon_dylib_path(resolved, canon, sizeof canon))
        snprintf(resolved, sizeof resolved, "%s", canon);
    DynImage *existing = dimg_find_by_path(resolved);
    if (existing)
        return existing;
    uint64_t fdev = 0, fino = 0;
    if (file_identity(resolved, &fdev, &fino) && (existing = dimg_find_by_identity(fdev, fino)))
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
        return NULL;
    }

    size_t flen = 0;
    uint8_t *buf = read_file(resolved, &flen);
    if (!buf) {
        if (ocerz_mode == OCERZ_MODE_NATIVE)
            OCERZ_LOG("dynamic: %s is not on disk, native mode has no image for it yet\n",
                      resolved);
        else
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
    d->file_dev = fdev;
    d->file_ino = fino;
    dimg_record_id(d);

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
    protect_ro_segments(d);

    if (ocerz_mode == OCERZ_MODE_CACHE)
        ocerz_dyldapi_register_image(d->load_base, d->path);
    canonicalize_objc_selrefs(d);
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLPATH disk-dep load_base=%#llx install=%s path=%s\n",
                (unsigned long long)d->load_base, d->install_name, resolved);
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
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLERR %s\n", host);
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
    file_identity(install_path, &d->file_dev, &d->file_ino);
    dimg_record_id(d);
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
    if (ocerz_mode == OCERZ_MODE_CACHE)
        ocerz_dyldapi_register_image(d->load_base, d->path);
    canonicalize_objc_selrefs(d);
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLPATH dlopen load_base=%#llx install=%s path=%s\n",
                (unsigned long long)d->load_base, d->install_name, install_path);
    OCERZ_LOG("dynamic: dlopen loaded %s at load_base=%#llx slide=%#llx\n",
              install_path, (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

int ocerz_canon_dylib_path(const char *path, char *out, size_t outsz)
{
    char cur[PATH_MAX];
    if (snprintf(cur, sizeof cur, "%s", path) >= (int)sizeof cur)
        return 0;
    for (int iter = 0; iter < 32; iter++) {
        char link[PATH_MAX];
        ssize_t n = readlink(cur, link, sizeof link - 1);
        if (n < 0)
            break;
        link[n] = 0;
        char next[PATH_MAX];
        if (link[0] == '/') {
            if (snprintf(next, sizeof next, "%s", link) >= (int)sizeof next)
                return 0;
        } else {
            const char *slash = strrchr(cur, '/');
            size_t dlen = slash ? (size_t)(slash - cur) + 1 : 0;
            if (snprintf(next, sizeof next, "%.*s%s", (int)dlen, cur, link) >= (int)sizeof next)
                return 0;
        }
        memcpy(cur, next, sizeof cur);
    }
    const char *slash = strrchr(cur, '/');
    if (slash) {
        char dir[PATH_MAX], rdir[PATH_MAX];
        size_t dlen = (size_t)(slash - cur);
        if (dlen == 0)
            dlen = 1;
        if (dlen < sizeof dir) {
            memcpy(dir, cur, dlen);
            dir[dlen] = 0;
            if (realpath(dir, rdir) &&
                snprintf(out, outsz, "%s/%s", rdir, slash + 1) < (int)outsz)
                return 1;
        }
    }
    return snprintf(out, outsz, "%s", cur) < (int)outsz;
}

static pthread_mutex_t g_load_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static uint64_t cache_dlopen_hit(struct OcerzVM *vm, uint64_t cmh)
{
    if (g_dlerror_g)
        ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
    if (g_run_init_ready && g_run_vm && !vm->exited) {
        uint64_t istk = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
        uint64_t sp = istk ? istk + DYN_STACK_SIZE - 64 : 0;
        ocerz_dyldapi_objc_map_one(g_run_vm, cmh);

        if (sp && !vm->exited) {
            g_init_cur_gen++;
            int pf = g_init_force;
            g_init_force = 1;
            run_load_phase(g_run_vm, g_run_cache, cmh, sp, 0);
            g_init_force = pf;
        }
        if (sp && !g_foundation_inited && !init_is_done(cmh) && !vm->exited) {
            int prev = g_init_dlopen_restricted;
            g_init_dlopen_restricted = 1;
            init_closure(g_run_vm, g_run_cache, cmh, g_run_init_args, sp);
            g_init_dlopen_restricted = prev;
        }
    }
    return cmh;
}

static int try_soname_in_pathlist(const char *list, const char *name, char *out, size_t n)
{
    if (!list || !list[0])
        return 0;
    for (const char *p = list; *p;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 900) {
            char cand[1024];
            snprintf(cand, sizeof cand, "%.*s/%s", (int)len, p, name);
            if (access(cand, F_OK) == 0) {
                snprintf(out, n, "%s", cand);
                return 1;
            }
        }
        p += len;
        if (*p == ':')
            p++;
    }
    return 0;
}

static int resolve_bare_soname(const char *name, char *out, size_t n)
{
    if (!name || strchr(name, '/') || name[0] == '@')
        return 0;
    if (try_soname_in_pathlist(getenv("DYLD_LIBRARY_PATH"), name, out, n))
        return 1;
    const char *fb = getenv("DYLD_FALLBACK_LIBRARY_PATH");
    if (fb && fb[0])
        return try_soname_in_pathlist(fb, name, out, n);
    const char *home = getenv("HOME");
    char def[1024];
    if (home && home[0])
        snprintf(def, sizeof def, "%s/lib:/usr/local/lib:/usr/lib", home);
    else
        snprintf(def, sizeof def, "/usr/local/lib:/usr/lib");
    return try_soname_in_pathlist(def, name, out, n);
}

static uint64_t ocerz_dlopen_inner(struct OcerzVM *vm, const char *hostpath, int mode)
{
    if (!g_run_cache) {
        dlerror_set("dlopen: runtime loader not initialized", NULL);
        return 0;
    }
    if (!hostpath) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return ocerz_main_mh ? ocerz_main_mh : ocerz_arena_lo;
    }
    if (ocerz_main_mh && g_main_hostpath[0]) {
        char rp[PATH_MAX];
        if (strcmp(hostpath, g_main_hostpath) == 0 ||
            (realpath(hostpath, rp) && strcmp(rp, g_main_hostpath) == 0)) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return ocerz_main_mh;
        }
    }
    DynImage *already = dimg_find_by_path(hostpath);
    if (already) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return already->load_base;
    }
    uint64_t cmh = dep_find(g_run_cache, hostpath);
    if (cmh)
        return cache_dlopen_hit(vm, cmh);
    char canon[PATH_MAX];
    const char *loadpath = hostpath;
    if (ocerz_canon_dylib_path(hostpath, canon, sizeof canon) &&
        strcmp(canon, hostpath) != 0) {
        already = dimg_find_by_path(canon);
        if (already) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return already->load_base;
        }
        cmh = dep_find(g_run_cache, canon);
        if (cmh)
            return cache_dlopen_hit(vm, cmh);
        loadpath = canon;
    }

    char sopath[PATH_MAX];
    if (!strchr(loadpath, '/') && loadpath[0] != '@' && access(loadpath, F_OK) != 0 &&
        resolve_bare_soname(loadpath, sopath, sizeof sopath)) {
        already = dimg_find_by_path(sopath);
        if (already) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return already->load_base;
        }
        cmh = dep_find(g_run_cache, sopath);
        if (cmh)
            return cache_dlopen_hit(vm, cmh);
        loadpath = sopath;
    }
    if (mode & 0x10) {
        dlerror_set("dlopen(%s): not already loaded (RTLD_NOLOAD)", hostpath);
        return 0;
    }
    uint64_t fdev, fino;
    if (file_identity(loadpath, &fdev, &fino)) {
        uint64_t same = 0;
        if (ocerz_main_mh && g_main_ino == fino && g_main_dev == fdev)
            same = ocerz_main_mh;
        else if ((already = dimg_find_by_identity(fdev, fino)))
            same = already->load_base;
        if (same) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return same;
        }
    }
    int before = g_dimgs_n;
    DynImage *d = dlopen_load_image(g_run_cache, loadpath);
    if (ocerz_mode == OCERZ_MODE_NATIVE)
        native_tlv_register_loaded(ocerz_main_mh);
    if (!d)
        return 0;
    if (!g_run_init_ready && g_run_vm && !vm->exited &&
        g_dimgs_n > before) {
        for (int i = g_dimgs_n - 1; i >= before; i--) {
            if (getenv("OCERZ_NO_AGXMAP") &&
                strstr(g_dimgs[i].path,
                       "/System/Library/Extensions/AGXMetal"))
                continue;
            ocerz_dyldapi_objc_map_one(g_run_vm, g_dimgs[i].load_base);
            if (vm->exited)
                break;
        }
    }
    if (g_run_init_ready && g_run_vm && !vm->exited && g_dimgs_n > before) {
        uint64_t istk = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
        if (istk) {
            uint64_t itop = istk + DYN_STACK_SIZE - 64;
            for (int i = g_dimgs_n - 1; i >= before; i--) {
                ocerz_tlv_register_image(g_run_vm, g_run_cache, g_dimgs[i].load_base, itop);
                if (vm->exited)
                    break;
            }
            for (int i = g_dimgs_n - 1;
                 i >= before && !vm->exited; i--) {
                if (getenv("OCERZ_NO_AGXMAP") &&
                    strstr(g_dimgs[i].path,
                           "/System/Library/Extensions/AGXMetal"))
                    continue;
                ocerz_dyldapi_objc_map_one(g_run_vm,
                                           g_dimgs[i].load_base);
            }
            init_closure(g_run_vm, g_run_cache, d->load_base, g_run_init_args, itop);
        }
    }
    if (g_dlerror_g)
        ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
    {
        static char pig_key[256], pig_lib[1024];
        static int pig = -1, pig_done;
        if (pig < 0) {
            const char *e = getenv("OCERZ_DLOPEN_PIGGYBACK");
            pig = 0;
            if (e) {
                const char *c = strchr(e, ':');
                if (c && (size_t)(c - e) < sizeof pig_key && strlen(c + 1) < sizeof pig_lib) {
                    memcpy(pig_key, e, (size_t)(c - e)); pig_key[c - e] = '\0';
                    strcpy(pig_lib, c + 1);
                    pig = 1;
                }
            }
        }
        if (pig && !pig_done && loadpath && strstr(loadpath, pig_key)) {
            pig_done = 1;
            fprintf(stderr, "ocerz: PIGGYBACK[%d] after \"%s\": dlopen \"%s\"\n",
                    (int)getpid(), loadpath, pig_lib);
            uint64_t pb = ocerz_dlopen_inner(vm, pig_lib, 2);
            fprintf(stderr, "ocerz: PIGGYBACK[%d] -> %#llx\n", (int)getpid(), (unsigned long long)pb);
        }
    }
    return d->load_base;
}

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode)
{
    if (getenv("OCERZ_DLOPENLOG"))
        fprintf(stderr, "ocerz: DLOPEN \"%s\" mode=%#x\n", hostpath ? hostpath : "(null)", mode);
    pthread_mutex_lock(&g_load_lock);
    uint64_t r = ocerz_dlopen_inner(vm, hostpath, mode);
    pthread_mutex_unlock(&g_load_lock);
    if (getenv("OCERZ_DLOPENLOG"))
        fprintf(stderr, "ocerz: DLOPEN \"%s\" -> %#llx%s%s\n", hostpath ? hostpath : "(null)", (unsigned long long)r,
                (!r && g_dlerror_g) ? " err=" : "", (!r && g_dlerror_g) ? (const char *)ocerz_g2h(g_dlerror_g) : "");
    return r;
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

static uint64_t main_image_resolve(const char *sym)
{
    if (!g_main_dimg_valid)
        return 0;
    uint64_t value = ocerz_image_self_resolve(&g_main_dimg, sym);
    return value ? value : image_symtab_resolve(&g_main_dimg, sym);
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
        uint64_t v = main_image_resolve(buf);
        if (!v)
            v = disk_flat_resolve(buf);
        if (v)
            return v;
        if (g_run_cache)
            v = ocerz_cache_resolve(g_run_cache, buf);
        return v;
    }
    if (sh == -5) {
        return main_image_resolve(buf);
    }
    if (sh == -1) {
        uint64_t v = main_image_resolve(buf);
        if (!v)
            v = disk_flat_resolve(buf);
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
    if (g_run_cache && ocerz_cache_has_image(g_run_cache, handle))
        return ocerz_cache_resolve_from_image(g_run_cache, handle, buf, NULL);
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

static uint32_t native_image_minos(const uint8_t *mh)
{
    if (rd32(mh) != MH_MAGIC_64)
        return 0;
    uint32_t ncmds = rd32(mh + 16);
    uint32_t sizeofcmds = rd32(mh + 20);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint32_t walked = 0;
    for (uint32_t i = 0; i < ncmds && walked + 16 <= sizeofcmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t cmdsize = rd32(lc + 4);
        if (cmdsize < 8 || walked + cmdsize > sizeofcmds)
            break;
        if (cmd == LC_BUILD_VERSION && cmdsize >= 16 && rd32(lc + 8) == PLATFORM_MACOS)
            return rd32(lc + 12);
        if (cmd == LC_VERSION_MIN_MACOSX && cmdsize >= 12)
            return rd32(lc + 8);
        lc += cmdsize;
        walked += cmdsize;
    }
    return 0;
}

static void progvars_point_at(const DynFrame *fr, uint64_t argc_at, uint64_t argv_at,
                              uint64_t environ_at, uint64_t progname_at)
{
    const uint64_t at[4] = { argc_at, argv_at, environ_at, progname_at };
    for (int i = 0; i < 4; i++)
        if (at[i])
            ocerz_st(fr->progvars + 8 + 8 * (uint64_t)i, 8, at[i]);
}

static void native_exit_through_libsystem(const DynFrame *fr)
{
    DynImage *libsys = dimg_find_by_install_name("/usr/lib/libSystem.B.dylib");
    int found = 0;
    uint64_t exit_fn = libsys ? ocerz_image_self_resolve_ex(libsys, "_exit", &found) : 0;
    if (!found || !exit_fn)
        return;
    uint8_t code[] = {
        0x89, 0xc3,
        0x48, 0x83, 0xec, 0x08,
        0x89, 0xc7,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xd0,
        0x89, 0xdf,
        0xb8, 0x01, 0x00, 0x00, 0x02,
        0x0f, 0x05,
    };
    memcpy(code + 10, &exit_fn, sizeof exit_fn);
    memcpy(ocerz_g2h(fr->exit_stub), code, sizeof code);
}

int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    if (ocerz_mem_init_identity(DYN_ARENA_SIZE) != OCERZ_OK)
        return OCERZ_ENOMEM;

    static OcerzCache cache;
    if (ocerz_mode == OCERZ_MODE_CACHE) {
        if (ocerz_cache_map(&cache) != OCERZ_OK) {
            OCERZ_FATAL("cannot map shared cache for dynamic loading\n");
            return OCERZ_EIO;
        }
    } else {
        OCERZ_LOG("dynamic: native mode, shared cache not mapped\n");
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

    char abspath[PATH_MAX];
    if (realpath(path, abspath))
        path = abspath;

    DynImage img;
    memset(&img, 0, sizeof img);
    img.slice = slice;
    snprintf(img.path, sizeof img.path, "%s", path);
    snprintf(img.install_name, sizeof img.install_name, "%s", path);
    snprintf(g_main_hostpath, sizeof g_main_hostpath, "%s", path);
    file_identity(path, &g_main_dev, &g_main_ino);
    int r = map_segments(&img, 1);
    if (r != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", path);
        free(buf);
        return r;
    }
    if (img.main_entry == 0 && img.thread_entry == 0) {
        OCERZ_FATAL("%s has no LC_MAIN or LC_UNIXTHREAD entry\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        uint32_t minos = native_image_minos(slice);
        OCERZ_LOG("dynamic: %s declares macOS %u.%u.%u\n", path, minos >> 16, (minos >> 8) & 0xff,
                  minos & 0xff);
        ocerz_apidb_set_minos(minos);
        ocerz_bridge_set_process_args(argc, argv);
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
    protect_ro_segments(&img);

    if (ocerz_mode == OCERZ_MODE_NATIVE && g_native_miss_n > 0) {
        for (int i = 0; i < g_native_miss_n; i++)
            fprintf(stderr, "ocerz: native: no bridge for %s in %s\n",
                    g_native_miss[i].sym, g_native_miss[i].lib);
        if (g_native_miss_dropped)
            fprintf(stderr, "ocerz: native: %d more unresolved imports not listed\n",
                    g_native_miss_dropped);
        fprintf(stderr, "ocerz: native: %d unresolved imports, which no virtual library exports\n",
                g_native_miss_n + g_native_miss_dropped);
        free(buf);
        return 71;
    }
    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        native_tlv_register_loaded(img.load_base);
        ocerz_objcbridge_fix_selrefs((const uint8_t *)ocerz_g2h(img.load_base), img.slide);
        ocerz_objcbridge_define_image((const uint8_t *)ocerz_g2h(img.load_base), img.slide);
        if (dimg_find_by_install_name(OCERZ_OBJC_LIBOBJC))
            ocerz_objcbridge_install_uncaught();
    }

    DynFrame fr;
    memset(&fr, 0, sizeof fr);
    if (build_frame(path, argc, argv, envp, &fr) != OCERZ_OK) {
        OCERZ_FATAL("cannot build dynamic entry frame\n");
        free(buf);
        return OCERZ_ENOMEM;
    }
    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        native_exit_through_libsystem(&fr);
        ocerz_bridge_set_process_args((int)fr.argc, (char **)ocerz_g2h(fr.argv_arr));
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
    {
        uint64_t htid = 0;
        pthread_threadid_np(NULL, &htid);
        ocerz_st(gs - 8, 8, htid);
    }

    if (g_main_dimg_valid)
        free(g_main_dimg.owned_buf);
    g_main_dimg = img;
    g_main_dimg.owned_buf = buf;
    g_main_dimg_valid = 1;
    buf = NULL;
    free(buf);

    ocerz_vm_install_handlers(vm);
    ocerz_commpage_init();
    { extern void ocerz_peek_dump(const char *); ocerz_peek_dump("cache-mapped"); }
    if (ocerz_mode == OCERZ_MODE_CACHE && ocerz_dyldapi_setup(&cache) != OCERZ_OK)
        OCERZ_LOG("dynamic: dyld API shim not installed\n");

    uint64_t environ_addr = ocerz_cache_resolve(&cache, "_environ");
    if (environ_addr) {
        ocerz_st(environ_addr, 8, fr.envp_arr);
        OCERZ_LOG("dynamic: environ=%#llx set to %#llx\n",
                  (unsigned long long)environ_addr, (unsigned long long)fr.envp_arr);
    }

    uint64_t progname_addr = ocerz_cache_resolve(&cache, "___progname");
    if (!progname_addr)
        progname_addr = ocerz_cache_resolve(&cache, "__progname");
    if (progname_addr && fr.argv_arr) {
        uint64_t argv0 = ocerz_ld(fr.argv_arr, 8);
        uint64_t leaf = argv0, a = argv0;
        for (int i = 0; argv0 && i < 4096; i++, a++) {
            uint8_t c = (uint8_t)ocerz_ld(a, 1);
            if (c == 0) break;
            if (c == '/') leaf = a + 1;
        }
        if (argv0)
            ocerz_st(progname_addr, 8, leaf);
        OCERZ_LOG("dynamic: libdyld __progname=%#llx set to %#llx\n",
                  (unsigned long long)progname_addr, (unsigned long long)leaf);
    }

    if (ocerz_mode == OCERZ_MODE_CACHE) {
        uint64_t argc_addr = ocerz_cache_resolve(&cache, "_NXArgc");
        uint64_t argv_addr = ocerz_cache_resolve(&cache, "_NXArgv");
        if (argc_addr)
            ocerz_st(argc_addr, 4, fr.argc);
        if (argv_addr)
            ocerz_st(argv_addr, 8, fr.argv_arr);
        progvars_point_at(&fr, argc_addr, argv_addr, environ_addr, progname_addr);
    } else {
        progvars_point_at(&fr, ocerz_h2g(_NSGetArgc()), ocerz_h2g(_NSGetArgv()),
                          ocerz_h2g(_NSGetEnviron()), ocerz_h2g(_NSGetProgname()));
    }

    extern uint64_t g_main_path;
    if (fr.exec_path)
        g_main_path = fr.exec_path;
    else if (fr.argv_arr)
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
            { extern void ocerz_peek_dump(const char *); ocerz_peek_dump("post-libSystem"); }
            ran_init = 1;
        } else {
            OCERZ_LOG("dynamic: no libSystem initializer found\n");
        }

        if (ran_init) {
            g_libsys_mh = dep_find(&cache, "/usr/lib/libSystem.B.dylib");
            init_mark_done_closure(&cache, g_libsys_mh);
        }
        if (ran_init && (img.links_cf || closure_links_cf(&cache, img.load_base) ||
                         getenv("OCERZ_INITPHASE")) && !getenv("OCERZ_NOINITPHASE")) {
            uint64_t libsys = g_libsys_mh;
            compute_eager_set(&cache, img.load_base);
            OCERZ_LOG("dynamic: eager init set = %d images (of closure)\n", g_eager_n);
            ocerz_tlv_register_closure(vm, &cache, img.load_base, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            g_init_cur_gen++;
            run_load_phase(vm, &cache, libsys, fr.stack_top, 0);
            if (vm->exited)
                return vm->exit_code;
            g_init_cur_gen++;
            OCERZ_LOG("dynamic: running dependency-ordered initializer phase\n");
            run_init_phase(vm, &cache, img.load_base, ia, fr.stack_top, libsys);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: initializer phase complete\n");
            { extern void ocerz_peek_dump(const char *); ocerz_peek_dump("post-init-phase"); }
        } else if (ran_init && !getenv("OCERZ_NOINITPHASE")) {
            static uint64_t order[INIT_CLOSURE_CAP];
            int n = 0;
            init_collect(&cache, img.load_base, order, &n, INIT_CLOSURE_CAP);
            for (int i = 0; i < n; i++) {
                int idx = init_mark(order[i]);
                if (idx >= 0)
                    g_init_being[idx] = 0;
            }
            for (int i = 0; i < n && !vm->exited; i++)
                ocerz_tlv_register_image(vm, &cache, order[i], fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            g_init_cur_gen++;
            OCERZ_LOG("dynamic: running dependency-ordered initializers for %d images\n", n);
            run_init_phase(vm, &cache, img.load_base, ia, fr.stack_top, g_libsys_mh);
            if (vm->exited)
                return vm->exit_code;
        }
        if (ran_init)
            g_run_init_ready = 1;
    }

    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        ocerz_objcbridge_run_loads(vm, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
        uint64_t nia[5] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars };
        for (int i = g_dimgs_n - 1; i >= 0 && !vm->exited; i--)
            run_image_inits(vm, g_dimgs[i].load_base, nia, fr.stack_top);
        if (!vm->exited)
            run_image_inits(vm, img.load_base, nia, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
    }

    OCERZ_LOG("dynamic: load_base=%#llx slide=%#llx main=%#llx\n",
              (unsigned long long)img.load_base, (unsigned long long)img.slide,
              (unsigned long long)img.main_entry);

    if (getenv("OCERZ_ZONEPROBE")) {
        uint64_t g_malloc_zones = 0x7ff8436b4758ULL;
        uint64_t g_malloc_num_zones = 0x7ff8436b49f0ULL;
        uint64_t g_default_zone = 0x7ff84388c178ULL;
        uint64_t g_initial_nano = 0x7ff8436b47a0ULL;
        uint64_t g_initial_scalable = 0x7ff8436b47a8ULL;
        uint64_t nz = ocerz_ld(g_malloc_num_zones, 4);
        uint64_t mzp = ocerz_ld(g_malloc_zones, 8);
        uint64_t dz = ocerz_ld(g_default_zone, 8);
        uint64_t z0 = mzp ? ocerz_ld(mzp, 8) : 0;
        fprintf(stderr, "ZONEPROBE num_zones=%llu malloc_zones=%#llx zones[0]=%#llx default_zone=%#llx initial_nano=%#llx initial_scalable=%#llx\n",
                (unsigned long long)nz, (unsigned long long)mzp, (unsigned long long)z0,
                (unsigned long long)dz, (unsigned long long)g_initial_nano, (unsigned long long)g_initial_scalable);

        for (uint64_t i = 0; i < nz && i < 6; i++) {
            uint64_t z = ocerz_ld(mzp + i * 8, 8);
            uint64_t zmalloc = z ? ocerz_ld(z + 0x18, 8) : 0;
            uint64_t znameptr = z ? ocerz_ld(z + 0x20, 8) : 0;
            char nm[64] = {0};
            if (znameptr) for (int k = 0; k < 63; k++) { uint64_t c = ocerz_ld(znameptr + k, 1); if (!c) break; nm[k] = (char)c; }
            fprintf(stderr, "ZONEPROBE  zones[%llu]=%#llx malloc=%#llx name@+0x20=%#llx name=\"%s\"\n",
                    (unsigned long long)i, (unsigned long long)z, (unsigned long long)zmalloc,
                    (unsigned long long)znameptr, nm);
        }

        uint64_t cfz = 0x7ff840095a00ULL;
        uint64_t cmpg = 0x7ff8436c2b20ULL;
        fprintf(stderr, "ZONEPROBE CFzone@%#llx: isa/[0]=%#llx [+0x18 malloc]=%#llx [+0x68 ver]=%#llx [+0xd0]=%#llx [+0xb0]=%#llx\n",
                (unsigned long long)cfz, (unsigned long long)ocerz_ld(cfz, 8),
                (unsigned long long)ocerz_ld(cfz + 0x18, 8), (unsigned long long)ocerz_ld(cfz + 0x68, 8),
                (unsigned long long)ocerz_ld(cfz + 0xd0, 8), (unsigned long long)ocerz_ld(cfz + 0xb0, 8));
        fprintf(stderr, "ZONEPROBE cmpglobal@%#llx=%#llx  (CFzone[0]==cmpglobal? %d)\n",
                (unsigned long long)cmpg, (unsigned long long)ocerz_ld(cmpg, 8),
                ocerz_ld(cfz, 8) == ocerz_ld(cmpg, 8));

        uint64_t cfa = 0x7ff8400964a8ULL;
        fprintf(stderr, "ZONEPROBE kCFAllocatorSystemDefault@%#llx -> %#llx\n",
                (unsigned long long)cfa, (unsigned long long)ocerz_ld(cfa, 8));
    }

    if (!img.main_entry) {
        uint64_t vec_end = fr.apple_arr;
        while (ocerz_ld(vec_end, 8) != 0)
            vec_end += 8;
        vec_end += 8;
        uint64_t vec_len = vec_end - fr.argv_arr;
        uint64_t sp = (fr.stack_top - vec_len - 8) & ~0xfull;
        ocerz_st(sp, 8, fr.argc);
        memcpy(ocerz_g2h(sp + 8), ocerz_g2h(fr.argv_arr), (size_t)vec_len);
        vm->cpu.gpr[OCERZ_RSP] = sp;
        vm->cpu.rip = img.thread_entry;
        return ocerz_vm_run(vm);
    }

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
