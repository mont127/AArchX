/*
 * The call into host code, checked as far as it can be checked without a VM.
 *
 * Two halves.  The first is the database: every export the bridge claims must
 * also be an export of the virtual library, or the guest can never reach it,
 * and every export the bridge deliberately does not claim must still be an
 * export of the virtual library, or it would take the 71 path (nothing bound)
 * instead of the 72 path (bound, ran, no bridge behind it) and the two failure
 * modes would stop being distinguishable.  So each name is put through BOTH
 * readers - ocerz_bridge_lookup and the export trie of the image
 * ocerz_vdylib_image builds - rather than through a list written for the test.
 * The unbridged names here are not an arbitrary selection: they are the
 * variadic exclusion bridge.h states (printf, open, fcntl, ioctl), so a bridge
 * that grows one of them has changed a documented rule rather than broken a
 * test.  The list once also held atof and strtod, until signatures could name a
 * double, and qsort and bsearch, until a callback argument had a trampoline
 * back into guest code; qsort and bsearch are bridged names now, and their
 * lookups succeeding is also what proves the callback notation parses.
 *
 * The bridged names are the whole of what native mode bridged before its
 * descriptors were made from the API database, libSystem's fn and special
 * records and every CoreFoundation function, and each one has to still have a
 * descriptor, so the database cannot quietly lose a crossing.  CoreFoundation's
 * list includes the functions whose struct records bind a shape, whose
 * descriptors exist only if every struct record was placed in a register and
 * every shape name found.  A stub record, a var record and a data record have
 * no descriptor, and neither has an export of one library asked for in the
 * other.  Eight threads that look up the same export for the first time at
 * once all receive the same pointer, and the OCERZ_BRIDGESTAT report, captured
 * at the end, has to count the crossings the second half made and name the
 * export it made most.
 *
 * The second half drives ocerz_bridge_invoke against a hand-built CPU.  The
 * memory map is the identity one, ocerz_mem_init_identity, because that is the
 * map native mode runs in and the one the bridge's pointer conversions are
 * written against; under it a host pointer and a guest pointer are the same
 * number, so a function that returns a pointer can be checked for the right
 * VALUE rather than merely a non-zero one.  What the non-identity map would
 * additionally pin - that the argument conversion really is applied, and that
 * a null return stays null instead of becoming the arena base - cannot be
 * pinned here as well, because a process gets one map and malloc's return is
 * only expressible as a guest address in this one.
 *
 * The dynamic-loading exports are special records, and each one has to have a
 * descriptor and be exported, since a guest that loads code at run time stops
 * on the first that does not.  Driven the same way, with no image loaded, which
 * is where a unit test stands: dlerror answers null until something fails and
 * then the message once, a dlsym that misses names the handle as dyld prints it
 * and the symbol, dlopen(NULL) answers RTLD_DEFAULT and under RTLD_FIRST
 * RTLD_MAIN_ONLY, a path nothing provides fails with the paths it tried, a
 * library the host's shared cache has but no database describes fails as a
 * native library without one, dlclose answers 0 for RTLD_DEFAULT and -1 with a
 * message for a handle no image has, the image list is empty, no cache image is
 * overridden, _dyld_shared_cache_contains_path answers for exactly the libraries
 * a database describes, reached by install name or through the framework's
 * symlink, and dladdr on a host function answers what the host's dladdr does.
 * Those run after the report, so they do not move its counts.
 *
 * The register contract asserted is the one the stub implies.  A guest calls
 * an export with a CALL, so the return address is already on the stack when
 * the stub's jump reaches the trap window; the bridge therefore has to consume
 * it exactly as a RET would, rip from [rsp] and rsp forward by eight, with the
 * result in rax.  rax is poisoned before every call so a bridge that returns
 * without writing it fails rather than inheriting whatever was there.
 */
#include "ocerz/bridge.h"
#include "ocerz/vdylib.h"
#include "ocerz/dyld.h"
#include "ocerz/dyldapi.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cpu.h"
#include "ocerz/interp.h"

#include <sys/mman.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOAD_BASE  0x0000000210000000ull
#define RET_ADDR   0x0000000044332200ull
#define RAX_POISON 0xfeedfacecafebeefull
#define ARENA      (4ull << 30)
#define SCRATCH    0x10000ull

