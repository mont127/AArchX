/*
 * The Objective-C bridge, checked against the host's own runtime.
 *
 * The encoding converter is run over every method of every class Foundation and
 * CoreFoundation define, class methods included, as the running arm64 runtime
 * reports them, so the table is whatever the installed frameworks are rather than
 * a list written for the test.  Each encoding either converts or is refused, and
 * the test decides which from a reading of its own: the return type and argument
 * types the runtime itself splits out with method_copyReturnType and
 * method_copyArgumentType, scanned for what the bridge refuses, in the order the
 * bridge meets them.  A converted notation has to parse, has to have the
 * runtime's argument count, and each argument and the result has to have the
 * size NSGetSizeAndAlignment gives its type, a structure measured by the ABI
 * engine's own layout, so a member lost, doubled or misaligned in conversion
 * shows up as a size.  A hand table beside it pins the classes one by one, and
 * the shapes the frameworks do not happen to contain: a union, a bitfield, a long
 * double, a complex, a 128-bit integer, an unknown type, an opaque structure, void
 * as an argument, seventeen arguments, a flattened array, a structure of
 * seventeen members, qualifiers, field names and block signatures.
 *
 * The format classifier is a table per dialect.  The variadic cursor is driven
 * against hand-built guest register and stack states whose unused slots, and the
 * upper halves of int-sized registers, hold garbage, so an argument read from the
 * wrong place, or extended from the wrong width, reads garbage back.  The selector
 * pass runs over a synthetic image laid out in guest memory, with selector
 * references in a __DATA_CONST section that must change, a null entry, a message
 * reference whose implementation word must not change, and a __TEXT section and a
 * __DATA section of other names that must not change either.
 *
 * The send handlers run in this process against real Foundation objects, as they
 * do under a guest: a hand-built cpu in the identity map, a return address on a
 * guest stack, arguments where System V puts them.  Nil receivers must zero every
 * result register, a nil _stret send must leave its memory alone and a nil super
 * _stret send must zero exactly the structure.  Every refusal runs in a forked
 * child, whose status must be 72 and whose stderr must name the reason.  A
 * native class whose forwardingTargetForSelector: answers a string, and a
 * second whose target forwards again, must have their sends crossed under the
 * signature of the object that finally has the method.
 *
 * Guest definitions start with the layout readers, run over structures written
 * into guest memory: a class_ro_t whose every byte differs, so a field read at
 * the wrong offset reads the wrong bytes; a class_t with each Swift bit; an
 * absolute method list with its fixed-up bits set; a relative one whose
 * implementation offset is negative, whose last implementation offset is zero,
 * and whose selectors are then made direct; lists of entries too small for
 * their kind; ivar, property and protocol lists; category_t with and without
 * class properties; protocol_t of 96 bytes and of 72.  The x86 type converter
 * is a table of what an x86 compiler writes, BOOL as c and long as q among it,
 * and of what it refuses, types without self and _cmd included.  Property
 * attributes are split, and too many or too long refused.  Superclass ordering
 * runs over two chains listed subclass first, a loop, and a class that is its
 * own superclass.
 *
 * A synthetic image then goes through ocerz_objcbridge_define_image against the
 * real runtime.  It has guest copies of NSObject and NSCopying and a protocol of
 * its own with a method and a property; a class whose instance start is 4, so
 * its two ivar offset variables must be slid past NSObject's 8 bytes by an
 * 8-aligned 8, and written in their low halves only, since the upper half of
 * one is poisoned; its subclass, listed first; a method whose long double
 * keeps its class but is bound to the named refusal; a category on NSString
 * with an instance method, a class method, a property and the protocol; a
 * category on the guest class replacing one of its methods; protocol
 * references; and a +load in both classes and in the NSString category, each a
 * few bytes of x86 that count and record the class they were called with.  The
 * runtime must find both classes by name at the guest's own addresses with the
 * guest's metaclasses, every implementation must be a slot bound to the guest's
 * function under the converted notation, two slots of one notation must share
 * one parsed signature, the protocols must be native and the references
 * rewritten to them, a second definition of the image must define nothing, and
 * the +load methods must run superclass first, then the class the non-lazy list
 * names, then the category, each with its class in rdi.  Each class refusal
 * runs in a forked child that must exit 72 naming it: a root class, a class
 * with a null superclass, a superclass that is a guest class never defined, a Swift class, a loop, a
 * category on an undefined guest class, a long double method called natively,
 * and a method defined after the bank was filled, called natively.
 *
 * The database is a directory the test writes for itself, one file per library
 * naming nothing but the library, because the bridge opens a host library only
 * for an install name the database has a file for, and this test must not depend
 * on which generated files happen to be installed.
 */
#include "ocerz/objcbridge.h"
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach-o/loader.h>

#define ARENA      (4ull << 30)
#define SCRATCH    0x40000ull
#define RET_ADDR   0x0000000044332200ull
#define POISON     0xfeedfacecafebeefull
#define GARBAGE    0xa5a5a5a5deadbeefull

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

static OcerzVM vm;
static uint64_t scratch;
static uint64_t stack_top;

static void *(*objc_getClass_)(const char *);
static void *(*object_getClass_)(void *);
static void *(*sel_registerName_)(const char *);
static void **(*class_copyMethodList_)(void *, unsigned *);
static const char *(*method_getTypeEncoding_)(void *);
static void *(*method_getName_)(void *);
static const char *(*sel_getName_)(void *);
static unsigned (*method_getNumberOfArguments_)(void *);
static char *(*method_copyReturnType_)(void *);
static char *(*method_copyArgumentType_)(void *, unsigned);
static const char *(*class_getName_)(void *);
static const char **(*objc_copyClassNamesForImage_)(const char *, unsigned *);
static const char *(*NSGetSizeAndAlignment_)(const char *, unsigned long *, unsigned long *);
static void *objc_msgSend_;
static long (*CFStringGetLength_)(void *);
static bool (*CFStringGetCString_)(void *, char *, long, uint32_t);

static void *need(void *handle, const char *name)
{
    void *p = dlsym(handle, name);
    if (!p) {
        fprintf(stderr, "cannot find %s\n", name);
        exit(2);
    }
    return p;
}

static char g_db_root[512];

static const char *const kDbFiles[][2] = {
    { "libobjc.A.dylib.api", OCERZ_OBJC_LIBOBJC },
    { "Foundation.api", OCERZ_OBJC_FOUNDATION },
    { "CoreFoundation.api", OCERZ_BRIDGE_COREFOUNDATION },
    { "libSystem.B.dylib.api", OCERZ_BRIDGE_LIBSYSTEM },
};
#define NDBFILES (sizeof kDbFiles / sizeof kDbFiles[0])

static void write_db(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_db_root, sizeof g_db_root, "%s/ocerz-objcbridge-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(g_db_root)) {
        perror("mkdtemp");
        exit(2);
    }
    const char *root = g_db_root;
    char dir[600];
    snprintf(dir, sizeof dir, "%s/macos", root);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/macos/27.0", root);
    mkdir(dir, 0755);
    for (size_t i = 0; i < NDBFILES; i++) {
        char path[700];
        snprintf(path, sizeof path, "%s/%s", dir, kDbFiles[i][0]);
        FILE *f = fopen(path, "w");
        if (!f) {
            perror(path);
            exit(2);
        }
        fprintf(f, "ocerz-apidb 1\nlibrary %s\nsdk macos 27.0\n", kDbFiles[i][1]);
        fclose(f);
    }
    setenv("OCERZ_APIDB", root, 1);
}

