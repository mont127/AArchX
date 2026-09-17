/*
 * The API database reader, checked against files written for each rule.
 *
 * The first half drives ocerz_apidb_parse directly.  One file carries every
 * record kind, comments and blank lines between them, a shape in two versions,
 * one struct record ahead of the fn and shape records it names and one after
 * them, and a final line with no newline, and every field of every record is
 * read back.  Then every rule the header states is broken in a file of its
 * own that is otherwise valid, and each refusal is asserted to be NULL and to
 * say exactly path:line: and the rule, so a parser that refuses for the wrong
 * reason, or names the wrong line, fails as surely as one that accepts.  The
 * committed interim database is parsed the same way, so a hand edit that
 * breaks a rule is caught here rather than as a guest that binds nothing.
 *
 * The second half is the part that happens once per process: the choice of
 * root and version directory, and the per-install-name cache.  Each scenario
 * therefore runs in a forked child, which does its own checks and sends its
 * counts back through a pipe, so one scenario's choice cannot leak into the
 * next.  The directory holds 10.9, 10.10, 10.13, 10.13.5, 26.6 and 27.0, next
 * to names that must be ignored - a README, a file named 28.0, a directory
 * named latest, a hidden directory, and 10.256 and 70000.0, which only a
 * reader that lets a component overflow its packed field would take for
 * versions - and each version directory's file says in its sdk record which
 * directory it came from.  10.10 is there because it sorts below 10.9 as a
 * string and above it as a version.  Every minimum OS is packed the way
 * Mach-O packs it, and the cases cover no declaration, an exact match, a patch
 * level between two directories, one older than every directory, and ones
 * newer than every directory.
 *
 * The default root, runtime/apis beside the executable, is checked by copying
 * this binary into a temporary directory that has such a root beside it and
 * running the copy, since the location of the running executable is the one
 * input a test cannot otherwise choose.
 */
#include "ocerz/apidb.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

#define HEAD "ocerz-apidb 1\nlibrary /usr/lib/libTest.dylib\nsdk macos 27.0\n"

static const OcerzApiLibrary *parse(const char *text, char *err, size_t errlen)
{
    return ocerz_apidb_parse("t.api", text, strlen(text), err, errlen);
}