static int checks;
static int failures;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static const char *const kLib = "/usr/lib/libSystem.B.dylib";

static const char *const kBridged[] = {
    "___bzero", "___error", "___stack_chk_fail", "__tlv_bootstrap", "___memcpy_chk",
    "___memmove_chk", "___memset_chk", "___strcpy_chk", "___stpcpy_chk", "___strcat_chk",
    "___strncpy_chk", "___stpncpy_chk", "___strncat_chk", "___strlcpy_chk",
    "___strlcat_chk", "_memcpy", "_memcmp", "_memmove", "_memset", "_memchr", "_strcmp",
    "_strncmp", "_strcpy", "_strncpy", "_strlen", "_strnlen", "_strcat", "_strchr",
    "_strrchr", "_strstr", "_strdup", "_strtod", "_puts", "_putchar", "_malloc", "_calloc",
    "_realloc", "_free", "_exit", "_abort", "_getenv", "_qsort", "_bsearch", "_abs",
    "_labs", "_atoi", "_atol", "_atof", "_write", "_read", "_close", "_isatty", "_getpid",
    "_time", "_clock", "_pthread_create", "_pthread_join", "_pthread_detach",
    "_pthread_self", "_pthread_mutex_init", "_pthread_mutex_lock", "_pthread_mutex_unlock",
    "_pthread_mutex_destroy", "_pthread_cond_init", "_pthread_cond_wait",
    "_pthread_cond_signal", "_pthread_cond_broadcast", "_pthread_cond_destroy",
    "_dispatch_get_global_queue", "_dispatch_async_f", "_dispatch_sync_f",
    "_dispatch_apply_f", "_dispatch_semaphore_create", "_dispatch_semaphore_wait",
    "_dispatch_semaphore_signal", "_dispatch_release", "_signal", "_sigaction", "_raise",
    "_kill", "_sigprocmask", "_pthread_sigmask", "_sigaltstack", "_pthread_kill",
    "_sigemptyset", "_sigfillset", "_sigaddset", "_sigdelset", "_sigismember",
};
#define NBRIDGED (sizeof kBridged / sizeof kBridged[0])

static const char *const kUnbridged[] = {
    "_scanf", "_sscanf", "_syslog",
    "_open", "_fcntl", "_ioctl",
};
#define NUNBRIDGED (sizeof kUnbridged / sizeof kUnbridged[0])

static const char *const kCF = OCERZ_BRIDGE_COREFOUNDATION;

