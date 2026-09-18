/*
 * The synthesized system libraries, checked as the loader will actually read
 * them.
 *
 * Native mode maps no shared cache, so ocerz_vdylib_image builds an x86_64
 * Mach-O for /usr/lib/libSystem.B.dylib in a malloc buffer and the ordinary
 * loader maps that buffer.  Every property asserted here is one some other
 * piece of ocerz keys on, so each is read back through the reader that keys on
 * it rather than through a copy written for the test: the export trie is
 * walked with ocerz_dyld_trie_resolve, the exact function import resolution
 * calls, and the twelve bytes behind each export are read with ocerz_decode,
 * the same decoder the interpreter and the JIT will see them through.  A
 * hand-rolled walker here could agree with an image the real one gets wrong.
 *
 * The trie is the case worth having.  Its reader matches a child edge by
 * prefix, takes the first match and never backtracks, so an image that files
 * every export as one flat level of children loses _writev the moment _write
 * is listed ahead of it: the reader takes the _write edge, arrives at a leaf
 * with a "v" still to spend, finds no children and gives up.  Order the two
 * the other way and both work, which is exactly what makes it the kind of bug
 * that survives a casual test.  The export list carries five such
 * proper-prefix pairs on purpose; each is asserted to resolve at both lengths
 * and to reach two different stubs, so a reader that loses the longer name and
 * one that aliases it to the shorter are both caught.  A sixth pair,
 * ___stack_chk_fail and ___stack_chk_guard, is no prefix of the other but
 * shares thirteen characters across the two kinds of export, a stub and a data
 * slot.  _wri, _writ, _memcp, _writev2, _strlength, ___stack_chk_ and
 * ___stack_chk_guards are asserted to reach nothing at all.
 *
 * A resolved address is header-relative: the trie stores offsets from the mach
 * header, and __TEXT carries that header because its fileoff is zero, so a
 * stub's offset in the buffer is its resolved address minus whatever load base
 * the caller passed - here an arbitrary one, since nothing is mapped.  The
 * jump's displacement is followed the same way into __DATA, where the slot has
 * to hold OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF: the one address inside the trap
 * window that turns a guest call to a system function into a bridge dispatch,
 * and the reason a synthesized image needs no fixups at all.
 *
 * Both of __TEXT's protection words are asserted to be exactly read and
 * execute and not write, and both are asserted rather than one, because
 * protect_ro_segments tests those three bits in the word at segment offset 56
 * - maxprot, whatever its local there is called - and silently leaves the
 * stubs mapped writable if any of them is off.  An image that set only the
 * word the segment structure calls initprot would pass a reading of that code
 * and still not be re-protected.
 *
 * ___stack_chk_guard is the one export that is not a function, and every
 * stack-protected guest reads it.  It has to resolve through the same trie to
 * an 8-byte slot inside __DATA's file range and vm size, outside __TEXT, past
 * every jump slot the stubs in __text read, on an 8-byte boundary, and ahead of
 * the trie, which sits past __DATA's file range.  The value there has to be
 * non-zero with its lowest byte zero.  The guaranteed functions are still
 * checked stub by stub exactly as before, ___stack_chk_fail among them, all of
 * them are counted, and __text still has room for at least that many stubs, so
 * a data export that displaced a function or took a stub of its own is caught.
 *
 * A second image is built in the same process, and the test asserts the guard
 * values of the two DIFFER.  The guard is drawn per build, so the only way two
 * builds agree is a constant written into the source or a 56-bit collision,
 * and a test that fails once in 2^56 runs is a price worth one line of proof
 * that the value is not baked in.  Everything else in the two buffers is
 * asserted byte-identical, so the second build re-interned the same export ids
 * and laid out the same slot, and each image carries its guard in its own slot.
 *
 * Both libraries are built from the committed API database, runtime/apis,
 * unless the environment already names another, since make runs the unit
 * binaries from the repository root.  The names every program linked against
 * native mode could bind before the database existed are written out here, the
 * whole of libSystem's list and the whole of CoreFoundation's, and every one of
 * them has to still resolve: the database may grow, but a name dropped from it
 * would turn a program that ran into one that stops with 71.  Beyond those,
 * every function record of both files - fn, special and stub alike - has to
 * resolve to exactly 41 bb <id> ff 25 <rel32> with int3 padding, move with the
 * load base, jump through a slot holding the bridge trap address, and carry an
 * id that ocerz_vdylib_export_name turns back into the file's install name and
 * that same symbol - which is also what proves no two names share an id or a
 * stub, and that CoreFoundation's ids did not collide with libSystem's in the
 * one id space the dispatcher reads.  The id itself is asserted to be the
 * library's position among the database's files shifted into the top twelve
 * bits plus the record's index in its file, and every var record has to
 * resolve to its slot in __DATA.  __text and __data are asserted to be exactly
 * one stub and one slot per function record of CoreFoundation's file, so a
 * native export that took either is caught.  No libSystem name resolves in
 * CoreFoundation and no CoreFoundation name resolves in libSystem, and names a
 * character shorter or longer than CoreFoundation's own reach nothing.
 *
 * ocerz_vdylib_xmm_contract is asserted for libSystem exports of each kind the
 * JIT treats differently: __tlv_bootstrap and ___chkstk_darwin have no contract,
 * since their callers keep every register, strlen reads and writes no xmm
 * register, strtod writes xmm0, printf's special reads all eight argument
 * registers and writes two, scanf's stub record has no contract, and neither has
 * an id no library minted.
 *
 * Every data record has to resolve to exactly what dlsym answers for its host
 * symbol in a CoreFoundation this process dlopens itself, and to the same value
 * at two different load bases: a terminal written without the absolute flag
 * would come back shifted by the base, and the second base is what catches it.
 *
 * The host's CoreFoundation is normally mapped below 2^35, so it cannot show
 * that a wider address survives, and it resolves every name, so it cannot show
 * what happens to one that does not.  ocerz_vdylib_image_with
 * builds the same image against a lookup the test supplies instead.  The first
 * answers every data record with a value five ULEB bytes cannot hold - 2^35 exactly,
 * bit 63 alone, all 64 bits set and others past 2^35 - and each has to come back
 * exact at both load bases.  That image has to be the same length as the real
 * one, with the same trie offset and size, and its stubs and __DATA the same
 * bytes, so a terminal's width is shown not to depend on its value and a native
 * export not to occupy a segment.  The second lookup answers nothing for every
 * third name, those names have to be absent from the trie rather than exported
 * as zero while every other export still resolves, and two builds against it
 * are captured with ocerz_verbose raised: each missing name has to be logged
 * exactly once across both, and no present name at all.
 *
 * Before any of that, a forked child builds CoreFoundation and only then
 * libSystem, and sends libSystem's bytes back with the guard slot zeroed.  A
 * program that links CoreFoundation names it ahead of libSystem, so that is the
 * order the loader really builds them in, and the parent's libSystem, built
 * first, has to match it byte for byte: ids minted in build order would
 * renumber every libSystem stub.  The child uses the synthetic lookup, so it
 * never loads CoreFoundation into a forked process.
 *
 * And before that, a second child points OCERZ_APIDB at a database of its own
 * holding a synthesized library of 6000 records - fn, special, stub, data and
 * var mixed, names that nest as _synth_1, _synth_10, _synth_100 and
 * _synth_1000 do, and names of several hundred characters - beside a small
 * second library whose file sorts after it.  It builds the small one first.
 * Every one of the 6000 exports has to resolve as its kind requires, the stubs
 * through the same byte-level check as above, the data at two load bases and
 * the missing data not at all, the var slots inside __DATA and filled, and
 * __text and __data have to span many pages; the ids of both libraries have to
 * follow the files' order in the directory rather than the order they were
 * built in.  The child sends its counts back through a pipe, because the
 * database a process reads is chosen once.
 *
 * ocerz_dyld_trie_each, which dladdr uses to name an address inside a
 * synthesized image, is walked over both real images and held to the resolver:
 * it has to visit exactly the names ocerz_dyld_trie_resolve finds among the
 * database's records, each once, each at the value the resolver answers at the
 * same load base, absolute terminals unshifted.  A visitor that asks to stop is
 * obeyed at once, and a trie whose declared size ends inside its root node is
 * refused with -1 rather than walked past its end.
 */
#include "ocerz/vdylib.h"
#include "ocerz/apidb.h"
#include "ocerz/bridge.h"
#include "ocerz/dyld.h"
#include "ocerz/dyldapi.h"
#include "ocerz/decode.h"
#include "ocerz/cpu.h"

#include <mach-o/loader.h>
#include <mach/machine.h>

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOAD_BASE       0x0000000210000000ull
#define LOAD_BASE_ALT   0x0000000c47000000ull
#define MOV_LEN         6
#define JMP_LEN         6
#define STUB_LEN        (MOV_LEN + JMP_LEN)
#define STUB_STRIDE     16
#define SLOT_LEN        8
#define LC_EXPORTS_TRIE 0x80000033u
#define IMAGE_MAX       (16u * 1024u * 1024u)

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

static const char *const kExports[] = {
    "___bzero", "_memcpy", "_memcmp", "_memmove", "_memset",
    "_strcmp", "_strncmp", "_strcpy", "_strlen",
    "_write", "_writev", "_read", "_readv",
    "_open", "_opendir", "_close", "_closedir",
    "_time", "_times", "_exit", "_malloc", "_free",
    "___stack_chk_fail",
};
#define NEXPORTS (sizeof kExports / sizeof kExports[0])

