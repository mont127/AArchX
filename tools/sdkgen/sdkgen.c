/*
 * sdkgen: writing a native-mode API database out of the macOS SDK.
 *
 * include/ocerz/apidb.h describes the file this writes and why it exists.  This
 * is the program that decides, for every export of one library, which of its
 * records the export gets, and it is written to be wrong only in the safe
 * direction: an export that cannot be shown to cross correctly becomes a stub
 * that names itself, or is left out, and every such decision is counted under
 * a reason so the counts can be watched as the rules and the SDK change.
 *
 * ---- the denominator ----
 * What the library exports comes from its .tbd, never from the headers.  Every
 * symbol, weak symbol and thread-local symbol listed for x86_64-macos counts,
 * together with those of each re-exported library that names a parent
 * umbrella; $ld$ pseudo-symbols are linker instructions, not exports, and are
 * skipped and counted.  The Objective-C classes, exception types and ivars a
 * .tbd lists apart from its symbols are exports as well, under the names the
 * linker gives them: _OBJC_CLASS_$_<class> and _OBJC_METACLASS_$_<class> for
 * a class, _OBJC_EHTYPE_$_<class> for an exception type and
 * _OBJC_IVAR_$_<class>.<ivar> for an ivar.  So the total in the coverage report
 * is the size of the real x86_64 library's export list, and an export no
 * header describes is still in it.
 *
 * ---- finding the declaration ----
 * Each pass in tools/sdkgen/libraries is an umbrella header parsed twice by
 * libclang, once for x86_64-apple-macos<version> and once for arm64, with the
 * SDK as sysroot and any defines the pass names, as C or, for a library the
 * configuration marks objective-c, as Objective-C, and an export takes its
 * declaration from the first pass that has one.  An export is matched by the
 * x86_64 parse's mangled name, which is the linker's name for it after every
 * asm label, so _opendir$INODE64 finds opendir and plain _opendir, the old
 * 32-bit-inode entry point, finds nothing.  The same declaration in the arm64
 * parse of that pass is the one with the same USR, and its mangled name
 * without the leading underscore is the host symbol, which is how an x86_64
 * $INODE64 export crosses to the arm64 function that has only ever had 64-bit
 * inodes; a function declared for x86_64 alone is stub no-arm64-declaration.
 * Every host symbol is looked up with dlsym in the real library on the machine
 * running the generator, and one that is missing is counted but still
 * written, since a database describes the SDK, the SDK can be newer than the
 * machine, and ocerz refuses a missing host symbol itself.  Cursor and type
 * kind numbers are read from libclang by spelling at startup, never compiled
 * in.
 *
 * ---- what becomes a fn ----
 * A function crosses when its result and every parameter map to a class of
 * the notation in include/ocerz/abi.h, the x86_64 and arm64 declarations give
 * the same notation, and no pointer in the signature leads somewhere the two
 * architectures disagree about.  Integers take their class from the canonical
 * type's size and signedness, an enum from its underlying integer, and any
 * data pointer is p, as are an Objective-C object pointer, id, Class and SEL.
 * So is IMP, found by its typedef: a method implementation is a function
 * pointer, but one a program passes around, stores and compares, and making it
 * a callback would hand native code a trampoline in place of the address the
 * runtime knows.  A pointer to any other function becomes c with the pointee's
 * own notation, which may not itself contain a callback and must fit the 47
 * characters the engine keeps for one.  A union declared transparent_union is
 * passed as its first member by both ABIs, so dispatch_object_t is p, and every
 * member is walked as a pointer below.
 *
 * A structure passed or returned by value becomes braces around its members'
 * classes, flattened: a nested structure is braces inside braces, and an array
 * is its elements one after another, scalars or structures alike.  The engine
 * computes the layout of such a notation itself, the natural C one abi.h
 * describes, so the notation is only written when that layout is the real
 * one.  At every level of nesting the offset of each field, and the size and
 * alignment of the structure, are computed the way the engine will and compared
 * with clang's, and any difference is struct-layout, which is what a packed
 * structure or an explicitly aligned field becomes.  The two architectures then
 * have to agree as they do for any notation.  A structure that cannot be
 * written at all is refused under the first reason met while walking it:
 * union-value for a union, by value or as a member; struct-bitfield;
 * struct-flexible-array for an incomplete, variable-length or zero-length
 * array; struct-too-many-members past sixteen flattened members;
 * struct-too-deep past eight levels; struct-empty; struct-incomplete for a
 * record clang cannot size; struct-callback for a function pointer member,
 * since a callback cannot be carried inside braces; and a member's own reason,
 * such as long-double or block, for a member no class describes.  A structure
 * inside a callback's notation follows the same rules and counts toward its
 * length.
 *
 * The other refusals, each a stub reason: variadic; long-double, which is 80
 * bits on one side and 64 on the other; va-list, spotted as a pointer to
 * x86_64's __va_list_tag; block; too-many-args past sixteen; nested-callback;
 * callback-too-long; callback-result for a function pointer handed back to the
 * guest, which would be arm64 code; callback-pointer for a pointer to a
 * function pointer; no-prototype; complex, vector, int128, float-width, atomic
 * and unexposed-type for the rarer kinds; and arch-mismatch when the two
 * declarations of one function give different notations.  Two notations that
 * differ only between i and u, or l and L, are not a mismatch: boolean_t is
 * unsigned int on x86_64 and int on arm64, and at 32 and 64 bits neither
 * callee reads more than the value's own width.  Nor is b on x86_64 where
 * arm64 has B, which is Objective-C's BOOL, a signed char on x86_64 and a bool
 * on arm64: its values are 0 and 1, which extend alike either way.  In both
 * cases the x86_64 notation is written and the difference is printed as a
 * note.
 *
 * Pointers are then walked on both architectures in step.  A pointer whose
 * target has a different layout on the two - size, alignment, any field's
 * offset, size or bit width, compared through embedded records, arrays and the
 * targets of pointer fields - is layout; an incomplete record is opaque and
 * fine, and two scalars of different kinds but one size and alignment, such as
 * unsigned long and unsigned long long, are the same bytes.  A pointer to a
 * record holding a function pointer or a block in its own storage is
 * callback-struct, because native code would call guest code through it, unless
 * the overrides give that argument a struct record.  The same checks run on a
 * pointer result, on the parameters of a callback, on every pointer member of a
 * structure passed or returned by value, and on a parameter declared as an
 * array, which is a pointer to its first element however the header spells
 * it.  The walk through pointer fields can meet a cycle, so a record found
 * again while its own comparison is under way is assumed equal for that moment,
 * and a record whose answer depended on such an assumption about a record
 * further up is not remembered: "different" never rests on an assumption,
 * "equal" is cached only once its whole cycle is settled.
 *
 * ---- variables and the undeclared ----
 * A variable becomes data when its type has the same layout on both
 * architectures, and is omitted under layout, thread-local or
 * no-arm64-declaration otherwise.  A writable variable that holds a function
 * pointer still becomes data, and is printed as a note, because a guest that
 * stores its own function there hands native code x86 to call.  An export with
 * no declaration is looked up on the host: a symbol inside a section marked as
 * holding instructions is a function nobody declared and becomes stub
 * no-declaration, one in any other section becomes data bound to the native
 * variable of the same name, and one the host lacks is omitted as
 * no-declaration-missing.  Binding a variable's name to a stub would hand the
 * guest code where it expects data; binding it to the native variable hands it
 * the variable itself, which is what the compiler-emitted references need:
 * __kCFBooleanTrue behind @YES, __NSArray0__struct behind an empty @[] literal,
 * and the calendar identifiers.  No header describes its layout, so nothing
 * checks that the two architectures agree on it; every such variable seen in
 * practice is an object or a pointer.  Swift's mangled symbols are the
 * exception and are omitted as swift: they are type metadata and witness tables
 * for the frameworks' Swift overlays, and an Intel Swift program bound to arm64
 * metadata would fail somewhere deep inside instead of refusing to load.  An Objective-C class, metaclass or exception type becomes data
 * bound to the same name in the host library, checked with dlsym like any other
 * host symbol, so a guest's class references and superclass pointers reach the
 * native classes.  An ivar is omitted as objc-ivar: its export is the offset a
 * guest subclass compiles its field accesses against, which belongs with the
 * guest classes a later milestone builds.
 *
 * ---- overrides ----
 * tools/sdkgen/overrides holds records in the database's own syntax that
 * replace what the rules produce: special, var, data, stub and fn for an
 * export, shape and struct for callback structures.  Two more kinds exist only
 * there.  omit leaves an export out of the file, so a guest importing it fails
 * to bind by name.  opaque names a record the guest never builds itself, which
 * callback-struct then ignores as a pointer target while layout still applies,
 * so a record the guest reads through inline macros must still agree field by
 * field.  A stub override names an export exactly, and then also covers its
 * $-suffixed variants, or with a fnmatch pattern, which only reaches exports
 * the rules made fn or stub; an exact name wins over a variant and a variant
 * over a pattern.  An override scoped to a library that matches nothing there
 * is an error, as is an opaque record no pointer ever reaches, a struct record
 * whose export does not come out fn, a struct record whose argument is not a
 * pointer to the named structure, and a shape whose words disagree with the
 * structure the header declares.
 *
 * ---- output ----
 * The database goes to <out>/macos/<version>/<leaf>.api, records sorted by
 * export name, then shapes, then struct records, then the omitted exports as
 * comments, with nothing in it that varies between runs.  The coverage counts
 * go to <build>/<leaf>.coverage and to standard output, and every record type
 * whose layout was measured goes to <build>/<leaf>.layouts, with the header,
 * defines and language it was parsed with, for tools/sdkgen/layout_check.sh to
 * hold against clang's own record dumps.
 */
#include "clang_api.h"
#include "tbd.h"

#include <dlfcn.h>
#include <fnmatch.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCH_X86 0
#define ARCH_ARM 1
#define SIG_MAX_ARGS 16
#define SIG_CB_MAX 48
#define SIG_STRUCT_MEMBERS 16
#define SIG_STRUCT_DEPTH 8
#define MAX_PASSES 8
#define MAX_DEFINES 8
#define MAX_FIELDS 4096

static const char *g_arch_name[2] = { "x86_64", "arm64" };

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("sdkgen: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xalloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *d = xalloc(NULL, n + 1);
    memcpy(d, s, n + 1);
    return d;
}

static char *xprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xalloc(NULL, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

typedef struct Buf {
    char *s;
    size_t n;
    size_t cap;
} Buf;

static void buf_add(Buf *b, const char *s)
{
    size_t k = strlen(s);
    if (b->n + k + 1 > b->cap) {
        b->cap = (b->n + k + 1) * 2;
        b->s = xalloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, k + 1);
    b->n += k;
}

static void buf_addc(Buf *b, char c)
{
    char t[2] = { c, 0 };
    buf_add(b, t);
}

static const char *buf_str(Buf *b)
{
    if (!b->s)
        buf_add(b, "");
    return b->s;
}

static char *cx(CXString s)
{
    const char *c = clang_getCString(s);
    char *d = xstrdup(c ? c : "");
    clang_disposeString(s);
    return d;
}

typedef struct Map {
    char **key;
    int *val;
    size_t cap;
    size_t n;
} Map;

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 1099511628211ull;
    return h;
}

