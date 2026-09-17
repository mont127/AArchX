/*
 * Reading the SDK's text-based dylib stubs.
 *
 * A .tbd file is the SDK's stand-in for a library: its install name, the
 * libraries it re-exports and, per set of targets, the symbols it exports.  It
 * is written in a small, regular subset of YAML, and version 4 of that format
 * is the only one the macOS SDKs sdkgen targets carry, so the reader handles
 * exactly that subset and refuses anything else by name rather than guessing.
 *
 * One file may hold several documents.  libSystem.B.tbd carries the stub of
 * every library it re-exports inline after its own, so a reader that stopped at
 * the first document would miss nearly all of libSystem.  Every document is
 * kept, in file order, as a TbdDoc.
 *
 * Within a document the reader keeps only what the generator asks about: the
 * install name, the top-level targets, and the list sections exports,
 * reexports, reexported-libraries and parent-umbrella, each a list of items
 * that carry their own targets.  An item's flow lists - symbols, weak-symbols,
 * thread-local-symbols, objc-classes, objc-eh-types, objc-ivars, libraries -
 * are kept as arrays of strings with YAML's quoting removed, since the quoting
 * exists only because names like 'R8289209$_close' contain a dollar sign.
 * Any other key is read past.  A flow list may run over as many lines as it
 * likes, and a bracket inside quotes does not close it.
 */
#ifndef SDKGEN_TBD_H
#define SDKGEN_TBD_H

#include <stddef.h>

typedef struct TbdList {
    char **v;
    int n;
    int cap;
} TbdList;

typedef struct TbdItem {
    TbdList targets;
    TbdList symbols;
    TbdList weak_symbols;
    TbdList tlv_symbols;
    TbdList objc_classes;
    TbdList objc_eh_types;
    TbdList objc_ivars;
    TbdList libraries;
    char *umbrella;
} TbdItem;

typedef struct TbdSection {
    TbdItem *items;
    int n;
    int cap;
} TbdSection;

typedef struct TbdDoc {
    char *install_name;
    TbdList targets;
    TbdSection exports;
    TbdSection reexports;
    TbdSection reexported_libraries;
    TbdSection parent_umbrella;
} TbdDoc;

typedef struct TbdFile {
    char *path;
    TbdDoc *docs;
    int n;
    int cap;
} TbdFile;

int tbd_read(const char *path, TbdFile *out, char *err, size_t errlen);
int tbd_has_target(const TbdList *targets, const char *target);
const TbdDoc *tbd_find(const TbdFile *file, const char *install_name);

#endif