static const char *const kCFBridged[] = {
    "_CFRetain", "_CFRelease", "_CFGetRetainCount", "_CFEqual", "_CFHash", "_CFGetTypeID",
    "_CFStringGetTypeID", "_CFArrayGetTypeID", "_CFDictionaryGetTypeID",
    "_CFNumberGetTypeID", "_CFBooleanGetTypeID", "_CFDataGetTypeID", "_CFCopyDescription",
    "_CFGetAllocator", "_CFStringCreateWithCString", "_CFStringCreateWithBytes",
    "_CFStringCreateCopy", "_CFStringCreateMutable", "_CFStringCreateMutableCopy",
    "_CFStringAppendCString", "_CFStringAppend", "_CFStringGetLength",
    "_CFStringGetCharacterAtIndex", "_CFStringGetCString", "_CFStringGetCStringPtr",
    "_CFStringGetMaximumSizeForEncoding", "_CFStringCompare", "_CFStringHasPrefix",
    "_CFStringHasSuffix", "_CFStringGetIntValue", "_CFStringGetDoubleValue",
    "_CFStringCreateArrayBySeparatingStrings", "_CFStringCreateByCombiningStrings",
    "___CFStringMakeConstantString", "_CFArrayCreate", "_CFArrayCreateMutable",
    "_CFArrayCreateCopy", "_CFArrayCreateMutableCopy", "_CFArrayGetCount",
    "_CFArrayGetValueAtIndex", "_CFArrayAppendValue", "_CFArrayInsertValueAtIndex",
    "_CFArraySetValueAtIndex", "_CFArrayRemoveValueAtIndex", "_CFArrayRemoveAllValues",
    "_CFDictionaryCreate", "_CFDictionaryCreateMutable", "_CFDictionaryCreateCopy",
    "_CFDictionaryCreateMutableCopy", "_CFDictionaryGetCount", "_CFDictionaryGetValue",
    "_CFDictionaryGetValueIfPresent", "_CFDictionaryContainsKey", "_CFDictionaryAddValue",
    "_CFDictionarySetValue", "_CFDictionaryRemoveValue", "_CFDictionaryGetKeysAndValues",
    "_CFDictionaryApplyFunction", "_CFNumberCreate", "_CFNumberGetValue",
    "_CFNumberGetType", "_CFNumberCompare", "_CFBooleanGetValue", "_CFDataCreate",
    "_CFDataGetLength", "_CFDataGetBytePtr", "_CFAbsoluteTimeGetCurrent",
    "_CFRunLoopGetCurrent", "_CFRunLoopGetMain", "_CFRunLoopRun", "_CFRunLoopRunInMode",
    "_CFRunLoopStop", "_CFRunLoopWakeUp", "_CFRunLoopAddTimer", "_CFRunLoopRemoveTimer",
    "_CFRunLoopTimerCreate", "_CFRunLoopTimerInvalidate", "_CFRunLoopTimerIsValid",
    "_CFRunLoopTimerGetNextFireDate", "_CFRunLoopTimerSetNextFireDate",
    "_CFRunLoopObserverCreate", "_CFRunLoopAddObserver", "_CFRunLoopRemoveObserver",
    "_CFRunLoopObserverInvalidate", "_CFRunLoopSourceCreate", "_CFRunLoopAddSource",
    "_CFRunLoopRemoveSource", "_CFRunLoopSourceSignal", "_CFRunLoopSourceInvalidate",
};
#define NCFBRIDGED (sizeof kCFBridged / sizeof kCFBridged[0])

static const char *const kNoDescriptor[][2] = {
    { "/usr/lib/libSystem.B.dylib", "dyld_stub_binder" },
    { "/usr/lib/libSystem.B.dylib", "__dyld_get_image_uuid" },
    { "/usr/lib/libSystem.B.dylib", "_fork" },
    { "/usr/lib/libSystem.B.dylib", "___stack_chk_guard" },
    { OCERZ_BRIDGE_COREFOUNDATION, "_kCFAllocatorDefault" },
    { OCERZ_BRIDGE_COREFOUNDATION, "_kCFTypeArrayCallBacks" },
    { OCERZ_BRIDGE_COREFOUNDATION, "_strlen" },
    { "/usr/lib/libSystem.B.dylib", "_CFRetain" },
};
#define NNODESC (sizeof kNoDescriptor / sizeof kNoDescriptor[0])

static const char *const kDlSpecials[] = {
    "_dlopen", "_dlopen_audited", "_dlopen_from", "_dlopen_preflight", "_dlsym", "_dladdr",
    "_dlclose", "_dlerror", "__dyld_image_count", "__dyld_get_image_header",
    "__dyld_get_image_name", "__dyld_get_image_vmaddr_slide", "__dyld_get_image_slide",
    "__dyld_register_func_for_add_image", "__dyld_register_func_for_remove_image",
    "__dyld_get_image_header_containing_address", "_dyld_image_header_containing_address",
    "_dyld_image_path_containing_address", "__dyld_image_containing_address",
    "__dyld_get_prog_image_header", "_dyld_get_program_sdk_version",
    "_dyld_get_program_min_os_version", "_dyld_get_sdk_version", "_dyld_get_min_os_version",
    "_dyld_get_active_platform", "_dyld_program_sdk_at_least", "_dyld_program_minos_at_least",
    "_dyld_sdk_at_least", "_dyld_minos_at_least", "__dyld_is_memory_immutable",
    "_dyld_shared_cache_some_image_overridden", "__dyld_shared_cache_contains_path",
};
#define NDLSPECIALS (sizeof kDlSpecials / sizeof kDlSpecials[0])

static OcerzVM vm;
static uint64_t scratch;
static uint64_t stack_top;

static uint64_t put_str(uint64_t gaddr, const char *s)
{
    memcpy(ocerz_g2h(gaddr), s, strlen(s) + 1);
    return gaddr;
}