static int map_get(const Map *m, const char *k)
{
    if (!m->cap)
        return -1;
    size_t i = (size_t)hash_str(k) & (m->cap - 1);
    while (m->key[i]) {
        if (strcmp(m->key[i], k) == 0)
            return m->val[i];
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

static void map_set(Map *m, const char *k, int v);

static void map_grow(Map *m)
{
    Map n = { 0 };
    n.cap = m->cap ? m->cap * 2 : 1024;
    n.key = calloc(n.cap, sizeof *n.key);
    n.val = calloc(n.cap, sizeof *n.val);
    if (!n.key || !n.val)
        die("out of memory");
    for (size_t i = 0; i < m->cap; i++)
        if (m->key[i])
            map_set(&n, m->key[i], m->val[i]);
    free(m->key);
    free(m->val);
    *m = n;
}

static void map_set(Map *m, const char *k, int v)
{
    if ((m->n + 1) * 2 > m->cap)
        map_grow(m);
    size_t i = (size_t)hash_str(k) & (m->cap - 1);
    while (m->key[i]) {
        if (strcmp(m->key[i], k) == 0) {
            m->val[i] = v;
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->key[i] = xstrdup(k);
    m->val[i] = v;
    m->n++;
}

static struct {
    int FunctionDecl, VarDecl, TypedefDecl, LinkageSpec, UnexposedDecl, UnionDecl;
} CK;

static struct {
    int Invalid, Unexposed, Void, Bool, Char_U, UChar, Char16, Char32, UShort, UInt, ULong,
        ULongLong, UInt128, Char_S, SChar, WChar, Short, Int, Long, LongLong, Int128, Float,
        Double, LongDouble, Float128, Half, Float16, Complex, Pointer, BlockPointer, Record,
        Enum, Typedef, ObjCObjectPointer, ObjCId, ObjCClass, ObjCSel, FunctionNoProto,
        FunctionProto, ConstantArray, Vector, IncompleteArray, VariableArray, Elaborated,
        Attributed, Atomic;
} TK;

static void resolve_kinds(void)
{
    struct { const char *name; int *slot; } cursors[] = {
        { "FunctionDecl", &CK.FunctionDecl }, { "VarDecl", &CK.VarDecl },
        { "TypedefDecl", &CK.TypedefDecl }, { "LinkageSpec", &CK.LinkageSpec },
        { "UnexposedDecl", &CK.UnexposedDecl }, { "UnionDecl", &CK.UnionDecl },
    };
    struct { const char *name; int *slot; } types[] = {
        { "Invalid", &TK.Invalid }, { "Unexposed", &TK.Unexposed }, { "Void", &TK.Void },
        { "Bool", &TK.Bool }, { "Char_U", &TK.Char_U }, { "UChar", &TK.UChar },
        { "Char16", &TK.Char16 }, { "Char32", &TK.Char32 }, { "UShort", &TK.UShort },
        { "UInt", &TK.UInt }, { "ULong", &TK.ULong }, { "ULongLong", &TK.ULongLong },
        { "UInt128", &TK.UInt128 }, { "Char_S", &TK.Char_S }, { "SChar", &TK.SChar },
        { "WChar", &TK.WChar }, { "Short", &TK.Short }, { "Int", &TK.Int }, { "Long", &TK.Long },
        { "LongLong", &TK.LongLong }, { "Int128", &TK.Int128 }, { "Float", &TK.Float },
        { "Double", &TK.Double }, { "LongDouble", &TK.LongDouble }, { "Float128", &TK.Float128 },
        { "Half", &TK.Half }, { "Float16", &TK.Float16 }, { "Complex", &TK.Complex },
        { "Pointer", &TK.Pointer }, { "BlockPointer", &TK.BlockPointer }, { "Record", &TK.Record },
        { "Enum", &TK.Enum }, { "Typedef", &TK.Typedef },
        { "ObjCObjectPointer", &TK.ObjCObjectPointer }, { "ObjCId", &TK.ObjCId },
        { "ObjCClass", &TK.ObjCClass }, { "ObjCSel", &TK.ObjCSel },
        { "FunctionNoProto", &TK.FunctionNoProto }, { "FunctionProto", &TK.FunctionProto },
        { "ConstantArray", &TK.ConstantArray }, { "Vector", &TK.Vector },
        { "IncompleteArray", &TK.IncompleteArray }, { "VariableArray", &TK.VariableArray },
        { "Elaborated", &TK.Elaborated }, { "Attributed", &TK.Attributed }, { "Atomic", &TK.Atomic },
    };
    size_t nc = sizeof cursors / sizeof cursors[0];
    size_t nt = sizeof types / sizeof types[0];
    for (size_t i = 0; i < nc; i++)
        *cursors[i].slot = -1;
    for (size_t i = 0; i < nt; i++)
        *types[i].slot = -1;
    for (int k = 0; k < 4096; k++) {
        if (!clang_isDeclaration(k))
            continue;
        CXString s = clang_getCursorKindSpelling(k);
        const char *c = clang_getCString(s);
        for (size_t i = 0; c && i < nc; i++)
            if (*cursors[i].slot < 0 && strcmp(c, cursors[i].name) == 0)
                *cursors[i].slot = k;
        clang_disposeString(s);
    }
    for (int k = 0; k < 4096; k++) {
        CXString s = clang_getTypeKindSpelling(k);
        const char *c = clang_getCString(s);
        for (size_t i = 0; c && i < nt; i++)
            if (*types[i].slot < 0 && strcmp(c, types[i].name) == 0)
                *types[i].slot = k;
        clang_disposeString(s);
    }
    for (size_t i = 0; i < nc; i++)
        if (*cursors[i].slot < 0)
            die("libclang has no cursor kind spelled %s", cursors[i].name);
    for (size_t i = 0; i < nt; i++)
        if (*types[i].slot < 0)
            die("libclang has no type kind spelled %s", types[i].name);
}

typedef struct Pass {
    char *header;
    char *defines[MAX_DEFINES];
    int ndefines;
    CXTranslationUnit tu[2];
    CXCursor *decl[2];
    int ndecl[2];
    int capdecl[2];
    Map mangled;
    Map usr;
    CXCursor *tdef;
    int ntdef;
    int captdef;
    Map tdefs;
} Pass;

typedef struct LibConfig {
    char *name;
    char *install;
    char *tbd;
    char *language;
    Pass passes[MAX_PASSES];
    int npasses;
} LibConfig;

struct Override {
    int line;
    char *scope;
    char *kind;
    char **f;
    int nf;
    int glob;
    int used;
};

typedef enum RKind { R_NONE, R_FN, R_DATA, R_VAR, R_SPECIAL, R_STUB, R_OMIT } RKind;

typedef struct Rec {
    char *name;
    RKind kind;
    char *host;
    char *sig;
    const char *reason;
    char *handler;
    char *filler;
    char *bytes;
    int override;
    int objc;
    int functionish;
    int host_missing;
    char *note;
} Rec;

typedef struct Meas {
    char *name;
    char *key[2];
    long long size[2];
    long long align[2];
    Buf offs[2];
} Meas;

typedef struct Override Override;

static const char *g_sdk;
static const char *g_version;
static const char *g_tooldir;
static LibConfig g_lib;
static Override *g_ovr;
static int g_novr;
static Map g_lay_memo;
static int *g_lay_state;
static int *g_lay_depth_of;
static int g_lay_cap;
static int g_lay_depth;
static int g_lay_low = INT_MAX;
static Map g_fnf_memo;
static int *g_fnf_val;
static int g_fnf_cap;
static Map g_meas_map;
static Meas *g_meas;
static int g_nmeas;
static int g_cap_meas;

static int is_fn_kind(int k)
{
    return k == TK.FunctionProto || k == TK.FunctionNoProto;
}

static CXType canon(CXType t)
{
    return clang_getCanonicalType(t);
}

static CXType desugar_step(CXType t, int *ok)
{
    *ok = 1;
    if (t.kind == TK.Elaborated)
        return clang_Type_getNamedType(t);
    if (t.kind == TK.Attributed)
        return clang_Type_getModifiedType(t);
    if (t.kind == TK.Typedef)
        return clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(t));
    *ok = 0;
    return t;
}

static CXType desugar_to(CXType t, int kind_a, int kind_b)
{
    for (int guard = 0; guard < 64; guard++) {
        if (t.kind == kind_a || t.kind == kind_b)
            return t;
        int ok;
        CXType n = desugar_step(t, &ok);
        if (!ok)
            break;
        t = n;
    }
    return canon(t);
}

static int typedef_chain_has(CXType t, const char *name)
{
    for (int guard = 0; guard < 64; guard++) {
        if (t.kind == TK.Typedef) {
            char *n = cx(clang_getTypedefName(t));
            int hit = strcmp(n, name) == 0;
            free(n);
            if (hit)
                return 1;
        }
        int ok;
        t = desugar_step(t, &ok);
        if (!ok)
            return 0;
    }
    return 0;
}

static char *innermost_typedef(CXType t)
{
    char *name = NULL;
    for (int guard = 0; guard < 64; guard++) {
        if (t.kind == TK.Typedef) {
            free(name);
            name = cx(clang_getTypedefName(t));
        }
        int ok;
        t = desugar_step(t, &ok);
        if (!ok)
            break;
    }
    return name;
}

static char *record_key(CXType c)
{
    return cx(clang_getTypeSpelling(clang_getUnqualifiedType(c)));
}

static int is_va_list_tag(CXType c)
{
    if (c.kind != TK.Record)
        return 0;
    char *s = record_key(c);
    int hit = strcmp(s, "struct __va_list_tag") == 0;
    free(s);
    return hit;
}

static int is_integer_kind(int k, int *sign)
{
    if (k == TK.Char_S || k == TK.SChar || k == TK.Short || k == TK.Int || k == TK.Long ||
        k == TK.LongLong || k == TK.WChar) {
        *sign = 1;
        return 1;
    }
    if (k == TK.Char_U || k == TK.UChar || k == TK.UShort || k == TK.UInt || k == TK.ULong ||
        k == TK.ULongLong || k == TK.Char16 || k == TK.Char32) {
        *sign = 0;
        return 1;
    }
    return 0;
}

static const char *fn_notation(CXType fnc, int depth, Buf *out);

static Map g_tu_memo;
static int *g_tu_val;
static int g_tu_cap;

static int transparent_union(CXType c)
{
    CXCursor decl = clang_getTypeDeclaration(c);
    if (decl.kind != CK.UnionDecl || clang_Type_getSizeOf(c) < 0)
        return 0;
    char *key = record_key(c);
    int idx = map_get(&g_tu_memo, key);
    if (idx >= 0) {
        free(key);
        return g_tu_val[idx];
    }
    void *policy = clang_getCursorPrintingPolicy(decl);
    char *text = cx(clang_getCursorPrettyPrinted(decl, policy));
    clang_PrintingPolicy_dispose(policy);
    char *brace = strchr(text, '{');
    if (brace)
        *brace = '\0';
    int hit = strstr(text, "transparent_union") != NULL;
    free(text);
    if ((int)g_tu_memo.n >= g_tu_cap) {
        g_tu_cap = g_tu_cap ? g_tu_cap * 2 : 64;
        g_tu_val = xalloc(g_tu_val, (size_t)g_tu_cap * sizeof *g_tu_val);
    }
    idx = (int)g_tu_memo.n;
    map_set(&g_tu_memo, key, idx);
    g_tu_val[idx] = hit;
    free(key);
    return hit;
}

static int first_field_visit(CXCursor c, CXClientData d)
{
    *(CXCursor *)d = c;
    return CXVisit_Break;
}

static CXType first_field_type(CXType rec)
{
    CXCursor f = clang_getNullCursor();
    clang_Type_visitFields(rec, first_field_visit, &f);
    return clang_getCursorType(f);
}

typedef struct FieldList {
    CXCursor *v;
    int n;
} FieldList;

static int field_visit(CXCursor c, CXClientData d)
{
    FieldList *fl = d;
    if (fl->n < MAX_FIELDS)
        fl->v[fl->n++] = c;
    return CXVisit_Continue;
}

static void fields_of(CXType rec, FieldList *fl)
{
    fl->v = xalloc(NULL, MAX_FIELDS * sizeof *fl->v);
    fl->n = 0;
    clang_Type_visitFields(rec, field_visit, fl);
    if (fl->n >= MAX_FIELDS)
        die("a record has more than %d fields", MAX_FIELDS);
}

typedef struct Flat {
    Buf *out;
    int members;
    int layout_bad;
} Flat;

static long long align_up(long long n, long long align)
{
    return (n + align - 1) / align * align;
}

static const char *type_class(CXType t, int depth, int is_result, Buf *out);

static const char *flat_record(Flat *f, CXType rec, int depth, int level, long long *size,
                               long long *align);

static int is_imp(CXType t)
{
    return typedef_chain_has(t, "IMP");
}

static const char *flat_type(Flat *f, CXType t, int depth, int level, long long *size,
                             long long *align)
{
    CXType c = canon(t);
    if (c.kind == TK.Record) {
        if (is_va_list_tag(c))
            return "va-list";
        return flat_record(f, c, depth, level + 1, size, align);
    }
    if (c.kind == TK.ConstantArray) {
        long long n = clang_getArraySize(c);
        if (n <= 0)
            return "struct-flexible-array";
        Buf one = { 0 };
        Flat g = { &one, 0, 0 };
        long long esize = 0, ealign = 1;
        const char *r = flat_type(&g, clang_getArrayElementType(c), depth, level, &esize, &ealign);
        if (!r && (n > SIG_STRUCT_MEMBERS || f->members + g.members * n > SIG_STRUCT_MEMBERS))
            r = "struct-too-many-members";
        if (!r) {
            for (long long i = 0; i < n; i++)
                buf_add(f->out, buf_str(&one));
            f->members += (int)(g.members * n);
            f->layout_bad |= g.layout_bad;
            *size = esize * n;
            *align = ealign;
        }
        free(one.s);
        return r;
    }
    if (c.kind == TK.IncompleteArray || c.kind == TK.VariableArray)
        return "struct-flexible-array";
    if (c.kind == TK.Pointer && is_fn_kind(canon(clang_getPointeeType(c)).kind) && !is_imp(t))
        return "struct-callback";
    Buf one = { 0 };
    const char *r = type_class(t, depth, 0, &one);
    if (!r) {
        if (++f->members > SIG_STRUCT_MEMBERS) {
            r = "struct-too-many-members";
        } else {
            char k = buf_str(&one)[0];
            *size = strchr("bB", k) ? 1 : strchr("hH", k) ? 2 : strchr("iuf", k) ? 4 : 8;
            *align = *size;
            if (clang_Type_getSizeOf(c) != *size)
                f->layout_bad = 1;
            buf_add(f->out, buf_str(&one));
        }
    }
    free(one.s);
    return r;
}

static const char *flat_record(Flat *f, CXType rec, int depth, int level, long long *size,
                               long long *align)
{
    if (level > SIG_STRUCT_DEPTH)
        return "struct-too-deep";
    if (clang_getTypeDeclaration(rec).kind == CK.UnionDecl)
        return "union-value";
    long long real_size = clang_Type_getSizeOf(rec), real_align = clang_Type_getAlignOf(rec);
    if (real_size < 0 || real_align < 0)
        return "struct-incomplete";
    FieldList fl;
    fields_of(rec, &fl);
    const char *r = fl.n == 0 ? "struct-empty" : NULL;
    long long off = 0, al = 1;
    buf_addc(f->out, '{');
    for (int i = 0; !r && i < fl.n; i++) {
        if (clang_Cursor_isBitField(fl.v[i])) {
            r = "struct-bitfield";
            break;
        }
        long long msize = 0, malign = 1;
        r = flat_type(f, clang_getCursorType(fl.v[i]), depth, level, &msize, &malign);
        if (r)
            break;
        off = align_up(off, malign);
        if (clang_Cursor_getOffsetOfField(fl.v[i]) != off * 8)
            f->layout_bad = 1;
        off += msize;
        if (malign > al)
            al = malign;
    }
    free(fl.v);
    if (r)
        return r;
    buf_addc(f->out, '}');
    *size = align_up(off, al);
    *align = al;
    if (*size != real_size || *align != real_align)
        f->layout_bad = 1;
    return NULL;
}

static const char *type_class(CXType t, int depth, int is_result, Buf *out)
{
    CXType c = canon(t);
    int k = c.kind;
    int sign;
    if (k == TK.Pointer && is_imp(t)) {
        buf_addc(out, 'p');
        return NULL;
    }
    if (k == TK.Void) {
        if (!is_result)
            return "void-argument";
        buf_addc(out, 'v');
        return NULL;
    }
    if (k == TK.Bool) {
        buf_addc(out, 'B');
        return NULL;
    }
    if (is_integer_kind(k, &sign)) {
        switch (clang_Type_getSizeOf(c)) {
        case 1: buf_addc(out, sign ? 'b' : 'B'); return NULL;
        case 2: buf_addc(out, sign ? 'h' : 'H'); return NULL;
        case 4: buf_addc(out, sign ? 'i' : 'u'); return NULL;
        case 8: buf_addc(out, sign ? 'l' : 'L'); return NULL;
        default: return "integer-width";
        }
    }
    if (k == TK.Int128 || k == TK.UInt128)
        return "int128";
    if (k == TK.Float) {
        buf_addc(out, 'f');
        return NULL;
    }
    if (k == TK.Double) {
        buf_addc(out, 'd');
        return NULL;
    }
    if (k == TK.LongDouble)
        return "long-double";
    if (k == TK.Half || k == TK.Float16 || k == TK.Float128)
        return "float-width";
    if (k == TK.Enum) {
        CXType it = clang_getEnumDeclIntegerType(clang_getTypeDeclaration(c));
        if (it.kind == TK.Invalid)
            return "unsupported-type";
        return type_class(it, depth, is_result, out);
    }
    if (k == TK.Pointer) {
        CXType pc = canon(clang_getPointeeType(c));
        if (is_fn_kind(pc.kind)) {
            if (is_result)
                return "callback-result";
            if (depth > 0)
                return "nested-callback";
            Buf cb = { 0 };
            const char *r = fn_notation(pc, depth + 1, &cb);
            if (!r && cb.n >= SIG_CB_MAX)
                r = "callback-too-long";
            if (!r) {
                buf_add(out, "c{");
                buf_add(out, cb.s);
                buf_addc(out, '}');
            }
            free(cb.s);
            return r;
        }
        if (is_va_list_tag(pc))
            return "va-list";
        CXType q = pc;
        for (int guard = 0; guard < 16 && q.kind == TK.Pointer; guard++) {
            q = canon(clang_getPointeeType(q));
            if (is_fn_kind(q.kind))
                return "callback-pointer";
        }
        buf_addc(out, 'p');
        return NULL;
    }
    if (k == TK.BlockPointer)
        return "block";
    if (k == TK.ObjCObjectPointer || k == TK.ObjCId || k == TK.ObjCClass || k == TK.ObjCSel) {
        buf_addc(out, 'p');
        return NULL;
    }
    if (k == TK.Record) {
        if (is_va_list_tag(c))
            return "va-list";
        if (!is_result && transparent_union(c))
            return type_class(first_field_type(c), depth, is_result, out);
        Buf sb = { 0 };
        Flat f = { &sb, 0, 0 };
        long long size = 0, align = 1;
        const char *r = flat_record(&f, c, depth, 1, &size, &align);
        if (!r && f.layout_bad)
            r = "struct-layout";
        if (!r)
            buf_add(out, buf_str(&sb));
        free(sb.s);
        return r;
    }
    if (k == TK.Complex)
        return "complex";
    if (k == TK.Vector)
        return "vector";
    if (k == TK.Atomic)
        return "atomic";
    if (k == TK.Unexposed)
        return "unexposed-type";
    return "unsupported-type";
}

static const char *fn_notation(CXType fnc, int depth, Buf *out)
{
    CXType fc = canon(fnc);
    if (fc.kind == TK.FunctionNoProto)
        return "no-prototype";
    if (fc.kind != TK.FunctionProto)
        return "unsupported-type";
    if (clang_isFunctionTypeVariadic(fc))
        return "variadic";
    int n = clang_getNumArgTypes(fc);
    if (n < 0)
        return "no-prototype";
    if (n > SIG_MAX_ARGS)
        return "too-many-args";
    CXType rs = clang_getResultType(fnc);
    const char *r = type_class(is_imp(rs) ? rs : clang_getResultType(fc), depth, 1, out);
    if (r)
        return r;
    buf_addc(out, '(');
    for (int i = 0; i < n; i++) {
        CXType as = fnc.kind == TK.FunctionProto ? clang_getArgType(fnc, (unsigned)i) : fc;
        r = type_class(is_imp(as) ? as : clang_getArgType(fc, (unsigned)i), depth, 0, out);
        if (r)
            return r;
    }
    buf_addc(out, ')');
    return NULL;
}

static int sig_valid(const char *s, int allow_cb);

static int sig_struct_valid(const char **sp, int level, int *members)
{
    const char *s = *sp + 1;
    int count = 0;
    if (level > SIG_STRUCT_DEPTH)
        return 0;
    while (*s != '}') {
        if (*s == '{') {
            if (!sig_struct_valid(&s, level + 1, members))
                return 0;
        } else if (*s && strchr("bBhHiulLpfd", *s)) {
            if (++*members > SIG_STRUCT_MEMBERS)
                return 0;
            s++;
        } else {
            return 0;
        }
        count++;
    }
    if (count == 0)
        return 0;
    *sp = s + 1;
    return 1;
}

static int sig_class_valid(const char **sp, int allow_cb, int is_result)
{
    const char *s = *sp;
    if (*s == '{') {
        int members = 0;
        if (!sig_struct_valid(&s, 1, &members))
            return 0;
        *sp = s;
        return 1;
    }
    if (*s == 'c' && !is_result) {
        if (!allow_cb || s[1] != '{')
            return 0;
        const char *open = s + 2, *close = open;
        int level = 0;
        for (; *close; close++) {
            if (*close == '{')
                level++;
            else if (*close == '}' && level-- == 0)
                break;
        }
        if (*close != '}')
            return 0;
        size_t len = (size_t)(close - open);
        if (len >= SIG_CB_MAX || memchr(open, 'c', len))
            return 0;
        char inner[SIG_CB_MAX];
        memcpy(inner, open, len);
        inner[len] = '\0';
        if (!sig_valid(inner, 0))
            return 0;
        *sp = close + 1;
        return 1;
    }
    if (!*s || !strchr(is_result ? "vbBhHiulLpfd" : "bBhHiulLpfd", *s))
        return 0;
    *sp = s + 1;
    return 1;
}

static int sig_valid(const char *s, int allow_cb)
{
    if (!sig_class_valid(&s, allow_cb, 1))
        return 0;
    if (*s++ != '(')
        return 0;
    int n = 0;
    while (*s && *s != ')') {
        if (!sig_class_valid(&s, allow_cb, 0))
            return 0;
        if (++n > SIG_MAX_ARGS)
            return 0;
    }
    return *s == ')' && s[1] == '\0';
}

static int nameable(const char *spelling)
{
    return strchr(spelling, '(') == NULL;
}

static void measure(const char *name, int arch, CXType c, const FieldList *fl, const char *key)
{
    int idx = map_get(&g_meas_map, name);
    if (idx < 0) {
        if (g_nmeas == g_cap_meas) {
            g_cap_meas = g_cap_meas ? g_cap_meas * 2 : 256;
            g_meas = xalloc(g_meas, (size_t)g_cap_meas * sizeof *g_meas);
        }
        idx = g_nmeas++;
        memset(&g_meas[idx], 0, sizeof g_meas[idx]);
        g_meas[idx].name = xstrdup(name);
        g_meas[idx].size[0] = g_meas[idx].size[1] = -1;
        map_set(&g_meas_map, name, idx);
    }
    Meas *m = &g_meas[idx];
    if (m->size[arch] >= 0)
        return;
    m->key[arch] = xstrdup(key);
    m->size[arch] = clang_Type_getSizeOf(c) * 8;
    m->align[arch] = clang_Type_getAlignOf(c) * 8;
    if (fl->n == 0)
        buf_add(&m->offs[arch], "-");
    for (int i = 0; i < fl->n; i++) {
        char t[32];
        snprintf(t, sizeof t, "%s%lld", i ? "," : "", clang_Cursor_getOffsetOfField(fl->v[i]));
        buf_add(&m->offs[arch], t);
    }
}

static int layout_same(CXType x, CXType a, CXType xsugar);

static int record_same(CXType x, CXType a, CXType xsugar)
{
    char *key = record_key(x);
    int idx = map_get(&g_lay_memo, key);
    if (idx < 0) {
        if ((int)g_lay_memo.n >= g_lay_cap) {
            g_lay_cap = g_lay_cap ? g_lay_cap * 2 : 1024;
            g_lay_state = xalloc(g_lay_state, (size_t)g_lay_cap * sizeof *g_lay_state);
            g_lay_depth_of = xalloc(g_lay_depth_of, (size_t)g_lay_cap * sizeof *g_lay_depth_of);
        }
        idx = (int)g_lay_memo.n;
        map_set(&g_lay_memo, key, idx);
        g_lay_state[idx] = 0;
    }
    if (g_lay_state[idx] == 2 || g_lay_state[idx] == 3) {
        free(key);
        return g_lay_state[idx] == 2;
    }
    if (g_lay_state[idx] == 1) {
        if (g_lay_depth_of[idx] < g_lay_low)
            g_lay_low = g_lay_depth_of[idx];
        free(key);
        return 1;
    }

    long long sx = clang_Type_getSizeOf(x), sa = clang_Type_getSizeOf(a);
    if (sx < 0 || sa < 0) {
        g_lay_state[idx] = (sx < 0 && sa < 0) ? 2 : 3;
        free(key);
        return g_lay_state[idx] == 2;
    }

    int depth = ++g_lay_depth;
    g_lay_state[idx] = 1;
    g_lay_depth_of[idx] = depth;
    int saved_low = g_lay_low;
    g_lay_low = INT_MAX;

    char *name = NULL;
    if (nameable(key))
        name = xstrdup(key);
    else
        name = innermost_typedef(xsugar);

    FieldList fx, fa;
    fields_of(x, &fx);
    fields_of(a, &fa);
    if (name) {
        char *akey = record_key(a);
        measure(name, ARCH_X86, x, &fx, key);
        measure(name, ARCH_ARM, a, &fa, akey);
        free(akey);
    }

    int same = sx == sa && clang_Type_getAlignOf(x) == clang_Type_getAlignOf(a) && fx.n == fa.n;
    for (int i = 0; i < fx.n && i < fa.n; i++) {
        if (clang_Cursor_getOffsetOfField(fx.v[i]) != clang_Cursor_getOffsetOfField(fa.v[i]))
            same = 0;
        int bx = clang_Cursor_isBitField(fx.v[i]) ? clang_getFieldDeclBitWidth(fx.v[i]) : -1;
        int ba = clang_Cursor_isBitField(fa.v[i]) ? clang_getFieldDeclBitWidth(fa.v[i]) : -1;
        if (bx != ba)
            same = 0;
        CXType tx = clang_getCursorType(fx.v[i]);
        CXType ta = clang_getCursorType(fa.v[i]);
        if (clang_Type_getSizeOf(canon(tx)) != clang_Type_getSizeOf(canon(ta)))
            same = 0;
        if (!layout_same(canon(tx), canon(ta), tx))
            same = 0;
    }
    free(fx.v);
    free(fa.v);
    free(name);

    g_lay_depth--;
    int sub_low = g_lay_low;
    if (!same)
        g_lay_state[idx] = 3;
    else if (sub_low >= depth)
        g_lay_state[idx] = 2;
    else
        g_lay_state[idx] = 0;
    g_lay_low = saved_low;
    if (sub_low < depth && sub_low < g_lay_low)
        g_lay_low = sub_low;
    free(key);
    return same;
}

static int layout_same(CXType x, CXType a, CXType xsugar)
{
    if (is_fn_kind(x.kind) && is_fn_kind(a.kind))
        return 1;
    if (x.kind != a.kind) {
        int aggregate_x = x.kind == TK.Record || x.kind == TK.ConstantArray ||
                          x.kind == TK.IncompleteArray || x.kind == TK.VariableArray;
        int aggregate_a = a.kind == TK.Record || a.kind == TK.ConstantArray ||
                          a.kind == TK.IncompleteArray || a.kind == TK.VariableArray;
        if (aggregate_x || aggregate_a || is_fn_kind(x.kind) || is_fn_kind(a.kind))
            return 0;
        return clang_Type_getSizeOf(x) == clang_Type_getSizeOf(a) &&
               clang_Type_getAlignOf(x) == clang_Type_getAlignOf(a);
    }
    int k = x.kind;
    if (k == TK.Void)
        return 1;
    if (k == TK.Pointer || k == TK.BlockPointer) {
        CXType px = clang_getPointeeType(x);
        CXType pa = clang_getPointeeType(a);
        return layout_same(canon(px), canon(pa), px);
    }
    if (k == TK.ObjCObjectPointer || k == TK.ObjCId || k == TK.ObjCClass || k == TK.ObjCSel)
        return 1;
    if (k == TK.Record)
        return record_same(x, a, xsugar);
    if (k == TK.ConstantArray) {
        if (clang_getArraySize(x) != clang_getArraySize(a))
            return 0;
        CXType ex = clang_getArrayElementType(x);
        return layout_same(canon(ex), canon(clang_getArrayElementType(a)), ex);
    }
    if (k == TK.IncompleteArray || k == TK.VariableArray) {
        CXType ex = clang_getArrayElementType(x);
        return layout_same(canon(ex), canon(clang_getArrayElementType(a)), ex);
    }
    return clang_Type_getSizeOf(x) == clang_Type_getSizeOf(a) &&
           clang_Type_getAlignOf(x) == clang_Type_getAlignOf(a);
}

static int holds_callback(CXType c, int depth)
{
    if (depth > 32)
        return 0;
    if (c.kind == TK.ConstantArray || c.kind == TK.IncompleteArray || c.kind == TK.VariableArray)
        return holds_callback(canon(clang_getArrayElementType(c)), depth + 1);
    if (c.kind == TK.BlockPointer)
        return 1;
    if (c.kind == TK.Pointer) {
        CXType q = c;
        for (int guard = 0; guard < 16 && q.kind == TK.Pointer; guard++) {
            q = canon(clang_getPointeeType(q));
            if (is_fn_kind(q.kind))
                return 1;
        }
        return 0;
    }
    if (c.kind != TK.Record || clang_Type_getSizeOf(c) < 0)
        return 0;
    char *key = record_key(c);
    int idx = map_get(&g_fnf_memo, key);
    if (idx >= 0) {
        free(key);
        return g_fnf_val[idx];
    }
    if ((int)g_fnf_memo.n >= g_fnf_cap) {
        g_fnf_cap = g_fnf_cap ? g_fnf_cap * 2 : 1024;
        g_fnf_val = xalloc(g_fnf_val, (size_t)g_fnf_cap * sizeof *g_fnf_val);
    }
    idx = (int)g_fnf_memo.n;
    map_set(&g_fnf_memo, key, idx);
    g_fnf_val[idx] = 0;
    FieldList fl;
    fields_of(c, &fl);
    int hit = 0;
    for (int i = 0; i < fl.n && !hit; i++)
        hit = holds_callback(canon(clang_getCursorType(fl.v[i])), depth + 1);
    free(fl.v);
    g_fnf_val[idx] = hit;
    free(key);
    return hit;
}

typedef struct Waiver {
    int argpos;
    const char *shape;
    int line;
} Waiver;

static int opaque_record(CXType c)
{
    if (c.kind != TK.Record)
        return 0;
    char *key = record_key(c);
    const char *bare = key;
    if (strncmp(bare, "struct ", 7) == 0)
        bare += 7;
    else if (strncmp(bare, "union ", 6) == 0)
        bare += 6;
    int hit = 0;
    for (int i = 0; i < g_novr; i++) {
        if (strcmp(g_ovr[i].kind, "opaque") == 0 && strcmp(g_ovr[i].f[1], bare) == 0) {
            g_ovr[i].used = 1;
            hit = 1;
        }
    }
    free(key);
    return hit;
}

static const char *pointer_checks(CXType tx, CXType ta, int waived)
{
    CXType px = clang_getPointeeType(desugar_to(tx, TK.Pointer, TK.Pointer));
    CXType pa = clang_getPointeeType(canon(ta));
    CXType cpx = canon(px), cpa = canon(pa);
    for (int guard = 0; guard < 16 && cpx.kind == TK.Pointer && cpa.kind == TK.Pointer; guard++) {
        px = clang_getPointeeType(cpx);
        pa = clang_getPointeeType(cpa);
        cpx = canon(px);
        cpa = canon(pa);
    }
    if (!layout_same(cpx, cpa, px))
        return "layout";
    if (!waived && holds_callback(cpx, 0) && !opaque_record(cpx))
        return "callback-struct";
    return NULL;
}

static const char *member_checks(CXType tx, CXType ta)
{
    CXType x = canon(tx), a = canon(ta);
    if (x.kind == TK.ConstantArray && a.kind == TK.ConstantArray)
        return member_checks(clang_getArrayElementType(x), clang_getArrayElementType(a));
    if (x.kind == TK.Pointer && a.kind == TK.Pointer) {
        if (is_imp(tx))
            return NULL;
        return pointer_checks(tx, ta, 0);
    }
    if (x.kind != TK.Record || a.kind != TK.Record)
        return NULL;
    FieldList fx, fa;
    fields_of(x, &fx);
    fields_of(a, &fa);
    const char *r = fx.n == fa.n ? NULL : "layout";
    for (int i = 0; !r && i < fx.n; i++)
        r = member_checks(clang_getCursorType(fx.v[i]), clang_getCursorType(fa.v[i]));
    free(fx.v);
    free(fa.v);
    return r;
}

static const char *value_checks(CXType tx, CXType ta)
{
    if (!layout_same(canon(tx), canon(ta), tx))
        return "layout";
    return member_checks(tx, ta);
}

static const char *pair_checks(CXType fx, CXType fa, int depth, const Waiver *w, int nw)
{
    fx = desugar_to(fx, TK.FunctionProto, TK.FunctionNoProto);
    fa = canon(fa);
    int n = clang_getNumArgTypes(fx);
    for (int i = -1; i < n; i++) {
        CXType tx = i < 0 ? clang_getResultType(fx) : clang_getArgType(fx, (unsigned)i);
        CXType ta = i < 0 ? clang_getResultType(fa) : clang_getArgType(fa, (unsigned)i);
        int kx = canon(tx).kind;
        if (kx == TK.ConstantArray || kx == TK.IncompleteArray || kx == TK.VariableArray)
            tx = clang_getArgType(canon(fx), (unsigned)i);
        if (i >= 0 && canon(tx).kind == TK.Record && transparent_union(canon(tx))) {
            FieldList ux, ua;
            fields_of(canon(tx), &ux);
            fields_of(canon(ta), &ua);
            const char *r = ux.n == ua.n ? NULL : "layout";
            for (int j = 0; !r && j < ux.n; j++) {
                CXType mx = clang_getCursorType(ux.v[j]), ma = clang_getCursorType(ua.v[j]);
                if (canon(mx).kind != TK.Pointer || canon(ma).kind != TK.Pointer)
                    r = layout_same(canon(mx), canon(ma), mx) ? NULL : "layout";
                else if (is_fn_kind(canon(clang_getPointeeType(canon(mx))).kind))
                    r = "callback-pointer";
                else
                    r = pointer_checks(mx, ma, 0);
            }
            free(ux.v);
            free(ua.v);
            if (r)
                return r;
            continue;
        }
        if (canon(tx).kind == TK.Record && canon(ta).kind == TK.Record) {
            const char *r = value_checks(tx, ta);
            if (r)
                return r;
            continue;
        }
        if (canon(tx).kind != TK.Pointer || canon(ta).kind != TK.Pointer || is_imp(tx))
            continue;
        CXType pxs = clang_getPointeeType(desugar_to(tx, TK.Pointer, TK.Pointer));
        if (is_fn_kind(canon(pxs).kind)) {
            const char *r = pair_checks(pxs, clang_getPointeeType(canon(ta)), depth + 1, NULL, 0);
            if (r)
                return r;
            continue;
        }
        int waived = 0;
        for (int j = 0; depth == 0 && j < nw; j++)
            if (w[j].argpos == i)
                waived = 1;
        const char *r = pointer_checks(tx, ta, waived);
        if (r)
            return r;
    }
    return NULL;
}

typedef struct Collect {
    Pass *p;
    int arch;
} Collect;

static int collect_visit(CXCursor c, CXCursor parent, CXClientData d)
{
    (void)parent;
    Collect *cc = d;
    Pass *p = cc->p;
    int k = c.kind;
    if (k == CK.LinkageSpec || k == CK.UnexposedDecl)
        return CXChildVisit_Recurse;
    if (k == CK.TypedefDecl && cc->arch == ARCH_X86) {
        char *name = cx(clang_getCursorSpelling(c));
        if (map_get(&p->tdefs, name) < 0) {
            if (p->ntdef == p->captdef) {
                p->captdef = p->captdef ? p->captdef * 2 : 1024;
                p->tdef = xalloc(p->tdef, (size_t)p->captdef * sizeof *p->tdef);
            }
            p->tdef[p->ntdef] = c;
            map_set(&p->tdefs, name, p->ntdef++);
        }
        free(name);
        return CXChildVisit_Continue;
    }
    if (k != CK.FunctionDecl && k != CK.VarDecl)
        return CXChildVisit_Continue;
    if (clang_getCursorLinkage(c) != CXLinkage_External)
        return CXChildVisit_Continue;

    int a = cc->arch;
    if (p->ndecl[a] == p->capdecl[a]) {
        p->capdecl[a] = p->capdecl[a] ? p->capdecl[a] * 2 : 4096;
        p->decl[a] = xalloc(p->decl[a], (size_t)p->capdecl[a] * sizeof *p->decl[a]);
    }
    int idx = p->ndecl[a];
    p->decl[a][p->ndecl[a]++] = c;
    char *key = a == ARCH_X86 ? cx(clang_Cursor_getMangling(c)) : cx(clang_getCursorUSR(c));
    Map *m = a == ARCH_X86 ? &p->mangled : &p->usr;
    int old = map_get(m, key);
    if (old < 0) {
        map_set(m, key, idx);
    } else if (k == CK.FunctionDecl) {
        int old_proto = canon(clang_getCursorType(p->decl[a][old])).kind == TK.FunctionProto;
        int new_proto = canon(clang_getCursorType(c)).kind == TK.FunctionProto;
        if (!old_proto && new_proto)
            map_set(m, key, idx);
    }
    free(key);
    return CXChildVisit_Continue;
}

static void parse_pass(CXIndex index, Pass *p)
{
    for (int arch = 0; arch < 2; arch++) {
        char target[96];
        snprintf(target, sizeof target, "%s-apple-macos%s", g_arch_name[arch], g_version);
        const char *args[16 + MAX_DEFINES];
        int n = 0;
        int objc = strcmp(g_lib.language, "objective-c") == 0;
        const char *file = objc ? "sdkgen-umbrella.m" : "sdkgen-umbrella.c";
        args[n++] = "-x";
        args[n++] = g_lib.language;
        args[n++] = "-target";
        args[n++] = target;
        args[n++] = "-isysroot";
        args[n++] = g_sdk;
        args[n++] = "-std=gnu17";
        args[n++] = "-w";
        for (int i = 0; i < p->ndefines; i++)
            args[n++] = p->defines[i];
        char *src = xprintf("#include \"%s\"\n", p->header);
        struct CXUnsavedFile uf = { file, src, strlen(src) };
        CXTranslationUnit tu = NULL;
        int rc = clang_parseTranslationUnit2(index, file, args, n, &uf, 1,
                                             CXTranslationUnit_SkipFunctionBodies, &tu);
        if (rc != 0 || !tu)
            die("libclang could not parse %s for %s (error %d)", p->header, g_arch_name[arch], rc);
        unsigned nd = clang_getNumDiagnostics(tu);
        int errors = 0;
        for (unsigned i = 0; i < nd; i++) {
            CXDiagnostic dg = clang_getDiagnostic(tu, i);
            if (clang_getDiagnosticSeverity(dg) >= CXDiagnostic_Error) {
                if (errors < 25) {
                    char *msg = cx(clang_formatDiagnostic(dg, clang_defaultDiagnosticDisplayOptions()));
                    fprintf(stderr, "sdkgen: %s: %s\n", g_arch_name[arch], msg);
                    free(msg);
                }
                errors++;
            }
            clang_disposeDiagnostic(dg);
        }
        if (errors)
            die("%s has %d error(s) for %s%s%s; fix the umbrella header", p->header, errors,
                g_arch_name[arch], p->ndefines ? " with " : "", p->ndefines ? p->defines[0] : "");
        free(src);
        p->tu[arch] = tu;
        Collect cc = { p, arch };
        clang_visitChildren(clang_getTranslationUnitCursor(tu), collect_visit, &cc);
    }
}

static char **split_words(char *line, int *n)
{
    int cap = 8;
    char **v = xalloc(NULL, (size_t)cap * sizeof *v);
    *n = 0;
    char *s = line;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        char *start = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = '\0';
        if (*n == cap) {
            cap *= 2;
            v = xalloc(v, (size_t)cap * sizeof *v);
        }
        v[(*n)++] = start;
    }
    return v;
}

static char *read_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("cannot open %s", path);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *t = xalloc(NULL, (size_t)size + 1);
    if (size > 0 && fread(t, 1, (size_t)size, fp) != (size_t)size)
        die("cannot read %s", path);
    t[size] = '\0';
    fclose(fp);
    return t;
}

static void load_config(const char *name)
{
    char *path = xprintf("%s/libraries", g_tooldir);
    char *text = read_text(path);
    int in = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        int n;
        char *copy = xstrdup(line);
        char **w = split_words(copy, &n);
        if (n == 0)
            continue;
        if (strcmp(w[0], "library") == 0) {
            if (n != 4)
                die("%s: library wants a name, an install name and a .tbd path", path);
            in = strcmp(w[1], name) == 0;
            if (in) {
                if (g_lib.name)
                    die("%s names library %s twice", path, name);
                g_lib.name = xstrdup(w[1]);
                g_lib.install = xstrdup(w[2]);
                g_lib.tbd = xstrdup(w[3]);
            }
        } else if (strcmp(w[0], "language") == 0) {
            if (n != 2 || (strcmp(w[1], "c") != 0 && strcmp(w[1], "objective-c") != 0))
                die("%s: language wants c or objective-c", path);
            if (!in)
                continue;
            if (g_lib.language)
                die("%s names a language for %s twice", path, name);
            g_lib.language = xstrdup(w[1]);
        } else if (strcmp(w[0], "pass") == 0) {
            if (!in)
                continue;
            if (n < 2 || n - 2 > MAX_DEFINES || g_lib.npasses == MAX_PASSES)
                die("%s: malformed pass for %s", path, name);
            Pass *p = &g_lib.passes[g_lib.npasses++];
            memset(p, 0, sizeof *p);
            p->header = xprintf("%s/headers/%s", g_tooldir, w[1]);
            for (int i = 2; i < n; i++)
                p->defines[p->ndefines++] = xstrdup(w[i]);
        } else {
            die("%s: unknown directive %s", path, w[0]);
        }
        free(w);
    }
    if (!g_lib.name)
        die("%s has no library named %s", path, name);
    if (!g_lib.npasses)
        die("%s gives library %s no pass", path, name);
    if (!g_lib.language)
        g_lib.language = xstrdup("c");
    free(text);
    free(path);
}

