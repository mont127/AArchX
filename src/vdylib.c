/*
 * Building the synthesized x86_64 system libraries.
 *
 * ---- the file that is never a file ----
 * An image here is laid out exactly as a linker would lay it on disk: header,
 * load commands and stubs filling __TEXT from offset zero, then __DATA with the
 * jump slots and after them the data exports, then the export trie past the
 * end of both.  The loader never reopens anything - map_segments memcpys out
 * of DynImage.slice and the trie walker reads that same buffer - so the buffer
 * IS the file, and keeping the file layout honest is what lets the image flow
 * through mapping, binding, dlopen and dladdr with no special case anywhere.
 * __TEXT has to be the segment with fileoff 0 and a non-zero filesize, because
 * that pair is how map_segments picks the text segment out and derives the
 * image's base from it.  __TEXT starts at vmaddr zero and every segment is 4 KB
 * aligned in both file and memory, so a symbol's offset in the trie is also its
 * offset in the buffer, and the loader's slide is the whole of the image's load
 * address.
 *
 * __TEXT is emitted with read+execute in BOTH maxprot and initprot.  Only one
 * of the two is really consulted - protect_ro_segments reads the word at
 * segment offset 56, which is maxprot rather than the initprot its local is
 * named for - and a segment named __TEXT is re-protected only when that word
 * has read and execute and not write.  Writing 5 into both means the stubs end
 * up executable-not-writable whichever word is meant, instead of staying in
 * the writable state every image is mapped in.
 *
 * The trie deliberately sits outside every segment.  Nothing maps it into
 * guest memory and nothing needs to: the only reader is the host-side walker,
 * which indexes the host buffer by the LC_DYLD_EXPORTS_TRIE dataoff.  Giving
 * it a __LINKEDIT segment would only cost the guest address space.
 *
 * ---- why the trie's numbers are fixed width ----
 * Every child edge in an export trie carries the absolute offset of the child
 * node from the start of the trie, so a node's size depends on how wide its
 * children's offsets encode, and those offsets depend on the sizes of the
 * nodes before them.  Encoding offsets minimally therefore means iterating to
 * a fixed point that is not guaranteed to settle.  Instead every offset
 * and every terminal address is emitted as a padded ULEB128 of fixed width,
 * high bit set on every byte but the last: redundant padding is legal ULEB and
 * the walker decodes it correctly because it keeps shifting while the high bit
 * is set.  Offsets, and the terminals of stubs and data slots, are five bytes,
 * which carry 35 bits, more than any image this file will ever build.  A native
 * data terminal carries a host address instead, and the image's size puts no
 * bound on that: the host's libraries sit wherever the kernel mapped them, and
 * an address past 2^35 written into five bytes would lose its top bits without
 * a word of complaint.  Those terminals are ten bytes, which is every bit a
 * 64-bit value has, the tenth byte holding bit 63 alone.  With every width
 * known up front - a terminal's kind comes from the table, and no width
 * depends on the value written into it - a node's size is known before
 * anything is placed, so layout is one pass over the nodes and emission is a
 * second, with no back-patching at all.  The same choice fixes a terminal at
 * one flags byte plus its address bytes, so the terminal size a node declares
 * is 6 for a stub or a slot and 11 for native data, a single ULEB byte either
 * way, and an image's layout does not move whatever addresses the host hands
 * back.
 *
 * The trie is a real radix trie, not a flat list of full names.  The walker
 * takes the FIRST child whose edge matches under strncmp, so with a flat root
 * the edge _write would swallow a lookup of _writev, consume six characters
 * and dead-end on a childless node.  Children of a node are therefore keyed by
 * distinct first characters, a shared prefix becomes an edge of its own, and a
 * symbol that is a proper prefix of another - _read inside _readv and _readdir
 * - becomes an interior node that also carries a terminal.  The symbol list
 * below is stocked with such pairs on purpose.
 *
 * ---- why twelve bytes at a stride of sixteen ----
 * A stub is mov r11d, imm32 (six bytes) then jmp qword [rip + rel32] (six
 * more).  Twelve is all the instructions need, but they are placed every
 * sixteen so a stub's address is its index shifted, which keeps the trie
 * addresses, the jump displacements and the __DATA slot for a given export all
 * derivable from one number; the four bytes of slack are filled with int3 so a
 * stray branch into the padding stops instead of quietly executing the zeros
 * that would otherwise be there.  The jump is rip-relative and the slot it
 * reads holds a constant, the one address in the dyld-API trap window that
 * means "bridge", so nothing in a synthesized image is position-dependent and
 * the image needs no rebases, no binds and no chained fixups: __TEXT and
 * __DATA slide together, and a displacement between them computed at build
 * time is still correct at every load address.
 *
 * ---- the exports that are not functions ----
 * Not every name a program imports from libSystem is one it calls.  A
 * stack-protected function copies the value at ___stack_chk_guard onto its
 * frame in the prologue and compares the copy against it again before it
 * returns, and both are loads through the pointer the loader bound, not
 * calls.  clang turns the protector on by default, so almost every ordinary
 * program imports that symbol whether its author asked for it or not, and an
 * image that lacks it refuses to bind the program at all.
 *
 * So there is a second kind of export: a name, a size and a way to fill it,
 * given a slot in __DATA after the last jump slot, each slot starting on an
 * 8-byte boundary, with __DATA's size taking the slots in and the trie moving
 * past them.  Its trie terminal is the slot's offset from the image start, the
 * same thing a function's terminal is for its stub, so the trie walker and the
 * loader cannot tell the two kinds apart and neither needed a change: the
 * guest's GOT entry is bound to the slot's address and the guest reads through
 * it.  A data export has no stub and takes no export id, because nothing ever
 * branches to it, and an id no trap can ever carry would only be a row in the
 * dispatch table that means nothing.  A fill left empty means zeroes, which is
 * what a calloc'd buffer already holds.
 *
 * ---- the canary ----
 * ___stack_chk_guard is eight bytes filled from arc4random_buf every time an
 * image is built.  It does not have to agree with the host's own guard or with
 * anything else: the only thing a guest ever compares it with is the copy its
 * own prologue took, and the bridge behind ___stack_chk_fail stops the run
 * without comparing anything.  The loader builds a given install name once, so
 * a running guest only ever sees one value.
 *
 * Its lowest byte is always zero.  x86 is little-endian and the copy sits above
 * the locals it guards, so the lowest byte is the first one an overrun climbing
 * out of a buffer reaches.  A string copy can write a zero only as its
 * terminator, so it cannot rewrite that byte and carry on to the saved frame
 * pointer and return address with the guard still matching; and a string read
 * that runs off the end of an unterminated buffer, which is how a guard usually
 * leaks into output, stops there before it has shown any of the other seven.
 * The host's own guard zeroes its second byte instead, which stops a copy just
 * as well, gives up one byte to a read, and in exchange notices an off-by-one
 * that writes nothing but a terminator; either is sound, and nothing ever
 * compares the two.  The other seven bytes are drawn again in the rare case
 * they all come back zero, since an all-zero guard is reproduced exactly by
 * any overrun that writes zeroes.
 *
 * ---- the data symbols deliberately left out ----
 * libSystem has other data that compiled programs reach for directly: _environ,
 * ___progname, __DefaultRuneLocale, and ___stdoutp and ___stderrp, which are
 * what stdout and stderr expand to.  None of them is exported, and none should
 * become a slot filled at build time, because each is a variable native code
 * also reads or writes on its own account, and a guest slot beside it is a
 * second copy that goes stale the first time either side writes.  _environ is
 * what native getenv, setenv and execvp walk, and it points at the host's
 * environment, not at the one on the guest's initial stack.  ___progname is
 * what native getprogname, err and warn print, and it names ocerz.  ___stdoutp
 * and ___stderrp are the streams native printf, puts and perror write through;
 * a program that assigns stdout expects printf to follow it, and a slot holding
 * anything but the native stream hands native stdio a FILE it never opened.
 * __DefaultRuneLocale is a 3208-byte table that carries pointers into host
 * memory and has to agree with native __maskrune, so a copy is wrong on both
 * counts, and a zeroed one silently answers false to every ctype question.  A
 * program importing any of them therefore still fails to bind and says which
 * name it wanted, and a refusal that names itself is better than a value that
 * is wrong without saying so.
 *
 * ---- the exports that are the host's own variables ----
 * CoreFoundation exports data a guest reaches by address, and the address is
 * the part that matters.  A CFSTR literal is a structure the compiler lays down
 * in the guest's own __cfstring section, and its first word is bound to
 * ___CFConstantStringClassReference: that word is the isa by which native
 * CoreFoundation and the Objective-C runtime recognize the literal as a string
 * at all.  kCFTypeArrayCallBacks and its dictionary siblings are handed over by
 * address, and CoreFoundation may compare the pointer it is given against the
 * address of its own.  A slot here holding a copy of either is a second object
 * at a second address: an isa naming a class nobody registered, and a callbacks
 * structure whose address nothing recognizes.  Some names on the list would
 * survive a copy - kCFBooleanTrue and kCFRunLoopDefaultMode are constant
 * pointers to objects that never move - but one rule for every native name is
 * simpler than deciding name by name, and costs nothing.
 *
 * So there is a third kind of export, native data, and its trie terminal is the
 * host variable's own address with EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE set.  The
 * address is what the host lookup, ocerz_bridge_host_symbol unless the caller
 * supplies another, answers for the name without its leading underscore in the
 * library whose install name the row carries.  The trie walker returns an
 * absolute terminal's value as it stands instead of adding the load address,
 * so every bind path that asks it - bind opcodes and chained fixups alike -
 * writes the native address into the guest's GOT entry or isa word.  That
 * address is usable from guest code only because the loader asks for a
 * virtual image in native mode alone, and native mode runs in the identity
 * map, where a host address is a guest address.  A native export takes no
 * stub, no slot, no export id and no byte of either segment: its trie entry is
 * all of it.
 *
 * A name the host lookup does not find is left out of the trie, and said once
 * per process through OCERZ_LOG however many times the image is built.  An
 * absolute export of zero would bind the guest's reference to a null pointer
 * that faults somewhere far from the import that caused it; an export that is
 * absent puts the name in the loader's unresolved-import report, and the run
 * stops with 71 before the guest has executed an instruction.
 *
 * ---- export ids ----
 * The first image built mints an id for every function of every row, in table
 * order, and later builds only look them up.  Minting as each image is built
 * would number a library's exports by whichever library a guest happened to
 * name first - a program linking CoreFoundation lists it ahead of libSystem -
 * so the same libSystem would carry different ids, and be different bytes,
 * from one guest to the next.  Minted up front, libSystem's stubs are the same
 * in every process and an id means the same export in every run.
 *
 * ---- what happens after the trap ----
 * The trap lands in ocerz_vdylib_dispatch, which turns the export id back into
 * the library and symbol it was minted from and asks the bridge whether it
 * knows how to perform that call for real.  If it does, the call is made there
 * and the guest goes on with the result in the registers x86 code expects.  If
 * it does not, the export names itself and the run stops, which is what every
 * export did before the bridge existed and what most of them still do; an
 * export the bridge has no descriptor for is not a failure, only one nobody
 * has written the crossing for yet.
 *
 * The descriptor is cached in the export's own table entry, together with the
 * fact that the lookup has been made at all.  The bridge resolves by comparing
 * strings, and this dispatch is on the path of every call a guest makes into a
 * virtual library, so resolving per call would put a string search in front of
 * memcpy - a cost that hides in a microbenchmark and does not hide in a
 * program that calls memcpy a million times.  A lookup that comes back empty is
 * remembered as empty for the same reason, so a name the bridge has already
 * said it does not have is never searched for twice.
 *
 * ---- the one stub that keeps r11 ----
 * An ordinary export's stub loads its id into r11, which is free to do: r11 is
 * a scratch register in the System V ABI and every linker stub on the platform
 * clobbers it.  __tlv_bootstrap is not an ordinary function.  It is the thunk a
 * thread-local variable's descriptor calls, and that calling convention keeps
 * every register but rax, so clang does hold live values in r11 across a
 * thread-local access; a program that loaded one thread-local into r11 and then
 * touched another read back garbage.  Its stub therefore pushes r11 before
 * loading the id, which still fits the sixteen-byte stride, and the bridge puts
 * r11 back from the stack before it returns.
 *
 * ---- dyld_stub_binder ----
 * A binary linked with classic lazy binding, which is every Intel binary built
 * for a macOS older than 12, imports dyld_stub_binder from libSystem whether or
 * not it ever reaches it: its __stub_helper entries jump there to bind a lazy
 * pointer on first call.  The loader applies lazy binds eagerly, so no helper
 * ever runs, but the import still has to bind or the whole program is refused
 * with 71 before its first instruction.  Its name is the one symbol here with
 * no leading underscore, because dyld defines it in assembly rather than in C.
 * So libSystem exports it as an ordinary
 * stub with no bridge behind it; a guest that did reach it would name it and
 * stop, which is the honest answer to a lazy bind the loader failed to make.
 */
