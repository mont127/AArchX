/*
 * i386 Mach-O images for m32 (include/ocerz/m32.h).
 *
 * The main executable of an i386 game is non-PIE and linked at 0x1000; the window is ours, so it maps at its own
 * addresses and needs no rebase.  Dylibs slide to wherever ocerz_map_anywhere puts them.  Every bind (lazy ones
 * included) is applied at load, so dyld_stub_binder is never reached and each __la_symbol_ptr holds its final
 * target from the first call on.
 */
#include <fcntl.h>
#include <pthread.h>
#include <mach-o/fat.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ocerz/bridge.h"
#include "ocerz/m32.h"
#include "ocerz/mem.h"
#include "ocerz/m32_image.h"
#include "ocerz/m32_objc.h"
#include "ocerz/types.h"

M32Image *m32_images[M32_MAX_IMAGES];
int m32_nimages;

static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* The i386 slice of a thin or fat file, and whether an x86_64 slice exists beside it. */
static const uint8_t *slice_i386(const uint8_t *buf, size_t len, size_t *slen, int *has_x86_64)
{
    *has_x86_64 = 0;
    if (len < 28)
        return NULL;
    uint32_t magic = le32(buf);
    if (magic == MH_MAGIC && le32(buf + 4) == CPU_TYPE_I386) {
        *slen = len;
        return buf;
    }
    if (magic == MH_MAGIC_64 && le32(buf + 4) == CPU_TYPE_X86_64)
        *has_x86_64 = 1;
    if (be32(buf) != FAT_MAGIC && be32(buf) != FAT_MAGIC_64)
        return NULL;
    int wide = be32(buf) == FAT_MAGIC_64;
    uint32_t n = be32(buf + 4);
    const uint8_t *found = NULL;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        const uint8_t *a = buf + 8 + i * (wide ? 32 : 20);
        if (a + (wide ? 32 : 20) > buf + len)
            break;
        uint32_t cpu = be32(a);
        uint64_t off = wide ? be64(a + 8) : be32(a + 8), size = wide ? be64(a + 16) : be32(a + 12);
        if (cpu == CPU_TYPE_X86_64)
            *has_x86_64 = 1;
        if (cpu == CPU_TYPE_I386 && off + size <= len) {
            found = buf + off;
            *slen = (size_t)size;
        }
    }
    return found;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (buf = malloc((size_t)st.st_size))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        if (got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        *len = got;
    }
    close(fd);
    return buf;
}

int m32_is_i386(const char *path)
{
    uint8_t head[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    if (n < 28)
        return 0;
    /* a fat file's slices can lie beyond the first page; its arch table cannot */
    if (le32(head) == MH_MAGIC)
        return le32(head + 4) == CPU_TYPE_I386;
    if (be32(head) == FAT_MAGIC || be32(head) == FAT_MAGIC_64) {
        int wide = be32(head) == FAT_MAGIC_64, i386 = 0, x64 = 0;
        for (uint32_t i = 0; i < be32(head + 4) && 8 + (i + 1) * (wide ? 32u : 20u) <= (uint32_t)n; i++) {
            uint32_t cpu = be32(head + 8 + i * (wide ? 32 : 20));
            i386 |= cpu == CPU_TYPE_I386;
            x64 |= cpu == CPU_TYPE_X86_64;
        }
        return i386 && !x64;
    }
    return 0;
}

static uint64_t uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

static int64_t sleb(const uint8_t **p, const uint8_t *end)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        if (shift < 64)
            v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

static uint32_t seg_addr(const M32Image *img, unsigned seg, uint64_t off)
{
    if (seg >= (unsigned)img->nseg)
        return 0;
    return (uint32_t)(img->seg[seg].vmaddr + img->slide + off);
}

static void rebase(M32Image *img)
{
    if (!img->slide || !img->rebase_size)
        return;
    const uint8_t *p = img->file + img->rebase_off, *end = p + img->rebase_size;
    unsigned seg = 0;
    uint64_t off = 0;
    int type = REBASE_TYPE_POINTER;
#define M32_REBASE_ONE() do { uint32_t a = seg_addr(img, seg, off); if (a) m32_wr(a, m32_rd(a) + (uint32_t)img->slide); \
                              off += 4; } while (0)
    while (p < end) {
        uint8_t b = *p++, op = b & REBASE_OPCODE_MASK, imm = b & REBASE_IMMEDIATE_MASK;
        switch (op) {
        case REBASE_OPCODE_DONE: p = end; break;
        case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = imm; off = uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: off += uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: off += (uint64_t)imm * 4; break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES: for (int i = 0; i < imm; i++) M32_REBASE_ONE(); break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: { uint64_t n = uleb(&p, end); for (uint64_t i = 0; i < n; i++) M32_REBASE_ONE(); break; }
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: M32_REBASE_ONE(); off += uleb(&p, end); break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
            uint64_t n = uleb(&p, end), skip = uleb(&p, end);
            for (uint64_t i = 0; i < n; i++) { M32_REBASE_ONE(); off += skip; }
            break;
        }
        default:
            fprintf(stderr, "ocerz: m32: %s: unknown rebase opcode %#x\n", img->path, op);
            p = end;
        }
    }
    (void)type;   /* POINTER and TEXT_ABSOLUTE32 both add the slide to a 32-bit word */
#undef M32_REBASE_ONE
}

uint32_t m32_image_symbol(M32Image *img, const char *name);

