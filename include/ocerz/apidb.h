/*
 * The API database: what each synthesized library exports and how each export
 * crosses, read from data files rather than compiled into ocerz.
 *
 * Until M9 the virtual libraries' export lists lived in src/vdylib.c and the
 * signature of every bridged function lived in a table in src/bridge.c, both
 * written by hand.  That does not scale to a framework, and a hand-written
 * signature is exactly the kind of fact that is wrong without anyone noticing.
 * The lists and signatures now come from files generated out of the SDK by
 * tools/sdkgen: the library's .tbd names every export the real library has, the
 * headers parsed by clang for x86_64 and for arm64 say what each one is, and
 * the few exports no header can describe correctly - the ones ocerz must answer
 * itself - come from tools/sdkgen/overrides and are written into the same files.
 * Because the files are data, supporting another framework or regenerating
 * against a newer SDK changes no object file, and the twenty committed unit
 * binaries that link every object are not rewritten for it.
 *
 * ---- where the files are ----
 * One file per library, named for the last component of its install name with
 * .api appended - libSystem.B.dylib.api, CoreFoundation.api - in a directory
 * per SDK version:
 *
 *     <root>/macos/<version>/<leaf>.api
 *
 * The root is OCERZ_APIDB when that is set, and otherwise runtime/apis beside
 * the ocerz executable.  The version directory is chosen once per process, from
 * the minimum macOS version the guest's main image declares
 * (LC_BUILD_VERSION, or LC_VERSION_MIN_MACOSX): the newest directory not newer
 * than that, or the oldest directory there is when every one is newer, or the
 * newest when the image declares nothing.  A library with no file is a library
 * native mode does not synthesize.
 *
 * ---- the format ----
 * Text, one record per line, fields separated by single spaces, nothing after
 * the last field.  Blank lines and lines starting with # are ignored.  The
 * first record is the header, the second names the library:
 *
 *     ocerz-apidb 1
 *     library <install-name>
 *     sdk macos <version>
 *
 * and every record after them is one of:
 *
 *     fn <export> <host-symbol> <signature>
 *         A bridged function: calls to <export> cross to <host-symbol> in the
 *         native library under <signature>, in the notation abi.h defines.
 *     data <export> <host-symbol>
 *         A native variable: the export is an absolute trie entry holding the
 *         address of <host-symbol> in the native library.
 *     var <export> <bytes> <filler>
 *         A variable of the image's own: a slot of <bytes> bytes in its __DATA,
 *         filled when the image is built by the filler vdylib.c knows by the
 *         name <filler> (stack_guard is the canary).
 *     special <export> <handler>
 *         A function ocerz answers itself: a stub whose trap goes to the
 *         handler bridge.c knows by the name <handler>.
 *     stub <export> <reason>
 *         An export that binds but has no crossing: calling it names it and
 *         stops with OCERZ_BRIDGE_UNIMPL_EXIT.  <reason> is one word saying
 *         why, such as variadic, struct-value, va-list, long-double, block,
 *         layout, callback-struct or override.
 *     shape <name> <version> <words> <word>...
 *         The layout of a structure of function pointers, <words> 8-byte words
 *         long, as it is when its first word holds <version>: each <word> is -
 *         for a word that is not a function pointer, or the signature of the
 *         function pointer stored there.
 *     struct <export> <argpos> <shape-name>
 *         Argument <argpos>, counted from 0, of the fn record <export> points
 *         at a structure described by the shapes named <shape-name>, which the
 *         bridge copies and converts for the length of the call.
 *
 * An export name appears in exactly one of fn, data, var, special and stub.
 * struct records may come before or after the records they name.  A file that
 * breaks any of these rules is refused whole, with its path, line number and
 * the rule, rather than loaded partly: a library half-described would bind
 * some imports to the wrong thing.
 *
 * ---- the interface ----
 * ocerz_apidb_library loads and parses the file for an install name the first
 * time it is asked and returns the same pointer every time after, or NULL when
 * there is no file.  The parsed library owns its strings and is never freed.
 * ocerz_apidb_parse is the parser on its own, for tests.  ocerz_apidb_set_minos
 * is called by the loader with the main image's minimum OS version, packed as
 * Mach-O packs it (xxxx.yy.zz in nibbles), before anything asks for a library.
 * ocerz_apidb_postfork_child puts the loading lock back to its initial state in
 * a fork child; a library is published only once it is parsed whole, so one
 * another thread was loading at the fork is simply loaded again.
 */
#ifndef OCERZ_APIDB_H
#define OCERZ_APIDB_H

#include "ocerz/types.h"

#define OCERZ_APIDB_SHAPE_WORDS 16
#define OCERZ_APIDB_STRUCT_ARGS 2

typedef enum OcerzApiKind {
    OCERZ_API_FN = 1,
    OCERZ_API_DATA,
    OCERZ_API_VAR,
    OCERZ_API_SPECIAL,
    OCERZ_API_STUB,
} OcerzApiKind;

typedef struct OcerzApiShape {
    const char *name;
    uint64_t version;
    int words;
    const char *word[OCERZ_APIDB_SHAPE_WORDS];
} OcerzApiShape;

typedef struct OcerzApiStructArg {
    int argpos;
    const char *shape;
} OcerzApiStructArg;

typedef struct OcerzApiEntry {
    OcerzApiKind kind;
    const char *export_name;
    const char *host;
    const char *sig;
    const char *handler;
    const char *filler;
    uint32_t bytes;
    const char *reason;
    int nstructs;
    OcerzApiStructArg structs[OCERZ_APIDB_STRUCT_ARGS];
} OcerzApiEntry;

typedef struct OcerzApiLibrary {
    const char *path;
    const char *install_name;
    const char *sdk_version;
    const OcerzApiEntry *entries;
    int nentries;
    const OcerzApiShape *shapes;
    int nshapes;
} OcerzApiLibrary;

const OcerzApiLibrary *ocerz_apidb_library(const char *install_name);
const OcerzApiLibrary *ocerz_apidb_parse(const char *path, const char *text, size_t len,
                                         char *err, size_t errlen);
const OcerzApiEntry *ocerz_apidb_find(const OcerzApiLibrary *lib, const char *export_name);
const OcerzApiShape *ocerz_apidb_shape(const OcerzApiLibrary *lib, const char *name,
                                       uint64_t version);
void ocerz_apidb_set_minos(uint32_t minos);
const char *ocerz_apidb_dir(void);
void ocerz_apidb_postfork_child(void);

#endif
