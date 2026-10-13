/*
 * steamgen -- the Steamworks interface database m32's Steam bridge reads (src/m32_steam.c).
 *
 *   steamgen --sdk <macOS SDK> --out <file> <steamworks_sdk_<ver> dir>...
 *
 * A 32-bit Mac game's libsteam_api asks Steam's client for interfaces by version string (SteamUser018, ...) and
 * calls their virtual methods.  There is no 32-bit client any more, so m32 hands the game objects whose vtables
 * cross into the 64-bit one, and it needs every method of every interface version in vtable order, with its i386
 * and arm64 notations.  This tool parses each Steamworks header set twice, as i386 and as arm64 macOS C++, and
 * writes, for each interface class whose header defines its version macro, the first SDK declaring a version
 * string with the most methods winning: Valve appends methods to an interface without a new version string
 * (SteamUtils010 gained five in 1.64), which a game built against the shorter list never calls, and kept
 * STEAMCONTROLLER_INTERFACE_VERSION for an empty placeholder class in 1.24-1.26.  A longer list that is not the
 * shorter one with methods appended is reported:
 *
 *   iface <version string> <class> <sdk dir name>
 *   m <slot> <method> <i386 notation> <arm64 notation> [I|S]
 *   bad <slot> <method> <reason>
 *
 * Notations are m32's (src/m32_cross.c): a result class, then the argument classes in parentheses, `this` first.
 * Itanium vtables list virtual methods in declaration order, a virtual destructor taking two slots.  CSteamID and
 * CGameID are 8-byte trivially copyable classes: passed and returned as a 64-bit integer is in both ABIs (Darwin
 * i386 returns 8-byte structures in edx:eax).  I marks a method returning an interface, which the bridge wraps by
 * the version string among its arguments; S one returning a C string, which it copies into guest memory.  A
 * function pointer, a structure by value of another size or one holding floats is `bad`: that slot answers 0.
 *
 * The headers are Valve's (Steamworks SDK license); they are read here and never copied into the output, which
 * holds only names and notations.  tools/steamgen.sh fetches them from Valve's Proton at a pinned commit.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clang_api.h"

enum {
    K_STRUCT = 2, K_CLASS = 4, K_CXXMETHOD = 21, K_NAMESPACE = 22, K_CONSTRUCTOR = 24, K_DESTRUCTOR = 25, K_BASE = 44,
    T_VOID = 2, T_BOOL = 3, T_CHAR_U = 4, T_UCHAR = 5, T_USHORT = 8, T_UINT = 9, T_ULONG = 10, T_ULONGLONG = 11,
    T_CHAR_S = 13, T_SCHAR = 14, T_SHORT = 16, T_INT = 17, T_LONG = 18, T_LONGLONG = 19, T_FLOAT = 21,
    T_DOUBLE = 22, T_POINTER = 101, T_LVALUEREF = 103, T_RECORD = 105, T_ENUM = 106, T_FUNCPROTO = 111,
};

typedef struct { char name[96]; char note[2][256]; char flag; const char *bad; } Method;
typedef struct { char cls[64]; Method *m; int n, cap; int defined; } Class;
typedef struct { Class *c; int n, cap; } Classes;
typedef struct { char *version, *cls, *sdk, *text, *sig; int n; } Iface;

static Iface *g_ifaces;
static FILE *g_out;
static int g_nifaces, g_cap_ifaces;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("steamgen: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static char *cstr(CXString s)
{
    char *r = strdup(clang_getCString(s) ? clang_getCString(s) : "");
    clang_disposeString(s);
    return r;
}

static int has_float(CXType rec);
static int float_field(CXCursor c, CXClientData d)
{
    CXType t = clang_getCanonicalType(clang_getCursorType(c));
    if (t.kind == T_FLOAT || t.kind == T_DOUBLE || (t.kind == T_RECORD && has_float(t)))
        *(int *)d = 1;
    return CXVisit_Continue;
}
static int has_float(CXType rec)
{
    int f = 0;
    clang_Type_visitFields(rec, float_field, &f);
    return f;
}

/* a record a call passes as its bytes has no copy or move constructor and no destructor of its own (CSteamID and
 * CGameID have none); with one, both ABIs pass a hidden reference to a copy instead.  ponytail: its own members only,
 * not its fields' or bases' */
static int own_copy(CXCursor c, CXCursor parent, CXClientData d)
{
    (void)parent;
    if ((c.kind == K_CONSTRUCTOR && (clang_CXXConstructor_isCopyConstructor(c) || clang_CXXConstructor_isMoveConstructor(c)) &&
         !clang_CXXMethod_isDefaulted(c)) ||
        (c.kind == K_DESTRUCTOR && !clang_CXXMethod_isDefaulted(c)))
        *(int *)d = 1;
    return CXChildVisit_Continue;
}
static int trivially_copied(CXType rec)
{
    int own = 0;
    clang_visitChildren(clang_getTypeDeclaration(rec), own_copy, &own);
    return !own;
}