static void remove_db(void)
{
    char path[700];
    for (size_t i = 0; i < NDBFILES; i++) {
        snprintf(path, sizeof path, "%s/macos/27.0/%s", g_db_root, kDbFiles[i][0]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/macos/27.0", g_db_root);
    rmdir(path);
    snprintf(path, sizeof path, "%s/macos", g_db_root);
    rmdir(path);
    rmdir(g_db_root);
}

static void *sel(const char *name)
{
    return sel_registerName_(name);
}

static void *cls(const char *name)
{
    return objc_getClass_(name);
}

static void *nsstr(const char *s)
{
    return ((void *(*)(void *, void *, const char *))objc_msgSend_)(cls("NSString"), sel("stringWithUTF8String:"), s);
}

static void cfstr_text(void *s, char *buf, size_t len)
{
    buf[0] = '\0';
    if (s)
        CFStringGetCString_(s, buf, (long)len, 0x08000100u);
}

static OcerzCPU *fresh_cpu(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t rsp = (stack_top & ~0xfull) - 8 * 64;
    memset(cpu->gpr, 0, sizeof cpu->gpr);
    for (int i = 0; i < 16; i++) {
        cpu->xmm[i].lo = GARBAGE ^ (uint64_t)i;
        cpu->xmm[i].hi = GARBAGE;
    }
    for (int i = 0; i < 64; i++)
        ocerz_st(rsp + 8 * (uint64_t)i, 8, GARBAGE + (uint64_t)i);
    ocerz_st(rsp, 8, RET_ADDR);
    cpu->gpr[OCERZ_RSP] = rsp;
    cpu->gpr[OCERZ_RAX] = POISON;
    cpu->gpr[OCERZ_RDX] = POISON;
    cpu->sig_pending = 0;
    cpu->sig_mask = 0;
    return cpu;
}

static void stack_arg(OcerzCPU *cpu, int slot, uint64_t v)
{
    ocerz_st(cpu->gpr[OCERZ_RSP] + 8 + 8 * (uint64_t)slot, 8, v);
}

static int returned(const OcerzCPU *cpu, uint64_t rsp_before)
{
    return cpu->rip == RET_ADDR && cpu->gpr[OCERZ_RSP] == rsp_before + 8;
}

typedef struct Expect {
    const char *enc;
    int rc;
    const char *notation;
    uint32_t blocks;
    uint32_t fnptrs;
} Expect;

static const Expect kEncodings[] = {
    { "B24@0:8@16", OCERZ_OBJC_OK, "b(ppp)", 0, 0 },
    { "c16@0:8", OCERZ_OBJC_OK, "b(pp)", 0, 0 },
    { "C16@0:8", OCERZ_OBJC_OK, "B(pp)", 0, 0 },
    { "s16@0:8", OCERZ_OBJC_OK, "h(pp)", 0, 0 },
    { "S24@0:8Q16", OCERZ_OBJC_OK, "H(ppL)", 0, 0 },
    { "i20@0:8i16", OCERZ_OBJC_OK, "i(ppi)", 0, 0 },
    { "I20@0:8I16", OCERZ_OBJC_OK, "u(ppu)", 0, 0 },
    { "l20@0:8L16", OCERZ_OBJC_OK, "i(ppu)", 0, 0 },
    { "q24@0:8q16", OCERZ_OBJC_OK, "l(ppl)", 0, 0 },
    { "f20@0:8f16", OCERZ_OBJC_OK, "f(ppf)", 0, 0 },
    { "d24@0:8d16", OCERZ_OBJC_OK, "d(ppd)", 0, 0 },
    { "v40@0:8*16#24:32", OCERZ_OBJC_OK, "v(ppppp)", 0, 0 },
    { "r*16@0:8", OCERZ_OBJC_OK, "p(pp)", 0, 0 },
    { "Vv16@0:8", OCERZ_OBJC_OK, "v(pp)", 0, 0 },
    { "v32@0:8o^@16n^{__CFString=}24", OCERZ_OBJC_OK, "v(pppp)", 0, 0 },
    { "@\"NSString\"16@0:8", OCERZ_OBJC_OK, "p(pp)", 0, 0 },
    { "{_NSRange=QQ}32@0:8@16{_NSRange=QQ}24", OCERZ_OBJC_OK, "{LL}(ppp{LL})", 0, 0 },
    { "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", OCERZ_OBJC_OK, "{{dd}{dd}}(pp)", 0, 0 },
    { "{CGAffineTransform=dddddd}16@0:8", OCERZ_OBJC_OK, "{dddddd}(pp)", 0, 0 },
    { "{S=\"a\"i\"b\"d}16@0:8", OCERZ_OBJC_OK, "{id}(pp)", 0, 0 },
    { "{?=[4f]}16@0:8", OCERZ_OBJC_OK, "{ffff}(pp)", 0, 0 },
    { "{s=[2{p=dd}]c}16@0:8", OCERZ_OBJC_OK, "{{dd}{dd}b}(pp)", 0, 0 },
    { "{u=^{x=(y=i)}@?#}16@0:8", OCERZ_OBJC_OK, "{ppp}(pp)", 0, 0 },
    { "v32@0:8[4i]16", OCERZ_OBJC_OK, "v(ppp)", 0, 0 },
    { "v24@0:8@?16", OCERZ_OBJC_OK, "v(ppp)", 1u << 2, 0 },
    { "v24@0:8@?<v@?@\"NSError\">16", OCERZ_OBJC_OK, "v(ppp)", 1u << 2, 0 },
    { "q32@0:8^?16^v24", OCERZ_OBJC_OK, "l(pppp)", 0, 1u << 2 },
    { "v40@0:8@16@?24^?32", OCERZ_OBJC_OK, "v(ppppp)", 1u << 3, 1u << 4 },
    { "@?16@0:8", OCERZ_OBJC_OK, "p(pp)", 0, 0 },
    { "v24@0:8B16", OCERZ_OBJC_OK, "v(ppb)", 0, 0 },
    { "v@:", OCERZ_OBJC_OK, "v(pp)", 0, 0 },
    { "{?=b8b4b1b1b18[8S]}16@0:8", OCERZ_OBJC_BITFIELD, NULL, 0, 0 },
    { "(?=iq)16@0:8", OCERZ_OBJC_UNION, NULL, 0, 0 },
    { "v24@0:8{S=(u=id)}16", OCERZ_OBJC_UNION, NULL, 0, 0 },
    { "D16@0:8", OCERZ_OBJC_LONG_DOUBLE, NULL, 0, 0 },
    { "v32@0:8jd16", OCERZ_OBJC_COMPLEX, NULL, 0, 0 },
    { "t16@0:8", OCERZ_OBJC_INT128, NULL, 0, 0 },
    { "v24@0:8T16", OCERZ_OBJC_INT128, NULL, 0, 0 },
    { "v24@0:8?16", OCERZ_OBJC_UNKNOWN, NULL, 0, 0 },
    { "v24@0:8{Opaque}16", OCERZ_OBJC_OPAQUE, NULL, 0, 0 },
    { "v24@0:8{Empty=}16", OCERZ_OBJC_OPAQUE, NULL, 0, 0 },
    { "v24@0:8v16", OCERZ_OBJC_VOID_VALUE, NULL, 0, 0 },
    { "{S=iv}16@0:8", OCERZ_OBJC_VOID_VALUE, NULL, 0, 0 },
    { "v@:iiiiiiiiiiiiiiii", OCERZ_OBJC_TOO_MANY_ARGS, NULL, 0, 0 },
    { "v@:iiiiiiiiiiiiii", OCERZ_OBJC_OK, "v(ppiiiiiiiiiiiiii)", 0, 0 },
    { "{S=[17c]}16@0:8", OCERZ_OBJC_ENGINE, NULL, 0, 0 },
    { "{S=[16c]c}16@0:8", OCERZ_OBJC_ENGINE, NULL, 0, 0 },
    { "{S=[0i]}16@0:8", OCERZ_OBJC_ENGINE, NULL, 0, 0 },
    { "{S=[16c]}16@0:8", OCERZ_OBJC_OK, "{bbbbbbbbbbbbbbbb}(pp)", 0, 0 },
    { "{S=dd", OCERZ_OBJC_MALFORMED, NULL, 0, 0 },
    { "", OCERZ_OBJC_MALFORMED, NULL, 0, 0 },
    { "v16@0:8x", OCERZ_OBJC_MALFORMED, NULL, 0, 0 },
};

static void test_encoding_table(void)
{
    for (size_t i = 0; i < sizeof kEncodings / sizeof kEncodings[0]; i++) {
        const Expect *e = &kEncodings[i];
        char out[OCERZ_OBJC_NOTATION_MAX];
        int nargs = -1;
        uint32_t blocks = 99, fnptrs = 99;
        int rc = ocerz_objc_notation(e->enc, out, sizeof out, &nargs, &blocks, &fnptrs);
        CHECK(rc == e->rc, "%s: rc %d (%s), want %d (%s)", e->enc, rc, ocerz_objc_refusal(rc), e->rc,
              ocerz_objc_refusal(e->rc));
        if (rc == OCERZ_OBJC_OK && e->rc == OCERZ_OBJC_OK) {
            CHECK(strcmp(out, e->notation) == 0, "%s: notation %s, want %s", e->enc, out, e->notation);
            CHECK(blocks == e->blocks && fnptrs == e->fnptrs, "%s: blocks %#x fnptrs %#x, want %#x %#x",
                  e->enc, blocks, fnptrs, e->blocks, e->fnptrs);
        }
    }

    char tiny[6];
    CHECK(ocerz_objc_notation("{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", tiny, sizeof tiny, NULL, NULL,
                              NULL) == OCERZ_OBJC_TOO_LONG, "a notation longer than its buffer is refused");
    char longenc[512] = "v@:";
    for (int i = 0; i < 14; i++)
        strcat(longenc, "{S={T=dd}{U=ii}}");
    char out[OCERZ_OBJC_NOTATION_MAX];
    CHECK(ocerz_objc_notation(longenc, out, sizeof out, NULL, NULL, NULL) == OCERZ_OBJC_OK,
          "fourteen structure arguments fit a notation");
    for (int i = OCERZ_OBJC_OK; i <= OCERZ_OBJC_MALFORMED; i++)
        CHECK(ocerz_objc_refusal(i) && *ocerz_objc_refusal(i), "refusal %d has words", i);
}

static const char *oracle_quals(const char *p)
{
    while (*p && strchr("rnNoORVA", *p))
        p++;
    return p;
}

static const char *oracle_close(const char *p, char open, char close)
{
    int depth = 0;
    do {
        if (*p == '"') {
            p = strchr(p + 1, '"');
            if (!p)
                return NULL;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
        }
        p++;
    } while (*p && depth > 0);
    return depth == 0 ? p : NULL;
}

typedef struct Oracle {
    int rc;
    int leaves;
    int deferred;
} Oracle;

static const char *oracle_type(const char *p, int member, int result, Oracle *o)
{
    p = oracle_quals(p);
    char c = *p;
    if (strchr("cCsSiIlLqQfdB*#:%", c) && c)
        return o->leaves++, p + 1;
    if (c == 'v') {
        if (!result && o->rc == OCERZ_OBJC_OK)
            o->rc = OCERZ_OBJC_VOID_VALUE;
        return p + 1;
    }
    if (c == '^' || c == 'j') {
        if (c == 'j' && o->rc == OCERZ_OBJC_OK)
            o->rc = OCERZ_OBJC_COMPLEX;
        Oracle inner = { OCERZ_OBJC_OK, 0, 0 };
        const char *q = oracle_type(p + 1, 0, 1, &inner);
        o->leaves++;
        return q;
    }
    if (c == '@') {
        o->leaves++;
        p++;
        if (*p == '?')
            return p[1] == '<' ? oracle_close(p + 1, '<', '>') : p + 1;
        if (*p == '"')
            return strchr(p + 1, '"') + 1;
        return p;
    }
    int code = c == '(' ? OCERZ_OBJC_UNION : c == 'b' ? OCERZ_OBJC_BITFIELD : c == 'D' ? OCERZ_OBJC_LONG_DOUBLE
             : c == 't' || c == 'T' ? OCERZ_OBJC_INT128 : c == '?' ? OCERZ_OBJC_UNKNOWN : 0;
    if (code) {
        if (o->rc == OCERZ_OBJC_OK)
            o->rc = code;
        if (c == '(')
            return oracle_close(p, '(', ')');
        p++;
        while (*p >= '0' && *p <= '9')
            p++;
        return p;
    }
    if (c == '[') {
        const char *end = oracle_close(p, '[', ']');
        if (!member) {
            o->leaves++;
            return end;
        }
        int n = atoi(p + 1);
        const char *q = p + 1;
        while (*q >= '0' && *q <= '9')
            q++;
        if ((n == 0 || n > 16) && o->rc == OCERZ_OBJC_OK)
            o->rc = OCERZ_OBJC_ENGINE;
        Oracle inner = { OCERZ_OBJC_OK, 0, 0 };
        oracle_type(q, 1, 0, &inner);
        if (inner.rc != OCERZ_OBJC_OK && o->rc == OCERZ_OBJC_OK)
            o->rc = inner.rc;
        o->leaves += n * inner.leaves;
        return end;
    }
    if (c == '{') {
        const char *end = oracle_close(p, '{', '}');
        const char *eq = strchr(p, '=');
        if (!eq || eq > end) {
            if (o->rc == OCERZ_OBJC_OK)
                o->rc = OCERZ_OBJC_OPAQUE;
            return end;
        }
        const char *q = eq + 1;
        int members = 0;
        Oracle inner = { OCERZ_OBJC_OK, 0, 0 };
        while (q && q < end - 1) {
            if (*q == '"')
                q = strchr(q + 1, '"') + 1;
            if (q >= end - 1)
                break;
            q = oracle_type(q, 1, 0, &inner);
            members++;
        }
        if (inner.rc != OCERZ_OBJC_OK && o->rc == OCERZ_OBJC_OK)
            o->rc = inner.rc;
        if (members == 0 && o->rc == OCERZ_OBJC_OK)
            o->rc = OCERZ_OBJC_OPAQUE;
        if (inner.leaves > 16)
            o->deferred = 1;
        o->leaves += inner.leaves;
        return end;
    }
    if (o->rc == OCERZ_OBJC_OK)
        o->rc = OCERZ_OBJC_MALFORMED;
    return p + 1;
}

static int oracle(void *m)
{
    Oracle o = { OCERZ_OBJC_OK, 0, 0 };
    char *rt = method_copyReturnType_(m);
    oracle_type(rt, 0, 1, &o);
    free(rt);
    unsigned n = method_getNumberOfArguments_(m);
    for (unsigned i = 0; i < n && o.rc == OCERZ_OBJC_OK; i++) {
        if (i == 16)
            return OCERZ_OBJC_TOO_MANY_ARGS;
        char *at = method_copyArgumentType_(m, i);
        oracle_type(at, 0, 0, &o);
        free(at);
    }
    if (o.rc == OCERZ_OBJC_OK && o.deferred)
        return OCERZ_OBJC_ENGINE;
    return o.rc;
}

static unsigned long class_size(char c, const OcerzAbiStruct *st)
{
    switch (c) {
    case 'v': return 0;
    case 'b': case 'B': return 1;
    case 'h': case 'H': return 2;
    case 'i': case 'u': case 'f': return 4;
    case '{': return st->size;
    default: return 8;
    }
}

static unsigned long runtime_size(const char *type, int toplevel)
{
    const char *t = oracle_quals(type);
    if (*t == 'v')
        return 0;
    if (toplevel && *t == '[')
        return 8;
    if (*t == 'l' || *t == 'L')
        return 4;
    unsigned long size = 0, align = 0;
    NSGetSizeAndAlignment_(type, &size, &align);
    return size;
}

typedef struct Tally {
    int methods;
    int converted;
    int refused[OCERZ_OBJC_MALFORMED + 1];
    char example[OCERZ_OBJC_MALFORMED + 1][160];
} Tally;

static void sweep_class(void *c, Tally *t)
{
    unsigned count = 0;
    void **list = class_copyMethodList_(c, &count);
    for (unsigned i = 0; i < count; i++) {
        void *m = list[i];
        const char *enc = method_getTypeEncoding_(m);
        if (!enc)
            continue;
        char out[OCERZ_OBJC_NOTATION_MAX];
        int nargs = -1;
        int rc = ocerz_objc_notation(enc, out, sizeof out, &nargs, NULL, NULL);
        int want = oracle(m);
        t->methods++;
        CHECK(rc == want, "%s %s %s: converter says %s, the runtime's own types say %s", class_getName_(c),
              sel_getName_(method_getName_(m)), enc, ocerz_objc_refusal(rc), ocerz_objc_refusal(want));
        if (rc != OCERZ_OBJC_OK) {
            if (rc >= 0 && rc <= OCERZ_OBJC_MALFORMED) {
                if (!t->refused[rc])
                    snprintf(t->example[rc], sizeof t->example[rc], "%s %s %s", class_getName_(c),
                             sel_getName_(method_getName_(m)), enc);
                t->refused[rc]++;
            }
            continue;
        }
        t->converted++;
        OcerzAbiSig sig;
        CHECK(ocerz_abi_parse(out, &sig) == OCERZ_OK, "%s: notation %s does not parse", enc, out);
        unsigned n = method_getNumberOfArguments_(m);
        CHECK(nargs == (int)n && sig.nargs == (int)n, "%s: %d arguments in %s, the runtime says %u", enc,
              sig.nargs, out, n);
        char *rt = method_copyReturnType_(m);
        CHECK(class_size(sig.ret, &sig.ret_struct) == runtime_size(rt, 1),
              "%s %s: result %s is %lu bytes as %c, the runtime says %lu", class_getName_(c), enc, rt,
              class_size(sig.ret, &sig.ret_struct), sig.ret, runtime_size(rt, 1));
        free(rt);
        for (unsigned a = 0; a < n && a < (unsigned)sig.nargs; a++) {
            char *at = method_copyArgumentType_(m, a);
            CHECK(class_size(sig.arg[a], &sig.arg_struct[a]) == runtime_size(at, 1),
                  "%s %s: argument %u %s is %lu bytes as %c, the runtime says %lu", class_getName_(c), enc, a,
                  at, class_size(sig.arg[a], &sig.arg_struct[a]), sig.arg[a], runtime_size(at, 1));
            free(at);
        }
    }
    free(list);
}

static void test_encoding_sweep(void)
{
    static const char *const images[] = { OCERZ_OBJC_FOUNDATION, OCERZ_BRIDGE_COREFOUNDATION };
    Tally t;
    memset(&t, 0, sizeof t);
    int classes = 0;
    for (size_t k = 0; k < sizeof images / sizeof images[0]; k++) {
        Dl_info info;
        void *h = dlopen(images[k], RTLD_LAZY | RTLD_NOLOAD);
        void *probe = h ? dlsym(h, k == 0 ? "NSLog" : "CFRetain") : NULL;
        if (!probe || !dladdr(probe, &info)) {
            CHECK(0, "cannot find the image of %s", images[k]);
            continue;
        }
        unsigned n = 0;
        const char **names = objc_copyClassNamesForImage_(info.dli_fname, &n);
        for (unsigned i = 0; i < n; i++) {
            void *c = objc_getClass_(names[i]);
            if (!c)
                continue;
            classes++;
            sweep_class(c, &t);
            sweep_class(object_getClass_(c), &t);
        }
        free(names);
    }
    CHECK(classes > 500 && t.methods > 10000, "swept %d classes and %d methods, expected far more", classes,
          t.methods);
    fprintf(stderr, "test_objcbridge: %d classes, %d method encodings, %d converted\n", classes, t.methods,
            t.converted);
    for (int r = 1; r <= OCERZ_OBJC_MALFORMED; r++)
        if (t.refused[r])
            fprintf(stderr, "test_objcbridge:   %5d refused for %s, e.g. %s\n", t.refused[r],
                    ocerz_objc_refusal(r), t.example[r]);
}

typedef struct FormatCase {
    int dialect;
    const char *fmt;
    const char *classes;
} FormatCase;

static const FormatCase kFormats[] = {
    { OCERZ_OBJC_FMT_C, "plain", "" },
    { OCERZ_OBJC_FMT_C, "%d %i %u %x %X %o", "iiiiii" },
    { OCERZ_OBJC_FMT_C, "%hhd %hd %ld %lld %qd %jd %zd %td", "iillllll" },
    { OCERZ_OBJC_FMT_C, "%lu %llx %zu %tx %jo", "lllll" },
    { OCERZ_OBJC_FMT_C, "%Ld", "i" },
    { OCERZ_OBJC_FMT_C, "%D %O %U", "lll" },
    { OCERZ_OBJC_FMT_C, "%c %lc %C %s %ls %S %p", "iiipppL" },
    { OCERZ_OBJC_FMT_C, "%f %e %g %a %F %E %G %A %lf", "ddddddddd" },
    { OCERZ_OBJC_FMT_C, "%% %5.2f%% %-10s|%+d|% d|%#x|%'d|%08.3f", "dpiiiid" },
    { OCERZ_OBJC_FMT_C, "%*d %.*s %*.*f %-*c", "iiipiidii" },
    { OCERZ_OBJC_FMT_C, "trailing %", "" },
    { OCERZ_OBJC_FMT_CF, "%@ %d %f %s %p", "pidpL" },
    { OCERZ_OBJC_FMT_CF, "%D %O %U %lD %qU", "iiill" },
    { OCERZ_OBJC_FMT_CF, "%C %c %S %hhu %hu %lu %zd", "iipiill" },
    { OCERZ_OBJC_FMT_CF, "%.3g %%@ %@%@", "dpp" },
    { OCERZ_OBJC_FMT_PREDICATE, "name == %@ AND age > %d AND %K BEGINSWITH %@", "pipp" },
    { OCERZ_OBJC_FMT_PREDICATE, "title == '%@' OR x == \"%K\" OR y == %f", "d" },
    { OCERZ_OBJC_FMT_PREDICATE, "s == 'it\\'s %@' AND z == %ld", "l" },
    { OCERZ_OBJC_FMT_TYPES, "i@d", "ppp" },
    { OCERZ_OBJC_FMT_TYPES, "{CGPoint=dd}^v[4c]@\"NSString\"r*", "ppppp" },
    { OCERZ_OBJC_FMT_TYPES, "", "" },
};

typedef struct FormatRefusal {
    int dialect;
    const char *fmt;
    const char *why;
} FormatRefusal;

static const FormatRefusal kFormatRefusals[] = {
    { OCERZ_OBJC_FMT_C, "%n", "%n" },
    { OCERZ_OBJC_FMT_C, "%d %hhn", "%n" },
    { OCERZ_OBJC_FMT_C, "%Lf", "long double" },
    { OCERZ_OBJC_FMT_CF, "%Lg", "long double" },
    { OCERZ_OBJC_FMT_C, "%1$d", "positional" },
    { OCERZ_OBJC_FMT_CF, "%2$@ %1$@", "positional" },
    { OCERZ_OBJC_FMT_C, "%*1$d", "positional" },
    { OCERZ_OBJC_FMT_C, "%.*2$f", "positional" },
    { OCERZ_OBJC_FMT_C, "%@", "C format" },
    { OCERZ_OBJC_FMT_CF, "%K", "predicate" },
    { OCERZ_OBJC_FMT_C, "%y", "dialect" },
    { OCERZ_OBJC_FMT_C, "%vd", "dialect" },
    { OCERZ_OBJC_FMT_TYPES, "{S=dd", "type list" },
};

static void test_formats(void)
{
    for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; i++) {
        const FormatCase *f = &kFormats[i];
        char out[64];
        const char *why = "unset";
        int n = ocerz_objc_format_classes(f->fmt, f->dialect, out, sizeof out, &why);
        CHECK(n == (int)strlen(f->classes) && strcmp(out, f->classes) == 0 && why == NULL,
              "dialect %d \"%s\": %d classes \"%s\", want \"%s\" (%s)", f->dialect, f->fmt, n,
              n >= 0 ? out : "", f->classes, why ? why : "no reason");
    }
    for (size_t i = 0; i < sizeof kFormatRefusals / sizeof kFormatRefusals[0]; i++) {
        const FormatRefusal *f = &kFormatRefusals[i];
        char out[64];
        const char *why = NULL;
        int n = ocerz_objc_format_classes(f->fmt, f->dialect, out, sizeof out, &why);
        CHECK(n == -1 && why && strstr(why, f->why), "dialect %d \"%s\": %d, reason %s, want a refusal naming %s",
              f->dialect, f->fmt, n, why ? why : "(none)", f->why);
    }

    char many[4 * OCERZ_OBJC_VARIADIC_MAX + 8] = "";
    for (int i = 0; i <= OCERZ_OBJC_VARIADIC_MAX; i++)
        strcat(many, "%d");
    char out[OCERZ_OBJC_VARIADIC_MAX + 1];
    const char *why = NULL;
    CHECK(ocerz_objc_format_classes(many, OCERZ_OBJC_FMT_C, out, sizeof out, &why) == -1 && why &&
          strstr(why, "more"), "a format with more conversions than slots is refused");
    many[2 * OCERZ_OBJC_VARIADIC_MAX] = '\0';
    CHECK(ocerz_objc_format_classes(many, OCERZ_OBJC_FMT_C, out, sizeof out, &why) == OCERZ_OBJC_VARIADIC_MAX,
          "a format with exactly as many conversions as slots is taken");
    CHECK(ocerz_objc_format_classes(NULL, OCERZ_OBJC_FMT_CF, out, sizeof out, &why) == 0,
          "a null format consumes nothing");
}

static void test_variadic_table(void)
{
    static const struct { const char *sel; int kind; int arg; int dialect; } rows[] = {
        { "stringWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "initWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "initWithFormat:locale:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "appendFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "localizedStringWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "stringByAppendingFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF },
        { "predicateWithFormat:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_PREDICATE },
        { "raise:format:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF },
        { "stringWithValidatedFormat:validFormatSpecifiers:error:", OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF },
        { "handleFailureInMethod:object:file:lineNumber:description:", OCERZ_OBJC_VA_FORMAT, 6,
          OCERZ_OBJC_FMT_CF },
        { "encodeValuesOfObjCTypes:", OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_TYPES },
        { "arrayWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
        { "initWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
        { "setWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
        { "dictionaryWithObjectsAndKeys:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
        { "initWithObjectsAndKeys:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
        { "orderedSetWithObjects:", OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const OcerzObjcVariadic *v = ocerz_objc_variadic(rows[i].sel);
        CHECK(v && v->kind == rows[i].kind && v->arg == rows[i].arg &&
              (rows[i].kind != OCERZ_OBJC_VA_FORMAT || v->dialect == rows[i].dialect),
              "%s is variadic of kind %d at argument %d", rows[i].sel, rows[i].kind, rows[i].arg);
    }
    static const char *const fixed[] = {
        "exceptionWithName:reason:userInfo:", "initWithObjects:count:", "arrayWithObjects:count:",
        "stringWithString:", "initWithFormat:arguments:", "raise:format:arguments:", "length", NULL,
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        CHECK(ocerz_objc_variadic(fixed[i]) == NULL, "%s is not variadic", fixed[i] ? fixed[i] : "(null)");
}

static void test_va_cursor(void)
{
    OcerzAbiSig sig;
    OcerzAbiVaList va;
    uint64_t v;
    OcerzCPU *cpu = fresh_cpu();

    CHECK(ocerz_abi_parse("i(p)", &sig) == OCERZ_OK, "i(p) parses");
    cpu->gpr[OCERZ_RDI] = 0x1000;
    cpu->gpr[OCERZ_RSI] = 0xffffffff00000000ull | 0xfffffff6u;
    cpu->gpr[OCERZ_RDX] = 0x123456789abcdef0ull;
    cpu->gpr[OCERZ_RCX] = 0x2000;
    cpu->gpr[OCERZ_R8] = 0xdead000000000007ull;
    cpu->gpr[OCERZ_R9] = 0;
    cpu->xmm[0].lo = 0x3ff8000000000000ull;
    cpu->xmm[1].lo = 0x4004000000000000ull;
    stack_arg(cpu, 0, 0x7777000000000011ull);
    stack_arg(cpu, 1, 0x3000);
    CHECK(ocerz_abi_va_start(&sig, cpu, &va) == OCERZ_OK, "va_start over i(p)");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'i', &v) == OCERZ_OK && v == (uint64_t)(int64_t)-10,
          "an int from rsi is sign-extended from its low half: %#llx", (unsigned long long)v);
    CHECK(ocerz_abi_va_arg(&va, cpu, 'd', &v) == OCERZ_OK && v == 0x3ff8000000000000ull,
          "the first double comes from xmm0 whatever integers came before: %#llx", (unsigned long long)v);
    CHECK(ocerz_abi_va_arg(&va, cpu, 'L', &v) == OCERZ_OK && v == 0x123456789abcdef0ull, "a word from rdx");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'p', &v) == OCERZ_OK && v == 0x2000, "a pointer from rcx");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'u', &v) == OCERZ_OK && v == 7, "an unsigned from r8 is zero-extended: %#llx",
          (unsigned long long)v);
    CHECK(ocerz_abi_va_arg(&va, cpu, 'p', &v) == OCERZ_OK && v == 0, "a null pointer from r9 stays null");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'd', &v) == OCERZ_OK && v == 0x4004000000000000ull, "the second double");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'i', &v) == OCERZ_OK && v == 0x11,
          "the seventh integer is the first stack eightbyte, extended from its low half: %#llx",
          (unsigned long long)v);
    CHECK(ocerz_abi_va_arg(&va, cpu, 'l', &v) == OCERZ_OK && v == 0x3000, "then the second eightbyte");

    cpu = fresh_cpu();
    CHECK(ocerz_abi_parse("v(ddddddddi)", &sig) == OCERZ_OK, "v(ddddddddi) parses");
    cpu->gpr[OCERZ_RDI] = 5;
    cpu->gpr[OCERZ_RSI] = 0x55;
    cpu->gpr[OCERZ_RDX] = 0x66;
    stack_arg(cpu, 0, 0x400c000000000000ull);
    stack_arg(cpu, 1, 0x77);
    CHECK(ocerz_abi_va_start(&sig, cpu, &va) == OCERZ_OK, "va_start after eight doubles");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'l', &v) == OCERZ_OK && v == 0x55,
          "the next integer is rsi, after the named int took rdi");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'd', &v) == OCERZ_OK && v == 0x400c000000000000ull,
          "a double past xmm7 is the first stack eightbyte");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'l', &v) == OCERZ_OK && v == 0x66,
          "an integer after it is rdx, not the stack: %#llx", (unsigned long long)v);

    cpu = fresh_cpu();
    CHECK(ocerz_abi_parse("{LLL}(p{LL}p{dd})", &sig) == OCERZ_OK, "a MEMORY result and two structures parse");
    cpu->gpr[OCERZ_RDI] = 0xaaaa;
    cpu->gpr[OCERZ_RSI] = 1;
    cpu->gpr[OCERZ_RDX] = 2;
    cpu->gpr[OCERZ_RCX] = 3;
    cpu->gpr[OCERZ_R8] = 4;
    cpu->gpr[OCERZ_R9] = 0x99;
    cpu->xmm[2].lo = 0x4020000000000000ull;
    CHECK(ocerz_abi_va_start(&sig, cpu, &va) == OCERZ_OK, "va_start past the result pointer and structures");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'l', &v) == OCERZ_OK && v == 0x99,
          "rdi is the result pointer, {LL} took rdx and rcx, so the next integer is r9");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'd', &v) == OCERZ_OK && v == 0x4020000000000000ull,
          "{dd} took xmm0 and xmm1, so the next double is xmm2");

    cpu = fresh_cpu();
    CHECK(ocerz_abi_parse("v(lllll{LL}l)", &sig) == OCERZ_OK, "a structure that does not fit parses");
    cpu->gpr[OCERZ_R9] = 0x606;
    stack_arg(cpu, 0, 0xa);
    stack_arg(cpu, 1, 0xb);
    stack_arg(cpu, 2, 0xc);
    CHECK(ocerz_abi_va_start(&sig, cpu, &va) == OCERZ_OK, "va_start past a stacked structure");
    CHECK(ocerz_abi_va_arg(&va, cpu, 'l', &v) == OCERZ_OK && v == 0xc,
          "{LL} after five longs is stacked at 0 and 8 and the named long after it took r9, so the first"
          " variadic word is stack 16: %#llx", (unsigned long long)v);

    for (const char *bad = "fbBhH{c"; *bad; bad++)
        CHECK(ocerz_abi_va_arg(&va, cpu, *bad, &v) == OCERZ_EUNSUP, "class %c is no variadic argument", *bad);
    CHECK(ocerz_abi_va_start(NULL, cpu, &va) == OCERZ_EUNDEF, "va_start with no signature");
}

