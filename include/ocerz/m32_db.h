/*
 * m32's i386 API database (src/m32_db.c).
 */
#ifndef OCERZ_M32_DB_H
#define OCERZ_M32_DB_H

enum { M32_DB_FN = 1, M32_DB_DATA, M32_DB_BAD };

typedef struct M32DbRec {
    int kind;
    char *name;             /* export name without variant suffix */
    char *host_symbol;
    char *guest, *host;     /* notations (fn32) */
    int guest_size, host_size;
    char *data_kind;        /* ptr, int, blob (data32) */
    char *reason;           /* bad32 */
    const char *variant;    /* the looked-up name's suffix, from its first '$', or NULL */
} M32DbRec;

int m32_db_lookup(const char *install_name, const char *symbol, M32DbRec *out);

#endif