static const char *leaf_of(const char *install)
{
    const char *s = strrchr(install, '/');
    return s ? s + 1 : install;
}

enum { OBJC_NONE, OBJC_DATA, OBJC_IVAR };

typedef struct ExportSet {
    Map names;
    char **v;
    int n;
    int cap;
    int ld_pseudo;
    Map objc[3];
    Map objc_kind;
} ExportSet;

static void export_add(ExportSet *e, const char *name)
{
    if (strncmp(name, "$ld$", 4) == 0) {
        e->ld_pseudo++;
        return;
    }
    if (map_get(&e->names, name) >= 0)
        return;
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 4096;
        e->v = xalloc(e->v, (size_t)e->cap * sizeof *e->v);
    }
    e->v[e->n] = xstrdup(name);
    map_set(&e->names, name, e->n++);
}

static void collect_section(ExportSet *e, const TbdSection *sec)
{
    for (int i = 0; i < sec->n; i++) {
        const TbdItem *it = &sec->items[i];
        if (!tbd_has_target(&it->targets, "x86_64-macos"))
            continue;
        const TbdList *lists[3] = { &it->symbols, &it->weak_symbols, &it->tlv_symbols };
        for (int l = 0; l < 3; l++)
            for (int j = 0; j < lists[l]->n; j++)
                export_add(e, lists[l]->v[j]);
        const TbdList *objc[3] = { &it->objc_classes, &it->objc_eh_types, &it->objc_ivars };
        for (int l = 0; l < 3; l++) {
            for (int j = 0; j < objc[l]->n; j++) {
                const char *v = objc[l]->v[j];
                map_set(&e->objc[l], v, 1);
                char *names[2] = { NULL, NULL };
                if (l == 0) {
                    names[0] = xprintf("_OBJC_CLASS_$_%s", v);
                    names[1] = xprintf("_OBJC_METACLASS_$_%s", v);
                } else {
                    names[0] = xprintf(l == 1 ? "_OBJC_EHTYPE_$_%s" : "_OBJC_IVAR_$_%s", v);
                }
                for (int k = 0; k < 2 && names[k]; k++) {
                    export_add(e, names[k]);
                    map_set(&e->objc_kind, names[k], l == 2 ? OBJC_IVAR : OBJC_DATA);
                    free(names[k]);
                }
            }
        }
    }
}

