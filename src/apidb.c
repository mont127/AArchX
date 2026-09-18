/*
 * Reading the API database.
 *
 * ocerz/apidb.h states the format, where the files live and how a version
 * directory is chosen; what follows is what this file knows that the header
 * does not.
 *
 * ---- refusing whole, and saying where ----
 * The parser walks the file once, in order, and stops at the first line that
 * breaks a rule, writing path:line and the rule into the caller's buffer.  Every
 * rule is checked on that walk except the ones a struct record can only be held
 * to once the whole file has been read, since a struct record may come before
 * the fn and shape records it names: those are checked after the walk, in the
 * order the struct records appear, and a failure there names the struct
 * record's own line.  Nothing is published until every rule has held, so a
 * caller never sees a library that is correct up to some line.
 *
 * A signature is held to the notation by ocerz_abi_parse itself, the parser the
 * bridge will hand it to, not by a copy of its grammar, so a signature this file
 * accepts is one the crossing can take.  A shape word is held to the stricter
 * rule a callback signature has to meet, no callback of its own and short
 * enough for the bank to store, because that is what the bridge does with it.
 * A signature the engine refuses is a broken file rather than an export to skip:
 * the generator wrote a crossing, and a crossing that silently became a stub
 * would move the failure from generation time to the middle of some program.
 *
 * ---- the root is made absolute before anything uses it ----
 * The guest runs inside this process and its chdir is the process's chdir.  A
 * library is loaded the first time something asks for it, which for a dlopen
 * can be long after the guest has changed directory, so a relative OCERZ_APIDB
 * resolved late would name a different directory from the one resolved early.
 * The root is therefore put through realpath when the version directory is
 * chosen, and every later path is built on that.
 *
 * ---- versions are numbers ----
 * A version directory's name is read as up to three decimal components and
 * packed the way Mach-O packs a minimum OS version, sixteen bits of major and
 * eight each of minor and patch, and every comparison is between packed
 * numbers.  Compared as strings, 10.13 sorts below 10.9 and 27.0 would not
 * outrank 26.6 once a 100 appeared; packed, they order the way the releases do.
 * A name that is not such a version, or that is not a directory, is ignored,
 * and two names that pack to the same number are settled by the smaller name,
 * so the choice never depends on the order the file system lists them in.
 *
 * ---- one answer per install name, including no ----
 * The loader asks whether an install name has a library for every dependency it
 * resolves an import through, so an answer of no is asked for far more often
 * than a yes and is remembered exactly like one: a list of install names with
 * the library or the absence of one, published by a single atomic store of its
 * head under the lock, and read without the lock.  A name is loaded at most
 * once, the pointer handed out for it never changes, and nothing is freed.  A
 * file whose library record names a different install name answers no for the
 * name that led to it, since two install names can share a last component.
 * A refused file is announced on stderr whatever the verbosity, because the
 * consequence - a library native mode does not synthesize - otherwise shows up
 * only as a list of imports nothing binds.
 *
 * ---- lookup by name ----
 * An export is found through an open-addressed hash of the entries' indices,
 * built while parsing, which is also what catches a name declared twice on the
 * line where the second declaration is.  The table and the source text the
 * entries' strings point into belong to a private structure the public one is
 * the first member of.
 */
#include "ocerz/apidb.h"
#include "ocerz/abi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define AD_VAR_BYTES_MAX (1u << 20)
#define AD_FIELDS_MAX (OCERZ_APIDB_SHAPE_WORDS + 8)

typedef struct AdLibrary {
    OcerzApiLibrary pub;
    char *text;
    char *path;
    uint32_t *hash;
    uint32_t hcap;
} AdLibrary;

typedef struct AdStructRec {
    int line;
    const char *export_name;
    int argpos;
    const char *shape;
} AdStructRec;