static const char *const kM8LibSystem[] = {
    "___bzero", "___error", "___stack_chk_fail", "__tlv_bootstrap", "___memcpy_chk",
    "___memmove_chk", "___memset_chk", "___strcpy_chk", "___stpcpy_chk", "___strcat_chk",
    "___strncpy_chk", "___stpncpy_chk", "___strncat_chk", "___strlcpy_chk", "___strlcat_chk",
    "_memcpy", "_memcmp", "_memmove", "_memset", "_memchr", "_strcmp", "_strncmp", "_strcpy",
    "_strncpy", "_strlen", "_strnlen", "_strcat", "_strncat", "_strchr", "_strrchr", "_strstr",
    "_strdup", "_strndup", "_strtol", "_strtoul", "_strtod", "_strerror", "_fopen", "_fclose",
    "_fread", "_fwrite", "_fflush", "_fprintf", "_fputs", "_fputc", "_fgets", "_printf",
    "_puts", "_putchar", "_snprintf", "_sprintf", "_vsnprintf", "_vfprintf", "_perror",
    "_malloc", "_calloc", "_realloc", "_free", "_exit", "_abort", "_atexit", "_getenv",
    "_setenv", "_unsetenv", "_qsort", "_bsearch", "_abs", "_labs", "_atoi", "_atol", "_atof",
    "_rand", "_srand", "_write", "_writev", "_read", "_readv", "_readdir", "_open", "_opendir",
    "_close", "_closedir", "_lseek", "_unlink", "_mkdir", "_rmdir", "_rename", "_access",
    "_dup", "_dup2", "_pipe", "_fcntl", "_ioctl", "_isatty", "_getpid", "_getppid", "_getuid",
    "_geteuid", "_mmap", "_munmap", "_mprotect", "_madvise", "_time", "_times", "_clock",
    "_clock_gettime", "_gettimeofday", "_nanosleep", "_sleep", "_usleep", "_mktime",
    "_localtime", "_gmtime", "_strftime", "_pthread_create", "_pthread_join",
    "_pthread_detach", "_pthread_self", "_pthread_mutex_init", "_pthread_mutex_lock",
    "_pthread_mutex_unlock", "_pthread_mutex_destroy", "_pthread_cond_init",
    "_pthread_cond_wait", "_pthread_cond_signal", "_pthread_cond_broadcast",
    "_pthread_cond_destroy", "_dispatch_get_global_queue", "_dispatch_async_f",
    "_dispatch_sync_f", "_dispatch_apply_f", "_dispatch_semaphore_create",
    "_dispatch_semaphore_wait", "_dispatch_semaphore_signal", "_dispatch_release", "_dlopen",
    "_dlsym", "_dlclose", "_dlerror", "_dladdr", "_signal", "_sigaction", "_raise", "_kill",
    "_sigprocmask", "_pthread_sigmask", "_sigaltstack", "_pthread_kill", "_sigemptyset",
    "_sigfillset", "_sigaddset", "_sigdelset", "_sigismember", "dyld_stub_binder",
    "___stack_chk_guard",
};
#define NM8LIBSYSTEM (sizeof kM8LibSystem / sizeof kM8LibSystem[0])

static const char *const kPrefixPairs[][2] = {
    { "_write", "_writev" },
    { "_read",  "_readv" },
    { "_open",  "_opendir" },
    { "_close", "_closedir" },
    { "_time",  "_times" },
    { "___stack_chk_fail", "___stack_chk_guard" },
};
#define NPAIRS (sizeof kPrefixPairs / sizeof kPrefixPairs[0])

static const char *const kNotExported[] = {
    "_wri", "_writ", "_memcp", "_writev2", "_strlength",
    "___stack_chk_", "___stack_chk_guards",
    "_ocerz_no_such_export_xyzzy",
};
#define NNOT (sizeof kNotExported / sizeof kNotExported[0])

static const char *const kGuard = "___stack_chk_guard";

static const char *const kCF = OCERZ_BRIDGE_COREFOUNDATION;

static const char *const kCFFunctions[] = {
    "_CFRetain",
    "_CFRelease",
    "_CFGetRetainCount",
    "_CFEqual",
    "_CFHash",
    "_CFGetTypeID",
    "_CFStringGetTypeID",
    "_CFArrayGetTypeID",
    "_CFDictionaryGetTypeID",
    "_CFNumberGetTypeID",
    "_CFBooleanGetTypeID",
    "_CFDataGetTypeID",
    "_CFCopyDescription",
    "_CFGetAllocator",
    "_CFStringCreateWithCString",
    "_CFStringCreateWithBytes",
    "_CFStringCreateCopy",
    "_CFStringCreateMutable",
    "_CFStringCreateMutableCopy",
    "_CFStringAppendCString",
    "_CFStringAppend",
    "_CFStringGetLength",
    "_CFStringGetCharacterAtIndex",
    "_CFStringGetCString",
    "_CFStringGetCStringPtr",
    "_CFStringGetMaximumSizeForEncoding",
    "_CFStringCompare",
    "_CFStringHasPrefix",
    "_CFStringHasSuffix",
    "_CFStringGetIntValue",
    "_CFStringGetDoubleValue",
    "_CFStringCreateArrayBySeparatingStrings",
    "_CFStringCreateByCombiningStrings",
    "___CFStringMakeConstantString",
    "_CFArrayCreate",
    "_CFArrayCreateMutable",
    "_CFArrayCreateCopy",
    "_CFArrayCreateMutableCopy",
    "_CFArrayGetCount",
    "_CFArrayGetValueAtIndex",
    "_CFArrayAppendValue",
    "_CFArrayInsertValueAtIndex",
    "_CFArraySetValueAtIndex",
    "_CFArrayRemoveValueAtIndex",
    "_CFArrayRemoveAllValues",
    "_CFDictionaryCreate",
    "_CFDictionaryCreateMutable",
    "_CFDictionaryCreateCopy",
    "_CFDictionaryCreateMutableCopy",
    "_CFDictionaryGetCount",
    "_CFDictionaryGetValue",
    "_CFDictionaryGetValueIfPresent",
    "_CFDictionaryContainsKey",
    "_CFDictionaryAddValue",
    "_CFDictionarySetValue",
    "_CFDictionaryRemoveValue",
    "_CFDictionaryGetKeysAndValues",
    "_CFDictionaryApplyFunction",
    "_CFNumberCreate",
    "_CFNumberGetValue",
    "_CFNumberGetType",
    "_CFNumberCompare",
    "_CFBooleanGetValue",
    "_CFDataCreate",
    "_CFDataGetLength",
    "_CFDataGetBytePtr",
    "_CFAbsoluteTimeGetCurrent",
    "_CFRunLoopGetCurrent",
    "_CFRunLoopGetMain",
    "_CFRunLoopRun",
    "_CFRunLoopRunInMode",
    "_CFRunLoopStop",
    "_CFRunLoopWakeUp",
    "_CFRunLoopAddTimer",
    "_CFRunLoopRemoveTimer",
    "_CFRunLoopTimerCreate",
    "_CFRunLoopTimerInvalidate",
    "_CFRunLoopTimerIsValid",
    "_CFRunLoopTimerGetNextFireDate",
    "_CFRunLoopTimerSetNextFireDate",
    "_CFRunLoopObserverCreate",
    "_CFRunLoopAddObserver",
    "_CFRunLoopRemoveObserver",
    "_CFRunLoopObserverInvalidate",
    "_CFRunLoopSourceCreate",
    "_CFRunLoopAddSource",
    "_CFRunLoopRemoveSource",
    "_CFRunLoopSourceSignal",
    "_CFRunLoopSourceInvalidate",
};
#define NCFFUNCS (sizeof kCFFunctions / sizeof kCFFunctions[0])

static const char *const kCFNatives[] = {
    "___CFConstantStringClassReference",
    "_kCFAllocatorDefault",
    "_kCFAllocatorSystemDefault",
    "_kCFAllocatorMalloc",
    "_kCFAllocatorNull",
    "_kCFTypeArrayCallBacks",
    "_kCFTypeDictionaryKeyCallBacks",
    "_kCFTypeDictionaryValueCallBacks",
    "_kCFCopyStringDictionaryKeyCallBacks",
    "_kCFBooleanTrue",
    "_kCFBooleanFalse",
    "_kCFNull",
    "_kCFRunLoopDefaultMode",
    "_kCFRunLoopCommonModes",
    "_kCFNumberPositiveInfinity",
    "_kCFNumberNegativeInfinity",
    "_kCFNumberNaN",
};
#define NCFNATIVES (sizeof kCFNatives / sizeof kCFNatives[0])

static const char *const kCFNotExported[] = {
    "_CFRetai", "_CFRetainX", "_CFArrayCreateMutableCop", "_CFRunLoopRu",
    "_CFRunLoopRunInModes", "_kCFBoolean", "_kCFBooleanTru",
    "___CFConstantStringClassReferences", "___CF", "_CF", "_kCF",
};
#define NCFNOT (sizeof kCFNotExported / sizeof kCFNotExported[0])

static const uint64_t kFakeHost[] = {
    0x0000000800000000ull,
    0x00000007ffffffffull,
    0x0000000800000008ull,
    0x00007ffffffff000ull,
    0x8000000000000000ull,
    0xffffffffffffffffull,
    0x0123456789abcdefull,
    0xfedcba9876543210ull,
};
#define NFAKE (sizeof kFakeHost / sizeof kFakeHost[0])

typedef struct {
    int found;
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    int sect_found;
    uint64_t sect_addr;
    uint64_t sect_size;
} SegInfo;

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void seg_from(const uint8_t *lc, uint32_t csize, SegInfo *s)
{
    struct segment_command_64 sc;
    memcpy(&sc, lc, sizeof sc);
    s->found = 1;
    s->vmaddr = sc.vmaddr;
    s->vmsize = sc.vmsize;
    s->fileoff = sc.fileoff;
    s->filesize = sc.filesize;
    s->maxprot = (uint32_t)sc.maxprot;
    s->initprot = (uint32_t)sc.initprot;
    if (sc.nsects >= 1 && csize >= sizeof sc + sizeof(struct section_64)) {
        struct section_64 sect;
        memcpy(&sect, lc + sizeof sc, sizeof sect);
        s->sect_found = 1;
        s->sect_addr = sect.addr;
        s->sect_size = sect.size;
    }
}