static void test_selrefs(void)
{
    uint64_t base = ocerz_map_anywhere(0x4000, PROT_READ | PROT_WRITE);
    CHECK(base != 0, "an image page");
    uint8_t *img = ocerz_g2h(base);
    memset(img, 0, 0x4000);

    const uint64_t strings = 0x2000, selrefs = 0x3000, msgrefs = 0x3040, text = 0x3080, other = 0x30c0;
    const char *names[] = { "length", "fooBar:baz:", "count", "ocerzTestSelectorNobodyRegistered:" };
    uint64_t at = strings, addr[4];
    for (int i = 0; i < 4; i++) {
        addr[i] = at;
        strcpy((char *)img + at, names[i]);
        at += strlen(names[i]) + 1;
    }

    struct mach_header_64 h = { .magic = MH_MAGIC_64, .filetype = MH_DYLIB, .ncmds = 2 };
    struct segment_command_64 data = { .cmd = LC_SEGMENT_64, .nsects = 3 };
    struct segment_command_64 tseg = { .cmd = LC_SEGMENT_64, .nsects = 1 };
    struct section_64 s_sel = { .addr = selrefs, .size = 32 };
    struct section_64 s_msg = { .addr = msgrefs, .size = 16 };
    struct section_64 s_text = { .addr = text, .size = 8 };
    struct section_64 s_other = { .addr = other, .size = 8 };
    data.cmdsize = (uint32_t)(sizeof data + 3 * sizeof(struct section_64));
    tseg.cmdsize = (uint32_t)(sizeof tseg + sizeof(struct section_64));
    memcpy(data.segname, "__DATA_CONST", 12);
    memcpy(tseg.segname, "__TEXT", 6);
    memcpy(s_sel.sectname, "__objc_selrefs", 14);
    memcpy(s_msg.sectname, "__objc_msgrefs", 14);
    memcpy(s_text.sectname, "__objc_selrefs", 14);
    memcpy(s_other.sectname, "__objc_classref", 15);
    h.sizeofcmds = data.cmdsize + tseg.cmdsize;

    uint8_t *p = img;
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    memcpy(p, &data, sizeof data);
    p += sizeof data;
    memcpy(p, &s_sel, sizeof s_sel);
    p += sizeof s_sel;
    memcpy(p, &s_msg, sizeof s_msg);
    p += sizeof s_msg;
    memcpy(p, &s_other, sizeof s_other);
    p += sizeof s_other;
    memcpy(p, &tseg, sizeof tseg);
    p += sizeof tseg;
    memcpy(p, &s_text, sizeof s_text);

    ocerz_st(base + selrefs, 8, base + addr[0]);
    ocerz_st(base + selrefs + 8, 8, 0);
    ocerz_st(base + selrefs + 16, 8, base + addr[1]);
    ocerz_st(base + selrefs + 24, 8, base + addr[3]);
    ocerz_st(base + msgrefs, 8, 0x1234);
    ocerz_st(base + msgrefs + 8, 8, base + addr[2]);
    ocerz_st(base + text, 8, base + addr[0]);
    ocerz_st(base + other, 8, base + addr[0]);

    int n = ocerz_objcbridge_fix_selrefs(img, (int64_t)base);
    CHECK(n == 4, "four selector words rewritten, got %d", n);
    CHECK(ocerz_ld(base + selrefs, 8) == ocerz_h2g(sel("length")), "a selref becomes the native SEL");
    CHECK(ocerz_ld(base + selrefs + 8, 8) == 0, "a null selref stays null");
    CHECK(ocerz_ld(base + selrefs + 16, 8) == ocerz_h2g(sel("fooBar:baz:")), "a second selref");
    uint64_t fresh = ocerz_ld(base + selrefs + 24, 8);
    CHECK(fresh != base + addr[3] && fresh == ocerz_h2g(sel(names[3])),
          "a selector nobody registered is registered, and is not the image's string");
    img[addr[3]] = 'X';
    CHECK(strcmp(sel_getName_((void *)(uintptr_t)fresh), names[3]) == 0,
          "the registered name does not point into the image");
    CHECK(ocerz_ld(base + msgrefs, 8) == 0x1234, "a message reference keeps its implementation word");
    CHECK(ocerz_ld(base + msgrefs + 8, 8) == ocerz_h2g(sel("count")), "and gets the native SEL");
    CHECK(ocerz_ld(base + text, 8) == base + addr[0], "a section of the name outside __DATA is untouched");
    CHECK(ocerz_ld(base + other, 8) == base + addr[0], "a __DATA section of another name is untouched");
    CHECK(ocerz_objcbridge_fix_selrefs(img, (int64_t)base) == 0, "a second pass rewrites nothing");
    CHECK(ocerz_objcbridge_fix_selrefs(NULL, 0) == 0, "no image, nothing");
    uint32_t bad = MH_MAGIC;
    CHECK(ocerz_objcbridge_fix_selrefs((const uint8_t *)&bad, 0) == 0, "a 32-bit header is not walked");
}