static int is_iface_record(CXType t)
{
    char *n = cstr(clang_getCursorSpelling(clang_getTypeDeclaration(t)));
    int r = !strncmp(n, "ISteam", 6);
    free(n);
    return r;
}

static int is_response_record(CXType t)
{
    char *n = cstr(clang_getCursorSpelling(clang_getTypeDeclaration(t)));
    size_t len = strlen(n);
    int r = !strncmp(n, "ISteam", 6) && len > 8 && !strcmp(n + len - 8, "Response");
    free(n);
    return r;
}

/* the class of one type, or 0 with *why set; *flag gets I or S for a result */
static char type_char(CXType t, int is_result, char *flag, const char **why)
{
    CXType c = clang_getCanonicalType(t);
    long long size = clang_Type_getSizeOf(c);
    switch (c.kind) {
    case T_VOID: return is_result ? 'v' : (*why = "void-argument", 0);
    case T_BOOL: case T_CHAR_U: case T_UCHAR: return 'B';
    case T_CHAR_S: case T_SCHAR: return 'b';
    case T_USHORT: return 'H';
    case T_SHORT: return 'h';
    case T_UINT: return 'u';
    case T_INT: return 'i';
    case T_ULONG: return size == 8 ? 'L' : 'u';
    case T_LONG: return size == 8 ? 'l' : 'i';
    case T_ULONGLONG: return 'L';
    case T_LONGLONG: return 'l';
    case T_FLOAT: return 'f';
    case T_DOUBLE: return 'd';
    case T_ENUM:
        return size == 4 ? 'i' : size == 2 ? 'h' : size == 1 ? 'b' : (*why = "enum-width", 0);
    case T_LVALUEREF:
        return 'p';
    case T_POINTER: {
        CXType p = clang_getCanonicalType(clang_getPointeeType(c));
        if (p.kind == T_FUNCPROTO) {
            *why = "function-pointer";
            return 0;
        }
        if (is_result && (p.kind == T_CHAR_S || p.kind == T_CHAR_U))
            *flag = 'S';
        else if (is_result && p.kind == T_RECORD && is_iface_record(p))
            *flag = 'I';
        else if (!is_result && p.kind == T_RECORD && is_response_record(p)) {
            *why = "callback-object";   /* an ISteam...Response the client calls back through its vtable */
            return 0;
        }
        return 'p';
    }
    case T_RECORD:
        if (!trivially_copied(c)) {
            *why = "copied-by-constructor";
            return 0;
        }
        if (size == 8 && !has_float(c)) return 'L';
        if (size == 4 && !has_float(c)) return 'u';
        *why = "struct-by-value";
        return 0;
    default:
        *why = "unsupported-type";
        return 0;
    }
}

typedef struct { Classes *out; Class *cur; } Walk;

static void add_method(Class *k, const char *name, const char *note, char flag, const char *bad)
{
    if (k->n == k->cap) {
        k->cap = k->cap ? k->cap * 2 : 32;
        k->m = realloc(k->m, (size_t)k->cap * sizeof *k->m);
    }
    Method *m = &k->m[k->n++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->note[0], sizeof m->note[0], "%s", note);
    m->flag = flag;
    m->bad = bad;
}

static int method_visit(CXCursor c, CXCursor parent, CXClientData d)
{
    (void)parent;
    Class *k = d;
    if (c.kind == K_BASE)   /* its virtual methods would take slots before these, as many as it has */
        die("%s derives from another class, whose vtable slots steamgen does not count\n", k->cls);
    if (c.kind == K_DESTRUCTOR && clang_CXXMethod_isVirtual(c)) {
        add_method(k, "~", "v(p)", 0, "destructor");
        add_method(k, "~", "v(p)", 0, "destructor");
        return CXChildVisit_Continue;
    }
    if (c.kind != K_CXXMETHOD || !clang_CXXMethod_isVirtual(c))
        return CXChildVisit_Continue;
    char *name = cstr(clang_getCursorSpelling(c));
    char note[256], flag = 0;
    const char *why = NULL;
    int n = 0;
    char r = type_char(clang_getCursorResultType(c), 1, &flag, &why);
    if (r) {
        note[n++] = r;
        note[n++] = '(';
        note[n++] = 'p';
        int na = clang_Cursor_getNumArguments(c);
        for (int i = 0; i < na && r; i++) {
            char f2 = 0;
            r = type_char(clang_getCursorType(clang_Cursor_getArgument(c, (unsigned)i)), 0, &f2, &why);
            if (r && n < (int)sizeof note - 2)
                note[n++] = r;
        }
        note[n++] = ')';
    }
    note[n] = 0;
    if (r && note[0] == 'p' && strstr(name, "GetISteam"))
        flag = 'I';   /* GetISteamGenericInterface and the DEPRECATED_ ones return void *, but an interface all the same */
    add_method(k, name, r ? note : "", flag, r ? NULL : why);
    free(name);
    return CXChildVisit_Continue;
}

