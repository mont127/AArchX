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
 *
 * A dependency is looked for in the shared cache both before and after its
 * symlinks are followed.  Only the first lookup used to happen, so a path that
 * named the cache under a different spelling was followed to a target that is
 * not on disk and the load failed as a missing file.  Eleven of the twelve
 * dylibs in /usr/lib/swift are symlinks into frameworks whose binaries exist
 * only in the cache, which is how Tailscale lost Network.framework through
 * Sparkle, naming a path the cache was holding all along.
 * An upward link is how a library declares the back edge of a dependency cycle,
 * and it is an ordering edge for nothing: the library that declares it may be
 * initialized first.  So it is not followed on the way DOWN.  Following it
 * there made CoreFoundation's upward link to CoreServicesInternal a real edge,
 * which put QuickLookThumbnailing, SiriTTS and CoreML inside libc++abi's
 * subtree, and libc++abi then sat unfinished on the recursion stack while
 * MLAssetIO's initializer called operator new into a libc++ that had not been
 * initialized yet.
 *
 * It is followed afterwards instead, once the image that declares it has been
 * initialized, because the target still has to be initialized at some point -
 * dyld loads and initializes an upward dependency like any other, it only
 * declines to order it first.  Leaving it out entirely is what broke sw_vers:
 * CoreFoundation links Foundation upward, so Foundation's code was reachable,
 * the whole cache being mapped, and its classes were registered, but its
 * initializer never ran.  NSString therefore stayed an abstract class cluster,
 * and the first +[NSString stringWithFormat:] fell into
 * _NSRequestConcreteObject, whose complaint is itself built with
 * +[NSString stringWithFormat:].  That recursion ran the 8 MB guest stack out.
 * OCERZ_NO_UPWARD_INIT=1 restores the old behaviour.
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
 * A cache image reached by dlopen has to join that same list, and this was the
 * one kind of image that never did.  dlopen of a disk dylib registered; dlopen
 * of a cache image returned the mach header and appended nothing, so the
 * library was loaded, its symbols resolved and dladdr knew where they lived,
 * while every walk of the image list said it was absent.  libobjc keys its
 * per-image queries off that list, and Metal loads its GPU driver through
 * them, so MTLCreateSystemDefaultDevice returned nil, MTLCopyAllDevices found
 * no device, OpenGL had no accelerated renderer left to offer and
 * CGLChoosePixelFormat failed for every accelerated attribute set.  Brawlhalla
 * put a window on screen and never drew into it.
 *
 * The registration happens at the end of cache_dlopen_hit, after the objc
 * mapping and the initializer phase, and the order is the whole of it.  libobjc
 * calls back into the dyld APIs while it maps an image, and an image already
 * standing in the list when that callback arrives is one it takes as handled:
 * registering first cost the newly loaded image its categories, which is
 * exactly what a late-loaded framework is usually dlopened for.
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
 * can run at all.  A dlopen defines its images the same way, but only once the
 * whole closure has bound, and runs each image's own +load methods just before
 * that image's initializers.
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
 * dlopen has bound, before anything in the new images runs.  The dlopen pass
 * walks every image the loader holds rather than only the new ones, which costs
 * a scan per image and spares it knowing which those are.  An image already
 * registered is skipped, and that is not a formality - once a descriptor for a
 * variable at offset 0 has been packed it no longer reads as packed, so packing
 * it again would store the template delta as its offset.  A dlopen that fails
 * registers nothing, because it unmaps everything it mapped before this pass.
 *
 * ---- loading code at run time in native mode ----
 * Native mode's dlopen family is answered here, reached from special exports in
 * src/bridge.c, and it is not cache mode's ocerz_dlopen: that one resolves
 * against the x86 shared cache and leaves Objective-C, thread-local variables
 * and the image list to the translated libobjc and libdyld, which native mode
 * does not have.
 *
 * A handle is the mach header of the image, which is also what dladdr and the
 * image list answer with, so a header obtained either way can be handed to
 * dlsym.  Its low bit, never set in a page-aligned header, marks a handle that
 * RTLD_FIRST asked for.  dlopen(NULL) answers RTLD_DEFAULT, or RTLD_MAIN_ONLY
 * under RTLD_FIRST, because that is what dyld answers: an arm64 program on the
 * host printed 0xfffffffffffffffe and 0xfffffffffffffffb for them.
 *
 * A path is resolved the way dyld resolves one: @executable_path against the
 * main executable, @loader_path against the image holding the caller's return
 * address, @rpath through that image's LC_RPATHs and then the main
 * executable's, and a bare name through DYLD_LIBRARY_PATH, the working
 * directory and the fallback path.  Each candidate is asked, in order, whether
 * it is the main executable, an image already loaded by path, install name or
 * file identity, an install name a database describes, the same once its
 * symlinks are resolved, a file on disk, or a library the host's shared cache
 * holds, whose real path the host's _dyld_shared_cache_real_path answers
 * without loading anything.  That last is how /usr/lib/libc.dylib, libz.dylib or
 * a framework's top-level symlink turn into the install name a database is
 * filed under.  A file with no x86_64 slice and a host library no database
 * describes are both refused as a native library without an API database, since
 * native mode runs x86 code only and reaches native code only through one.
 *
 * A load binds its whole closure before anything in it runs.  The loader's
 * usual path defines an image's Objective-C as soon as that image's fixups are
 * bound, which is harmless at startup, where a failed bind ends the process; a
 * dlopen that fails must leave nothing behind, and a class handed to the native
 * runtime cannot be taken back, so while a dlopen loads, those passes wait.
 * Misses are counted against the collected-import table and a non-weak
 * dependency that did not load is recorded, and either fails the dlopen with
 * dyld's wording, Symbol not found or Library not loaded, naming the image that
 * referenced it.  Every image the attempt mapped is then unmapped and dropped
 * and the table put back as it was, so a second attempt fails the same way.
 * Only a closure that bound completely is published into the image list - one
 * release store of the count, which readers load with acquire, so no reader
 * sees an image half built and none takes a lock - and then, in dyld's order,
 * its thread-local descriptors are registered, every add-image callback is
 * called for each new image, every new image's selectors are rewritten and its
 * classes, categories and protocols defined, dependencies first by the order
 * their loads completed, and then image by image, again dependencies first,
 * that image's +load methods and its initializers run on the calling thread
 * below the caller's stack pointer.  RTLD_LOCAL marks the image it loaded, not
 * its dependencies, which hides it from RTLD_DEFAULT, RTLD_NEXT and flat lookups
 * until a dlopen without it; RTLD_NOLOAD answers only what is already loaded.
 * Nothing is ever unloaded, so dlclose checks its handle and answers 0, and a
 * mach header never comes to name a second image.
 *
 * dlsym takes the C name and searches for it with a leading underscore, as
 * dyld does.  RTLD_DEFAULT searches the main executable and then every global
 * image in load order, RTLD_MAIN_ONLY the main executable alone, RTLD_NEXT the
 * images after the one holding the caller's return address and RTLD_SELF that
 * image first; a handle searches its image and then its dependencies breadth
 * first, or its image alone under RTLD_FIRST.  A plug-in bundle's imports from
 * the executable that loaded it carry the main-executable ordinal, and the
 * resolver answers those from the main image once every other place has
 * missed, as it does for a flat or weak lookup nothing else answered; before
 * that, no other image's import could bind to the main image at all.
 *
 * dladdr names a guest image's symbol from its symbol table, read out of the
 * mapped __LINKEDIT so that the name is guest memory, and an image with none -
 * every synthesized one - from its export trie, walked once into a sorted
 * table.  An address in no guest image is native memory, a data export's target
 * or a host framework's code, and the host's own dladdr answers it, with
 * strings and a base that are host addresses the identity map makes guest ones.
 *
 * dlerror is per thread, as POSIX and dyld make it.  Each thread keeps a buffer
 * in guest memory behind a pthread key whose destructor unmaps it; a message
 * stays readable until that thread's next failure, and dlerror answers it once.
 *
 * The image list is the main executable and then every image the loader holds,
 * synthesized libraries included, in load order.  x86 code walks it expecting
 * the libraries it links to be in it - a crash reporter naming libSystem, a
 * framework looking for its own header by name - and those libraries are the
 * synthesized images, so a list of guest dylibs alone would hide what the guest
 * believes it has linked, while listing the host's images would hand x86 code
 * arm64 headers.  An add-image callback registered late is called at once for
 * every image already listed, and then for each image a dlopen adds.
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
#include "ocerz/abi.h"
#include "ocerz/syscall.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <crt_externs.h>

#define DYN_ARENA_SIZE (256ull << 30)
#define DYN_STACK_SIZE (8ull << 20)
#define MISS_LIST_MAX 24
#define BIND_ORDINAL_FLAT_LOOKUP (-2)