static void test_nil(void)
{
    OcerzCPU *cpu = fresh_cpu();
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = 0;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
    CHECK(ocerz_objc_msgSend(&vm, cpu) == OCERZ_STEP_OK, "a send to nil");
    CHECK(returned(cpu, rsp), "a send to nil returns");
    CHECK(cpu->gpr[OCERZ_RAX] == 0 && cpu->gpr[OCERZ_RDX] == 0 && cpu->xmm[0].lo == 0 && cpu->xmm[0].hi == 0 &&
          cpu->xmm[1].lo == 0 && cpu->xmm[1].hi == 0, "a send to nil zeroes rax, rdx, xmm0 and xmm1");

    uint64_t buf = scratch + 0x100;
    memset(ocerz_g2h(buf), 0xaa, 64);
    cpu = fresh_cpu();
    rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = buf;
    cpu->gpr[OCERZ_RSI] = 0;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(sel("rectValue"));
    CHECK(ocerz_objc_msgSend_stret(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp), "a _stret send to nil");
    CHECK(cpu->gpr[OCERZ_RAX] == buf && cpu->gpr[OCERZ_RDX] == 0 && cpu->xmm[0].lo == 0 && cpu->xmm[1].lo == 0,
          "it answers the result pointer in rax");
    CHECK(((uint8_t *)ocerz_g2h(buf))[0] == 0xaa && ((uint8_t *)ocerz_g2h(buf))[31] == 0xaa,
          "and leaves the memory, whose size no class says");

    uint64_t super = scratch + 0x200;
    ocerz_st(super, 8, 0);
    ocerz_st(super + 8, 8, ocerz_h2g(cls("NSValue")));
    cpu = fresh_cpu();
    rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = buf;
    cpu->gpr[OCERZ_RSI] = super;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(sel("rectValue"));
    CHECK(ocerz_objc_msgSendSuper_stret(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp),
          "a super _stret send to nil");
    uint8_t *b = ocerz_g2h(buf);
    int zero = 1;
    for (int i = 0; i < 32; i++)
        zero &= b[i] == 0;
    CHECK(zero && b[32] == 0xaa && cpu->gpr[OCERZ_RAX] == buf,
          "zeroes exactly the 32 bytes of the CGRect its class names");

    cpu = fresh_cpu();
    rsp = cpu->gpr[OCERZ_RSP];
    uint64_t super2 = scratch + 0x220;
    ocerz_st(super2, 8, 0);
    ocerz_st(super2 + 8, 8, ocerz_h2g(cls("NSString")));
    cpu->gpr[OCERZ_RDI] = super2;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
    CHECK(ocerz_objc_msgSendSuper2(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp) &&
          cpu->gpr[OCERZ_RAX] == 0 && cpu->gpr[OCERZ_RDX] == 0, "a super2 send to nil answers zero");
}

static void test_sends(void)
{
    void *hello = nsstr("hello, world");
    OcerzCPU *cpu = fresh_cpu();
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(hello);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
    CHECK(ocerz_objc_msgSend(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp) && cpu->gpr[OCERZ_RAX] == 12,
          "-length answers 12 in rax, got %#llx", (unsigned long long)cpu->gpr[OCERZ_RAX]);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(hello);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("rangeOfString:"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("world"));
    ocerz_objc_msgSend(&vm, cpu);
    CHECK(cpu->gpr[OCERZ_RAX] == 7 && cpu->gpr[OCERZ_RDX] == 5, "-rangeOfString: answers {7, 5} in rax:rdx");

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(hello);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("isEqual:"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("hello, world"));
    ocerz_objc_msgSend(&vm, cpu);
    CHECK(cpu->gpr[OCERZ_RAX] == 1, "-isEqual: answers YES");

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(cls("NSNumber"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("numberWithFloat:"));
    cpu->xmm[0].lo = 0x3fc00000u;
    ocerz_objc_msgSend(&vm, cpu);
    void *num = (void *)(uintptr_t)cpu->gpr[OCERZ_RAX];
    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(num);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("doubleValue"));
    ocerz_objc_msgSend(&vm, cpu);
    CHECK(cpu->xmm[0].lo == 0x3ff8000000000000ull && cpu->xmm[0].hi == 0,
          "+numberWithFloat: takes a float from xmm0 and -doubleValue answers 1.5 in xmm0");

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(cls("NSString"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("stringWithFormat:"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("%d|%@|%.2f|%ld|%s|%f|%c|%lu|%d"));
    cpu->gpr[OCERZ_RCX] = 0xffffffff00000000ull | (uint32_t)-42;
    cpu->gpr[OCERZ_R8] = ocerz_h2g(nsstr("obj"));
    cpu->xmm[0].lo = 0x4004000000000000ull;
    cpu->gpr[OCERZ_R9] = 1234567890123ull;
    stack_arg(cpu, 0, ocerz_h2g("cstr"));
    cpu->xmm[1].lo = 0x3fe0000000000000ull;
    stack_arg(cpu, 1, 0xabcdef00000000ull | 'Z');
    stack_arg(cpu, 2, 18446744073709551615ull);
    stack_arg(cpu, 3, 0x1111111100000007ull);
    ocerz_objc_msgSend(&vm, cpu);
    char text[256];
    cfstr_text((void *)(uintptr_t)cpu->gpr[OCERZ_RAX], text, sizeof text);
    CHECK(strcmp(text, "-42|obj|2.50|1234567890123|cstr|0.500000|Z|18446744073709551615|7") == 0,
          "+stringWithFormat: gathers registers, xmm and stack slots: \"%s\"", text);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(cls("NSArray"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("arrayWithObjects:"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("a"));
    cpu->gpr[OCERZ_RCX] = ocerz_h2g(nsstr("b"));
    cpu->gpr[OCERZ_R8] = ocerz_h2g(nsstr("c"));
    cpu->gpr[OCERZ_R9] = ocerz_h2g(nsstr("d"));
    stack_arg(cpu, 0, ocerz_h2g(nsstr("e")));
    stack_arg(cpu, 1, 0);
    ocerz_objc_msgSend(&vm, cpu);
    void *arr = (void *)(uintptr_t)cpu->gpr[OCERZ_RAX];
    unsigned long count = ((unsigned long (*)(void *, void *))objc_msgSend_)(arr, sel("count"));
    CHECK(count == 5, "+arrayWithObjects: stops at the nil on the stack, got %lu", count);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(cls("NSArray"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("arrayWithObjects:"));
    cpu->gpr[OCERZ_RDX] = 0;
    ocerz_objc_msgSend(&vm, cpu);
    count = ((unsigned long (*)(void *, void *))objc_msgSend_)((void *)(uintptr_t)cpu->gpr[OCERZ_RAX], sel("count"));
    CHECK(count == 0, "+arrayWithObjects:nil reads no variadic argument, got %lu", count);

    double rect[4] = { 1.5, 2.5, 30, 40 };
    void *value = ((void *(*)(void *, void *, double, double, double, double))objc_msgSend_)(
        cls("NSValue"), sel("valueWithRect:"), rect[0], rect[1], rect[2], rect[3]);
    uint64_t out = scratch + 0x300;
    memset(ocerz_g2h(out), 0x55, 40);
    cpu = fresh_cpu();
    uint64_t rsp2 = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = out;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(value);
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(sel("rectValue"));
    CHECK(ocerz_objc_msgSend_stret(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp2), "-rectValue by _stret");
    CHECK(memcmp(ocerz_g2h(out), rect, sizeof rect) == 0 && ((uint8_t *)ocerz_g2h(out))[32] == 0x55 &&
          cpu->gpr[OCERZ_RAX] == out, "writes the CGRect through the guest's pointer and answers it in rax");

    uint64_t super = scratch + 0x400;
    void *mstr = ((void *(*)(void *, void *, void *))objc_msgSend_)(cls("NSMutableString"), sel("stringWithString:"),
                                                                    nsstr("abc"));
    ocerz_st(super, 8, ocerz_h2g(mstr));
    ocerz_st(super + 8, 8, ocerz_h2g(object_getClass_(mstr)));
    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = super;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
    ocerz_objc_msgSendSuper(&vm, cpu);
    CHECK(cpu->gpr[OCERZ_RAX] == 3, "a super send through a guest objc_super reaches the receiver");

    uint64_t guest_buf = scratch + 0x500;
    cpu = fresh_cpu();
    rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = guest_buf;
    cpu->gpr[OCERZ_RSI] = 64;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g("%d %s %.2f %ld %p %c%%");
    cpu->gpr[OCERZ_RCX] = 0xdeadbeef00000000ull | (uint32_t)-7;
    cpu->gpr[OCERZ_R8] = ocerz_h2g("str");
    cpu->xmm[0].lo = 0x400c000000000000ull;
    cpu->gpr[OCERZ_R9] = (uint64_t)-5;
    stack_arg(cpu, 0, 0x1234);
    stack_arg(cpu, 1, 0x7700000000000000ull | 'k');
    CHECK(ocerz_fmt_snprintf(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp), "snprintf crosses");
    CHECK(strcmp(ocerz_g2h(guest_buf), "-7 str 3.50 -5 0x1234 k%") == 0 && cpu->gpr[OCERZ_RAX] == 24,
          "snprintf formats \"%s\" and answers %lld", (char *)ocerz_g2h(guest_buf), (long long)cpu->gpr[OCERZ_RAX]);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = guest_buf;
    cpu->gpr[OCERZ_RSI] = 4;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g("%s");
    cpu->gpr[OCERZ_RCX] = ocerz_h2g("truncated");
    ocerz_fmt_snprintf(&vm, cpu);
    CHECK(strcmp(ocerz_g2h(guest_buf), "tru") == 0 && cpu->gpr[OCERZ_RAX] == 9,
          "snprintf truncates and answers the full length");

    cpu = fresh_cpu();
    uint64_t strp = scratch + 0x600;
    cpu->gpr[OCERZ_RDI] = strp;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g("%d-%d");
    cpu->gpr[OCERZ_RDX] = 4;
    cpu->gpr[OCERZ_RCX] = 2;
    ocerz_fmt_asprintf(&vm, cpu);
    char *as = (char *)(uintptr_t)ocerz_ld(strp, 8);
    CHECK(as && strcmp(as, "4-2") == 0 && cpu->gpr[OCERZ_RAX] == 3, "asprintf allocates \"4-2\"");
    free(as);

    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe");
    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = (uint64_t)fds[1] | 0xffffffff00000000ull;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g("fd %d %f\n");
    cpu->gpr[OCERZ_RDX] = 9;
    cpu->xmm[0].lo = 0x3fd0000000000000ull;
    ocerz_fmt_dprintf(&vm, cpu);
    char got[64] = { 0 };
    ssize_t r = read(fds[0], got, sizeof got - 1);
    CHECK(r == 14 && strcmp(got, "fd 9 0.250000\n") == 0 && cpu->gpr[OCERZ_RAX] == 14,
          "dprintf writes to the int fd from edi, ignoring the upper half");
    close(fds[0]);
    close(fds[1]);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = (uint64_t)fds[1];
    cpu->gpr[OCERZ_RSI] = ocerz_h2g("closed %d\n");
    cpu->gpr[OCERZ_RDX] = 1;
    errno = 0;
    ocerz_fmt_dprintf(&vm, cpu);
    CHECK((int32_t)cpu->gpr[OCERZ_RAX] == -1 && errno == EBADF,
          "dprintf to a closed fd answers -1 with errno EBADF, got %d errno %d", (int)cpu->gpr[OCERZ_RAX], errno);

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = guest_buf;
    cpu->gpr[OCERZ_RSI] = 0;
    cpu->gpr[OCERZ_RDX] = 64;
    cpu->gpr[OCERZ_RCX] = ocerz_h2g("chk %s %d");
    cpu->gpr[OCERZ_R8] = ocerz_h2g("ok");
    cpu->gpr[OCERZ_R9] = 3;
    ocerz_fmt_sprintf_chk(&vm, cpu);
    CHECK(strcmp(ocerz_g2h(guest_buf), "chk ok 3") == 0 && cpu->gpr[OCERZ_RAX] == 8,
          "__sprintf_chk takes its format fourth");

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = guest_buf;
    cpu->gpr[OCERZ_RSI] = 64;
    cpu->gpr[OCERZ_RDX] = 0;
    cpu->gpr[OCERZ_RCX] = 64;
    cpu->gpr[OCERZ_R8] = ocerz_h2g("%s|%d|%d");
    cpu->gpr[OCERZ_R9] = ocerz_h2g("snchk");
    stack_arg(cpu, 0, 11);
    stack_arg(cpu, 1, 12);
    ocerz_fmt_snprintf_chk(&vm, cpu);
    CHECK(strcmp(ocerz_g2h(guest_buf), "snchk|11|12") == 0, "__snprintf_chk takes its variadic arguments from"
          " r9 and then the stack: \"%s\"", (char *)ocerz_g2h(guest_buf));

    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = 0;
    cpu->gpr[OCERZ_RSI] = 0;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("cf %d %@ %.1f"));
    cpu->gpr[OCERZ_RCX] = 5;
    cpu->gpr[OCERZ_R8] = ocerz_h2g(nsstr("x"));
    cpu->xmm[0].lo = 0x4022000000000000ull;
    ocerz_fmt_CFStringCreateWithFormat(&vm, cpu);
    void *cfs = (void *)(uintptr_t)cpu->gpr[OCERZ_RAX];
    cfstr_text(cfs, text, sizeof text);
    CHECK(strcmp(text, "cf 5 x 9.0") == 0, "CFStringCreateWithFormat: \"%s\"", text);

    void *mutable = ((void *(*)(void *, void *, void *))objc_msgSend_)(cls("NSMutableString"),
                                                                       sel("stringWithString:"), nsstr("m"));
    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(mutable);
    cpu->gpr[OCERZ_RSI] = 0;
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("+%lu"));
    cpu->gpr[OCERZ_RCX] = 77;
    rsp = cpu->gpr[OCERZ_RSP];
    ocerz_fmt_CFStringAppendFormat(&vm, cpu);
    cfstr_text(mutable, text, sizeof text);
    CHECK(strcmp(text, "m+77") == 0 && returned(cpu, rsp) && cpu->gpr[OCERZ_RAX] == 0,
          "CFStringAppendFormat: \"%s\"", text);

    CHECK(ocerz_bridge_in_flight() == NULL, "no bridge frame is left raised");
}

typedef struct Refusal {
    const char *what;
    void (*setup)(OcerzCPU *cpu);
    int (*handler)(struct OcerzVM *, OcerzCPU *);
    const char *message;
} Refusal;

static void r_memory_plain(OcerzCPU *cpu)
{
    double r[4] = { 1, 2, 3, 4 };
    void *v = ((void *(*)(void *, void *, double, double, double, double))objc_msgSend_)(
        cls("NSValue"), sel("valueWithRect:"), r[0], r[1], r[2], r[3]);
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(v);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("rectValue"));
}

static void r_stret_small(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = scratch + 0x100;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(nsstr("abc"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(sel("length"));
}

static void r_fpret(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(nsstr("abc"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("doubleValue"));
}

static void r_unrecognized(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(nsstr("abc"));
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("ocerzNoSuchMethod"));
}

static void r_guest_block(OcerzCPU *cpu)
{
    uint64_t block = scratch + 0x700;
    ocerz_st(block, 8, 0);
    ocerz_st(block + 8, 8, 0);
    ocerz_st(block + 16, 8, scratch + 0x800);
    ocerz_st(block + 24, 8, 0);
    void *a = ((void *(*)(void *, void *, void *))objc_msgSend_)(cls("NSArray"), sel("arrayWithObject:"), nsstr("x"));
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(a);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("enumerateObjectsUsingBlock:"));
    cpu->gpr[OCERZ_RDX] = block;
}

static void r_guest_fnptr(OcerzCPU *cpu)
{
    void *a = ((void *(*)(void *, void *, void *))objc_msgSend_)(cls("NSArray"), sel("arrayWithObject:"), nsstr("x"));
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(a);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("sortedArrayUsingFunction:context:"));
    cpu->gpr[OCERZ_RDX] = scratch + 0x800;
    cpu->gpr[OCERZ_RCX] = 0;
}

static void r_bitfield(OcerzCPU *cpu)
{
    void *d = ((void *(*)(void *, void *, void *))objc_msgSend_)(cls("NSDecimalNumber"),
                                                                 sel("decimalNumberWithString:"), nsstr("1.5"));
    cpu->gpr[OCERZ_RDI] = scratch + 0x100;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(d);
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(sel("decimalValue"));
}

static void r_printf_n(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = ocerz_h2g("count %n");
    cpu->gpr[OCERZ_RSI] = scratch + 0x100;
}

static void r_nslog_positional(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(nsstr("%2$@ %1$@"));
}

static void r_null_super(OcerzCPU *cpu)
{
    cpu->gpr[OCERZ_RDI] = 0;
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
}

static const Refusal kRefusals[] = {
    { "a MEMORY result through plain objc_msgSend", r_memory_plain, ocerz_objc_msgSend,
      "rectValue] returns a structure System V returns in memory" },
    { "a register result through objc_msgSend_stret", r_stret_small, ocerz_objc_msgSend_stret,
      "length] was sent with objc_msgSend_stret, but its result" },
    { "objc_msgSend_fpret", r_fpret, ocerz_objc_msgSend_fpret, "long double result" },
    { "objc_msgSend_fp2ret", r_fpret, ocerz_objc_msgSend_fp2ret, "long double _Complex result" },
    { "an unrecognized selector", r_unrecognized, ocerz_objc_msgSend,
      "ocerzNoSuchMethod] is not a selector the receiver recognizes" },
    { "a guest block", r_guest_block, ocerz_objc_msgSend,
      "enumerateObjectsUsingBlock:] cannot cross: argument 0 is a block whose code is x86" },
    { "a guest function pointer", r_guest_fnptr, ocerz_objc_msgSend,
      "sortedArrayUsingFunction:context:] cannot cross: argument 0 is an x86 function pointer" },
    { "a bitfield result", r_bitfield, ocerz_objc_msgSend_stret, "decimalValue] cannot cross: its method type"
      " encoding {" },
    { "printf %n", r_printf_n, ocerz_fmt_printf, "_printf refuses the format \"count %n\": it has %n" },
    { "NSLog positional", r_nslog_positional, ocerz_fmt_NSLog, "_NSLog refuses the format \"%2$@ %1$@\": it has"
      " a positional argument" },
    { "a null objc_super", r_null_super, ocerz_objc_msgSendSuper, "_objc_msgSendSuper was handed a null struct"
      " objc_super" },
};

static int run_child(void (*setup)(OcerzCPU *), int (*handler)(struct OcerzVM *, OcerzCPU *), char *err,
                     size_t errlen, int *status)
{
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        OcerzCPU *cpu = fresh_cpu();
        setup(cpu);
        handler(&vm, cpu);
        _exit(0);
    }
    close(fds[1]);
    size_t have = 0;
    ssize_t r;
    while (have + 1 < errlen && (r = read(fds[0], err + have, errlen - 1 - have)) > 0)
        have += (size_t)r;
    err[have] = '\0';
    close(fds[0]);
    return pid > 0 && waitpid(pid, status, 0) == pid;
}

static void test_refusals(void)
{
    for (size_t i = 0; i < sizeof kRefusals / sizeof kRefusals[0]; i++) {
        const Refusal *r = &kRefusals[i];
        char err[4096];
        int status = 0;
        CHECK(run_child(r->setup, r->handler, err, sizeof err, &status), "%s: child ran", r->what);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 72, "%s: exits 72, status %#x", r->what, status);
        CHECK(strstr(err, "ocerz: bridge: ") && strstr(err, r->message), "%s: stderr names it: %s", r->what, err);
    }
}