#include "ocerz/vdylib.h"
#include "ocerz/bridge.h"
#include "ocerz/dyldapi.h"
#include "ocerz/interp.h"

#include <stdlib.h>
#include <mach-o/loader.h>

#define VD_PAGE 4096u
#define VD_STUB_STRIDE 16u
#define VD_STUB_BYTES 12u
#define VD_SLOT_BYTES 8u
#define VD_ULEB_WIDTH 5u
#define VD_ULEB_ABS_WIDTH 10u

#define VD_SYMS_MAX 512
#define VD_NODE_MAX (2 * VD_SYMS_MAX + 2)
#define VD_EDGE_MAX 96
#define VD_EXPORT_MAX 4096
#define VD_VAR_BYTES_MAX (1u << 20)

static const char *const vd_libsystem_syms[] = {
    "___bzero",
    "___error",
    "___stack_chk_fail",
    "__tlv_bootstrap",
    "___memcpy_chk",
    "___memmove_chk",
    "___memset_chk",
    "___strcpy_chk",
    "___stpcpy_chk",
    "___strcat_chk",
    "___strncpy_chk",
    "___stpncpy_chk",
    "___strncat_chk",
    "___strlcpy_chk",
    "___strlcat_chk",
    "_memcpy",
    "_memcmp",
    "_memmove",
    "_memset",
    "_memchr",
    "_strcmp",
    "_strncmp",
    "_strcpy",
    "_strncpy",
    "_strlen",
    "_strnlen",
    "_strcat",
    "_strncat",
    "_strchr",
    "_strrchr",
    "_strstr",
    "_strdup",
    "_strndup",
    "_strtol",
    "_strtoul",
    "_strtod",
    "_strerror",
    "_fopen",
    "_fclose",
    "_fread",
    "_fwrite",
    "_fflush",
    "_fprintf",
    "_fputs",
    "_fputc",
    "_fgets",
    "_printf",
    "_puts",
    "_putchar",
    "_snprintf",
    "_sprintf",
    "_vsnprintf",
    "_vfprintf",
    "_perror",
    "_malloc",
    "_calloc",
    "_realloc",
    "_free",
    "_exit",
    "_abort",
    "_atexit",
    "_getenv",
    "_setenv",
    "_unsetenv",
    "_qsort",
    "_bsearch",
    "_abs",
    "_labs",
    "_atoi",
    "_atol",
    "_atof",
    "_rand",
    "_srand",
    "_write",
    "_writev",
    "_read",
    "_readv",
    "_readdir",
    "_open",
    "_opendir",
    "_close",
    "_closedir",
    "_lseek",
    "_unlink",
    "_mkdir",
    "_rmdir",
    "_rename",
    "_access",
    "_dup",
    "_dup2",
    "_pipe",
    "_fcntl",
    "_ioctl",
    "_isatty",
    "_getpid",
    "_getppid",
    "_getuid",
    "_geteuid",
    "_mmap",
    "_munmap",
    "_mprotect",
    "_madvise",
    "_time",
    "_times",
    "_clock",
    "_clock_gettime",
    "_gettimeofday",
    "_nanosleep",
    "_sleep",
    "_usleep",
    "_mktime",
    "_localtime",
    "_gmtime",
    "_strftime",
    "_pthread_create",
    "_pthread_join",
    "_pthread_detach",
    "_pthread_self",
    "_pthread_mutex_init",
    "_pthread_mutex_lock",
    "_pthread_mutex_unlock",
    "_pthread_mutex_destroy",
    "_pthread_cond_init",
    "_pthread_cond_wait",
    "_pthread_cond_signal",
    "_pthread_cond_broadcast",
    "_pthread_cond_destroy",
    "_dispatch_get_global_queue",
    "_dispatch_async_f",
    "_dispatch_sync_f",
    "_dispatch_apply_f",
    "_dispatch_semaphore_create",
    "_dispatch_semaphore_wait",
    "_dispatch_semaphore_signal",
    "_dispatch_release",
    "_dlopen",
    "_dlsym",
    "_dlclose",
    "_dlerror",
    "_dladdr",
    "_signal",
    "_sigaction",
    "_raise",
    "_kill",
    "_sigprocmask",
    "_pthread_sigmask",
    "_sigaltstack",
    "_pthread_kill",
    "_sigemptyset",
    "_sigfillset",
    "_sigaddset",
    "_sigdelset",
    "_sigismember",
    "dyld_stub_binder",
};