static unsigned long g_dynlookup_miss;

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
        return -2;
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
    int is_virtual;
    int local;
    uint32_t seq;
    uint64_t map_base;
    uint64_t map_size;
    uint64_t file_dev;
    uint64_t file_ino;
    struct SymIndex *symidx;
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
    uint64_t cbase = 0;
    const char *cname = ocerz_cache_name_for_addr(addr, &cbase);
    if (cname && (!best || cbase > best->load_base)) {
        if (base_out) *base_out = cbase;
        return cname;
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
        img->map_base = region;
        img->map_size = vmhi - vmlo;
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

/*
 * Exports of an image that has no export trie.
 *
 * A dylib built by an old enough toolchain, or linked without one, carries its
 * exports only in the classic symbol table that LC_SYMTAB points at, and says
 * nothing in LC_DYLD_INFO or LC_DYLD_EXPORTS_TRIE.  Resolving through the trie
 * alone therefore finds nothing in it at all, and every import naming it goes
 * unresolved - twenty-one of the sixty dylibs Photoshop ships are of that kind,
 * which is how its OpenCV could not find a single Intel IPP entry point.
 *
 * The table is read once per image and turned into an open-addressed index of
 * the external symbols it defines, which is what an export trie holds.
 * LC_DYSYMTAB names the run of them, so the scan touches those and not the
 * image's local symbols.  Only an image with no trie is indexed: where there is
 * one it is the whole truth about what the image exports, and a miss in it is
 * a real miss rather than a reason to go looking somewhere slower.
 */
typedef struct SymIndex {
    uint32_t cap;
    uint32_t n;
    const char *strtab;
    struct { uint32_t stroff; uint64_t value; } *ent;
} SymIndex;

static uint32_t symidx_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static SymIndex *symidx_build(const uint8_t *slice, uint64_t text_vmaddr)
{
    uint32_t ncmds = rd32(slice + 16);
    const uint8_t *lc = slice + sizeof(struct mach_header_64);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint32_t iextdef = 0, nextdef = 0;
    int have_dysym = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SYMTAB) {
            symoff = rd32(lc + 8); nsyms = rd32(lc + 12);
            stroff = rd32(lc + 16); strsize = rd32(lc + 20);
        } else if (cmd == LC_DYSYMTAB) {
            iextdef = rd32(lc + 8 + 3 * 4); nextdef = rd32(lc + 8 + 4 * 4);
            have_dysym = 1;
        }
        lc += rd32(lc + 4);
    }
    if (!symoff || !nsyms || !stroff || !strsize)
        return NULL;
    uint32_t first = 0, count = nsyms;
    if (have_dysym && nextdef && iextdef + nextdef <= nsyms) {
        first = iextdef;
        count = nextdef;
    }
    SymIndex *ix = (SymIndex *)calloc(1, sizeof *ix);
    if (!ix)
        return NULL;
    uint32_t cap = 64;
    while (cap < count * 2u)
        cap <<= 1;
    ix->ent = calloc(cap, sizeof *ix->ent);
    if (!ix->ent) {
        free(ix);
        return NULL;
    }
    ix->cap = cap;
    ix->strtab = (const char *)(slice + stroff);
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *nl = slice + symoff + (size_t)(first + k) * 16;
        uint32_t strx = rd32(nl);
        uint8_t type = nl[4];
        if (strx == 0 || strx >= strsize)
            continue;
        if (!(type & N_EXT) || (type & N_TYPE) != N_SECT)
            continue;
        uint64_t value = rd64(nl + 8);
        if (!value)
            continue;
        const char *name = ix->strtab + strx;
        uint32_t h = symidx_hash(name) & (cap - 1);
        while (ix->ent[h].stroff)
            h = (h + 1) & (cap - 1);
        ix->ent[h].stroff = strx;
        ix->ent[h].value = value - text_vmaddr;
        ix->n++;
    }
    return ix;
}

static uint64_t symtab_export_resolve(DynImage *img, const char *sym, int *found)
{
    if (!img->symidx) {
        uint32_t tsize = 0;
        if (image_export_trie(img->slice, &tsize) && tsize)
            return 0;
        uint64_t text = img->seg_count > 0 ? img->seg_vmaddr[0] : 0;
        img->symidx = symidx_build(img->slice, text);
        if (!img->symidx)
            return 0;
        OCERZ_LOG("dynamic: %s has no export trie; indexed %u exports from its symbol table\n",
                  img->install_name[0] ? img->install_name : img->path, img->symidx->n);
    }
    SymIndex *ix = img->symidx;
    if (!ix->cap)
        return 0;
    uint32_t h = symidx_hash(sym) & (ix->cap - 1);
    while (ix->ent[h].stroff) {
        if (strcmp(ix->strtab + ix->ent[h].stroff, sym) == 0) {
            if (found)
                *found = 1;
            return img->load_base + ix->ent[h].value;
        }
        h = (h + 1) & (ix->cap - 1);
    }
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

typedef struct TrieWalk {
    const uint8_t *start;
    const uint8_t *end;
    uint64_t load_base;
    OcerzTrieVisit visit;
    void *ctx;
    int count;
    int stopped;
    char name[4096];
} TrieWalk;

static int trie_walk(TrieWalk *w, uint64_t off, size_t len, int depth)
{
    if (depth > 512 || off >= (uint64_t)(w->end - w->start))
        return -1;
    const uint8_t *p = w->start + off;
    uint64_t term = self_uleb(&p, w->end);
    if (p >= w->end || term >= (uint64_t)(w->end - p))
        return -1;
    const uint8_t *after = p + term;
    if (term) {
        const uint8_t *tp = p;
        uint64_t flags = self_uleb(&tp, after);
        uint64_t value = 0;
        if (!(flags & 0x08)) {
            uint64_t raw = self_uleb(&tp, after);
            value = (flags & 0x03) == 0x02 ? raw : w->load_base + raw;
        }
        w->name[len] = '\0';
        w->count++;
        if (w->visit && w->visit(w->ctx, w->name, value, flags)) {
            w->stopped = 1;
            return 0;
        }
    }
    p = after;
    uint8_t children = *p++;
    for (uint8_t i = 0; i < children; i++) {
        if (p >= w->end)
            return -1;
        size_t elen = strnlen((const char *)p, (size_t)(w->end - p));
        if (elen == (size_t)(w->end - p) || len + elen >= sizeof w->name)
            return -1;
        memcpy(w->name + len, p, elen);
        p += elen + 1;
        uint64_t child = self_uleb(&p, w->end);
        if (trie_walk(w, child, len + elen, depth + 1) < 0)
            return -1;
        if (w->stopped)
            return 0;
    }
    return 0;
}

int ocerz_dyld_trie_each(const uint8_t *slice, uint64_t load_base, OcerzTrieVisit visit, void *ctx)
{
    uint32_t tsize = 0;
    uint64_t toff = slice ? image_export_trie(slice, &tsize) : 0;
    if (!toff || !tsize)
        return 0;
    TrieWalk *w = calloc(1, sizeof *w);
    if (!w)
        return -1;
    w->start = slice + toff;
    w->end = w->start + tsize;
    w->load_base = load_base;
    w->visit = visit;
    w->ctx = ctx;
    int rc = trie_walk(w, 0, 0, 0);
    int n = w->count;
    free(w);
    return rc < 0 ? -1 : n;
}

static uint64_t ocerz_image_self_resolve_ex(DynImage *img, const char *sym, int *found)
{
    if (ocerz_mode == OCERZ_MODE_NATIVE && img->is_virtual &&
        strcmp(img->install_name, "/usr/lib/libSystem.B.dylib") == 0 &&
        (strncmp(sym, "__Unwind_", 9) == 0 || strncmp(sym, "_unw_", 5) == 0 ||
         strcmp(sym, "___register_frame") == 0 || strcmp(sym, "___deregister_frame") == 0)) {
        DynImage *unwind = dimg_find_by_install_name("/usr/lib/libunwind.1.dylib");
        if (unwind && !unwind->is_virtual) {
            int present = 0;
            uint64_t addr = ocerz_dyld_trie_resolve(unwind->slice, unwind->load_base, sym, &present);
            if (present) {
                if (found)
                    *found = 1;
                return addr;
            }
        }
    }
    int f = 0;
    uint64_t v = ocerz_dyld_trie_resolve(img->slice, img->load_base, sym, &f);
    if (f) {
        if (found)
            *found = 1;
        return v;
    }
    return symtab_export_resolve(img, sym, found);
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
        if (g_dimgs[i].local)
            continue;
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

typedef struct RpathList RpathList;
static DynImage *load_disk_dylib(OcerzCache *cache, const char *install_name, DynImage *loader,
                                 const RpathList *rpaths);

static uint64_t virt_flat_resolve_ex(const char *name, const char *want, int *found, const char **hit)
{
    for (int i = 0; i < g_dimgs_n; i++) {
        if (!g_dimgs[i].is_virtual)
            continue;
        if (want && strcmp(g_dimgs[i].install_name, want) == 0)
            continue;
        int f = 0;
        uint64_t v = ocerz_image_self_resolve_ex(&g_dimgs[i], name, &f);
        if (f) {
            *found = 1;
            if (hit)
                *hit = g_dimgs[i].install_name;
            return v;
        }
    }
    return 0;
}

static uint64_t virt_ondemand_resolve_ex(OcerzCache *cache, DynImage *img, const char *name, int *found,
                                         const char **hit)
{
    int n = 0;
    const char **names = ocerz_apidb_install_names(&n);
    for (int i = 0; i < n; i++) {
        if (dimg_find_by_install_name(names[i]))
            continue;
        const OcerzApiLibrary *lib = ocerz_apidb_library(names[i]);
        if (!lib || !ocerz_apidb_find(lib, name))
            continue;
        DynImage *dep = load_disk_dylib(cache, names[i], img, NULL);
        if (!dep)
            continue;
        int f = 0;
        uint64_t v = ocerz_image_self_resolve_ex(dep, name, &f);
        if (f) {
            *found = 1;
            if (hit)
                *hit = dep->install_name;
            return v;
        }
    }
    return 0;
}

static int expand_at_prefix(DynImage *loader, const char *name, char *out, size_t n);

#define NATIVE_MISS_MAX 256
struct native_miss { char lib[256]; char sym[256]; char from[256]; };
static struct native_miss g_native_miss[NATIVE_MISS_MAX];
static int g_native_miss_n;
static int g_native_miss_dropped;

static void native_miss_add(const char *lib, const char *sym, const char *from)
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
    snprintf(g_native_miss[g_native_miss_n].from, sizeof g_native_miss[0].from, "%s", from ? from : "");
    g_native_miss_n++;
}

static uint64_t main_image_resolve_ex(const char *sym, int *found);

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
    if (!found && virtual_dep) {
        const char *hit = NULL;
        value = virt_flat_resolve_ex(name, tgt, &found, &hit);
        if (found)
            OCERZ_LOG("dynamic: %s in %s bound in %s instead\n", name, tgt ? tgt : "(flat)",
                      hit ? hit : "?");
    }
    if (!found && !virtual_dep)
        value = disk_flat_resolve_ex(name, &found);
    if (!found && (libord == -1 || libord == -2 || libord == -3))
        value = main_image_resolve_ex(name, &found);
    if (!found && ocerz_mode == OCERZ_MODE_NATIVE) {
        const char *hit = NULL;
        value = virt_ondemand_resolve_ex(cache, img, name, &found, &hit);
        if (found)
            OCERZ_LOG("dynamic: %s in %s bound in %s instead\n", name, tgt ? tgt : "(flat)",
                      hit ? hit : "?");
    }
    if (!found && !weak) {
        if (ocerz_mode == OCERZ_MODE_NATIVE) {
            native_miss_add(libord == -1 ? "(main executable)" : tgt, name, img->path);
        } else if (libord == BIND_ORDINAL_FLAT_LOOKUP) {
            g_dynlookup_miss++;
            if (getenv("OCERZ_DYNLOOKUPLOG"))
                fprintf(stderr, "ocerz: dynamic-lookup miss: %s, wanted by %s\n", name,
                        img->install_name[0] ? img->install_name : img->path);
        } else {
            static int shown, all = -1;
            if (all < 0)
                all = getenv("OCERZ_ALLMISS") ? 1 : 0;
            if (all || shown < MISS_LIST_MAX)
                OCERZ_FATAL("unresolved import: %s, wanted by %s from %s\n", name,
                            img->install_name[0] ? img->install_name : img->path,
                            tgt && tgt[0] ? tgt : "the flat namespace");
            else if (shown == MISS_LIST_MAX)
                OCERZ_FATAL("unresolved import: further ones are not listed;"
                            " OCERZ_ALLMISS=1 lists them all\n");
            shown++;
        }
    }
    return value;
}