static void test_records(void)
{
    static const char text[] =
        "# a comment before the header\n"
        "\n"
        "ocerz-apidb 1\n"
        "# between header records\n"
        "library /System/Library/Frameworks/Test.framework/Versions/A/Test\n"
        "sdk macos 26.6.1\n"
        "struct _MakeThing 2 ThingCallBacks\n"
        "fn _MakeThing MakeThing p(plpp)\n"
        "\n"
        "data _kThingDefault kThingDefault\n"
        "var ___guard 8 stack_guard\n"
        "special _exit exit\n"
        "stub _printf variadic\n"
        "stub dyld_stub_binder override\n"
        "shape ThingCallBacks 0 3 - p(p) v(p)\n"
        "shape ThingCallBacks 1 4 - p(p) v(p) B(pp)\n"
        "fn _Other Other d(dp)\n"
        "struct _MakeThing 3 ThingCallBacks\n"
        "struct _Other 1 ThingCallBacks";

    char err[256] = "untouched";
    const OcerzApiLibrary *lib = parse(text, err, sizeof err);
    CHECK(lib != NULL, "the every-kind file was refused: %s", err);
    CHECK(err[0] == '\0', "a successful parse left \"%s\" in the error buffer", err);
    if (!lib)
        return;

    CHECK(strcmp(lib->path, "t.api") == 0, "path is %s", lib->path);
    CHECK(strcmp(lib->install_name, "/System/Library/Frameworks/Test.framework/Versions/A/Test") == 0,
          "install name is %s", lib->install_name);
    CHECK(strcmp(lib->sdk_version, "26.6.1") == 0, "sdk version is %s", lib->sdk_version);
    CHECK(lib->nentries == 7, "%d entries, want 7", lib->nentries);
    CHECK(lib->nshapes == 2, "%d shapes, want 2", lib->nshapes);
    if (lib->nentries != 7 || lib->nshapes != 2)
        return;

    static const char *const order[] = {
        "_MakeThing", "_kThingDefault", "___guard", "_exit", "_printf", "dyld_stub_binder", "_Other",
    };
    for (int i = 0; i < 7; i++) {
        CHECK(strcmp(lib->entries[i].export_name, order[i]) == 0,
              "entry %d is %s, want %s: entries are not in file order", i,
              lib->entries[i].export_name, order[i]);
        CHECK(ocerz_apidb_find(lib, order[i]) == &lib->entries[i],
              "ocerz_apidb_find(%s) does not return entry %d", order[i], i);
    }

    const OcerzApiEntry *e = ocerz_apidb_find(lib, "_MakeThing");
    CHECK(e && e->kind == OCERZ_API_FN && strcmp(e->host, "MakeThing") == 0 &&
          strcmp(e->sig, "p(plpp)") == 0 && !e->handler && !e->filler && !e->reason && e->bytes == 0,
          "_MakeThing is not fn MakeThing p(plpp) with nothing else set");
    CHECK(e && e->nstructs == 2 && e->structs[0].argpos == 2 &&
          strcmp(e->structs[0].shape, "ThingCallBacks") == 0 && e->structs[1].argpos == 3 &&
          strcmp(e->structs[1].shape, "ThingCallBacks") == 0,
          "_MakeThing's struct records, one before it and one after, are not bound in order");

    e = ocerz_apidb_find(lib, "_kThingDefault");
    CHECK(e && e->kind == OCERZ_API_DATA && strcmp(e->host, "kThingDefault") == 0 && !e->sig &&
          e->nstructs == 0, "_kThingDefault is not data kThingDefault");
    e = ocerz_apidb_find(lib, "___guard");
    CHECK(e && e->kind == OCERZ_API_VAR && e->bytes == 8 && strcmp(e->filler, "stack_guard") == 0 &&
          !e->host, "___guard is not var 8 stack_guard");
    e = ocerz_apidb_find(lib, "_exit");
    CHECK(e && e->kind == OCERZ_API_SPECIAL && strcmp(e->handler, "exit") == 0 && !e->host,
          "_exit is not special exit");
    e = ocerz_apidb_find(lib, "_printf");
    CHECK(e && e->kind == OCERZ_API_STUB && strcmp(e->reason, "variadic") == 0,
          "_printf is not stub variadic");
    e = ocerz_apidb_find(lib, "dyld_stub_binder");
    CHECK(e && e->kind == OCERZ_API_STUB && strcmp(e->reason, "override") == 0,
          "dyld_stub_binder, with no underscore, is not stub override");
    e = ocerz_apidb_find(lib, "_Other");
    CHECK(e && e->kind == OCERZ_API_FN && e->nstructs == 1 && e->structs[0].argpos == 1,
          "_Other's struct record after the shapes is not bound");

    const OcerzApiShape *s0 = ocerz_apidb_shape(lib, "ThingCallBacks", 0);
    const OcerzApiShape *s1 = ocerz_apidb_shape(lib, "ThingCallBacks", 1);
    CHECK(s0 && s0->version == 0 && s0->words == 3 && s0->word[0] == NULL &&
          strcmp(s0->word[1], "p(p)") == 0 && strcmp(s0->word[2], "v(p)") == 0,
          "ThingCallBacks version 0 is not - p(p) v(p)");
    CHECK(s1 && s1->version == 1 && s1->words == 4 && s1->word[0] == NULL &&
          strcmp(s1->word[3], "B(pp)") == 0, "ThingCallBacks version 1 is not - p(p) v(p) B(pp)");
    CHECK(s0 != s1, "both versions of ThingCallBacks are the same record");
    CHECK(ocerz_apidb_shape(lib, "ThingCallBacks", 2) == NULL, "a version nobody declared resolved");
    CHECK(ocerz_apidb_shape(lib, "NoSuchShape", 0) == NULL, "a shape nobody declared resolved");
    CHECK(ocerz_apidb_shape(NULL, "ThingCallBacks", 0) == NULL, "a shape resolved in no library");

    CHECK(ocerz_apidb_find(lib, "_NoSuchExport") == NULL, "an export nobody declared resolved");
    CHECK(ocerz_apidb_find(lib, "_MakeThin") == NULL, "a prefix of an export resolved");
    CHECK(ocerz_apidb_find(lib, "MakeThing") == NULL, "a host symbol resolved as an export");
    CHECK(ocerz_apidb_find(NULL, "_exit") == NULL, "an export resolved in no library");
    CHECK(ocerz_apidb_find(lib, NULL) == NULL, "a null export name resolved");

    static const char big_shape[] =
        HEAD "shape Wide 7 16 - - - - - - - - - - - - - - - v()\n";
    const OcerzApiLibrary *wide = parse(big_shape, err, sizeof err);
    CHECK(wide && wide->nshapes == 1 && wide->shapes[0].words == OCERZ_APIDB_SHAPE_WORDS &&
          wide->shapes[0].version == 7 && strcmp(wide->shapes[0].word[15], "v()") == 0,
          "a shape of exactly %d words was not read: %s", OCERZ_APIDB_SHAPE_WORDS, err);

    static const char empty_body[] = HEAD;
    const OcerzApiLibrary *none = parse(empty_body, err, sizeof err);
    CHECK(none && none->nentries == 0 && none->nshapes == 0,
          "a file with a header and no records was refused: %s", err);
}