static uint64_t arm_call(OcerzCPU *cpu, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint64_t sp = stack_top - 8;

    memset(cpu->gpr, 0, sizeof cpu->gpr);
    cpu->gpr[OCERZ_RDI] = a0;
    cpu->gpr[OCERZ_RSI] = a1;
    cpu->gpr[OCERZ_RDX] = a2;
    cpu->gpr[OCERZ_RAX] = RAX_POISON;
    cpu->gpr[OCERZ_RSP] = sp;
    cpu->rip = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;
    ocerz_st(sp, 8, RET_ADDR);
    return sp;
}

static void check_return(const char *sym, const OcerzCPU *cpu, uint64_t sp)
{
    CHECK(cpu->rip == RET_ADDR,
          "%s: rip is %#llx after the bridge, want the return address %#llx",
          sym, (unsigned long long)cpu->rip, (unsigned long long)RET_ADDR);
    CHECK(cpu->gpr[OCERZ_RSP] == sp + 8,
          "%s: rsp is %#llx after the bridge, want %#llx (the call's return "
          "address popped)",
          sym, (unsigned long long)cpu->gpr[OCERZ_RSP],
          (unsigned long long)(sp + 8));
}

static void test_table(void)
{
    size_t i;
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);

    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);

    for (i = 0; i < NBRIDGED; i++) {
        const char *sym = kBridged[i];
        const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);

        CHECK(fn != NULL, "%s has no bridge descriptor", sym);
        CHECK(fn == ocerz_bridge_lookup(kLib, sym),
              "%s resolves to a different descriptor on a second lookup", sym);
        if (img) {
            int found = 0;
            uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
            CHECK(found,
                  "%s is bridged but %s does not export it, so no guest can "
                  "reach the bridge", sym, kLib);
            CHECK(!found || a != 0, "%s resolved to address zero", sym);
        }
    }

    for (i = 0; i < NUNBRIDGED; i++) {
        const char *sym = kUnbridged[i];
        const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);

        CHECK(fn == NULL,
              "%s has a bridge descriptor, but bridge.h excludes it", sym);
        if (img) {
            int found = 0;
            ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
            CHECK(found,
                  "%s is neither bridged nor exported by %s, so it would take "
                  "the 71 path instead of the 72 one", sym, kLib);
        }
    }

    CHECK(ocerz_bridge_lookup("/usr/lib/libNoSuchLibrary.dylib", "_memcpy") == NULL,
          "a library nobody provides has a bridge for _memcpy");
    CHECK(ocerz_bridge_lookup("", "_memcpy") == NULL,
          "the empty install name has a bridge for _memcpy");
    CHECK(ocerz_bridge_lookup(kLib, "_ocerz_no_such_export_xyzzy") == NULL,
          "a symbol nobody exports has a bridge descriptor");

    for (i = 0; i < NNODESC; i++)
        CHECK(ocerz_bridge_lookup(kNoDescriptor[i][0], kNoDescriptor[i][1]) == NULL,
              "%s in %s has a descriptor, but it is not a fn or special record there",
              kNoDescriptor[i][1], kNoDescriptor[i][0]);

    size_t cf_len = 0;
    uint8_t *cf = ocerz_vdylib_image(kCF, &cf_len);
    CHECK(cf != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kCF);
    for (i = 0; i < NCFBRIDGED; i++) {
        const char *sym = kCFBridged[i];
        const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kCF, sym);
        CHECK(fn != NULL, "CoreFoundation's %s has no bridge descriptor", sym);
        CHECK(fn == ocerz_bridge_lookup(kCF, sym),
              "CoreFoundation's %s resolves to a different descriptor on a second lookup", sym);
        if (cf) {
            int found = 0;
            ocerz_dyld_trie_resolve(cf, LOAD_BASE, sym, &found);
            CHECK(found, "%s is bridged but CoreFoundation does not export it", sym);
        }
    }
    free(cf);

    free(img);
}

static pthread_mutex_t g_race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_race_cond = PTHREAD_COND_INITIALIZER;
static int g_race_go;