typedef struct AdParse {
    const char *path;
    char *err;
    size_t errlen;
    AdLibrary *lib;
    OcerzApiEntry *entries;
    int *entry_line;
    int nentries;
    int centries;
    OcerzApiShape *shapes;
    int *shape_line;
    int nshapes;
    int cshapes;
    AdStructRec *structs;
    int nstructs;
    int cstructs;
} AdParse;

static int ad_refuse(AdParse *p, int line, const char *fmt, ...)
{
    if (!p->err || p->errlen == 0)
        return 0;
    int n = snprintf(p->err, p->errlen, "%s:%d: ", p->path, line);
    if (n < 0 || (size_t)n >= p->errlen)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err + n, p->errlen - (size_t)n, fmt, ap);
    va_end(ap);
    return 0;
}

static uint32_t ad_hash_name(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static int ad_hash_find(const AdLibrary *lib, const OcerzApiEntry *entries, const char *name)
{
    if (!lib->hcap)
        return -1;
    uint32_t mask = lib->hcap - 1;
    for (uint32_t i = ad_hash_name(name) & mask;; i = (i + 1) & mask) {
        uint32_t slot = lib->hash[i];
        if (!slot)
            return -1;
        if (strcmp(entries[slot - 1].export_name, name) == 0)
            return (int)(slot - 1);
    }
}

static int ad_hash_grow(AdLibrary *lib, const OcerzApiEntry *entries, int n)
{
    uint32_t cap = lib->hcap ? lib->hcap : 64;
    while ((uint64_t)(n + 1) * 2 > cap)
        cap *= 2;
    if (cap == lib->hcap)
        return 1;
    uint32_t *h = calloc(cap, sizeof *h);
    if (!h)
        return 0;
    for (int k = 0; k < n; k++) {
        uint32_t i = ad_hash_name(entries[k].export_name) & (cap - 1);
        while (h[i])
            i = (i + 1) & (cap - 1);
        h[i] = (uint32_t)k + 1;
    }
    free(lib->hash);
    lib->hash = h;
    lib->hcap = cap;
    return 1;
}

static void ad_hash_put(AdLibrary *lib, const OcerzApiEntry *entries, int k)
{
    uint32_t mask = lib->hcap - 1;
    uint32_t i = ad_hash_name(entries[k].export_name) & mask;
    while (lib->hash[i])
        i = (i + 1) & mask;
    lib->hash[i] = (uint32_t)k + 1;
}

static int ad_grow(void **arr, int *cap, int want, size_t elem)
{
    if (want <= *cap)
        return 1;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < want)
        nc *= 2;
    void *na = realloc(*arr, (size_t)nc * elem);
    if (!na)
        return 0;
    *arr = na;
    *cap = nc;
    return 1;
}

static int ad_decimal(const char *s, uint64_t max, uint64_t *out)
{
    if (!s[0] || (s[0] == '0' && s[1]))
        return 0;
    uint64_t v = 0;
    for (const char *c = s; *c; c++) {
        if (*c < '0' || *c > '9')
            return 0;
        uint64_t d = (uint64_t)(*c - '0');
        if (v > (UINT64_MAX - d) / 10)
            return 0;
        v = v * 10 + d;
    }
    if (v > max)
        return 0;
    *out = v;
    return 1;
}

static int ad_version(const char *s, uint32_t *packed)
{
    uint32_t part[3] = { 0, 0, 0 };
    static const uint32_t limit[3] = { 0xffff, 0xff, 0xff };
    int n = 0;
    const char *c = s;
    for (;;) {
        if (n == 3 || *c < '0' || *c > '9')
            return 0;
        uint32_t v = 0;
        while (*c >= '0' && *c <= '9') {
            v = v * 10 + (uint32_t)(*c - '0');
            if (v > limit[n])
                return 0;
            c++;
        }
        part[n++] = v;
        if (*c == '\0')
            break;
        if (*c != '.')
            return 0;
        c++;
    }
    *packed = part[0] << 16 | part[1] << 8 | part[2];
    return 1;
}