typedef struct Refusal {
    const char *text;
    int line;
    const char *rule;
} Refusal;

static const Refusal kRefusals[] = {
    { HEAD "stub\t_a x\n", 4, "the line contains the control character 0x09" },
    { HEAD "stub _a x\r\n", 4, "the line contains the control character 0x0d" },
    { HEAD " stub _a x\n", 4, "the line starts with a space" },
    { HEAD "stub _a x \n", 4, "the line ends with a space, and nothing may follow the last field" },
    { HEAD "stub  _a x\n", 4, "two fields are separated by more than one space" },
    { "library /usr/lib/libTest.dylib\n", 1, "the first record is not the header 'ocerz-apidb 1'" },
    { "# c\n\nocerz-apidb 2\n", 3, "the header declares format 2, and this ocerz reads format 1" },
    { "ocerz-apidb 1 extra\n", 1, "the first record is not the header 'ocerz-apidb 1'" },
    { "ocerz-apidb 1\nsdk macos 27.0\n", 2, "the second record is not 'library <install-name>'" },
    { "ocerz-apidb 1\nlibrary a b\n", 2, "the second record is not 'library <install-name>'" },
    { "ocerz-apidb 1\nlibrary /x\nsdk ios 27.0\n", 3, "the third record is not 'sdk macos <version>'" },
    { "ocerz-apidb 1\nlibrary /x\nstub _a x\n", 3, "the third record is not 'sdk macos <version>'" },
    { "ocerz-apidb 1\nlibrary /x\nsdk macos 27.x\n", 3,
      "the sdk version 27.x is not X, X.Y or X.Y.Z in decimal" },
    { "ocerz-apidb 1\nlibrary /x\nsdk macos 1.2.3.4\n", 3,
      "the sdk version 1.2.3.4 is not X, X.Y or X.Y.Z in decimal" },
    { "ocerz-apidb 1\nlibrary /x\nsdk macos 10.256\n", 3,
      "the sdk version 10.256 is not X, X.Y or X.Y.Z in decimal" },
    { "", 1, "the file ends before its ocerz-apidb header record" },
    { "# only a comment\n", 1, "the file ends before its ocerz-apidb header record" },
    { "ocerz-apidb 1\n", 1, "the file ends before its library record" },
    { "ocerz-apidb 1\nlibrary /x\n\n", 3, "the file ends before its sdk record" },
    { HEAD "fn _a a\n", 4, "a fn record has 3 fields, want 4: fn <export> <host-symbol> <signature>" },
    { HEAD "fn _a a i(p) x\n", 4, "a fn record has 5 fields, want 4" },
    { HEAD "fn _a a x(p)\n", 4, "fn _a is declared x(p), which is not a signature in the notation abi.h defines" },
    { HEAD "fn _a a v(s)\n", 4, "fn _a is declared v(s), which is not a signature" },
    { HEAD "fn _a a i(p\n", 4, "fn _a is declared i(p, which is not a signature" },
    { HEAD "fn _a a v(pppppppppppppppppp)\n", 4, "fn _a is declared v(pppppppppppppppppp), which is not" },
    { HEAD "data _a\n", 4, "a data record has 2 fields, want 3: data <export> <host-symbol>" },
    { HEAD "var _a 8\n", 4, "a var record has 3 fields, want 4: var <export> <bytes> <filler>" },
    { HEAD "var _a 0 stack_guard\n", 4, "var _a is 0 bytes, want a decimal count from 1 to 1048576" },
    { HEAD "var _a 8b stack_guard\n", 4, "var _a is 8b bytes, want a decimal count" },
    { HEAD "var _a 08 stack_guard\n", 4, "var _a is 08 bytes, want a decimal count" },
    { HEAD "var _a 1048577 stack_guard\n", 4, "var _a is 1048577 bytes, want a decimal count" },
    { HEAD "special _a\n", 4, "a special record has 2 fields, want 3: special <export> <handler>" },
    { HEAD "stub _a\n", 4, "a stub record has 2 fields, want 3: stub <export> <reason>" },
    { HEAD "stub _a two words\n", 4, "a stub record has 4 fields, want 3" },
    { HEAD "shape S 0\n", 4, "a shape record has 3 fields, want at least 4" },
    { HEAD "shape S v0 1 -\n", 4, "shape S has version v0, want a decimal number" },
    { HEAD "shape S 0 0\n", 4, "shape S version 0 is 0 words long, want a decimal count from 1 to 16" },
    { HEAD "shape S 0 17 - - - - - - - - - - - - - - - - -\n", 4,
      "shape S version 0 is 17 words long, want a decimal count from 1 to 16" },
    { HEAD "shape S 0 3 - -\n", 4, "shape S version 0 declares 3 words and lists 2" },
    { HEAD "shape S 0 1 - -\n", 4, "shape S version 0 declares 1 words and lists 2" },
    { HEAD "shape S 0 30 - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n", 4,
      "a shape record has 34 fields, and a shape may have at most 16 words" },
    { HEAD "shape S 0 2 - q(p)\n", 4,
      "word 1 of shape S version 0 is q(p), which is neither - nor a signature abi.h can call back through" },
    { HEAD "shape S 0 1 v(c{v()})\n", 4, "word 0 of shape S version 0 is v(c{v()}), which is neither" },
    { HEAD "shape S 0 1 v(pppppppppppppppppppppp)\n", 4,
      "word 0 of shape S version 0 is v(pppppppppppppppppppppp), which is neither" },
    { HEAD "shape S 1 1 -\nshape T 1 1 -\nshape S 1 2 - -\n", 6,
      "shape S version 1 is already declared on line 4" },
    { HEAD "struct _a 1\n", 4, "a struct record has 3 fields, want 4: struct <export> <argpos> <shape-name>" },
    { HEAD "struct _a x S\n", 4, "struct _a names argument x, want a decimal position from 0 to 15" },
    { HEAD "struct _a 16 S\n", 4, "struct _a names argument 16, want a decimal position from 0 to 15" },
    { HEAD "struct _a 0 S\nshape S 0 1 -\n", 4, "struct names _a, which no fn record declares" },
    { HEAD "shape S 0 1 -\nstub _a x\nstruct _a 0 S\n", 6,
      "struct names _a, which is a stub record on line 5, not a fn record" },
    { HEAD "struct _a 0 S\nfn _a a v(p)\n", 4, "struct names shape S, which no shape record declares" },
    { HEAD "fn _a a v(ip)\nshape S 0 1 -\nstruct _a 0 S\n", 6,
      "struct binds argument 0 of _a, which its signature v(ip) does not declare as a pointer" },
    { HEAD "fn _a a v(ip)\nshape S 0 1 -\nstruct _a 2 S\n", 6,
      "struct binds argument 2 of _a, which its signature v(ip) does not declare as a pointer" },
    { HEAD "struct _a 1 S\nfn _a a v(pp)\nshape S 0 1 -\nstruct _a 1 S\n", 7,
      "argument 1 of _a is already bound to a shape on line 4" },
    { HEAD "fn _a a v(ppp)\nshape S 0 1 -\nstruct _a 0 S\nstruct _a 1 S\nstruct _a 2 S\n", 8,
      "_a already has the 2 struct records one fn may have" },
    { HEAD "fn _a a v()\n\n# c\nstub _a x\n", 7, "export _a is already declared, as fn, on line 4" },
    { HEAD "data _a a\nvar _a 8 stack_guard\n", 5, "export _a is already declared, as data, on line 4" },
    { HEAD "stub _a x\nstub _a y\n", 5, "export _a is already declared, as stub, on line 4" },
    { HEAD "special _a exit\nfn _a a v()\n", 5, "export _a is already declared, as special, on line 4" },
    { HEAD "func _a a v()\n", 4, "func is not a record kind; want fn, data, var, special, stub, shape or struct" },
    { HEAD "library /y\n", 4, "a library record may appear only once, in the header" },
    { HEAD "sdk macos 26.0\n", 4, "a sdk record may appear only once, in the header" },
    { HEAD "ocerz-apidb 1\n", 4, "a ocerz-apidb record may appear only once, in the header" },
};