static void *race_lookup(void *arg)
{
    pthread_mutex_lock(&g_race_lock);
    while (!g_race_go)
        pthread_cond_wait(&g_race_cond, &g_race_lock);
    pthread_mutex_unlock(&g_race_lock);
    *(const struct OcerzBridgeFn **)arg = ocerz_bridge_lookup(kLib, "_strncmp");
    return NULL;
}

static void test_race(void)
{
    const struct OcerzBridgeFn *got[8];
    pthread_t th[8];
    int started = 0;
    for (int i = 0; i < 8; i++)
        started += pthread_create(&th[i], NULL, race_lookup, (void *)&got[i]) == 0;
    pthread_mutex_lock(&g_race_lock);
    g_race_go = 1;
    pthread_cond_broadcast(&g_race_cond);
    pthread_mutex_unlock(&g_race_lock);
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    CHECK(started == 8, "only %d of 8 lookup threads started", started);
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, "_strncmp");
    CHECK(fn != NULL, "_strncmp has no descriptor");
    for (int i = 0; i < started; i++)
        CHECK(got[i] == fn, "thread %d was handed descriptor %p for _strncmp, the main thread %p",
              i, (void *)got[i], (void *)fn);
}

static void test_report(void)
{
    FILE *cap = tmpfile();
    int saved = dup(2);
    CHECK(cap != NULL && saved >= 0, "cannot capture the report");
    if (!cap || saved < 0) {
        if (cap)
            fclose(cap);
        if (saved >= 0)
            close(saved);
        return;
    }
    fflush(stderr);
    dup2(fileno(cap), 2);
    ocerz_bridge_report();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    char text[8192];
    rewind(cap);
    size_t n = fread(text, 1, sizeof text - 1, cap);
    text[n] = '\0';
    fclose(cap);
    fputs(text, stderr);

    char head[128];
    snprintf(head, sizeof head, "ocerz: BRIDGESTAT[%d] crossings=", (int)getpid());
    const char *at = strstr(text, head);
    unsigned long long crossings = 0;
    int used = 0, declared = 0;
    int parsed = at && sscanf(at + strlen(head), "%llu over %d of %d bridged export(s)", &crossings,
                              &used, &declared) == 3;
    CHECK(parsed, "the report has no crossings line: %s", text);
    CHECK(crossings == 6 && used == 4,
          "the report counts %llu crossings over %d exports, want 6 over 4 (_strlen and _strchr "
          "twice, _malloc and _free once)", crossings, used);
    CHECK(declared >= (int)(NBRIDGED + NCFBRIDGED),
          "the report says %d bridged exports, fewer than the %zu this test looked up", declared,
          NBRIDGED + NCFBRIDGED);
    char first[128];
    snprintf(first, sizeof first, "ocerz: BRIDGESTAT[%d]   # 1 _str", (int)getpid());
    CHECK(strstr(text, first) != NULL, "the busiest export in the report is not _strlen or _strchr");
}

static uint64_t dl_call(const char *sym, uint64_t a0, uint64_t a1, uint64_t a2)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);
    CHECK(fn != NULL, "%s has no bridge descriptor", sym);
    if (!fn)
        return RAX_POISON;
    uint64_t sp = arm_call(cpu, a0, a1, a2);
    int r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "%s: invoke returned %d, want OCERZ_STEP_OK", sym, r);
    check_return(sym, cpu, sp);
    return cpu->gpr[OCERZ_RAX];
}

static const char *dl_text(uint64_t g)
{
    return g ? (const char *)ocerz_g2h(g) : "(null)";
}

