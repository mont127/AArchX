/*
 * Application bundles (include/ocerz/bundle.h): a bundle's executable and the strings of its Info.plist, which is
 * XML or a binary property list.  main.c runs the executable of a bundle it is given; ocerz -report (report.c)
 * names the bundle's identifier and versions.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ocerz/bundle.h"

/* A big-endian unsigned integer of size bytes, as binary property lists store them. */
static uint64_t bplist_be(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    for (unsigned k = 0; k < size; k++)
        v = v << 8 | p[k];
    return v;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    unsigned off_size, ref_size;
    uint64_t nobj, table;
} Bplist;

/* Where the object at index i starts, or 0 if the index or its offset is out of bounds. */
static size_t bplist_object(const Bplist *b, uint64_t i)
{
    if (i >= b->nobj)
        return 0;
    uint64_t at = b->table + i * b->off_size;
    if (at + b->off_size > b->len - 32)
        return 0;
    uint64_t off = bplist_be(b->buf + at, b->off_size);
    return off >= 8 && off < b->len - 32 ? (size_t)off : 0;
}

/* The count in a marker's low nibble, or the integer object after it when the nibble is 0xF; where the payload
   starts in *pos. */
static int bplist_count(const Bplist *b, size_t *pos, uint64_t *count)
{
    unsigned low = b->buf[*pos] & 0x0f;
    (*pos)++;
    if (low != 0x0f) {
        *count = low;
        return 1;
    }
    if (*pos >= b->len || (b->buf[*pos] & 0xf0) != 0x10)
        return 0;
    unsigned size = 1u << (b->buf[*pos] & 0x0f);
    if (size > 8 || *pos + 1 + size > b->len)
        return 0;
    *count = bplist_be(b->buf + *pos + 1, size);
    *pos += 1 + size;
    return 1;
}

/* An ASCII or UTF-16 string object holding only ASCII, as a C string; 0 if it is anything else. */
static int bplist_ascii(const Bplist *b, size_t pos, char *out, size_t n)
{
    unsigned type = b->buf[pos] >> 4;
    uint64_t count;
    if ((type != 5 && type != 6) || !bplist_count(b, &pos, &count) || count >= n)
        return 0;
    uint64_t bytes = type == 5 ? count : count * 2;
    if (pos + bytes > b->len)
        return 0;
    for (uint64_t k = 0; k < count; k++) {
        unsigned c = type == 5 ? b->buf[pos + k] : (unsigned)bplist_be(b->buf + pos + 2 * k, 2);
        if (c == 0 || c > 0x7e)
            return 0;
        out[k] = (char)c;
    }
    out[count] = '\0';
    return 1;
}

/* The string the top-level dictionary of a binary property list ("bplist00") gives key, bounds-checked against
   a file that may be anything. */
static int bplist_string(const uint8_t *buf, size_t len, const char *key, char *out, size_t n)
{
    if (len < 40 || memcmp(buf, "bplist00", 8) != 0)
        return 0;
    const uint8_t *t = buf + len - 32;
    Bplist b = {buf, len, t[6], t[7], bplist_be(t + 8, 8), bplist_be(t + 24, 8)};
    if (!b.off_size || b.off_size > 8 || !b.ref_size || b.ref_size > 8 || b.table >= len)
        return 0;
    size_t pos = bplist_object(&b, bplist_be(t + 16, 8));
    uint64_t count;
    if (!pos || (buf[pos] >> 4) != 0xd || !bplist_count(&b, &pos, &count) || count > len ||
        pos + 2 * count * b.ref_size > len)
        return 0;
    char name[256];
    for (uint64_t k = 0; k < count; k++) {
        size_t kp = bplist_object(&b, bplist_be(buf + pos + k * b.ref_size, b.ref_size));
        if (!kp || !bplist_ascii(&b, kp, name, sizeof name) || strcmp(name, key) != 0)
            continue;
        size_t vp = bplist_object(&b, bplist_be(buf + pos + (count + k) * b.ref_size, b.ref_size));
        return vp && bplist_ascii(&b, vp, out, n);
    }
    return 0;
}

/* the string under key in a bundle's Contents/Info.plist, XML or binary */
int ocerz_bundle_plist(const char *dir, const char *key, char *out, size_t n)
{
    char plist[PATH_MAX], tag[256];
    snprintf(plist, sizeof plist, "%s/Contents/Info.plist", dir);
    snprintf(tag, sizeof tag, "<key>%s</key>", key);
    FILE *f = fopen(plist, "rb");
    if (!f)
        return 0;
    static char buf[1 << 20];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    const char *k = strstr(buf, tag);
    const char *v = k ? strstr(k, "<string>") : NULL;
    const char *e = v ? strstr(v, "</string>") : NULL;
    if (e && (size_t)(e - v - 8) < n) {
        memcpy(out, v + 8, (size_t)(e - v - 8));
        out[e - v - 8] = '\0';
        return 1;
    }
    return bplist_string((const uint8_t *)buf, len, key, out, n);
}

int ocerz_bundle_executable(const char *dir, char *out, size_t n)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    char name[PATH_MAX] = "";
    if (!ocerz_bundle_plist(dir, "CFBundleExecutable", name, sizeof name) || strchr(name, '/'))
        name[0] = '\0';
    if (!name[0]) {
        const char *b = strrchr(dir, '/');
        b = b ? b + 1 : dir;
        size_t bl = strlen(b);
        while (bl > 0 && b[bl - 1] == '/')
            bl--;
        if (bl > 4 && strncmp(b + bl - 4, ".app", 4) == 0)
            bl -= 4;
        if (bl == 0 || bl >= sizeof name)
            return 0;
        memcpy(name, b, bl);
        name[bl] = '\0';
    }
    int len = snprintf(out, n, "%s/Contents/MacOS/%s", dir, name);
    return len > 0 && (size_t)len < n && stat(out, &st) == 0 && S_ISREG(st.st_mode);
}
