/*
 * Building the synthesized x86_64 system libraries.
 *
 * ---- what an image is made of ----
 * Nothing about a library is compiled in here.  ocerz_apidb_library hands back
 * the parsed database file for an install name, and the builder walks its
 * entries in file order: a fn, special or stub record becomes a function stub,
 * a var record becomes a slot in __DATA filled by a filler this file knows by
 * name, and a data record becomes an absolute export naming the host's own
 * variable.  shape and struct records are the bridge's business and never reach
 * the image.  ocerz_vdylib_have is exactly the question whether native mode has
 * a database file for the install name, so a library is synthesized precisely
 * when there is a file describing it.
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
 * Every size is computed from the entries.  __TEXT is as many pages as the
 * header, the load commands and one stub per function take, __DATA as many as
 * the jump slots and the var slots take and never less than one, and the trie
 * as many bytes as its nodes need, so a library of five thousand exports is
 * built the same way as one of fifty.  The only bound is the one the
 * formats impose: an image is addressed with 32-bit file offsets, and an export
 * id carries its entry index in twenty bits.
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
 * which carry 35 bits, more than a 32-bit file offset can reach.  A native
 * data terminal carries a host address instead, and the image's size puts no
 * bound on that: the host's libraries sit wherever the kernel mapped them, and
 * an address past 2^35 written into five bytes would lose its top bits without
 * a word of complaint.  Those terminals are ten bytes, which is every bit a
 * 64-bit value has, the tenth byte holding bit 63 alone.  With every width
 * known up front - a terminal's kind comes from the record, and no width
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
 * - becomes an interior node that also carries a terminal.  An edge is a span
 * of the name it came from rather than a copy, so no name is too long to be an
 * edge, and a trie over n names has at most 2n + 1 nodes, which is what is
 * allocated for it.
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
 * So a var record is a name, a size and a filler, given a slot in __DATA after
 * the last jump slot, each slot starting on an 8-byte boundary, with __DATA's
 * size taking the slots in and the trie moving past them.  Its trie terminal
 * is the slot's offset from the image start, the same thing a function's
 * terminal is for its stub, so the trie walker and the loader cannot tell the
 * two kinds apart and neither needed a change: the guest's GOT entry is bound
 * to the slot's address and the guest reads through it.  A var export has no
 * stub and no export id, because nothing ever branches to it.  The filler is
 * named in the record and looked up in a table here, because what fills a slot
 * is code; a name the table does not have refuses the image rather than
 * leaving the slot zero, since a record that asked for a canary and got zeroes
 * would be quietly weaker than it says.
 *
 * ---- the canary ----
 * The stack_guard filler draws ___stack_chk_guard from arc4random_buf every
 * time an image is built.  It does not have to agree with the host's own guard
 * or with anything else: the only thing a guest ever compares it with is the
 * copy its own prologue took, and the handler behind ___stack_chk_fail stops
 * the run without comparing anything.  The loader builds a given install name
 * once, so a running guest only ever sees one value.
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
 * ---- the data symbols that must not be slots ----
 * libSystem has other data that compiled programs reach for directly: _environ,
 * ___progname, __DefaultRuneLocale, and ___stdoutp and ___stderrp, which are
 * what stdout and stderr expand to.  None of them can be a var record, because
 * each is a variable native code also reads or writes on its own account, and
 * a guest slot beside it is a second copy that goes stale the first time either
 * side writes.  _environ is what native getenv, setenv and execvp walk, and it
 * points at the host's environment, not at the one on the guest's initial
 * stack.  ___progname is what native getprogname, err and warn print, and ocerz
 * points it at the last component of the guest's argv[0] before the guest runs,
 * as it does NXArgv and NXArgc (bridge.c).  ___stdoutp and ___stderrp are the
 * streams native printf, puts
 * and perror write through; a program that assigns stdout expects printf to
 * follow it, and a slot holding anything but the native stream hands native
 * stdio a FILE it never opened.  __DefaultRuneLocale is a 3208-byte table that
 * carries pointers into host memory and has to agree with native __maskrune,
 * so a copy is wrong on both counts, and a zeroed one silently answers false to
 * every ctype question.  A database that does not export one of them leaves a
 * program importing it failing to bind and saying which name it wanted, and a
 * refusal that names itself is better than a value that is wrong without
 * saying so.
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
 * structure whose address nothing recognizes.  Some such names would survive a
 * copy - kCFBooleanTrue and kCFRunLoopDefaultMode are constant pointers to
 * objects that never move - but one rule for every native name is simpler than
 * deciding name by name, and costs nothing.
 *
 * So a data record's trie terminal is the host variable's own address with
 * EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE set.  The address is what the host lookup,
 * ocerz_bridge_host_symbol unless the caller supplies another, answers for the
 * record's host symbol in the library whose install name the file names.  The
 * trie walker returns an absolute terminal's value as it stands instead of
 * adding the load address, so every bind path that asks it - bind opcodes and
 * chained fixups alike - writes the native address into the guest's GOT entry
 * or isa word.  That address is usable from guest code only because the loader
 * asks for a virtual image in native mode alone, and native mode runs in the
 * identity map, where a host address is a guest address.  A native export
 * takes no stub, no slot, no export id and no byte of either segment: its trie
 * entry is all of it.
 *
 * A name the host lookup does not find is left out of the trie, and said once
 * per process through OCERZ_LOG however many times the image is built.  An
 * absolute export of zero would bind the guest's reference to a null pointer
 * that faults somewhere far from the import that caused it; an export that is
 * absent puts the name in the loader's unresolved-import report, and the run
 * stops with 71 before the guest has executed an instruction.
 *
 * ---- export ids ----
 * An export id is the library's ordinal in its top twelve bits and the entry's
 * index in its file in the low twenty.  The ordinal is the position of the
 * library's file among the .api files of the version directory in use, sorted
 * by name, read once per process.  That makes an id a function of the database
 * alone: numbering libraries as they were first loaded would number them by
 * whichever one a guest happened to name first - a program linking
 * CoreFoundation lists it ahead of libSystem - so the same libSystem would
 * carry different ids, and be different bytes, from one guest to the next.
 * Numbered by the directory, libSystem's stubs are the same in every process
 * that reads the same database, and an id means the same export in every run.
 * Indices of var and data entries are simply never minted, since neither has a
 * stub.
 *
 * ---- what happens after the trap ----
 * The trap lands in ocerz_vdylib_dispatch, which splits the export id into the
 * library and the entry it was minted from and asks the bridge whether it
 * knows how to perform that call for real.  If it does, the call is made there
 * and the guest goes on with the result in the registers x86 code expects.  If
 * it does not - a stub record, or a fn whose host symbol will not resolve - the
 * export names itself and the run stops with 72.
 *
 * The answer is cached in the library's own table, one atomic word per entry,
 * with a marker for an answer of no.  The bridge resolves by name, and this
 * dispatch is on the path of every call a guest makes into a virtual library,
 * so resolving per call would put a hash lookup in front of memcpy - a cost
 * that hides in a microbenchmark and does not hide in a program that calls
 * memcpy a million times.  Two threads that race to fill the same word store
 * the same pointer, because the bridge makes each descriptor once.
 *
 * Translated code reaches the same dispatch without the trap, through
 * ocerz_vdylib_fastcall.  What it has to decide on top is whether translated
 * code may carry on as though a function had returned.  It notes rsp and the
 * JIT's retirement count, performs the dispatch, and answers zero only if the
 * result is an ordinary step, rip is the word just below the new rsp and rsp
 * rose by eight or, for the r11-keeping stubs, sixteen, nothing asks the thread
 * to stop or to interpret its next instruction, and no translation was retired
 * in the meantime.  Everything else, a delivered signal among it, goes back
 * through the dispatcher exactly as the trap path would have.  The xmm contract
 * the translator asks for comes from the same database record: a fn record's
 * signature names the argument and result registers, a special reads the eight
 * argument registers and writes the two result ones, and __tlv_bootstrap and
 * ___chkstk_darwin, whose callers rely on every register surviving, have no
 * contract.
 *
 * ---- the stubs that keep r11 ----
 * An ordinary export's stub loads its id into r11, which is free to do: r11 is
 * a scratch register in the System V ABI and every linker stub on the platform
 * clobbers it.  __tlv_bootstrap is not an ordinary function.  It is the thunk a
 * thread-local variable's descriptor calls, and that calling convention keeps
 * every register but rax, so clang does hold live values in r11 across a
 * thread-local access; a program that loaded one thread-local into r11 and then
 * touched another read back garbage.  The stack probe ___chkstk_darwin has the
 * same contract for the same reason: clang calls it from a function prologue
 * and expects every register back.  The stub of a special record whose handler
 * is tlv_bootstrap or chkstk therefore pushes r11 before loading the id, which
 * still fits the sixteen-byte stride, and those handlers put r11 back from the
 * stack before they return.  Keying the push on the handler rather than on the
 * export's name keeps the two halves of that convention in one record.
 *
 * ---- dyld_stub_binder ----
 * A binary linked with classic lazy binding, which is every Intel binary built
 * for a macOS older than 12, imports dyld_stub_binder from libSystem whether or
 * not it ever reaches it: its __stub_helper entries jump there to bind a lazy
 * pointer on first call.  The loader applies lazy binds eagerly, so no helper
 * ever runs, but the import still has to bind or the whole program is refused
 * with 71 before its first instruction.  Its name is the one symbol with no
 * leading underscore, because dyld defines it in assembly rather than in C, and
 * libSystem's database exports it as a stub record; a guest that did reach it
 * would name it and stop, which is the honest answer to a lazy bind the loader
 * failed to make.
 */