/* the first definition of a weak symbol among the loaded images, the main executable first */
static uint32_t m32_weak_definition(const char *name)
{
    for (int i = 0; i < m32_nimages; i++) {
        uint32_t v = m32_image_symbol(m32_images[i], name);
        if (v)
            return v;
    }
    return 0;
}

/* bind, weak bind and lazy bind share one walker; lazy streams keep going after each DONE */
static int bind_stream(M32Image *img, uint32_t boff, uint32_t bsize, int lazy, int weak_stream)
{
    if (!bsize)
        return 0;
    const uint8_t *p = img->file + boff, *end = p + bsize;
    int ordinal = 0, type = BIND_TYPE_POINTER, flags = 0, bad = 0;
    const char *name = "";
    int64_t addend = 0;
    unsigned seg = 0;
    uint64_t off = 0;
#define M32_BIND_ONE() do {                                                                         \
        uint32_t a = seg_addr(img, seg, off);                                                        \
        if (weak_stream) {  /* dyld coalesces a weak symbol to its first definition in load order */  \
            uint32_t w = m32_weak_definition(name);                                                  \
            if (a && w) m32_wr(a, type == BIND_TYPE_TEXT_PCREL32 ? w + (uint32_t)addend - (a + 4)     \
                                                                 : w + (uint32_t)addend);            \
            off += 4; break;                                                                         \
        }                                                                                             \
        uint32_t v = m32_resolve(img, ordinal, name, flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT);          \
        if (!v && !(flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT)) bad++;                                   \
        if (a) {                                                                                      \
            if (type == BIND_TYPE_TEXT_PCREL32) m32_wr(a, v + (uint32_t)addend - (a + 4));           \
            else m32_wr(a, v ? v + (uint32_t)addend : 0);                                             \
        }                                                                                             \
        off += 4;                                                                                     \
    } while (0)
    while (p < end) {
        uint8_t b = *p++, op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        switch (op) {
        case BIND_OPCODE_DONE: if (!lazy) p = end; break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = imm; break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ordinal = (int)uleb(&p, end); break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = imm ? (int)(int8_t)(BIND_OPCODE_MASK | imm) : 0; break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            flags = imm;
            name = (const char *)p;
            while (p < end && *p) p++;
            p++;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = imm; off = uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND: M32_BIND_ONE(); break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: M32_BIND_ONE(); off += uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: M32_BIND_ONE(); off += (uint64_t)imm * 4; break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
            uint64_t n = uleb(&p, end), skip = uleb(&p, end);
            for (uint64_t i = 0; i < n; i++) { M32_BIND_ONE(); off += skip; }
            break;
        }
        default:
            fprintf(stderr, "ocerz: m32: %s: unknown bind opcode %#x\n", img->path, op);
            p = end;
        }
    }
#undef M32_BIND_ONE
    return bad;
}

/* Parse the load commands of a slice already in memory. */
static int parse(M32Image *img)
{
    const uint8_t *f = img->file;
    if (img->flen < 28 || le32(f) != MH_MAGIC)
        return -1;
    img->filetype = le32(f + 12);
    uint32_t ncmds = le32(f + 16), sizeofcmds = le32(f + 20);
    const uint8_t *lc = f + 28, *lend = lc + sizeofcmds;
    if (lend > f + img->flen)
        return -1;
    for (uint32_t i = 0; i < ncmds && lc + 8 <= lend; i++) {
        uint32_t cmd = le32(lc), size = le32(lc + 4);
        if (size < 8 || lc + size > lend)
            return -1;
        switch (cmd) {
        case LC_SEGMENT: {
            if (img->nseg >= M32_MAX_SEGS)
                return -1;
            M32Seg *s = &img->seg[img->nseg++];
            memcpy(s->name, lc + 8, 16);
            s->vmaddr = le32(lc + 24);
            s->vmsize = le32(lc + 28);
            s->fileoff = le32(lc + 32);
            s->filesize = le32(lc + 36);
            s->initprot = le32(lc + 44);
            uint32_t nsects = le32(lc + 48);
            for (uint32_t k = 0; k < nsects && img->nsect < M32_MAX_SECTS; k++) {
                const uint8_t *sc = lc + 56 + k * 68;
                M32Sect *t = &img->sect[img->nsect++];
                memcpy(t->name, sc, 16);
                memcpy(t->seg, sc + 16, 16);
                t->addr = le32(sc + 32);
                t->size = le32(sc + 36);
                t->offset = le32(sc + 40);
                t->flags = le32(sc + 56);
                t->reserved1 = le32(sc + 60);
                t->reserved2 = le32(sc + 64);
            }
            break;
        }
        case LC_DYSYMTAB:
            img->indirectsymoff = le32(lc + 56); img->nindirectsyms = le32(lc + 60);
            img->extreloff = le32(lc + 64); img->nextrel = le32(lc + 68);
            img->locreloff = le32(lc + 72); img->nlocrel = le32(lc + 76);
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY:
            img->has_dyld_info = 1;
            img->rebase_off = le32(lc + 8);  img->rebase_size = le32(lc + 12);
            img->bind_off = le32(lc + 16);   img->bind_size = le32(lc + 20);
            img->weak_off = le32(lc + 24);   img->weak_size = le32(lc + 28);
            img->lazy_off = le32(lc + 32);   img->lazy_size = le32(lc + 36);
            img->export_off = le32(lc + 40); img->export_size = le32(lc + 44);
            break;
        case LC_SYMTAB:
            img->symoff = le32(lc + 8); img->nsyms = le32(lc + 12);
            img->stroff = le32(lc + 16); img->strsize = le32(lc + 20);
            break;
        case LC_MAIN:
            img->entry_main_off = (uint32_t)(le32(lc + 8) | (uint64_t)le32(lc + 12) << 32);
            img->has_main = 1;
            break;
        case LC_UNIXTHREAD:
            if (le32(lc + 8) == 1 /* x86_THREAD_STATE32 */)
                img->entry_thread = le32(lc + 16 + 40);
            break;
        case LC_ID_DYLIB:
            snprintf(img->install_name, sizeof img->install_name, "%s", (const char *)lc + le32(lc + 8));
            break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB:
            if (img->ndeps >= M32_MAX_DEPS)
                return -1;
            img->dep_weak[img->ndeps] = cmd == LC_LOAD_WEAK_DYLIB;
            img->dep_reexport[img->ndeps] = cmd == LC_REEXPORT_DYLIB;
            img->deps[img->ndeps++] = strdup((const char *)lc + le32(lc + 8));
            break;
        case LC_RPATH:
            if (img->nrpath < M32_MAX_RPATHS)
                img->rpath[img->nrpath++] = strdup((const char *)lc + le32(lc + 8));
            break;
        default:
            break;
        }
        lc += size;
    }
    return 0;
}