static const char *ad_kind_name(OcerzApiKind k)
{
    switch (k) {
    case OCERZ_API_FN: return "fn";
    case OCERZ_API_DATA: return "data";
    case OCERZ_API_VAR: return "var";
    case OCERZ_API_SPECIAL: return "special";
    case OCERZ_API_STUB: return "stub";
    }
    return "unknown";
}

static int ad_callback_sig(const char *s)
{
    if (strlen(s) >= OCERZ_ABI_CB_MAX)
        return 0;
    OcerzAbiSig sig;
    if (ocerz_abi_parse(s, &sig) != OCERZ_OK)
        return 0;
    for (int i = 0; i < sig.nargs; i++)
        if (sig.arg[i] == 'c')
            return 0;
    return 1;
}

static int ad_add_entry(AdParse *p, int line, OcerzApiKind kind, const char *name,
                        OcerzApiEntry **out)
{
    int prior = ad_hash_find(p->lib, p->entries, name);
    if (prior >= 0)
        return ad_refuse(p, line, "export %s is already declared, as %s, on line %d", name,
                         ad_kind_name(p->entries[prior].kind), p->entry_line[prior]);
    if (!ad_grow((void **)&p->entries, &p->centries, p->nentries + 1, sizeof *p->entries))
        return ad_refuse(p, line, "out of memory");
    int lcap = p->centries;
    int *nl = realloc(p->entry_line, (size_t)lcap * sizeof *nl);
    if (!nl)
        return ad_refuse(p, line, "out of memory");
    p->entry_line = nl;
    if (!ad_hash_grow(p->lib, p->entries, p->nentries))
        return ad_refuse(p, line, "out of memory");
    int k = p->nentries++;
    OcerzApiEntry *e = &p->entries[k];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->export_name = name;
    p->entry_line[k] = line;
    ad_hash_put(p->lib, p->entries, k);
    *out = e;
    return 1;
}