/*
 * The protections an image's segments are meant to have, applied once its
 * fixups and ocerz's own writes into it are done.
 *
 * A segment is mapped writable while it is being bound, and dyld then puts each
 * one at its initprot; a segment carrying SG_READ_ONLY, which is what
 * __DATA_CONST is, ends up read-only rather than at the read-write initprot it
 * declares.  ocerz used to do this for __TEXT alone, so everything else stayed
 * writable and said so: mach_vm_region reported read-write for __DATA_CONST and
 * __LINKEDIT where the same query on a real system reports read-only.  That is
 * not a cosmetic difference.  Electron asks mach_vm_region about a region it
 * expects to be read-only and executes an int3 when the answer is anything
 * else, which is why Discord's renderers died on startup.
 *
 * The word read here is initprot at offset 60, not maxprot at 56.  The two
 * agree in everything a modern linker emits, so the distinction only shows up
 * in an older image whose __TEXT is writable in maxprot and not in initprot -
 * which would previously have been left unprotected.
 *
 * This runs after canonicalize_objc_selrefs, because the selector references it
 * rewrites live in __DATA_CONST, and after the fixups for the same reason.
 * Native mode has a third writer to wait for: there the objc bridge builds the
 * image's classes itself, and ocerz_objcbridge_define_image writes them into
 * the same read-only segment long after the image is loaded.  So native mode
 * queues each image and protects the batch once the bridge has defined it -
 * protecting at load time bus-errored ocerz itself, inside its own loader,
 * before the guest ran a single instruction.
 * OCERZ_NO_TEXT_RO=1 leaves every segment as it was mapped.
 */
#define SEG_FLAG_READ_ONLY 0x10u

static DynImage *g_ro_pending[DYN_DIMG_MAX];
static int g_ro_pending_n;

static void apply_seg_prots(DynImage *img)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 60);
            uint32_t flags = rd32(lc + 68);
            if (!vmsize || (vmaddr == 0 && initprot == 0)) {
                lc += rd32(lc + 4);
                continue;
            }
            int want;
            if (flags & SEG_FLAG_READ_ONLY)
                want = PROT_READ;
            else if (initprot & VM_PROT_WRITE)
                want = -1;
            else
                want = ((initprot & VM_PROT_READ) ? PROT_READ : 0) |
                       ((initprot & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
            if (want > 0)
                ocerz_protect(vmaddr + img->slide, vmsize, want);
        }
        lc += rd32(lc + 4);
    }
}

static void protect_ro_segments(DynImage *img)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_TEXT_RO") ? 1 : 0;
    if (dis || !img->slice)
        return;
    for (int i = 0; i < g_ro_pending_n; i++)
        if (g_ro_pending[i] == img)
            return;
    if (g_ro_pending_n < DYN_DIMG_MAX) {
        g_ro_pending[g_ro_pending_n++] = img;
        return;
    }
    apply_seg_prots(img);
}

static void protect_ro_flush(void)
{
    int n = g_ro_pending_n;
    g_ro_pending_n = 0;
    for (int i = 0; i < n; i++)
        apply_seg_prots(g_ro_pending[i]);
}

static void protect_ro_drop(const DynImage *img)
{
    int n = 0;
    for (int i = 0; i < g_ro_pending_n; i++)
        if (g_ro_pending[i] != img)
            g_ro_pending[n++] = g_ro_pending[i];
    g_ro_pending_n = n;
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
    ocerz_vm_set_main_stack(stack, stack + DYN_STACK_SIZE);
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
static int seg_index(uint64_t addr)
{
    int lo = 0, hi = g_segs_n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_segs[mid].lo <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best >= 0 && addr < g_segs[best].hi ? best : -1;
}


#define EAGER_MAX 4096
static uint64_t g_eager[EAGER_MAX];
static int g_eager_n;
#define EAGER_SET (EAGER_MAX * 2)
static uint64_t g_eager_set[EAGER_SET];
static int g_eager_set_n;

static unsigned eager_slot(uint64_t mh)
{
    unsigned at = (unsigned)((mh * 0x9e3779b97f4a7c15ull) >> 40) & (EAGER_SET - 1);
    while (g_eager_set[at] && g_eager_set[at] != mh)
        at = (at + 1) & (EAGER_SET - 1);
    return at;
}

static int eager_has(uint64_t mh)
{
    if (g_eager_set_n != g_eager_n) {
        memset(g_eager_set, 0, sizeof g_eager_set);
        for (int i = 0; i < g_eager_n; i++)
            g_eager_set[eager_slot(g_eager[i])] = g_eager[i];
        g_eager_set_n = g_eager_n;
    }
    return g_eager_set[eager_slot(mh)] == mh;
}
static void eager_add(uint64_t mh)
{
    if (mh && !eager_has(mh) && g_eager_n < EAGER_MAX) {
        g_eager[g_eager_n++] = mh;
        g_eager_set[eager_slot(mh)] = mh;
        g_eager_set_n = g_eager_n;
    }
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
                    uint64_t last_lo = 1, last_hi = 0;
                    for (uint64_t pp = a; pp + 8 <= e; pp += 8) {
                        uint64_t v = rd64((const uint8_t *)ocerz_g2h(pp));
                        if (v >= last_lo && v < last_hi)
                            continue;
                        int at = seg_index(v);
                        if (at < 0)
                            continue;
                        last_lo = g_segs[at].lo;
                        last_hi = g_segs[at].hi;
                        if (g_segs[at].mh != mh) eager_add(g_segs[at].mh);
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

static int upward_init_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_UPWARD_INIT") ? 0 : 1;
    return on;
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

static int dylib_lc_is_upward_dep(const uint8_t *lc)
{
    uint32_t cmd = rd32(lc);
    if (cmd == LC_LOAD_UPWARD_DYLIB)
        return 1;
    if ((cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB) &&
        rd32(lc + 4) >= sizeof(struct dylib_use_command) &&
        rd32(lc + 8) == sizeof(struct dylib_use_command) && rd32(lc + 12) == DYLIB_USE_MARKER)
        return (rd32(lc + 24) & DYLIB_USE_UPWARD) != 0;
    return 0;
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
    if (!upward_init_enabled() || vm->exited)
        return;
    lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        if (dylib_lc_is_upward_dep(lc)) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4)) {
                uint64_t umh = dep_mh(cache, (const char *)(lc + noff));
                if (getenv("OCERZ_INITEDGE"))
                    fprintf(stderr, "INITEDGE-UPWARD %#llx -> %#llx \"%s\"\n",
                            (unsigned long long)mh, (unsigned long long)umh,
                            (const char *)(lc + noff));
                run_init_phase(vm, cache, umh, ia, stack_top, skip_mh);
            }
        }
        lc += rd32(lc + 4);
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

typedef struct NativeDlLoad {
    int active;
    char missing[1024];
    char missing_from[1024];
    char reason[256];
} NativeDlLoad;

static NativeDlLoad g_ndl;
static uint32_t g_dimg_seq;

static const char *(*g_host_cache_real_path)(const char *);
static pthread_once_t g_host_cache_real_path_once = PTHREAD_ONCE_INIT;

static void host_cache_real_path_init(void)
{
    g_host_cache_real_path =
        (const char *(*)(const char *))dlsym(RTLD_DEFAULT, "_dyld_shared_cache_real_path");
}

static const char *host_cache_real_path(const char *path)
{
    pthread_once(&g_host_cache_real_path_once, host_cache_real_path_init);
    return g_host_cache_real_path && path ? g_host_cache_real_path(path) : NULL;
}

static void native_dl_reason(const char *what, const char *path)
{
    if (!g_ndl.active || g_ndl.reason[0])
        return;
    snprintf(g_ndl.reason, sizeof g_ndl.reason, what, path ? path : "");
}

static int native_guest_path(const char *name, char *out, size_t n)
{
    const char *root = getenv("OCERZ_GUEST_ROOT");
    if (ocerz_mode != OCERZ_MODE_NATIVE || !name || name[0] != '/')
        return 0;
    char default_root[PATH_MAX];
    if (!root) {
        char exe[PATH_MAX];
        uint32_t size = sizeof exe;
        if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, default_root))
            return 0;
        char *slash = strrchr(default_root, '/');
        if (!slash || (size_t)(slash - default_root) + sizeof "/runtime/guest" > sizeof default_root)
            return 0;
        strcpy(slash, "/runtime/guest");
        root = default_root;
    }
    if (!root[0])
        return 0;
    static const char cryptex[] = "/System/Volumes/Preboot/Cryptexes/OS";
    if (strncmp(name, cryptex, sizeof cryptex - 1) == 0 && name[sizeof cryptex - 1] == '/')
        name += sizeof cryptex - 1;
    int len = snprintf(out, n, "%s%s", root, name);
    struct stat st;
    return len > 0 && (size_t)len < n && lstat(out, &st) == 0;
}