/* Map the image's segments into the window: at their own addresses (want_fixed) or wherever there is room. */
static int map(M32Image *img, int want_fixed)
{
    uint32_t lo = 0xffffffffu, hi = 0;
    for (int i = 0; i < img->nseg; i++) {
        const M32Seg *s = &img->seg[i];
        if (!s->vmsize || (!s->filesize && !s->initprot))   /* __PAGEZERO */
            continue;
        if (s->vmaddr < lo) lo = s->vmaddr;
        if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
    }
    if (lo >= hi)
        return -1;
    uint32_t span = (hi - lo + 0x3fff) & ~0x3fffu;
    if (want_fixed) {
        /* host pages are 16 KB: a program linked at 0x1000 also gets guest page 0 (a NULL read sees zeros) */
        uint32_t flo = lo & ~0x3fffu, fspan = (hi - flo + 0x3fff) & ~0x3fffu;
        if (ocerz_map_fixed(flo, fspan, PROT_READ | PROT_WRITE) != OCERZ_OK)
            return -1;
        img->slide = 0;
    } else {
        uint64_t base = ocerz_map_anywhere(span, PROT_READ | PROT_WRITE);
        if (!base || base + span > M32_HANDLE_LO)
            return -1;
        img->slide = (int32_t)((uint32_t)base - lo);
    }
    img->lo = lo + (uint32_t)img->slide;
    img->hi = hi + (uint32_t)img->slide;
    for (int i = 0; i < img->nseg; i++) {
        const M32Seg *s = &img->seg[i];
        if (!s->vmsize || (!s->filesize && !s->initprot))
            continue;
        if (s->fileoff + s->filesize > img->flen)
            return -1;
        memcpy(m32_h(s->vmaddr + (uint32_t)img->slide), img->file + s->fileoff, s->filesize);
        if (s->fileoff == 0 && s->filesize)
            img->base = s->vmaddr + (uint32_t)img->slide;
    }
    return 0;
}

const M32Sect *m32_section(const M32Image *img, const char *seg, const char *sect)
{
    for (int i = 0; i < img->nsect; i++)
        if (!strncmp(img->sect[i].seg, seg, 16) && !strncmp(img->sect[i].name, sect, 16))
            return &img->sect[i];
    return NULL;
}

/* Load an i386 image from path. The main executable maps at its own addresses. */
M32Image *m32_image_load(const char *path, int is_main)
{
    size_t len = 0, slen = 0;
    int x64 = 0;
    uint8_t *buf = read_all(path, &len);
    const uint8_t *slice = buf ? slice_i386(buf, len, &slen, &x64) : NULL;
    if (!slice) {
        fprintf(stderr, "ocerz: m32: %s: %s\n", path, buf ? "no i386 slice" : "cannot read");
        free(buf);
        return NULL;
    }
    M32Image *img = calloc(1, sizeof *img);
    img->buf = buf;
    img->file = slice;
    img->flen = slen;
    snprintf(img->path, sizeof img->path, "%s", path);
    snprintf(img->install_name, sizeof img->install_name, "%s", path);
    if (parse(img) != 0 || map(img, is_main) != 0) {
        fprintf(stderr, "ocerz: m32: %s: cannot map the image\n", path);
        free(buf);
        free(img);
        return NULL;
    }
    if (img->has_main)
        img->entry_main = img->base + img->entry_main_off;   /* entryoff counts from the mach header */
    if (img->entry_thread)
        img->entry_thread += (uint32_t)img->slide;
    if (m32_nimages < M32_MAX_IMAGES)
        m32_images[m32_nimages++] = img;
    static int log_images = -1;
    if (log_images < 0)
        log_images = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "images");
    if (log_images)
        fprintf(stderr, "ocerz: m32: image %#x-%#x slide %#x %s\n", img->lo, img->hi, (unsigned)img->slide, img->path);
    return img;
}

/* ---- images without dyld info (linked for 10.5 and older, or by tools that never switched) ----
 * Local relocations slide 32-bit words (r_address counts from the first segment on i386); indirect symbols fill
 * the non-lazy and lazy pointer tables and patch __IMPORT,__jump_table stubs into jmp rel32. */