static int guard_shape_ok(uint64_t v)
{
    return v != 0 && (v & 0xff) == 0;
}

static void check_distinct(const char *what, const uint64_t *v, const int *ok, size_t n)
{
    size_t bad_i = n, bad_j = n;
    char msg[192];

    for (size_t i = 0; i < n && bad_i == n; i++) {
        if (!ok[i])
            continue;
        for (size_t j = i + 1; j < n; j++) {
            if (ok[j] && v[i] == v[j]) {
                bad_i = i;
                bad_j = j;
                break;
            }
        }
    }
    if (bad_i == n)
        snprintf(msg, sizeof msg, "every export has its own %s", what);
    else
        snprintf(msg, sizeof msg, "%s and %s share a %s (%#llx)",
                 kExports[bad_i], kExports[bad_j], what,
                 (unsigned long long)v[bad_i]);
    CHECK(bad_i == n, "%s", msg);
}

static int report(void)
{
    printf("test_vdylib: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

static void check_libsystem(void)
{
    CHECK(ocerz_vdylib_have(kLib) != 0,
          "ocerz_vdylib_have(\"%s\") says no", kLib);
    CHECK(ocerz_vdylib_have("/usr/lib/libNoSuchLibrary.dylib") == 0,
          "ocerz_vdylib_have claims a library nobody provides");
    CHECK(ocerz_vdylib_have("") == 0,
          "ocerz_vdylib_have claims the empty install name");

    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);
    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);
    if (!img)
        return;

    CHECK(len > sizeof(struct mach_header_64),
          "image is %zu bytes, too small to hold a mach header", len);
    CHECK(len < IMAGE_MAX, "image is %zu bytes, which is implausible", len);
    if (len <= sizeof(struct mach_header_64) || len >= IMAGE_MAX) {
        free(img);
        return;
    }

    struct mach_header_64 mh;
    memcpy(&mh, img, sizeof mh);
    CHECK(mh.magic == MH_MAGIC_64, "magic is %#x, want MH_MAGIC_64", mh.magic);
    CHECK(mh.cputype == CPU_TYPE_X86_64,
          "cputype is %d, want CPU_TYPE_X86_64", (int)mh.cputype);
    CHECK(mh.filetype == MH_DYLIB, "filetype is %u, want MH_DYLIB", mh.filetype);
    CHECK(mh.ncmds > 0, "ncmds is zero");
    CHECK(mh.sizeofcmds > 0, "sizeofcmds is zero");
    CHECK((uint64_t)sizeof mh + mh.sizeofcmds <= (uint64_t)len,
          "header plus sizeofcmds %u runs past the %zu byte image",
          mh.sizeofcmds, len);
    if (mh.magic != MH_MAGIC_64 || mh.ncmds == 0 ||
        (uint64_t)sizeof mh + mh.sizeofcmds > (uint64_t)len) {
        free(img);
        return;
    }

    SegInfo text, data;
    memset(&text, 0, sizeof text);
    memset(&data, 0, sizeof data);
    uint32_t trie_off = 0, trie_size = 0;
    int trie_found = 0, id_found = 0, id_named = 0, malformed = 0;
    uint32_t walked = 0, seen = 0;
    const uint8_t *lc = img + sizeof mh;

    for (uint32_t i = 0; i < mh.ncmds; i++) {
        if ((uint64_t)walked + 8 > mh.sizeofcmds) {
            malformed = 1;
            break;
        }
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (csize < 8 || (csize & 7) != 0 ||
            (uint64_t)walked + csize > mh.sizeofcmds) {
            malformed = 1;
            break;
        }
        if (cmd == LC_SEGMENT_64 && csize >= sizeof(struct segment_command_64)) {
            if (memcmp(lc + 8, "__TEXT", 7) == 0)
                seg_from(lc, csize, &text);
            else if (memcmp(lc + 8, "__DATA", 7) == 0)
                seg_from(lc, csize, &data);
        } else if (cmd == LC_EXPORTS_TRIE && csize >= 16) {
            trie_found = 1;
            trie_off = rd32(lc + 8);
            trie_size = rd32(lc + 12);
        } else if (cmd == LC_ID_DYLIB && csize >= 24) {
            uint32_t noff = rd32(lc + 8);
            id_found = 1;
            if (noff >= 24 && noff < csize) {
                const char *nm = (const char *)(lc + noff);
                size_t room = csize - noff;
                id_named = strnlen(nm, room) < room && strcmp(nm, kLib) == 0;
            }
        }
        lc += csize;
        walked += csize;
        seen++;
    }

    CHECK(!malformed, "load command %u is malformed or runs past sizeofcmds", seen);
    CHECK(seen == mh.ncmds,
          "walked %u load commands, ncmds says %u", seen, mh.ncmds);
    CHECK(walked == mh.sizeofcmds,
          "load commands span %u bytes, sizeofcmds says %u", walked, mh.sizeofcmds);

    CHECK(id_found, "no LC_ID_DYLIB");
    CHECK(id_named, "LC_ID_DYLIB does not name \"%s\"", kLib);

    CHECK(trie_found, "no LC_DYLD_EXPORTS_TRIE");
    CHECK(trie_size != 0, "LC_DYLD_EXPORTS_TRIE has a zero datasize");
    CHECK((uint64_t)trie_off + trie_size <= (uint64_t)len,
          "the export trie [%u,%llu) is not inside the %zu byte image",
          trie_off, (unsigned long long)trie_off + trie_size, len);

    CHECK(text.found, "no __TEXT segment");
    CHECK(data.found, "no __DATA segment");
    if (!text.found || !data.found) {
        free(img);
        return;
    }

    CHECK(text.fileoff == 0,
          "__TEXT fileoff is %llu, want 0", (unsigned long long)text.fileoff);
    CHECK(text.filesize != 0, "__TEXT filesize is zero");
    CHECK(text.vmsize != 0, "__TEXT vmsize is zero");
    CHECK(text.fileoff + text.filesize <= (uint64_t)len,
          "__TEXT file range ends at %llu, past the %zu byte image",
          (unsigned long long)(text.fileoff + text.filesize), len);
    CHECK((text.initprot & VM_PROT_READ) != 0,
          "__TEXT initprot %#x is not readable", text.initprot);
    CHECK((text.initprot & VM_PROT_EXECUTE) != 0,
          "__TEXT initprot %#x is not executable", text.initprot);
    CHECK((text.initprot & VM_PROT_WRITE) == 0,
          "__TEXT initprot %#x is writable", text.initprot);
    CHECK((text.maxprot & VM_PROT_READ) != 0,
          "__TEXT maxprot %#x is not readable", text.maxprot);
    CHECK((text.maxprot & VM_PROT_EXECUTE) != 0,
          "__TEXT maxprot %#x is not executable, so protect_ro_segments leaves the stubs writable",
          text.maxprot);
    CHECK((text.maxprot & VM_PROT_WRITE) == 0,
          "__TEXT maxprot %#x is writable, so protect_ro_segments leaves the stubs writable",
          text.maxprot);

    CHECK(data.filesize != 0,
          "__DATA filesize is zero, so the bridge slots are not in the image");
    CHECK(data.fileoff + data.filesize <= (uint64_t)len,
          "__DATA file range ends at %llu, past the %zu byte image",
          (unsigned long long)(data.fileoff + data.filesize), len);
    CHECK(data.vmaddr >= text.vmaddr,
          "__DATA vmaddr %#llx is below __TEXT vmaddr %#llx",
          (unsigned long long)data.vmaddr, (unsigned long long)text.vmaddr);
    if (data.vmaddr < text.vmaddr || data.filesize == 0) {
        free(img);
        return;
    }

    const uint64_t text_lo = LOAD_BASE;
    const uint64_t text_hi = LOAD_BASE + text.vmsize;
    const uint64_t data_lo = LOAD_BASE + (data.vmaddr - text.vmaddr);
    const uint64_t data_hi = data_lo + data.filesize;
    const uint64_t want_slot = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;

    uint64_t addr[NEXPORTS], ids[NEXPORTS], slots[NEXPORTS];
    int ok[NEXPORTS];

    for (size_t i = 0; i < NEXPORTS; i++) {
        const char *sym = kExports[i];
        addr[i] = ids[i] = slots[i] = 0;
        ok[i] = 0;

        int found = 0;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
        CHECK(found, "%s does not resolve through the export trie", sym);
        if (!found)
            continue;

        int in_text = a >= text_lo && a + STUB_LEN <= text_hi;
        CHECK(in_text,
              "%s resolved to %#llx, outside the mapped __TEXT [%#llx,%#llx)",
              sym, (unsigned long long)a, (unsigned long long)text_lo,
              (unsigned long long)text_hi);
        if (!in_text)
            continue;
        addr[i] = a;

        uint64_t foff = text.fileoff + (a - text_lo);
        CHECK(foff + STUB_LEN <= (uint64_t)len,
              "%s stub at offset %llu runs past the %zu byte image",
              sym, (unsigned long long)foff, len);
        if (foff + STUB_LEN > (uint64_t)len)
            continue;

        X86Insn mov;
        memset(&mov, 0, sizeof mov);
        int r = ocerz_decode(img + foff, len - (size_t)foff, a, &mov);
        CHECK(r == OCERZ_OK,
              "%s stub: the first instruction does not decode (%d)", sym, r);
        if (r != OCERZ_OK)
            continue;
        CHECK(mov.op == OCERZ_OP_MOV,
              "%s stub: the first instruction is %s, want MOV",
              sym, ocerz_op_name(mov.op));
        CHECK(mov.len == MOV_LEN,
              "%s stub: the first instruction is %u bytes, want %d",
              sym, mov.len, MOV_LEN);
        CHECK(mov.nops == 2, "%s stub: the first instruction has %u operands, want 2",
              sym, mov.nops);
        CHECK(mov.ops[0].kind == OCERZ_OPK_REG && mov.ops[0].reg == OCERZ_R11 &&
              mov.ops[0].size == 4,
              "%s stub: the first instruction does not write r11d", sym);
        CHECK(mov.ops[1].kind == OCERZ_OPK_IMM,
              "%s stub: the first instruction's source is not an immediate", sym);
        ids[i] = mov.ops[1].imm;

        X86Insn jmp;
        memset(&jmp, 0, sizeof jmp);
        r = ocerz_decode(img + foff + MOV_LEN, len - (size_t)foff - MOV_LEN,
                         a + MOV_LEN, &jmp);
        CHECK(r == OCERZ_OK,
              "%s stub: the second instruction does not decode (%d)", sym, r);
        if (r != OCERZ_OK)
            continue;
        CHECK(jmp.op == OCERZ_OP_JMP,
              "%s stub: the second instruction is %s, want JMP",
              sym, ocerz_op_name(jmp.op));
        CHECK(jmp.len == JMP_LEN,
              "%s stub: the second instruction is %u bytes, want %d",
              sym, jmp.len, JMP_LEN);
        CHECK(jmp.opsize == 8,
              "%s stub: the jump reads %u bytes, want 8", sym, jmp.opsize);
        CHECK(jmp.nops == 1 && jmp.ops[0].kind == OCERZ_OPK_MEM,
              "%s stub: the jump is not through memory", sym);
        CHECK(jmp.ops[0].riprel == 1,
              "%s stub: the jump's address is not rip-relative", sym);
        CHECK(jmp.ops[0].base == OCERZ_REG_NONE &&
              jmp.ops[0].index == OCERZ_REG_NONE,
              "%s stub: the jump's address carries a register term", sym);
        if (jmp.nops != 1 || jmp.ops[0].kind != OCERZ_OPK_MEM ||
            jmp.ops[0].riprel != 1)
            continue;

        uint64_t slot = (uint64_t)jmp.ops[0].disp;
        int in_data = slot >= data_lo && slot + 8 <= data_hi;
        CHECK(in_data,
              "%s stub: the jump reads %#llx, outside the mapped __DATA [%#llx,%#llx)",
              sym, (unsigned long long)slot, (unsigned long long)data_lo,
              (unsigned long long)data_hi);
        if (!in_data)
            continue;
        slots[i] = slot;
        ok[i] = 1;

        uint64_t soff = data.fileoff + (slot - data_lo);
        CHECK(soff + 8 <= (uint64_t)len,
              "%s stub: its __DATA slot at offset %llu is past the %zu byte image",
              sym, (unsigned long long)soff, len);
        if (soff + 8 > (uint64_t)len)
            continue;

        uint64_t got = rd64(img + soff);
        CHECK(got == want_slot,
              "%s stub: its __DATA slot holds %#llx, want %#llx",
              sym, (unsigned long long)got, (unsigned long long)want_slot);
    }

    check_distinct("stub address", addr, ok, NEXPORTS);
    check_distinct("export id", ids, ok, NEXPORTS);
    check_distinct("__DATA slot", slots, ok, NEXPORTS);

    for (size_t i = 0; i < NPAIRS; i++) {
        const char *shorter = kPrefixPairs[i][0];
        const char *longer = kPrefixPairs[i][1];
        int fs = 0, fl = 0;
        uint64_t as = ocerz_dyld_trie_resolve(img, LOAD_BASE, shorter, &fs);
        uint64_t al = ocerz_dyld_trie_resolve(img, LOAD_BASE, longer, &fl);

        CHECK(fs && fl, "%s / %s: one of the pair does not resolve", shorter, longer);
        CHECK(!(fs && fl) || as != al,
              "%s and %s both resolve to %#llx: the trie matched the shorter name by prefix",
              shorter, longer, (unsigned long long)as);
    }

    int stubs_ok = 0;
    for (size_t i = 0; i < NEXPORTS; i++)
        stubs_ok += ok[i];
    CHECK(stubs_ok == (int)NEXPORTS,
          "only %d of the %zu guaranteed function exports reach a stub that jumps through __DATA",
          stubs_ok, NEXPORTS);

    CHECK(text.sect_found, "__TEXT carries no section, so the stubs cannot be counted");
    CHECK(data.sect_found, "__DATA carries no section, so the jump slots cannot be located");
    if (!text.sect_found || !data.sect_found) {
        free(img);
        return;
    }
    CHECK(text.sect_size % STUB_STRIDE == 0,
          "__text is %llu bytes, not a whole number of %d-byte stubs",
          (unsigned long long)text.sect_size, STUB_STRIDE);
    const uint64_t nstubs = text.sect_size / STUB_STRIDE;
    CHECK(nstubs >= NEXPORTS,
          "__text holds %llu stubs, fewer than the %zu guaranteed function exports",
          (unsigned long long)nstubs, NEXPORTS);

    const uint64_t jslots_lo = LOAD_BASE + (data.sect_addr - text.vmaddr);
    const uint64_t jslots_hi = jslots_lo + nstubs * SLOT_LEN;
    int jslots_in = 0;
    for (size_t i = 0; i < NEXPORTS; i++)
        if (ok[i] && slots[i] >= jslots_lo && slots[i] + SLOT_LEN <= jslots_hi)
            jslots_in++;
    CHECK(jslots_in == stubs_ok,
          "only %d of %d stubs read a slot inside the %llu jump slots at [%#llx,%#llx)",
          jslots_in, stubs_ok, (unsigned long long)nstubs,
          (unsigned long long)jslots_lo, (unsigned long long)jslots_hi);

    int gfound = 0;
    uint64_t guard = ocerz_dyld_trie_resolve(img, LOAD_BASE, kGuard, &gfound);
    uint64_t guard_val = 0;
    uint64_t guard_off = 0;
    int guard_ok = 0;
    CHECK(gfound, "%s does not resolve through the export trie", kGuard);
    if (gfound) {
        const uint64_t data_vm_hi = data_lo + data.vmsize;
        CHECK(guard + SLOT_LEN <= text_lo || guard >= text_hi,
              "%s resolved to %#llx, inside __TEXT [%#llx,%#llx)",
              kGuard, (unsigned long long)guard, (unsigned long long)text_lo,
              (unsigned long long)text_hi);
        CHECK(guard >= data_lo && guard + SLOT_LEN <= data_vm_hi,
              "%s resolved to %#llx, outside __DATA's vm range [%#llx,%#llx)",
              kGuard, (unsigned long long)guard, (unsigned long long)data_lo,
              (unsigned long long)data_vm_hi);
        CHECK(guard >= data_lo && guard + SLOT_LEN <= data_hi,
              "%s resolved to %#llx, outside __DATA's file range [%#llx,%#llx)",
              kGuard, (unsigned long long)guard, (unsigned long long)data_lo,
              (unsigned long long)data_hi);
        CHECK(guard >= jslots_hi,
              "%s resolved to %#llx, below the end of the jump slots at %#llx",
              kGuard, (unsigned long long)guard, (unsigned long long)jslots_hi);
        CHECK(((guard - LOAD_BASE) & (SLOT_LEN - 1)) == 0,
              "%s resolved to %#llx, which is not on an 8-byte boundary",
              kGuard, (unsigned long long)guard);
        CHECK((uint64_t)trie_off >= data.fileoff + data.filesize,
              "the export trie at %u starts inside __DATA's file range, which ends at %llu",
              trie_off, (unsigned long long)(data.fileoff + data.filesize));

        if (guard >= data_lo && guard + SLOT_LEN <= data_hi &&
            guard + SLOT_LEN <= data_vm_hi) {
            guard_off = data.fileoff + (guard - data_lo);
            CHECK(guard_off + SLOT_LEN <= (uint64_t)len,
                  "%s's slot at offset %llu runs past the %zu byte image",
                  kGuard, (unsigned long long)guard_off, len);
            if (guard_off + SLOT_LEN <= (uint64_t)len) {
                guard_val = rd64(img + guard_off);
                int has_zero = 0;
                for (int b = 0; b < SLOT_LEN; b++)
                    has_zero |= ((guard_val >> (8 * b)) & 0xff) == 0;
                CHECK(guard_val != 0, "%s holds zero", kGuard);
                CHECK(has_zero, "%s has no zero byte", kGuard);
                CHECK((guard_val & 0xff) == 0,
                      "%s's lowest byte is not zero, so a string copy can reproduce it", kGuard);
                guard_ok = guard_shape_ok(guard_val);
            }
        }
    }

    size_t len2 = 0;
    uint8_t *img2 = ocerz_vdylib_image(kLib, &len2);
    CHECK(img2 != NULL, "a second ocerz_vdylib_image(\"%s\") returned no buffer", kLib);
    if (img2) {
        CHECK(len2 == len, "the second image is %zu bytes, the first %zu", len2, len);
        int g2found = 0;
        uint64_t guard2 = ocerz_dyld_trie_resolve(img2, LOAD_BASE, kGuard, &g2found);
        CHECK(g2found && guard2 == guard,
              "%s resolves to %#llx in the second image and %#llx in the first",
              kGuard, (unsigned long long)guard2, (unsigned long long)guard);
        if (guard_ok && len2 == len && g2found && guard2 == guard) {
            uint64_t v2 = rd64(img2 + guard_off);
            CHECK(guard_shape_ok(v2),
                  "the second image's %s is zero or has a non-zero lowest byte", kGuard);
            CHECK(v2 != guard_val,
                  "both images carry the same %s, so it is not drawn per build", kGuard);
            size_t tail = (size_t)guard_off + SLOT_LEN;
            CHECK(memcmp(img, img2, (size_t)guard_off) == 0 &&
                  memcmp(img + tail, img2 + tail, len - tail) == 0,
                  "the two images differ outside %s's slot", kGuard);
        }
        free(img2);
    }

    for (size_t i = 0; i < NNOT; i++) {
        int found = 1;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, kNotExported[i], &found);
        CHECK(!found, "%s is not an export but resolved to %#llx",
              kNotExported[i], (unsigned long long)a);
        CHECK(found || a == 0, "%s reported not-found but returned %#llx",
              kNotExported[i], (unsigned long long)a);
    }

    {
        int tfound = 0;
        uint64_t ta = ocerz_dyld_trie_resolve(img, LOAD_BASE, "__tlv_bootstrap", &tfound);
        CHECK(tfound, "__tlv_bootstrap does not resolve through the export trie");
        int tin = tfound && ta >= text_lo && ta + 16 <= text_hi;
        CHECK(!tfound || tin, "__tlv_bootstrap resolved to %#llx, outside the mapped __TEXT",
              (unsigned long long)ta);
        if (tin) {
            const uint8_t *t = img + text.fileoff + (ta - text_lo);
            CHECK(t[0] == 0x41 && t[1] == 0x53,
                  "__tlv_bootstrap stub does not start with push r11 (41 53), so a thread-local"
                  " access would destroy a value the compiler keeps in r11");
            CHECK(t[2] == 0x41 && t[3] == 0xbb, "__tlv_bootstrap stub does not load its id with mov r11d");
            CHECK(t[8] == 0xff && t[9] == 0x25, "__tlv_bootstrap stub does not end with jmp [rip+disp]");
            int32_t disp = (int32_t)((uint32_t)t[10] | (uint32_t)t[11] << 8 |
                                     (uint32_t)t[12] << 16 | (uint32_t)t[13] << 24);
            uint64_t slot = ta + 14 + (uint64_t)(int64_t)disp;
            int sin = slot >= data_lo && slot + 8 <= data_hi;
            CHECK(sin, "__tlv_bootstrap stub jumps through %#llx, outside __DATA", (unsigned long long)slot);
            if (sin) {
                uint64_t v = 0;
                memcpy(&v, img + data.fileoff + (slot - data_lo), 8);
                CHECK(v == want_slot, "__tlv_bootstrap slot holds %#llx, want the trap address %#llx",
                      (unsigned long long)v, (unsigned long long)want_slot);
            }
        }
    }

    free(img);
}

