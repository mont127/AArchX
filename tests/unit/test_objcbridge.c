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
 * child, whose status must be 72 and whose stderr must name the reason.
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
    test_encoding_sweep();

    ((void (*)(void *))need(objc, "objc_autoreleasePoolPop"))(pool);
    remove_db();
    return report();
}
