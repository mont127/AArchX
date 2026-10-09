//! Mini-dyld: loads, links and launches a dynamic x86_64 executable against the
//! shared cache.
//!
//! ---- binding ----
//! A cache dependency is bound in the SPECIFIC dylib the image linked, not by a
//! flat search across every cache image: openssl links libcrypto.46.dylib
//! (3.3.6) while the cache also holds libcrypto.44 (2.8.3) exporting the same
//! names, and the flat fallback bound OpenSSL_version to .44 and reported the
//! wrong version.  In the export trie, a terminal size of 0 with the name fully
//! consumed is not a miss: the node carries an empty edge to the terminal child,
//! which happens whenever a symbol is a strict prefix of others (_libiconv
//! versus _libiconv_open), so the search falls through to the child.
//!
//! Real dyld maps each segment with its initprot; this one maps every image
//! read-write so the copy and the fixups can land, then gives __TEXT its real
//! protection once fixups are done.  Leaving it writable is not merely untidy:
//! code sitting in a writable slot is treated as possibly self-modifying, and a
//! write-trapped page costs a fault per store to its data neighbours.
//!
//! dlsym on a handle searches the image, then everything it links breadth
//! first, skipping upward links, which is what dyld answers: a symbol two
//! dependencies down is found, one behind an upward link is not.  The disk-image
//! path used to search the image alone, so dlsym(handle, "malloc") failed for
//! any dylib, and the cache path followed upward links, so CoreFoundation's
//! handle found OpenGL's glBegin by way of Foundation.  A cache image met on the
//! way hands its own closure to ocerz_cache_dlsym_image.  OCERZ_NO_DLSYM_DEPS
//! restores the image-only search for disk images.
//!
//! ---- the initial stack ----
//! Every argument and environment entry goes onto the guest stack, counted first
//! and sized to the real need.  The arrays were once fixed at 64 with the
//! environment cut at 60, which silently dropped the last variables of a large
//! environment - and Wine's loader marks its one-time re-exec by appending
//! WINELOADERNOEXEC=1, so a launch with one variable too many re-exec'd every
//! Wine process forever (2026-09-06, Steam).  The three vectors are one
//! contiguous ascending run - argv NULL envp NULL apple NULL - because that is
//! what XNU's exec path produces and libSystem relies on it: apple is not passed
//! anywhere, it is found by walking off the end of envp.  Stacking them downward
//! instead put the argv vector where apple belongs, so that walk ran past argv's
//! terminator into the string area and handed strlen() the bytes of
//! "th_port=0x..." as a pointer, and python3 died there.
//!
//! The ProgramVars block handed to libSystem's initializer and to every image
//! initializer points at the real NXArgc, NXArgv, environ and __progname -
//! libdyld's in cache mode, the host's in native mode - and not at cells of its
//! own beside the vectors.  libc's _NSGetEnviron and _NSGetArgv answer whatever
//! that block points at, while the environ a program reads is libdyld's variable,
//! so with cells of their own the two came apart at the first setenv: setenv moved
//! *_NSGetEnviron() to a new array and environ went on naming the old one, and a
//! program that called setenv and then execve with environ handed its child an
//! environment without the variable.  On a real system they are one variable,
//! which the app_bundle native case checks against an arm64 build and cache mode.
//!
//! ---- initializers ----
//! Getting this phase right is most of the file.  Whether a program pulls in
//! CoreFoundation/Foundation/AppKit cannot be asked of the main executable's own
//! load commands, because a .app is a small stub linking one umbrella framework
//! and libSystem - Safari, a Cocoa app by any measure, read as "not a CF
//! program" and skipped the dependency-ordered phase entirely.  But libSystem's
//! own closure is deliberately not followed either: libxpc weak-links
//! XPCSupport, which links Foundation, so descending there makes every
//! dynamically linked program look like a Cocoa app, /usr/bin/sort included.
//!
//! The eager set is closed under dependencies as well as over data references,
//! because the walk recurses into every dependency but only RUNS the
//! initializers of images in the set - so an image in it whose dependency is
//! missing gets initialized on top of an uninitialized library.  That is how
//! Safari aborted in an Engram static initializer that called operator new
//! before libc++abi's own initializer had run.
//!
//! The order is dyld's: per image and bottom-up, that image's objc load
//! notification and then its own initializers, with every dependency already
//! finished.  Neither global order works - all +load first runs SiriTTSService's
//! ahead of libc++'s initializer, and all initializers first recurses
//! libsystem_malloc into its own zone setup.  Pruning on the done flag is what
//! keeps the walk small: libSystem's closure is marked done the moment
//! libSystem_initializer returns, so a library whose only dependency is
//! libSystem stops right there instead of descending through libxpc into
//! Foundation and dragging the whole system into its subtree.  Its objc load
//! notifications are therefore delivered before that prune can hide them.
//!
//! A dependency is looked for in the shared cache both before and after its
//! symlinks are followed.  Only the first lookup used to happen, so a path that
//! named the cache under a different spelling was followed to a target that is
//! not on disk and the load failed as a missing file.  Eleven of the twelve
//! dylibs in /usr/lib/swift are symlinks into frameworks whose binaries exist
//! only in the cache, which is how Tailscale lost Network.framework through
//! Sparkle, naming a path the cache was holding all along.
//! An upward link is how a library declares the back edge of a dependency cycle,
//! and it is an ordering edge for nothing: the library that declares it may be
//! initialized first.  So it is not followed on the way DOWN.  Following it
//! there made CoreFoundation's upward link to CoreServicesInternal a real edge,
//! which put QuickLookThumbnailing, SiriTTS and CoreML inside libc++abi's
//! subtree, and libc++abi then sat unfinished on the recursion stack while
//! MLAssetIO's initializer called operator new into a libc++ that had not been
//! initialized yet.
//!
//! It is followed afterwards instead, because the target still has to be
//! initialized at some point - dyld loads and initializes an upward dependency
//! like any other, it only declines to order it first.  "Afterwards" means once
//! the whole walk from the root has finished, not once the declaring image has:
//! each upward target goes on a list the root call owns, and that list is worked
//! through after the root's own initializers, including the targets it adds.
//! Following it right after the declaring image broke sw_vers on macOS 26.7.1,
//! where libobjc links libswiftCore upward and libswiftCore links Foundation
//! upward.  libobjc is a dependency of CoreFoundation, so Foundation's whole
//! subtree, SkyLight among it, ran its initializers while CoreFoundation was
//! still unfinished on the recursion stack, and SkyLight's first allocation
//! through CoreFoundation's allocator recursed between malloc and
//! malloc_zone_malloc until the stack ran out.  Leaving it out entirely is what
//! broke sw_vers on macOS 27:
//! CoreFoundation links Foundation upward, so Foundation's code was reachable,
//! the whole cache being mapped, and its classes were registered, but its
//! initializer never ran.  NSString therefore stayed an abstract class cluster,
//! and the first +[NSString stringWithFormat:] fell into
//! _NSRequestConcreteObject, whose complaint is itself built with
//! +[NSString stringWithFormat:].  That recursion ran the 8 MB guest stack out.
//! OCERZ_NO_UPWARD_INIT=1 restores the old behaviour.
//!
//! The dlopen closure (init_closure) collects its images in dependency
//! post-order and runs them in that order, never sorted by load address: a
//! dlopen'd image is mapped below the dependencies it pulls in, so address
//! order ran a dependent ahead of its dependency.  steamui initialized before
//! libtier0_s, and its first CUtlMemory growth called through g_pMemAlloc while
//! tier0's allocator singleton still had a null vtable.
//!
//! Each image in that closure has its +load methods run before its
//! initializers, as dyld does and as the startup phase always did.  The closure
//! used to run initializers only, so a cache framework first pulled in by a
//! dlopen - Foundation under Wine's ntdll.so - never had its +load run, and
//! +[NSString alloc] and +[NSMutableString alloc] kept handing out the abstract
//! classes instead of their placeholders.  Wine's mountmgr then answered
//! Steam's DHCP query through SystemConfiguration, CFBundle built a string on an
//! abstract NSMutableString, and the same _NSRequestConcreteObject recursion
//! ran winedevice's stack out; Steam saw no network adapters.  The dynamic test
//! dlopen_load_phase pins it; OCERZ_NO_CLOSURE_LOADS=1 restores the old
//! behaviour.
//!
//! The dlopen closure and the dlopen load phase follow an upward link only when
//! it points at Foundation, CoreFoundation or libobjc, so a program that
//! dlopens SystemConfiguration still gets Foundation's +load and initializer
//! through CoreFoundation's upward link.  Following every upward link there
//! brings in CoreServicesInternal's subtree - some 360 more initializers in
//! every Wine process, SkyLight and CoreML among them - and Steam's CEF browser
//! process then died in objc_msgSend.  The dynamic test dlopen_objc_core pins
//! it; OCERZ_NO_DLOPEN_UPWARD=1 turns it off.
//!
//! A dlopen path that begins with @ is resolved against the image that called
//! dlopen, as dyld does: @rpath through that image's LC_RPATHs and then the
//! main executable's, @loader_path against its directory.  The caller is the
//! return address of the dlopen slot, or dlopen_from's third argument.  The
//! path used to go to the loader as written, so it only ever worked relative to
//! nothing; the Game Porting Toolkit's libd3dshared.dylib dlopens
//! @rpath/D3DMetal.framework through its own @loader_path rpath, failed, and
//! aborted every D3DMetal game at its first Direct3D call.  The dynamic test
//! dlopen_caller_rpath pins it; OCERZ_NO_DLOPEN_CALLER_RPATH=1 restores the old
//! behaviour.
//!
//! ---- thread-local variables ----
//! Two descriptor layouts share the same 24 bytes.  A static linker emits the
//! classic tlv_descriptor { thunk, key:u64, offset:u64 }, so the offset is at
//! +0x10; the shared cache ships the packed form dyld uses now - { thunk,
//! key:u32, offset:u32, initialContentDelta:i32, initialContentSize:u32 } -
//! where the offset is the u32 at +0xc and +0x10 is the delta.  We always WRITE
//! the packed form, so reading +0x10 unconditionally took the delta (0) for a
//! cache image and stored it over the real offset: every thread-local in the
//! image collapsed onto offset 0 and they all aliased each other.  SwiftUI reads
//! a thread-local holding its current PropertyList element that way, got the
//! small integer living at block offset 0, and cast it unconditionally to a
//! class - "Could not cast value of type 'NSIndirectTaggedPointerString'".  A
//! key is small enough that a classic descriptor's u64 leaves +0xc zero, so a
//! non-zero +0xc means the packed form is already there.
//!
//! ---- the hand-built main thread ----
//! libpthread caches __thread_selfid() at TSD base - 8, and guest libpthread
//! only fills it in _pthread_set_self_internal, which the hand-built main thread
//! never runs - so pthread_threadid_np() returned 0 on it.  The pthread firstfit
//! mutex protocol stores that tid as the lock owner, and owner 0 looks UNLOCKED,
//! so any mutex taken by the main thread had no mutual exclusion at all against
//! other threads (CFRunLoopSource locks among them): lost psynch wakes,
//! corrupted signaled flags, and the explorer sync freeze.
//!
//! OCERZ_DLOPEN_PIGGYBACK loads a chosen dylib, guest initializers and all,
//! right after the first successful dlopen whose path, as the caller wrote it,
//! matches - a probe placed inside the real process, on the same thread, at the
//! same point in its life.  It fires for shared-cache images too, which is how
//! a CGL probe was walked through Wine's process start to show that the only
//! thing that mattered was the stack CGL first ran on.
//!
//! ---- the executable's own identity ----
//! The main-executable path is realpath()'d so the guest always sees an
//! ABSOLUTE exec path - it feeds executable_path=, the DynFrame exec_path and
//! the host path used for matching.  Real macOS always resolves argv[0] to
//! absolute, and without it a `./prog` launch gave a relative
//! _dyld_get_image_name(0) and _NSGetExecutablePath: Steam's tier1 built
//! "/../Steam.AppBundle/..." out of one and V_RemoveDotSlashes asserted.
//! Pinned by the dynamic test exec_abspath.
//!
//! A dlopen of the main executable's own path returns the already-loaded main
//! image rather than mapping a second copy, matched against that host path raw
//! or realpath'd.  Native dyld never loads a second copy of the running
//! executable; deduping only against the loaded-image list was not enough, so
//! Steam's bootstrapper dlopening its own steam_osx mapped a duplicate, which
//! gave duplicate GURLHelper and UpdateEventHandlers objc classes and crashed
//! steamui on a null vtable.  Pinned by the dynamic test dlopen_self.
//!
//! ---- native mode ----
//! OCERZ_MODE_NATIVE maps no shared cache at all: the guest's system libraries
//! are to become synthesized x86 images whose exports bridge into the host's own
//! arm64 frameworks, and until one of those exists there is nothing for a system
//! import to bind to.  The loader is not forked for it.  Every consumer still
//! receives the same static OcerzCache, simply left zeroed, because a zeroed
//! cache already answers the way this mode needs: mapped is 0, so each resolve
//! reports not-found and the has-image test says no, and images_cnt is 0, so the
//! dependency map and the initializer search walk nothing, ran_init stays clear
//! and control reaches the cache-free process start at the tail of
//! ocerz_dyld_run.  A null pointer would say exactly the same thing at the price
//! of a null check in every one of those callers and a second path to keep
//! honest, so the zeroed struct is the one that travels.
//!
//! Unresolved imports are collected rather than announced one at a time.  In
//! cache mode a miss is a real failure and prints where it happens; in native
//! mode a program commonly imports several things no virtual library exports
//! yet, and the report is only useful if it names all of them, so a miss is
//! recorded by (library, symbol) pair in a fixed table, deduplicated, and the set
//! is reported once the main image's fixups are done - after which the process
//! stops with 71 rather than running a program with imports bound to zero.  The
//! library is the two-level ordinal's target install name, or (flat) when the
//! import names no ordinal.
//!
//! An import whose ordinal names a virtual image is resolved in that image and
//! nowhere else.  The flat search over every loaded image that follows a
//! two-level miss is there for disk dylibs whose re-exports the resolver does not
//! follow, and a virtual image re-exports nothing, so falling back would only let
//! a CoreFoundation import CoreFoundation does not export bind silently to a
//! libSystem export of the same name.
//!
//! A system library that a dependency names is not on disk at all on a modern
//! macOS - /usr/lib/libSystem.B.dylib exists only inside the cache - so in
//! native mode failing to read one is the ordinary case rather than a fault,
//! and it is logged rather than announced as fatal.  The collected import
//! report is what names the consequence, symbol by symbol.
//!
//! Which libraries ocerz synthesizes, and what each exports, comes from the API
//! database (apidb.h), whose version directory is chosen by the minimum macOS
//! the main image declares.  So before any dependency is loaded, native mode
//! reads that version out of the main image - the minos of an LC_BUILD_VERSION
//! whose platform is macOS, or else the version of an LC_VERSION_MIN_MACOSX -
//! and hands it to ocerz_apidb_set_minos; an image declaring neither, or only
//! another platform's version, hands over zero, which chooses the newest
//! directory.
//!
//! At the same point, before any framework can be opened and run its
//! initializers, native mode hands the guest's arguments to
//! ocerz_bridge_set_process_args, which makes them the host's own argv, argc and
//! program name, and it hands over the vectors on the guest's stack once the
//! frame is built, so that the host's variables name the argv main is given.  The
//! process path CoreFoundation takes the main bundle from is the main image's real
//! path, the one ocerz_dyld_main_path answers, which is set before the first
//! dependency is looked at; bridge.c hands it to CoreFoundation when it first opens
//! a framework.
//!
//! A name that ocerz synthesizes is answered rather than missed.  Before an
//! install name is expanded at all, native mode asks ocerz_vdylib_have whether
//! it has an image for it, and if it does the image is built in memory and
//! registered as an ordinary DynImage whose slice points at that buffer instead
//! of at the bytes of a file - which is the only difference, since every disk
//! image is already mapped and read out of a host buffer, so the segment copy,
//! the trie walker, the import resolver, dlopen and dladdr all carry on
//! unchanged.  Asking before the expansion keeps @-resolution off a path that
//! was never going to exist, and asking before the dep_find veto - which
//! refuses anything the cache already owns - leaves that veto dead in native
//! mode by position rather than by an exception written into it.
//!
//! A virtual image then returns right there instead of falling through the rest
//! of the disk loader.  It names no dependencies, it is built needing no fixups
//! because its one data slot holds a constant and its stubs reach that slot
//! rip-relative, it carries no Objective-C, and the dyld-API image list is a
//! cache-mode structure: ocerz_dyldapi_register_image ends in closure_add, and
//! the closure is allocated by ocerz_dyldapi_setup, which native mode does not
//! run.  Both of its call sites are therefore confined to cache mode, so that a
//! registration that would do nothing is not made at all.  Mapping the segments
//! and handing __TEXT back its protection is the whole of the work.
//!
//! A cache image reached by dlopen has to join that same list, and this was the
//! one kind of image that never did.  dlopen of a disk dylib registered; dlopen
//! of a cache image returned the mach header and appended nothing, so the
//! library was loaded, its symbols resolved and dladdr knew where they lived,
//! while every walk of the image list said it was absent.  libobjc keys its
//! per-image queries off that list, and Metal loads its GPU driver through
//! them, so MTLCreateSystemDefaultDevice returned nil, MTLCopyAllDevices found
//! no device, OpenGL had no accelerated renderer left to offer and
//! CGLChoosePixelFormat failed for every accelerated attribute set.  Brawlhalla
//! put a window on screen and never drew into it.
//!
//! The registration happens at the end of cache_dlopen_hit, after the objc
//! mapping and the initializer phase, and the order is the whole of it.  libobjc
//! calls back into the dyld APIs while it maps an image, and an image already
//! standing in the list when that callback arrives is one it takes as handled:
//! registering first cost the newly loaded image its categories, which is
//! exactly what a late-loaded framework is usually dlopened for.
//!
//! An @rpath name is tried in each LC_RPATH directory in order, and the first
//! candidate ocerz can supply wins, as dyld's first existing file does: a file
//! on disk, an image of the x86 cache, or in native mode the guest runtime's
//! copy or an image an API database builds (rpath_supplied).  An app built for
//! systems before 10.14.4 bundles the Swift runtime and lists /usr/lib/swift
//! ahead of its own Frameworks, so every later system loads its own
//! libswiftCore, which exists only in the shared cache.  Counting files alone
//! loaded the bundled one, which stops with "This copy of libswiftCore.dylib
//! requires an OS version prior to 10.14.4" (iGlance).  In native
//! mode the image found that way is loaded by its absolute name and remembers
//! the @rpath name it was found for, which the binder looks images up by.
//!
//! Native mode runs no libSystem initializer, so the initializer phase that
//! cache mode gates on it never runs either, and for a while nothing ran a guest
//! image's own initializers at all: a C constructor or a C++ static object's
//! constructor was silently skipped.  Each guest image's __mod_init_func and
//! __init_offsets entries now run just before main, after every +load, the
//! dylibs in the reverse of the order they were loaded, so a library's
//! dependencies are initialized before it, and the main image last.  dyld
//! interleaves the two per image, +load then constructors, image by image; ocerz
//! runs all +load methods first, a difference only a constructor that sends a
//! message to a class in a later image could see.  Virtual images carry no
//! initializers, so walking them costs a header scan.
//!
//! A guest's main returns into a few bytes ocerz writes at the top of its stack.
//! In cache mode they make the exit syscall with main's result, since dyld's own
//! start has already been bypassed.  In native mode they call the virtual
//! libSystem's _exit export instead, the C library's exit, because that is what
//! start does on a real system and it is what runs the guest's atexit handlers
//! and flushes its stdio; the exit syscall stays behind the call only as the path
//! taken if that export is missing.
//!
//! No x86 Objective-C runtime runs in native mode, so nothing canonicalizes a
//! guest image's selectors, or reads its classes, the way the translated runtime
//! does in cache mode.  canonicalize_objc_selrefs hands each disk dylib, after
//! its fixups, to ocerz_objcbridge_fix_selrefs instead, which makes every
//! selector reference the host runtime's own SEL, and then to
//! ocerz_objcbridge_define_image, which makes the image's protocols, classes and
//! categories the host runtime's own and queues its +load methods; a dylib's
//! dependencies are loaded, and so defined, before it.  The main image gets the
//! same two passes right after the unresolved-import report, before any guest
//! code runs.  When the guest links libobjc, ocerz's uncaught-exception handler
//! is installed at that same point, once every host framework the guest links
//! has been opened and has installed its own handler for ocerz's to chain to
//! (objcbridge.h).  The queued +load methods run through
//! ocerz_objcbridge_run_loads once the guest's thread block is in place and the
//! handlers are installed, just before main, which is the first point guest code
//! can run at all.  A dlopen defines its images the same way, but only once the
//! whole closure has bound, and runs each image's own +load methods just before
//! that image's initializers.
//!
//! ---- thread-local variables in native mode ----
//! Descriptors are rewritten into the same packed form as in cache mode, but the
//! thunk word is left exactly as the fixups bound it.  In cache mode the import
//! binds to __tlv_bootstrap, which is not the code that answers, so the rewrite
//! points the word at tlv_get_addr beside it; in native mode the same import binds
//! to the synthesized libSystem's __tlv_bootstrap stub, which is itself the entry
//! that answers, so the binding already says the right thing and resolving the stub
//! a second time could only disagree with it.  The fixups have landed by the time
//! registration looks: each disk dylib is bound inside its own load, and the main
//! image before the unresolved-import report.
//!
//! The key is ocerz's own, a small integer per image counted from 1, not a pthread
//! key.  Cache mode's key is read by the guest's tlv_get_addr through the guest's
//! pthread_getspecific; here nothing reads it but ocerz_tlv_address, and there is no
//! x86 libpthread to create one in.  A dense key makes each thread's table a flat
//! array indexed by it, and leaves 0 to mean a descriptor registration never
//! rewrote - a classic descriptor's key word is 0 on disk - so such a descriptor is
//! refused instead of resolved into some other image's block.  The table has an
//! entry for every image the loader can hold, DYN_DIMG_MAX dylibs and the main
//! image, so no key can outgrow it and it never has to be reallocated under a
//! thread that is reading it.
//!
//! The table lives in guest memory, found through the slot OCERZ_TLV_TABLE_SLOT
//! past gs in the thread block ocerz built, and not in host thread-local storage.
//! An attached thread is torn down by a pthread key destructor, and by then the
//! host's own __thread storage has already been released and reads back as zero
//! (see the attach section of vm.c): a table reached through a host thread variable
//! would be invisible at the one moment it has to be freed, and every block behind
//! it would leak.  The slot also travels with the cpu.  A guest call runs a copy of
//! the thread's cpu and the copy carries the same gs, so a variable touched inside a
//! native callback is the same variable as outside it.  Nothing else in ocerz
//! writes that slot, and in native mode no x86 libpthread is there to use it.
//!
//! A thread's block for an image is made the first time that thread touches one of
//! the image's variables - copied from the template when the image has initialized
//! thread data, left zeroed when it has only __thread_bss - and from then on an
//! access is the descriptor, the table pointer and one entry.  An attached thread's
//! blocks and its table are freed when its personality is torn down, at thread exit
//! or detach, before its region is unmapped.  Each block is unmapped by the length
//! recorded when its key was given out, never by a length read back out of guest
//! memory, and a descriptor whose size disagrees with its key's is refused for the
//! same reason.  The main thread's blocks live as long as the process, and since
//! dlclose unloads nothing, a key always names the image it was given to.
//!
//! Registration runs for the main image and everything loaded with it right after
//! the unresolved-import report, before any guest code can run, and as soon as a
//! dlopen has bound, before anything in the new images runs.  The dlopen pass
//! walks every image the loader holds rather than only the new ones, which costs
//! a scan per image and spares it knowing which those are.  An image already
//! registered is skipped, and that is not a formality - once a descriptor for a
//! variable at offset 0 has been packed it no longer reads as packed, so packing
//! it again would store the template delta as its offset.  A dlopen that fails
//! registers nothing, because it unmaps everything it mapped before this pass.
//!
//! ---- loading code at run time in native mode ----
//! Native mode's dlopen family is answered here, reached from special exports in
//! src/bridge.c, and it is not cache mode's ocerz_dlopen: that one resolves
//! against the x86 shared cache and leaves Objective-C, thread-local variables
//! and the image list to the translated libobjc and libdyld, which native mode
//! does not have.
//!
//! A handle is the mach header of the image, which is also what dladdr and the
//! image list answer with, so a header obtained either way can be handed to
//! dlsym.  Its low bit, never set in a page-aligned header, marks a handle that
//! RTLD_FIRST asked for.  dlopen(NULL) answers RTLD_DEFAULT, or RTLD_MAIN_ONLY
//! under RTLD_FIRST, because that is what dyld answers: an arm64 program on the
//! host printed 0xfffffffffffffffe and 0xfffffffffffffffb for them.
//!
//! A path is resolved the way dyld resolves one: @executable_path against the
//! main executable, @loader_path against the image holding the caller's return
//! address, @rpath through that image's LC_RPATHs and then the main
//! executable's, and a bare name through DYLD_LIBRARY_PATH, the working
//! directory and the fallback path.  Each candidate is asked, in order, whether
//! it is the main executable, an image already loaded by path, install name or
//! file identity, an install name a database describes, the same once its
//! symlinks are resolved, a file on disk, or a library the host's shared cache
//! holds, whose real path the host's _dyld_shared_cache_real_path answers
//! without loading anything.  That last is how /usr/lib/libc.dylib, libz.dylib or
//! a framework's top-level symlink turn into the install name a database is
//! filed under.  A file with no x86_64 slice and a host library no database
//! describes are both refused as a native library without an API database, since
//! native mode runs x86 code only and reaches native code only through one.
//!
//! A load binds its whole closure before anything in it runs.  The loader's
//! usual path defines an image's Objective-C as soon as that image's fixups are
//! bound, which is harmless at startup, where a failed bind ends the process; a
//! dlopen that fails must leave nothing behind, and a class handed to the native
//! runtime cannot be taken back, so while a dlopen loads, those passes wait.
//! Misses are counted against the collected-import table and a non-weak
//! dependency that did not load is recorded, and either fails the dlopen with
//! dyld's wording, Symbol not found or Library not loaded, naming the image that
//! referenced it.  Every image the attempt mapped is then unmapped and dropped
//! and the table put back as it was, so a second attempt fails the same way.
//! Only a closure that bound completely is published into the image list - one
//! release store of the count, which readers load with acquire, so no reader
//! sees an image half built and none takes a lock - and then, in dyld's order,
//! its thread-local descriptors are registered, every add-image callback is
//! called for each new image, every new image's selectors are rewritten and its
//! classes, categories and protocols defined, dependencies first by the order
//! their loads completed, every objc_addLoadImageFunc function is called for
//! each new image, and then image by image, again dependencies first,
//! that image's +load methods and its initializers run on the calling thread
//! below the caller's stack pointer.  RTLD_LOCAL marks the image it loaded, not
//! its dependencies, which hides it from RTLD_DEFAULT, RTLD_NEXT and flat lookups
//! until a dlopen without it; RTLD_NOLOAD answers only what is already loaded.
//! Nothing is ever unloaded, so dlclose checks its handle and answers 0, and a
//! mach header never comes to name a second image.
//!
//! dlsym takes the C name and searches for it with a leading underscore, as
//! dyld does.  RTLD_DEFAULT searches the main executable and then every global
//! image in load order, RTLD_MAIN_ONLY the main executable alone, RTLD_NEXT the
//! images after the one holding the caller's return address and RTLD_SELF that
//! image first; a handle searches its image and then its dependencies breadth
//! first, or its image alone under RTLD_FIRST.  A plug-in bundle's imports from
//! the executable that loaded it carry the main-executable ordinal, and the
//! resolver answers those from the main image once every other place has
//! missed, as it does for a flat or weak lookup nothing else answered; before
//! that, no other image's import could bind to the main image at all.
//!
//! dladdr names a guest image's symbol from its symbol table, read out of the
//! mapped __LINKEDIT so that the name is guest memory, and an image with none -
//! every synthesized one - from its export trie, walked once into a sorted
//! table.  An address in no guest image is native memory, a data export's target
//! or a host framework's code, and the host's own dladdr answers it, with
//! strings and a base that are host addresses the identity map makes guest ones.
//!
//! dlerror is per thread, as POSIX and dyld make it.  Each thread keeps a buffer
//! in guest memory behind a pthread key whose destructor unmaps it; a message
//! stays readable until that thread's next failure, and dlerror answers it once.
//!
//! The image list is the main executable and then every image the loader holds,
//! synthesized libraries included, in load order.  x86 code walks it expecting
//! the libraries it links to be in it - a crash reporter naming libSystem, a
//! framework looking for its own header by name - and those libraries are the
//! synthesized images, so a list of guest dylibs alone would hide what the guest
//! believes it has linked, while listing the host's images would hand x86 code
//! arm64 headers.  An add-image callback registered late is called at once for
//! every image already listed, and then for each image a dlopen adds.  A
//! function the guest gives objc_addLoadImageFunc, which the Swift runtime uses
//! to find each image's Swift sections, is kept the same way and called with
//! each image's header: for those already listed when it is registered, and
//! then for each new image after its classes are defined and before its +load
//! methods run, which is where libobjc calls one.  It is never registered with
//! the native runtime, whose images are the host's.
//!
//! Chained fixups name an import by ordinal at every location that uses it, so
//! apply_fixups resolves each ordinal once and reuses the answer, and the
//! classic bind opcodes, which repeat a symbol across consecutive locations,
//! reuse the last answer when the symbol, library and weakness are unchanged.
//! A weak-coalescing bind is answered only from cache images that define weak
//! symbols, as dyld does; see src/cache.c.  Of those, only images already loaded
//! in the guest count, again as dyld does: it coalesces among the images loaded
//! so far, in load order, so a weak definition in a cache image nobody has
//! loaded never wins.  ocerz searched every cache image, and a library that
//! carries its own copy of a C++ template bound it to Apple's copy instead.
//! DXMT's winemetal.so statically links its own LLVM, and its 4481 weak binds
//! include llvm::AnalysisManager<Module>::getResultImpl, which Apple's libLLVM in
//! GPUCompiler.framework also exports; DXMT's shader compiler then ran Apple's
//! LLVM on its own LLVM's objects, read a garbage pointer, failed every shader,
//! and Metal aborted on the nil function it was handed (R.E.P.O., 2026-10-03).
//! The main program and its startup dependencies are bound before the set of
//! loaded images exists, so until then every cache image still counts, which
//! is what it was; OCERZ_WEAK_ALL_CACHE=1 keeps that for every bind.
//!
//! Some x86_64 libraries ship only in the Rosetta cryptex, and Intel code names
//! them by their system path: Metal opens /usr/lib/libMTLHud.dylib for its
//! performance HUD, and nothing exists there on disk.  In cache mode a dlopen
//! of an absolute path found neither in the cache nor on disk retries the same
//! path under /System/Volumes/Preboot/Cryptexes/Rosetta.
//!
//! A dylib built for macOS 10.5 or earlier has no compressed fixup information:
//! its pointers are fixed through local relocations, which are rebased by the
//! slide, external relocations, which add a symbol's address to the value in
//! place, and the indirect symbol table behind its lazy and non-lazy symbol
//! pointer sections.  On x86_64 a relocation's address counts from the first
//! writable segment.  ocerz applied none of this, so such a library kept every
//! pointer unslid and unbound, and Steam's steamloader.dylib, which Steam
//! injects into every game it launches, jumped to zero in its first call.
//!
//! DYLD_INSERT_LIBRARIES is honoured in cache mode: once the startup
//! initializers have run, each library it names is opened as the guest's dlopen
//! would, before the program's entry point.  src/main.c keeps the host's dyld
//! from acting on the same variable.