static void test_refusals(void)
{
    for (size_t i = 0; i < sizeof kRefusals / sizeof kRefusals[0]; i++) {
        const Refusal *r = &kRefusals[i];
        char err[512] = "";
        const OcerzApiLibrary *lib = ocerz_apidb_parse("dir/f.api", r->text, strlen(r->text), err,
                                                       sizeof err);
        char want[64];
        snprintf(want, sizeof want, "dir/f.api:%d: ", r->line);
        CHECK(lib == NULL, "case %zu (%s) was accepted", i, r->rule);
        CHECK(strncmp(err, want, strlen(want)) == 0 && strstr(err + strlen(want), r->rule) == err + strlen(want),
              "case %zu: refusal reads \"%s\", want \"%s%s...\"", i, err, want, r->rule);
    }

    static const char nul[] = HEAD "stub _a\0b x\n";
    char nerr[256] = "";
    CHECK(ocerz_apidb_parse("dir/f.api", nul, sizeof nul - 1, nerr, sizeof nerr) == NULL,
          "a line with a NUL byte in it was accepted");
    CHECK(strcmp(nerr, "dir/f.api:4: the line contains a NUL byte") == 0,
          "a NUL byte was refused as \"%s\"", nerr);

    char small[12];
    memset(small, 'x', sizeof small);
    const OcerzApiLibrary *lib = ocerz_apidb_parse("a-rather-long-path.api", "junk\n", 5, small, 8);
    CHECK(lib == NULL, "junk was accepted");
    CHECK(small[7] == '\0' && small[8] == 'x', "a refusal overran an 8-byte error buffer");
    CHECK(ocerz_apidb_parse("p", "junk\n", 5, NULL, 0) == NULL, "junk was accepted with no buffer");
    CHECK(ocerz_apidb_parse("p", NULL, 5, small, sizeof small) == NULL, "no text was accepted");
}

