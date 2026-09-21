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
 * variadic exclusion bridge.h states, the functions whose ellipsis stands for a
 * list no format veneer reads (syslog, err, warn), so a bridge
 * that grows one of them has changed a documented rule rather than broken a
 * test.  The list once also held atof and strtod, until signatures could name a
 * double, and qsort and bsearch, until a callback argument had a trampoline
 * back into guest code; qsort and bsearch are bridged names now, and their
 * lookups succeeding is also what proves the callback notation parses.  open,
 * fcntl and ioctl stood there too, until src/sysbridge.c gave the variadic
 * functions whose optional argument is fixed handlers of their own, and scanf
 * and sscanf stood there until the scanf family grew veneers that count their
 * pointer arguments from the format string.
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
 *
 * The handlers src/sysbridge.c adds are driven the same way, after the report
 * has been checked so their crossings do not change its counts.  The variadic
 * ones are handed an optional argument where System V leaves it, with garbage in
 * the upper half of a register that carries an int, and the file they touch is
 * checked through the host's own calls.  The memory ones must hand out arena
 * memory, keep ocerz's page table in step, and pass a range ocerz never mapped,
 * a posix_memalign page or a region the test allocated with the host's own Mach
 * call, to the host kernel.  The jump ones must lay the jmp_buf out as Apple's
 * x86_64 libplatform does, restore exactly what they saved and the mask only
 * when they saved one, and refuse a jump from inside a callback to a setjmp
 * taken outside it; the refusal ends the process, so that case runs in a fork
 * child whose status and message are read back.
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
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ulimit.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

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
    "_open", "_open$NOCANCEL", "_openat", "_openat$NOCANCEL", "_fcntl", "_fcntl$NOCANCEL",
    "_ioctl", "_sem_open", "_shm_open", "_semctl", "_ulimit", "_mmap", "_munmap", "_mprotect",
    "_madvise", "_mach_vm_allocate", "_mach_vm_deallocate", "_mach_vm_protect",
    "_vm_allocate", "_vm_deallocate", "_vm_protect", "_setjmp", "__setjmp", "_sigsetjmp",
    "_longjmp", "__longjmp", "_siglongjmp", "_fork", "_vfork", "_execve", "_execv",
    "_execvp", "_execvP", "_execl", "_execle", "_execlp", "_posix_spawn", "_posix_spawnp",
    "_posix_spawnattr_init", "_posix_spawn_file_actions_adddup2", "_system", "_popen",
    "_pclose", "_syslog", "_vsyslog", "_warn", "_warnx", "_swprintf", "_vswprintf",
};
#define NBRIDGED (sizeof kBridged / sizeof kBridged[0])

