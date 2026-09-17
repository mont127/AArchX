/*
 * The .tbd reader: version 4 text stubs, every document in a file.
 *
 * The format is line oriented in practice even though it is YAML in name.  A
 * document opens with a line starting --- and closes with a line of three
 * dots.  A key at column zero either holds a scalar or a flow list, like
 * install-name and targets, or opens a section whose items follow, each item
 * introduced by "- " and continued by indented keys.  So a line is read as its
 * indentation, an optional item marker, a key and a value, and a value that
 * opens a flow list without closing it swallows the lines after it until the
 * bracket closes outside quotes.  That is the whole grammar Apple's tapi tool
 * writes, and a line that does not fit it is reported with its number rather
 * than skipped, because a silently dropped section would shrink the export
 * list the generator is supposed to account for in full.  The search for the
 * closing bracket resumes where the previous line left it instead of starting
 * over, because Foundation's symbol list runs for tens of thousands of lines
 * and rescanning it from the opening bracket on every line took a minute and a
 * half.
 *
 * Quoting is YAML's: single quotes double an embedded quote, double quotes take
 * a backslash escape.  Both are stripped so a symbol reads as the linker spells
 * it.  Documents of any tbd-version other than 4 are refused.
 */
#include "tbd.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *tbd_alloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "sdkgen: out of memory reading a .tbd\n");
        exit(2);
    }
    return q;
}

static char *tbd_strndup(const char *s, size_t n)
{
    char *d = tbd_alloc(NULL, n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void tbd_push(TbdList *l, char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = tbd_alloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
}

static TbdItem *tbd_new_item(TbdSection *sec)
{
    if (sec->n == sec->cap) {
        sec->cap = sec->cap ? sec->cap * 2 : 8;
        sec->items = tbd_alloc(sec->items, (size_t)sec->cap * sizeof *sec->items);
    }
    TbdItem *it = &sec->items[sec->n++];
    memset(it, 0, sizeof *it);
    return it;
}

static TbdDoc *tbd_new_doc(TbdFile *f)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->docs = tbd_alloc(f->docs, (size_t)f->cap * sizeof *f->docs);
    }
    TbdDoc *d = &f->docs[f->n++];
    memset(d, 0, sizeof *d);
    return d;
}

static int tbd_fail(char *err, size_t errlen, const char *path, int line, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(err, errlen, "%s:%d: %s", path, line, msg);
    return -1;
}

typedef struct TbdScan {
    size_t pos;
    int depth;
    char q;
} TbdScan;