use crate::ffi::{self, OcerzCPU, OcerzCache, OcerzTrieVisit, OcerzVM};
use crate::ported::dyldapi::macho::{
    LC_BUILD_VERSION, LC_ID_DYLIB, LC_SEGMENT_64, LC_SYMTAB, LC_VERSION_MIN_MACOSX, LoadCommand,
    MH_MAGIC_64, MachHeader64, N_SECT, N_TYPE, SegmentCommand64,
};
use core::ffi::{c_char, c_int, c_uint, c_ulong, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32};
use libc::c_ulonglong;

const MODE_CACHE: c_int = ffi::OCERZ_MODE_CACHE as c_int;
const MODE_NATIVE: c_int = ffi::OCERZ_MODE_NATIVE as c_int;
const MH_MAGIC: u32 = 0xfeed_face;

#[inline(always)]
pub(super) fn cstr_ptr(s: &'static core::ffi::CStr) -> *const c_char {
    s.as_ptr()
}

mod bind;
mod dlopen;
mod eager;
mod exports;
mod frame;
mod init;
mod load;
mod map;
mod native;
mod run;
mod tlv;

const DYN_ARENA_SIZE: u64 = 256u64 << 30;
const DYN_STACK_SIZE: u64 = 8u64 << 20;
const MISS_LIST_MAX: c_int = 24;
const BIND_ORDINAL_FLAT_LOOKUP: c_int = -2;
const DYN_SEG_MAX: usize = 16;
const DYN_DIMG_MAX: usize = 256;
const NATIVE_MISS_MAX: usize = 256;
const SEG_FLAG_READ_ONLY: u32 = 0x10;
const DEPMAP_BITS: usize = 13;
const SEG_MAX: usize = 32768;
const EAGER_MAX: usize = 4096;
const EAGER_SET: usize = EAGER_MAX * 2;
const TLV_REG_MAX: usize = 4096;
const NATIVE_TLV_KEYS: usize = DYN_DIMG_MAX + 1;
const NATIVE_TLV_TABLE_BYTES: u64 = (NATIVE_TLV_KEYS as u64 + 1) * 8;
const INIT_VISITED_MAX: usize = 8192;
const INIT_CLOSURE_CAP: usize = 4096;
const RPATH_MAX: usize = 64;
const NDL_RTLD_LOCAL: c_int = 0x4;
const NDL_RTLD_NOLOAD: c_int = 0x10;
const NDL_RTLD_FIRST: c_int = 0x100;
const NDL_NEXT: u64 = u64::MAX;
const NDL_DEFAULT: u64 = u64::MAX - 1;
const NDL_SELF: u64 = u64::MAX - 2;
const NDL_MAIN_ONLY: u64 = u64::MAX - 4;
const NDL_ERR_BYTES: usize = 2048;
const NDL_TRIED_BYTES: usize = 1536;
const ROSETTA_CRYPTEX: &[u8] = b"/System/Volumes/Preboot/Cryptexes/Rosetta\0";
const CPU_TYPE_X86_64: u32 = 0x0100_0007;
const FAT_MAGIC: u32 = 0xcafe_babe;
const FAT_CIGAM: u32 = 0xbebafeca;
const FAT_MAGIC_64: u32 = 0xcafe_babf;
const FAT_CIGAM_64: u32 = 0xbfbafeca;
const LC_UNIXTHREAD: u32 = 0x5;
const LC_LOAD_DYLINKER: u32 = 0xe;
const LC_DYLD_INFO: u32 = 0x22;
const LC_DYLD_INFO_ONLY: u32 = 0x8000_0022;
const LC_MAIN: u32 = 0x8000_0028;
const LC_DYLD_CHAINED_FIXUPS: u32 = 0x8000_0034;
const LC_DYLD_EXPORTS_TRIE: u32 = 0x8000_0033;
const MH_PIE: u32 = 0x0020_0000;
const VM_PROT_EXECUTE: u32 = 0x4;