static const char *const kUnbridged[] = {
    "_err", "_errx", "_asl_log",
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
    { "/usr/lib/libSystem.B.dylib", "_forkpty" },
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

    CHECK(dl_call("_dlopen", put_str(scratch + 256, "/usr/lib/libsqlite3.dylib"), 2, 0) == 0,
          "dlopen of a native library with no database answered a handle");
    e = dl_call("_dlerror", 0, 0, 0);
    CHECK(e && strstr(dl_text(e), "native library without an API database") != NULL,
          "dlopen of sqlite3 left '%s'", dl_text(e));

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
    CHECK(dl_call("__dyld_shared_cache_contains_path", put_str(scratch + 256, "/usr/lib/libsqlite3.dylib"), 0, 0) == 0,
          "sqlite3, which no database describes, is a library the cache contains");

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


static uint64_t arm_call6(OcerzCPU *cpu, const uint64_t a[6])
{
    uint64_t sp = arm_call(cpu, a[0], a[1], a[2]);
    cpu->gpr[OCERZ_RCX] = a[3];
    cpu->gpr[OCERZ_R8] = a[4];
    cpu->gpr[OCERZ_R9] = a[5];
    return sp;
}

static int64_t sys_call(const char *sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5)
{
    OcerzCPU *cpu = &vm.cpu;
    const struct OcerzBridgeFn *fn = ocerz_bridge_lookup(kLib, sym);
    CHECK(fn != NULL, "%s has no bridge descriptor", sym);
    if (!fn)
        return INT64_MIN;
    const uint64_t a[6] = { a0, a1, a2, a3, a4, a5 };
    uint64_t sp = arm_call6(cpu, a);
    int r = ocerz_bridge_invoke(&vm, cpu, fn);
    CHECK(r == OCERZ_STEP_OK, "%s: invoke returned %d, want OCERZ_STEP_OK", sym, r);
    check_return(sym, cpu, sp);
    return (int64_t)cpu->gpr[OCERZ_RAX];
}

static void test_sys_files(void)
{
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char path[512], other[512], real[PATH_MAX];
    snprintf(path, sizeof path, "%s/ocerz-test-bridge-%d", tmp, (int)getpid());
    snprintf(other, sizeof other, "%s/ocerz-test-bridge-%d.at", tmp, (int)getpid());
    unlink(path);
    unlink(other);
    mode_t old_mask = umask(022);
    uint64_t gpath = put_str(scratch + 0x400, path);
    uint64_t gother = put_str(scratch + 0x600, other);
    struct stat st;

    int64_t fd = sys_call("_open", gpath, O_CREAT | O_EXCL | O_RDWR, 0xdeadbeef00000000ull | 0640, 0, 0, 0);
    CHECK((int32_t)fd >= 0, "_open with O_CREAT returned %lld", (long long)fd);
    CHECK(fstat((int)fd, &st) == 0 && (st.st_mode & 0777) == 0640,
          "_open created the file with mode %o, want 0640 from the low half of rdx",
          (unsigned)(st.st_mode & 0777));

    int64_t rd = sys_call("_open$NOCANCEL", gpath, O_RDONLY, 0xffffffffffffffffull, 0, 0, 0);
    CHECK((int32_t)rd >= 0, "_open$NOCANCEL without O_CREAT returned %lld", (long long)rd);
    if ((int32_t)rd >= 0)
        close((int)rd);
    errno = 0;
    int64_t miss = sys_call("_open", put_str(scratch + 0x800, "/ocerz/no/such/file"), O_RDONLY, 0, 0,
                            0, 0);
    CHECK(miss == -1 && errno == ENOENT, "_open of a missing file gave %lld errno %d, want -1 ENOENT",
          (long long)miss, errno);

    int64_t at = sys_call("_openat", (uint64_t)(int64_t)AT_FDCWD, gother, O_CREAT | O_WRONLY, 0600, 0, 0);
    CHECK((int32_t)at >= 0 && fstat((int)at, &st) == 0 && (st.st_mode & 0777) == 0600,
          "_openat with O_CREAT gave fd %lld mode %o, want mode 0600 from rcx", (long long)at,
          (unsigned)(st.st_mode & 0777));
    if ((int32_t)at >= 0)
        close((int)at);

    int64_t fl = sys_call("_fcntl", (uint64_t)fd, F_GETFL, 0, 0, 0, 0);
    CHECK((fl & O_ACCMODE) == O_RDWR, "_fcntl F_GETFL gave %#llx", (long long)fl);
    CHECK(sys_call("_fcntl", (uint64_t)fd, F_SETFL, 0x7777777700000000ull | (uint64_t)(fl | O_NONBLOCK),
                   0, 0, 0) == 0, "_fcntl F_SETFL with a dirty upper half failed");
    CHECK(fcntl((int)fd, F_GETFL) & O_NONBLOCK, "F_SETFL did not set O_NONBLOCK");
    CHECK(sys_call("_fcntl$NOCANCEL", (uint64_t)fd, F_SETFD, 0xabcdef0000000000ull | FD_CLOEXEC, 0, 0,
                   0) == 0 && (fcntl((int)fd, F_GETFD) & FD_CLOEXEC),
          "_fcntl$NOCANCEL F_SETFD did not set FD_CLOEXEC");
    uint64_t gbuf = scratch + 0x1000;
    memset(ocerz_g2h(gbuf), 0, PATH_MAX);
    CHECK(sys_call("_fcntl", (uint64_t)fd, F_GETPATH, gbuf, 0, 0, 0) == 0 &&
              realpath(path, real) && strcmp(ocerz_g2h(gbuf), real) == 0,
          "_fcntl F_GETPATH wrote '%s', want '%s'", (const char *)ocerz_g2h(gbuf), real);
    struct flock lk = { .l_start = 0, .l_len = 16, .l_pid = 0, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    uint64_t glk = scratch + 0x2000;
    memcpy(ocerz_g2h(glk), &lk, sizeof lk);
    CHECK(sys_call("_fcntl", (uint64_t)fd, F_SETLK, glk, 0, 0, 0) == 0, "_fcntl F_SETLK failed");
    CHECK(sys_call("_fcntl", (uint64_t)fd, F_GETLK, glk, 0, 0, 0) == 0 &&
              ((struct flock *)ocerz_g2h(glk))->l_type == F_UNLCK,
          "_fcntl F_GETLK on the owner's own lock did not answer F_UNLCK");
    close((int)fd);

    int p[2];
    CHECK(pipe(p) == 0, "pipe failed");
    CHECK(write(p[1], "hello", 5) == 5, "pipe write failed");
    uint64_t gint = scratch + 0x2100;
    ocerz_st(gint, 4, 0);
    CHECK(sys_call("_ioctl", (uint64_t)p[0], FIONREAD, gint, 0, 0, 0) == 0 && ocerz_ld(gint, 4) == 5,
          "_ioctl FIONREAD gave %u, want 5", (unsigned)ocerz_ld(gint, 4));
    CHECK(sys_call("_ioctl", (uint64_t)p[1], FIOCLEX, 0x1234, 0, 0, 0) == 0 &&
              (fcntl(p[1], F_GETFD) & FD_CLOEXEC),
          "_ioctl FIOCLEX, a request that carries no pointer, did not set FD_CLOEXEC");
    close(p[0]);
    close(p[1]);

    char name[64];
    snprintf(name, sizeof name, "/ocz-tb-%d", (int)getpid());
    sem_unlink(name);
    uint64_t gname = put_str(scratch + 0x2200, name);
    int64_t sem = sys_call("_sem_open", gname, O_CREAT | O_EXCL, 0600, 2, 0, 0);
    CHECK(sem != -1 && sem != 0, "_sem_open with O_CREAT returned %lld", (long long)sem);
    if (sem != -1 && sem != 0) {
        sem_t *h = (sem_t *)(uintptr_t)sem;
        CHECK(sem_trywait(h) == 0 && sem_trywait(h) == 0 && sem_trywait(h) == -1,
              "the semaphore _sem_open made did not start at the value 2 in rcx");
        sem_close(h);
    }
    sem_unlink(name);
    errno = 0;
    CHECK(sys_call("_sem_open", gname, 0, 0, 0, 0, 0) == -1 && errno == ENOENT,
          "_sem_open of an unlinked name did not fail with ENOENT");

    snprintf(name, sizeof name, "/ocz-tb-shm-%d", (int)getpid());
    shm_unlink(name);
    gname = put_str(scratch + 0x2200, name);
    int64_t shm = sys_call("_shm_open", gname, O_CREAT | O_RDWR, 0600, 0, 0, 0);
    CHECK((int32_t)shm >= 0 && fstat((int)shm, &st) == 0 && (st.st_mode & 0777) == 0600,
          "_shm_open with O_CREAT gave %lld mode %o", (long long)shm, (unsigned)(st.st_mode & 0777));
    if ((int32_t)shm >= 0)
        close((int)shm);
    shm_unlink(name);

    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_FSIZE, &rl) == 0, "getrlimit failed");
    int64_t ul = sys_call("_ulimit", UL_GETFSIZE, 0, 0, 0, 0, 0);
    CHECK(rl.rlim_cur == RLIM_INFINITY || ul == (int64_t)(rl.rlim_cur / 512),
          "_ulimit UL_GETFSIZE gave %lld", (long long)ul);

    unlink(path);
    unlink(other);
    umask(old_mask);
}