static int legacy_ordinal(uint16_t n_desc)
{
    int ord = (n_desc >> 8) & 0xff;
    return ord == 0 ? BIND_SPECIAL_DYLIB_SELF : ord == 0xff ? BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE
         : ord == 0xfe ? BIND_SPECIAL_DYLIB_FLAT_LOOKUP : ord;
}

static uint32_t legacy_symbol(M32Image *img, uint32_t symidx, const char **name_out, int *weak)
{
    if (symidx >= img->nsyms)
        return 0;
    const uint8_t *nl = img->file + img->symoff + symidx * 12;
    uint32_t strx = le32(nl);
    uint16_t desc;
    memcpy(&desc, nl + 6, 2);
    const char *name = strx < img->strsize ? (const char *)img->file + img->stroff + strx : "";
    *name_out = name;
    *weak = (desc & N_WEAK_REF) != 0;
    if ((nl[4] & N_TYPE) == N_SECT)   /* defined here: its own address */
        return le32(nl + 8) + (uint32_t)img->slide;
    return m32_resolve(img, legacy_ordinal(desc), name, *weak);
}

static int legacy_fixups(M32Image *img)
{
    uint32_t reloc_base = img->nseg ? img->seg[0].vmaddr : 0;
    if (img->nseg && !img->seg[0].vmsize && img->nseg > 1)   /* skip __PAGEZERO */
        reloc_base = img->seg[1].vmaddr;
    for (uint32_t i = 0; img->slide && i < img->nlocrel; i++) {
        const uint8_t *r = img->file + img->locreloff + i * 8;
        uint32_t w0 = le32(r), w1 = le32(r + 4), addr, len;
        if (w0 & 0x80000000u) {        /* scattered: r_address 24 bits, r_length at bits 28-29 */
            addr = w0 & 0x00ffffffu;
            len = (w0 >> 28) & 3;
        } else {
            addr = w0;
            len = (w1 >> 25) & 3;
            if ((w1 >> 24) & 1)        /* pc-relative: unchanged by a slide */
                continue;
        }
        if (len != 2)
            continue;
        uint32_t a = reloc_base + addr + (uint32_t)img->slide;
        m32_wr(a, m32_rd(a) + (uint32_t)img->slide);
    }
    int bad = 0;
    for (uint32_t i = 0; i < img->nextrel; i++) {   /* external relocations: a symbol's address plus the addend in place */
        const uint8_t *r = img->file + img->extreloff + i * 8;
        uint32_t w0 = le32(r), w1 = le32(r + 4);
        if ((w0 & 0x80000000u) || !((w1 >> 27) & 1) || ((w1 >> 25) & 3) != 2)
            continue;
        const char *name;
        int weak;
        uint32_t v = legacy_symbol(img, w1 & 0x00ffffffu, &name, &weak);
        if (!v && !weak)
            bad++;
        uint32_t a = reloc_base + w0 + (uint32_t)img->slide;
        m32_wr(a, m32_rd(a) + v);
    }
    const uint32_t *ind = (const uint32_t *)(img->file + img->indirectsymoff);
    for (int k = 0; k < img->nsect; k++) {
        const M32Sect *sc = &img->sect[k];
        uint32_t type = sc->flags & SECTION_TYPE, stride;
        if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS)
            stride = 4;
        else if (type == S_SYMBOL_STUBS && (sc->flags & S_ATTR_SELF_MODIFYING_CODE) && sc->reserved2 == 5)
            stride = 5;
        else
            continue;
        for (uint32_t j = 0; j < sc->size / stride; j++) {
            uint32_t n = sc->reserved1 + j, a = sc->addr + (uint32_t)img->slide + j * stride;
            if (n >= img->nindirectsyms)
                break;
            uint32_t symidx = ind[n];
            if (symidx & INDIRECT_SYMBOL_ABS)
                continue;
            uint32_t v;
            if (symidx & INDIRECT_SYMBOL_LOCAL) {
                if (stride == 4)
                    m32_wr(a, m32_rd(a) + (uint32_t)img->slide);
                continue;
            }
            const char *name;
            int weak;
            v = legacy_symbol(img, symidx, &name, &weak);
            if (!v && !weak)
                bad++;
            if (stride == 4)
                m32_wr(a, v);
            else {
                uint8_t jmp[5] = { 0xe9 };
                uint32_t rel = v - (a + 5);
                memcpy(jmp + 1, &rel, 4);
                memcpy(m32_h(a), jmp, 5);
            }
        }
    }
    return bad;
}

/* Each segment gets its own protection once fixups have written it (map() made all of them read-write).  The JIT
 * write-protects a host page while it holds translations of code it believes writable, so a __TEXT left writable
 * sharing a 16 KB host page with __DATA (i386 segments are 4 KB aligned) had every data store there fault and
 * throw the page's translations away, and a lock-prefixed store racing that from another thread killed the process. */
static void protect_segments(M32Image *img)
{
    for (int i = 0; i < img->nseg; i++) {
        const M32Seg *g = &img->seg[i];
        if (!g->vmsize || (!g->filesize && !g->initprot))
            continue;
        ocerz_protect(g->vmaddr + (uint32_t)img->slide, (g->vmsize + 0xfffu) & ~0xfffu, (int)(g->initprot & 7));
    }
}