#include "ocerz/vdylib.h"
#include "ocerz/abi.h"
#include "ocerz/apidb.h"
#include "ocerz/bridge.h"
#include "ocerz/dyldapi.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <mach-o/loader.h>

#define VD_PAGE 4096u
#define VD_STUB_STRIDE 16u
#define VD_STUB_BYTES 12u
#define VD_SLOT_BYTES 8u
#define VD_ULEB_WIDTH 5u
#define VD_ULEB_ABS_WIDTH 10u

#define VD_INDEX_BITS 20
#define VD_INDEX_MASK ((1u << VD_INDEX_BITS) - 1)
#define VD_LIBS_MAX (1u << (32 - VD_INDEX_BITS))

#define VD_NONE ((const struct OcerzBridgeFn *)(uintptr_t)1)

typedef struct VdFiller {
    const char *name;
    void (*fill)(uint8_t *slot, uint32_t size);
} VdFiller;

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

static const VdFiller g_vd_fillers[] = {
    { "stack_guard", vd_fill_stack_guard },
};

static const VdFiller *vd_filler(const char *name)
{
    for (size_t i = 0; i < sizeof g_vd_fillers / sizeof g_vd_fillers[0]; i++)
        if (strcmp(g_vd_fillers[i].name, name) == 0)
            return &g_vd_fillers[i];
    return NULL;
}