static DynImage *load_disk_dylib(OcerzCache *cache, const char *install_name, DynImage *loader,
                                 const RpathList *rpaths)
{
    DynImage *by_name = dimg_find_by_install_name(install_name);
    if (by_name)
        return by_name;
    char guest_path[1024];
    int guest_override = native_guest_path(install_name, guest_path, sizeof guest_path);
    if (!guest_override && ocerz_mode == OCERZ_MODE_NATIVE && ocerz_vdylib_have(install_name)) {
        if (g_dimgs_n >= DYN_DIMG_MAX) {
            OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
            native_dl_reason("the loader holds as many images as it can", NULL);
            return NULL;
        }
        size_t vlen = 0;
        uint8_t *vbuf = ocerz_vdylib_image(install_name, &vlen);
        if (!vbuf || vlen == 0) {
            OCERZ_FATAL("cannot synthesize %s\n", install_name);
            native_dl_reason("its API database would not build an image", NULL);
            free(vbuf);
            return NULL;
        }
        DynImage *v = &g_dimgs[g_dimgs_n++];
        memset(v, 0, sizeof *v);
        v->slice = vbuf;
        v->owned_buf = vbuf;
        v->is_virtual = 1;
        snprintf(v->path, sizeof v->path, "%s", install_name);
        snprintf(v->install_name, sizeof v->install_name, "%s", install_name);
        dimg_record_id(v);
        if (map_segments(v, 0) != OCERZ_OK) {
            OCERZ_FATAL("cannot map segments of virtual %s\n", install_name);
            native_dl_reason("its synthesized image could not be mapped", NULL);
            g_dimgs_n--;
            free(vbuf);
            return NULL;
        }
        protect_ro_segments(v);
        v->seq = ++g_dimg_seq;
        OCERZ_LOG("dynamic: registered virtual dylib %s at load_base=%#llx slide=%#llx\n",
                  install_name, (unsigned long long)v->load_base,
                  (unsigned long long)v->slide);
        return v;
    }
    char resolved[1024];
    if (guest_override) {
        snprintf(resolved, sizeof resolved, "%s", guest_path);
        OCERZ_LOG("dynamic: guest override %s -> %s\n", install_name, resolved);
    } else if (!expand_install_name(loader, install_name, rpaths, resolved, sizeof resolved) ||
        resolved[0] == '@') {
        native_dl_reason("it is in no LC_RPATH directory of the images that load it", NULL);
        return NULL;
    }
    if (dep_find(cache, resolved) != 0)
        return NULL;

    char canon[1024];
    if (ocerz_canon_dylib_path(resolved, canon, sizeof canon) && strcmp(canon, resolved) != 0) {
        snprintf(resolved, sizeof resolved, "%s", canon);
        if (dep_find(cache, resolved) != 0)
            return NULL;
    }
    DynImage *existing = dimg_find_by_path(resolved);
    if (existing)
        return existing;
    uint64_t fdev = 0, fino = 0;
    if (file_identity(resolved, &fdev, &fino) && (existing = dimg_find_by_identity(fdev, fino)))
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
        native_dl_reason("the loader holds as many images as it can", NULL);
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
        if (g_ndl.active) {
            const char *real = host_cache_real_path(resolved);
            if (real)
                native_dl_reason("%s is a native library without an API database", real);
            else
                native_dl_reason("no such file", NULL);
        }
        return NULL;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        if (g_ndl.active)
            OCERZ_LOG("dynamic: %s has no x86_64 slice\n", resolved);
        else
            OCERZ_FATAL("incompatible architecture: %s has no x86_64 slice\n", resolved);
        native_dl_reason("%s has no x86_64 slice, so it is a native library without an API database", resolved);
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
        native_dl_reason("its segments could not be mapped", NULL);
        g_dimgs_n--;
        free(buf);
        return NULL;
    }

    RpathList *merged = malloc(sizeof *merged);
    if (!merged) {
        OCERZ_FATAL("no memory for the rpaths of %s\n", resolved);
        return NULL;
    }
    collect_rpaths(d, rpaths, merged);
    load_disk_deps(cache, d, merged);
    free(merged);

    if (apply_fixups(d, cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot apply fixups of %s\n", resolved);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);
    d->seq = ++g_dimg_seq;

    if (ocerz_mode == OCERZ_MODE_CACHE)
        ocerz_dyldapi_register_image(d->load_base, d->path);
    if (!g_ndl.active)
        canonicalize_objc_selrefs(d);
    protect_ro_segments(d);
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
            if (noff < rd32(lc + 4)) {
                const char *name = (const char *)(lc + noff);
                DynImage *dep = load_disk_dylib(cache, name, loader, rpaths);
                if (!dep && g_ndl.active && cmd != LC_LOAD_WEAK_DYLIB && !g_ndl.missing[0]) {
                    snprintf(g_ndl.missing, sizeof g_ndl.missing, "%s", name);
                    snprintf(g_ndl.missing_from, sizeof g_ndl.missing_from, "%s", loader->path);
                    if (!g_ndl.reason[0])
                        snprintf(g_ndl.reason, sizeof g_ndl.reason, "it could not be loaded");
                }
                if (g_ndl.active && !g_ndl.missing[0])
                    g_ndl.reason[0] = '\0';
            }
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
    RpathList *merged = malloc(sizeof *merged);
    if (!merged) {
        dlerror_set("dlopen(%s): no memory for its rpaths", install_path);
        return NULL;
    }
    collect_rpaths(d, NULL, merged);
    load_disk_deps(cache, d, merged);
    free(merged);
    if (apply_fixups(d, cache) != OCERZ_OK) {
        dlerror_set("dlopen(%s): cannot apply fixups", install_path);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);
    if (ocerz_mode == OCERZ_MODE_CACHE)
        ocerz_dyldapi_register_image(d->load_base, d->path);
    canonicalize_objc_selrefs(d);
    protect_ro_segments(d);
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
    ocerz_dyldapi_register_cache_image(cmh);
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
    protect_ro_flush();
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

static uint64_t main_image_resolve_ex(const char *sym, int *found)
{
    *found = 0;
    if (!g_main_dimg_valid)
        return 0;
    uint64_t value = ocerz_image_self_resolve_ex(&g_main_dimg, sym, found);
    if (*found)
        return value;
    value = image_symtab_resolve(&g_main_dimg, sym);
    *found = value != 0;
    return value;
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

#define NDL_RTLD_LOCAL 0x4
#define NDL_RTLD_NOLOAD 0x10
#define NDL_RTLD_FIRST 0x100
#define NDL_NEXT ((uint64_t)-1)
#define NDL_DEFAULT ((uint64_t)-2)
#define NDL_SELF ((uint64_t)-3)
#define NDL_MAIN_ONLY ((uint64_t)-5)
#define NDL_ERR_BYTES 2048
#define NDL_TRIED_BYTES 1536

extern uint64_t g_main_path;

static _Atomic int g_dimgs_pub;
static uint64_t g_native_init_args[5];

static void native_publish(void)
{
    atomic_store_explicit(&g_dimgs_pub, g_dimgs_n, memory_order_release);
}

static int ndl_pub(void)
{
    return atomic_load_explicit(&g_dimgs_pub, memory_order_acquire);
}

typedef struct NdlErr {
    uint64_t buf;
    int pending;
} NdlErr;

static pthread_key_t g_ndl_err_key;
static pthread_once_t g_ndl_err_once = PTHREAD_ONCE_INIT;

static void ndl_err_release(void *p)
{
    NdlErr *e = p;
    if (!e)
        return;
    if (e->buf)
        ocerz_unmap(e->buf, NDL_ERR_BYTES);
    free(e);
}

static void ndl_err_init(void)
{
    pthread_key_create(&g_ndl_err_key, ndl_err_release);
}

static NdlErr *ndl_err_self(int make)
{
    pthread_once(&g_ndl_err_once, ndl_err_init);
    NdlErr *e = pthread_getspecific(g_ndl_err_key);
    if (e || !make)
        return e;
    e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->buf = ocerz_map_anywhere(NDL_ERR_BYTES, PROT_READ | PROT_WRITE);
    if (!e->buf || pthread_setspecific(g_ndl_err_key, e) != 0) {
        if (e->buf)
            ocerz_unmap(e->buf, NDL_ERR_BYTES);
        free(e);
        return NULL;
    }
    return e;
}

static void ndl_err_clear(void)
{
    NdlErr *e = ndl_err_self(0);
    if (e)
        e->pending = 0;
}

__attribute__((format(printf, 1, 2)))
static void ndl_err(const char *fmt, ...)
{
    char msg[NDL_ERR_BYTES];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLERR %s\n", msg);
    NdlErr *e = ndl_err_self(1);
    if (!e)
        return;
    memcpy(ocerz_g2h(e->buf), msg, strlen(msg) + 1);
    e->pending = 1;
}

uint64_t ocerz_dyld_native_dlerror(void)
{
    NdlErr *e = ndl_err_self(0);
    if (!e || !e->pending)
        return 0;
    e->pending = 0;
    return e->buf;
}

typedef struct NdlImage {
    uint64_t mh;
    int64_t slide;
    uint64_t name;
    DynImage *d;
} NdlImage;

uint32_t ocerz_dyld_image_count(void)
{
    return ocerz_main_mh ? 1u + (uint32_t)ndl_pub() : 0;
}

static int ndl_image(uint32_t index, NdlImage *out)
{
    memset(out, 0, sizeof *out);
    if (!ocerz_main_mh)
        return 0;
    if (index == 0) {
        out->mh = ocerz_main_mh;
        out->slide = image_slide_d(ocerz_main_mh);
        out->name = g_main_path ? g_main_path : ocerz_h2g(g_main_hostpath);
        out->d = g_main_dimg_valid ? &g_main_dimg : NULL;
        return 1;
    }
    if (index - 1 >= (uint32_t)ndl_pub())
        return 0;
    DynImage *d = &g_dimgs[index - 1];
    out->mh = d->load_base;
    out->slide = (int64_t)d->slide;
    out->name = ocerz_h2g(d->path);
    out->d = d;
    return 1;
}

static int ndl_covers(uint64_t mh, uint64_t addr, uint64_t len, int *readonly)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (!mh || rd32(h) != MH_MAGIC_64)
        return 0;
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        if (rd32(lc) == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24), vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 60);
            uint64_t lo = (uint64_t)((int64_t)vmaddr + slide);
            if (vmsize && !(vmaddr == 0 && initprot == 0) && addr >= lo && addr - lo < vmsize) {
                if (readonly)
                    *readonly = !(initprot & VM_PROT_WRITE) && len <= vmsize - (addr - lo);
                return 1;
            }
        }
        lc += rd32(lc + 4);
    }
    return 0;
}