/* Apply rebases and every bind of one image; returns the number of required imports left unresolved. */
int m32_image_fixup(M32Image *img)
{
    if (!img->has_dyld_info) {
        int bad = legacy_fixups(img);
        m32_cf_alias_strings(img);
        m32_tlv_register(img);
        m32_objc_load(img);
        protect_segments(img);
        return bad;
    }
    rebase(img);
    int bad = bind_stream(img, img->bind_off, img->bind_size, 0, 0);
    bad += bind_stream(img, img->lazy_off, img->lazy_size, 1, 0);
    bind_stream(img, img->weak_off, img->weak_size, 0, 1);
    m32_cf_alias_strings(img);
    m32_tlv_register(img);
    m32_objc_load(img);
    protect_segments(img);
    return bad;
}

/* ---- guest dylibs ---- */

static char g_exe_dir[1024], g_guest32_root[1024];

static void guest32_root(void)
{
    if (g_guest32_root[0])
        return;
    const char *env = getenv("OCERZ_GUEST32_ROOT");
    if (env && *env) {
        snprintf(g_guest32_root, sizeof g_guest32_root, "%s", env);
        return;
    }
    char exe[1024], real[1024];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0 && realpath(exe, real) && strrchr(real, '/')) {
        *strrchr(real, '/') = 0;
        snprintf(g_guest32_root, sizeof g_guest32_root, "%s/runtime/guest32", real);
    }
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* expand @executable_path, @loader_path and @rpath; out is empty when the name is a system library */
int m32_resolve_path(const M32Image *loader, const char *name, char *out, size_t outlen)
{
    out[0] = 0;
    char buf[2048];
    if (!strncmp(name, "@executable_path/", 17))
        snprintf(buf, sizeof buf, "%s/%s", g_exe_dir, name + 17);
    else if (!strncmp(name, "@loader_path/", 13) && loader) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", loader->path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        snprintf(buf, sizeof buf, "%s/%s", dir, name + 13);
    } else if (!strncmp(name, "@rpath/", 7)) {
        for (int pass = 0; pass < 2; pass++) {
            const M32Image *img = pass == 0 ? loader : (m32_nimages ? m32_images[0] : NULL);
            for (int i = 0; img && i < img->nrpath; i++) {
                char rp[1024], cand[2048];
                m32_resolve_path(img, img->rpath[i], rp, sizeof rp);
                if (!rp[0])
                    snprintf(rp, sizeof rp, "%s", img->rpath[i]);
                snprintf(cand, sizeof cand, "%s/%s", rp, name + 7);
                if (file_exists(cand) && realpath(cand, out))
                    return 0;
            }
        }
        return -1;
    } else {
        guest32_root();
        snprintf(buf, sizeof buf, "%s%s", g_guest32_root, name);   /* our i386 runtime (libstdc++, libgcc_s) */
        if (name[0] == '/' && file_exists(buf) && realpath(buf, out))
            return 0;
        if (!strncmp(name, "/usr/lib/", 9) || !strncmp(name, "/System/", 8))
            return 0;   /* a system library: bridged */
        snprintf(buf, sizeof buf, "%s", name);
    }
    if (file_exists(buf) && realpath(buf, out))
        return 0;
    return -1;
}

static M32Image *find_loaded(const char *path)
{
    for (int i = 0; i < m32_nimages; i++)
        if (!strcmp(m32_images[i]->path, path))
            return m32_images[i];
    return NULL;
}

/* load every guest dependency of img, recursively; returns -1 when a required one is missing */
static int load_deps(M32Image *img)
{
    for (int i = 0; i < img->ndeps; i++) {
        char path[1024];
        if (m32_resolve_path(img, img->deps[i], path, sizeof path) != 0) {
            if (img->dep_weak[i])
                continue;
            fprintf(stderr, "ocerz: m32: %s: cannot find %s\n", img->path, img->deps[i]);
            return -1;
        }
        if (!path[0]) {   /* system: open it now, so the classes of one used only for them (WebKit) exist */
            ocerz_bridge_host_library(img->deps[i]);
            continue;
        }
        M32Image *d = find_loaded(path);
        if (!d) {
            d = m32_image_load(path, 0);
            if (!d || load_deps(d) != 0)
                return -1;
        }
        img->dep_img[i] = d;
    }
    return 0;
}

/* ---- exports of a guest image: the dyld-info trie, or the symbol table of an older image ---- */

static uint32_t lookup_in(M32Image *img, const char *name, int depth);

static uint32_t trie_lookup(M32Image *img, const char *name, int depth)
{
    const uint8_t *start = img->file + img->export_off, *end = start + img->export_size, *p = start;
    const char *s = name;
    while (p < end) {
        uint64_t term = uleb(&p, end);
        const uint8_t *children = p + term;
        if (!*s && term) {
            uint64_t flags = uleb(&p, end);
            if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
                uint64_t ord = uleb(&p, end);
                const char *other = (const char *)p;
                if (ord >= 1 && ord <= (uint64_t)img->ndeps) {
                    const char *want = *other ? other : name;
                    M32Image *d = img->dep_img[ord - 1];
                    return d ? lookup_in(d, want, depth + 1) : m32_export(img->deps[ord - 1], want);
                }
                return 0;
            }
            uint64_t off = uleb(&p, end);
            if (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
                return 0;   /* ponytail: resolvers are not run; none of the i386 libraries we load has one */
            return (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
                       ? (uint32_t)off : img->base + (uint32_t)off;
        }
        p = children;
        if (p >= end)
            return 0;
        uint8_t n = *p++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < n && p < end; i++) {
            const char *edge = (const char *)p;
            size_t elen = strnlen(edge, (size_t)(end - p));
            p += elen + 1;
            uint64_t child = uleb(&p, end);
            if (!next && !strncmp(s, edge, elen)) {
                s += elen;
                next = start + child;
            }
        }
        if (!next)
            return 0;
        p = next;
    }
    return 0;
}