static const TbdDoc *find_doc(const TbdFile *main, const char *install, TbdFile **extra, int *nextra)
{
    const TbdDoc *d = tbd_find(main, install);
    if (d)
        return d;
    for (int i = 0; i < *nextra; i++)
        if ((d = tbd_find(&(*extra)[i], install)))
            return d;
    char *rel = xstrdup(install + (install[0] == '/'));
    size_t len = strlen(rel);
    char *path;
    if (len > 6 && strcmp(rel + len - 6, ".dylib") == 0) {
        rel[len - 6] = '\0';
        path = xprintf("%s/%s.tbd", g_sdk, rel);
    } else {
        path = xprintf("%s/%s.tbd", g_sdk, rel);
    }
    *extra = xalloc(*extra, (size_t)(*nextra + 1) * sizeof **extra);
    char err[1024];
    if (tbd_read(path, &(*extra)[*nextra], err, sizeof err) != 0)
        die("re-exported %s: %s", install, err);
    (*nextra)++;
    d = tbd_find(&(*extra)[*nextra - 1], install);
    if (!d)
        die("%s does not describe %s", path, install);
    free(rel);
    free(path);
    return d;
}

static void collect_doc(ExportSet *e, const TbdFile *main, const TbdDoc *doc, Map *seen,
                        TbdFile **extra, int *nextra, int top)
{
    if (map_get(seen, doc->install_name) >= 0)
        return;
    map_set(seen, doc->install_name, 1);
    if (!top && doc->parent_umbrella.n == 0)
        return;
    collect_section(e, &doc->exports);
    collect_section(e, &doc->reexports);
    for (int i = 0; i < doc->reexported_libraries.n; i++) {
        const TbdItem *it = &doc->reexported_libraries.items[i];
        if (!tbd_has_target(&it->targets, "x86_64-macos"))
            continue;
        for (int j = 0; j < it->libraries.n; j++) {
            const TbdDoc *sub = find_doc(main, it->libraries.v[j], extra, nextra);
            collect_doc(e, main, sub, seen, extra, nextra, 0);
        }
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_overrides(void)
{
    char *path = xprintf("%s/overrides", g_tooldir);
    char *text = read_text(path);
    char *scope = NULL;
    int lineno = 0;
    char *p = text;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol)
            *eol = '\0';
        char *line = p;
        p = eol ? eol + 1 : p + strlen(p);
        lineno++;
        if (line[0] == '#' || line[0] == '\0')
            continue;
        int n;
        char **w = split_words(xstrdup(line), &n);
        if (n == 0)
            continue;
        const char *k = w[0];
        if (strcmp(k, "library") == 0) {
            if (n != 2)
                die("%s:%d: library wants one install name", path, lineno);
            scope = xstrdup(w[1]);
            continue;
        }
        int ok = (strcmp(k, "fn") == 0 && n == 4 && sig_valid(w[3], 1)) ||
                 (strcmp(k, "data") == 0 && n == 3) ||
                 (strcmp(k, "omit") == 0 && n == 2) ||
                 (strcmp(k, "opaque") == 0 && n == 2) ||
                 (strcmp(k, "var") == 0 && n == 4 && atoi(w[2]) > 0) ||
                 (strcmp(k, "special") == 0 && n == 3) ||
                 (strcmp(k, "stub") == 0 && n == 3) ||
                 (strcmp(k, "struct") == 0 && n == 4) ||
                 (strcmp(k, "shape") == 0 && n >= 4 && atoi(w[3]) > 0 && n == 4 + atoi(w[3]));
        if (!ok)
            die("%s:%d: malformed %s record", path, lineno, k);
        if (strcmp(k, "shape") == 0)
            for (int i = 4; i < n; i++)
                if (strcmp(w[i], "-") != 0 && !sig_valid(w[i], 0))
                    die("%s:%d: shape word %s is not a notation", path, lineno, w[i]);
        int glob = strcmp(k, "shape") != 0 && strcmp(k, "opaque") != 0 && strpbrk(w[1], "*?[") != NULL;
        if (glob && strcmp(k, "stub") != 0)
            die("%s:%d: only a stub may name exports by pattern", path, lineno);
        if (scope && strcmp(scope, g_lib.install) != 0)
            continue;
        g_ovr = xalloc(g_ovr, (size_t)(g_novr + 1) * sizeof *g_ovr);
        Override *o = &g_ovr[g_novr++];
        o->line = lineno;
        o->scope = scope;
        o->kind = w[0];
        o->f = w;
        o->nf = n;
        o->glob = glob;
        o->used = 0;
    }
    free(path);
}