static int tu_visit(CXCursor c, CXCursor parent, CXClientData d)
{
    (void)parent;
    Classes *cs = d;
    if (c.kind == K_NAMESPACE)
        return CXChildVisit_Recurse;
    if ((c.kind != K_CLASS && c.kind != K_STRUCT) || !clang_isCursorDefinition(c))
        return CXChildVisit_Continue;
    char *name = cstr(clang_getCursorSpelling(c));
    if (strncmp(name, "ISteam", 6) != 0) {
        free(name);
        return CXChildVisit_Continue;
    }
    for (int i = 0; i < cs->n; i++)
        if (!strcmp(cs->c[i].cls, name)) {
            free(name);
            return CXChildVisit_Continue;
        }
    if (cs->n == cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 64;
        cs->c = realloc(cs->c, (size_t)cs->cap * sizeof *cs->c);
    }
    Class *k = &cs->c[cs->n++];
    memset(k, 0, sizeof *k);
    snprintf(k->cls, sizeof k->cls, "%s", name);
    k->defined = 1;
    clang_visitChildren(c, method_visit, k);
    free(name);
    return CXChildVisit_Continue;
}

static int header_names(const char *dir, char ***out)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 3 || strcmp(e->d_name + l - 2, ".h") != 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            *out = realloc(*out, (size_t)cap * sizeof **out);
        }
        (*out)[n++] = strdup(e->d_name);
    }
    closedir(d);
    return n;
}

static Classes parse(CXIndex idx, const char *dir, const char *target, const char *sdk, char **hdrs, int nh)
{
    char src[16384];
    int o = snprintf(src, sizeof src, "#include \"steam_api.h\"\n");
    for (int i = 0; i < nh && o < (int)sizeof src - 128; i++)
        if (!strncmp(hdrs[i], "isteam", 6) || !strcmp(hdrs[i], "steam_gameserver.h"))
            o += snprintf(src + o, sizeof src - (size_t)o, "#include \"%s\"\n", hdrs[i]);
    struct CXUnsavedFile f = { "steamgen.cpp", src, (unsigned long)o };
    const char *args[] = { "-x", "c++", "-std=c++11", "-fsyntax-only", "-target", target, "-isysroot", sdk,
                           "-I", dir, "-Wno-everything", "-DPOSIX", "-DOSX" };
    CXTranslationUnit tu = NULL;
    Classes cs = { 0 };
    if (clang_parseTranslationUnit2(idx, "steamgen.cpp", args, (int)(sizeof args / sizeof args[0]), &f, 1,
                                    CXTranslationUnit_SkipFunctionBodies, &tu) || !tu)
        return cs;
    clang_visitChildren(clang_getTranslationUnitCursor(tu), tu_visit, &cs);
    clang_disposeTranslationUnit(tu);
    return cs;
}