typedef struct {
    SegInfo text;
    SegInfo data;
    uint32_t trie_off;
    uint32_t trie_size;
} Layout;

static int layout_of(const char *what, const uint8_t *img, size_t len, const char *install,
                     Layout *ly)
{
    memset(ly, 0, sizeof *ly);
    struct mach_header_64 mh;
    if (len <= sizeof mh) {
        CHECK(0, "%s: image is %zu bytes, too small to hold a mach header", what, len);
        return 0;
    }
    memcpy(&mh, img, sizeof mh);
    int shape = mh.magic == MH_MAGIC_64 && mh.cputype == CPU_TYPE_X86_64 &&
                mh.filetype == MH_DYLIB && mh.ncmds > 0 &&
                (uint64_t)sizeof mh + mh.sizeofcmds <= (uint64_t)len;
    CHECK(shape, "%s: not an x86_64 MH_DYLIB whose load commands fit the image", what);
    if (!shape)
        return 0;

    int trie_found = 0, id_named = 0, malformed = 0;
    uint32_t walked = 0;
    const uint8_t *lc = img + sizeof mh;
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (csize < 8 || (uint64_t)walked + csize > mh.sizeofcmds) {
            malformed = 1;
            break;
        }
        if (cmd == LC_SEGMENT_64 && csize >= sizeof(struct segment_command_64)) {
            if (memcmp(lc + 8, "__TEXT", 7) == 0)
                seg_from(lc, csize, &ly->text);
            else if (memcmp(lc + 8, "__DATA", 7) == 0)
                seg_from(lc, csize, &ly->data);
        } else if (cmd == LC_EXPORTS_TRIE && csize >= 16) {
            trie_found = 1;
            ly->trie_off = rd32(lc + 8);
            ly->trie_size = rd32(lc + 12);
        } else if (cmd == LC_ID_DYLIB && csize >= 24) {
            uint32_t noff = rd32(lc + 8);
            if (noff >= 24 && noff < csize) {
                const char *nm = (const char *)(lc + noff);
                size_t room = csize - noff;
                id_named = strnlen(nm, room) < room && strcmp(nm, install) == 0;
            }
        }
        lc += csize;
        walked += csize;
    }
    CHECK(!malformed, "%s: a load command is malformed", what);
    CHECK(id_named, "%s: LC_ID_DYLIB does not name \"%s\"", what, install);
    int placed = trie_found && ly->trie_size != 0 &&
                 (uint64_t)ly->trie_off + ly->trie_size <= (uint64_t)len;
    CHECK(placed, "%s: no export trie inside the image", what);
    int segs = ly->text.found && ly->data.found && ly->text.fileoff == 0 &&
               ly->text.vmaddr == 0 && ly->data.filesize != 0 &&
               ly->data.fileoff + ly->data.filesize <= (uint64_t)len &&
               ly->data.vmaddr == ly->data.fileoff &&
               (uint64_t)ly->trie_off >= ly->data.fileoff + ly->data.filesize;
    CHECK(segs, "%s: __TEXT and __DATA are not laid out file-equals-memory ahead of the trie",
          what);
    return !malformed && id_named && placed && segs;
}