static const Override *find_shape(const char *name, const char *version)
{
    for (int i = 0; i < g_novr; i++)
        if (strcmp(g_ovr[i].kind, "shape") == 0 && strcmp(g_ovr[i].f[1], name) == 0 &&
            strcmp(g_ovr[i].f[2], version) == 0)
            return &g_ovr[i];
    return NULL;
}

static void verify_shapes(void)
{
    for (int i = 0; i < g_novr; i++) {
        Override *o = &g_ovr[i];
        if (strcmp(o->kind, "shape") != 0)
            continue;
        char *tname = strcmp(o->f[2], "0") == 0 ? xstrdup(o->f[1]) : xprintf("%s%s", o->f[1], o->f[2]);
        int found = 0;
        for (int pi = 0; pi < g_lib.npasses && !found; pi++) {
            Pass *p = &g_lib.passes[pi];
            int idx = map_get(&p->tdefs, tname);
            if (idx < 0)
                continue;
            found = 1;
            CXType rec = canon(clang_getTypedefDeclUnderlyingType(p->tdef[idx]));
            if (rec.kind != TK.Record)
                die("overrides:%d: %s is not a structure", o->line, tname);
            FieldList fl;
            fields_of(rec, &fl);
            int words = atoi(o->f[3]);
            if (clang_Type_getSizeOf(rec) != 8LL * words || fl.n != words)
                die("overrides:%d: shape %s version %s has %d words, but %s is %lld bytes in %d fields",
                    o->line, o->f[1], o->f[2], words, tname, clang_Type_getSizeOf(rec), fl.n);
            for (int w = 0; w < words; w++) {
                CXType ft = canon(clang_getCursorType(fl.v[w]));
                Buf nb = { 0 };
                const char *word = "-";
                if (clang_Cursor_getOffsetOfField(fl.v[w]) != 64LL * w)
                    die("overrides:%d: field %d of %s is not at word %d", o->line, w, tname, w);
                if (ft.kind == TK.Pointer && is_fn_kind(canon(clang_getPointeeType(ft)).kind)) {
                    const char *r = fn_notation(canon(clang_getPointeeType(ft)), 1, &nb);
                    if (r)
                        die("overrides:%d: word %d of %s is a function pointer that cannot cross (%s)",
                            o->line, w, tname, r);
                    word = buf_str(&nb);
                } else if (holds_callback(ft, 0)) {
                    die("overrides:%d: word %d of %s holds a callback the shape cannot describe",
                        o->line, w, tname);
                }
                if (strcmp(word, o->f[4 + w]) != 0)
                    die("overrides:%d: shape %s version %s says word %d is %s, the header says %s",
                        o->line, o->f[1], o->f[2], w, o->f[4 + w], word);
                free(nb.s);
            }
            free(fl.v);
        }
        if (!found)
            die("overrides:%d: no header declares %s, so shape %s version %s cannot be checked",
                o->line, tname, o->f[1], o->f[2]);
        o->used = 1;
        free(tname);
    }
}