static char *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    size_t r;
    while (buf && (r = fread(buf + len, 1, cap - len, f)) > 0) {
        len += r;
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
    }
    fclose(f);
    *len_out = len;
    return buf;
}

static void test_committed(void)
{
    static const char *const files[][2] = {
        { "runtime/apis/macos/27.0/libSystem.B.dylib.api", "/usr/lib/libSystem.B.dylib" },
        { "runtime/apis/macos/27.0/CoreFoundation.api",
          "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation" },
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        size_t len = 0;
        char *text = slurp(files[i][0], &len);
        CHECK(text != NULL, "cannot read %s (run from the repository root)", files[i][0]);
        if (!text)
            continue;
        char err[512] = "";
        const OcerzApiLibrary *lib = ocerz_apidb_parse(files[i][0], text, len, err, sizeof err);
        CHECK(lib != NULL, "the committed %s is refused: %s", files[i][0], err);
        CHECK(!lib || strcmp(lib->install_name, files[i][1]) == 0, "%s describes %s", files[i][0],
              lib ? lib->install_name : "");
        CHECK(!lib || lib->nentries > 0, "%s declares nothing", files[i][0]);
        free(text);
    }
}

typedef struct Counts {
    int checks;
    int failures;
    char note[256];
} Counts;

static char g_root[PATH_MAX];
static char g_note[256];