static void s_null_block(OcerzCPU *cpu)
{
    void *a = ((void *(*)(void *, void *))objc_msgSend_)(cls("NSArray"), sel("array"));
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(a);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("enumerateObjectsUsingBlock:"));
    cpu->gpr[OCERZ_RDX] = 0;
}

static const struct { unsigned long reserved, size; } kBlockDescriptor = { 0, 32 };

static void s_native_block(OcerzCPU *cpu)
{
    void *a = ((void *(*)(void *, void *))objc_msgSend_)(cls("NSArray"), sel("array"));
    uint64_t block = scratch + 0x700;
    ocerz_st(block, 8, ocerz_h2g(dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock")));
    ocerz_st(block + 8, 8, 1u << 28);
    ocerz_st(block + 16, 8, ocerz_h2g(dlsym(RTLD_DEFAULT, "strlen")));
    ocerz_st(block + 24, 8, ocerz_h2g(&kBlockDescriptor));
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(a);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("enumerateObjectsUsingBlock:"));
    cpu->gpr[OCERZ_RDX] = block;
}

static void test_callables_allowed(void)
{
    char err[4096];
    int status = 0;
    CHECK(run_child(s_null_block, ocerz_objc_msgSend, err, sizeof err, &status) && !strstr(err, "cannot cross"),
          "a null block is not refused: %s", err);
    CHECK(run_child(s_native_block, ocerz_objc_msgSend, err, sizeof err, &status) && !strstr(err, "cannot cross") &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a block whose invoke is native code is not refused, and an empty array never calls it: %s", err);
}

static void *g_forward_target;

static void *forward_target_imp(void *self, void *cmd, void *selector)
{
    return g_forward_target;
}

static void *chain_target_imp(void *self, void *cmd, void *selector)
{
    return nsstr("forwarded twice");
}

static void test_forwarding_target(void)
{
    void *(*allocate)(void *, const char *, size_t) = dlsym(RTLD_DEFAULT, "objc_allocateClassPair");
    bool (*add_method)(void *, void *, void *, const char *) = dlsym(RTLD_DEFAULT, "class_addMethod");
    void (*register_pair)(void *) = dlsym(RTLD_DEFAULT, "objc_registerClassPair");
    void *nsobject = cls("NSObject");

    void *fwd = allocate(nsobject, "OcerzForwarder", 0);
    add_method(fwd, sel("forwardingTargetForSelector:"), (void *)forward_target_imp, "@24@0:8:16");
    register_pair(fwd);
    void *chain = allocate(nsobject, "OcerzForwarderChain", 0);
    add_method(chain, sel("forwardingTargetForSelector:"), (void *)chain_target_imp, "@24@0:8:16");
    register_pair(chain);

    void *obj = ((void *(*)(void *, void *))objc_msgSend_)(fwd, sel("new"));
    g_forward_target = nsstr("five!");
    OcerzCPU *cpu = fresh_cpu();
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(obj);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("length"));
    CHECK(ocerz_objc_msgSend(&vm, cpu) == OCERZ_STEP_OK && returned(cpu, rsp) && cpu->gpr[OCERZ_RAX] == 5,
          "a send the receiver forwards to a target takes the target's signature: -length answers %llu",
          (unsigned long long)cpu->gpr[OCERZ_RAX]);

    g_forward_target = ((void *(*)(void *, void *))objc_msgSend_)(chain, sel("new"));
    cpu = fresh_cpu();
    cpu->gpr[OCERZ_RDI] = ocerz_h2g(obj);
    cpu->gpr[OCERZ_RSI] = ocerz_h2g(sel("rangeOfString:"));
    cpu->gpr[OCERZ_RDX] = ocerz_h2g(nsstr("twice"));
    ocerz_objc_msgSend(&vm, cpu);
    CHECK(cpu->gpr[OCERZ_RAX] == 10 && cpu->gpr[OCERZ_RDX] == 5,
          "a target that forwards again is followed to the object with the method: {%llu, %llu}",
          (unsigned long long)cpu->gpr[OCERZ_RAX], (unsigned long long)cpu->gpr[OCERZ_RDX]);
}

typedef struct Syn {
    uint64_t base;
    uint64_t at;
    uint64_t size;
    int nsect;
    char sectname[16][17];
    uint64_t sectaddr[16];
    uint64_t sectsize[16];
} Syn;

#define SYN_SPAN 0x10000ull
#define SYN_HEADER 0x800ull

static void syn_init(Syn *s)
{
    memset(s, 0, sizeof *s);
    s->base = ocerz_map_anywhere(SYN_SPAN, PROT_READ | PROT_WRITE);
    if (!s->base) {
        fprintf(stderr, "synthetic image alloc failed\n");
        exit(2);
    }
    memset(ocerz_g2h(s->base), 0, SYN_SPAN);
    s->size = SYN_SPAN;
    s->at = SYN_HEADER;
}

static uint64_t syn_alloc(Syn *s, uint64_t n)
{
    uint64_t a = s->base + s->at;
    s->at = (s->at + n + 7) & ~7ull;
    if (s->at > s->size) {
        fprintf(stderr, "synthetic image overflow\n");
        exit(2);
    }
    return a;
}

static uint64_t syn_str(Syn *s, const char *str)
{
    uint64_t a = syn_alloc(s, strlen(str) + 1);
    strcpy(ocerz_g2h(a), str);
    return a;
}

static void syn_w(uint64_t addr, int size, uint64_t v)
{
    ocerz_st(addr, size, v);
}

typedef struct SynMethod {
    const char *name;
    const char *types;
    uint64_t imp;
} SynMethod;

static uint64_t syn_methods(Syn *s, const SynMethod *m, int n)
{
    if (n == 0)
        return 0;
    uint64_t list = syn_alloc(s, 8 + 24 * (uint64_t)n);
    syn_w(list, 4, 24);
    syn_w(list + 4, 4, (uint64_t)n);
    for (int i = 0; i < n; i++) {
        syn_w(list + 8 + 24 * (uint64_t)i, 8, syn_str(s, m[i].name));
        syn_w(list + 16 + 24 * (uint64_t)i, 8, m[i].types ? syn_str(s, m[i].types) : 0);
        syn_w(list + 24 + 24 * (uint64_t)i, 8, m[i].imp);
    }
    return list;
}

static uint64_t syn_refs(Syn *s, const uint64_t *refs, int n)
{
    uint64_t list = syn_alloc(s, 8 + 8 * (uint64_t)n);
    syn_w(list, 8, (uint64_t)n);
    for (int i = 0; i < n; i++)
        syn_w(list + 8 + 8 * (uint64_t)i, 8, refs[i]);
    return list;
}

static uint64_t syn_props(Syn *s, const char *const *pairs, int n)
{
    uint64_t list = syn_alloc(s, 8 + 16 * (uint64_t)n);
    syn_w(list, 4, 16);
    syn_w(list + 4, 4, (uint64_t)n);
    for (int i = 0; i < n; i++) {
        syn_w(list + 8 + 16 * (uint64_t)i, 8, syn_str(s, pairs[2 * i]));
        syn_w(list + 16 + 16 * (uint64_t)i, 8, syn_str(s, pairs[2 * i + 1]));
    }
    return list;
}

static uint64_t syn_protocol(Syn *s, const char *name, uint64_t adopted, uint64_t required, uint64_t props,
                             uint32_t size)
{
    uint64_t p = syn_alloc(s, 96);
    syn_w(p + 8, 8, syn_str(s, name));
    syn_w(p + 16, 8, adopted);
    syn_w(p + 24, 8, required);
    syn_w(p + 56, 8, props);
    syn_w(p + 64, 4, size);
    return p;
}

typedef struct SynClass {
    const char *name;
    uint64_t super;
    uint64_t supermeta;
    uint32_t flags;
    uint32_t start;
    uint32_t size;
    uint64_t imethods;
    uint64_t cmethods;
    uint64_t ivars;
    uint64_t protocols;
    uint64_t props;
    uint64_t swift_bits;
} SynClass;

static uint64_t syn_class(Syn *s, const SynClass *c, uint64_t *meta_out)
{
    uint64_t name = syn_str(s, c->name);
    uint64_t klass = syn_alloc(s, 40), meta = syn_alloc(s, 40);
    uint64_t ro = syn_alloc(s, 72), mro = syn_alloc(s, 72);
    syn_w(ro, 4, c->flags);
    syn_w(ro + 4, 4, c->start);
    syn_w(ro + 8, 4, c->size);
    syn_w(ro + 24, 8, name);
    syn_w(ro + 32, 8, c->imethods);
    syn_w(ro + 40, 8, c->protocols);
    syn_w(ro + 48, 8, c->ivars);
    syn_w(ro + 64, 8, c->props);
    syn_w(mro, 4, c->flags | 1);
    syn_w(mro + 4, 4, 40);
    syn_w(mro + 8, 4, 40);
    syn_w(mro + 24, 8, name);
    syn_w(mro + 32, 8, c->cmethods);
    syn_w(mro + 40, 8, c->protocols);
    syn_w(klass, 8, meta);
    syn_w(klass + 8, 8, c->super);
    syn_w(klass + 24, 8, 0x1122334455667788ull);
    syn_w(klass + 32, 8, ro | c->swift_bits);
    syn_w(meta, 8, ocerz_h2g(object_getClass_(cls("NSObject"))));
    syn_w(meta + 8, 8, c->supermeta);
    syn_w(meta + 32, 8, mro);
    if (meta_out)
        *meta_out = meta;
    return klass;
}

static uint64_t syn_category(Syn *s, const char *name, uint64_t cls, uint64_t imethods, uint64_t cmethods,
                             uint64_t protocols, uint64_t props)
{
    uint64_t cat = syn_alloc(s, 56);
    syn_w(cat, 8, syn_str(s, name));
    syn_w(cat + 8, 8, cls);
    syn_w(cat + 16, 8, imethods);
    syn_w(cat + 24, 8, cmethods);
    syn_w(cat + 32, 8, protocols);
    syn_w(cat + 40, 8, props);
    return cat;
}

static void syn_section(Syn *s, const char *name, const uint64_t *words, int n)
{
    uint64_t addr = syn_alloc(s, 8 * (uint64_t)n);
    for (int i = 0; i < n; i++)
        syn_w(addr + 8 * (uint64_t)i, 8, words[i]);
    snprintf(s->sectname[s->nsect], sizeof s->sectname[0], "%s", name);
    s->sectaddr[s->nsect] = addr;
    s->sectsize[s->nsect] = 8 * (uint64_t)n;
    s->nsect++;
}

static const uint8_t *syn_header(Syn *s, const char *segname)
{
    uint8_t *img = ocerz_g2h(s->base);
    struct mach_header_64 h = { .magic = MH_MAGIC_64, .filetype = MH_DYLIB, .ncmds = 1 };
    struct segment_command_64 seg = { .cmd = LC_SEGMENT_64, .nsects = (uint32_t)s->nsect };
    seg.cmdsize = (uint32_t)(sizeof seg + (size_t)s->nsect * sizeof(struct section_64));
    snprintf(seg.segname, sizeof seg.segname, "%s", segname);
    h.sizeofcmds = seg.cmdsize;
    memcpy(img, &h, sizeof h);
    memcpy(img + sizeof h, &seg, sizeof seg);
    for (int i = 0; i < s->nsect; i++) {
        struct section_64 sc = { .addr = s->sectaddr[i], .size = s->sectsize[i] };
        memcpy(sc.sectname, s->sectname[i], strnlen(s->sectname[i], 16));
        memcpy(sc.segname, segname, strlen(segname));
        memcpy(img + sizeof h + sizeof seg + (size_t)i * sizeof sc, &sc, sizeof sc);
    }
    return img;
}

static void test_class_layouts(void)
{
    Syn s;
    syn_init(&s);

    uint64_t ro = syn_alloc(&s, 72);
    for (int i = 0; i < 72; i++)
        syn_w(ro + (uint64_t)i, 1, (uint64_t)(0x10 + i));
    OcerzObjcRo r;
    CHECK(ocerz_objc_read_ro(ro, &r) == OCERZ_OBJC_OK && r.flags == 0x13121110u && r.instance_start == 0x17161514u &&
              r.instance_size == 0x1b1a1918u && r.reserved == 0x1f1e1d1cu &&
              r.ivar_layout == 0x2726252423222120ull && r.name == 0x2f2e2d2c2b2a2928ull &&
              r.base_methods == 0x3736353433323130ull && r.base_protocols == 0x3f3e3d3c3b3a3938ull &&
              r.ivars == 0x4746454443424140ull && r.weak_ivar_layout == 0x4f4e4d4c4b4a4948ull &&
              r.base_properties == 0x5756555453525150ull,
          "class_ro_t is read field by field at its LP64 offsets");
    syn_w(ro, 4, 0x40);
    CHECK(ocerz_objc_read_ro(ro, &r) == OCERZ_OBJC_SWIFT,
          "a class_ro_t with a Swift metadata initializer is a Swift class");
    CHECK(ocerz_objc_read_ro(0, &r) == OCERZ_OBJC_NULL, "a null class_ro_t");

    uint64_t cl = syn_alloc(&s, 40);
    syn_w(cl, 8, 0x1111);
    syn_w(cl + 8, 8, 0x2222);
    syn_w(cl + 16, 8, 0x3333);
    syn_w(cl + 24, 8, 0x4444);
    syn_w(cl + 32, 8, ro);
    OcerzObjcClass c;
    CHECK(ocerz_objc_read_class(cl, &c) == OCERZ_OBJC_OK && c.isa == 0x1111 && c.superclass == 0x2222 &&
              c.cache == 0x3333 && c.vtable == 0x4444 && c.ro == ro && c.swift == 0,
          "class_t is isa, superclass, cache, vtable and bits");
    syn_w(cl + 32, 8, ro | 1);
    CHECK(ocerz_objc_read_class(cl, &c) == OCERZ_OBJC_SWIFT && c.ro == ro, "bits with the legacy Swift bit");
    syn_w(cl + 32, 8, ro | 2);
    CHECK(ocerz_objc_read_class(cl, &c) == OCERZ_OBJC_SWIFT && c.swift == 2, "bits with the stable Swift bit");
    syn_w(cl + 32, 8, 0);
    CHECK(ocerz_objc_read_class(cl, &c) == OCERZ_OBJC_NULL, "a class with no class_ro_t");

    uint64_t n0 = syn_str(&s, "alpha"), n1 = syn_str(&s, "beta:"), t0 = syn_str(&s, "c24@0:8@16"),
             t1 = syn_str(&s, "v16@0:8");
    uint64_t abs = syn_alloc(&s, 8 + 2 * 24);
    syn_w(abs, 4, 24 | 3);
    syn_w(abs + 4, 4, 2);
    syn_w(abs + 8, 8, n0);
    syn_w(abs + 16, 8, t0);
    syn_w(abs + 24, 8, 0xaaaa);
    syn_w(abs + 32, 8, n1);
    syn_w(abs + 40, 8, t1);
    syn_w(abs + 48, 8, 0xbbbb);
    OcerzObjcList l;
    OcerzObjcMethod m;
    CHECK(ocerz_objc_method_list(abs, &l) == OCERZ_OBJC_OK && l.count == 2 && l.entsize == 24 && l.flags == 3,
          "an absolute method list with its fixed-up bits: entsize %u count %u flags %#x", l.entsize, l.count, l.flags);
    CHECK(ocerz_objc_method_at(&l, 1, &m) == OCERZ_OBJC_OK && m.name == n1 && m.types == t1 && m.imp == 0xbbbb,
          "an absolute method entry is name, types and imp words");
    CHECK(ocerz_objc_method_at(&l, 2, &m) == OCERZ_OBJC_BAD_LIST, "an index past the count");
    syn_w(abs, 4, 16);
    CHECK(ocerz_objc_method_list(abs, &l) == OCERZ_OBJC_BAD_LIST, "an absolute list of 16-byte entries is refused");
    CHECK(ocerz_objc_method_list(0, &l) == OCERZ_OBJC_OK && l.count == 0, "no list is an empty list");

    uint64_t selref = syn_alloc(&s, 8);
    syn_w(selref, 8, n0);
    uint64_t imp_target = s.base + 0x40;
    uint64_t rel = syn_alloc(&s, 8 + 2 * 12);
    syn_w(rel, 4, 0x80000000u | 12);
    syn_w(rel + 4, 4, 2);
    syn_w(rel + 8, 4, (uint32_t)(int32_t)(selref - (rel + 8)));
    syn_w(rel + 12, 4, (uint32_t)(int32_t)(t0 - (rel + 12)));
    syn_w(rel + 16, 4, (uint32_t)(int32_t)(int64_t)(imp_target - (rel + 16)));
    syn_w(rel + 20, 4, (uint32_t)(int32_t)(selref - (rel + 20)));
    syn_w(rel + 24, 4, (uint32_t)(int32_t)(t1 - (rel + 24)));
    syn_w(rel + 28, 4, 0);
    CHECK(ocerz_objc_method_list(rel, &l) == OCERZ_OBJC_OK && l.entsize == 12 && l.count == 2,
          "a relative method list");
    CHECK(ocerz_objc_method_at(&l, 0, &m) == OCERZ_OBJC_OK && m.name == n0 && m.types == t0 && m.imp == imp_target,
          "a relative entry's name goes through its selector reference, its types and imp are offsets, and the imp"
          " offset is negative: name %#llx types %#llx imp %#llx", (unsigned long long)m.name,
          (unsigned long long)m.types, (unsigned long long)m.imp);
    CHECK(ocerz_objc_method_at(&l, 1, &m) == OCERZ_OBJC_OK && m.imp == 0, "a zero imp offset is no imp");
    syn_w(rel, 4, 0xc0000000u | 12);
    syn_w(rel + 8, 4, (uint32_t)(int32_t)(n1 - (rel + 8)));
    CHECK(ocerz_objc_method_list(rel, &l) == OCERZ_OBJC_OK && ocerz_objc_method_at(&l, 0, &m) == OCERZ_OBJC_OK &&
              m.name == n1, "a relative list with direct selectors names its string itself");
    syn_w(rel, 4, 0x80000000u | 24);
    CHECK(ocerz_objc_method_list(rel, &l) == OCERZ_OBJC_BAD_LIST, "a relative list of 24-byte entries is refused");

    uint64_t off = syn_alloc(&s, 8), iname = syn_str(&s, "_count"), itype = syn_str(&s, "i");
    uint64_t ivars = syn_alloc(&s, 8 + 32);
    syn_w(ivars, 4, 32);
    syn_w(ivars + 4, 4, 1);
    syn_w(ivars + 8, 8, off);
    syn_w(ivars + 16, 8, iname);
    syn_w(ivars + 24, 8, itype);
    syn_w(ivars + 32, 4, 2);
    syn_w(ivars + 36, 4, 4);
    OcerzObjcIvar iv;
    CHECK(ocerz_objc_ivar_list(ivars, &l) == OCERZ_OBJC_OK && ocerz_objc_ivar_at(&l, 0, &iv) == OCERZ_OBJC_OK &&
              iv.offset == off && iv.name == iname && iv.type == itype && iv.alignment == 2 && iv.size == 4,
          "an ivar entry is offset pointer, name, type, alignment and size");
    syn_w(ivars, 4, 24);
    CHECK(ocerz_objc_ivar_list(ivars, &l) == OCERZ_OBJC_BAD_LIST, "an ivar list of 24-byte entries is refused");

    static const char *const kProps[] = { "name", "T@\"NSString\",C,N,V_name" };
    uint64_t props = syn_props(&s, kProps, 1);
    OcerzObjcProperty pr;
    CHECK(ocerz_objc_property_list(props, &l) == OCERZ_OBJC_OK && ocerz_objc_property_at(&l, 0, &pr) == OCERZ_OBJC_OK &&
              strcmp(ocerz_g2h(pr.name), "name") == 0 && strcmp(ocerz_g2h(pr.attributes), kProps[1]) == 0,
          "a property entry is name and attributes");

    uint64_t refs[3] = { 0x10, 0x20, 0x30 };
    uint64_t plist = syn_refs(&s, refs, 3);
    CHECK(ocerz_objc_protocol_count(plist) == 3 && ocerz_objc_protocol_ref(plist, 2) == 0x30 &&
              ocerz_objc_protocol_count(0) == 0, "a protocol list is a 64-bit count and its references");

    uint64_t cat = syn_alloc(&s, 56);
    for (int i = 0; i < 7; i++)
        syn_w(cat + 8 * (uint64_t)i, 8, 0x100 + (uint64_t)i);
    OcerzObjcCategory ct;
    CHECK(ocerz_objc_read_category(cat, 1, &ct) == OCERZ_OBJC_OK && ct.name == 0x100 && ct.cls == 0x101 &&
              ct.instance_methods == 0x102 && ct.class_methods == 0x103 && ct.protocols == 0x104 &&
              ct.instance_properties == 0x105 && ct.class_properties == 0x106,
          "category_t with class properties");
    CHECK(ocerz_objc_read_category(cat, 0, &ct) == OCERZ_OBJC_OK && ct.class_properties == 0,
          "category_t from an image whose info says it has no class properties");

    uint64_t proto = syn_alloc(&s, 96);
    for (int i = 0; i < 12; i++)
        syn_w(proto + 8 * (uint64_t)i, 8, 0x200 + (uint64_t)i);
    syn_w(proto + 64, 4, 96);
    syn_w(proto + 68, 4, 5);
    OcerzObjcProtocol pt;
    CHECK(ocerz_objc_read_protocol(proto, &pt) == OCERZ_OBJC_OK && pt.name == 0x201 && pt.protocols == 0x202 &&
              pt.instance_methods == 0x203 && pt.class_methods == 0x204 && pt.optional_instance_methods == 0x205 &&
              pt.optional_class_methods == 0x206 && pt.instance_properties == 0x207 && pt.size == 96 &&
              pt.flags == 5 && pt.extended_types == 0x209 && pt.demangled_name == 0x20a &&
              pt.class_properties == 0x20b,
          "protocol_t of 96 bytes");
    syn_w(proto + 64, 4, 72);
    CHECK(ocerz_objc_read_protocol(proto, &pt) == OCERZ_OBJC_OK && pt.extended_types == 0 && pt.class_properties == 0,
          "protocol_t of 72 bytes has no fields past its size");
}

typedef struct GuestNotation {
    const char *types;
    int rc;
    const char *notation;
} GuestNotation;

static const GuestNotation kGuestNotations[] = {
    { "c24@0:8@16", OCERZ_OBJC_OK, "b(ppp)" },
    { "v20@0:8c16", OCERZ_OBJC_OK, "v(ppb)" },
    { "q16@0:8", OCERZ_OBJC_OK, "l(pp)" },
    { "Q16@0:8", OCERZ_OBJC_OK, "L(pp)" },
    { "v24@0:8q16", OCERZ_OBJC_OK, "v(ppl)" },
    { "@24@0:8^{_NSZone=}16", OCERZ_OBJC_OK, "p(ppp)" },
    { "c56@0:8q16{CGRect={CGPoint=dd}{CGSize=dd}}24", OCERZ_OBJC_OK, "b(ppl{{dd}{dd}})" },
    { "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", OCERZ_OBJC_OK, "v(pp{{dd}{dd}})" },
    { "{_NSRange=QQ}16@0:8", OCERZ_OBJC_OK, "{LL}(pp)" },
    { "#16@0:8", OCERZ_OBJC_OK, "p(pp)" },
    { "Vv16@0:8", OCERZ_OBJC_OK, "v(pp)" },
    { "v24@0:8@?16", OCERZ_OBJC_OK, "v(ppp)" },
    { "i20@0:8i16", OCERZ_OBJC_OK, "i(ppi)" },
    { "D16@0:8", OCERZ_OBJC_LONG_DOUBLE, NULL },
    { "v32@0:8D16", OCERZ_OBJC_LONG_DOUBLE, NULL },
    { "i8i0i4", OCERZ_OBJC_NOT_METHOD, NULL },
    { "v8@0", OCERZ_OBJC_NOT_METHOD, NULL },
    { "v24@0:8(?=iq)16", OCERZ_OBJC_UNION, NULL },
    { "", OCERZ_OBJC_MALFORMED, NULL },
};

static void test_guest_notation(void)
{
    for (size_t i = 0; i < sizeof kGuestNotations / sizeof kGuestNotations[0]; i++) {
        const GuestNotation *g = &kGuestNotations[i];
        char out[OCERZ_OBJC_NOTATION_MAX];
        int rc = ocerz_objc_method_notation(g->types, out, sizeof out);
        CHECK(rc == g->rc, "x86 types %s: %s, want %s", g->types, ocerz_objc_refusal(rc), ocerz_objc_refusal(g->rc));
        if (rc == OCERZ_OBJC_OK && g->notation)
            CHECK(strcmp(out, g->notation) == 0, "x86 types %s give %s, want %s", g->types, out, g->notation);
    }
    for (int i = OCERZ_OBJC_NOT_METHOD; i <= OCERZ_OBJC_CYCLE; i++)
        CHECK(ocerz_objc_refusal(i) && *ocerz_objc_refusal(i) &&
                  strcmp(ocerz_objc_refusal(i), ocerz_objc_refusal(99)) != 0,
              "class refusal %d has words of its own", i);

    OcerzObjcAttribute a[8];
    char storage[128];
    int n = ocerz_objc_property_attributes("T@\"NSString\",C,N,V_name", a, 8, storage, sizeof storage);
    CHECK(n == 4 && strcmp(a[0].name, "T") == 0 && strcmp(a[0].value, "@\"NSString\"") == 0 &&
              strcmp(a[1].name, "C") == 0 && strcmp(a[1].value, "") == 0 && strcmp(a[2].name, "N") == 0 &&
              strcmp(a[3].name, "V") == 0 && strcmp(a[3].value, "_name") == 0,
          "property attributes split at commas, one letter of name each, got %d", n);
    n = ocerz_objc_property_attributes("T{CGRect={CGPoint=dd}{CGSize=dd}},R", a, 8, storage, sizeof storage);
    CHECK(n == 2 && strcmp(a[0].value, "{CGRect={CGPoint=dd}{CGSize=dd}}") == 0 && strcmp(a[1].name, "R") == 0,
          "a structure type attribute");
    CHECK(ocerz_objc_property_attributes("", a, 8, storage, sizeof storage) == 0 &&
              ocerz_objc_property_attributes(NULL, a, 8, storage, sizeof storage) == 0,
          "no attributes");
    CHECK(ocerz_objc_property_attributes("Ti,N,R,C,&,W,D,P,G_g,S_s:", a, 4, storage, sizeof storage) == -1,
          "more attributes than the caller has room for");
    CHECK(ocerz_objc_property_attributes("T@\"AVeryLongClassNameThatDoesNotFit\"", a, 8, storage, 16) == -1,
          "attributes longer than the storage");
}

static void test_class_order(void)
{
    uint64_t classes[6] = { 0x60, 0x50, 0x40, 0x30, 0x20, 0x10 };
    uint64_t supers[6] = { 0x50, 0x40, 0x9000, 0x20, 0x10, 0x9000 };
    int order[6], culprit;
    CHECK(ocerz_objc_class_order(classes, supers, 6, order, &culprit) == OCERZ_OBJC_OK && culprit == -1,
          "two chains listed subclass first order");
    int pos[6];
    for (int i = 0; i < 6; i++)
        pos[order[i]] = i;
    int ok = 1, seen[6] = { 0 };
    for (int i = 0; i < 6; i++) {
        seen[order[i]]++;
        for (int j = 0; j < 6; j++)
            if (supers[i] == classes[j] && pos[j] > pos[i])
                ok = 0;
    }
    for (int i = 0; i < 6; i++)
        ok = ok && seen[i] == 1;
    CHECK(ok, "every class comes after its superclass and exactly once: %d %d %d %d %d %d", order[0], order[1],
          order[2], order[3], order[4], order[5]);
    CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0,
          "a chain whose root is native starts at the class nearest the root");

    uint64_t cyc[3] = { 0x10, 0x20, 0x30 }, cycs[3] = { 0x20, 0x30, 0x10 };
    CHECK(ocerz_objc_class_order(cyc, cycs, 3, order, &culprit) == OCERZ_OBJC_CYCLE && culprit >= 0 && culprit < 3,
          "a chain of superclasses that comes back to itself");
    uint64_t self_c[2] = { 0x10, 0x20 }, self_s[2] = { 0, 0x20 };
    CHECK(ocerz_objc_class_order(self_c, self_s, 2, order, &culprit) == OCERZ_OBJC_CYCLE && culprit == 1,
          "a class that is its own superclass");
    CHECK(ocerz_objc_class_order(NULL, NULL, 0, order, &culprit) == OCERZ_OBJC_OK, "no classes");
}