static int host_is_code(void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64)
            continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const struct load_command *lc = (const struct load_command *)(mh + 1);
        for (uint32_t c = 0; c < mh->ncmds; c++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64)
                continue;
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            uintptr_t lo = (uintptr_t)(seg->vmaddr + (uint64_t)slide);
            if (a < lo || a >= lo + seg->vmsize)
                continue;
            const struct section_64 *s = (const struct section_64 *)(seg + 1);
            for (uint32_t k = 0; k < seg->nsects; k++, s++) {
                uintptr_t slo = (uintptr_t)(s->addr + (uint64_t)slide);
                if (a >= slo && a < slo + s->size)
                    return (s->flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0;
            }
            return 0;
        }
    }
    return 0;
}

static void *g_host;

static int host_has(const char *sym)
{
    return sym && dlsym(g_host, sym) != NULL;
}

static void set_stub(Rec *r, const char *reason)
{
    r->kind = R_STUB;
    r->reason = reason;
    r->functionish = 1;
}

static void set_omit(Rec *r, const char *reason)
{
    r->kind = R_OMIT;
    r->reason = reason;
    r->functionish = 0;
}

static char *host_name(const char *mangled)
{
    return xstrdup(mangled[0] == '_' ? mangled + 1 : mangled);
}

static int same_but_signedness(const char *x, const char *a)
{
    if (strlen(x) != strlen(a))
        return 0;
    for (; *x; x++, a++) {
        char cx = *x == 'u' ? 'i' : *x == 'L' ? 'l' : *x;
        char ca = *a == 'u' ? 'i' : *a == 'L' ? 'l' : *a;
        if (cx != ca && !(*x == 'b' && *a == 'B'))
            return 0;
    }
    return 1;
}