static int ndl_containing(uint64_t addr, NdlImage *out, uint32_t *index_out)
{
    uint32_t n = ocerz_dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        if (ndl_image(i, out) && ndl_covers(out->mh, addr, 1, NULL)) {
            if (index_out)
                *index_out = i;
            return 1;
        }
    }
    memset(out, 0, sizeof *out);
    return 0;
}

int ocerz_dyld_image_at(uint32_t index, uint64_t *mh, uint64_t *slide, uint64_t *name)
{
    NdlImage im;
    int ok = ndl_image(index, &im);
    if (mh)
        *mh = im.mh;
    if (slide)
        *slide = (uint64_t)im.slide;
    if (name)
        *name = im.name;
    return ok;
}

int ocerz_dyld_unwind_sections(uint64_t addr, uint64_t sections)
{
    NdlImage im;
    if (!sections)
        return 0;
    uint64_t result[5] = {0};
    if (!ndl_containing(addr, &im, NULL) || (im.d && im.d->is_virtual)) {
        memcpy(ocerz_g2h(sections), result, sizeof result);
        return 0;
    }
    result[0] = im.mh;
    const struct mach_header_64 *mh = ocerz_g2h(im.mh);
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    const uint8_t *end = lc + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds && (size_t)(end - lc) >= sizeof(struct load_command); i++) {
        const struct load_command *cmd = (const void *)lc;
        if (cmd->cmdsize < sizeof *cmd || cmd->cmdsize > (size_t)(end - lc))
            break;
        if (cmd->cmd == LC_SEGMENT_64 && cmd->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const void *)lc;
            const struct section_64 *sec = (const void *)(seg + 1);
            uint32_t count = (cmd->cmdsize - sizeof *seg) / sizeof *sec;
            for (uint32_t j = 0; j < seg->nsects && j < count; j++) {
                int slot = 0;
                if (strncmp(sec[j].segname, "__TEXT", 16) != 0)
                    continue;
                if (strncmp(sec[j].sectname, "__eh_frame", 16) == 0)
                    slot = 1;
                else if (strncmp(sec[j].sectname, "__unwind_info", 16) == 0)
                    slot = 3;
                if (slot && sec[j].size) {
                    result[slot] = sec[j].addr + im.slide;
                    result[slot + 1] = sec[j].size;
                }
            }
        }
        lc += cmd->cmdsize;
    }
    memcpy(ocerz_g2h(sections), result, sizeof result);
    return 1;
}

int ocerz_dyld_image_containing(uint64_t addr, uint64_t *mh, uint64_t *name)
{
    NdlImage im;
    int ok = ndl_containing(addr, &im, NULL);
    if (mh)
        *mh = im.mh;
    if (name)
        *name = im.name;
    return ok;
}

int ocerz_dyld_image_slide(uint64_t mh, uint64_t *slide)
{
    uint32_t n = ocerz_dyld_image_count();
    NdlImage im;
    for (uint32_t i = 0; i < n; i++) {
        if (ndl_image(i, &im) && im.mh == mh) {
            *slide = (uint64_t)im.slide;
            return 1;
        }
    }
    *slide = 0;
    return 0;
}

static DynImage *ndl_dimg_for_mh(uint64_t mh)
{
    if (!mh)
        return NULL;
    if (mh == ocerz_main_mh)
        return g_main_dimg_valid ? &g_main_dimg : NULL;
    int pub = ndl_pub();
    for (int i = 0; i < pub; i++)
        if (g_dimgs[i].load_base == mh)
            return &g_dimgs[i];
    return NULL;
}

static uint64_t ndl_lookup_in(DynImage *d, const char *usym, int *found)
{
    uint64_t v = ocerz_image_self_resolve_ex(d, usym, found);
    if (*found)
        return v;
    v = image_symtab_resolve(d, usym);
    *found = v != 0;
    return v;
}

static DynImage *ndl_dep_of(DynImage *img, const char *name, int pub)
{
    char ex[1024];
    const char *alt = NULL;
    if (name[0] == '@' && expand_at_prefix(img, name, ex, sizeof ex))
        alt = ex;
    for (int i = 0; i < pub; i++) {
        DynImage *d = &g_dimgs[i];
        if (strcmp(d->install_name, name) == 0 || strcmp(d->id_name, name) == 0 ||
            strcmp(d->path, name) == 0)
            return d;
        if (alt && (strcmp(d->path, alt) == 0 || strcmp(d->install_name, alt) == 0))
            return d;
    }
    return NULL;
}

static uint64_t ndl_search_deps(DynImage *root, const char *usym, int *found)
{
    int pub = ndl_pub();
    DynImage *queue[DYN_DIMG_MAX + 1];
    int qn = 0;
    queue[qn++] = root;
    for (int qi = 0; qi < qn; qi++) {
        DynImage *d = queue[qi];
        uint64_t v = ndl_lookup_in(d, usym, found);
        if (*found)
            return v;
        const uint8_t *mh = d->slice;
        uint32_t ncmds = rd32(mh + 16);
        const uint8_t *lc = mh + sizeof(struct mach_header_64);
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc);
            uint32_t noff = rd32(lc + 8);
            if ((cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB ||
                 cmd == LC_LOAD_UPWARD_DYLIB) && noff < rd32(lc + 4)) {
                DynImage *dep = ndl_dep_of(d, (const char *)(lc + noff), pub);
                int seen = !dep;
                for (int k = 0; k < qn && !seen; k++)
                    seen = queue[k] == dep;
                if (!seen && qn < DYN_DIMG_MAX + 1)
                    queue[qn++] = dep;
            }
            lc += rd32(lc + 4);
        }
    }
    *found = 0;
    return 0;
}

static uint64_t ndl_search_from(uint32_t start, uint32_t own, const char *usym, int *found)
{
    uint32_t n = ocerz_dyld_image_count();
    NdlImage im;
    for (uint32_t i = start; i < n; i++) {
        if (!ndl_image(i, &im) || !im.d || (im.d->local && i != own))
            continue;
        uint64_t v = ndl_lookup_in(im.d, usym, found);
        if (*found)
            return v;
    }
    *found = 0;
    return 0;
}

static void ndl_handle_text(uint64_t handle, char *out, size_t n)
{
    if (handle == NDL_DEFAULT)
        snprintf(out, n, "RTLD_DEFAULT");
    else if (handle == NDL_NEXT)
        snprintf(out, n, "RTLD_NEXT");
    else if (handle == NDL_SELF)
        snprintf(out, n, "RTLD_SELF");
    else if (handle == NDL_MAIN_ONLY)
        snprintf(out, n, "RTLD_MAIN_ONLY");
    else
        snprintf(out, n, "%#llx", (unsigned long long)handle);
}

uint64_t ocerz_dyld_native_dlsym(uint64_t handle, const char *name, uint64_t caller)
{
    char htext[32];
    char usym[1024];
    int found = 0;
    uint64_t v = 0;

    ndl_err_clear();
    ndl_handle_text(handle, htext, sizeof htext);
    if (!name || snprintf(usym, sizeof usym, "_%s", name) >= (int)sizeof usym) {
        ndl_err("dlsym(%s, %s): symbol not found", htext, name ? name : "(null)");
        return 0;
    }
    if (handle == NDL_DEFAULT) {
        v = ndl_search_from(0, UINT32_MAX, usym, &found);
    } else if (handle == NDL_MAIN_ONLY) {
        if (g_main_dimg_valid)
            v = ndl_lookup_in(&g_main_dimg, usym, &found);
    } else if (handle == NDL_NEXT || handle == NDL_SELF) {
        NdlImage im;
        uint32_t ci = 0, start = 0, own = UINT32_MAX;
        if (ndl_containing(caller, &im, &ci)) {
            start = handle == NDL_NEXT ? ci + 1 : ci;
            own = handle == NDL_SELF ? ci : UINT32_MAX;
        }
        v = ndl_search_from(start, own, usym, &found);
    } else {
        DynImage *d = ndl_dimg_for_mh(handle & ~1ull);
        if (!d) {
            ndl_err("dlsym(%s, %s): invalid handle", htext, name);
            return 0;
        }
        v = (handle & 1) ? ndl_lookup_in(d, usym, &found) : ndl_search_deps(d, usym, &found);
    }
    if (!found) {
        ndl_err("dlsym(%s, %s): symbol not found", htext, name);
        return 0;
    }
    return v;
}

int ocerz_dyld_native_dlclose(uint64_t handle)
{
    ndl_err_clear();
    if (handle == NDL_DEFAULT || handle == NDL_MAIN_ONLY || ndl_dimg_for_mh(handle & ~1ull))
        return 0;
    ndl_err("dlclose(%#llx): invalid handle", (unsigned long long)handle);
    return -1;
}

