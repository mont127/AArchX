/*
 * IOKit's registry calls that name a plane, a class or a path with an io_name_t or io_string_t (include/ocerz/m32.h).
 *
 * Those are char arrays, which the i386 database cannot describe: it marked the calls unsupported-type, and they
 * answered 0 - success - writing nothing.  Wine's Mac driver finds a display's GPU by climbing from its registry entry
 * to the IOPCIDevice above it with IORegistryEntryGetParentEntry until the call fails at the root, and given success and
 * no parent it climbed forever (explorer.exe /desktop at 100% CPU).  The arrays are the guest's own bytes either way,
 * and an io_object_t is a 32-bit Mach port name on both sides, so each call is made as it stands with its names and its
 * out-words in guest memory.  Only the CFTypeRefs cross: a key or an allocator in, a property or a path out.  The
 * notification calls, whose callbacks would need the guest's, are not here.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(n) m32_arg(cpu, n)
#define G(n) ((void *)(A(n) ? m32_h(A(n)) : NULL))   /* a guest name or out-word, as a pointer the host writes */

typedef int (*EntryPlaneOut)(uint32_t, const char *, void *);
typedef int (*EntryOut)(uint32_t, void *);
typedef int (*EntryPlaneOptsOut)(uint32_t, const char *, uint32_t, void *);
typedef int (*EntryName)(uint32_t, const char *);
typedef uint32_t (*PortPath)(uint32_t, const char *);
typedef CFStringRef (*EntryPlaneCF)(uint32_t, const char *);
typedef CFTypeRef (*SearchCF)(uint32_t, const char *, CFStringRef, CFAllocatorRef, uint32_t);

enum { PARENT, CHILD, PARENT_ITER, CHILD_ITER, NAME_IN_PLANE, LOCATION, PATH, CREATE_ITER, REGISTRY_ITER, NAME,
       CLASS, IN_PLANE, CONFORMS, FROM_PATH, COPY_PATH, SEARCH, NFN };
static const char *const g_names[NFN] = {
    "IORegistryEntryGetParentEntry", "IORegistryEntryGetChildEntry", "IORegistryEntryGetParentIterator",
    "IORegistryEntryGetChildIterator", "IORegistryEntryGetNameInPlane", "IORegistryEntryGetLocationInPlane",
    "IORegistryEntryGetPath", "IORegistryEntryCreateIterator", "IORegistryCreateIterator", "IORegistryEntryGetName",
    "IOObjectGetClass", "IORegistryEntryInPlane", "IOObjectConformsTo", "IORegistryEntryFromPath",
    "IORegistryEntryCopyPath", "IORegistryEntrySearchCFProperty",
};
static void *g_fn[NFN];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void load(void)
{
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
    for (int i = 0; io && i < NFN; i++)
        g_fn[i] = dlsym(io, g_names[i]);
}

static void *fn(int which)
{
    pthread_once(&g_once, load);
    return g_fn[which];
}

#define KERN_FAILURE_ 5
#define CALL(type, which, ...) do { type f_ = (type)fn(which); RET(f_ ? f_(__VA_ARGS__) : KERN_FAILURE_); } while (0)

/* (entry, plane, out): the parent or child entry, an iterator over them, a name, a location or a path */
static int sp_parent(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, PARENT, A(0), G(1), G(2)); }
static int sp_child(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, CHILD, A(0), G(1), G(2)); }
static int sp_parent_iter(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, PARENT_ITER, A(0), G(1), G(2)); }
static int sp_child_iter(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, CHILD_ITER, A(0), G(1), G(2)); }
static int sp_name_in_plane(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, NAME_IN_PLANE, A(0), G(1), G(2)); }
static int sp_location(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, LOCATION, A(0), G(1), G(2)); }
static int sp_path(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOut, PATH, A(0), G(1), G(2)); }
/* (entry or main port, plane, options, iterator out) */
static int sp_create_iter(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOptsOut, CREATE_ITER, A(0), G(1), A(2), G(3)); }
static int sp_registry_iter(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryPlaneOptsOut, REGISTRY_ITER, A(0), G(1), A(2), G(3)); }
/* (object, name out) */
static int sp_name(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryOut, NAME, A(0), G(1)); }
static int sp_class(struct OcerzVM *vm, OcerzCPU *cpu) { CALL(EntryOut, CLASS, A(0), G(1)); }

/* boolean_t (object, name): false when IOKit is not there */
static int sp_in_plane(struct OcerzVM *vm, OcerzCPU *cpu)
{
    EntryName f = (EntryName)fn(IN_PLANE);
    RET(f ? f(A(0), G(1)) : 0);
}
static int sp_conforms(struct OcerzVM *vm, OcerzCPU *cpu)
{
    EntryName f = (EntryName)fn(CONFORMS);
    RET(f ? f(A(0), G(1)) : 0);
}

/* io_registry_entry_t (main port, path): MACH_PORT_NULL when there is no such entry */
static int sp_from_path(struct OcerzVM *vm, OcerzCPU *cpu)
{
    PortPath f = (PortPath)fn(FROM_PATH);
    RET(f ? f(A(0), G(1)) : 0);
}

static int sp_copy_path(struct OcerzVM *vm, OcerzCPU *cpu)
{
    EntryPlaneCF f = (EntryPlaneCF)fn(COPY_PATH);
    RET(f ? m32_handle((void *)f(A(0), G(1))) : 0);
}

/* (entry, plane, key, allocator, options): a property of the entry or of one above or below it in the plane */
static int sp_search(struct OcerzVM *vm, OcerzCPU *cpu)
{
    SearchCF f = (SearchCF)fn(SEARCH);
    RET(f ? m32_handle((void *)f(A(0), G(1), (CFStringRef)m32_host(A(2)), (CFAllocatorRef)m32_host(A(3)), A(4))) : 0);
}

const M32SpecialEntry m32_iokit_specials[] = {
    { "_IORegistryEntryGetParentEntry", sp_parent }, { "_IORegistryEntryGetChildEntry", sp_child },
    { "_IORegistryEntryGetParentIterator", sp_parent_iter }, { "_IORegistryEntryGetChildIterator", sp_child_iter },
    { "_IORegistryEntryGetNameInPlane", sp_name_in_plane }, { "_IORegistryEntryGetLocationInPlane", sp_location },
    { "_IORegistryEntryGetPath", sp_path }, { "_IORegistryEntryCreateIterator", sp_create_iter },
    { "_IORegistryCreateIterator", sp_registry_iter }, { "_IORegistryEntryGetName", sp_name },
    { "_IOObjectGetClass", sp_class }, { "_IORegistryEntryInPlane", sp_in_plane },
    { "_IOObjectConformsTo", sp_conforms }, { "_IORegistryEntryFromPath", sp_from_path },
    { "_IORegistryEntryCopyPath", sp_copy_path }, { "_IORegistryEntrySearchCFProperty", sp_search },
    { NULL, NULL }
};