static void classify(Rec *r)
{
    if (r->objc == OBJC_DATA) {
        r->kind = R_DATA;
        r->host = host_name(r->name);
        r->host_missing = !host_has(r->host);
        return;
    }
    if (r->objc == OBJC_IVAR) {
        set_omit(r, "objc-ivar");
        return;
    }
    Pass *p = NULL;
    CXCursor x = { 0 }, a = { 0 };
    int have_arm = 0;
    for (int pi = 0; pi < g_lib.npasses; pi++) {
        int idx = map_get(&g_lib.passes[pi].mangled, r->name);
        if (idx < 0)
            continue;
        p = &g_lib.passes[pi];
        x = p->decl[ARCH_X86][idx];
        char *usr = cx(clang_getCursorUSR(x));
        int ai = map_get(&p->usr, usr);
        free(usr);
        if (ai >= 0) {
            a = p->decl[ARCH_ARM][ai];
            have_arm = 1;
        }
        break;
    }

    if (!p) {
        if (r->name[0] != '_') {
            set_omit(r, "no-declaration-missing");
            return;
        }
        void *addr = dlsym(g_host, r->name + 1);
        if (!addr)
            set_omit(r, "no-declaration-missing");
        else if (host_is_code(addr))
            set_stub(r, "no-declaration");
        else if (strncmp(r->name, "_$s", 3) == 0 || strncmp(r->name, "_$S", 3) == 0)
            set_omit(r, "swift");
        else {
            r->kind = R_DATA;
            r->host = xstrdup(r->name + 1);
            r->host_missing = 0;
        }
        return;
    }

    int is_fn = x.kind == CK.FunctionDecl;
    if (!have_arm || a.kind != x.kind) {
        if (is_fn)
            set_stub(r, "no-arm64-declaration");
        else
            set_omit(r, "no-arm64-declaration");
        return;
    }
    char *mangled = cx(clang_Cursor_getMangling(a));
    char *host = host_name(mangled);
    free(mangled);

    if (!is_fn) {
        if (clang_getCursorTLSKind(x) != CXTLS_None) {
            set_omit(r, "thread-local");
        } else {
            CXType tx = clang_getCursorType(x), ta = clang_getCursorType(a);
            if (!layout_same(canon(tx), canon(ta), tx)) {
                set_omit(r, "layout");
            } else {
                r->kind = R_DATA;
                r->host = host;
                r->host_missing = !host_has(host);
                if (!clang_isConstQualifiedType(canon(tx)) && holds_callback(canon(tx), 0))
                    r->note = xstrdup("writable variable holding a function pointer");
                return;
            }
        }
        free(host);
        return;
    }

    Waiver w[8];
    int nw = 0;
    for (int i = 0; i < g_novr; i++) {
        Override *o = &g_ovr[i];
        if (strcmp(o->kind, "struct") != 0 || strcmp(o->f[1], r->name) != 0)
            continue;
        if (nw == 8)
            die("overrides:%d: too many struct records for %s", o->line, r->name);
        w[nw].argpos = atoi(o->f[2]);
        w[nw].shape = o->f[3];
        w[nw].line = o->line;
        nw++;
    }

    CXType tx = clang_getCursorType(x), ta = clang_getCursorType(a);
    Buf sx = { 0 }, sa = { 0 };
    const char *rx = fn_notation(desugar_to(tx, TK.FunctionProto, TK.FunctionNoProto), 0, &sx);
    if (rx) {
        set_stub(r, rx);
    } else {
        const char *ra = fn_notation(desugar_to(ta, TK.FunctionProto, TK.FunctionNoProto), 0, &sa);
        int same = !ra && strcmp(buf_str(&sx), buf_str(&sa)) == 0;
        if (!ra && !same && same_but_signedness(buf_str(&sx), buf_str(&sa))) {
            same = 1;
            r->note = xprintf("only the signedness of a class differs: x86_64 %s, arm64 %s",
                              buf_str(&sx), buf_str(&sa));
        }
        if (!same) {
            set_stub(r, "arch-mismatch");
            r->note = xprintf("x86_64 %s, arm64 %s", buf_str(&sx), ra ? ra : buf_str(&sa));
        } else {
            CXType fxs = desugar_to(tx, TK.FunctionProto, TK.FunctionNoProto);
            for (int j = 0; j < nw; j++) {
                int nargs = clang_getNumArgTypes(canon(tx));
                if (w[j].argpos < 0 || w[j].argpos >= nargs)
                    die("overrides:%d: %s has no argument %d", w[j].line, r->name, w[j].argpos);
                CXType at = clang_getArgType(fxs, (unsigned)w[j].argpos);
                if (canon(at).kind != TK.Pointer)
                    die("overrides:%d: argument %d of %s is not a pointer", w[j].line, w[j].argpos, r->name);
                CXType pt = clang_getPointeeType(desugar_to(at, TK.Pointer, TK.Pointer));
                if (!typedef_chain_has(pt, w[j].shape))
                    die("overrides:%d: argument %d of %s does not point at a %s", w[j].line,
                        w[j].argpos, r->name, w[j].shape);
                if (!find_shape(w[j].shape, "0"))
                    die("overrides:%d: no shape %s version 0", w[j].line, w[j].shape);
            }
            const char *rc = pair_checks(tx, ta, 0, w, nw);
            if (rc) {
                set_stub(r, rc);
            } else {
                r->kind = R_FN;
                r->host = host;
                host = NULL;
                r->sig = xstrdup(buf_str(&sx));
                r->functionish = 1;
                r->host_missing = !host_has(r->host);
                if (!sig_valid(r->sig, 1))
                    die("generated notation %s for %s does not parse", r->sig, r->name);
            }
        }
    }
    free(sx.s);
    free(sa.s);
    free(host);
    for (int j = 0; j < nw; j++)
        if (r->kind != R_FN)
            die("overrides:%d: struct record for %s, which comes out stub %s", w[j].line, r->name,
                r->reason);
}

static void apply_override(Rec *r, Override *o)
{
    free(r->host);
    free(r->sig);
    free(r->note);
    r->host = r->sig = r->note = NULL;
    r->override = 1;
    r->host_missing = 0;
    o->used = 1;
    if (strcmp(o->kind, "fn") == 0) {
        r->kind = R_FN;
        r->host = xstrdup(o->f[2]);
        r->sig = xstrdup(o->f[3]);
        r->host_missing = !host_has(r->host);
    } else if (strcmp(o->kind, "omit") == 0) {
        r->kind = R_OMIT;
        r->reason = "override";
    } else if (strcmp(o->kind, "data") == 0) {
        r->kind = R_DATA;
        r->host = xstrdup(o->f[2]);
        r->host_missing = !host_has(r->host);
    } else if (strcmp(o->kind, "var") == 0) {
        r->kind = R_VAR;
        r->bytes = o->f[2];
        r->filler = o->f[3];
    } else if (strcmp(o->kind, "special") == 0) {
        r->kind = R_SPECIAL;
        r->handler = o->f[2];
    } else {
        r->kind = R_STUB;
        r->reason = o->f[2];
    }
}

static void overrides_for(Rec *r)
{
    Override *exact = NULL, *variant = NULL, *glob = NULL;
    size_t nlen = strlen(r->name);
    for (int i = 0; i < g_novr; i++) {
        Override *o = &g_ovr[i];
        const char *k = o->kind;
        if (strcmp(k, "shape") == 0 || strcmp(k, "struct") == 0 || strcmp(k, "opaque") == 0)
            continue;
        const char *pat = o->f[1];
        if (o->glob) {
            if (!glob && r->functionish && fnmatch(pat, r->name, 0) == 0)
                glob = o;
            continue;
        }
        if (strcmp(pat, r->name) == 0) {
            if (exact)
                die("overrides:%d and overrides:%d both name %s", exact->line, o->line, r->name);
            exact = o;
            continue;
        }
        size_t plen = strlen(pat);
        if (strcmp(k, "stub") == 0 && plen < nlen && strncmp(pat, r->name, plen) == 0 &&
            r->name[plen] == '$')
            variant = variant ? variant : o;
    }
    Override *o = exact ? exact : variant ? variant : glob;
    if (o)
        apply_override(r, o);
}

typedef struct Count {
    const char *reason;
    int n;
} Count;

static void count_reason(Count **v, int *n, const char *reason)
{
    for (int i = 0; i < *n; i++)
        if (strcmp((*v)[i].reason, reason) == 0) {
            (*v)[i].n++;
            return;
        }
    *v = xalloc(*v, (size_t)(*n + 1) * sizeof **v);
    (*v)[*n].reason = reason;
    (*v)[*n].n = 1;
    (*n)++;
}

static int cmp_count(const void *a, const void *b)
{
    return strcmp(((const Count *)a)->reason, ((const Count *)b)->reason);
}

static int cmp_shape(const void *a, const void *b)
{
    const Override *x = *(Override *const *)a, *y = *(Override *const *)b;
    int c = strcmp(x->f[1], y->f[1]);
    return c ? c : atoi(x->f[2]) - atoi(y->f[2]);
}

static int cmp_struct(const void *a, const void *b)
{
    const Override *x = *(Override *const *)a, *y = *(Override *const *)b;
    int c = strcmp(x->f[1], y->f[1]);
    return c ? c : atoi(x->f[2]) - atoi(y->f[2]);
}

