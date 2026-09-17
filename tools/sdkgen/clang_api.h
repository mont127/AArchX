/*
 * The part of libclang's C API that sdkgen uses, declared by hand.
 *
 * The Command Line Tools ship /Library/Developer/CommandLineTools/usr/lib/
 * libclang.dylib but not the clang-c headers that describe it, and Xcode is not
 * something this tool may assume.  So the handful of structures and functions
 * the generator calls are declared here, copied in shape from clang-c/Index.h.
 * The structures are passed and returned by value, which is why their layouts
 * are written out rather than left opaque: a CXCursor is an int kind, an int
 * of extra data and three pointers, a CXType an int kind and two pointers, a
 * CXString a pointer and an unsigned of flags, and a CXSourceLocation two
 * pointers and an unsigned.  Those layouts have been stable since libclang
 * gained each type, because every client compiled against an older header
 * depends on them.
 *
 * Enumerations are declared as int.  The enumerators that name cursor and type
 * kinds are deliberately absent: their numbers have moved between clang
 * releases, so sdkgen asks clang_getCursorKindSpelling and
 * clang_getTypeKindSpelling for the spelling of every value at startup and
 * takes the numbers from whichever values spell the names it wants.  A cursor
 * kind's spelling is asked for only when clang_isDeclaration, a pure range
 * test, accepts the value, because clang_getCursorKindSpelling ends in an
 * unreachable-code marker for a value that is not an enumerator.  The few
 * constants that are kept - the visitor results, the one parse flag, the error
 * severity, the external linkage value and the no-thread-local value - are part
 * of the ABI every libclang client is compiled against and have never been
 * renumbered.
 *
 * The printing policy is an opaque pointer.  sdkgen pretty-prints a union's
 * declaration only to see whether it carries __attribute__((transparent_union)),
 * the one attribute that changes how an argument is passed and that libclang
 * exposes no other way.
 */
#ifndef SDKGEN_CLANG_API_H
#define SDKGEN_CLANG_API_H

typedef void *CXIndex;
typedef struct CXTranslationUnitImpl *CXTranslationUnit;
typedef void *CXClientData;
typedef void *CXFile;
typedef void *CXDiagnostic;

typedef struct {
    const void *data;
    unsigned private_flags;
} CXString;

typedef struct {
    int kind;
    int xdata;
    const void *data[3];
} CXCursor;

typedef struct {
    int kind;
    void *data[2];
} CXType;

typedef struct {
    const void *ptr_data[2];
    unsigned int_data;
} CXSourceLocation;

struct CXUnsavedFile {
    const char *Filename;
    const char *Contents;
    unsigned long Length;
};

#define CXChildVisit_Break 0
#define CXChildVisit_Continue 1
#define CXChildVisit_Recurse 2

#define CXVisit_Break 0
#define CXVisit_Continue 1

#define CXTranslationUnit_SkipFunctionBodies 0x40

#define CXDiagnostic_Error 3

#define CXLinkage_External 4

#define CXTLS_None 0

typedef int (*CXCursorVisitor)(CXCursor cursor, CXCursor parent, CXClientData client_data);
typedef int (*CXFieldVisitor)(CXCursor cursor, CXClientData client_data);

CXIndex clang_createIndex(int exclude_declarations_from_pch, int display_diagnostics);
void clang_disposeIndex(CXIndex index);
int clang_parseTranslationUnit2(CXIndex index, const char *source_filename,
                                const char *const *command_line_args, int num_command_line_args,
                                struct CXUnsavedFile *unsaved_files, unsigned num_unsaved_files,
                                unsigned options, CXTranslationUnit *out_tu);
void clang_disposeTranslationUnit(CXTranslationUnit tu);
CXCursor clang_getTranslationUnitCursor(CXTranslationUnit tu);
CXCursor clang_getNullCursor(void);
unsigned clang_visitChildren(CXCursor parent, CXCursorVisitor visitor, CXClientData client_data);

CXString clang_getClangVersion(void);
const char *clang_getCString(CXString string);
void clang_disposeString(CXString string);

unsigned clang_isDeclaration(int kind);
CXString clang_getCursorKindSpelling(int kind);
CXString clang_getTypeKindSpelling(int kind);

CXString clang_getCursorSpelling(CXCursor cursor);
CXString clang_Cursor_getMangling(CXCursor cursor);
CXString clang_getCursorUSR(CXCursor cursor);
int clang_getCursorLinkage(CXCursor cursor);
int clang_getCursorTLSKind(CXCursor cursor);
CXType clang_getCursorType(CXCursor cursor);
CXSourceLocation clang_getCursorLocation(CXCursor cursor);
void clang_getExpansionLocation(CXSourceLocation location, CXFile *file, unsigned *line,
                                unsigned *column, unsigned *offset);
CXString clang_getFileName(CXFile file);

CXType clang_getCanonicalType(CXType type);
CXType clang_getUnqualifiedType(CXType type);
CXType clang_getPointeeType(CXType type);
CXType clang_getResultType(CXType type);
int clang_getNumArgTypes(CXType type);
CXType clang_getArgType(CXType type, unsigned index);
unsigned clang_isFunctionTypeVariadic(CXType type);
unsigned clang_isConstQualifiedType(CXType type);
CXCursor clang_getTypeDeclaration(CXType type);
CXType clang_getEnumDeclIntegerType(CXCursor cursor);
CXType clang_getTypedefDeclUnderlyingType(CXCursor cursor);
CXType clang_getArrayElementType(CXType type);
long long clang_getArraySize(CXType type);
CXType clang_Type_getNamedType(CXType type);
CXType clang_Type_getModifiedType(CXType type);
CXString clang_getTypedefName(CXType type);
CXString clang_getTypeSpelling(CXType type);
long long clang_Type_getSizeOf(CXType type);
long long clang_Type_getAlignOf(CXType type);
unsigned clang_Type_visitFields(CXType type, CXFieldVisitor visitor, CXClientData client_data);
long long clang_Cursor_getOffsetOfField(CXCursor cursor);
int clang_getFieldDeclBitWidth(CXCursor cursor);
unsigned clang_Cursor_isBitField(CXCursor cursor);

void *clang_getCursorPrintingPolicy(CXCursor cursor);
void clang_PrintingPolicy_dispose(void *policy);
CXString clang_getCursorPrettyPrinted(CXCursor cursor, void *policy);

unsigned clang_getNumDiagnostics(CXTranslationUnit tu);
CXDiagnostic clang_getDiagnostic(CXTranslationUnit tu, unsigned index);
int clang_getDiagnosticSeverity(CXDiagnostic diagnostic);
CXString clang_formatDiagnostic(CXDiagnostic diagnostic, unsigned options);
unsigned clang_defaultDiagnosticDisplayOptions(void);
void clang_disposeDiagnostic(CXDiagnostic diagnostic);

#endif