static void test_dl_specials(void)
{
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);
    for (size_t i = 0; i < NDLSPECIALS; i++) {
        int found = 0;
        CHECK(ocerz_bridge_lookup(kLib, kDlSpecials[i]) != NULL, "%s has no descriptor", kDlSpecials[i]);
        if (img)
            ocerz_dyld_trie_resolve(img, LOAD_BASE, kDlSpecials[i], &found);
        CHECK(found, "%s is not exported by %s", kDlSpecials[i], kLib);
    }
    free(img);

    CHECK(dl_call("_dlerror", 0, 0, 0) == 0, "dlerror answered something before any call failed");
    CHECK(dl_call("_dlsym", (uint64_t)-2, put_str(scratch + 256, "ocerz_no_such_symbol"), 0) == 0,
          "dlsym(RTLD_DEFAULT) found a symbol no image exports");
    uint64_t e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strcmp(dl_text(e), "dlsym(RTLD_DEFAULT, ocerz_no_such_symbol): symbol not found") == 0,
          "dlerror after a missed dlsym answered '%s'", dl_text(e));
    CHECK(dl_call("_dlerror", 0, 0, 0) == 0, "dlerror answered a second time for one failure");

    CHECK(dl_call("_dlopen", 0, 2, 0) == (uint64_t)-2, "dlopen(NULL) is not RTLD_DEFAULT");
    CHECK(dl_call("_dlopen", 0, 0x102, 0) == (uint64_t)-5, "dlopen(NULL, RTLD_FIRST) is not RTLD_MAIN_ONLY");
    CHECK(dl_call("_dlerror", 0, 0, 0) == 0, "dlerror answered after dlopen(NULL) succeeded");

    CHECK(dl_call("_dlopen", put_str(scratch + 256, "/nonexistent/ocerz/libnope.dylib"), 2, 0) == 0,
          "dlopen of a path nothing provides answered a handle");
    e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strstr(dl_text(e), "tried: '/nonexistent/ocerz/libnope.dylib' (no such file)") != NULL,
          "dlopen of a missing path left '%s'", dl_text(e));

    CHECK(dl_call("_dlopen", put_str(scratch + 256, "/usr/lib/libz.1.dylib"), 2, 0) == 0,
          "dlopen of a native library with no database answered a handle");
    e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strstr(dl_text(e), "native library without an API database") != NULL,
          "dlopen of libz left '%s'", dl_text(e));

    CHECK(dl_call("_dlclose", (uint64_t)-2, 0, 0) == 0, "dlclose(RTLD_DEFAULT) failed");
    CHECK((uint32_t)dl_call("_dlclose", 0x1234, 0, 0) == 0xffffffffu, "dlclose of a bogus handle did not fail");
    e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strcmp(dl_text(e), "dlclose(0x1234): invalid handle") == 0,
          "dlclose of a bogus handle left '%s'", dl_text(e));
    CHECK(dl_call("_dlsym", 0x1234, put_str(scratch + 256, "strlen"), 0) == 0,
          "dlsym on a bogus handle found something");
    e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strcmp(dl_text(e), "dlsym(0x1234, strlen): invalid handle") == 0,
          "dlsym on a bogus handle left '%s'", dl_text(e));

    CHECK((uint32_t)dl_call("__dyld_image_count", 0, 0, 0) == 0, "an image list with nothing loaded is not empty");
    CHECK(dl_call("__dyld_get_image_header", 0, 0, 0) == 0, "image 0 has a header with nothing loaded");
    CHECK(dl_call("_dyld_shared_cache_some_image_overridden", 0, 0, 0) == 0,
          "an image of a cache native mode does not have is overridden");
    CHECK(dl_call("__dyld_shared_cache_contains_path", put_str(scratch + 256, kLib), 0, 0) == 1,
          "libSystem, which a database describes, is not a library the cache contains");
    CHECK(dl_call("__dyld_shared_cache_contains_path",
                  put_str(scratch + 256, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"),
                  0, 0) == 1,
          "CoreFoundation reached through its framework symlink is not a library the cache contains");
    CHECK(dl_call("__dyld_shared_cache_contains_path", put_str(scratch + 256, "/usr/lib/libz.1.dylib"), 0, 0) == 0,
          "libz, which no database describes, is a library the cache contains");

    uint64_t info = scratch + 512;
    Dl_info host;
    memset(ocerz_g2h(info), 0, 32);
    CHECK(dladdr((const void *)strlen, &host) != 0, "the host's dladdr does not know strlen");
    CHECK(dl_call("_dladdr", ocerz_h2g((const void *)strlen), info, 0) == 1,
          "dladdr on the host's strlen answered nothing");
    CHECK(ocerz_ld(info + 0x08, 8) == ocerz_h2g(host.dli_fbase) &&
          ocerz_ld(info + 0x18, 8) == ocerz_h2g(host.dli_saddr) &&
          strcmp(dl_text(ocerz_ld(info + 0x10, 8)), host.dli_sname) == 0,
          "dladdr on the host's strlen answered base %#llx symbol %s, where the host says %p %s",
          (unsigned long long)ocerz_ld(info + 0x08, 8), dl_text(ocerz_ld(info + 0x10, 8)), host.dli_fbase,
          host.dli_sname);
    CHECK(dl_call("_dladdr", scratch, info, 0) == 0, "dladdr on guest memory no image holds answered something");
}