static void check_function_stubs(const char *what, const uint8_t *img, size_t len,
                                 const Layout *ly, const char *lib,
                                 const char *const *names, size_t n)
{
    const uint64_t want_slot = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;
    size_t good = 0;

    for (size_t i = 0; i < n; i++) {
        const char *sym = names[i];
        int found = 0, found_alt = 0;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
        uint64_t a_alt = ocerz_dyld_trie_resolve(img, LOAD_BASE_ALT, sym, &found_alt);
        CHECK(found && found_alt, "%s: %s does not resolve through the export trie", what, sym);
        if (!found || !found_alt)
            continue;
        CHECK(a_alt - a == LOAD_BASE_ALT - LOAD_BASE,
              "%s: %s resolves to %#llx at one load base and %#llx at another, "
              "so its stub does not move with the image",
              what, sym, (unsigned long long)a, (unsigned long long)a_alt);

        uint64_t off = a - LOAD_BASE;
        int in_text = a >= LOAD_BASE && off + STUB_STRIDE <= ly->text.filesize &&
                      off + STUB_STRIDE <= (uint64_t)len;
        CHECK(in_text, "%s: %s resolved to offset %#llx, outside __TEXT", what, sym,
              (unsigned long long)off);
        if (!in_text)
            continue;

        const uint8_t *st = img + off;
        int shape = st[0] == 0x41 && st[1] == 0xbb && st[6] == 0xff && st[7] == 0x25 &&
                    st[12] == 0xcc && st[13] == 0xcc && st[14] == 0xcc && st[15] == 0xcc;
        CHECK(shape, "%s: %s stub is %02x %02x .. %02x %02x, want 41 bb <id> ff 25 <rel32> "
              "and int3 padding", what, sym, st[0], st[1], st[6], st[7]);
        if (!shape)
            continue;

        uint32_t id = rd32(st + 2);
        int32_t rel = (int32_t)rd32(st + 8);
        int64_t slot = (int64_t)off + STUB_LEN + rel;
        int in_data = slot >= (int64_t)ly->data.fileoff &&
                      (uint64_t)slot + SLOT_LEN <= ly->data.fileoff + ly->data.filesize;
        CHECK(in_data, "%s: %s stub jumps through offset %#llx, outside __DATA", what, sym,
              (unsigned long long)slot);
        if (!in_data)
            continue;
        uint64_t held = rd64(img + slot);
        CHECK(held == want_slot, "%s: %s stub's slot holds %#llx, want %#llx", what, sym,
              (unsigned long long)held, (unsigned long long)want_slot);

        const char *got_lib = NULL, *got_sym = NULL;
        int named = ocerz_vdylib_export_name(id, &got_lib, &got_sym);
        int back = named && got_lib && got_sym && strcmp(got_lib, lib) == 0 &&
                   strcmp(got_sym, sym) == 0;
        CHECK(back, "%s: %s stub carries id %u, which dispatches to %s %s", what, sym, id,
              named && got_lib ? got_lib : "(no library)",
              named && got_sym ? got_sym : "(no symbol)");
        good += held == want_slot && back;
    }
    CHECK(good == n, "%s: only %zu of the %zu function exports reach a stub that traps as "
          "themselves", what, good, n);
}

static int db_ordinal(const OcerzApiLibrary *api)
{
    const char *dir = ocerz_apidb_dir();
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return -1;
    const char *base = strrchr(api->path, '/');
    base = base ? base + 1 : api->path;
    int before = 0, found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (de->d_name[0] == '.' || n <= 4 || strcmp(de->d_name + n - 4, ".api") != 0)
            continue;
        int c = strcmp(de->d_name, base);
        before += c < 0;
        found |= c == 0;
    }
    closedir(d);
    return found ? before : -1;
}