typedef struct VdVar {
    const char *name;
    uint32_t size;
    void (*fill)(uint8_t *slot, uint32_t size);
} VdVar;

static void vd_fill_stack_guard(uint8_t *slot, uint32_t size)
{
    int live = 0;
    while (!live && size > 1) {
        arc4random_buf(slot + 1, size - 1);
        for (uint32_t i = 1; i < size; i++)
            live |= slot[i] != 0;
    }
    slot[0] = 0;
}

static const VdVar vd_libsystem_vars[] = {
    { "___stack_chk_guard", 8, vd_fill_stack_guard },
};

static const char *const vd_corefoundation_syms[] = {
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

static const char *const vd_corefoundation_natives[] = {
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

typedef struct VdLib {
    const char *install_name;
    const char *const *syms;
    int nsyms;
    const VdVar *vars;
    int nvars;
    const char *const *natives;
    int nnatives;
} VdLib;

#define VD_COUNT(a) ((int)(sizeof (a) / sizeof (a)[0]))

static const VdLib g_vd_libs[] = {
    { OCERZ_BRIDGE_LIBSYSTEM,
      vd_libsystem_syms, VD_COUNT(vd_libsystem_syms),
      vd_libsystem_vars, VD_COUNT(vd_libsystem_vars),
      NULL, 0 },
    { OCERZ_BRIDGE_COREFOUNDATION,
      vd_corefoundation_syms, VD_COUNT(vd_corefoundation_syms),
      NULL, 0,
      vd_corefoundation_natives, VD_COUNT(vd_corefoundation_natives) },
};

#define VD_LIBS_N VD_COUNT(g_vd_libs)

static uint8_t g_vd_native_missed[VD_LIBS_N][VD_SYMS_MAX];

typedef struct VdExport {
    const char *lib;
    const char *sym;
    const struct OcerzBridgeFn *fn;
    int resolved;
} VdExport;

static VdExport g_vd_exports[VD_EXPORT_MAX];
static int g_vd_exports_n;

static const VdLib *vd_lib_for(const char *install_name)
{
    if (!install_name)
        return NULL;
    for (int i = 0; i < VD_LIBS_N; i++)
        if (strcmp(g_vd_libs[i].install_name, install_name) == 0)
            return &g_vd_libs[i];
    return NULL;
}

int ocerz_vdylib_have(const char *install_name)
{
    return vd_lib_for(install_name) != NULL;
}

static int vd_export_id(const char *lib, const char *sym)
{
    for (int i = 0; i < g_vd_exports_n; i++)
        if (strcmp(g_vd_exports[i].lib, lib) == 0 && strcmp(g_vd_exports[i].sym, sym) == 0)
            return i;
    if (g_vd_exports_n >= VD_EXPORT_MAX)
        return -1;
    g_vd_exports[g_vd_exports_n].lib = lib;
    g_vd_exports[g_vd_exports_n].sym = sym;
    g_vd_exports[g_vd_exports_n].fn = NULL;
    g_vd_exports[g_vd_exports_n].resolved = 0;
    return g_vd_exports_n++;
}

static int vd_mint_ids(void)
{
    static int minted;
    if (minted)
        return 1;
    for (int l = 0; l < VD_LIBS_N; l++)
        for (int i = 0; i < g_vd_libs[l].nsyms; i++)
            if (vd_export_id(g_vd_libs[l].install_name, g_vd_libs[l].syms[i]) < 0)
                return 0;
    minted = 1;
    return 1;
}

int ocerz_vdylib_export_name(uint64_t id, const char **lib_out, const char **sym_out)
{
    if (id >= (uint64_t)g_vd_exports_n)
        return 0;
    if (lib_out)
        *lib_out = g_vd_exports[id].lib;
    if (sym_out)
        *sym_out = g_vd_exports[id].sym;
    return 1;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t vd_round_up(uint32_t v, uint32_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static void vd_uleb_fixed(uint8_t *p, uint64_t v, uint32_t width)
{
    for (uint32_t i = 0; i + 1 < width; i++)
        p[i] = (uint8_t)(((v >> (7 * i)) & 0x7f) | 0x80);
    p[width - 1] = (uint8_t)((v >> (7 * (width - 1))) & 0x7f);
}

static uint32_t vd_term_width(int absolute)
{
    return absolute ? VD_ULEB_ABS_WIDTH : VD_ULEB_WIDTH;
}

typedef struct VdSym {
    const char *name;
    uint64_t addr;
    int absolute;
} VdSym;

typedef struct VdNode {
    char edge[VD_EDGE_MAX];
    int first_child;
    int next_sibling;
    int terminal;
    int absolute;
    uint64_t addr;
    uint32_t off;
    uint32_t size;
} VdNode;

typedef struct VdTrie {
    VdNode *node;
    int n;
    int overflow;
    const VdSym *sym;
} VdTrie;

static int vd_sym_cmp(const void *a, const void *b)
{
    const VdSym *x = a;
    const VdSym *y = b;
    return strcmp(x->name, y->name);
}

static int vd_node_new(VdTrie *t)
{
    if (t->n >= VD_NODE_MAX) {
        t->overflow = 1;
        return -1;
    }
    int i = t->n++;
    VdNode *nd = &t->node[i];
    nd->edge[0] = '\0';
    nd->first_child = -1;
    nd->next_sibling = -1;
    nd->terminal = 0;
    nd->absolute = 0;
    nd->addr = 0;
    nd->off = 0;
    nd->size = 0;
    return i;
}

static int vd_trie_build(VdTrie *t, int lo, int hi, size_t depth)
{
    int me = vd_node_new(t);
    if (me < 0)
        return -1;

    int i = lo;
    if (strlen(t->sym[lo].name) == depth) {
        t->node[me].terminal = 1;
        t->node[me].absolute = t->sym[lo].absolute;
        t->node[me].addr = t->sym[lo].addr;
        i = lo + 1;
    }

    int last = -1;
    while (i < hi) {
        int j = i + 1;
        while (j < hi && t->sym[j].name[depth] == t->sym[i].name[depth])
            j++;
        const char *a = t->sym[i].name;
        const char *b = t->sym[j - 1].name;
        size_t lcp = depth;
        while (a[lcp] != '\0' && a[lcp] == b[lcp])
            lcp++;
        size_t elen = lcp - depth;
        if (elen == 0 || elen >= VD_EDGE_MAX) {
            t->overflow = 1;
            return -1;
        }
        int kid = vd_trie_build(t, i, j, lcp);
        if (kid < 0)
            return -1;
        memcpy(t->node[kid].edge, a + depth, elen);
        t->node[kid].edge[elen] = '\0';
        if (last < 0)
            t->node[me].first_child = kid;
        else
            t->node[last].next_sibling = kid;
        last = kid;
        i = j;
    }
    return me;
}

static uint32_t vd_trie_layout(VdTrie *t)
{
    for (int i = 0; i < t->n; i++) {
        uint32_t sz = 1;
        int kids = 0;
        if (t->node[i].terminal)
            sz += 1u + vd_term_width(t->node[i].absolute);
        sz += 1;
        for (int c = t->node[i].first_child; c >= 0; c = t->node[c].next_sibling) {
            sz += (uint32_t)strlen(t->node[c].edge) + 1 + VD_ULEB_WIDTH;
            kids++;
        }
        if (kids > 255) {
            t->overflow = 1;
            return 0;
        }
        t->node[i].size = sz;
    }
    uint32_t off = 0;
    for (int i = 0; i < t->n; i++) {
        t->node[i].off = off;
        off += t->node[i].size;
    }
    return off;
}

static void vd_trie_emit(const VdTrie *t, uint8_t *out)
{
    for (int i = 0; i < t->n; i++) {
        const VdNode *nd = &t->node[i];
        uint8_t *p = out + nd->off;
        if (nd->terminal) {
            uint32_t w = vd_term_width(nd->absolute);
            *p++ = (uint8_t)(1u + w);
            *p++ = nd->absolute ? EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE : 0;
            vd_uleb_fixed(p, nd->addr, w);
            p += w;
        } else {
            *p++ = 0;
        }
        uint8_t kids = 0;
        for (int c = nd->first_child; c >= 0; c = t->node[c].next_sibling)
            kids++;
        *p++ = kids;
        for (int c = nd->first_child; c >= 0; c = t->node[c].next_sibling) {
            size_t elen = strlen(t->node[c].edge);
            memcpy(p, t->node[c].edge, elen + 1);
            p += elen + 1;
            vd_uleb_fixed(p, t->node[c].off, VD_ULEB_WIDTH);
            p += VD_ULEB_WIDTH;
        }
    }
}

static void vd_write_segment(uint8_t *p, uint32_t cmdsize, const char *name,
                             uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                             uint64_t filesize, uint32_t prot, uint32_t nsects)
{
    wr32(p + 0, LC_SEGMENT_64);
    wr32(p + 4, cmdsize);
    memcpy(p + 8, name, strlen(name));
    wr64(p + 24, vmaddr);
    wr64(p + 32, vmsize);
    wr64(p + 40, fileoff);
    wr64(p + 48, filesize);
    wr32(p + 56, prot);
    wr32(p + 60, prot);
    wr32(p + 64, nsects);
    wr32(p + 68, 0);
}

static void vd_write_section(uint8_t *p, const char *sect, const char *seg,
                             uint64_t addr, uint64_t size, uint32_t off,
                             uint32_t align, uint32_t flags)
{
    memcpy(p + 0, sect, strlen(sect));
    memcpy(p + 16, seg, strlen(seg));
    wr64(p + 32, addr);
    wr64(p + 40, size);
    wr32(p + 48, off);
    wr32(p + 52, align);
    wr32(p + 56, 0);
    wr32(p + 60, 0);
    wr32(p + 64, flags);
    wr32(p + 68, 0);
    wr32(p + 72, 0);
    wr32(p + 76, 0);
}

uint8_t *ocerz_vdylib_image_with(const char *install_name, OcerzVdylibHostSym host_sym,
                                 size_t *len_out)
{
    const VdLib *lib = vd_lib_for(install_name);
    if (!lib)
        return NULL;
    if (!host_sym)
        host_sym = ocerz_bridge_host_symbol;
    if (!vd_mint_ids()) {
        OCERZ_FATAL("virtual export table is full at %d entries\n", VD_EXPORT_MAX);
        return NULL;
    }

    int n = lib->nsyms;
    int nv = lib->nvars;
    int nn = lib->nnatives;
    if (n <= 0 || nv < 0 || nn < 0 || n > VD_SYMS_MAX - nv - nn) {
        OCERZ_FATAL("virtual %s declares %d exports, the limit is %d\n",
                    lib->install_name, n + nv + nn, VD_SYMS_MAX);
        return NULL;
    }

    uint32_t name_len = (uint32_t)strlen(lib->install_name) + 1;
    uint32_t id_cmdsize = vd_round_up((uint32_t)sizeof(struct dylib_command) + name_len, 8);
    uint32_t seg_cmdsize = (uint32_t)(sizeof(struct segment_command_64) + sizeof(struct section_64));
    uint32_t trie_cmdsize = (uint32_t)sizeof(struct linkedit_data_command);
    uint32_t sizeofcmds = 2 * seg_cmdsize + id_cmdsize + trie_cmdsize;
    uint32_t hdr_size = (uint32_t)sizeof(struct mach_header_64);

    uint32_t stubs_off = vd_round_up(hdr_size + sizeofcmds, VD_STUB_STRIDE);
    uint32_t stubs_size = (uint32_t)n * VD_STUB_STRIDE;
    uint32_t text_size = vd_round_up(stubs_off + stubs_size, VD_PAGE);
    uint32_t slots_off = text_size;
    uint32_t slots_size = (uint32_t)n * VD_SLOT_BYTES;
    uint32_t vars_off = vd_round_up(slots_off + slots_size, VD_SLOT_BYTES);

    uint32_t *ids = calloc((size_t)n, sizeof *ids);
    uint32_t *var_addr = calloc((size_t)nv + 1, sizeof *var_addr);
    VdSym *syms = calloc((size_t)(n + nv + nn), sizeof *syms);
    VdNode *nodes = calloc(VD_NODE_MAX, sizeof *nodes);
    uint8_t *buf = NULL;
    if (!ids || !var_addr || !syms || !nodes) {
        OCERZ_FATAL("out of memory building virtual %s\n", lib->install_name);
        goto fail;
    }

    uint32_t vars_end = vars_off;
    for (int j = 0; j < nv; j++) {
        const VdVar *v = &lib->vars[j];
        if (strlen(v->name) + 1 > VD_EDGE_MAX) {
            OCERZ_FATAL("virtual export %s is longer than the %d-byte edge limit\n",
                        v->name, VD_EDGE_MAX);
            goto fail;
        }
        if (v->size == 0 || v->size > VD_VAR_BYTES_MAX) {
            OCERZ_FATAL("virtual data export %s is %u bytes, it must be 1 to %u\n",
                        v->name, (unsigned)v->size, (unsigned)VD_VAR_BYTES_MAX);
            goto fail;
        }
        var_addr[j] = vars_end;
        vars_end += vd_round_up(v->size, VD_SLOT_BYTES);
        syms[n + j].name = v->name;
        syms[n + j].addr = var_addr[j];
        syms[n + j].absolute = 0;
    }
    uint32_t data_used = vars_end - slots_off;
    uint32_t data_size = vd_round_up(data_used, VD_PAGE);
    uint32_t trie_off = slots_off + data_size;

    for (int i = 0; i < n; i++) {
        const char *sym = lib->syms[i];
        if (strlen(sym) + 1 > VD_EDGE_MAX) {
            OCERZ_FATAL("virtual export %s is longer than the %d-byte edge limit\n",
                        sym, VD_EDGE_MAX);
            goto fail;
        }
        int id = vd_export_id(lib->install_name, sym);
        if (id < 0) {
            OCERZ_FATAL("virtual export table is full at %d entries, cannot add %s %s\n",
                        VD_EXPORT_MAX, lib->install_name, sym);
            goto fail;
        }
        ids[i] = (uint32_t)id;
        syms[i].name = sym;
        syms[i].addr = stubs_off + (uint32_t)i * VD_STUB_STRIDE;
        syms[i].absolute = 0;
    }

    int m = n + nv;
    int missed = 0;
    for (int k = 0; k < nn; k++) {
        const char *sym = lib->natives[k];
        if (sym[0] != '_' || sym[1] == '\0' || strlen(sym) + 1 > VD_EDGE_MAX) {
            OCERZ_FATAL("virtual native export %s is not an underscored name under the %d-byte "
                        "edge limit\n", sym, VD_EDGE_MAX);
            goto fail;
        }
        void *host = host_sym(lib->install_name, sym + 1);
        if (!host) {
            uint8_t *seen = &g_vd_native_missed[lib - g_vd_libs][k];
            if (!*seen) {
                *seen = 1;
                OCERZ_LOG("vdylib: host %s has no %s, so virtual %s does not export %s\n",
                          lib->install_name, sym + 1, lib->install_name, sym);
            }
            missed++;
            continue;
        }
        syms[m].name = sym;
        syms[m].addr = (uint64_t)(uintptr_t)host;
        syms[m].absolute = 1;
        m++;
    }

    qsort(syms, (size_t)m, sizeof *syms, vd_sym_cmp);
    for (int i = 1; i < m; i++) {
        if (strcmp(syms[i - 1].name, syms[i].name) == 0) {
            OCERZ_FATAL("virtual %s exports %s twice\n", lib->install_name, syms[i].name);
            goto fail;
        }
    }

    VdTrie trie;
    trie.node = nodes;
    trie.n = 0;
    trie.overflow = 0;
    trie.sym = syms;
    if (vd_trie_build(&trie, 0, m, 0) != 0 || trie.overflow) {
        OCERZ_FATAL("virtual %s does not fit the export trie limits\n", lib->install_name);
        goto fail;
    }
    uint32_t trie_size = vd_trie_layout(&trie);
    if (trie.overflow || trie_size == 0) {
        OCERZ_FATAL("virtual %s does not fit the export trie limits\n", lib->install_name);
        goto fail;
    }

    uint32_t total = trie_off + trie_size;
    buf = calloc(1, total);
    if (!buf) {
        OCERZ_FATAL("out of memory building virtual %s\n", lib->install_name);
        goto fail;
    }

    wr32(buf + 0, MH_MAGIC_64);
    wr32(buf + 4, (uint32_t)CPU_TYPE_X86_64);
    wr32(buf + 8, (uint32_t)CPU_SUBTYPE_X86_64_ALL);
    wr32(buf + 12, MH_DYLIB);
    wr32(buf + 16, 4);
    wr32(buf + 20, sizeofcmds);
    wr32(buf + 24, MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL);
    wr32(buf + 28, 0);

    uint8_t *lc = buf + hdr_size;

    vd_write_segment(lc, seg_cmdsize, "__TEXT", 0, text_size, 0, text_size,
                     VM_PROT_READ | VM_PROT_EXECUTE, 1);
    vd_write_section(lc + sizeof(struct segment_command_64), "__text", "__TEXT",
                     stubs_off, stubs_size, stubs_off, 4,
                     S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    lc += seg_cmdsize;

    vd_write_segment(lc, seg_cmdsize, "__DATA", slots_off, data_size, slots_off, data_size,
                     VM_PROT_READ | VM_PROT_WRITE, 1);
    vd_write_section(lc + sizeof(struct segment_command_64), "__data", "__DATA",
                     slots_off, data_used, slots_off, 3, S_REGULAR);
    lc += seg_cmdsize;

    wr32(lc + 0, LC_ID_DYLIB);
    wr32(lc + 4, id_cmdsize);
    wr32(lc + 8, (uint32_t)sizeof(struct dylib_command));
    wr32(lc + 12, 1);
    wr32(lc + 16, 0x10000);
    wr32(lc + 20, 0x10000);
    memcpy(lc + sizeof(struct dylib_command), lib->install_name, name_len);
    lc += id_cmdsize;

    wr32(lc + 0, LC_DYLD_EXPORTS_TRIE);
    wr32(lc + 4, trie_cmdsize);
    wr32(lc + 8, trie_off);
    wr32(lc + 12, trie_size);

    vd_trie_emit(&trie, buf + trie_off);

    for (int i = 0; i < n; i++) {
        uint32_t stub_addr = stubs_off + (uint32_t)i * VD_STUB_STRIDE;
        uint32_t slot_addr = slots_off + (uint32_t)i * VD_SLOT_BYTES;
        uint8_t *s = buf + stub_addr;
        uint32_t at = 0;
        if (strcmp(lib->syms[i], "__tlv_bootstrap") == 0) {
            s[at++] = 0x41;
            s[at++] = 0x53;
        }
        s[at++] = 0x41;
        s[at++] = 0xbb;
        wr32(s + at, ids[i]);
        at += 4;
        s[at++] = 0xff;
        s[at++] = 0x25;
        int64_t rel = (int64_t)slot_addr - (int64_t)(stub_addr + at + 4);
        wr32(s + at, (uint32_t)(int32_t)rel);
        at += 4;
        for (uint32_t k = at; k < VD_STUB_STRIDE; k++)
            s[k] = 0xcc;
        wr64(buf + slot_addr, OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF);
    }

    for (int j = 0; j < nv; j++)
        if (lib->vars[j].fill)
            lib->vars[j].fill(buf + var_addr[j], lib->vars[j].size);

    OCERZ_LOG("vdylib: built %s with %d exports (%d data, %d native, %d native missing), "
              "%u bytes (text %u data %u trie %u at %u)\n",
              lib->install_name, m, nv, m - n - nv, missed, (unsigned)total,
              (unsigned)text_size, (unsigned)data_size, (unsigned)trie_size,
              (unsigned)trie_off);

    free(ids);
    free(var_addr);
    free(syms);
    free(nodes);
    if (len_out)
        *len_out = total;
    return buf;

fail:
    free(ids);
    free(var_addr);
    free(syms);
    free(nodes);
    free(buf);
    return NULL;
}

uint8_t *ocerz_vdylib_image(const char *install_name, size_t *len_out)
{
    return ocerz_vdylib_image_with(install_name, ocerz_bridge_host_symbol, len_out);
}

int ocerz_vdylib_dispatch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int hooked = -1;
    if (hooked < 0) {
        hooked = 0;
        if (getenv("OCERZ_BRIDGESTAT"))
            atexit(ocerz_bridge_report);
    }

    uint64_t id = cpu->gpr[OCERZ_R11] & 0xffffffffull;

    if (id >= (uint64_t)g_vd_exports_n) {
        fprintf(stderr, "ocerz: bridge: export id %llu is not one of the %d synthesized exports\n",
                (unsigned long long)id, g_vd_exports_n);
        exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    }

    VdExport *e = &g_vd_exports[id];
    if (!e->resolved) {
        e->fn = ocerz_bridge_lookup(e->lib, e->sym);
        e->resolved = 1;
    }
    if (e->fn)
        return ocerz_bridge_invoke(vm, cpu, e->fn);

    fprintf(stderr, "ocerz: bridge: %s %s not implemented\n", e->lib, e->sym);

    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    return OCERZ_STEP_OK;
}