typedef struct NdlSym {
    uint64_t addr;
    char *name;
} NdlSym;

typedef struct NdlSyms {
    NdlSym *v;
    int n;
    int cap;
    uint64_t lo;
    uint64_t hi;
} NdlSyms;

static NdlSyms *_Atomic g_ndl_syms[DYN_DIMG_MAX];
static pthread_mutex_t g_ndl_syms_lock = PTHREAD_MUTEX_INITIALIZER;

static int ndl_syms_add(void *ctx, const char *name, uint64_t value, uint64_t flags)
{
    NdlSyms *s = ctx;
    if ((flags & 0x08) || (flags & 0x03) == 0x02 || value < s->lo || value >= s->hi)
        return 0;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 256;
        NdlSym *grown = realloc(s->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return 1;
        s->v = grown;
        s->cap = cap;
    }
    char *copy = strdup(name);
    if (!copy)
        return 1;
    s->v[s->n].addr = value;
    s->v[s->n].name = copy;
    s->n++;
    return 0;
}

static int ndl_sym_cmp(const void *a, const void *b)
{
    uint64_t x = ((const NdlSym *)a)->addr, y = ((const NdlSym *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static NdlSyms *ndl_syms_of(DynImage *d)
{
    long idx = d - g_dimgs;
    if (idx < 0 || idx >= DYN_DIMG_MAX)
        return NULL;
    NdlSyms *s = g_ndl_syms[idx];
    if (s)
        return s;
    pthread_mutex_lock(&g_ndl_syms_lock);
    s = g_ndl_syms[idx];
    if (!s) {
        s = calloc(1, sizeof *s);
        if (s) {
            s->lo = d->map_base;
            s->hi = d->map_base + d->map_size;
            ocerz_dyld_trie_each(d->slice, d->load_base, ndl_syms_add, s);
            if (s->n > 1)
                qsort(s->v, (size_t)s->n, sizeof s->v[0], ndl_sym_cmp);
            g_ndl_syms[idx] = s;
        }
    }
    pthread_mutex_unlock(&g_ndl_syms_lock);
    return s;
}

static int ndl_trie_nearest(DynImage *d, uint64_t addr, uint64_t *sname, uint64_t *saddr)
{
    NdlSyms *s = ndl_syms_of(d);
    if (!s || !s->n || addr < s->v[0].addr)
        return 0;
    int lo = 0, hi = s->n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (s->v[mid].addr <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }
    const char *nm = s->v[lo].name;
    *sname = ocerz_h2g(nm[0] == '_' ? nm + 1 : nm);
    *saddr = s->v[lo].addr;
    return 1;
}

static int ndl_symtab_nearest(uint64_t mh, int64_t slide, uint64_t addr, uint64_t *sname,
                              uint64_t *saddr)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint64_t le_vmaddr = 0, le_fileoff = 0, le_filesize = 0;
    int have_le = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SYMTAB) {
            symoff = rd32(lc + 8);
            nsyms = rd32(lc + 12);
            stroff = rd32(lc + 16);
            strsize = rd32(lc + 20);
        } else if (cmd == LC_SEGMENT_64 && strncmp((const char *)(lc + 8), "__LINKEDIT", 16) == 0) {
            le_vmaddr = rd64(lc + 24);
            le_fileoff = rd64(lc + 40);
            le_filesize = rd64(lc + 48);
            have_le = 1;
        }
        lc += rd32(lc + 4);
    }
    if (!have_le || !nsyms || symoff < le_fileoff || stroff < le_fileoff ||
        symoff + (uint64_t)nsyms * 16 > le_fileoff + le_filesize ||
        (uint64_t)stroff + strsize > le_fileoff + le_filesize)
        return 0;
    uint64_t symtab = (uint64_t)((int64_t)le_vmaddr + slide) + (symoff - le_fileoff);
    uint64_t strtab = (uint64_t)((int64_t)le_vmaddr + slide) + (stroff - le_fileoff);
    uint64_t best = 0, best_strx = 0;
    int have = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = (const uint8_t *)ocerz_g2h(symtab + (uint64_t)i * 16);
        uint32_t strx = rd32(e);
        uint8_t type = e[4];
        if ((type & 0xe0) || (type & 0x0e) != 0x0e || strx == 0 || strx >= strsize)
            continue;
        uint64_t val = (uint64_t)((int64_t)rd64(e + 8) + slide);
        if (val > addr || (have && val <= best))
            continue;
        best = val;
        best_strx = strx;
        have = 1;
    }
    if (!have)
        return 0;
    uint64_t namep = strtab + best_strx;
    if (*(const char *)ocerz_g2h(namep) == '_')
        namep++;
    *sname = namep;
    *saddr = best;
    return 1;
}

static int ndl_host_dladdr(uint64_t addr, uint64_t info)
{
    const void *h = ocerz_g2h(addr);
    Dl_info di;
    if (ocerz_host_in_guest_reservation(h) || !dladdr(h, &di))
        return 0;
    ocerz_st(info + 0x00, 8, di.dli_fname ? ocerz_h2g(di.dli_fname) : 0);
    ocerz_st(info + 0x08, 8, di.dli_fbase ? ocerz_h2g(di.dli_fbase) : 0);
    ocerz_st(info + 0x10, 8, di.dli_sname ? ocerz_h2g(di.dli_sname) : 0);
    ocerz_st(info + 0x18, 8, di.dli_saddr ? ocerz_h2g(di.dli_saddr) : 0);
    return 1;
}

int ocerz_dyld_native_dladdr(uint64_t addr, uint64_t info)
{
    NdlImage im;
    if (!info)
        return 0;
    if (!ndl_containing(addr, &im, NULL))
        return ndl_host_dladdr(addr, info);
    uint64_t sname = 0, saddr = 0;
    if (!ndl_symtab_nearest(im.mh, im.slide, addr, &sname, &saddr) && im.d && im.d != &g_main_dimg)
        ndl_trie_nearest(im.d, addr, &sname, &saddr);
    ocerz_st(info + 0x00, 8, im.name);
    ocerz_st(info + 0x08, 8, im.mh);
    ocerz_st(info + 0x10, 8, sname);
    ocerz_st(info + 0x18, 8, saddr);
    return 1;
}

typedef enum NdlKind {
    NDL_NONE,
    NDL_MAIN,
    NDL_LOADED,
    NDL_VIRTUAL,
    NDL_FILE,
    NDL_NATIVE_ONLY,
    NDL_BAD_FILE,
} NdlKind;

typedef struct NdlTarget {
    NdlKind kind;
    DynImage *img;
    uint8_t *buf;
    size_t len;
    char path[PATH_MAX];
    char why[PATH_MAX + 128];
    char tried[NDL_TRIED_BYTES];
    size_t tried_len;
} NdlTarget;

static void ndl_tried(NdlTarget *t, const char *cand)
{
    size_t room = sizeof t->tried - t->tried_len;
    int n = snprintf(t->tried + t->tried_len, room, "%s'%s' (no such file)", t->tried_len ? ", " : "", cand);
    if (n > 0)
        t->tried_len += (size_t)n < room ? (size_t)n : room - 1;
}

static int ndl_is_main_path(const char *p)
{
    char rp[PATH_MAX];
    return g_main_hostpath[0] &&
           (strcmp(p, g_main_hostpath) == 0 || (realpath(p, rp) && strcmp(rp, g_main_hostpath) == 0));
}

static int ndl_known(const char *p, NdlTarget *t)
{
    if (ndl_is_main_path(p)) {
        t->kind = NDL_MAIN;
        return 1;
    }
    t->img = dimg_find_by_path(p);
    if (!t->img)
        t->img = dimg_find_by_install_name(p);
    if (t->img) {
        t->kind = NDL_LOADED;
        return 1;
    }
    char guest_path[PATH_MAX];
    if (!native_guest_path(p, guest_path, sizeof guest_path) && ocerz_vdylib_have(p)) {
        t->kind = NDL_VIRTUAL;
        snprintf(t->path, sizeof t->path, "%s", p);
        return 1;
    }
    return 0;
}

static int ndl_try(const char *cand, NdlTarget *t)
{
    char canon[PATH_MAX];
    char guest_path[PATH_MAX];
    const char *c = cand;
    if (ndl_known(cand, t))
        return 1;
    int guest_override = native_guest_path(cand, guest_path, sizeof guest_path);
    if (guest_override) {
        c = guest_path;
    } else if (ocerz_canon_dylib_path(cand, canon, sizeof canon) && strcmp(canon, cand) != 0) {
        c = canon;
        if (ndl_known(c, t))
            return 1;
    }
    uint64_t dev = 0, ino = 0;
    if (file_identity(c, &dev, &ino)) {
        if (ocerz_main_mh && dev == g_main_dev && ino == g_main_ino) {
            t->kind = NDL_MAIN;
            return 1;
        }
        if ((t->img = dimg_find_by_identity(dev, ino))) {
            t->kind = NDL_LOADED;
            return 1;
        }
        char abs[PATH_MAX];
        if (!realpath(c, abs))
            snprintf(abs, sizeof abs, "%s", c);
        size_t len = 0;
        uint8_t *buf = read_file(abs, &len);
        if (!buf) {
            t->kind = NDL_BAD_FILE;
            snprintf(t->why, sizeof t->why, "'%s' could not be read", abs);
            return 1;
        }
        uint32_t magic = len >= 4 ? rd32(buf) : 0;
        if (select_slice(buf, len)) {
            t->kind = NDL_FILE;
            t->buf = buf;
            t->len = len;
            snprintf(t->path, sizeof t->path, "%s", abs);
            return 1;
        }
        free(buf);
        if (magic == MH_MAGIC_64 || magic == MH_MAGIC || magic == FAT_MAGIC || magic == FAT_CIGAM ||
            magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
            t->kind = NDL_NATIVE_ONLY;
            snprintf(t->why, sizeof t->why, "'%s' has no x86_64 slice", abs);
        } else {
            t->kind = NDL_BAD_FILE;
            snprintf(t->why, sizeof t->why, "'%s' is not a Mach-O file", abs);
        }
        return 1;
    }
    if (guest_override) {
        t->kind = NDL_BAD_FILE;
        snprintf(t->why, sizeof t->why, "guest override '%s' could not be read", c);
        return 1;
    }
    const char *real = host_cache_real_path(cand);
    if (real) {
        if (ndl_known(real, t))
            return 1;
        t->kind = NDL_NATIVE_ONLY;
        snprintf(t->why, sizeof t->why, "'%s' is in the host's shared cache", real);
        return 1;
    }
    ndl_tried(t, cand);
    return 0;
}