static void check_xmm_contracts(void)
{
    static const struct {
        const char *sym;
        int known;
        uint16_t in, out;
    } kWant[] = {
        { "__tlv_bootstrap", 0, 0, 0 },
        { "___chkstk_darwin", 0, 0, 0 },
        { "_strlen", 1, 0x00, 0x0 },
        { "_strtod", 1, 0x00, 0x1 },
        { "_printf", 1, 0xff, 0x3 },
        { "_scanf", 0, 0, 0 },
    };
    const OcerzApiLibrary *api = ocerz_apidb_library(kLib);
    int ord = api ? db_ordinal(api) : -1;
    CHECK(api != NULL && ord >= 0, "xmm contracts: the database has no numbered file for %s", kLib);
    if (!api || ord < 0)
        return;
    for (size_t i = 0; i < sizeof kWant / sizeof kWant[0]; i++) {
        const OcerzApiEntry *e = ocerz_apidb_find(api, kWant[i].sym);
        CHECK(e != NULL, "xmm contracts: %s has no record", kWant[i].sym);
        if (!e)
            continue;
        uint64_t id = ((uint64_t)ord << 20) | (uint64_t)(e - api->entries);
        uint16_t in = 0xeeee, out = 0xeeee;
        int known = ocerz_vdylib_xmm_contract(id, &in, &out);
        CHECK(known == kWant[i].known && (!known || (in == kWant[i].in && out == kWant[i].out)),
              "xmm contracts: %s answers %d with read %#x written %#x, want %d with read %#x written %#x",
              kWant[i].sym, known, in, out, kWant[i].known, kWant[i].in, kWant[i].out);
    }
    uint16_t in = 0, out = 0;
    CHECK(ocerz_vdylib_xmm_contract(((uint64_t)0xfff << 20) | 7, &in, &out) == 0,
          "xmm contracts: an id no library minted has a contract");
}

static void check_database_stubs(const char *what, const uint8_t *img, size_t len,
                                 const Layout *ly, const char *lib)
{
    const OcerzApiLibrary *api = ocerz_apidb_library(lib);
    CHECK(api != NULL, "%s: the database has no file for %s", what, lib);
    if (!api)
        return;
    int ord = db_ordinal(api);
    CHECK(ord >= 0, "%s: %s is not among the database's files", what, api->path);
    const char **names = calloc((size_t)api->nentries + 1, sizeof *names);
    CHECK(names != NULL, "%s: out of memory", what);
    if (!names || ord < 0) {
        free(names);
        return;
    }
    size_t n = 0, want_ids = 0, ids_ok = 0, want_vars = 0, vars_ok = 0;
    const char *bad_id = NULL, *bad_var = NULL;
    for (int i = 0; i < api->nentries; i++) {
        const OcerzApiEntry *e = &api->entries[i];
        int found = 0;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, e->export_name, &found);
        uint64_t off = a - LOAD_BASE;
        if (e->kind == OCERZ_API_FN || e->kind == OCERZ_API_SPECIAL || e->kind == OCERZ_API_STUB) {
            int tlv = e->kind == OCERZ_API_SPECIAL && (strcmp(e->handler, "tlv_bootstrap") == 0 ||
                                                       strcmp(e->handler, "chkstk") == 0);
            want_ids++;
            uint32_t want = (uint32_t)ord << 20 | (uint32_t)i;
            int ok = found && off + STUB_STRIDE <= ly->text.filesize &&
                     rd32(img + off + (tlv ? 4 : 2)) == want;
            ids_ok += ok;
            if (!ok && !bad_id)
                bad_id = e->export_name;
            if (!tlv)
                names[n++] = e->export_name;
        } else if (e->kind == OCERZ_API_VAR) {
            want_vars++;
            int ok = found && off >= ly->data.fileoff && (off & (SLOT_LEN - 1)) == 0 &&
                     off + e->bytes <= ly->data.fileoff + ly->data.filesize &&
                     off + e->bytes <= (uint64_t)len;
            vars_ok += ok;
            if (!ok && !bad_var)
                bad_var = e->export_name;
        }
    }
    CHECK(ids_ok == want_ids, "%s: only %zu of %zu function records carry the id of file %d, "
          "entry index (first wrong: %s)", what, ids_ok, want_ids, ord, bad_id ? bad_id : "");
    CHECK(vars_ok == want_vars, "%s: only %zu of %zu var records resolve to an aligned slot in "
          "__DATA (first wrong: %s)", what, vars_ok, want_vars, bad_var ? bad_var : "");
    check_function_stubs(what, img, len, ly, lib, names, n);
    free(names);
}

static void check_libsystem_database(void)
{
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);
    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);
    if (!img)
        return;
    Layout ly;
    if (layout_of("libSystem", img, len, kLib, &ly)) {
        check_database_stubs("libSystem", img, len, &ly, kLib);
        for (size_t i = 0; i < NM8LIBSYSTEM; i++) {
            int found = 0;
            ocerz_dyld_trie_resolve(img, LOAD_BASE, kM8LibSystem[i], &found);
            CHECK(found, "libSystem no longer exports %s, which native mode exported before its "
                  "exports came from the database", kM8LibSystem[i]);
        }
    }
    free(img);
}

static void check_absent(const char *what, const uint8_t *img, const char *const *names,
                         size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int found = 1;
        uint64_t v = ocerz_dyld_trie_resolve(img, LOAD_BASE, names[i], &found);
        CHECK(!found && v == 0, "%s: %s resolved to %#llx, but it is not exported there", what,
              names[i], (unsigned long long)v);
    }
}

static const OcerzApiEntry **g_natives;
static size_t g_nnatives;
static size_t g_cf_functions;

static int load_cf_records(void)
{
    const OcerzApiLibrary *api = ocerz_apidb_library(kCF);
    if (!api)
        return 0;
    g_natives = calloc((size_t)api->nentries + 1, sizeof *g_natives);
    if (!g_natives)
        return 0;
    for (int i = 0; i < api->nentries; i++) {
        const OcerzApiEntry *e = &api->entries[i];
        if (e->kind == OCERZ_API_DATA)
            g_natives[g_nnatives++] = e;
        else if (e->kind == OCERZ_API_FN || e->kind == OCERZ_API_SPECIAL || e->kind == OCERZ_API_STUB)
            g_cf_functions++;
    }
    return 1;
}

static int native_index(const char *host_sym)
{
    for (size_t k = 0; k < g_nnatives; k++)
        if (strcmp(g_natives[k]->host, host_sym) == 0)
            return (int)k;
    return -1;
}

static uint64_t fake_host_value(size_t k)
{
    if (k < NFAKE)
        return kFakeHost[k];
    return 0x0000100000000000ull + ((uint64_t)k << 40);
}

static int fake_misses(size_t k)
{
    return k % 3 == 0;
}

static int g_fake_wrong_lib;

static void *fake_host_all(const char *install_name, const char *host_sym)
{
    if (!install_name || strcmp(install_name, kCF) != 0)
        g_fake_wrong_lib++;
    int k = native_index(host_sym);
    return k < 0 ? NULL : (void *)(uintptr_t)fake_host_value((size_t)k);
}

static void *fake_host_some(const char *install_name, const char *host_sym)
{
    if (!install_name || strcmp(install_name, kCF) != 0)
        g_fake_wrong_lib++;
    int k = native_index(host_sym);
    if (k < 0 || fake_misses((size_t)k))
        return NULL;
    return (void *)(uintptr_t)fake_host_value((size_t)k);
}

static void check_natives_are(const char *what, const uint8_t *img, void *(*want)(size_t))
{
    for (size_t k = 0; k < g_nnatives; k++) {
        const char *sym = g_natives[k]->export_name;
        uint64_t w = (uint64_t)(uintptr_t)want(k);
        int found = 0, found_alt = 0;
        uint64_t v = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
        uint64_t v_alt = ocerz_dyld_trie_resolve(img, LOAD_BASE_ALT, sym, &found_alt);
        if (w == 0) {
            CHECK(!found && !found_alt && v == 0,
                  "%s: %s has no host address but resolved to %#llx", what, sym,
                  (unsigned long long)v);
            continue;
        }
        CHECK(found && v == w, "%s: %s resolved to %#llx (found %d), want exactly %#llx", what,
              sym, (unsigned long long)v, found, (unsigned long long)w);
        CHECK(found_alt && v_alt == w,
              "%s: %s resolved to %#llx at a second load base, want %#llx: the value is not "
              "absolute", what, sym, (unsigned long long)v_alt, (unsigned long long)w);
    }
}

static int same_segments(const uint8_t *a, const uint8_t *b, const Layout *ly)
{
    uint64_t lo = ly->text.sect_addr;
    return lo < ly->trie_off && memcmp(a + lo, b + lo, ly->trie_off - lo) == 0;
}

static void *g_cf_handle;

static void *host_native(size_t k)
{
    return g_cf_handle ? dlsym(g_cf_handle, g_natives[k]->host) : NULL;
}

static void *fake_all_native(size_t k)
{
    return (void *)(uintptr_t)fake_host_value(k);
}

static void *fake_some_native(size_t k)
{
    return fake_misses(k) ? NULL : (void *)(uintptr_t)fake_host_value(k);
}

static char *read_all(int fd, size_t *len_out)
{
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf)
        return NULL;
    lseek(fd, 0, SEEK_SET);
    for (;;) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb)
                break;
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

static size_t count_of(const char *hay, const char *needle)
{
    size_t c = 0, nl = strlen(needle);
    for (const char *p = strstr(hay, needle); p; p = strstr(p + nl, needle))
        c++;
    return c;
}