static int ad_record(AdParse *p, int line, char **f, int nf)
{
    const char *kind = f[0];
    OcerzApiEntry *e = NULL;
    uint64_t num = 0;

    if (strcmp(kind, "fn") == 0) {
        if (nf != 4)
            return ad_refuse(p, line, "a fn record has %d fields, want 4: fn <export> <host-symbol> "
                             "<signature>", nf);
        OcerzAbiSig sig;
        if (ocerz_abi_parse(f[3], &sig) != OCERZ_OK)
            return ad_refuse(p, line, "fn %s is declared %s, which is not a signature in the "
                             "notation abi.h defines", f[1], f[3]);
        if (!ad_add_entry(p, line, OCERZ_API_FN, f[1], &e))
            return 0;
        e->host = f[2];
        e->sig = f[3];
        return 1;
    }
    if (strcmp(kind, "data") == 0) {
        if (nf != 3)
            return ad_refuse(p, line, "a data record has %d fields, want 3: data <export> "
                             "<host-symbol>", nf);
        if (!ad_add_entry(p, line, OCERZ_API_DATA, f[1], &e))
            return 0;
        e->host = f[2];
        return 1;
    }
    if (strcmp(kind, "var") == 0) {
        if (nf != 4)
            return ad_refuse(p, line, "a var record has %d fields, want 4: var <export> <bytes> "
                             "<filler>", nf);
        if (!ad_decimal(f[2], AD_VAR_BYTES_MAX, &num) || num == 0)
            return ad_refuse(p, line, "var %s is %s bytes, want a decimal count from 1 to %u", f[1],
                             f[2], (unsigned)AD_VAR_BYTES_MAX);
        if (!ad_add_entry(p, line, OCERZ_API_VAR, f[1], &e))
            return 0;
        e->bytes = (uint32_t)num;
        e->filler = f[3];
        return 1;
    }
    if (strcmp(kind, "special") == 0) {
        if (nf != 3)
            return ad_refuse(p, line, "a special record has %d fields, want 3: special <export> "
                             "<handler>", nf);
        if (!ad_add_entry(p, line, OCERZ_API_SPECIAL, f[1], &e))
            return 0;
        e->handler = f[2];
        return 1;
    }
    if (strcmp(kind, "stub") == 0) {
        if (nf != 3)
            return ad_refuse(p, line, "a stub record has %d fields, want 3: stub <export> <reason>",
                             nf);
        if (!ad_add_entry(p, line, OCERZ_API_STUB, f[1], &e))
            return 0;
        e->reason = f[2];
        return 1;
    }
    if (strcmp(kind, "shape") == 0) {
        if (nf < 4)
            return ad_refuse(p, line, "a shape record has %d fields, want at least 4: shape <name> "
                             "<version> <words> <word>...", nf);
        uint64_t version = 0;
        if (!ad_decimal(f[2], UINT64_MAX, &version))
            return ad_refuse(p, line, "shape %s has version %s, want a decimal number", f[1], f[2]);
        if (!ad_decimal(f[3], OCERZ_APIDB_SHAPE_WORDS, &num) || num == 0)
            return ad_refuse(p, line, "shape %s version %s is %s words long, want a decimal count "
                             "from 1 to %d", f[1], f[2], f[3], OCERZ_APIDB_SHAPE_WORDS);
        if ((uint64_t)(nf - 4) != num)
            return ad_refuse(p, line, "shape %s version %s declares %s words and lists %d", f[1],
                             f[2], f[3], nf - 4);
        for (int w = 0; w < (int)num; w++) {
            const char *word = f[4 + w];
            if (strcmp(word, "-") != 0 && !ad_callback_sig(word))
                return ad_refuse(p, line, "word %d of shape %s version %s is %s, which is neither - "
                                 "nor a signature abi.h can call back through", w, f[1], f[2],
                                 word);
        }
        for (int s = 0; s < p->nshapes; s++)
            if (p->shapes[s].version == version && strcmp(p->shapes[s].name, f[1]) == 0)
                return ad_refuse(p, line, "shape %s version %s is already declared on line %d",
                                 f[1], f[2], p->shape_line[s]);
        if (!ad_grow((void **)&p->shapes, &p->cshapes, p->nshapes + 1, sizeof *p->shapes))
            return ad_refuse(p, line, "out of memory");
        int *nl = realloc(p->shape_line, (size_t)p->cshapes * sizeof *nl);
        if (!nl)
            return ad_refuse(p, line, "out of memory");
        p->shape_line = nl;
        OcerzApiShape *sh = &p->shapes[p->nshapes];
        memset(sh, 0, sizeof *sh);
        sh->name = f[1];
        sh->version = version;
        sh->words = (int)num;
        for (int w = 0; w < (int)num; w++)
            sh->word[w] = strcmp(f[4 + w], "-") == 0 ? NULL : f[4 + w];
        p->shape_line[p->nshapes++] = line;
        return 1;
    }
    if (strcmp(kind, "struct") == 0) {
        if (nf != 4)
            return ad_refuse(p, line, "a struct record has %d fields, want 4: struct <export> "
                             "<argpos> <shape-name>", nf);
        if (!ad_decimal(f[2], OCERZ_ABI_MAX_ARGS - 1, &num))
            return ad_refuse(p, line, "struct %s names argument %s, want a decimal position from 0 "
                             "to %d", f[1], f[2], OCERZ_ABI_MAX_ARGS - 1);
        if (!ad_grow((void **)&p->structs, &p->cstructs, p->nstructs + 1, sizeof *p->structs))
            return ad_refuse(p, line, "out of memory");
        AdStructRec *r = &p->structs[p->nstructs++];
        r->line = line;
        r->export_name = f[1];
        r->argpos = (int)num;
        r->shape = f[3];
        return 1;
    }
    if (strcmp(kind, "ocerz-apidb") == 0 || strcmp(kind, "library") == 0 ||
        strcmp(kind, "sdk") == 0)
        return ad_refuse(p, line, "a %s record may appear only once, in the header", kind);
    return ad_refuse(p, line, "%s is not a record kind; want fn, data, var, special, stub, shape or "
                     "struct", kind);
}