static void in_child(const char *what, void (*fn)(void *), void *arg)
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
        g_note[0] = '\0';
        fn(arg);
        Counts c;
        memset(&c, 0, sizeof c);
        c.checks = checks;
        c.failures = failures;
        snprintf(c.note, sizeof c.note, "%s", g_note);
        ssize_t w = write(fds[1], &c, sizeof c);
        _exit(w == (ssize_t)sizeof c ? 0 : 1);
    }
    close(fds[1]);
    Counts c;
    memset(&c, 0, sizeof c);
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

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int make_version_dir(const char *root, const char *version)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/macos/%s", root, version);
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return 0;
    char file[PATH_MAX + 32];
    char text[512];
    snprintf(file, sizeof file, "%s/libT.dylib.api", path);
    snprintf(text, sizeof text,
             "ocerz-apidb 1\nlibrary /usr/lib/libT.dylib\nsdk macos %s\nstub _only_%s x\n",
             version, version);
    if (!write_file(file, text))
        return 0;
    snprintf(file, sizeof file, "%s/libBroken.dylib.api", path);
    if (!write_file(file, "ocerz-apidb 1\nlibrary /usr/lib/libBroken.dylib\nsdk macos 1\nnope\n"))
        return 0;
    snprintf(file, sizeof file, "%s/libElse.dylib.api", path);
    return write_file(file, "ocerz-apidb 1\nlibrary /usr/lib/libSomethingElse.dylib\nsdk macos 1\n");
}

static int build_tree(char *root, size_t rootlen)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(root, rootlen, "%s/ocerz-apidb-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(root))
        return 0;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/macos", root);
    if (mkdir(path, 0755) != 0)
        return 0;
    static const char *const versions[] = { "10.9", "10.10", "10.13", "10.13.5", "26.6", "27.0" };
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++)
        if (!make_version_dir(root, versions[i]))
            return 0;
    static const char *const decoys[] = { "latest", ".hidden", "10.256", "70000.0" };
    for (size_t i = 0; i < sizeof decoys / sizeof decoys[0]; i++) {
        snprintf(path, sizeof path, "%s/macos/%s", root, decoys[i]);
        if (mkdir(path, 0755) != 0)
            return 0;
        char file[PATH_MAX + 32];
        snprintf(file, sizeof file, "%s/libT.dylib.api", path);
        if (!write_file(file, "ocerz-apidb 1\nlibrary /usr/lib/libT.dylib\nsdk macos 99.0\n"))
            return 0;
    }
    snprintf(path, sizeof path, "%s/macos/28.0", root);
    if (!write_file(path, "not a directory\n"))
        return 0;
    snprintf(path, sizeof path, "%s/macos/README", root);
    return write_file(path, "decoy\n");
}

static void remove_tree(const char *root)
{
    char cmd[PATH_MAX + 32];
    if (strncmp(root, "/", 1) != 0 || !strstr(root, "ocerz-apidb-"))
        return;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "test_apidb: could not remove %s\n", root);
}

static const char *leaf(const char *p)
{
    const char *s = p ? strrchr(p, '/') : NULL;
    return s ? s + 1 : p;
}

typedef struct Choice {
    uint32_t minos;
    const char *want;
} Choice;

static void choose(void *arg)
{
    const Choice *c = arg;
    setenv("OCERZ_APIDB", g_root, 1);
    ocerz_apidb_set_minos(c->minos);
    const char *dir = ocerz_apidb_dir();
    CHECK(dir && strcmp(leaf(dir), c->want) == 0,
          "a guest declaring %u.%u.%u was given %s, want %s", c->minos >> 16,
          (c->minos >> 8) & 0xff, c->minos & 0xff, dir ? dir : "(no directory)", c->want);
    CHECK(dir && dir[0] == '/', "the chosen directory %s is not absolute", dir ? dir : "(none)");
    const OcerzApiLibrary *lib = ocerz_apidb_library("/usr/lib/libT.dylib");
    CHECK(lib && strcmp(lib->sdk_version, c->want) == 0,
          "libT came from the %s directory, want %s", lib ? lib->sdk_version : "(no)", c->want);
    char only[64];
    snprintf(only, sizeof only, "_only_%s", c->want);
    CHECK(lib && ocerz_apidb_find(lib, only) != NULL, "libT does not export %s", only);
}

