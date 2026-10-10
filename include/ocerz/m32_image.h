/*
 * m32's i386 images (src/m32_dyld.c), shared by the m32 files only.
 */
#ifndef OCERZ_M32_IMAGE_H
#define OCERZ_M32_IMAGE_H

#include <stdint.h>
#include <stddef.h>

#define M32_MAX_IMAGES 256
#define M32_MAX_SEGS 16
#define M32_MAX_SECTS 96
#define M32_MAX_DEPS 64
#define M32_MAX_RPATHS 8

typedef struct M32Seg {
    char name[17];
    uint32_t vmaddr, vmsize, fileoff, filesize, initprot;
} M32Seg;

typedef struct M32Sect {
    char seg[17], name[17];
    uint32_t addr, size, offset, flags;   /* addr before the slide */
    uint32_t reserved1, reserved2;        /* indirect symbol index, stub size */
} M32Sect;

typedef struct M32Image {
    char path[1024];
    char install_name[1024];
    uint8_t *buf;                 /* the whole file */
    const uint8_t *file;          /* the i386 slice */
    size_t flen;
    uint32_t filetype;
    int32_t slide;
    uint32_t base, lo, hi;        /* mach header, extent (after the slide) */
    uint32_t entry_main, entry_main_off, entry_thread;
    int has_main;
    M32Seg seg[M32_MAX_SEGS];
    int nseg;
    M32Sect sect[M32_MAX_SECTS];
    int nsect;
    uint32_t rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size;
    uint32_t lazy_off, lazy_size, export_off, export_size;
    uint32_t symoff, nsyms, stroff, strsize;
    uint32_t indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;   /* LC_DYSYMTAB */
    int has_dyld_info;
    char *deps[M32_MAX_DEPS];
    int dep_weak[M32_MAX_DEPS], dep_reexport[M32_MAX_DEPS];
    struct M32Image *dep_img[M32_MAX_DEPS];   /* NULL: a system library */
    int ndeps;
    char *rpath[M32_MAX_RPATHS];
    int nrpath;
} M32Image;

extern M32Image *m32_images[M32_MAX_IMAGES];
extern int m32_nimages;

M32Image *m32_image_load(const char *path, int is_main);
M32Image *m32_load_program(const char *path);          /* main image and its guest dependencies, not yet fixed up */
int m32_fixup_all(int from);                            /* fix up m32_images[from..], dependencies first */
int m32_resolve_path(const M32Image *loader, const char *name, char *out, size_t outlen);   /* "" = system */
uint32_t m32_image_symbol(M32Image *img, const char *name);
int m32_image_fixup(M32Image *img);
uint32_t m32_resolve(M32Image *img, int ordinal, const char *name, int weak_import);
const M32Sect *m32_section(const M32Image *img, const char *seg, const char *sect);
void m32_cf_alias_strings(M32Image *img);   /* src/m32_cf.c */
void m32_tlv_register(M32Image *img);       /* src/m32_thread.c */
struct OcerzVM;
void m32_run_initializers(struct OcerzVM *vm, M32Image *img, const uint32_t args[5]);

#endif