static void test_invoke_int(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, "_strlen");
    uint64_t gs = put_str(scratch, "ocerz-bridge");
    uint64_t sp;
    int r;

    CHECK(fn != NULL, "_strlen has no bridge descriptor");
    if (!fn)
        return;

    sp = arm_call(cpu, gs, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strlen: invoke returned %d, want OCERZ_STEP_OK", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 12,
          "_strlen(\"ocerz-bridge\") put %#llx in rax, want 12",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strlen", cpu, sp);

    sp = arm_call(cpu, put_str(scratch + 64, ""), 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strlen: invoke returned %d on an empty string", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 0,
          "_strlen(\"\") put %#llx in rax, want 0",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strlen", cpu, sp);
}

static void test_invoke_ptr(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, "_strchr");
    uint64_t gs = put_str(scratch + 128, "ocerz-bridge");
    uint64_t sp;
    int r;

    CHECK(fn != NULL, "_strchr has no bridge descriptor");
    if (!fn)
        return;

    sp = arm_call(cpu, gs, '-', 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strchr: invoke returned %d, want OCERZ_STEP_OK", r);
    CHECK(cpu->gpr[OCERZ_RAX] == gs + 5,
          "_strchr(\"ocerz-bridge\", '-') put %#llx in rax, want %#llx",
          (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)(gs + 5));
    check_return("_strchr", cpu, sp);

    sp = arm_call(cpu, gs, 'q', 0);
    r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "_strchr: invoke returned %d on a miss", r);
    CHECK(cpu->gpr[OCERZ_RAX] == 0,
          "_strchr(\"ocerz-bridge\", 'q') put %#llx in rax, want 0",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    check_return("_strchr", cpu, sp);
}

static void test_invoke_heap(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *mal = ocerz_bridge_lookup(kLib, "_malloc");
    const struct OcerzBridgeFn *fre = ocerz_bridge_lookup(kLib, "_free");
    unsigned char want[64];
    uint64_t block, sp;
    int r;

    CHECK(mal != NULL, "_malloc has no bridge descriptor");
    CHECK(fre != NULL, "_free has no bridge descriptor");
    if (!mal || !fre)
        return;

    sp = arm_call(cpu, 64, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, mal);
    CHECK(r == OCERZ_STEP_OK, "_malloc: invoke returned %d, want OCERZ_STEP_OK", r);
    block = cpu->gpr[OCERZ_RAX];
    CHECK(block != 0 && block != RAX_POISON,
          "_malloc(64) put %#llx in rax", (unsigned long long)block);
    check_return("_malloc", cpu, sp);
    if (block == 0 || block == RAX_POISON)
        return;

    memset(want, 0x5a, sizeof want);
    memcpy(ocerz_g2h(block), want, sizeof want);
    CHECK(memcmp(ocerz_g2h(block), want, sizeof want) == 0,
          "the 64 bytes written through the guest address %#llx that _malloc "
          "returned did not read back",
          (unsigned long long)block);

    sp = arm_call(cpu, block, 0, 0);
    r = ocerz_bridge_invoke(&vm, cpu, fre);
    CHECK(r == OCERZ_STEP_OK, "_free: invoke returned %d, want OCERZ_STEP_OK", r);
    check_return("_free", cpu, sp);
}

static int report(void)
{
    printf("test_bridge: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    setenv("OCERZ_APIDB", "runtime/apis", 0);
    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }
    if (ocerz_vm_init(&vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    scratch = ocerz_map_anywhere(SCRATCH, PROT_READ | PROT_WRITE);
    if (scratch == 0) {
        fprintf(stderr, "scratch alloc failed\n");
        return 2;
    }
    stack_top = scratch + SCRATCH / 2;

    test_table();
    test_invoke_int();
    test_invoke_ptr();
    test_invoke_heap();
    test_race();
    test_report();
    test_dl_specials();

    return report();
}