static unsigned host_prot(uint64_t addr, uint64_t *base)
{
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt,
                       &obj) != KERN_SUCCESS)
        return ~0u;
    *base = a;
    return (unsigned)info.protection;
}

static void test_sys_memory(void)
{
    const uint64_t len = 0x8000;
    int64_t m = sys_call("_mmap", 0, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                         0xffffffffull, 0);
    CHECK(m != -1 && ocerz_mem_overlaps((uint64_t)m, len) && (m & 0xfff) == 0,
          "_mmap handed out %#llx, which is not a page of the guest arena", (long long)m);
    if (m == -1)
        return;
    memset(ocerz_g2h((uint64_t)m), 0x6b, len);
    CHECK(ocerz_addr_prot((uint64_t)m) == (PROT_READ | PROT_WRITE),
          "the page table says %d for fresh mmap memory", ocerz_addr_prot((uint64_t)m));
    CHECK(sys_call("_mprotect", (uint64_t)m, 0x1000, PROT_READ, 0, 0, 0) == 0 &&
              ocerz_addr_prot((uint64_t)m) == PROT_READ &&
              ocerz_addr_prot((uint64_t)m + 0x1000) == (PROT_READ | PROT_WRITE),
          "_mprotect of one 4 KB guest page did not change exactly that page");
    CHECK(sys_call("_madvise", (uint64_t)m, len, MADV_WILLNEED, 0, 0, 0) == 0, "_madvise failed");
    CHECK(sys_call("_munmap", (uint64_t)m, len, 0, 0, 0, 0) == 0 && ocerz_addr_prot((uint64_t)m) == -1,
          "_munmap left the pages in the table");

    void *host = NULL;
    CHECK(posix_memalign(&host, 0x4000, 0x4000) == 0, "posix_memalign failed");
    uint64_t base = 0;
    if (host) {
        uint64_t h = (uint64_t)(uintptr_t)host;
        CHECK(!ocerz_mem_overlaps(h, 0x4000), "a host heap page is inside the guest arena");
        CHECK(sys_call("_mprotect", h, 0x4000, PROT_READ, 0, 0, 0) == 0 &&
                  host_prot(h, &base) == PROT_READ && base <= h,
              "_mprotect of a host heap page did not reach the host kernel");
        CHECK(sys_call("_mprotect", h, 0x4000, PROT_READ | PROT_WRITE, 0, 0, 0) == 0,
              "_mprotect could not give a host heap page its write access back");
        ((volatile char *)host)[0] = 1;
        free(host);
    }

    uint64_t gaddr = scratch + 0x2300;
    ocerz_st(gaddr, 8, 0);
    CHECK(sys_call("_mach_vm_allocate", mach_task_self(), gaddr, 0x3000, VM_FLAGS_ANYWHERE, 0, 0) ==
              KERN_SUCCESS && ocerz_mem_overlaps(ocerz_ld(gaddr, 8), 0x3000),
          "_mach_vm_allocate did not hand out arena memory");
    uint64_t va = ocerz_ld(gaddr, 8);
    if (va) {
        CHECK(*(volatile uint64_t *)ocerz_g2h(va) == 0, "_mach_vm_allocate memory is not zeroed");
        CHECK(sys_call("_vm_protect", mach_task_self(), va, 0x1000, 0, VM_PROT_READ, 0) == KERN_SUCCESS &&
                  ocerz_addr_prot(va) == PROT_READ,
              "_vm_protect did not change the guest page");
        CHECK(sys_call("_vm_deallocate", mach_task_self(), va, 0x3000, 0, 0, 0) == KERN_SUCCESS &&
                  ocerz_addr_prot(va) == -1,
              "_vm_deallocate left the pages in the table");
    }

    mach_vm_address_t hv = 0;
    CHECK(mach_vm_allocate(mach_task_self(), &hv, 0x4000, VM_FLAGS_ANYWHERE) == KERN_SUCCESS,
          "the host would not allocate");
    if (hv) {
        CHECK(!ocerz_mem_overlaps(hv, 0x4000), "host Mach memory is inside the guest arena");
        CHECK(sys_call("_mach_vm_protect", mach_task_self(), hv, 0x4000, 0, VM_PROT_READ, 0) ==
                  KERN_SUCCESS && host_prot(hv, &base) == VM_PROT_READ,
              "_mach_vm_protect of host memory did not reach the host kernel");
        CHECK(sys_call("_mach_vm_deallocate", mach_task_self(), hv, 0x4000, 0, 0, 0) == KERN_SUCCESS &&
                  (host_prot(hv, &base) == ~0u || base > hv),
              "_mach_vm_deallocate of host memory left it mapped");
    }
}