typedef struct VdLib {
    const OcerzApiLibrary *api;
    uint32_t ordinal;
    const struct OcerzBridgeFn *_Atomic *fn;
    _Atomic uint8_t *native_missed;
} VdLib;

static VdLib *_Atomic g_vd_libs[VD_LIBS_MAX];
static pthread_mutex_t g_vd_lock = PTHREAD_MUTEX_INITIALIZER;
static char **g_vd_files;
static int g_vd_nfiles;
static _Atomic int g_vd_listed;

static int vd_name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void vd_list_files_locked(void)
{
    if (g_vd_listed)
        return;
    const char *dir = ocerz_apidb_dir();
    DIR *d = dir ? opendir(dir) : NULL;
    int cap = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t n = strlen(de->d_name);
            if (de->d_name[0] == '.' || n <= 4 || strcmp(de->d_name + n - 4, ".api") != 0)
                continue;
            if (g_vd_nfiles == cap) {
                int nc = cap ? cap * 2 : 64;
                char **nf = realloc(g_vd_files, (size_t)nc * sizeof *nf);
                if (!nf)
                    break;
                g_vd_files = nf;
                cap = nc;
            }
            char *copy = strdup(de->d_name);
            if (!copy)
                break;
            g_vd_files[g_vd_nfiles++] = copy;
        }
        closedir(d);
    }
    if (g_vd_nfiles > 1)
        qsort(g_vd_files, (size_t)g_vd_nfiles, sizeof *g_vd_files, vd_name_cmp);
    g_vd_listed = 1;
}

static int vd_ordinal(const OcerzApiLibrary *api)
{
    const char *base = strrchr(api->path, '/');
    base = base ? base + 1 : api->path;
    if (!g_vd_listed) {
        pthread_mutex_lock(&g_vd_lock);
        vd_list_files_locked();
        pthread_mutex_unlock(&g_vd_lock);
    }
    char *const *hit = g_vd_nfiles ? bsearch(&base, g_vd_files, (size_t)g_vd_nfiles,
                                             sizeof *g_vd_files, vd_name_cmp) : NULL;
    return hit ? (int)(hit - g_vd_files) : -1;
}