static uint64_t load_stub(Syn *s, uint64_t counter, uint64_t slot, uint64_t who)
{
    static const uint8_t kStub[] = {
        0x48, 0x8b, 0x05, 0, 0, 0, 0,
        0x48, 0xff, 0xc0,
        0x48, 0x89, 0x05, 0, 0, 0, 0,
        0x48, 0x89, 0x05, 0, 0, 0, 0,
        0x48, 0x89, 0x3d, 0, 0, 0, 0,
        0xc3,
    };
    uint64_t code = syn_alloc(s, sizeof kStub);
    uint8_t *p = ocerz_g2h(code);
    memcpy(p, kStub, sizeof kStub);
    int32_t d;
    d = (int32_t)(counter - (code + 7));
    memcpy(p + 3, &d, 4);
    d = (int32_t)(counter - (code + 17));
    memcpy(p + 13, &d, 4);
    d = (int32_t)(slot - (code + 24));
    memcpy(p + 20, &d, 4);
    d = (int32_t)(who - (code + 31));
    memcpy(p + 27, &d, 4);
    return code;
}

typedef struct DefinedImage {
    Syn s;
    uint64_t base, base_meta, sub, sub_meta;
    uint64_t off_count, off_obj;
    uint64_t guest_proto, guest_copying, protoref_proto, protoref_copying;
    uint64_t imp_count, imp_sub_count, imp_cat_count, imp_equal, imp_long_double, imp_make, imp_reverse;
    uint64_t counter, slot[3], who[3];
} DefinedImage;