static void test_selection(void)
{
    static const Choice cases[] = {
        { 0x00000000, "27.0" },
        { 0x000a0d00, "10.13" },
        { 0x000a0d04, "10.13" },
        { 0x000a0d05, "10.13.5" },
        { 0x000a0e00, "10.13.5" },
        { 0x000a0900, "10.9" },
        { 0x000a0901, "10.9" },
        { 0x000a0a00, "10.10" },
        { 0x000a0c00, "10.10" },
        { 0x000a0800, "10.9" },
        { 0x000a0000, "10.9" },
        { 0x001a0600, "26.6" },
        { 0x001a0700, "26.6" },
        { 0x001b0000, "27.0" },
        { 0x001c0000, "27.0" },
        { 0x00630000, "27.0" },
        { 0xffffffff, "27.0" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char what[64];
        snprintf(what, sizeof what, "selection case %zu", i);
        in_child(what, choose, (void *)&cases[i]);
    }
}

static void choice_stands(void *arg)
{
    setenv("OCERZ_APIDB", g_root, 1);
    ocerz_apidb_set_minos(0x000a0900);
    const char *first = ocerz_apidb_dir();
    ocerz_apidb_set_minos(0);
    const char *second = ocerz_apidb_dir();
    CHECK(first && second && strcmp(leaf(second), "10.9") == 0,
          "a minimum OS set after the choice moved it to %s", second ? second : "(none)");
    const OcerzApiLibrary *lib = ocerz_apidb_library("/usr/lib/libT.dylib");
    CHECK(lib && strcmp(lib->sdk_version, "10.9") == 0, "libT did not come from 10.9");
}

static void thread_load(void *arg)
{
    *(const OcerzApiLibrary **)arg = ocerz_apidb_library("/usr/lib/libT.dylib");
}

static void *thread_main(void *arg)
{
    thread_load(arg);
    return NULL;
}

static void libraries(void *arg)
{
    setenv("OCERZ_APIDB", g_root, 1);
    const OcerzApiLibrary *got[8];
    pthread_t th[8];
    int started = 0;
    for (int i = 0; i < 8; i++)
        started += pthread_create(&th[i], NULL, thread_main, &got[i]) == 0;
    CHECK(started == 8, "only %d of 8 loader threads started", started);
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    const OcerzApiLibrary *lib = ocerz_apidb_library("/usr/lib/libT.dylib");
    CHECK(lib != NULL, "libT does not load");
    for (int i = 0; i < started; i++)
        CHECK(got[i] == lib, "thread %d loaded libT at %p and the main thread has %p", i,
              (void *)got[i], (void *)lib);
    CHECK(ocerz_apidb_library("/usr/lib/libT.dylib") == lib, "a repeated load moved libT");
    CHECK(lib && strcmp(leaf(lib->path), "libT.dylib.api") == 0 && lib->path[0] == '/',
          "libT's path is %s", lib ? lib->path : "(none)");

    CHECK(ocerz_apidb_library("/usr/lib/libNoSuchThing.dylib") == NULL,
          "a library with no file loaded");
    CHECK(ocerz_apidb_library("/usr/lib/libNoSuchThing.dylib") == NULL,
          "a library with no file loaded on the second ask");
    CHECK(ocerz_apidb_library("/opt/elsewhere/libT.dylib") == NULL,
          "a different install name with the same last component took libT's file");
    CHECK(ocerz_apidb_library("/usr/lib/libElse.dylib") == NULL,
          "a file describing another install name was taken for libElse");
    CHECK(ocerz_apidb_library("/usr/lib/libBroken.dylib") == NULL, "a file that breaks a rule loaded");
    CHECK(ocerz_apidb_library("/usr/lib/libBroken.dylib") == NULL,
          "a file that breaks a rule loaded on the second ask");
    CHECK(ocerz_apidb_library("/usr/lib/") == NULL, "an install name with no last component loaded");
    CHECK(ocerz_apidb_library("") == NULL, "the empty install name loaded");
    CHECK(ocerz_apidb_library(NULL) == NULL, "no install name loaded");
    CHECK(ocerz_apidb_library("/usr/lib/libT.dylib") == lib, "libT moved after the misses");
}

static void relative_root(void *arg)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", g_root);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        CHECK(0, "the temporary root %s has no parent", g_root);
        return;
    }
    *slash = '\0';
    CHECK(chdir(parent) == 0, "cannot enter %s", parent);
    setenv("OCERZ_APIDB", slash + 1, 1);
    const char *dir = ocerz_apidb_dir();
    CHECK(dir && dir[0] == '/', "a relative root chose %s, which is not absolute", dir ? dir : "(none)");
    CHECK(chdir("/") == 0, "cannot leave for /");
    const OcerzApiLibrary *lib = ocerz_apidb_library("/usr/lib/libT.dylib");
    CHECK(lib != NULL, "after the guest's chdir a relative root no longer finds libT");
}