static int ad_resolve_structs(AdParse *p)
{
    for (int i = 0; i < p->nstructs; i++) {
        AdStructRec *r = &p->structs[i];
        int k = ad_hash_find(p->lib, p->entries, r->export_name);
        if (k < 0)
            return ad_refuse(p, r->line, "struct names %s, which no fn record declares",
                             r->export_name);
        OcerzApiEntry *e = &p->entries[k];
        if (e->kind != OCERZ_API_FN)
            return ad_refuse(p, r->line, "struct names %s, which is a %s record on line %d, not a "
                             "fn record", r->export_name, ad_kind_name(e->kind),
                             p->entry_line[k]);
        int shaped = 0;
        for (int s = 0; s < p->nshapes && !shaped; s++)
            shaped = strcmp(p->shapes[s].name, r->shape) == 0;
        if (!shaped)
            return ad_refuse(p, r->line, "struct names shape %s, which no shape record declares",
                             r->shape);
        OcerzAbiSig sig;
        if (ocerz_abi_parse(e->sig, &sig) != OCERZ_OK || r->argpos >= sig.nargs ||
            sig.arg[r->argpos] != 'p')
            return ad_refuse(p, r->line, "struct binds argument %d of %s, which its signature %s "
                             "does not declare as a pointer", r->argpos, r->export_name, e->sig);
        for (int j = 0; j < i; j++) {
            AdStructRec *q = &p->structs[j];
            if (q->argpos == r->argpos && strcmp(q->export_name, r->export_name) == 0)
                return ad_refuse(p, r->line, "argument %d of %s is already bound to a shape on "
                                 "line %d", r->argpos, r->export_name, q->line);
        }
        if (e->nstructs >= OCERZ_APIDB_STRUCT_ARGS)
            return ad_refuse(p, r->line, "%s already has the %d struct records one fn may have",
                             r->export_name, OCERZ_APIDB_STRUCT_ARGS);
        e->structs[e->nstructs].argpos = r->argpos;
        e->structs[e->nstructs].shape = r->shape;
        e->nstructs++;
    }
    return 1;
}

static void ad_free_parse(AdParse *p)
{
    free(p->entries);
    free(p->entry_line);
    free(p->shapes);
    free(p->shape_line);
    free(p->structs);
    if (p->lib) {
        free(p->lib->hash);
        free(p->lib->text);
        free(p->lib->path);
        free(p->lib);
    }
}