/* `#define <BASE>_INTERFACE_VERSION<suffix> "<string>"`: BASE is the class name without its I, upper case */
static void versions(const char *dir, char **hdrs, int nh, Classes *x86, Classes *arm, const char *sdkname)
{
    for (int h = 0; h < nh; h++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, hdrs[h]);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        char line[1024];
        while (fgets(line, sizeof line, fp)) {
            char base[128], ver[256];
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "#define", 7)) continue;
            if (sscanf(p, "#define %127[A-Za-z0-9]_INTERFACE_VERSION%*[A-Za-z0-9_] \"%255[^\"]\"", base, ver) != 2 &&
                sscanf(p, "#define %127[A-Za-z0-9]_INTERFACE_VERSION \"%255[^\"]\"", base, ver) != 2)
                continue;
            int at = -1;
            for (int i = 0; i < g_nifaces && at < 0; i++)
                if (!strcmp(g_ifaces[i].version, ver)) at = i;
            Class *kx = NULL, *ka = NULL;
            for (int i = 0; i < x86->n; i++) {
                char up[96];
                const char *cn = x86->c[i].cls + 1;
                size_t k = 0;
                for (; cn[k] && k < sizeof up - 1; k++) up[k] = (char)toupper((unsigned char)cn[k]);
                up[k] = 0;
                if (!strcmp(up, base)) kx = &x86->c[i];
            }
            if (!kx)
                continue;
            for (int i = 0; i < arm->n; i++)
                if (!strcmp(arm->c[i].cls, kx->cls)) ka = &arm->c[i];
            if (!ka || ka->n != kx->n) {
                fprintf(stderr, "steamgen: %s %s: the arm64 parse differs, skipped\n", sdkname, ver);
                continue;
            }
            size_t cap = 256 + (size_t)kx->n * 640, o = 0, so = 0;
            char *text = malloc(cap), *sig = malloc(cap);   /* sig: the slots without their names, for comparing */
            text[0] = sig[0] = 0;
            for (int m = 0; m < kx->n; m++) {
                Method *me = &kx->m[m];
                const char *bad = me->bad ? me->bad : ka->m[m].bad;
                if (bad)
                    o += (size_t)snprintf(text + o, cap - o, "bad %d %s %s\n", m, me->name, bad);
                else if (me->flag)
                    o += (size_t)snprintf(text + o, cap - o, "m %d %s %s %s %c\n", m, me->name, me->note[0],
                                          ka->m[m].note[0], me->flag);
                else
                    o += (size_t)snprintf(text + o, cap - o, "m %d %s %s %s\n", m, me->name, me->note[0],
                                          ka->m[m].note[0]);
                so += (size_t)snprintf(sig + so, cap - so, "%s %s %c|", bad ? "bad" : me->note[0],
                                       bad ? "" : ka->m[m].note[0], me->flag ? me->flag : '-');
            }
            if (at >= 0) {
                Iface *f = &g_ifaces[at];
                if (kx->n > f->n) {
                    if (strncmp(sig, f->sig, strlen(f->sig)) != 0)
                        fprintf(stderr, "steamgen: %s in %s is not %s's with methods appended; taking the longer\n",
                                ver, sdkname, f->sdk);
                    free(f->text);
                    free(f->sig);
                    *f = (Iface){ f->version, strdup(kx->cls), strdup(sdkname), text, sig, kx->n };
                } else {
                    if (strncmp(f->sig, sig, strlen(sig)) != 0)
                        fprintf(stderr, "steamgen: %s in %s is not a prefix of %s's; keeping %s\n", ver, sdkname,
                                f->sdk, f->sdk);
                    free(text);
                    free(sig);
                }
                continue;
            }
            if (g_nifaces == g_cap_ifaces) {
                g_cap_ifaces = g_cap_ifaces ? g_cap_ifaces * 2 : 256;
                g_ifaces = realloc(g_ifaces, (size_t)g_cap_ifaces * sizeof *g_ifaces);
            }
            g_ifaces[g_nifaces++] = (Iface){ strdup(ver), strdup(kx->cls), strdup(sdkname), text, sig, kx->n };
        }
        fclose(fp);
    }
}

int main(int argc, char **argv)
{
    const char *sdk = NULL, *out = NULL;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a += 2) {
        if (a + 1 >= argc) die("missing value for %s\n", argv[a]);
        if (!strcmp(argv[a], "--sdk")) sdk = argv[a + 1];
        else if (!strcmp(argv[a], "--out")) out = argv[a + 1];
        else die("unknown option %s\n", argv[a]);
    }
    if (!sdk || !out || a >= argc)
        die("usage: steamgen --sdk <macOS SDK> --out <file> <steamworks_sdk dir>...\n");
    g_out = fopen(out, "w");
    if (!g_out)
        die("cannot write %s\n", out);
    fprintf(g_out, "# m32 Steam bridge interfaces, generated by tools/steamgen.sh from the Steamworks headers: "
                   "not for committing\n");
    CXIndex idx = clang_createIndex(0, 0);
    for (; a < argc; a++) {
        const char *dir = argv[a];
        const char *sdkname = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
        char **hdrs = NULL;
        int nh = header_names(dir, &hdrs);
        if (!nh)
            continue;
        Classes x86 = parse(idx, dir, "i386-apple-macos10.7", sdk, hdrs, nh);
        Classes arm = parse(idx, dir, "arm64-apple-macos11", sdk, hdrs, nh);
        versions(dir, hdrs, nh, &x86, &arm, sdkname);
    }
    clang_disposeIndex(idx);
    for (int i = 0; i < g_nifaces; i++)
        fprintf(g_out, "iface %s %s %s\n%s", g_ifaces[i].version, g_ifaces[i].cls, g_ifaces[i].sdk, g_ifaces[i].text);
    fclose(g_out);
    fprintf(stderr, "steamgen: %d interface versions\n", g_nifaces);
    return g_nifaces ? 0 : 1;
}