static void build_defined_image(DefinedImage *d)
{
    Syn *s = &d->s;
    syn_init(s);
    uint64_t guest = s->base + 0x100;
    d->imp_count = guest + 0x10;
    d->imp_sub_count = guest + 0x20;
    d->imp_cat_count = guest + 0x30;
    d->imp_equal = guest + 0x40;
    d->imp_long_double = guest + 0x50;
    d->imp_make = guest + 0x60;
    d->imp_reverse = guest + 0x70;
    d->counter = syn_alloc(s, 8);
    for (int i = 0; i < 3; i++) {
        d->slot[i] = syn_alloc(s, 8);
        d->who[i] = syn_alloc(s, 8);
    }

    static const SynMethod kProtoMethods[] = { { "ocerzM11Count", "i16@0:8", 0 } };
    static const char *const kProtoProps[] = { "ocerzM11Count", "Ti,R" };
    uint64_t guest_nsobject = syn_protocol(s, "NSObject", 0, 0, 0, 96);
    uint64_t adopted[1] = { guest_nsobject };
    d->guest_proto = syn_protocol(s, "OcerzM11Proto", syn_refs(s, adopted, 1), syn_methods(s, kProtoMethods, 1),
                                  syn_props(s, kProtoProps, 1), 96);
    d->guest_copying = syn_protocol(s, "NSCopying", 0, 0, 0, 96);
    uint64_t class_protos[2] = { d->guest_proto, d->guest_copying };
    uint64_t protos = syn_refs(s, class_protos, 2);

    d->off_count = syn_alloc(s, 8);
    d->off_obj = syn_alloc(s, 8);
    syn_w(d->off_count, 8, 4);
    syn_w(d->off_obj, 8, 0xaaaaaaaa00000010ull);
    uint64_t ivars = syn_alloc(s, 8 + 2 * 32);
    syn_w(ivars, 4, 32);
    syn_w(ivars + 4, 4, 2);
    syn_w(ivars + 8, 8, d->off_count);
    syn_w(ivars + 16, 8, syn_str(s, "_count"));
    syn_w(ivars + 24, 8, syn_str(s, "i"));
    syn_w(ivars + 32, 4, 2);
    syn_w(ivars + 36, 4, 4);
    syn_w(ivars + 40, 8, d->off_obj);
    syn_w(ivars + 48, 8, syn_str(s, "_obj"));
    syn_w(ivars + 56, 8, syn_str(s, "@"));
    syn_w(ivars + 64, 4, 3);
    syn_w(ivars + 68, 4, 8);

    SynMethod base_i[] = {
        { "ocerzM11Count", "i16@0:8", d->imp_count },
        { "isEqual:", "c24@0:8@16", d->imp_equal },
        { "ocerzM11LongDouble", "D16@0:8", d->imp_long_double },
        { "ocerzM11Replaced", "i16@0:8", guest + 0x80 },
    };
    SynMethod base_c[] = {
        { "load", "v16@0:8", load_stub(s, d->counter, d->slot[0], d->who[0]) },
        { "ocerzM11Make", "@16@0:8", d->imp_make },
    };
    static const char *const kBaseProps[] = { "ocerzM11Count", "Ti,R,V_count" };
    SynMethod sub_i[] = { { "ocerzM11Count", "i16@0:8", d->imp_sub_count } };
    SynMethod sub_c[] = { { "load", "v16@0:8", load_stub(s, d->counter, d->slot[1], d->who[1]) } };

    void *nsobject = cls("NSObject");
    SynClass base = { "OcerzM11Base", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 4, 24,
                      syn_methods(s, base_i, 4), syn_methods(s, base_c, 2), ivars, protos,
                      syn_props(s, kBaseProps, 1), 0 };
    d->base = syn_class(s, &base, &d->base_meta);
    SynClass sub = { "OcerzM11Sub", d->base, d->base_meta, 0x80, 24, 24, syn_methods(s, sub_i, 1),
                     syn_methods(s, sub_c, 1), 0, 0, 0, 0 };
    d->sub = syn_class(s, &sub, &d->sub_meta);

    SynMethod str_i[] = { { "ocerzM11Reverse", "@16@0:8", d->imp_reverse } };
    SynMethod str_c[] = {
        { "ocerzM11StringCount", "Q16@0:8", guest + 0x90 },
        { "load", "v16@0:8", load_stub(s, d->counter, d->slot[2], d->who[2]) },
    };
    static const char *const kStrProps[] = { "ocerzM11Reversed", "T@\"NSString\",R" };
    uint64_t cat_protos[1] = { d->guest_proto };
    uint64_t strcat = syn_category(s, "OcerzM11", ocerz_h2g(cls("NSString")), syn_methods(s, str_i, 1),
                                   syn_methods(s, str_c, 2), syn_refs(s, cat_protos, 1), syn_props(s, kStrProps, 1));
    SynMethod basecat_i[] = { { "ocerzM11Replaced", "i16@0:8", d->imp_cat_count } };
    uint64_t basecat = syn_category(s, "OcerzM11Own", d->base, syn_methods(s, basecat_i, 1), 0, 0, 0);

    uint64_t classlist[2] = { d->sub, d->base };
    uint64_t nlclslist[1] = { d->sub };
    uint64_t catlist[2] = { strcat, basecat };
    uint64_t nlcatlist[1] = { strcat };
    uint64_t protolist[3] = { d->guest_proto, guest_nsobject, d->guest_copying };
    uint64_t protorefs[2] = { d->guest_proto, d->guest_copying };
    syn_section(s, "__objc_classlist", classlist, 2);
    syn_section(s, "__objc_nlclslist", nlclslist, 1);
    syn_section(s, "__objc_catlist", catlist, 2);
    syn_section(s, "__objc_nlcatlist", nlcatlist, 1);
    syn_section(s, "__objc_protolist", protolist, 3);
    syn_section(s, "__objc_protorefs", protorefs, 2);
    uint64_t info[1] = { 0x40ull << 32 };
    syn_section(s, "__objc_imageinfo", info, 1);
    d->protoref_proto = s->sectaddr[5];
    d->protoref_copying = s->sectaddr[5] + 8;
}

static void *imp_of(void *k, const char *selname)
{
    void *(*getm)(void *, void *) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
    void *(*getimp)(void *) = dlsym(RTLD_DEFAULT, "method_getImplementation");
    void *m = getm(k, sel(selname));
    return m ? getimp(m) : NULL;
}

static int slot_notation(void *imp, const char *want_ret_args, uint64_t want_fn)
{
    uint64_t fn = 0;
    const OcerzAbiSig *sig = ocerz_abi_callback_sig(imp, &fn);
    if (!sig || fn != want_fn)
        return 0;
    char got[OCERZ_ABI_MAX_ARGS + 2];
    got[0] = sig->ret;
    for (int i = 0; i < sig->nargs; i++)
        got[1 + i] = sig->arg[i];
    got[1 + sig->nargs] = '\0';
    return strcmp(got, want_ret_args) == 0;
}