static int ndl_try_dirs(const char *list, const char *leaf, NdlTarget *t)
{
    for (const char *p = list; p && *p;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char cand[PATH_MAX];
        if (len > 0 && snprintf(cand, sizeof cand, "%.*s/%s", (int)len, p, leaf) < (int)sizeof cand &&
            ndl_try(cand, t))
            return 1;
        p += len;
        if (*p == ':')
            p++;
    }
    return 0;
}

static RpathList *ndl_rpaths(DynImage *caller)
{
    RpathList *own = calloc(1, sizeof *own), *all = calloc(1, sizeof *all);
    if (!own || !all) {
        free(own);
        free(all);
        return NULL;
    }
    if (caller && caller != &g_main_dimg)
        collect_rpaths(caller, NULL, own);
    if (g_main_dimg_valid)
        collect_rpaths(&g_main_dimg, own, all);
    else
        memcpy(all, own, sizeof *all);
    free(own);
    return all;
}

static int ndl_resolve(const char *p, DynImage *caller, NdlTarget *t)
{
    char cand[PATH_MAX];
    if (strncmp(p, "@rpath/", 7) == 0) {
        RpathList *rp = ndl_rpaths(caller);
        int ok = 0;
        for (int i = 0; rp && i < rp->n && !ok; i++)
            if (snprintf(cand, sizeof cand, "%s/%s", rp->entry[i], p + 7) < (int)sizeof cand)
                ok = ndl_try(cand, t);
        free(rp);
        if (!ok && !t->tried_len)
            ndl_tried(t, p);
        return ok;
    }
    if (p[0] == '@') {
        if (expand_at_prefix(caller, p, cand, sizeof cand))
            return ndl_try(cand, t);
        ndl_tried(t, p);
        return 0;
    }
    if (strchr(p, '/'))
        return ndl_try(p, t);
    if (ndl_try_dirs(getenv("DYLD_LIBRARY_PATH"), p, t) || ndl_try(p, t))
        return 1;
    const char *fb = getenv("DYLD_FALLBACK_LIBRARY_PATH");
    return ndl_try_dirs(fb && fb[0] ? fb : "/usr/local/lib:/usr/lib", p, t);
}

static void rpaths_append(RpathList *dst, const RpathList *src)
{
    for (int i = 0; src && i < src->n && dst->n < RPATH_MAX; i++)
        snprintf(dst->entry[dst->n++], sizeof dst->entry[0], "%s", src->entry[i]);
}