static VdLib *vd_lib(const char *install_name)
{
    const OcerzApiLibrary *api = ocerz_apidb_library(install_name);
    if (!api)
        return NULL;
    int ord = vd_ordinal(api);
    if (ord < 0 || (uint32_t)ord >= VD_LIBS_MAX) {
        OCERZ_FATAL("virtual %s: %s is not one of the first %u files of %s\n", install_name,
                    api->path, (unsigned)VD_LIBS_MAX, ocerz_apidb_dir() ? ocerz_apidb_dir() : "(none)");
        return NULL;
    }
    if ((uint64_t)api->nentries > (uint64_t)VD_INDEX_MASK + 1) {
        OCERZ_FATAL("virtual %s declares %d exports, and an export id has room for %u\n",
                    install_name, api->nentries, (unsigned)(VD_INDEX_MASK + 1));
        return NULL;
    }
    VdLib *lib = g_vd_libs[ord];
    if (lib)
        return lib;

    pthread_mutex_lock(&g_vd_lock);
    lib = g_vd_libs[ord];
    if (!lib) {
        size_t n = api->nentries > 0 ? (size_t)api->nentries : 1;
        VdLib *nl = calloc(1, sizeof *nl);
        const struct OcerzBridgeFn *_Atomic *fn = calloc(n, sizeof *fn);
        _Atomic uint8_t *missed = calloc(n, sizeof *missed);
        if (nl && fn && missed) {
            nl->api = api;
            nl->ordinal = (uint32_t)ord;
            nl->fn = fn;
            nl->native_missed = missed;
            g_vd_libs[ord] = nl;
            lib = nl;
        } else {
            free(nl);
            free((void *)fn);
            free(missed);
            OCERZ_FATAL("out of memory registering virtual %s\n", install_name);
        }
    }
    pthread_mutex_unlock(&g_vd_lock);
    return lib;
}

int ocerz_vdylib_have(const char *install_name)
{
    return ocerz_apidb_library(install_name) != NULL;
}

static int vd_has_stub(OcerzApiKind kind)
{
    return kind == OCERZ_API_FN || kind == OCERZ_API_SPECIAL || kind == OCERZ_API_STUB;
}

static VdLib *vd_lib_of_id(uint64_t id, const OcerzApiEntry **entry_out)
{
    if (id > UINT32_MAX)
        return NULL;
    uint32_t ord = (uint32_t)id >> VD_INDEX_BITS;
    uint32_t idx = (uint32_t)id & VD_INDEX_MASK;
    VdLib *lib = g_vd_libs[ord];
    if (!lib || idx >= (uint32_t)lib->api->nentries || !vd_has_stub(lib->api->entries[idx].kind))
        return NULL;
    *entry_out = &lib->api->entries[idx];
    return lib;
}

int ocerz_vdylib_export_name(uint64_t id, const char **lib_out, const char **sym_out)
{
    const OcerzApiEntry *e = NULL;
    VdLib *lib = vd_lib_of_id(id, &e);
    if (!lib)
        return 0;
    if (lib_out)
        *lib_out = lib->api->install_name;
    if (sym_out)
        *sym_out = e->export_name;
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

static uint64_t vd_round_up(uint64_t v, uint64_t align)
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
    const char *edge;
    size_t elen;
    int first_child;
    int next_sibling;
    int terminal;
    int absolute;
    uint64_t addr;
    uint64_t off;
    uint64_t size;
} VdNode;

typedef struct VdTrie {
    VdNode *node;
    int n;
    int cap;
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
    if (t->n >= t->cap) {
        t->overflow = 1;
        return -1;
    }
    int i = t->n++;
    VdNode *nd = &t->node[i];
    memset(nd, 0, sizeof *nd);
    nd->first_child = -1;
    nd->next_sibling = -1;
    return i;
}

static int vd_trie_build(VdTrie *t, int lo, int hi, size_t depth)
{
    int me = vd_node_new(t);
    if (me < 0)
        return -1;

    int i = lo;
    if (i < hi && strlen(t->sym[lo].name) == depth) {
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
        if (elen == 0) {
            t->overflow = 1;
            return -1;
        }
        int kid = vd_trie_build(t, i, j, lcp);
        if (kid < 0)
            return -1;
        t->node[kid].edge = a + depth;
        t->node[kid].elen = elen;
        if (last < 0)
            t->node[me].first_child = kid;
        else
            t->node[last].next_sibling = kid;
        last = kid;
        i = j;
    }
    return me;
}