static void missing_root(void *arg)
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/nowhere", g_root);
    setenv("OCERZ_APIDB", path, 1);
    CHECK(ocerz_apidb_dir() == NULL, "a root that does not exist chose a directory");
    CHECK(ocerz_apidb_library("/usr/lib/libT.dylib") == NULL, "a root that does not exist loaded libT");
}

static void empty_root(void *arg)
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/macos/latest", g_root);
    setenv("OCERZ_APIDB", path, 1);
    CHECK(ocerz_apidb_dir() == NULL, "a root with no macos directory chose one");
}

static void default_root(void *arg)
{
    char exe[PATH_MAX];
    uint32_t sz = sizeof exe;
    CHECK(_NSGetExecutablePath(exe, &sz) == 0, "no executable path");
    char bin[PATH_MAX], apis[PATH_MAX], copy[PATH_MAX], cmd[3 * PATH_MAX];
    snprintf(bin, sizeof bin, "%s/bin", g_root);
    snprintf(apis, sizeof apis, "%s/bin/runtime/apis/macos/5.0", g_root);
    snprintf(copy, sizeof copy, "%s/bin/test_apidb_copy", g_root);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' && cp '%s' '%s'", apis, exe, copy);
    CHECK(system(cmd) == 0, "cannot stage a copy of the test beside a runtime directory");
    char file[PATH_MAX + 32];
    snprintf(file, sizeof file, "%s/libT.dylib.api", apis);
    CHECK(write_file(file, "ocerz-apidb 1\nlibrary /usr/lib/libT.dylib\nsdk macos 5.0\nstub _five x\n"),
          "cannot write the default-root file");
    snprintf(cmd, sizeof cmd, "env -u OCERZ_APIDB '%s' --print-default", copy);
    FILE *p = popen(cmd, "r");
    char line[PATH_MAX] = "";
    if (p) {
        if (!fgets(line, sizeof line, p))
            line[0] = '\0';
        pclose(p);
    }
    line[strcspn(line, "\n")] = '\0';
    char want[PATH_MAX + 64];
    char real_bin[PATH_MAX];
    if (!realpath(bin, real_bin))
        snprintf(real_bin, sizeof real_bin, "%s", bin);
    snprintf(want, sizeof want, "%s/runtime/apis/macos/5.0 1", real_bin);
    CHECK(strcmp(line, want) == 0, "with no OCERZ_APIDB the copy chose \"%s\", want \"%s\"", line, want);
}

static int print_default(void)
{
    const char *dir = ocerz_apidb_dir();
    const OcerzApiLibrary *lib = ocerz_apidb_library("/usr/lib/libT.dylib");
    printf("%s %d\n", dir ? dir : "(none)", lib && ocerz_apidb_find(lib, "_five") ? 1 : 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--print-default") == 0)
        return print_default();

    test_records();
    test_refusals();
    test_committed();

    if (!build_tree(g_root, sizeof g_root)) {
        CHECK(0, "cannot build the temporary database tree under %s", g_root);
    } else {
        test_selection();
        in_child("a choice stands", choice_stands, NULL);
        in_child("libraries", libraries, NULL);
        in_child("relative root", relative_root, NULL);
        in_child("missing root", missing_root, NULL);
        in_child("empty root", empty_root, NULL);
        in_child("default root", default_root, NULL);
        remove_tree(g_root);
    }

    printf("test_apidb: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