static FILE *open_out(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
        die("cannot write %s", path);
    return fp;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: sdkgen --sdk <sdk> --version <v> --tooldir <tools/sdkgen> --library <name>\n"
            "              --out <root> --build <dir>\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *libname = NULL, *out = NULL, *build = NULL;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc)
            usage();
        if (strcmp(argv[i], "--sdk") == 0)
            g_sdk = argv[++i];
        else if (strcmp(argv[i], "--version") == 0)
            g_version = argv[++i];
        else if (strcmp(argv[i], "--tooldir") == 0)
            g_tooldir = argv[++i];
        else if (strcmp(argv[i], "--library") == 0)
            libname = argv[++i];
        else if (strcmp(argv[i], "--out") == 0)
            out = argv[++i];
        else if (strcmp(argv[i], "--build") == 0)
            build = argv[++i];
        else
            usage();
    }
    if (!g_sdk || !g_version || !g_tooldir || !libname || !out || !build)
        usage();

    resolve_kinds();
    load_config(libname);
    load_overrides();

    const char *leaf = leaf_of(g_lib.install);
    char *tbd_path = xprintf("%s/%s", g_sdk, g_lib.tbd);
    TbdFile tbd;
    char err[1024];
    if (tbd_read(tbd_path, &tbd, err, sizeof err) != 0)
        die("%s", err);
    const TbdDoc *top = tbd_find(&tbd, g_lib.install);
    if (!top)
        die("%s does not describe %s", tbd_path, g_lib.install);
    ExportSet ex;
    memset(&ex, 0, sizeof ex);
    Map seen = { 0 };
    TbdFile *extra = NULL;
    int nextra = 0;
    collect_doc(&ex, &tbd, top, &seen, &extra, &nextra, 1);
    qsort(ex.v, (size_t)ex.n, sizeof *ex.v, cmp_str);

    g_host = dlopen(g_lib.install, RTLD_LAZY | RTLD_LOCAL);
    if (!g_host)
        die("cannot open host %s: %s", g_lib.install, dlerror());

    CXIndex index = clang_createIndex(0, 0);
    char *ver = cx(clang_getClangVersion());
    fprintf(stderr, "sdkgen: %s, %s, SDK %s at %s\n", ver, g_lib.install, g_version, g_sdk);
    free(ver);
    for (int pi = 0; pi < g_lib.npasses; pi++)
        parse_pass(index, &g_lib.passes[pi]);

    verify_shapes();

    Rec *recs = calloc((size_t)ex.n, sizeof *recs);
    if (!recs)
        die("out of memory");
    for (int i = 0; i < ex.n; i++) {
        recs[i].name = ex.v[i];
        recs[i].objc = map_get(&ex.objc_kind, ex.v[i]) > 0 ? map_get(&ex.objc_kind, ex.v[i]) : OBJC_NONE;
        classify(&recs[i]);
    }
    for (int i = 0; i < ex.n; i++)
        overrides_for(&recs[i]);
    for (int i = 0; i < g_novr; i++) {
        Override *o = &g_ovr[i];
        if (strcmp(o->kind, "struct") == 0) {
            int idx = map_get(&ex.names, o->f[1]);
            if (idx < 0)
                die("overrides:%d: %s does not export %s", o->line, g_lib.install, o->f[1]);
            for (int j = 0; j < ex.n; j++)
                if (strcmp(recs[j].name, o->f[1]) == 0 && recs[j].kind != R_FN)
                    die("overrides:%d: struct record for %s, which is not fn", o->line, o->f[1]);
            o->used = 1;
        }
        if (!o->used && o->scope && strcmp(o->kind, "opaque") == 0)
            die("overrides:%d: no pointer in %s's signatures leads to a record named %s", o->line,
                g_lib.install, o->f[1]);
        if (!o->used && o->scope)
            die("overrides:%d: %s %s matches no export of %s", o->line, o->kind, o->f[1],
                g_lib.install);
    }

    char *dir = xprintf("%s/macos/%s", out, g_version);
    char *cmd = xprintf("mkdir -p '%s' '%s'", dir, build);
    if (system(cmd) != 0)
        die("cannot create %s", dir);
    free(cmd);

    char *api_path = xprintf("%s/%s.api", dir, leaf);
    FILE *api = open_out(api_path);
    fprintf(api, "# Generated by tools/sdkgen from the macOS %s SDK: the x86_64 exports of\n", g_version);
    fprintf(api, "# %s's .tbd, matched to their declarations in the SDK headers\n", g_lib.name);
    fprintf(api, "# parsed for x86_64 and arm64.  Do not edit; change tools/sdkgen/overrides\n");
    fprintf(api, "# or the generator and run tools/sdkgen.sh %s again.\n", g_lib.name);
    fprintf(api, "ocerz-apidb 1\nlibrary %s\nsdk macos %s\n", g_lib.install, g_version);

    int nfn = 0, ndata = 0, nvar = 0, nspecial = 0, nmissing = 0, novr = 0;
    Count *stubs = NULL, *omits = NULL;
    int nstubs = 0, nomits = 0;
    for (int i = 0; i < ex.n; i++) {
        Rec *r = &recs[i];
        if (r->override)
            novr++;
        if (r->host_missing)
            nmissing++;
        switch (r->kind) {
        case R_FN:
            fprintf(api, "fn %s %s %s\n", r->name, r->host, r->sig);
            nfn++;
            break;
        case R_DATA:
            fprintf(api, "data %s %s\n", r->name, r->host);
            ndata++;
            break;
        case R_VAR:
            fprintf(api, "var %s %s %s\n", r->name, r->bytes, r->filler);
            nvar++;
            break;
        case R_SPECIAL:
            fprintf(api, "special %s %s\n", r->name, r->handler);
            nspecial++;
            break;
        case R_STUB:
            fprintf(api, "stub %s %s\n", r->name, r->reason);
            count_reason(&stubs, &nstubs, r->reason);
            break;
        case R_OMIT:
            count_reason(&omits, &nomits, r->reason);
            break;
        default:
            die("%s was never classified", r->name);
        }
    }

    Override **shapes = xalloc(NULL, (size_t)(g_novr + 1) * sizeof *shapes);
    Override **structs = xalloc(NULL, (size_t)(g_novr + 1) * sizeof *structs);
    int nshapes = 0, nstructs = 0, nopaque = 0;
    for (int i = 0; i < g_novr; i++) {
        if (strcmp(g_ovr[i].kind, "opaque") == 0)
            nopaque++;
        if (strcmp(g_ovr[i].kind, "shape") == 0)
            shapes[nshapes++] = &g_ovr[i];
        else if (strcmp(g_ovr[i].kind, "struct") == 0)
            structs[nstructs++] = &g_ovr[i];
    }
    qsort(shapes, (size_t)nshapes, sizeof *shapes, cmp_shape);
    qsort(structs, (size_t)nstructs, sizeof *structs, cmp_struct);
    for (int i = 0; i < nshapes; i++) {
        fputs("shape", api);
        for (int j = 1; j < shapes[i]->nf; j++)
            fprintf(api, " %s", shapes[i]->f[j]);
        fputc('\n', api);
    }
    for (int i = 0; i < nstructs; i++)
        fprintf(api, "struct %s %s %s\n", structs[i]->f[1], structs[i]->f[2], structs[i]->f[3]);
    int header_done = 0;
    for (int i = 0; i < ex.n; i++) {
        if (recs[i].kind != R_OMIT)
            continue;
        if (!header_done) {
            fprintf(api, "# Exports with no record, so a program importing one fails to bind by name:\n");
            header_done = 1;
        }
        fprintf(api, "# omitted %s %s\n", recs[i].name, recs[i].reason);
    }
    fclose(api);

    qsort(stubs, (size_t)nstubs, sizeof *stubs, cmp_count);
    qsort(omits, (size_t)nomits, sizeof *omits, cmp_count);
    Buf cov = { 0 };
    char line[512];
    snprintf(line, sizeof line, "library %s\nsdk macos %s\nexports %d\nfn %d\ndata %d\nvar %d\nspecial %d\n",
             g_lib.install, g_version, ex.n, nfn, ndata, nvar, nspecial);
    buf_add(&cov, line);
    for (int i = 0; i < nstubs; i++) {
        snprintf(line, sizeof line, "stub %s %d\n", stubs[i].reason, stubs[i].n);
        buf_add(&cov, line);
    }
    for (int i = 0; i < nomits; i++) {
        snprintf(line, sizeof line, "omit %s %d\n", omits[i].reason, omits[i].n);
        buf_add(&cov, line);
    }
    snprintf(line, sizeof line,
             "host-missing %d\nobjc-classes %zu\nobjc-eh-types %zu\nobjc-ivars %zu\nld-pseudo-skipped %d\n"
             "overrides %d\noverride-shapes %d\noverride-structs %d\noverride-opaque %d\n",
             nmissing, ex.objc[0].n, ex.objc[1].n, ex.objc[2].n, ex.ld_pseudo, novr, nshapes, nstructs,
             nopaque);
    buf_add(&cov, line);

    char *cov_path = xprintf("%s/%s.coverage", build, leaf);
    FILE *cf = open_out(cov_path);
    fputs(buf_str(&cov), cf);
    fclose(cf);
    fputs(buf_str(&cov), stdout);

    char *lay_path = xprintf("%s/%s.layouts", build, leaf);
    FILE *lf = open_out(lay_path);
    fprintf(lf, "header %s\n", g_lib.passes[0].header);
    fprintf(lf, "language %s\n", g_lib.language);
    for (int i = 0; i < g_lib.passes[0].ndefines; i++)
        fprintf(lf, "define %s\n", g_lib.passes[0].defines[i]);
    char **names = xalloc(NULL, (size_t)(g_nmeas + 1) * sizeof *names);
    for (int i = 0; i < g_nmeas; i++)
        names[i] = g_meas[i].name;
    qsort(names, (size_t)g_nmeas, sizeof *names, cmp_str);
    for (int i = 0; i < g_nmeas; i++) {
        Meas *m = &g_meas[map_get(&g_meas_map, names[i])];
        for (int a = 0; a < 2; a++)
            fprintf(lf, "record %s %lld %lld %s %s\t%s\n", g_arch_name[a], m->size[a], m->align[a],
                    buf_str(&m->offs[a]), m->name, m->key[a]);
    }
    fclose(lf);

    for (int i = 0; i < ex.n; i++)
        if (recs[i].note)
            fprintf(stderr, "sdkgen: note: %s %s%s%s: %s\n",
                    recs[i].kind == R_FN ? "fn" : recs[i].kind == R_DATA ? "data" :
                    recs[i].kind == R_STUB ? "stub" : "record", recs[i].name,
                    recs[i].kind == R_STUB ? " " : "", recs[i].kind == R_STUB ? recs[i].reason : "",
                    recs[i].note);
    for (int i = 0; i < ex.n; i++)
        if (recs[i].host_missing)
            fprintf(stderr, "sdkgen: host-missing: %s %s\n", recs[i].name, recs[i].host);
    fprintf(stderr, "sdkgen: wrote %s, %s and %s (%d measured records)\n", api_path, cov_path,
            lay_path, g_nmeas);

    for (int pi = 0; pi < g_lib.npasses; pi++)
        for (int a = 0; a < 2; a++)
            clang_disposeTranslationUnit(g_lib.passes[pi].tu[a]);
    clang_disposeIndex(index);
    return 0;
}