static int tbd_flow_scan(TbdScan *st, const char *s, size_t len)
{
    size_t i = st->pos;
    for (; i < len; i++) {
        char c = s[i];
        if (st->q) {
            if (c == st->q) {
                if (st->q == '\'' && i + 1 >= len)
                    break;
                if (st->q == '\'' && s[i + 1] == '\'')
                    i++;
                else
                    st->q = 0;
            } else if (st->q == '"' && c == '\\') {
                if (i + 1 >= len)
                    break;
                i++;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            st->q = c;
        } else if (c == '[') {
            st->depth++;
        } else if (c == ']' && --st->depth == 0) {
            st->pos = i + 1;
            return 1;
        }
    }
    st->pos = i;
    return 0;
}

static int tbd_flow_closed(const char *s, size_t len)
{
    TbdScan st = { 0, 0, 0 };
    return tbd_flow_scan(&st, s, len);
}

static char *tbd_unquote(const char *s, size_t len)
{
    while (len && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len && isspace((unsigned char)s[len - 1]))
        len--;
    if (len >= 2 && (s[0] == '\'' || s[0] == '"') && s[len - 1] == s[0]) {
        char q = s[0];
        char *d = tbd_alloc(NULL, len);
        size_t k = 0;
        for (size_t i = 1; i + 1 < len; i++) {
            if (q == '\'' && s[i] == '\'' && i + 2 < len && s[i + 1] == '\'')
                i++;
            else if (q == '"' && s[i] == '\\' && i + 2 < len)
                i++;
            d[k++] = s[i];
        }
        d[k] = '\0';
        return d;
    }
    return tbd_strndup(s, len);
}

static int tbd_parse_flow(const char *s, size_t len, TbdList *out)
{
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i]))
        i++;
    if (i >= len || s[i] != '[')
        return -1;
    i++;
    for (;;) {
        while (i < len && isspace((unsigned char)s[i]))
            i++;
        if (i >= len)
            return -1;
        if (s[i] == ']')
            return 0;
        size_t start = i;
        if (s[i] == '\'' || s[i] == '"') {
            char q = s[i++];
            while (i < len) {
                if (s[i] == q) {
                    if (q == '\'' && i + 1 < len && s[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    break;
                }
                if (q == '"' && s[i] == '\\')
                    i++;
                i++;
            }
            if (i >= len)
                return -1;
            i++;
        } else {
            while (i < len && s[i] != ',' && s[i] != ']')
                i++;
        }
        tbd_push(out, tbd_unquote(s + start, i - start));
        while (i < len && isspace((unsigned char)s[i]))
            i++;
        if (i < len && s[i] == ',')
            i++;
    }
}

static TbdSection *tbd_section(TbdDoc *d, const char *key, size_t klen)
{
    if (klen == 7 && memcmp(key, "exports", 7) == 0)
        return &d->exports;
    if (klen == 9 && memcmp(key, "reexports", 9) == 0)
        return &d->reexports;
    if (klen == 20 && memcmp(key, "reexported-libraries", 20) == 0)
        return &d->reexported_libraries;
    if (klen == 15 && memcmp(key, "parent-umbrella", 15) == 0)
        return &d->parent_umbrella;
    return NULL;
}

static TbdList *tbd_item_list(TbdItem *it, const char *key, size_t klen)
{
    static const struct { const char *k; size_t off; } map[] = {
        { "targets", offsetof(TbdItem, targets) },
        { "symbols", offsetof(TbdItem, symbols) },
        { "weak-symbols", offsetof(TbdItem, weak_symbols) },
        { "thread-local-symbols", offsetof(TbdItem, tlv_symbols) },
        { "objc-classes", offsetof(TbdItem, objc_classes) },
        { "objc-eh-types", offsetof(TbdItem, objc_eh_types) },
        { "objc-ivars", offsetof(TbdItem, objc_ivars) },
        { "libraries", offsetof(TbdItem, libraries) },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strlen(map[i].k) == klen && memcmp(map[i].k, key, klen) == 0)
            return (TbdList *)((char *)it + map[i].off);
    return NULL;
}

int tbd_read(const char *path, TbdFile *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    out->path = tbd_strndup(path, strlen(path));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return tbd_fail(err, errlen, path, 0, "cannot open");
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = tbd_alloc(NULL, (size_t)size + 1);
    if (size > 0 && fread(text, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        free(text);
        return tbd_fail(err, errlen, path, 0, "cannot read");
    }
    fclose(fp);
    text[size] = '\0';

    TbdDoc *doc = NULL;
    TbdSection *sec = NULL;
    TbdItem *item = NULL;
    int lineno = 0;
    char *acc = NULL;
    size_t acccap = 0;
    const char *p = text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;
        p = eol ? eol + 1 : p + len;
        lineno++;
        int startline = lineno;

        if (len >= 3 && memcmp(line, "---", 3) == 0) {
            doc = tbd_new_doc(out);
            sec = NULL;
            item = NULL;
            continue;
        }
        if (len >= 3 && memcmp(line, "...", 3) == 0) {
            doc = NULL;
            continue;
        }

        size_t indent = 0;
        while (indent < len && line[indent] == ' ')
            indent++;
        const char *c = line + indent;
        size_t clen = len - indent;
        while (clen && isspace((unsigned char)c[clen - 1]))
            clen--;
        if (clen == 0 || c[0] == '#')
            continue;
        if (!doc) {
            free(text);
            return tbd_fail(err, errlen, path, lineno, "content outside a document");
        }

        int item_start = 0;
        if (clen >= 2 && c[0] == '-' && c[1] == ' ') {
            item_start = 1;
            c += 2;
            clen -= 2;
            indent += 2;
            while (clen && *c == ' ') {
                c++;
                clen--;
                indent++;
            }
        }

        const char *colon = memchr(c, ':', clen);
        if (!colon) {
            free(text);
            return tbd_fail(err, errlen, path, lineno, "expected key: value");
        }
        const char *key = c;
        size_t klen = (size_t)(colon - c);
        const char *val = colon + 1;
        size_t vlen = clen - klen - 1;
        while (vlen && isspace((unsigned char)*val)) {
            val++;
            vlen--;
        }

        if (vlen && *val == '[' && !tbd_flow_closed(val, vlen)) {
            if (acccap < vlen + 1) {
                acccap = (vlen + 1) * 2;
                acc = tbd_alloc(acc, acccap);
            }
            memcpy(acc, val, vlen);
            size_t alen = vlen;
            TbdScan scan = { 0, 0, 0 };
            while (!tbd_flow_scan(&scan, acc, alen)) {
                if (!*p) {
                    free(text);
                    free(acc);
                    return tbd_fail(err, errlen, path, startline, "flow list never closes");
                }
                const char *e2 = strchr(p, '\n');
                size_t l2 = e2 ? (size_t)(e2 - p) : strlen(p);
                if (alen + l2 + 2 > acccap) {
                    acccap = (alen + l2 + 2) * 2;
                    acc = tbd_alloc(acc, acccap);
                }
                acc[alen++] = ' ';
                memcpy(acc + alen, p, l2);
                alen += l2;
                p = e2 ? e2 + 1 : p + l2;
                lineno++;
            }
            val = acc;
            vlen = alen;
        }

        if (indent == 0 && !item_start) {
            sec = tbd_section(doc, key, klen);
            item = NULL;
            if (klen == 11 && memcmp(key, "tbd-version", 11) == 0) {
                if (!(vlen == 1 && val[0] == '4')) {
                    free(text);
                    free(acc);
                    return tbd_fail(err, errlen, path, startline,
                                    "tbd-version %.*s is not the version 4 this reader knows",
                                    (int)vlen, val);
                }
            } else if (klen == 12 && memcmp(key, "install-name", 12) == 0) {
                doc->install_name = tbd_unquote(val, vlen);
            } else if (klen == 7 && memcmp(key, "targets", 7) == 0) {
                if (tbd_parse_flow(val, vlen, &doc->targets) != 0) {
                    free(text);
                    free(acc);
                    return tbd_fail(err, errlen, path, startline, "malformed targets list");
                }
            }
            continue;
        }

        if (!sec)
            continue;
        if (item_start)
            item = tbd_new_item(sec);
        if (!item) {
            free(text);
            free(acc);
            return tbd_fail(err, errlen, path, startline, "section key outside an item");
        }
        if (klen == 8 && memcmp(key, "umbrella", 8) == 0) {
            item->umbrella = tbd_unquote(val, vlen);
            continue;
        }
        TbdList *list = tbd_item_list(item, key, klen);
        if (!list)
            continue;
        if (tbd_parse_flow(val, vlen, list) != 0) {
            free(text);
            free(acc);
            return tbd_fail(err, errlen, path, startline, "malformed %.*s list", (int)klen, key);
        }
    }

    free(text);
    free(acc);
    for (int i = 0; i < out->n; i++)
        if (!out->docs[i].install_name)
            return tbd_fail(err, errlen, path, 0, "document %d has no install-name", i + 1);
    return 0;
}

int tbd_has_target(const TbdList *targets, const char *target)
{
    for (int i = 0; i < targets->n; i++)
        if (strcmp(targets->v[i], target) == 0)
            return 1;
    return 0;
}

const TbdDoc *tbd_find(const TbdFile *file, const char *install_name)
{
    for (int i = 0; i < file->n; i++)
        if (strcmp(file->docs[i].install_name, install_name) == 0)
            return &file->docs[i];
    return NULL;
}