const OcerzApiLibrary *ocerz_apidb_parse(const char *path, const char *text, size_t len,
                                         char *err, size_t errlen)
{
    AdParse p;
    memset(&p, 0, sizeof p);
    p.path = path ? path : "(unnamed)";
    p.err = err;
    p.errlen = errlen;
    if (err && errlen)
        err[0] = '\0';

    p.lib = calloc(1, sizeof *p.lib);
    char *buf = malloc(len + 1);
    char *pathcopy = strdup(p.path);
    if (!p.lib || !buf || !pathcopy || (!text && len)) {
        free(buf);
        free(pathcopy);
        ad_refuse(&p, 0, "%s", text || !len ? "out of memory" : "no text");
        ad_free_parse(&p);
        return NULL;
    }
    if (len)
        memcpy(buf, text, len);
    buf[len] = '\0';
    p.lib->text = buf;
    p.lib->path = pathcopy;

    int stage = 0;
    int line = 0;
    size_t pos = 0;
    while (pos < len) {
        line++;
        char *s = buf + pos;
        char *nl = memchr(s, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - s) : len - pos;
        pos += ll + (nl ? 1 : 0);
        if (memchr(s, '\0', ll)) {
            ad_refuse(&p, line, "the line contains a NUL byte");
            goto refuse;
        }
        s[ll] = '\0';
        if (ll == 0 || s[0] == '#')
            continue;
        for (size_t i = 0; i < ll; i++) {
            uint8_t c = (uint8_t)s[i];
            if (c < 0x20 || c == 0x7f) {
                ad_refuse(&p, line, "the line contains the control character 0x%02x; fields are "
                          "separated by single spaces", c);
                goto refuse;
            }
        }
        if (s[0] == ' ') {
            ad_refuse(&p, line, "the line starts with a space");
            goto refuse;
        }
        if (s[ll - 1] == ' ') {
            ad_refuse(&p, line, "the line ends with a space, and nothing may follow the last field");
            goto refuse;
        }
        if (strstr(s, "  ")) {
            ad_refuse(&p, line, "two fields are separated by more than one space");
            goto refuse;
        }

        char *f[AD_FIELDS_MAX];
        int nf = 0;
        for (char *c = s;;) {
            if (nf < AD_FIELDS_MAX)
                f[nf] = c;
            nf++;
            char *sp = strchr(c, ' ');
            if (!sp)
                break;
            *sp = '\0';
            c = sp + 1;
        }
        if (nf > AD_FIELDS_MAX) {
            if (strcmp(f[0], "shape") == 0)
                ad_refuse(&p, line, "a shape record has %d fields, and a shape may have at most %d "
                          "words", nf, OCERZ_APIDB_SHAPE_WORDS);
            else
                ad_refuse(&p, line, "a %s record has %d fields, far more than any record has",
                          f[0], nf);
            goto refuse;
        }

        if (stage == 0) {
            if (strcmp(f[0], "ocerz-apidb") != 0 || nf != 2) {
                ad_refuse(&p, line, "the first record is not the header 'ocerz-apidb 1'");
                goto refuse;
            }
            if (strcmp(f[1], "1") != 0) {
                ad_refuse(&p, line, "the header declares format %s, and this ocerz reads format 1",
                          f[1]);
                goto refuse;
            }
            stage = 1;
        } else if (stage == 1) {
            if (strcmp(f[0], "library") != 0 || nf != 2) {
                ad_refuse(&p, line, "the second record is not 'library <install-name>'");
                goto refuse;
            }
            p.lib->pub.install_name = f[1];
            stage = 2;
        } else if (stage == 2) {
            uint32_t packed = 0;
            if (strcmp(f[0], "sdk") != 0 || nf != 3 || strcmp(f[1], "macos") != 0) {
                ad_refuse(&p, line, "the third record is not 'sdk macos <version>'");
                goto refuse;
            }
            if (!ad_version(f[2], &packed)) {
                ad_refuse(&p, line, "the sdk version %s is not X, X.Y or X.Y.Z in decimal", f[2]);
                goto refuse;
            }
            p.lib->pub.sdk_version = f[2];
            stage = 3;
        } else if (!ad_record(&p, line, f, nf)) {
            goto refuse;
        }
    }
    if (stage < 3) {
        ad_refuse(&p, line > 0 ? line : 1, "the file ends before its %s record",
                  stage == 0 ? "ocerz-apidb header" : stage == 1 ? "library" : "sdk");
        goto refuse;
    }
    if (!ad_resolve_structs(&p))
        goto refuse;

    p.lib->pub.path = p.lib->path;
    p.lib->pub.entries = p.entries;
    p.lib->pub.nentries = p.nentries;
    p.lib->pub.shapes = p.shapes;
    p.lib->pub.nshapes = p.nshapes;
    free(p.entry_line);
    free(p.shape_line);
    free(p.structs);
    return &p.lib->pub;

refuse:
    ad_free_parse(&p);
    return NULL;
}

const OcerzApiEntry *ocerz_apidb_find(const OcerzApiLibrary *lib, const char *export_name)
{
    if (!lib || !export_name)
        return NULL;
    const AdLibrary *al = (const AdLibrary *)lib;
    int k = ad_hash_find(al, lib->entries, export_name);
    return k < 0 ? NULL : &lib->entries[k];
}