static uint32_t nlist_lookup(M32Image *img, const char *name)
{
    for (uint32_t i = 0; i < img->nsyms; i++) {
        const uint8_t *nl = img->file + img->symoff + i * 12;   /* struct nlist: strx, type, sect, desc, value */
        uint8_t type = nl[4];
        if ((type & N_TYPE) != N_SECT || !(type & N_EXT))
            continue;
        uint32_t strx = le32(nl);
        if (strx < img->strsize && !strcmp((const char *)img->file + img->stroff + strx, name))
            return le32(nl + 8) + (uint32_t)img->slide;
    }
    return 0;
}

static uint32_t lookup_in(M32Image *img, const char *name, int depth)
{
    if (depth > 8)
        return 0;
    uint32_t v = img->export_size ? trie_lookup(img, name, depth) : nlist_lookup(img, name);
    for (int i = 0; !v && i < img->ndeps; i++)   /* LC_REEXPORT_DYLIB */
        if (img->dep_reexport[i])
            v = img->dep_img[i] ? lookup_in(img->dep_img[i], name, depth + 1) : m32_export(img->deps[i], name);
    return v;
}

uint32_t m32_image_symbol(M32Image *img, const char *name)
{
    return lookup_in(img, name, 0);
}

/* Where an import of img, by two-level ordinal, comes from. */
uint32_t m32_resolve(M32Image *img, int ordinal, const char *name, int weak_import)
{
    uint32_t v = 0;
    const char *lib = NULL;
    if (ordinal > 0 && ordinal <= img->ndeps) {
        lib = img->deps[ordinal - 1];
        M32Image *d = img->dep_img[ordinal - 1];
        v = d ? lookup_in(d, name, 0) : m32_export(lib, name);
    } else if (ordinal == BIND_SPECIAL_DYLIB_SELF) {
        v = lookup_in(img, name, 0);
    } else if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) {
        v = m32_nimages ? lookup_in(m32_images[0], name, 0) : 0;
    } else {   /* flat lookup: every image in load order, then the system */
        for (int i = 0; !v && i < m32_nimages; i++)
            v = lookup_in(m32_images[i], name, 0);
        if (!v)
            v = m32_export(NULL, name);
    }
    if (!v && !weak_import)
        fprintf(stderr, "ocerz: m32: %s: unresolved %s from %s\n", img->path, name, lib ? lib : "(flat)");
    return v;
}

/* the main executable and everything it needs: loaded, fixed up (dependencies first); initializers are the caller's */
M32Image *m32_load_program(const char *path)
{
    char real[1024];
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", realpath(path, real) ? real : path);
    char *slash = strrchr(g_exe_dir, '/');
    if (slash) *slash = 0;
    M32Image *main_img = m32_image_load(path, 1);
    if (!main_img || load_deps(main_img) != 0)
        return NULL;
    return main_img;
}

int m32_fixup_all(int from)
{
    int bad = 0;
    for (int i = m32_nimages - 1; i >= from; i--)
        bad += m32_image_fixup(m32_images[i]);
    return bad;
}