static DynImage *ndl_load_file(NdlTarget *t, const RpathList *chain)
{
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        native_dl_reason("the loader holds as many images as it can", NULL);
        return NULL;
    }
    DynImage *d = &g_dimgs[g_dimgs_n++];
    memset(d, 0, sizeof *d);
    d->slice = select_slice(t->buf, t->len);
    d->owned_buf = t->buf;
    t->buf = NULL;
    snprintf(d->path, sizeof d->path, "%s", t->path);
    snprintf(d->install_name, sizeof d->install_name, "%s", t->path);
    file_identity(t->path, &d->file_dev, &d->file_ino);
    dimg_record_id(d);
    if (map_segments(d, 0) != OCERZ_OK) {
        native_dl_reason("its segments could not be mapped", NULL);
        g_dimgs_n--;
        free(d->owned_buf);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    RpathList *rp = calloc(1, sizeof *rp);
    if (!rp) {
        native_dl_reason("no memory for its rpaths", NULL);
        return NULL;
    }
    collect_rpaths(d, NULL, rp);
    rpaths_append(rp, chain);
    load_disk_deps(g_run_cache, d, rp);
    free(rp);
    if (apply_fixups(d, g_run_cache) != OCERZ_OK) {
        native_dl_reason("its chained fixups use a pointer format ocerz does not apply", NULL);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, g_run_cache);
    protect_ro_segments(d);
    d->seq = ++g_dimg_seq;
    OCERZ_LOG("dynamic: native dlopen loaded %s at load_base=%#llx slide=%#llx\n", d->path,
              (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

static void ndl_rollback(int before)
{
    for (int i = g_dimgs_n - 1; i >= before; i--) {
        DynImage *d = &g_dimgs[i];
        protect_ro_drop(d);
        if (d->map_size)
            ocerz_unmap(d->map_base, d->map_size);
        free(d->owned_buf);
        memset(d, 0, sizeof *d);
    }
    g_dimgs_n = before;
}

static uint64_t *g_ndl_add_funcs;
static int g_ndl_add_n, g_ndl_add_cap;
static uint64_t *g_ndl_remove_funcs;
static int g_ndl_remove_n, g_ndl_remove_cap;

static int ndl_append_func(uint64_t **arr, int *n, int *cap, uint64_t fn)
{
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 16;
        uint64_t *grown = realloc(*arr, (size_t)c * sizeof *grown);
        if (!grown)
            return 0;
        *arr = grown;
        *cap = c;
    }
    (*arr)[(*n)++] = fn;
    return 1;
}

static void ndl_call_add(struct OcerzVM *vm, uint64_t fn, uint64_t mh, int64_t slide,
                         uint64_t stack_top)
{
    uint64_t args[2] = { mh, (uint64_t)slide };
    ocerz_vm_call(vm, fn, args, 2, stack_top);
}

int ocerz_dyld_native_add_image_func(struct OcerzVM *vm, uint64_t func, uint64_t stack_top)
{
    if (!func)
        return 0;
    pthread_mutex_lock(&g_load_lock);
    int ok = ndl_append_func(&g_ndl_add_funcs, &g_ndl_add_n, &g_ndl_add_cap, func);
    uint32_t n = ok ? ocerz_dyld_image_count() : 0;
    NdlImage im;
    for (uint32_t i = 0; i < n && !vm->exited; i++)
        if (ndl_image(i, &im))
            ndl_call_add(vm, func, im.mh, im.slide, stack_top);
    pthread_mutex_unlock(&g_load_lock);
    return ok;
}

int ocerz_dyld_native_remove_image_func(uint64_t func)
{
    if (!func)
        return 0;
    pthread_mutex_lock(&g_load_lock);
    int ok = ndl_append_func(&g_ndl_remove_funcs, &g_ndl_remove_n, &g_ndl_remove_cap, func);
    pthread_mutex_unlock(&g_load_lock);
    return ok;
}

static int ndl_seq_cmp(const void *a, const void *b)
{
    uint32_t x = (*(DynImage *const *)a)->seq, y = (*(DynImage *const *)b)->seq;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void ndl_fail_load(const char *path, int mode, DynImage *top, int miss_before)
{
    if (!top) {
        ndl_err("dlopen(%s, 0x%04x): %s", path, mode,
                g_ndl.reason[0] ? g_ndl.reason : "it could not be loaded");
    } else if (g_ndl.missing[0]) {
        ndl_err("dlopen(%s, 0x%04x): Library not loaded: %s\n  Referenced from: %s\n  Reason: %s", path,
                mode, g_ndl.missing, g_ndl.missing_from, g_ndl.reason);
    } else if (g_native_miss_n > miss_before) {
        const struct native_miss *m = &g_native_miss[miss_before];
        ndl_err("dlopen(%s, 0x%04x): Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s", path,
                mode, m->sym, m->from, m->lib);
    } else {
        ndl_err("dlopen(%s, 0x%04x): more symbols were not found than ocerz keeps a record of", path, mode);
    }
}

static uint64_t ndl_dlopen_locked(struct OcerzVM *vm, const char *path, int mode, uint64_t caller,
                                  uint64_t stack_top)
{
    ndl_err_clear();
    if (!path)
        return (mode & NDL_RTLD_FIRST) ? NDL_MAIN_ONLY : NDL_DEFAULT;

    NdlImage cim;
    DynImage *caller_d = ndl_containing(caller, &cim, NULL) ? cim.d : NULL;
    NdlTarget *t = calloc(1, sizeof *t);
    if (!t) {
        ndl_err("dlopen(%s, 0x%04x): no memory", path, mode);
        return 0;
    }
    uint64_t first = (mode & NDL_RTLD_FIRST) ? 1 : 0;
    int load = 0;
    uint64_t handle = 0;
    if (!ndl_resolve(path, caller_d, t)) {
        ndl_err("dlopen(%s, 0x%04x): tried: %s", path, mode, t->tried);
    } else if (t->kind == NDL_MAIN) {
        handle = ocerz_main_mh | first;
    } else if (t->kind == NDL_LOADED) {
        if (!(mode & NDL_RTLD_LOCAL))
            t->img->local = 0;
        handle = t->img->load_base | first;
    } else if (t->kind == NDL_NATIVE_ONLY) {
        ndl_err("dlopen(%s, 0x%04x): native library without an API database: %s", path, mode, t->why);
    } else if (t->kind == NDL_BAD_FILE) {
        ndl_err("dlopen(%s, 0x%04x): %s", path, mode, t->why);
    } else if (mode & NDL_RTLD_NOLOAD) {
        ndl_err("dlopen(%s, 0x%04x): not loaded, and RTLD_NOLOAD forbids loading it", path, mode);
    } else {
        load = 1;
    }
    if (!load) {
        free(t->buf);
        free(t);
        return handle;
    }

    int before = g_dimgs_n;
    int miss_before = g_native_miss_n, dropped_before = g_native_miss_dropped;
    memset(&g_ndl, 0, sizeof g_ndl);
    g_ndl.active = 1;
    DynImage *top;
    if (t->kind == NDL_VIRTUAL) {
        top = load_disk_dylib(g_run_cache, t->path, NULL, NULL);
    } else {
        RpathList *chain = ndl_rpaths(caller_d);
        top = ndl_load_file(t, chain);
        free(chain);
    }
    g_ndl.active = 0;
    free(t->buf);
    free(t);
    if (!top || g_ndl.missing[0] || g_native_miss_n > miss_before ||
        g_native_miss_dropped > dropped_before) {
        ndl_fail_load(path, mode, top, miss_before);
        g_native_miss_n = miss_before;
        g_native_miss_dropped = dropped_before;
        ndl_rollback(before);
        return 0;
    }

    int after = g_dimgs_n;
    if (mode & NDL_RTLD_LOCAL)
        top->local = 1;
    native_publish();
    native_tlv_register_loaded(ocerz_main_mh);

    int nf = g_ndl_add_n;
    for (int f = 0; f < nf && !vm->exited; f++)
        for (int i = before; i < after && !vm->exited; i++)
            ndl_call_add(vm, g_ndl_add_funcs[f], g_dimgs[i].load_base, (int64_t)g_dimgs[i].slide,
                         stack_top);

    DynImage *order[DYN_DIMG_MAX];
    int n = 0;
    for (int i = before; i < after; i++)
        if (!g_dimgs[i].is_virtual)
            order[n++] = &g_dimgs[i];
    if (n > 1)
        qsort(order, (size_t)n, sizeof order[0], ndl_seq_cmp);
    for (int i = 0; i < n; i++) {
        const uint8_t *h = (const uint8_t *)ocerz_g2h(order[i]->load_base);
        ocerz_objcbridge_fix_selrefs(h, (int64_t)order[i]->slide);
        ocerz_objcbridge_define_image(h, (int64_t)order[i]->slide);
    }
    if (dimg_find_by_install_name(OCERZ_OBJC_LIBOBJC))
        ocerz_objcbridge_install_uncaught();
    protect_ro_flush();
    for (int i = 0; i < n && !vm->exited; i++) {
        ocerz_objcbridge_run_image_loads(vm, (const uint8_t *)ocerz_g2h(order[i]->load_base), stack_top);
        if (!vm->exited)
            run_image_inits(vm, order[i]->load_base, g_native_init_args, stack_top);
    }
    return top->load_base | first;
}

uint64_t ocerz_dyld_native_dlopen(struct OcerzVM *vm, const char *path, int mode, uint64_t caller,
                                  uint64_t stack_top)
{
    int log = getenv("OCERZ_DLOPENLOG") != NULL;
    if (log)
        fprintf(stderr, "ocerz: DLOPEN \"%s\" mode=%#x\n", path ? path : "(null)", mode);
    pthread_mutex_lock(&g_load_lock);
    uint64_t h = ndl_dlopen_locked(vm, path, mode, caller, stack_top);
    pthread_mutex_unlock(&g_load_lock);
    if (log)
        fprintf(stderr, "ocerz: DLOPEN \"%s\" -> %#llx\n", path ? path : "(null)", (unsigned long long)h);
    return h;
}

int ocerz_dyld_native_dlopen_preflight(const char *path, uint64_t caller)
{
    int ok = 0;
    pthread_mutex_lock(&g_load_lock);
    ndl_err_clear();
    NdlImage cim;
    DynImage *caller_d = ndl_containing(caller, &cim, NULL) ? cim.d : NULL;
    NdlTarget *t = path ? calloc(1, sizeof *t) : NULL;
    if (!path) {
        ok = 1;
    } else if (!t) {
        ndl_err("dlopen_preflight(%s): no memory", path);
    } else if (!ndl_resolve(path, caller_d, t)) {
        ndl_err("dlopen_preflight(%s): tried: %s", path, t->tried);
    } else if (t->kind == NDL_NATIVE_ONLY) {
        ndl_err("dlopen_preflight(%s): native library without an API database: %s", path, t->why);
    } else if (t->kind == NDL_BAD_FILE) {
        ndl_err("dlopen_preflight(%s): %s", path, t->why);
    } else {
        ok = 1;
    }
    if (t)
        free(t->buf);
    free(t);
    pthread_mutex_unlock(&g_load_lock);
    return ok;
}

int ocerz_dyld_native_names_library(const char *path)
{
    char canon[PATH_MAX];
    if (!path || !path[0])
        return 0;
    if (ocerz_vdylib_have(path))
        return 1;
    if (ocerz_canon_dylib_path(path, canon, sizeof canon) && strcmp(canon, path) != 0 &&
        ocerz_vdylib_have(canon))
        return 1;
    const char *real = host_cache_real_path(path);
    return real && ocerz_vdylib_have(real);
}

typedef struct NdlBuildVersion {
    uint32_t platform;
    uint32_t version;
} NdlBuildVersion;

typedef struct NdlHostDyld {
    bool (*immutable)(const void *addr, size_t len);
    bool (*sdk_at_least)(const void *mh, NdlBuildVersion v);
    bool (*minos_at_least)(const void *mh, NdlBuildVersion v);
} NdlHostDyld;

static NdlHostDyld g_ndl_host;
static pthread_once_t g_ndl_host_once = PTHREAD_ONCE_INIT;

static void ndl_host_init(void)
{
    g_ndl_host.immutable = (bool (*)(const void *, size_t))dlsym(RTLD_DEFAULT, "_dyld_is_memory_immutable");
    g_ndl_host.sdk_at_least = (bool (*)(const void *, NdlBuildVersion))dlsym(RTLD_DEFAULT, "dyld_sdk_at_least");
    g_ndl_host.minos_at_least =
        (bool (*)(const void *, NdlBuildVersion))dlsym(RTLD_DEFAULT, "dyld_minos_at_least");
}

static const NdlHostDyld *ndl_host(void)
{
    pthread_once(&g_ndl_host_once, ndl_host_init);
    return &g_ndl_host;
}

int ocerz_dyld_is_memory_immutable(uint64_t addr, uint64_t len)
{
    uint32_t n = ocerz_dyld_image_count();
    NdlImage im;
    for (uint32_t i = 0; i < n; i++) {
        int ro = 0;
        if (ndl_image(i, &im) && ndl_covers(im.mh, addr, len, &ro))
            return ro;
    }
    const void *h = ocerz_g2h(addr);
    if (ocerz_host_in_guest_reservation(h) || !ndl_host()->immutable)
        return 0;
    return ndl_host()->immutable(h, (size_t)len) ? 1 : 0;
}

static int ndl_is_image(uint64_t mh)
{
    uint64_t slide;
    return mh && ocerz_dyld_image_slide(mh, &slide);
}

int ocerz_dyld_build_version(uint64_t mh, uint32_t *platform, uint32_t *minos, uint32_t *sdk)
{
    *platform = *minos = *sdk = 0;
    if (!ndl_is_image(mh))
        return 0;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_BUILD_VERSION) {
            *platform = rd32(lc + 8);
            *minos = rd32(lc + 12);
            *sdk = rd32(lc + 16);
            return 1;
        }
        if (cmd == LC_VERSION_MIN_MACOSX) {
            *platform = PLATFORM_MACOS;
            *minos = rd32(lc + 8);
            *sdk = rd32(lc + 12);
            return 1;
        }
        lc += rd32(lc + 4);
    }
    return 0;
}

int ocerz_dyld_version_at_least(uint64_t mh, uint64_t version, int sdk)
{
    NdlBuildVersion v = { (uint32_t)version, (uint32_t)(version >> 32) };
    uint32_t platform, minos, have;
    if (!ocerz_dyld_build_version(mh, &platform, &minos, &have))
        return 0;
    bool (*host)(const void *, NdlBuildVersion) = sdk ? ndl_host()->sdk_at_least : ndl_host()->minos_at_least;
    if (host)
        return host(ocerz_g2h(mh), v) ? 1 : 0;
    if (v.platform == 0xffffffffu)
        return 1;
    return v.platform == platform && (sdk ? have : minos) >= v.version;
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
        OCERZ_LOG("dynamic: native mode is Rosetta-independent: x86 code runs JIT-translated, system calls bridge to arm64\n");
        OCERZ_LOG("dynamic: native JIT %s, bridge fastcall %s\n",
                  vm->jit_enabled ? "enabled" : "disabled (-no-jit)",
                  getenv("OCERZ_NO_BRIDGE_FASTCALL") ? "disabled (trap path)" : "enabled");
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
        fprintf(stderr, "ocerz: native: to run it translated instead, supply an x86_64 build at $OCERZ_GUEST_ROOT%s (JIT, no Rosetta needed)\n",
                " or runtime/guest beside ocerz");
        free(buf);
        return 71;
    }
    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        native_publish();
        native_tlv_register_loaded(img.load_base);
        ocerz_objcbridge_fix_selrefs((const uint8_t *)ocerz_g2h(img.load_base), img.slide);
        ocerz_objcbridge_define_image((const uint8_t *)ocerz_g2h(img.load_base), img.slide);
        if (dimg_find_by_install_name(OCERZ_OBJC_LIBOBJC))
            ocerz_objcbridge_install_uncaught();
        protect_ro_flush();
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
    if (ocerz_mode == OCERZ_MODE_NATIVE)
        ocerz_fork_register();
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
        protect_ro_flush();
    }

    if (ocerz_mode == OCERZ_MODE_NATIVE) {
        uint64_t nia[5] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars };
        memcpy(g_native_init_args, nia, sizeof nia);
        int loaded = g_dimgs_n;
        ocerz_objcbridge_run_loads(vm, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
        for (int i = loaded - 1; i >= 0 && !vm->exited; i--)
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