const OcerzApiShape *ocerz_apidb_shape(const OcerzApiLibrary *lib, const char *name,
                                       uint64_t version)
{
    if (!lib || !name)
        return NULL;
    for (int i = 0; i < lib->nshapes; i++)
        if (lib->shapes[i].version == version && strcmp(lib->shapes[i].name, name) == 0)
            return &lib->shapes[i];
    return NULL;
}

typedef struct AdSlot {
    char *install_name;
    const OcerzApiLibrary *lib;
    struct AdSlot *next;
} AdSlot;

static pthread_mutex_t g_ad_lock = PTHREAD_MUTEX_INITIALIZER;
static AdSlot *_Atomic g_ad_slots;
static _Atomic int g_ad_chosen;
static uint32_t g_ad_minos;
static char g_ad_dir[PATH_MAX];

void ocerz_apidb_postfork_child(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    g_ad_lock = fresh;
}

static int ad_default_root(char *out, size_t outlen)
{
    char small[PATH_MAX];
    char *exe = small;
    uint32_t sz = sizeof small;
    if (_NSGetExecutablePath(exe, &sz) != 0) {
        exe = malloc(sz);
        if (!exe || _NSGetExecutablePath(exe, &sz) != 0) {
            free(exe);
            return 0;
        }
    }
    char real[PATH_MAX];
    int ok = realpath(exe, real) != NULL;
    if (exe != small)
        free(exe);
    if (!ok)
        return 0;
    char *slash = strrchr(real, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    int n = snprintf(out, outlen, "%s/runtime/apis", real[0] ? real : "/");
    return n > 0 && (size_t)n < outlen;
}

static void ad_choose_locked(void)
{
    if (g_ad_chosen)
        return;
    g_ad_dir[0] = '\0';

    char root[PATH_MAX];
    const char *env = getenv("OCERZ_APIDB");
    int have_root;
    if (env && env[0]) {
        int n = snprintf(root, sizeof root, "%s", env);
        have_root = n > 0 && (size_t)n < sizeof root;
    } else {
        have_root = ad_default_root(root, sizeof root);
    }

    char real_root[PATH_MAX];
    char macos[PATH_MAX];
    DIR *d = NULL;
    if (have_root && realpath(root, real_root)) {
        int n = snprintf(macos, sizeof macos, "%s/macos", real_root);
        if (n > 0 && (size_t)n < sizeof macos)
            d = opendir(macos);
    }
    if (!d) {
        OCERZ_LOG("apidb: no API database at %s/macos\n", have_root ? root : "(no root)");
        g_ad_chosen = 1;
        return;
    }

    uint32_t minos = g_ad_minos;
    char best[256] = "";
    uint32_t best_v = 0;
    int have_best = 0;
    char oldest[256] = "";
    uint32_t oldest_v = 0;
    int have_oldest = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint32_t v = 0;
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof best || !ad_version(de->d_name, &v))
            continue;
        char full[PATH_MAX];
        struct stat st;
        int n = snprintf(full, sizeof full, "%s/%s", macos, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof full || stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (!have_oldest || v < oldest_v || (v == oldest_v && strcmp(de->d_name, oldest) < 0)) {
            snprintf(oldest, sizeof oldest, "%s", de->d_name);
            oldest_v = v;
            have_oldest = 1;
        }
        if (minos && v > minos)
            continue;
        if (!have_best || v > best_v || (v == best_v && strcmp(de->d_name, best) < 0)) {
            snprintf(best, sizeof best, "%s", de->d_name);
            best_v = v;
            have_best = 1;
        }
    }
    closedir(d);

    const char *pick = have_best ? best : have_oldest ? oldest : NULL;
    if (pick) {
        int n = snprintf(g_ad_dir, sizeof g_ad_dir, "%s/%s", macos, pick);
        if (n <= 0 || (size_t)n >= sizeof g_ad_dir)
            g_ad_dir[0] = '\0';
    }
    if (g_ad_dir[0])
        OCERZ_LOG("apidb: using %s for a guest declaring macOS %u.%u.%u\n", g_ad_dir,
                  minos >> 16, (minos >> 8) & 0xff, minos & 0xff);
    else
        OCERZ_LOG("apidb: %s holds no version directory\n", macos);
    g_ad_chosen = 1;
}