/* __mod_init_func (S_MOD_INIT_FUNC_POINTERS): 4-byte function pointers, called as fn(argc, argv, envp, apple, vars) */
void m32_run_initializers(struct OcerzVM *vm, M32Image *img, const uint32_t args[5])
{
    m32_objc_run_loads(img);   /* +load before C++ static initializers, as dyld and objc do */
    for (int i = 0; i < img->nsect; i++) {
        if ((img->sect[i].flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS)
            continue;
        uint32_t at = img->sect[i].addr + (uint32_t)img->slide;
        for (uint32_t k = 0; k + 4 <= img->sect[i].size; k += 4) {
            uint32_t fn = m32_rd(at + k);
            if (fn)
                m32_call(vm, fn, args, 5, NULL, NULL);
        }
    }
}

/* ---- dlopen and the dyld image APIs ---- */

#include "ocerz/interp.h"
#include "ocerz/vm.h"

#define DL_SYSTEM_MAGIC 0x4d33534cu   /* a dlopen handle for a system library: {magic, index} */
#define RTLD_DEFAULT32 0xfffffffeu
#define RTLD_NEXT32    0xffffffffu
#define RTLD_SELF32    0xfffffffdu
#define RTLD_MAIN32    0xfffffffbu

static pthread_mutex_t g_dl_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_syslibs[256];
static uint32_t g_syshandles[256];
static int g_nsyslibs;
static __thread uint32_t t_dlerror;
static uint32_t g_add_image_cb[32];
static int g_nadd_image_cb;
uint32_t m32_argc_var, m32_argv_var, m32_environ_var, m32_progname_var;

static void set_dlerror(const char *fmt, const char *what)
{
    char buf[1200];
    snprintf(buf, sizeof buf, fmt, what);
    t_dlerror = m32_cstring(buf);
}

static uint32_t system_handle(const char *install)
{
    for (int i = 0; i < g_nsyslibs; i++)
        if (!strcmp(g_syslibs[i], install))
            return g_syshandles[i];
    if (g_nsyslibs >= 256)
        return 0;
    uint32_t h = m32_static_alloc(8, 8);
    m32_wr(h, DL_SYSTEM_MAGIC);
    m32_wr(h + 4, (uint32_t)g_nsyslibs);
    g_syslibs[g_nsyslibs] = strdup(install);
    g_syshandles[g_nsyslibs++] = h;
    return h;
}

static void run_add_image_callbacks(struct OcerzVM *vm, int from, int cb_from, int cb_to)
{
    for (int c = cb_from; c < cb_to; c++)
        for (int i = from; i < m32_nimages; i++) {
            uint32_t args[2] = { m32_images[i]->base, (uint32_t)m32_images[i]->slide };
            m32_call(vm, g_add_image_cb[c], args, 2, NULL, NULL);
        }
}

static int sp_dlopen(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t pg = m32_arg(cpu, 0);
    if (!pg) {
        m32_ret(cpu, RTLD_DEFAULT32, 0, 0);   /* dlopen(NULL): the program */
        return OCERZ_STEP_OK;
    }
    const char *name = m32_h(pg);
    char path[1024];
    pthread_mutex_lock(&g_dl_lock);
    uint32_t handle = 0;
    if (m32_resolve_path(m32_nimages ? m32_images[0] : NULL, name, path, sizeof path) != 0) {
        set_dlerror("dlopen(%s): image not found", name);
    } else if (!path[0]) {
        handle = system_handle(name);
    } else {
        M32Image *img = find_loaded(path);
        if (img)
            handle = img->base;
        else {
            int from = m32_nimages;
            img = m32_image_load(path, 0);
            if (!img || load_deps(img) != 0) {
                set_dlerror("dlopen(%s): cannot load the image or its dependencies", name);
            } else {
                m32_fixup_all(from);
                uint32_t init_args[5] = { m32_rd(m32_argc_var), m32_rd(m32_argv_var), m32_rd(m32_environ_var), 0, 0 };
                pthread_mutex_unlock(&g_dl_lock);
                for (int i = m32_nimages - 1; i >= from; i--)
                    m32_run_initializers(vm, m32_images[i], init_args);
                run_add_image_callbacks(vm, from, 0, g_nadd_image_cb);
                pthread_mutex_lock(&g_dl_lock);
                handle = img->base;
            }
        }
    }
    pthread_mutex_unlock(&g_dl_lock);
    m32_ret(cpu, handle, 0, 0);
    return OCERZ_STEP_OK;
}

static uint32_t system_symbol(const char *lib, const char *sym)
{
    uint32_t v = m32_export(lib, sym);
    uint32_t id = v - (uint32_t)OCERZ_DYLDAPI_LO;
    M32Export *e = v - OCERZ_DYLDAPI_LO < OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO ? m32_export_by_id(id) : NULL;
    return e && e->kind == M32_EX_UNRESOLVED ? 0 : v;
}

static int sp_dlsym(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t h = m32_arg(cpu, 0), sp = m32_arg(cpu, 1);
    char sym[512];
    snprintf(sym, sizeof sym, "_%s", sp ? (const char *)m32_h(sp) : "");
    uint32_t v = 0;
    if (h == RTLD_DEFAULT32 || h == RTLD_NEXT32 || h == RTLD_SELF32 || h == RTLD_MAIN32) {
        for (int i = 0; !v && i < m32_nimages; i++)
            v = lookup_in(m32_images[i], sym, 0);
        if (!v && h != RTLD_MAIN32)
            v = system_symbol(NULL, sym);
    } else if (h && m32_rd(h) == DL_SYSTEM_MAGIC && m32_rd(h + 4) < (uint32_t)g_nsyslibs) {
        v = system_symbol(g_syslibs[m32_rd(h + 4)], sym);
    } else {
        for (int i = 0; i < m32_nimages; i++)
            if (m32_images[i]->base == h)
                v = lookup_in(m32_images[i], sym, 0);
    }
    if (!v)
        set_dlerror("dlsym: symbol %s not found", sym + 1);
    m32_ret(cpu, v, 0, 0);
    return OCERZ_STEP_OK;
}

static int sp_dlclose(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, 0, 0, 0); return OCERZ_STEP_OK; }

static int sp_dlerror(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t e = t_dlerror;
    t_dlerror = 0;
    m32_ret(cpu, e, 0, 0);
    return OCERZ_STEP_OK;
}

static M32Image *image_at(uint32_t addr)
{
    for (int i = 0; i < m32_nimages; i++)
        if (addr >= m32_images[i]->lo && addr < m32_images[i]->hi)
            return m32_images[i];
    return NULL;
}