static uint64_t vd_trie_layout(VdTrie *t)
{
    for (int i = 0; i < t->n; i++) {
        uint64_t sz = 1;
        int kids = 0;
        if (t->node[i].terminal)
            sz += 1u + vd_term_width(t->node[i].absolute);
        sz += 1;
        for (int c = t->node[i].first_child; c >= 0; c = t->node[c].next_sibling) {
            sz += t->node[c].elen + 1 + VD_ULEB_WIDTH;
            kids++;
        }
        if (kids > 255) {
            t->overflow = 1;
            return 0;
        }
        t->node[i].size = sz;
    }
    uint64_t off = 0;
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
            memcpy(p, t->node[c].edge, t->node[c].elen);
            p += t->node[c].elen;
            *p++ = '\0';
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

static int vd_keeps_r11(const OcerzApiEntry *e)
{
    return e->kind == OCERZ_API_SPECIAL && e->handler &&
           (strcmp(e->handler, "tlv_bootstrap") == 0 || strcmp(e->handler, "chkstk") == 0);
}

uint8_t *ocerz_vdylib_image_with(const char *install_name, OcerzVdylibHostSym host_sym,
                                 size_t *len_out)
{
    VdLib *lib = vd_lib(install_name);
    if (!lib)
        return NULL;
    if (!host_sym)
        host_sym = ocerz_bridge_host_symbol;

    const OcerzApiLibrary *api = lib->api;
    const char *name = api->install_name;
    int ne = api->nentries;
    int n = 0, nv = 0;
    for (int k = 0; k < ne; k++) {
        OcerzApiKind kind = api->entries[k].kind;
        n += vd_has_stub(kind);
        nv += kind == OCERZ_API_VAR;
    }

    uint32_t name_len = (uint32_t)strlen(name) + 1;
    uint32_t id_cmdsize = (uint32_t)vd_round_up(sizeof(struct dylib_command) + name_len, 8);
    uint32_t seg_cmdsize = (uint32_t)(sizeof(struct segment_command_64) + sizeof(struct section_64));
    uint32_t trie_cmdsize = (uint32_t)sizeof(struct linkedit_data_command);
    uint32_t sizeofcmds = 2 * seg_cmdsize + id_cmdsize + trie_cmdsize;
    uint32_t hdr_size = (uint32_t)sizeof(struct mach_header_64);

    uint64_t stubs_off = vd_round_up(hdr_size + sizeofcmds, VD_STUB_STRIDE);
    uint64_t stubs_size = (uint64_t)n * VD_STUB_STRIDE;
    uint64_t text_size = vd_round_up(stubs_off + stubs_size, VD_PAGE);
    uint64_t slots_off = text_size;
    uint64_t slots_size = (uint64_t)n * VD_SLOT_BYTES;
    uint64_t vars_off = vd_round_up(slots_off + slots_size, VD_SLOT_BYTES);

    uint64_t *var_addr = calloc((size_t)ne + 1, sizeof *var_addr);
    VdSym *syms = calloc((size_t)ne + 1, sizeof *syms);
    VdNode *nodes = calloc(2 * (size_t)ne + 2, sizeof *nodes);
    uint8_t *buf = NULL;
    if (!var_addr || !syms || !nodes) {
        OCERZ_FATAL("out of memory building virtual %s\n", name);
        goto fail;
    }

    int m = 0;
    int missed = 0;
    uint64_t vars_end = vars_off;
    uint32_t stub_index = 0;
    for (int k = 0; k < ne; k++) {
        const OcerzApiEntry *e = &api->entries[k];
        if (vd_has_stub(e->kind)) {
            syms[m].name = e->export_name;
            syms[m].addr = stubs_off + (uint64_t)stub_index++ * VD_STUB_STRIDE;
            syms[m].absolute = 0;
            m++;
        } else if (e->kind == OCERZ_API_VAR) {
            if (!vd_filler(e->filler)) {
                OCERZ_FATAL("virtual %s: %s asks for the filler %s, which ocerz does not have\n",
                            name, e->export_name, e->filler);
                goto fail;
            }
            if (e->bytes == 0) {
                OCERZ_FATAL("virtual %s: %s is a data slot of no bytes\n", name, e->export_name);
                goto fail;
            }
            var_addr[k] = vars_end;
            vars_end += vd_round_up(e->bytes, VD_SLOT_BYTES);
            syms[m].name = e->export_name;
            syms[m].addr = var_addr[k];
            syms[m].absolute = 0;
            m++;
        } else if (e->kind == OCERZ_API_DATA) {
            void *host = host_sym(name, e->host);
            if (!host) {
                if (!atomic_exchange(&lib->native_missed[k], 1))
                    OCERZ_LOG("vdylib: host %s has no %s, so virtual %s does not export %s\n",
                              name, e->host, name, e->export_name);
                missed++;
                continue;
            }
            syms[m].name = e->export_name;
            syms[m].addr = (uint64_t)(uintptr_t)host;
            syms[m].absolute = 1;
            m++;
        }
    }

    uint64_t data_used = vars_end - slots_off;
    uint64_t data_size = vd_round_up(data_used ? data_used : 1, VD_PAGE);
    uint64_t trie_off = slots_off + data_size;

    qsort(syms, (size_t)m, sizeof *syms, vd_sym_cmp);
    for (int i = 1; i < m; i++) {
        if (strcmp(syms[i - 1].name, syms[i].name) == 0) {
            OCERZ_FATAL("virtual %s exports %s twice\n", name, syms[i].name);
            goto fail;
        }
    }

    VdTrie trie;
    trie.node = nodes;
    trie.n = 0;
    trie.cap = 2 * ne + 2;
    trie.overflow = 0;
    trie.sym = syms;
    if (vd_trie_build(&trie, 0, m, 0) != 0 || trie.overflow) {
        OCERZ_FATAL("virtual %s cannot be laid out as an export trie\n", name);
        goto fail;
    }
    uint64_t trie_size = vd_trie_layout(&trie);
    if (trie.overflow || trie_size == 0) {
        OCERZ_FATAL("virtual %s has a trie node with more than 255 children\n", name);
        goto fail;
    }

    uint64_t total = trie_off + trie_size;
    if (total > UINT32_MAX) {
        OCERZ_FATAL("virtual %s would be %llu bytes, past what 32-bit file offsets reach\n", name,
                    (unsigned long long)total);
        goto fail;
    }
    buf = calloc(1, (size_t)total);
    if (!buf) {
        OCERZ_FATAL("out of memory building virtual %s\n", name);
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
                     stubs_off, stubs_size, (uint32_t)stubs_off, 4,
                     S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    lc += seg_cmdsize;

    vd_write_segment(lc, seg_cmdsize, "__DATA", slots_off, data_size, slots_off, data_size,
                     VM_PROT_READ | VM_PROT_WRITE, 1);
    vd_write_section(lc + sizeof(struct segment_command_64), "__data", "__DATA",
                     slots_off, data_used, (uint32_t)slots_off, 3, S_REGULAR);
    lc += seg_cmdsize;

    wr32(lc + 0, LC_ID_DYLIB);
    wr32(lc + 4, id_cmdsize);
    wr32(lc + 8, (uint32_t)sizeof(struct dylib_command));
    wr32(lc + 12, 1);
    wr32(lc + 16, 0x10000);
    wr32(lc + 20, 0x10000);
    memcpy(lc + sizeof(struct dylib_command), name, name_len);
    lc += id_cmdsize;

    wr32(lc + 0, LC_DYLD_EXPORTS_TRIE);
    wr32(lc + 4, trie_cmdsize);
    wr32(lc + 8, (uint32_t)trie_off);
    wr32(lc + 12, (uint32_t)trie_size);

    vd_trie_emit(&trie, buf + trie_off);

    uint32_t base = lib->ordinal << VD_INDEX_BITS;
    stub_index = 0;
    for (int k = 0; k < ne; k++) {
        const OcerzApiEntry *e = &api->entries[k];
        if (e->kind == OCERZ_API_VAR) {
            vd_filler(e->filler)->fill(buf + var_addr[k], e->bytes);
            continue;
        }
        if (!vd_has_stub(e->kind))
            continue;
        uint64_t stub_addr = stubs_off + (uint64_t)stub_index * VD_STUB_STRIDE;
        uint64_t slot_addr = slots_off + (uint64_t)stub_index * VD_SLOT_BYTES;
        stub_index++;
        uint8_t *s = buf + stub_addr;
        uint32_t at = 0;
        if (vd_keeps_r11(e)) {
            s[at++] = 0x41;
            s[at++] = 0x53;
        }
        s[at++] = 0x41;
        s[at++] = 0xbb;
        wr32(s + at, base | (uint32_t)k);
        at += 4;
        s[at++] = 0xff;
        s[at++] = 0x25;
        int64_t rel = (int64_t)slot_addr - (int64_t)(stub_addr + at + 4);
        wr32(s + at, (uint32_t)(int32_t)rel);
        at += 4;
        for (uint32_t pad = at; pad < VD_STUB_STRIDE; pad++)
            s[pad] = 0xcc;
        wr64(buf + slot_addr, OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF);
    }

    OCERZ_LOG("vdylib: built %s from %s with %d exports (%d functions, %d data, %d native, "
              "%d native missing), %llu bytes (text %llu data %llu trie %llu at %llu)\n",
              name, api->path, m, n, nv, m - n - nv, missed, (unsigned long long)total,
              (unsigned long long)text_size, (unsigned long long)data_size,
              (unsigned long long)trie_size, (unsigned long long)trie_off);

    free(var_addr);
    free(syms);
    free(nodes);
    if (len_out)
        *len_out = (size_t)total;
    return buf;

fail:
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

static inline __attribute__((always_inline)) int vd_dispatch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int hooked = -1;
    if (hooked < 0) {
        hooked = 0;
        if (getenv("OCERZ_BRIDGESTAT"))
            atexit(ocerz_bridge_report);
    }

    uint64_t id = cpu->gpr[OCERZ_R11] & 0xffffffffull;
    const OcerzApiEntry *e = NULL;
    VdLib *lib = vd_lib_of_id(id, &e);
    if (!lib) {
        fprintf(stderr, "ocerz: bridge: export id %#llx is not one a synthesized library minted\n",
                (unsigned long long)id);
        exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    }

    const struct OcerzBridgeFn *_Atomic *slot = &lib->fn[e - lib->api->entries];
    const struct OcerzBridgeFn *fn = *slot;
    if (!fn) {
        fn = ocerz_bridge_lookup(lib->api->install_name, e->export_name);
        if (!fn)
            fn = VD_NONE;
        *slot = fn;
    }
    if (fn != VD_NONE)
        return ocerz_bridge_invoke(vm, cpu, fn);

    fprintf(stderr, "ocerz: bridge: %s %s not implemented\n", lib->api->install_name,
            e->export_name);

    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    return OCERZ_STEP_OK;
}

int ocerz_vdylib_dispatch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return vd_dispatch(vm, cpu);
}

int ocerz_vdylib_xmm_contract(uint64_t id, uint16_t *in, uint16_t *out)
{
    const OcerzApiEntry *e = NULL;
    if (!in || !out || !vd_lib_of_id(id, &e))
        return 0;
    if (e->kind == OCERZ_API_FN) {
        OcerzAbiSig sig;
        if (!e->sig || ocerz_abi_parse(e->sig, &sig) != OCERZ_OK)
            return 0;
        ocerz_abi_xmm_contract(&sig, in, out);
        return 1;
    }
    if (e->kind == OCERZ_API_SPECIAL && e->handler && strcmp(e->handler, "tlv_bootstrap") != 0 &&
        strcmp(e->handler, "chkstk") != 0) {
        *in = 0xff;
        *out = 0x3;
        return 1;
    }
    return 0;
}

int ocerz_vdylib_fastcall(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t epoch = ocerz_jit_retire_epoch();
    uint64_t rsp0 = cpu->gpr[OCERZ_RSP];
    cpu->rip = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;
    if (cpu->cc_op != OCERZ_CC_NONE)
        ocerz_flags_materialize(cpu);
    int rc = vd_dispatch(vm, cpu);
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    if (rc == OCERZ_STEP_OK && rsp - rsp0 - 8 <= 8 && cpu->rip == ocerz_ld(rsp - 8, 8) &&
        !cpu->interrupt && !cpu->terminated && !vm->exited && !cpu->suspend_count &&
        !cpu->interp_once && ocerz_jit_retire_epoch() == epoch)
        return 0;
    if (rc == OCERZ_STEP_EXIT || rc == OCERZ_STEP_FATAL)
        return rc + 1;
    return OCERZ_STEP_OK + 1;
}