static void check_corefoundation(void)
{
    CHECK(ocerz_vdylib_have(kCF) != 0, "ocerz_vdylib_have(\"%s\") says no", kCF);

    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kCF, &len);
    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kCF);
    if (!img)
        return;
    CHECK(len < IMAGE_MAX, "CoreFoundation image is %zu bytes, which is implausible", len);

    Layout ly;
    if (!layout_of("CoreFoundation", img, len, kCF, &ly)) {
        free(img);
        return;
    }
    CHECK(ly.text.sect_found && ly.text.sect_size == g_cf_functions * STUB_STRIDE,
          "CoreFoundation's __text is %llu bytes, want one stub for each of the %zu function "
          "records and none for its data", (unsigned long long)ly.text.sect_size, g_cf_functions);
    CHECK(ly.data.sect_found && ly.data.sect_size == g_cf_functions * SLOT_LEN,
          "CoreFoundation's __data is %llu bytes, want one jump slot for each of the %zu "
          "function records and nothing for its data", (unsigned long long)ly.data.sect_size,
          g_cf_functions);

    check_function_stubs("CoreFoundation", img, len, &ly, kCF, kCFFunctions, NCFFUNCS);
    check_database_stubs("CoreFoundation", img, len, &ly, kCF);
    for (size_t k = 0; k < NCFNATIVES; k++) {
        const OcerzApiEntry *e = ocerz_apidb_find(ocerz_apidb_library(kCF), kCFNatives[k]);
        CHECK(e && e->kind == OCERZ_API_DATA && strcmp(e->host, kCFNatives[k] + 1) == 0,
              "CoreFoundation's database does not carry %s as data naming %s", kCFNatives[k],
              kCFNatives[k] + 1);
    }

    g_cf_handle = dlopen(kCF, RTLD_LAZY | RTLD_LOCAL);
    CHECK(g_cf_handle != NULL, "the host will not dlopen %s: %s", kCF, dlerror());
    size_t host_has = 0;
    for (size_t k = 0; k < g_nnatives; k++)
        host_has += host_native(k) != NULL;
    size_t m8_has = 0;
    for (size_t k = 0; k < NCFNATIVES; k++)
        m8_has += g_cf_handle && dlsym(g_cf_handle, kCFNatives[k] + 1) != NULL;
    CHECK(m8_has == NCFNATIVES, "the host CoreFoundation has only %zu of the %zu native data "
          "names", m8_has, NCFNATIVES);
    check_natives_are("CoreFoundation", img, host_native);

    size_t ls_len = 0;
    uint8_t *ls = ocerz_vdylib_image(kLib, &ls_len);
    CHECK(ls != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);
    if (ls) {
        check_absent("libSystem", ls, kCFFunctions, NCFFUNCS);
        check_absent("libSystem", ls, kCFNatives, NCFNATIVES);
        free(ls);
    }
    check_absent("CoreFoundation", img, kExports, NEXPORTS);
    check_absent("CoreFoundation", img, (const char *const[]){ kGuard, "__tlv_bootstrap" }, 2);
    check_absent("CoreFoundation", img, kCFNotExported, NCFNOT);

    g_fake_wrong_lib = 0;
    size_t all_len = 0;
    uint8_t *all = ocerz_vdylib_image_with(kCF, fake_host_all, &all_len);
    CHECK(all != NULL, "CoreFoundation built against high host addresses returned no buffer");
    if (all) {
        Layout aly;
        if (layout_of("CoreFoundation (high)", all, all_len, kCF, &aly)) {
            check_natives_are("CoreFoundation (high)", all, fake_all_native);
            check_function_stubs("CoreFoundation (high)", all, all_len, &aly, kCF,
                                 kCFFunctions, NCFFUNCS);
            if (host_has == g_nnatives) {
                CHECK(all_len == len && aly.trie_off == ly.trie_off &&
                      aly.trie_size == ly.trie_size,
                      "the image is %zu bytes (trie %u at %u) against high addresses and %zu "
                      "(trie %u at %u) against the host's, so a terminal's width depends on "
                      "its value", all_len, aly.trie_size, aly.trie_off, len, ly.trie_size,
                      ly.trie_off);
            }
            CHECK(aly.trie_off == ly.trie_off && same_segments(all, img, &ly),
                  "the stubs or __DATA differ when only native addresses change, so a native "
                  "export took bytes in a segment");
        }
        free(all);
    }

    int saved = dup(2);
    FILE *cap = tmpfile();
    int prev_verbose = ocerz_verbose;
    size_t some_len = 0, again_len = 0;
    uint8_t *some = NULL, *again = NULL;
    if (saved >= 0 && cap) {
        fflush(stderr);
        dup2(fileno(cap), 2);
        ocerz_verbose = 1;
        some = ocerz_vdylib_image_with(kCF, fake_host_some, &some_len);
        again = ocerz_vdylib_image_with(kCF, fake_host_some, &again_len);
        ocerz_verbose = prev_verbose;
        fflush(stderr);
        dup2(saved, 2);
    }
    if (saved >= 0)
        close(saved);
    CHECK(cap != NULL && saved >= 0, "cannot capture stderr to check the missing-name log");
    CHECK(g_fake_wrong_lib == 0,
          "the host lookup was asked %d times about a library other than %s",
          g_fake_wrong_lib, kCF);

    CHECK(some != NULL, "CoreFoundation built with missing host names returned no buffer");
    if (some) {
        Layout sly;
        if (layout_of("CoreFoundation (missing)", some, some_len, kCF, &sly)) {
            check_natives_are("CoreFoundation (missing)", some, fake_some_native);
            check_function_stubs("CoreFoundation (missing)", some, some_len, &sly, kCF,
                                 kCFFunctions, NCFFUNCS);
            CHECK(some_len < len, "leaving native names out did not shrink the trie (%zu vs %zu)",
                  some_len, len);
            CHECK(sly.trie_off == ly.trie_off && same_segments(some, img, &ly),
                  "the stubs or __DATA differ when native names are missing, so a native "
                  "export took bytes in a segment");
        }
    }
    CHECK(again != NULL && some != NULL && again_len == some_len &&
          memcmp(again, some, some_len) == 0,
          "two builds against the same missing names are not identical");

    if (cap) {
        size_t log_len = 0;
        char *log = read_all(fileno(cap), &log_len);
        CHECK(log != NULL, "cannot read the captured log back");
        if (log) {
            for (size_t k = 0; k < g_nnatives; k++) {
                char line[512];
                snprintf(line, sizeof line, "does not export %s\n", g_natives[k]->export_name);
                size_t c = count_of(log, line);
                if (!host_native(k))
                    continue;
                if (fake_misses(k))
                    CHECK(c == 1, "%s went missing in two builds and was logged %zu times, "
                          "want once", g_natives[k]->export_name, c);
                else
                    CHECK(c == 0, "%s resolved but was logged missing %zu times",
                          g_natives[k]->export_name, c);
            }
            free(log);
        }
        fclose(cap);
    }
    free(some);
    free(again);
    free(img);
}

static uint8_t *libsystem_unguarded(size_t *len_out)
{
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);
    int found = 0;
    uint64_t guard = img ? ocerz_dyld_trie_resolve(img, 0, kGuard, &found) : 0;
    if (!img || !found || guard + SLOT_LEN > len) {
        free(img);
        return NULL;
    }
    memset(img + guard, 0, SLOT_LEN);
    *len_out = len;
    return img;
}

static uint8_t *libsystem_built_after_corefoundation(size_t *len_out)
{
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        size_t cf_len = 0, len = 0;
        uint8_t *cf = ocerz_vdylib_image_with(kCF, fake_host_all, &cf_len);
        uint8_t *img = cf ? libsystem_unguarded(&len) : NULL;
        if (!img)
            _exit(1);
        for (size_t at = 0; at < len;) {
            ssize_t w = write(fds[1], img + at, len - at);
            if (w <= 0)
                _exit(1);
            at += (size_t)w;
        }
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    uint8_t *img = (uint8_t *)read_all(fds[0], &len);
    close(fds[0]);
    int status = 0;
    int reaped = waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                 WEXITSTATUS(status) == 0;
    if (!img || !reaped || len == 0) {
        free(img);
        return NULL;
    }
    *len_out = len;
    return img;
}

#define SYNTH_LIB  "/usr/lib/libocerzsynth.dylib"
#define SYNTH_LIB2 "/usr/lib/libocerzsynth2.dylib"
#define SYNTH_N    6000

typedef struct {
    int checks;
    int failures;
} Counts;

static void in_child(const char *what, void (*fn)(void))
{
    int fds[2];
    if (pipe(fds) != 0) {
        CHECK(0, "%s: no pipe", what);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        checks = 0;
        failures = 0;
        fn();
        Counts c = { checks, failures };
        _exit(write(fds[1], &c, sizeof c) == (ssize_t)sizeof c ? 0 : 1);
    }
    close(fds[1]);
    Counts c = { 0, 0 };
    ssize_t got = pid > 0 ? read(fds[0], &c, sizeof c) : -1;
    close(fds[0]);
    int status = 0;
    int ok = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
             WEXITSTATUS(status) == 0 && got == (ssize_t)sizeof c;
    CHECK(ok, "%s: the child did not report back (status %#x)", what, status);
    if (ok) {
        checks += c.checks;
        failures += c.failures;
    }
}

static void synth_name(char *out, size_t cap, int i)
{
    if (i % 500 == 7) {
        int n = snprintf(out, cap, "_synth_long_%d_", i);
        while (n < 400 && (size_t)n + 1 < cap) {
            out[n] = (char)('a' + (i + n) % 26);
            n++;
        }
        out[n] = '\0';
        return;
    }
    snprintf(out, cap, "_synth_%d", i);
}

static int synth_misses(int i)
{
    return i % 40 == 18;
}

static uint64_t synth_value(int i)
{
    return 0x0000100000000000ull + ((uint64_t)i << 12) + (uint64_t)i;
}

static void *synth_host(const char *install_name, const char *host_sym)
{
    int i = 0;
    if (strcmp(install_name, SYNTH_LIB) != 0 || sscanf(host_sym, "synthhost_%d", &i) != 1 ||
        synth_misses(i))
        return NULL;
    return (void *)(uintptr_t)synth_value(i);
}

static OcerzApiKind synth_kind(int i)
{
    if (i == 3)
        return OCERZ_API_SPECIAL;
    switch (i % 10) {
    case 6:
    case 7: return OCERZ_API_STUB;
    case 8: return OCERZ_API_DATA;
    case 9: return OCERZ_API_VAR;
    default: return OCERZ_API_FN;
    }
}