/* dladdr(addr, Dl_info *): i386 Dl_info is {dli_fname, dli_fbase, dli_sname, dli_saddr} */
static int sp_dladdr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t addr = m32_arg(cpu, 0), info = m32_arg(cpu, 1);
    M32Image *img = image_at(addr);
    if (!img || !info) {
        m32_ret(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    uint32_t best = 0, best_name = 0;
    for (uint32_t i = 0; i < img->nsyms; i++) {   /* the nearest external symbol at or below addr */
        const uint8_t *nl = img->file + img->symoff + i * 12;
        if ((nl[4] & N_TYPE) != N_SECT || !(nl[4] & N_EXT))
            continue;
        uint32_t a = le32(nl + 8) + (uint32_t)img->slide;
        if (a <= addr && a >= best) {
            best = a;
            best_name = le32(nl);
        }
    }
    m32_wr(info, m32_cstring(img->path));
    m32_wr(info + 4, img->base);
    m32_wr(info + 8, best && best_name < img->strsize ? m32_cstring((const char *)img->file + img->stroff + best_name + 1) : 0);
    m32_wr(info + 12, best);
    m32_ret(cpu, 1, 0, 0);
    return OCERZ_STEP_OK;
}

static int sp_image_count(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, (uint32_t)m32_nimages, 0, 0); return OCERZ_STEP_OK; }
static int sp_image_name(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t i = m32_arg(cpu, 0);
    m32_ret(cpu, i < (uint32_t)m32_nimages ? m32_cstring(m32_images[i]->path) : 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_image_header(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t i = m32_arg(cpu, 0);
    m32_ret(cpu, i < (uint32_t)m32_nimages ? m32_images[i]->base : 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_image_slide(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t i = m32_arg(cpu, 0);
    m32_ret(cpu, i < (uint32_t)m32_nimages ? (uint32_t)m32_images[i]->slide : 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_register_add_image(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t fn = m32_arg(cpu, 0);
    int c = -1;
    pthread_mutex_lock(&g_dl_lock);
    if (fn && g_nadd_image_cb < 32) {
        c = g_nadd_image_cb;
        g_add_image_cb[g_nadd_image_cb++] = fn;
    }
    pthread_mutex_unlock(&g_dl_lock);
    if (c >= 0)
        run_add_image_callbacks(vm, 0, c, c + 1);   /* dyld calls it once per image already loaded */
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_ret0(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, 0, 0, 0); return OCERZ_STEP_OK; }

static int sp_NSGetExecutablePath(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t buf = m32_arg(cpu, 0), szp = m32_arg(cpu, 1);
    const char *p = m32_nimages ? m32_images[0]->path : "";
    uint32_t need = (uint32_t)strlen(p) + 1, have = m32_rd(szp);
    if (need > have) {
        m32_wr(szp, need);
        m32_ret(cpu, 0xffffffffu, 0, 0);
        return OCERZ_STEP_OK;
    }
    memcpy(m32_h(buf), p, need);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_NSGetArgc(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, m32_argc_var, 0, 0); return OCERZ_STEP_OK; }
static int sp_NSGetArgv(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, m32_argv_var, 0, 0); return OCERZ_STEP_OK; }
static int sp_NSGetEnviron(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, m32_environ_var, 0, 0); return OCERZ_STEP_OK; }
static int sp_NSGetProgname(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, m32_progname_var, 0, 0); return OCERZ_STEP_OK; }
static int sp_getprogname(struct OcerzVM *vm, OcerzCPU *cpu) { m32_ret(cpu, m32_rd(m32_progname_var), 0, 0); return OCERZ_STEP_OK; }

/* getsectdata family: answered from the m32 images (the main executable unless a header is given) */
static uint32_t sect_addr(M32Image *img, const char *seg, const char *sect, uint32_t *size)
{
    const M32Sect *s = img ? m32_section(img, seg, sect) : NULL;
    *size = s ? s->size : 0;
    return s ? s->addr + (uint32_t)img->slide : 0;
}
static int sp_getsectdata(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t size = 0, a = sect_addr(m32_nimages ? m32_images[0] : NULL, m32_h(m32_arg(cpu, 0)), m32_h(m32_arg(cpu, 1)), &size);
    uint32_t szp = m32_arg(cpu, 2);
    if (szp)
        m32_wr(szp, size);
    m32_ret(cpu, a ? a - (uint32_t)m32_images[0]->slide : 0, 0, 0);   /* getsectdata answers the unslid address */
    return OCERZ_STEP_OK;
}
static int sp_getsectdatafromheader(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t mh = m32_arg(cpu, 0), size = 0, a = 0;
    M32Image *img = image_at(mh);
    a = sect_addr(img, m32_h(m32_arg(cpu, 1)), m32_h(m32_arg(cpu, 2)), &size);
    uint32_t szp = m32_arg(cpu, 3);
    if (szp)
        m32_wr(szp, size);
    m32_ret(cpu, a ? a - (uint32_t)img->slide : 0, 0, 0);
    return OCERZ_STEP_OK;
}

const M32SpecialEntry m32_dyld_specials[] = {
    { "_dlopen", sp_dlopen }, { "_dlsym", sp_dlsym }, { "_dlclose", sp_dlclose }, { "_dlerror", sp_dlerror },
    { "_dladdr", sp_dladdr },
    { "__dyld_image_count", sp_image_count }, { "__dyld_get_image_name", sp_image_name },
    { "__dyld_get_image_header", sp_image_header }, { "__dyld_get_image_vmaddr_slide", sp_image_slide },
    { "__dyld_register_func_for_add_image", sp_register_add_image },
    { "__dyld_register_func_for_remove_image", sp_ret0 },
    { "__NSGetExecutablePath", sp_NSGetExecutablePath }, { "__NSGetArgc", sp_NSGetArgc },
    { "__NSGetArgv", sp_NSGetArgv }, { "__NSGetEnviron", sp_NSGetEnviron }, { "__NSGetProgname", sp_NSGetProgname },
    { "_getprogname", sp_getprogname },
    { "_getsectdata", sp_getsectdata }, { "_getsectdatafromheader", sp_getsectdatafromheader },
    { NULL, NULL }
};