const char *ocerz_apidb_dir(void)
{
    if (!g_ad_chosen) {
        pthread_mutex_lock(&g_ad_lock);
        ad_choose_locked();
        pthread_mutex_unlock(&g_ad_lock);
    }
    return g_ad_dir[0] ? g_ad_dir : NULL;
}

void ocerz_apidb_set_minos(uint32_t minos)
{
    pthread_mutex_lock(&g_ad_lock);
    if (!g_ad_chosen)
        g_ad_minos = minos;
    else if (minos != g_ad_minos)
        OCERZ_LOG("apidb: minimum macOS %#x arrived after the version directory was chosen, which "
                  "stands\n", minos);
    pthread_mutex_unlock(&g_ad_lock);
}

static char *ad_read_file(const char *path, size_t *len_out, int *missing)
{
    *missing = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *missing = errno == ENOENT || errno == ENOTDIR;
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    size_t cap = (size_t)st.st_size + 1, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            free(buf);
            buf = NULL;
            break;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    close(fd);
    *len_out = len;
    return buf;
}

static const OcerzApiLibrary *ad_load_locked(const char *install_name)
{
    ad_choose_locked();
    if (!g_ad_dir[0])
        return NULL;
    const char *leaf = strrchr(install_name, '/');
    leaf = leaf ? leaf + 1 : install_name;
    if (!leaf[0])
        return NULL;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s.api", g_ad_dir, leaf);
    if (n <= 0 || (size_t)n >= sizeof path)
        return NULL;

    size_t len = 0;
    int missing = 0;
    char *text = ad_read_file(path, &len, &missing);
    if (!text) {
        if (!missing)
            fprintf(stderr, "ocerz: apidb: cannot read %s\n", path);
        return NULL;
    }
    char err[512];
    const OcerzApiLibrary *lib = ocerz_apidb_parse(path, text, len, err, sizeof err);
    free(text);
    if (!lib) {
        fprintf(stderr, "ocerz: apidb: refusing %s\n", err);
        return NULL;
    }
    if (strcmp(lib->install_name, install_name) != 0) {
        OCERZ_LOG("apidb: %s describes %s, not %s\n", path, lib->install_name, install_name);
        AdLibrary *al = (AdLibrary *)lib;
        free(al->hash);
        free(al->text);
        free(al->path);
        free((void *)lib->entries);
        free((void *)lib->shapes);
        free(al);
        return NULL;
    }
    OCERZ_LOG("apidb: loaded %s, %d exports and %d shapes\n", path, lib->nentries, lib->nshapes);
    return lib;
}

static AdSlot *ad_find_slot(const char *install_name)
{
    for (AdSlot *s = g_ad_slots; s; s = s->next)
        if (strcmp(s->install_name, install_name) == 0)
            return s;
    return NULL;
}

const OcerzApiLibrary *ocerz_apidb_library(const char *install_name)
{
    if (!install_name || !install_name[0])
        return NULL;
    AdSlot *s = ad_find_slot(install_name);
    if (s)
        return s->lib;

    pthread_mutex_lock(&g_ad_lock);
    s = ad_find_slot(install_name);
    if (!s) {
        const OcerzApiLibrary *lib = ad_load_locked(install_name);
        s = calloc(1, sizeof *s);
        char *name = strdup(install_name);
        if (s && name) {
            s->install_name = name;
            s->lib = lib;
            s->next = g_ad_slots;
            g_ad_slots = s;
        } else {
            free(s);
            free(name);
            pthread_mutex_unlock(&g_ad_lock);
            return lib;
        }
    }
    pthread_mutex_unlock(&g_ad_lock);
    return s->lib;
}