#[repr(C)]
#[derive(Clone, Copy)]
struct SymIndexEntry {
    stroff: u32,
    value: u64,
}

#[repr(C)]
struct SymIndex {
    cap: u32,
    n: u32,
    strtab: *const c_char,
    ent: *mut SymIndexEntry,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DynImage {
    slice: *const u8,
    owned_buf: *mut u8,
    path: [c_char; 1024],
    install_name: [c_char; 1024],
    id_name: [c_char; 1024],
    rpath_name: [c_char; 256],
    slide: u64,
    load_base: u64,
    main_entry: u64,
    thread_entry: u64,
    cf_off: u32,
    cf_size: u32,
    rebase_off: u32,
    rebase_size: u32,
    bind_off: u32,
    bind_size: u32,
    weak_bind_off: u32,
    weak_bind_size: u32,
    lazy_bind_off: u32,
    lazy_bind_size: u32,
    has_dyld_info: c_int,
    seg_vmaddr: [u64; DYN_SEG_MAX],
    seg_count: c_int,
    is_pie: c_int,
    links_dylib: c_int,
    links_cf: c_int,
    is_virtual: c_int,
    local: c_int,
    seq: u32,
    map_base: u64,
    map_size: u64,
    file_dev: u64,
    file_ino: u64,
    symidx: *mut SymIndex,
}

impl DynImage {
    const ZERO: Self = Self {
        slice: ptr::null(),
        owned_buf: ptr::null_mut(),
        path: [0; 1024],
        install_name: [0; 1024],
        id_name: [0; 1024],
        rpath_name: [0; 256],
        slide: 0,
        load_base: 0,
        main_entry: 0,
        thread_entry: 0,
        cf_off: 0,
        cf_size: 0,
        rebase_off: 0,
        rebase_size: 0,
        bind_off: 0,
        bind_size: 0,
        weak_bind_off: 0,
        weak_bind_size: 0,
        lazy_bind_off: 0,
        lazy_bind_size: 0,
        has_dyld_info: 0,
        seg_vmaddr: [0; DYN_SEG_MAX],
        seg_count: 0,
        is_pie: 0,
        links_dylib: 0,
        links_cf: 0,
        is_virtual: 0,
        local: 0,
        seq: 0,
        map_base: 0,
        map_size: 0,
        file_dev: 0,
        file_ino: 0,
        symidx: ptr::null_mut(),
    };
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DynFrame {
    argc: u64,
    argv_arr: u64,
    envp_arr: u64,
    apple_arr: u64,
    progvars: u64,
    stack_top: u64,
    exit_stub: u64,
    exec_path: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RpathList {
    entry: [[c_char; 1024]; RPATH_MAX],
    n: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NativeMiss {
    lib: [c_char; 256],
    sym: [c_char; 256],
    from: [c_char; 256],
}

#[unsafe(no_mangle)]
pub static mut ocerz_main_mh: u64 = 0;

static mut g_dynlookup_miss: c_ulong = 0;
static mut g_dimgs: [DynImage; DYN_DIMG_MAX] = [DynImage::ZERO; DYN_DIMG_MAX];
static mut g_dimgs_n: c_int = 0;
static mut g_main_dimg: DynImage = DynImage::ZERO;
static mut g_main_dimg_valid: c_int = 0;
static mut g_main_hostpath: [c_char; 1024] = [0; 1024];
static mut g_main_dev: u64 = 0;
static mut g_main_ino: u64 = 0;
static mut g_run_cache: *mut OcerzCache = ptr::null_mut();
static mut g_run_vm: *mut OcerzVM = ptr::null_mut();
static mut g_run_init_args: [u64; 5] = [0; 5];
static mut g_run_init_ready: c_int = 0;
static mut g_dlerror_g: u64 = 0;
static mut g_native_miss: [NativeMiss; NATIVE_MISS_MAX] = [NativeMiss {
    lib: [0; 256],
    sym: [0; 256],
    from: [0; 256],
}; NATIVE_MISS_MAX];
static mut g_native_miss_n: c_int = 0;
static mut g_native_miss_dropped: c_int = 0;
static g_dimgs_pub: AtomicI32 = AtomicI32::new(0);
static g_native_tlv_keys: AtomicU32 = AtomicU32::new(0);
static g_ndl_syms: [AtomicPtr<native::NdlSyms>; DYN_DIMG_MAX] =
    [const { AtomicPtr::new(ptr::null_mut()) }; DYN_DIMG_MAX];

#[inline(always)]
unsafe fn rd16(p: *const u8) -> u16 {
    ptr::read_unaligned(p.cast::<u16>())
}

#[inline(always)]
unsafe fn rd32(p: *const u8) -> u32 {
    ptr::read_unaligned(p.cast::<u32>())
}

#[inline(always)]
unsafe fn rd64(p: *const u8) -> u64 {
    ptr::read_unaligned(p.cast::<u64>())
}

#[inline(always)]
unsafe fn wr64(p: *mut u8, value: u64) {
    ptr::write_unaligned(p.cast::<u64>(), value)
}

pub(super) unsafe fn read_file(path: *const c_char, len_out: *mut usize) -> *mut u8 {
    let fd = libc::open(path, libc::O_RDONLY);
    if fd < 0 {
        return ptr::null_mut();
    }
    let size = libc::lseek(fd, 0, libc::SEEK_END);
    if size <= 0 {
        libc::close(fd);
        return ptr::null_mut();
    }
    let buf = libc::malloc(size as usize).cast::<u8>();
    if buf.is_null() {
        libc::close(fd);
        return ptr::null_mut();
    }
    if libc::pread(fd, buf.cast(), size as usize, 0) != size as libc::ssize_t {
        libc::free(buf.cast());
        libc::close(fd);
        return ptr::null_mut();
    }
    libc::close(fd);
    *len_out = size as usize;
    buf
}

#[inline(always)]
unsafe fn dimg_at(index: usize) -> *mut DynImage {
    ptr::addr_of_mut!(g_dimgs).cast::<DynImage>().add(index)
}

unsafe fn select_slice(buf: *const u8, len: usize) -> *const u8 {
    let magic = rd32(buf);
    if magic == MH_MAGIC_64 {
        return if len >= 8 && rd32(buf.add(4)) == CPU_TYPE_X86_64 {
            buf
        } else {
            ptr::null()
        };
    }
    if magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64 {
        let swap = magic == FAT_CIGAM || magic == FAT_CIGAM_64;
        let is64 = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
        let mut nfat = rd32(buf.add(4));
        if swap {
            nfat = nfat.swap_bytes();
        }
        let fa = buf.add(8);
        let stride = if is64 { 32 } else { 20 };
        for i in 0..nfat {
            let entry = fa.add(i as usize * stride);
            let mut cputype = rd32(entry);
            let mut offset = if is64 {
                rd64(entry.add(8))
            } else {
                rd32(entry.add(8)) as u64
            };
            if swap {
                cputype = cputype.swap_bytes();
                offset = if is64 {
                    offset.swap_bytes()
                } else {
                    (offset as u32).swap_bytes() as u64
                };
            }
            if cputype == CPU_TYPE_X86_64 && offset < len as u64 {
                return buf.add(offset as usize);
            }
        }
    }
    ptr::null()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_peek_dynamic(path: *const c_char) -> c_int {
    let mut flen = 0usize;
    let buf = read_file(path, &mut flen);
    if buf.is_null() {
        return -1;
    }
    let slice = select_slice(buf, flen);
    if slice.is_null() {
        libc::free(buf.cast());
        return -2;
    }
    let ncmds = rd32(slice.add(16));
    let mut lc = slice.add(core::mem::size_of::<MachHeader64>());
    let mut dynamic = -1;
    let mut has_thread = 0;
    let mut has_dylinker = 0;
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == 0x8000_0028 {
            dynamic = 1;
            break;
        }
        if cmd == LC_UNIXTHREAD {
            has_thread = 1;
        }
        if cmd == LC_LOAD_DYLINKER {
            has_dylinker = 1;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if dynamic < 0 && has_thread != 0 {
        dynamic = has_dylinker;
    }
    libc::free(buf.cast());
    dynamic
}

unsafe fn dimg_find_by_path(path: *const c_char) -> *mut DynImage {
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if (*d).path[0] != 0 && libc::strcmp(ptr::addr_of!((*d).path).cast(), path) == 0 {
            return d;
        }
    }
    ptr::null_mut()
}

unsafe fn dimg_find_by_install_name(iname: *const c_char) -> *mut DynImage {
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if ((*d).install_name[0] != 0
            && libc::strcmp(ptr::addr_of!((*d).install_name).cast(), iname) == 0)
            || ((*d).id_name[0] != 0
                && libc::strcmp(ptr::addr_of!((*d).id_name).cast(), iname) == 0)
            || ((*d).rpath_name[0] != 0
                && libc::strcmp(ptr::addr_of!((*d).rpath_name).cast(), iname) == 0)
        {
            return d;
        }
    }
    ptr::null_mut()
}

unsafe fn dimg_record_id(d: *mut DynImage) {
    let mh = (*d).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let csize = rd32(lc.add(4));
        let noff = rd32(lc.add(8));
        if rd32(lc) == LC_ID_DYLIB && noff < csize {
            libc::snprintf(
                ptr::addr_of_mut!((*d).id_name).cast(),
                core::mem::size_of_val(&(*d).id_name),
                cstr_ptr(c"%.*s"),
                (csize - noff) as c_int,
                lc.add(noff as usize).cast::<c_char>(),
            );
            return;
        }
        lc = lc.add(csize as usize);
    }
}

unsafe fn file_identity(path: *const c_char, dev: *mut u64, ino: *mut u64) -> c_int {
    let fd = libc::open(path, libc::O_RDONLY);
    if fd < 0 {
        return 0;
    }
    let mut st: libc::stat = core::mem::zeroed();
    let ok = libc::fstat(fd, &mut st) == 0 && st.st_ino != 0;
    libc::close(fd);
    if !ok {
        return 0;
    }
    *dev = st.st_dev as u64;
    *ino = st.st_ino as u64;
    1
}

unsafe fn dimg_find_by_identity(dev: u64, ino: u64) -> *mut DynImage {
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if (*d).file_ino == ino && (*d).file_dev == dev {
            return d;
        }
    }
    ptr::null_mut()
}

unsafe fn dimg_segments_overlap(d: *const DynImage, lo: u64, hi: u64, exec_only: c_int) -> c_int {
    if (*d).slice.is_null() || (*d).is_virtual != 0 {
        return 0;
    }
    let mh = (*d).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64
            && rd64(lc.add(32)) != 0
            && rd32(lc.add(60)) != 0
            && (exec_only == 0 || rd32(lc.add(60)) & VM_PROT_EXECUTE != 0)
        {
            let slo = rd64(lc.add(24)).wrapping_add((*d).slide);
            let shi = slo.wrapping_add(rd64(lc.add(32)));
            if lo < shi && hi > slo {
                return 1;
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    0
}

unsafe fn dimg_containing(addr: u64) -> *mut DynImage {
    if addr == 0 {
        return ptr::null_mut();
    }
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if dimg_segments_overlap(d, addr, addr.wrapping_add(1), 0) != 0 {
            return d;
        }
    }
    let main = ptr::addr_of_mut!(g_main_dimg);
    if g_main_dimg_valid != 0 && dimg_segments_overlap(main, addr, addr.wrapping_add(1), 0) != 0 {
        return main;
    }
    ptr::null_mut()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_dump_images() {
    libc::fprintf(
        crate::log::stderr(),
        cstr_ptr(c"ocerz: IMGMAP[%d] n=%d arena_lo=%#llx\n"),
        libc::getpid(),
        g_dimgs_n,
        ffi::ocerz_arena_lo as libc::c_ulonglong,
    );
    let n = g_dimgs_n;
    for i in 0..n {
        let im = dimg_at(i as usize);
        let mut hi = (*im).load_base;
        for s in 0..(*im).seg_count {
            let v = (*im).seg_vmaddr[s as usize].wrapping_add((*im).slide);
            if v > hi {
                hi = v;
            }
        }
        let name = if (*im).install_name[0] != 0 {
            ptr::addr_of!((*im).install_name).cast::<c_char>()
        } else {
            ptr::addr_of!((*im).path).cast::<c_char>()
        };
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz:   img[%d] base=%#llx slide=%#llx segs=%d hi~=%#llx %s\n"),
            i,
            (*im).load_base as libc::c_ulonglong,
            (*im).slide as libc::c_ulonglong,
            (*im).seg_count,
            hi as libc::c_ulonglong,
            name,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_name_for_addr(addr: u64, base_out: *mut u64) -> *const c_char {
    let mut best = ptr::null_mut::<DynImage>();
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if (*d).load_base <= addr && (best.is_null() || (*d).load_base > (*best).load_base) {
            best = d;
        }
    }
    let mut cbase = 0u64;
    let cname = ffi::ocerz_cache_name_for_addr(addr, &mut cbase);
    if !cname.is_null() && (best.is_null() || cbase > (*best).load_base) {
        if !base_out.is_null() {
            *base_out = cbase;
        }
        return cname;
    }
    if best.is_null() {
        return ptr::null();
    }
    if !base_out.is_null() {
        *base_out = (*best).load_base;
    }
    if (*best).install_name[0] != 0 {
        ptr::addr_of!((*best).install_name).cast()
    } else {
        ptr::addr_of!((*best).path).cast()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_image_overlapping(
    lo: u64,
    hi: u64,
    exec_only: c_int,
    base_out: *mut u64,
) -> *const c_char {
    let n = g_dimgs_n;
    for i in 0..n {
        let d = dimg_at(i as usize);
        if dimg_segments_overlap(d, lo, hi, exec_only) != 0 {
            if !base_out.is_null() {
                *base_out = (*d).load_base;
            }
            if (*d).install_name[0] != 0 {
                return ptr::addr_of!((*d).install_name).cast();
            }
            return ptr::addr_of!((*d).path).cast();
        }
    }
    ptr::null()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_main_path() -> *const c_char {
    let path = ptr::addr_of!(g_main_hostpath).cast::<c_char>();
    if path.read() != 0 { path } else { ptr::null() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_resolve_guest_sym(name: *const c_char) -> u64 {
    if g_run_cache.is_null() || name.is_null() {
        return 0;
    }
    ffi::ocerz_cache_resolve(g_run_cache, name)
}