static void test_define_image(void)
{
    DefinedImage d;
    build_defined_image(&d);
    const uint8_t *img = syn_header(&d.s, "__DATA_CONST");

    int n = ocerz_objcbridge_define_image(img, 0);
    CHECK(n == 4, "two classes and two categories defined, got %d", n);

    void *base = ocerz_g2h(d.base), *sub = ocerz_g2h(d.sub);
    void *(*superclass)(void *) = dlsym(RTLD_DEFAULT, "class_getSuperclass");
    size_t (*instance_size)(void *) = dlsym(RTLD_DEFAULT, "class_getInstanceSize");
    void *(*get_protocol)(const char *) = dlsym(RTLD_DEFAULT, "objc_getProtocol");
    bool (*conforms)(void *, void *) = dlsym(RTLD_DEFAULT, "class_conformsToProtocol");
    void *(*get_property)(void *, const char *) = dlsym(RTLD_DEFAULT, "class_getProperty");
    const char *(*property_attrs)(void *) = dlsym(RTLD_DEFAULT, "property_getAttributes");
    bool (*responds)(void *, void *) = dlsym(RTLD_DEFAULT, "class_respondsToSelector");

    CHECK(cls("OcerzM11Base") == base && cls("OcerzM11Sub") == sub,
          "the native runtime finds both classes by name at the guest's own addresses");
    CHECK(superclass(sub) == base && superclass(base) == cls("NSObject"),
          "the subclass, listed first, was defined after its superclass");
    CHECK(object_getClass_(base) == ocerz_g2h(d.base_meta) && object_getClass_(sub) == ocerz_g2h(d.sub_meta),
          "each class's metaclass is the guest's own");
    CHECK(ocerz_objcbridge_is_defined(d.base) && ocerz_objcbridge_is_defined(d.sub) &&
              !ocerz_objcbridge_is_defined(ocerz_h2g(cls("NSObject"))),
          "ocerz records the two guest classes and not the native one");
    CHECK(ocerz_ld(d.base + 16, 8) == ocerz_h2g(dlsym(RTLD_DEFAULT, "_objc_empty_cache")),
          "the cache word is the native empty cache");

    CHECK(ocerz_ld(d.off_count, 8) == 12 && ocerz_ld(d.off_obj, 8) == 0xaaaaaaaa00000018ull,
          "the runtime slid the ivars past NSObject's 8 bytes by an 8-aligned 8 and wrote the low 32 bits of each"
          " offset variable: %#llx %#llx", (unsigned long long)ocerz_ld(d.off_count, 8),
          (unsigned long long)ocerz_ld(d.off_obj, 8));
    CHECK(instance_size(base) == 32 && instance_size(sub) == 32, "instance sizes after the slide: %zu %zu",
          instance_size(base), instance_size(sub));

    void *count_imp = imp_of(base, "ocerzM11Count");
    void *sub_imp = imp_of(sub, "ocerzM11Count");
    CHECK(slot_notation(count_imp, "ipp", d.imp_count), "-[OcerzM11Base ocerzM11Count] is a slot bound to its guest"
          " IMP under i(pp)");
    CHECK(slot_notation(sub_imp, "ipp", d.imp_sub_count) && sub_imp != count_imp,
          "the override in the subclass is a slot of its own bound to its own IMP");
    CHECK(ocerz_abi_callback_sig(count_imp, NULL) == ocerz_abi_callback_sig(sub_imp, NULL),
          "two slots of one notation share one parsed signature");
    CHECK(slot_notation(imp_of(base, "isEqual:"), "bppp", d.imp_equal), "an x86 BOOL result is b");
    CHECK(imp_of(base, "ocerzM11LongDouble") == ocerz_objcbridge_dead_imp(),
          "a long double method is bound to the named refusal, not left out and not refused whole");
    CHECK(slot_notation(imp_of(object_getClass_(base), "ocerzM11Make"), "ppp", d.imp_make),
          "a class method is bound on the metaclass");
    CHECK(slot_notation(imp_of(base, "ocerzM11Replaced"), "ipp", d.imp_cat_count),
          "a category on a guest class replaces the class's own method");

    void *proto = get_protocol("OcerzM11Proto");
    void *copying = get_protocol("NSCopying");
    CHECK(proto && (uint64_t)(uintptr_t)proto != d.guest_proto, "the guest-only protocol is registered natively");
    CHECK(conforms(base, proto) && conforms(base, copying) && !conforms(sub, proto) &&
              ((bool (*)(void *, void *, void *))objc_msgSend_)(sub, sel("conformsToProtocol:"), proto),
          "the class conforms to the native protocols itself, and its subclass through it");
    CHECK(ocerz_ld(d.protoref_proto, 8) == ocerz_h2g(proto) && ocerz_ld(d.protoref_copying, 8) == ocerz_h2g(copying),
          "protocol references hold the native protocols");
    bool (*proto_conforms)(void *, void *) = dlsym(RTLD_DEFAULT, "protocol_conformsToProtocol");
    CHECK(proto_conforms(proto, get_protocol("NSObject")), "the registered protocol adopts the native NSObject");
    struct { void *name; const char *types; } (*describe)(void *, void *, bool, bool) =
        dlsym(RTLD_DEFAULT, "protocol_getMethodDescription");
    CHECK(describe(proto, sel("ocerzM11Count"), true, true).types &&
              strcmp(describe(proto, sel("ocerzM11Count"), true, true).types, "i16@0:8") == 0,
          "the registered protocol has its required method");
    void *(*proto_prop)(void *, const char *, bool, bool) = dlsym(RTLD_DEFAULT, "protocol_getProperty");
    CHECK(proto_prop(proto, "ocerzM11Count", true, true) != NULL, "and its property");
    void *bp = get_property(base, "ocerzM11Count");
    CHECK(bp && strcmp(property_attrs(bp), "Ti,R,V_count") == 0, "the class property list is the guest's own");

    void *nsstring = cls("NSString");
    CHECK(responds(nsstring, sel("ocerzM11Reverse")) &&
              slot_notation(imp_of(nsstring, "ocerzM11Reverse"), "ppp", d.imp_reverse),
          "a category on a native class adds its instance method as a slot");
    CHECK(slot_notation(imp_of(object_getClass_(nsstring), "ocerzM11StringCount"), "Lpp", d.s.base + 0x190),
          "and its class method");
    void *sp = get_property(nsstring, "ocerzM11Reversed");
    CHECK(sp && strcmp(property_attrs(sp), "T@\"NSString\",R") == 0, "and its property");
    CHECK(conforms(nsstring, proto), "and its protocol");

    CHECK(ocerz_objcbridge_define_image(img, 0) == 0 && cls("OcerzM11Base") == base,
          "defining the same image again defines nothing");

    int saved = vm.jit_enabled;
    vm.jit_enabled = 0;
    int ran = ocerz_objcbridge_run_loads(&vm, stack_top);
    vm.jit_enabled = saved;
    CHECK(ran == 3, "three +load methods ran, got %d", ran);
    CHECK(ocerz_ld(d.slot[0], 8) == 1 && ocerz_ld(d.slot[1], 8) == 2 && ocerz_ld(d.slot[2], 8) == 3,
          "+load order is the superclass, the class the non-lazy list names, then the category: %llu %llu %llu",
          (unsigned long long)ocerz_ld(d.slot[0], 8), (unsigned long long)ocerz_ld(d.slot[1], 8),
          (unsigned long long)ocerz_ld(d.slot[2], 8));
    CHECK(ocerz_ld(d.who[0], 8) == d.base && ocerz_ld(d.who[1], 8) == d.sub &&
              ocerz_ld(d.who[2], 8) == ocerz_h2g(nsstring),
          "each +load receives its class in rdi");
    CHECK(ocerz_objcbridge_run_loads(&vm, stack_top) == 0, "the queue is empty afterwards");
}

typedef struct ClassRefusal {
    const char *what;
    void (*build)(Syn *s);
    const char *message;
} ClassRefusal;

static void cr_root(Syn *s)
{
    SynClass c = { "OcerzM11Root", 0, 0, 0x82, 8, 8, 0, 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static void cr_unknown_super(Syn *s)
{
    void *nsobject = cls("NSObject");
    SynClass p = { "OcerzM11NotListed", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 8, 8,
                   0, 0, 0, 0, 0, 0 };
    uint64_t meta;
    uint64_t parent = syn_class(s, &p, &meta);
    SynClass c = { "OcerzM11Orphan", parent, meta, 0x80, 8, 8, 0, 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static void cr_null_super(Syn *s)
{
    SynClass c = { "OcerzM11WeakSuper", 0, 0, 0x80, 8, 8, 0, 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static void cr_swift(Syn *s)
{
    void *nsobject = cls("NSObject");
    SynClass c = { "OcerzM11Swift", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 8, 8,
                   0, 0, 0, 0, 0, 2 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static void cr_cycle(Syn *s)
{
    SynClass a = { "OcerzM11CycleA", 0, 0, 0x80, 8, 8, 0, 0, 0, 0, 0, 0 };
    SynClass b = { "OcerzM11CycleB", 0, 0, 0x80, 8, 8, 0, 0, 0, 0, 0, 0 };
    uint64_t am, bm;
    uint64_t ca = syn_class(s, &a, &am), cb = syn_class(s, &b, &bm);
    syn_w(ca + 8, 8, cb);
    syn_w(cb + 8, 8, ca);
    uint64_t list[2] = { ca, cb };
    syn_section(s, "__objc_classlist", list, 2);
}

static void cr_category(Syn *s)
{
    void *nsobject = cls("NSObject");
    SynClass p = { "OcerzM11NotDefined", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 8, 8,
                   0, 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_category(s, "Stray", syn_class(s, &p, NULL), 0, 0, 0, 0) };
    syn_section(s, "__objc_catlist", list, 1);
}

static void cr_call_dead(Syn *s)
{
    void *nsobject = cls("NSObject");
    SynMethod m[] = { { "ocerzM11Wide", "D16@0:8", s->base + 0x100 } };
    SynClass c = { "OcerzM11Dead", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 8, 8,
                   syn_methods(s, m, 1), 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static void cr_full_bank(Syn *s)
{
    for (uint64_t k = 0; k < OCERZ_ABI_CALLBACK_SLOTS; k++)
        if (!ocerz_abi_callback_intern(s->base + 0x1000 + k, "v(pp)"))
            break;
    void *nsobject = cls("NSObject");
    SynMethod m[] = { { "ocerzM11Wide", "v16@0:8", s->base + 0x100 } };
    SynClass c = { "OcerzM11Dead", ocerz_h2g(nsobject), ocerz_h2g(object_getClass_(nsobject)), 0x80, 8, 8,
                   syn_methods(s, m, 1), 0, 0, 0, 0, 0 };
    uint64_t list[1] = { syn_class(s, &c, NULL) };
    syn_section(s, "__objc_classlist", list, 1);
}

static const ClassRefusal kClassRefusals[] = {
    { "a guest root class", cr_root, "guest class OcerzM11Root is a root class" },
    { "a null superclass", cr_null_super, "guest class OcerzM11WeakSuper has a null superclass, as a class whose"
      " weak-linked superclass the host lacks does" },
    { "a superclass ocerz never defined", cr_unknown_super,
      "guest class OcerzM11Orphan has the superclass OcerzM11NotListed at" },
    { "a Swift class", cr_swift, "is a Swift class, and Swift classes do not cross" },
    { "a superclass cycle", cr_cycle, "is its own superclass, through a chain of superclasses" },
    { "a category on an undefined guest class", cr_category,
      "guest category Stray is on the class OcerzM11NotDefined at" },
    { "calling a method whose types do not cross", cr_call_dead,
      "-[OcerzM11Dead ocerzM11Wide] is guest code native code cannot call: its type encoding D16@0:8 has a long"
      " double" },
    { "calling a method the full bank had no slot for", cr_full_bank,
      "-[OcerzM11Dead ocerzM11Wide] is guest code native code cannot call: its type encoding v16@0:8 has no slot"
      " left in the callback bank" },
};

static int class_child(const ClassRefusal *r, char *err, size_t errlen, int *status)
{
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        Syn s;
        syn_init(&s);
        r->build(&s);
        ocerz_objcbridge_define_image(syn_header(&s, "__DATA"), 0);
        void *k = cls("OcerzM11Dead");
        if (k) {
            void *obj = ((void *(*)(void *, void *))objc_msgSend_)(k, sel("new"));
            ((void (*)(void *, void *))objc_msgSend_)(obj, sel("ocerzM11Wide"));
        }
        _exit(0);
    }
    close(fds[1]);
    size_t have = 0;
    ssize_t rd;
    while (have + 1 < errlen && (rd = read(fds[0], err + have, errlen - 1 - have)) > 0)
        have += (size_t)rd;
    err[have] = '\0';
    close(fds[0]);
    return pid > 0 && waitpid(pid, status, 0) == pid;
}

static void test_class_refusals(void)
{
    for (size_t i = 0; i < sizeof kClassRefusals / sizeof kClassRefusals[0]; i++) {
        const ClassRefusal *r = &kClassRefusals[i];
        char err[4096];
        int status = 0;
        CHECK(class_child(r, err, sizeof err, &status), "%s: child ran", r->what);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 72, "%s: exits 72, status %#x: %s", r->what, status, err);
        CHECK(strstr(err, "ocerz: bridge: ") && strstr(err, r->message), "%s: stderr names it: %s", r->what, err);
    }
}

static int report(void)
{
    if (failures) {
        fprintf(stderr, "test_objcbridge: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_objcbridge: %d checks passed\n", checks);
    return 0;
}

int main(void)
{
    write_db();
    if (ocerz_mem_init_identity(ARENA) != OCERZ_OK) {
        fprintf(stderr, "identity mem init failed\n");
        return 2;
    }
    if (ocerz_vm_init(&vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }
    scratch = ocerz_map_anywhere(SCRATCH, PROT_READ | PROT_WRITE);
    if (!scratch) {
        fprintf(stderr, "scratch alloc failed\n");
        return 2;
    }
    stack_top = scratch + SCRATCH;

    void *objc = dlopen(OCERZ_OBJC_LIBOBJC, RTLD_LAZY);
    void *found = dlopen(OCERZ_OBJC_FOUNDATION, RTLD_LAZY);
    void *cf = dlopen(OCERZ_BRIDGE_COREFOUNDATION, RTLD_LAZY);
    if (!objc || !found || !cf) {
        fprintf(stderr, "cannot open libobjc, Foundation or CoreFoundation\n");
        return 2;
    }
    objc_getClass_ = need(objc, "objc_getClass");
    object_getClass_ = need(objc, "object_getClass");
    sel_registerName_ = need(objc, "sel_registerName");
    class_copyMethodList_ = need(objc, "class_copyMethodList");
    method_getTypeEncoding_ = need(objc, "method_getTypeEncoding");
    method_getName_ = need(objc, "method_getName");
    sel_getName_ = need(objc, "sel_getName");
    method_getNumberOfArguments_ = need(objc, "method_getNumberOfArguments");
    method_copyReturnType_ = need(objc, "method_copyReturnType");
    method_copyArgumentType_ = need(objc, "method_copyArgumentType");
    class_getName_ = need(objc, "class_getName");
    objc_copyClassNamesForImage_ = need(objc, "objc_copyClassNamesForImage");
    objc_msgSend_ = need(objc, "objc_msgSend");
    NSGetSizeAndAlignment_ = need(found, "NSGetSizeAndAlignment");
    CFStringGetLength_ = need(cf, "CFStringGetLength");
    CFStringGetCString_ = need(cf, "CFStringGetCString");

    void *pool = ((void *(*)(void))need(objc, "objc_autoreleasePoolPush"))();

    test_encoding_table();
    test_formats();
    test_variadic_table();
    test_va_cursor();
    test_selrefs();
    test_nil();
    test_sends();
    test_refusals();
    test_callables_allowed();
    test_forwarding_target();
    test_class_layouts();
    test_guest_notation();
    test_class_order();
    test_define_image();
    test_class_refusals();
    test_encoding_sweep();

    ((void (*)(void *))need(objc, "objc_autoreleasePoolPop"))(pool);
    remove_db();
    return report();
}