static int write_synth(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/libocerzsynth.dylib.api", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "# synthesized by test_vdylib\nocerz-apidb 1\nlibrary %s\nsdk macos 27.0\n", SYNTH_LIB);
    for (int i = 0; i < SYNTH_N; i++) {
        char name[512];
        synth_name(name, sizeof name, i);
        switch (synth_kind(i)) {
        case OCERZ_API_SPECIAL: fprintf(f, "special %s exit\n", name); break;
        case OCERZ_API_STUB: fprintf(f, "stub %s synthetic\n", name); break;
        case OCERZ_API_DATA: fprintf(f, "data %s synthhost_%d\n", name, i); break;
        case OCERZ_API_VAR: fprintf(f, "var %s %d stack_guard\n", name, i % 20 == 9 ? 24 : 8); break;
        default: fprintf(f, "fn %s strlen L(p)\n", name); break;
        }
    }
    if (fclose(f) != 0)
        return 0;
    snprintf(path, sizeof path, "%s/libocerzsynth2.dylib.api", dir);
    f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "ocerz-apidb 1\nlibrary %s\nsdk macos 27.0\nfn _two_a strlen L(p)\n"
            "stub _two_b synthetic\nfn _two_c strlen L(p)\n", SYNTH_LIB2);
    return fclose(f) == 0;
}

static void synth_child(void)
{
    const char *tmp = getenv("TMPDIR");
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/ocerz-vdylib-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    CHECK(mkdtemp(root) != NULL, "cannot make a temporary database root");
    char dir[PATH_MAX + 32];
    snprintf(dir, sizeof dir, "%s/macos", root);
    int made = mkdir(dir, 0755) == 0;
    snprintf(dir, sizeof dir, "%s/macos/27.0", root);
    made = made && mkdir(dir, 0755) == 0 && write_synth(dir);
    CHECK(made, "cannot write the synthetic database under %s", root);
    if (!made)
        return;
    setenv("OCERZ_APIDB", root, 1);

    CHECK(ocerz_vdylib_have(SYNTH_LIB), "the synthetic library is not synthesized");
    CHECK(!ocerz_vdylib_have(kLib), "libSystem is synthesized from a database with no file for it");

    size_t len2 = 0, len = 0;
    uint8_t *img2 = ocerz_vdylib_image_with(SYNTH_LIB2, synth_host, &len2);
    uint8_t *img = ocerz_vdylib_image_with(SYNTH_LIB, synth_host, &len);
    CHECK(img != NULL && img2 != NULL, "the synthetic libraries were not built");

    Layout ly;
    if (img && layout_of("synthetic", img, len, SYNTH_LIB, &ly)) {
        size_t nfunc = 0, nvar = 0;
        for (int i = 0; i < SYNTH_N; i++) {
            OcerzApiKind k = synth_kind(i);
            nfunc += k == OCERZ_API_FN || k == OCERZ_API_SPECIAL || k == OCERZ_API_STUB;
            nvar += k == OCERZ_API_VAR;
        }
        CHECK(ly.text.sect_found && ly.text.sect_size == nfunc * STUB_STRIDE,
              "synthetic __text is %llu bytes, want %zu stubs", (unsigned long long)ly.text.sect_size,
              nfunc);
        CHECK(ly.text.vmsize >= 16 * 4096 && ly.data.vmsize >= 4 * 4096,
              "synthetic __TEXT is %llu bytes and __DATA %llu, which cannot hold %zu stubs",
              (unsigned long long)ly.text.vmsize, (unsigned long long)ly.data.vmsize, nfunc);
        check_database_stubs("synthetic", img, len, &ly, SYNTH_LIB);

        size_t data_ok = 0, data_n = 0, var_ok = 0, long_ok = 0, long_n = 0;
        for (int i = 0; i < SYNTH_N; i++) {
            char name[512];
            synth_name(name, sizeof name, i);
            int found = 0, found_alt = 0;
            uint64_t v = ocerz_dyld_trie_resolve(img, LOAD_BASE, name, &found);
            uint64_t v_alt = ocerz_dyld_trie_resolve(img, LOAD_BASE_ALT, name, &found_alt);
            if (strlen(name) > 300) {
                long_n++;
                long_ok += found;
            }
            if (synth_kind(i) == OCERZ_API_DATA) {
                data_n++;
                if (synth_misses(i))
                    data_ok += !found && !found_alt;
                else
                    data_ok += found && found_alt && v == synth_value(i) && v_alt == synth_value(i);
            } else if (synth_kind(i) == OCERZ_API_VAR && found) {
                uint64_t off = v - LOAD_BASE;
                uint64_t g = off + 8 <= len ? rd64(img + off) : 0;
                var_ok += g != 0 && (g & 0xff) == 0;
            }
        }
        CHECK(data_ok == data_n, "only %zu of %zu synthetic data records resolve absolute, or are "
              "absent when the host has no such name", data_ok, data_n);
        CHECK(var_ok == nvar, "only %zu of %zu synthetic var slots hold a filled canary", var_ok, nvar);
        CHECK(long_n > 0 && long_ok == long_n, "only %zu of %zu names longer than 300 characters "
              "resolve", long_ok, long_n);
        check_absent("synthetic", img,
                     (const char *const[]){ "_synth_6000", "_synth_", "_synth_long_7_", "_synth_10000",
                                            "_synth_1a" }, 5);
    }
    Layout ly2;
    if (img2 && layout_of("synthetic second", img2, len2, SYNTH_LIB2, &ly2))
        check_database_stubs("synthetic second", img2, len2, &ly2, SYNTH_LIB2);
    const OcerzApiLibrary *api2 = ocerz_apidb_library(SYNTH_LIB2);
    CHECK(api2 && db_ordinal(api2) == 1 && ocerz_apidb_library(SYNTH_LIB) &&
          db_ordinal(ocerz_apidb_library(SYNTH_LIB)) == 0,
          "the synthetic files are not numbered 0 and 1 in directory order");
    free(img);
    free(img2);

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (strstr(root, "ocerz-vdylib-") && system(cmd) != 0)
        fprintf(stderr, "test_vdylib: could not remove %s\n", root);
}

typedef struct TrieSeen {
    const uint8_t *img;
    const OcerzApiLibrary *api;
    const char *what;
    int visited;
    int stop_at;
} TrieSeen;

static int trie_seen_visit(void *ctx, const char *name, uint64_t value, uint64_t flags)
{
    TrieSeen *t = ctx;
    int found = 0;
    uint64_t want = ocerz_dyld_trie_resolve(t->img, LOAD_BASE, name, &found);
    t->visited++;
    CHECK(found && want == value, "%s: the trie walk visits %s at %#llx (flags %#llx), where the "
          "resolver answers %s%#llx", t->what, name, (unsigned long long)value,
          (unsigned long long)flags, found ? "" : "nothing, not ", (unsigned long long)want);
    CHECK(ocerz_apidb_find(t->api, name) != NULL, "%s: the trie walk visits %s, which the database "
          "does not name", t->what, name);
    return t->stop_at && t->visited == t->stop_at;
}

static void check_trie_each_of(const char *what, const char *install_name)
{
    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(install_name, &len);
    const OcerzApiLibrary *api = ocerz_apidb_library(install_name);
    CHECK(img != NULL && api != NULL, "%s: no image or no database to walk", what);
    if (!img || !api) {
        free(img);
        return;
    }
    int expected = 0;
    for (int i = 0; i < api->nentries; i++) {
        int found = 0;
        ocerz_dyld_trie_resolve(img, LOAD_BASE, api->entries[i].export_name, &found);
        expected += found;
    }
    TrieSeen t = { img, api, what, 0, 0 };
    int n = ocerz_dyld_trie_each(img, LOAD_BASE, trie_seen_visit, &t);
    CHECK(n == expected && t.visited == expected, "%s: the trie walk answered %d and visited %d "
          "terminals, where the resolver finds %d of the database's names", what, n, t.visited,
          expected);

    TrieSeen early = { img, api, what, 0, 10 };
    ocerz_dyld_trie_each(img, LOAD_BASE, trie_seen_visit, &early);
    CHECK(early.visited == 10, "%s: a visitor that asked to stop at 10 was called %d times", what,
          early.visited);

    uint32_t ncmds = 0;
    memcpy(&ncmds, img + 16, 4);
    uint8_t *lc = img + 32;
    int cut = 0;
    for (uint32_t i = 0; i < ncmds && !cut; i++) {
        uint32_t cmd = 0, size = 0, two = 2;
        memcpy(&cmd, lc, 4);
        memcpy(&size, lc + 4, 4);
        if (cmd == 0x80000033) {
            memcpy(lc + 12, &two, 4);
            cut = 1;
        }
        lc += size;
    }
    CHECK(cut, "%s: the image has no LC_DYLD_EXPORTS_TRIE to cut short", what);
    CHECK(ocerz_dyld_trie_each(img, LOAD_BASE, NULL, NULL) == -1,
          "%s: a trie cut to two bytes was walked instead of refused", what);
    free(img);
}

static void check_trie_each(void)
{
    check_trie_each_of("libSystem", kLib);
    check_trie_each_of("CoreFoundation", kCF);
}

int main(void)
{
    setenv("OCERZ_APIDB", "runtime/apis", 0);
    in_child("the 6000-export synthetic library", synth_child);
    CHECK(load_cf_records(), "the database has no CoreFoundation records under %s",
          getenv("OCERZ_APIDB"));

    size_t late_len = 0;
    uint8_t *late = libsystem_built_after_corefoundation(&late_len);

    check_libsystem();
    check_xmm_contracts();
    check_libsystem_database();
    check_corefoundation();
    check_trie_each();

    CHECK(late != NULL, "a child that built CoreFoundation before libSystem sent nothing back");
    size_t early_len = 0;
    uint8_t *early = libsystem_unguarded(&early_len);
    CHECK(early != NULL, "cannot rebuild libSystem to compare against the child's");
    if (late && early)
        CHECK(late_len == early_len && memcmp(late, early, early_len) == 0,
              "libSystem built after CoreFoundation differs from libSystem built first, so "
              "its export ids depend on which library a guest names first");
    free(late);
    free(early);
    return report();
}