#define JB_BYTES 152

static void test_sys_jmp(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t env = scratch + 0x3000;
    memset(ocerz_g2h(env), 0xee, JB_BYTES);
    const struct OcerzBridgeFn *sj = ocerz_bridge_lookup(kLib, "_setjmp");
    const struct OcerzBridgeFn *lj = ocerz_bridge_lookup(kLib, "_longjmp");
    const struct OcerzBridgeFn *usj = ocerz_bridge_lookup(kLib, "__setjmp");
    const struct OcerzBridgeFn *ulj = ocerz_bridge_lookup(kLib, "__longjmp");
    const struct OcerzBridgeFn *ssj = ocerz_bridge_lookup(kLib, "_sigsetjmp");
    const struct OcerzBridgeFn *slj = ocerz_bridge_lookup(kLib, "_siglongjmp");
    CHECK(sj && lj && usj && ulj && ssj && slj, "a setjmp or longjmp export has no descriptor");
    if (!sj || !lj || !usj || !ulj || !ssj || !slj)
        return;

    uint64_t sp = arm_call(cpu, env, 0, 0);
    cpu->gpr[OCERZ_RBX] = 0x1111;
    cpu->gpr[OCERZ_RBP] = 0x2222;
    cpu->gpr[OCERZ_R12] = 0x3333;
    cpu->gpr[OCERZ_R13] = 0x4444;
    cpu->gpr[OCERZ_R14] = 0x5555;
    cpu->gpr[OCERZ_R15] = 0x6666;
    cpu->mxcsr = 0x3fa0;
    cpu->fcw = 0x027f;
    cpu->sig_mask = 0x5;
    CHECK(ocerz_bridge_invoke(&vm, cpu, sj) == OCERZ_STEP_OK, "_setjmp did not step");
    check_return("_setjmp", cpu, sp);
    CHECK(cpu->gpr[OCERZ_RAX] == 0, "_setjmp returned %#llx, want 0",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    CHECK(ocerz_ld(env + 0, 8) == 0x1111 && ocerz_ld(env + 8, 8) == 0x2222 &&
              ocerz_ld(env + 16, 8) == sp + 8 && ocerz_ld(env + 24, 8) == 0x3333 &&
              ocerz_ld(env + 32, 8) == 0x4444 && ocerz_ld(env + 40, 8) == 0x5555 &&
              ocerz_ld(env + 48, 8) == 0x6666 && ocerz_ld(env + 56, 8) == RET_ADDR,
          "_setjmp did not lay rbx, rbp, rsp, r12-r15 and rip out at 0..56");
    CHECK(ocerz_ld(env + 64, 8) == 0xeeeeeeeeeeeeeeeeull, "_setjmp wrote the unused rflags word");
    CHECK(ocerz_ld(env + 72, 4) == 0x3fa0 && ocerz_ld(env + 76, 2) == 0x027f,
          "_setjmp did not save MXCSR at 72 and the x87 control word at 76");
    CHECK(ocerz_ld(env + 80, 4) == 0x5 && ocerz_ld(env + 88, 4) == 0x4,
          "_setjmp saved mask %#llx and alternate-stack flags %#llx at 80 and 88, want 5 and "
          "SS_DISABLE", (unsigned long long)ocerz_ld(env + 80, 4),
          (unsigned long long)ocerz_ld(env + 88, 4));
    CHECK(ocerz_ld(env + 84, 4) == 0xeeeeeeee, "_setjmp wrote sigsetjmp's savemask word");

    arm_call(cpu, env, 0xffffffff00000000ull, 0);
    cpu->mxcsr = 0x1f80;
    cpu->fcw = 0x037f;
    cpu->sig_mask = 0;
    cpu->rflags |= OCERZ_DF;
    CHECK(ocerz_bridge_invoke(&vm, cpu, lj) == OCERZ_STEP_OK, "_longjmp did not step");
    CHECK(cpu->rip == RET_ADDR && cpu->gpr[OCERZ_RSP] == sp + 8,
          "_longjmp went to rip %#llx rsp %#llx, want the setjmp's return",
          (unsigned long long)cpu->rip, (unsigned long long)cpu->gpr[OCERZ_RSP]);
    CHECK(cpu->gpr[OCERZ_RAX] == 1, "_longjmp with a value whose low half is 0 returned %#llx, want 1",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);
    CHECK(cpu->gpr[OCERZ_RBX] == 0x1111 && cpu->gpr[OCERZ_RBP] == 0x2222 &&
              cpu->gpr[OCERZ_R12] == 0x3333 && cpu->gpr[OCERZ_R15] == 0x6666,
          "_longjmp did not restore the callee-saved registers");
    CHECK(cpu->mxcsr == 0x3fa0 && cpu->fcw == 0x027f && !(cpu->rflags & OCERZ_DF),
          "_longjmp did not restore MXCSR and the x87 control word and clear DF");
    CHECK(cpu->sig_mask == 0x5, "_longjmp did not restore the signal mask setjmp saved");
    ocerz_apply_mxcsr_round(0x1f80);
    cpu->mxcsr = 0x1f80;

    memset(ocerz_g2h(env), 0xee, JB_BYTES);
    sp = arm_call(cpu, env, 0, 0);
    cpu->sig_mask = 0x3;
    CHECK(ocerz_bridge_invoke(&vm, cpu, usj) == OCERZ_STEP_OK, "__setjmp did not step");
    CHECK(ocerz_ld(env + 80, 4) == 0xeeeeeeee, "__setjmp saved a signal mask");
    arm_call(cpu, env, 7, 0);
    cpu->sig_mask = 0x9;
    CHECK(ocerz_bridge_invoke(&vm, cpu, ulj) == OCERZ_STEP_OK, "__longjmp did not step");
    CHECK(cpu->gpr[OCERZ_RAX] == 7 && cpu->sig_mask == 0x9 && cpu->rip == RET_ADDR,
          "__longjmp returned %#llx with mask %#llx, want 7 and the mask left alone",
          (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)cpu->sig_mask);

    memset(ocerz_g2h(env), 0xee, JB_BYTES);
    arm_call(cpu, env, 1, 0);
    cpu->sig_mask = 0x10;
    CHECK(ocerz_bridge_invoke(&vm, cpu, ssj) == OCERZ_STEP_OK, "_sigsetjmp did not step");
    CHECK(ocerz_ld(env + 84, 4) == 1 && ocerz_ld(env + 80, 4) == 0x10,
          "_sigsetjmp(env, 1) did not record savemask and the mask");
    arm_call(cpu, env, 3, 0);
    cpu->sig_mask = 0;
    CHECK(ocerz_bridge_invoke(&vm, cpu, slj) == OCERZ_STEP_OK && cpu->sig_mask == 0x10 &&
              cpu->gpr[OCERZ_RAX] == 3,
          "_siglongjmp of a buffer that saved its mask did not restore it");
    arm_call(cpu, env, 0, 0);
    cpu->sig_mask = 0x10;
    CHECK(ocerz_bridge_invoke(&vm, cpu, ssj) == OCERZ_STEP_OK && ocerz_ld(env + 84, 4) == 0,
          "_sigsetjmp(env, 0) did not record a zero savemask");
    arm_call(cpu, env, 3, 0);
    cpu->sig_mask = 0x20;
    CHECK(ocerz_bridge_invoke(&vm, cpu, slj) == OCERZ_STEP_OK && cpu->sig_mask == 0x20,
          "_siglongjmp of a buffer that saved no mask changed the mask");
    cpu->sig_mask = 0;
}

static void test_sys_jmp_refused(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t env = scratch + 0x3200;
    const struct OcerzBridgeFn *sj = ocerz_bridge_lookup(kLib, "__setjmp");
    const struct OcerzBridgeFn *lj = ocerz_bridge_lookup(kLib, "__longjmp");
    if (!sj || !lj)
        return;
    arm_call(cpu, env, 0, 0);
    CHECK(ocerz_bridge_invoke(&vm, cpu, sj) == OCERZ_STEP_OK, "__setjmp did not step");

    struct OcerzBridgeFrame outer, saved;
    ocerz_bridge_raise(&outer, kLib, "_qsort", "v(pLLc{i(pp)})", NULL);
    ocerz_bridge_guest_enter(&saved);
    uint64_t inner = scratch + 0x3400;
    arm_call(cpu, inner, 0, 0);
    CHECK(ocerz_bridge_invoke(&vm, cpu, sj) == OCERZ_STEP_OK, "__setjmp inside a callback did not step");
    arm_call(cpu, inner, 5, 0);
    CHECK(ocerz_bridge_invoke(&vm, cpu, lj) == OCERZ_STEP_OK && cpu->gpr[OCERZ_RAX] == 5,
          "a __longjmp inside a callback to a setjmp in the same callback was not performed");

    int p[2];
    CHECK(pipe(p) == 0, "pipe failed");
    fflush(stderr);
    pid_t child = fork();
    if (child == 0) {
        dup2(p[1], 2);
        arm_call(cpu, env, 1, 0);
        ocerz_bridge_invoke(&vm, cpu, lj);
        _exit(0);
    }
    close(p[1]);
    char msg[512] = { 0 };
    ssize_t n = read(p[0], msg, sizeof msg - 1);
    (void)n;
    close(p[0]);
    int status = 0;
    CHECK(child > 0 && waitpid(child, &status, 0) == child, "the refusal child could not be waited for");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == OCERZ_BRIDGE_UNIMPL_EXIT,
          "a __longjmp out of a callback to a setjmp taken outside it ended with status %#x, want exit %d",
          status, OCERZ_BRIDGE_UNIMPL_EXIT);
    CHECK(strstr(msg, "__longjmp") && strstr(msg, "native frames of _qsort"),
          "the refusal did not name the jump and the call it would skip: '%s'", msg);

    ocerz_bridge_guest_leave(&saved);
    ocerz_bridge_lower(&outer);
    arm_call(cpu, env, 9, 0);
    CHECK(ocerz_bridge_invoke(&vm, cpu, lj) == OCERZ_STEP_OK && cpu->gpr[OCERZ_RAX] == 9,
          "once the callback returned, a __longjmp to the setjmp outside it was not performed");
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
    test_sys_files();
    test_sys_memory();
    test_sys_jmp();
    test_sys_jmp_refused();

    return report();
}
