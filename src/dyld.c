/*
 * src/dyld.c
 *
 * Ocerz's mini-dyld implementation (dyld.h): load, link and launch a
 * dynamically-linked x86_64 LC_MAIN executable against the shared cache,
 * doing the work Apple's dyld would, so no real dyld runs.
 *
 * Flow of ocerz_dyld_run:
 *  1. Identity memory mode (guest_base = 0) with a reserved low arena for the
 *     executable, stack and heap; the shared cache is mapped separately at its
 *     own absolute addresses by ocerz_cache_map.
 *  2. Read the executable, require MH_PIE, and choose a slide so its lowest
 *     real segment (the fileoff==0 __TEXT) lands at LOAD_BASE. Map every
 *     LC_SEGMENT_64 (skipping __PAGEZERO) at vmaddr+slide, pread its file
 *     bytes, and apply its protections (guest exec dropped to read).
 *  3. Apply LC_DYLD_CHAINED_FIXUPS. The fixups metadata (header, starts,
 *     imports, symbol strings) is read straight from the file image; the
 *     chains themselves are walked in the now-mapped segments. For the
 *     DYLD_CHAINED_PTR_64 / _64_OFFSET formats each chain slot is either a
 *     rebase (an internal pointer: load_base + target for _OFFSET, or
 *     vmaddr + slide for the plain _64 form, OR'd with any high8 byte) or a
 *     bind (an import: the 24-bit ordinal selects an entry in the imports
 *     table whose symbol name is resolved with ocerz_cache_resolve and stored
 *     with its addend). The 12-bit next field strides the chain by 4 bytes.
 *  4. Build the entry frame: a guest stack with argc/argv/envp/apple and the
 *     C string data, a tiny exit trampoline pushed as main's return address
 *     (so `ret` from main becomes exit(status)), and the System V argument
 *     registers rdi=argc, rsi=argv, rdx=envp, rcx=apple. cpu.rip is set to
 *     main (text_vmaddr + LC_MAIN.entryoff + slide) and ocerz_vm_run drives it.
 *
 * Deliberately not done yet (the honest edge): initializers (libSystem's
 * +load/__mod_init_func) are NOT run, TLV is not set up, and re-exported /
 * cache-export-table imports are not resolved. This is enough to launch a
 * program and reach main; functions whose correctness depends on libSystem
 * initialization will expose the next round of work, which is the point of
 * getting here.
 */
#include "ocerz/dyld.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/dyldapi.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#define DYN_ARENA_SIZE (4ull << 30)
#define DYN_STACK_SIZE (8ull << 20)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        close(fd);
        return NULL;
    }
    if (pread(fd, buf, (size_t)sz, 0) != (ssize_t)sz) {
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);
    *len_out = (size_t)sz;
    return buf;
}

static const uint8_t *select_slice(const uint8_t *buf, size_t len)
{
    uint32_t magic = rd32(buf);
    if (magic == MH_MAGIC_64)
        return buf;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        int swap = (magic == FAT_CIGAM || magic == FAT_CIGAM_64);
        int is64 = (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64);
        uint32_t nfat = rd32(buf + 4);
        if (swap)
            nfat = __builtin_bswap32(nfat);
        const uint8_t *fa = buf + 8;
        size_t stride = is64 ? 32 : 20;
        for (uint32_t i = 0; i < nfat; i++) {
            const uint8_t *e = fa + i * stride;
            uint32_t cputype = rd32(e);
            uint64_t off = is64 ? rd64(e + 8) : rd32(e + 8);
            if (swap) {
                cputype = __builtin_bswap32(cputype);
                off = is64 ? __builtin_bswap64(off) : __builtin_bswap32((uint32_t)off);
            }
            if (cputype == CPU_TYPE_X86_64 && off < len)
                return buf + off;
        }
    }
    return NULL;
}

int ocerz_peek_dynamic(const char *path)
{
    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf)
        return -1;
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        free(buf);
        return -1;
    }
    uint32_t ncmds = rd32(slice + 16);
    const uint8_t *lc = slice + sizeof(struct mach_header_64);
    int dynamic = -1;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000028) {
            dynamic = 1;
            break;
        }
        if (cmd == LC_UNIXTHREAD) {
            dynamic = 0;
            break;
        }
        lc += rd32(lc + 4);
    }
    free(buf);
    return dynamic;
}

typedef struct DynImage {
    const uint8_t *slice;
    uint64_t slide;
    uint64_t load_base;
    uint64_t main_entry;
    uint32_t cf_off;
    uint32_t cf_size;
    int is_pie;
    int links_dylib;
} DynImage;

static int map_segments(DynImage *img)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    img->is_pie = (rd32(mh + 24) & MH_PIE) != 0;
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint64_t text_vmaddr = 0;
    int have_text = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64 && rd64(lc + 40) == 0 && rd64(lc + 24) != 0) {
            text_vmaddr = rd64(lc + 24);
            have_text = 1;
            break;
        }
        lc += rd32(lc + 4);
    }
    if (!have_text)
        return OCERZ_EFORMAT;
    img->load_base = ocerz_arena_lo;
    img->slide = img->load_base - text_vmaddr;

    uint64_t vmlo = ~0ull, vmhi = 0;
    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 56);
            if (!(vmaddr == 0 && initprot == 0)) {
                if (vmaddr < vmlo)
                    vmlo = vmaddr;
                if (vmaddr + vmsize > vmhi)
                    vmhi = vmaddr + vmsize;
            }
        }
        lc += rd32(lc + 4);
    }
    if (vmhi <= vmlo)
        return OCERZ_EFORMAT;
    if (ocerz_map_fixed(vmlo + img->slide, vmhi - vmlo, PROT_READ | PROT_WRITE) != OCERZ_OK)
        return OCERZ_ENOMEM;

    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t fileoff = rd64(lc + 40);
            uint64_t filesize = rd64(lc + 48);
            uint32_t initprot = rd32(lc + 56);
            if (!(vmaddr == 0 && initprot == 0) && filesize)
                memcpy(ocerz_g2h(vmaddr + img->slide), img->slice + fileoff, (size_t)filesize);
        } else if (cmd == 0x80000028) {
            img->main_entry = text_vmaddr + rd64(lc + 8) + img->slide;
        } else if (cmd == 0x80000034) {
            img->cf_off = rd32(lc + 8);
            img->cf_size = rd32(lc + 12);
        } else if (cmd == 0x0c || cmd == 0x8000001f || cmd == 0x80000018) {
            img->links_dylib = 1;
        }
        lc += csize;
    }
    return OCERZ_OK;
}

static int apply_fixups(DynImage *img, OcerzCache *cache)
{
    if (img->cf_off == 0)
        return OCERZ_OK;
    const uint8_t *cf = img->slice + img->cf_off;
    uint32_t starts_off = rd32(cf + 4);
    uint32_t imports_off = rd32(cf + 8);
    uint32_t symbols_off = rd32(cf + 12);
    uint32_t imports_cnt = rd32(cf + 16);

    const uint8_t *sii = cf + starts_off;
    uint32_t seg_count = rd32(sii);
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so = rd32(sii + 4 + s * 4);
        if (so == 0)
            continue;
        const uint8_t *sis = sii + so;
        uint16_t page_size = rd16(sis + 4);
        uint16_t ptr_format = rd16(sis + 6);
        uint64_t seg_off = rd64(sis + 8);
        uint16_t page_count = rd16(sis + 0x14);
        const uint8_t *page_start = sis + 0x16;
        if (ptr_format != 2 && ptr_format != 6) {
            OCERZ_FATAL("unsupported chained pointer format %u\n", ptr_format);
            return OCERZ_EUNSUP;
        }
        for (uint16_t pg = 0; pg < page_count; pg++) {
            uint16_t start = rd16(page_start + pg * 2);
            if (start == 0xffff)
                continue;
            uint64_t addr = img->load_base + seg_off + (uint64_t)pg * page_size + start;
            for (;;) {
                uint64_t raw = ocerz_ld(addr, 8);
                int bind = (int)(raw >> 63) & 1;
                uint32_t next = (uint32_t)((raw >> 51) & 0xfff);
                if (bind) {
                    uint32_t ordinal = (uint32_t)(raw & 0xffffff);
                    uint64_t addend = (raw >> 24) & 0xff;
                    uint64_t value = 0;
                    if (ordinal < imports_cnt) {
                        uint32_t imp = rd32(cf + imports_off + ordinal * 4);
                        uint32_t noff = imp >> 9;
                        const char *name = (const char *)(cf + symbols_off + noff);
                        value = ocerz_cache_resolve(cache, name);
                        if (value == 0)
                            OCERZ_FATAL("unresolved import: %s\n", name);
                    }
                    ocerz_st(addr, 8, value + addend);
                } else {
                    uint64_t target = raw & 0xfffffffffull;
                    uint64_t high8 = (raw >> 36) & 0xff;
                    uint64_t value = (ptr_format == 6)
                        ? img->load_base + target
                        : target + img->slide;
                    value |= high8 << 56;
                    ocerz_st(addr, 8, value);
                }
                if (next == 0)
                    break;
                addr += (uint64_t)next * 4;
            }
        }
    }
    return OCERZ_OK;
}

typedef struct DynFrame {
    uint64_t argc;
    uint64_t argv_arr;
    uint64_t envp_arr;
    uint64_t apple_arr;
    uint64_t progvars;
    uint64_t stack_top;
    uint64_t exit_stub;
} DynFrame;

static uint64_t put_str(uint64_t *sp, const char *s)
{
    size_t l = strlen(s) + 1;
    *sp -= l;
    memcpy(ocerz_g2h(*sp), s, l);
    return *sp;
}

static int build_frame(const char *path, int argc, char **argv, char **envp, DynFrame *out)
{
    uint64_t aux = ocerz_map_anywhere(1u << 20, PROT_READ | PROT_WRITE);
    uint64_t stack = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
    if (aux == 0 || stack == 0)
        return OCERZ_ENOMEM;

    static const uint8_t exit_stub[] = { 0x89, 0xc7, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05 };
    out->exit_stub = stack + DYN_STACK_SIZE - 64;
    memcpy(ocerz_g2h(out->exit_stub), exit_stub, sizeof exit_stub);
    out->stack_top = (out->exit_stub - 256) & ~0xfull;

    char apple0[2048];
    snprintf(apple0, sizeof apple0, "executable_path=%s", path);

    uint64_t sp = aux + (1u << 20);
    uint64_t argv_g[64], envp_g[64], apple_g[8];
    int envc = 0;
    while (envp && envp[envc] && envc < 60)
        envc++;

    for (int i = argc - 1; i >= 0; i--)
        argv_g[i] = put_str(&sp, argv[i]);
    for (int i = envc - 1; i >= 0; i--)
        envp_g[i] = put_str(&sp, envp[i]);
    char thbuf[64], stkbuf[160];
    snprintf(thbuf, sizeof thbuf, "th_port=0x%x", (unsigned)mach_thread_self());
    snprintf(stkbuf, sizeof stkbuf, "main_stack=0x%llx,0x%llx,0x%llx,0x%llx",
             (unsigned long long)(stack + DYN_STACK_SIZE), (unsigned long long)DYN_STACK_SIZE,
             (unsigned long long)0x4000, (unsigned long long)0x4000);
    apple_g[0] = put_str(&sp, apple0);
    apple_g[1] = put_str(&sp, "stack_guard=0x6f6365727a5f6773");
    apple_g[2] = put_str(&sp, "ptr_munge=0xa3f1c2b4d5e60718");
    apple_g[3] = put_str(&sp, "malloc_entropy=0x91827364a5b6c7d8,0x1f2e3d4c5b6a7988");
    apple_g[4] = put_str(&sp, stkbuf);
    apple_g[5] = put_str(&sp, thbuf);
    int applec = 6;

    sp &= ~0xfull;

    uint64_t argv_arr = sp - (uint64_t)(argc + 1) * 8;
    for (int i = 0; i < argc; i++)
        ocerz_st(argv_arr + (uint64_t)i * 8, 8, argv_g[i]);
    ocerz_st(argv_arr + (uint64_t)argc * 8, 8, 0);

    uint64_t envp_arr = argv_arr - (uint64_t)(envc + 1) * 8;
    for (int i = 0; i < envc; i++)
        ocerz_st(envp_arr + (uint64_t)i * 8, 8, envp_g[i]);
    ocerz_st(envp_arr + (uint64_t)envc * 8, 8, 0);

    uint64_t apple_arr = envp_arr - (uint64_t)(applec + 1) * 8;
    for (int i = 0; i < applec; i++)
        ocerz_st(apple_arr + (uint64_t)i * 8, 8, apple_g[i]);
    ocerz_st(apple_arr + (uint64_t)applec * 8, 8, 0);

    uint64_t cells = (apple_arr - 8 * 8) & ~0xfull;
    ocerz_st(cells + 0, 4, (uint64_t)argc);
    ocerz_st(cells + 8, 8, argv_arr);
    ocerz_st(cells + 16, 8, envp_arr);
    ocerz_st(cells + 24, 8, argv_g[0]);

    uint64_t pv = cells - 48;
    ocerz_st(pv + 0, 8, ocerz_arena_lo);
    ocerz_st(pv + 8, 8, cells + 0);
    ocerz_st(pv + 16, 8, cells + 8);
    ocerz_st(pv + 24, 8, cells + 16);
    ocerz_st(pv + 32, 8, cells + 24);

    out->argc = (uint64_t)argc;
    out->argv_arr = argv_arr;
    out->envp_arr = envp_arr;
    out->apple_arr = apple_arr;
    out->progvars = pv;
    return OCERZ_OK;
}

static uint64_t find_dylib_init(OcerzCache *cache, const char *substr)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (!path || !strstr(path, substr) || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint32_t ns = rd32(lc + 64);
                const uint8_t *sec = lc + 72;
                for (uint32_t s = 0; s < ns; s++) {
                    uint8_t ty = rd32(sec + 64) & 0xff;
                    uint64_t sa = rd64(sec + 32), ssz = rd64(sec + 40);
                    if (ty == 0x16 && ssz >= 4)
                        return mh + rd32((const uint8_t *)(uintptr_t)sa);
                    if (ty == 0x09 && ssz >= 8)
                        return rd64((const uint8_t *)(uintptr_t)sa);
                    sec += 80;
                }
            }
            lc += rd32(lc + 4);
        }
        return 0;
    }
    return 0;
}

int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    if (ocerz_mem_init_identity(DYN_ARENA_SIZE) != OCERZ_OK)
        return OCERZ_ENOMEM;

    static OcerzCache cache;
    if (ocerz_cache_map(&cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot map shared cache for dynamic loading\n");
        return OCERZ_EIO;
    }

    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf) {
        OCERZ_FATAL("cannot read %s\n", path);
        return OCERZ_EIO;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        OCERZ_FATAL("%s has no x86_64 slice\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    DynImage img;
    memset(&img, 0, sizeof img);
    img.slice = slice;
    int r = map_segments(&img);
    if (r != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", path);
        free(buf);
        return r;
    }
    if (!img.is_pie) {
        OCERZ_FATAL("%s is not PIE; dynamic loading needs a slidable image\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }
    if (img.main_entry == 0) {
        OCERZ_FATAL("%s has no LC_MAIN entry\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    r = apply_fixups(&img, &cache);
    if (r != OCERZ_OK) {
        free(buf);
        return r;
    }

    DynFrame fr;
    memset(&fr, 0, sizeof fr);
    if (build_frame(path, argc, argv, envp, &fr) != OCERZ_OK) {
        OCERZ_FATAL("cannot build dynamic entry frame\n");
        free(buf);
        return OCERZ_ENOMEM;
    }

    uint64_t tsd = ocerz_map_anywhere(0x8000, PROT_READ | PROT_WRITE);
    if (tsd == 0) {
        free(buf);
        return OCERZ_ENOMEM;
    }
    uint64_t self = tsd + 0x4000;
    uint64_t gs = self + 0xe0;
    vm->cpu.gs_base = gs;
    ocerz_st(gs, 8, self);

    free(buf);

    ocerz_vm_install_handlers(vm);
    ocerz_commpage_init();
    if (ocerz_dyldapi_setup(&cache) != OCERZ_OK)
        OCERZ_LOG("dynamic: dyld API shim not installed\n");

    int ran_init = 0;
    int want_init = !getenv("OCERZ_NOINIT") && (img.links_dylib || getenv("OCERZ_INIT"));
    if (want_init) {
        uint64_t init = find_dylib_init(&cache, "libSystem.B.dylib");
        if (init) {
            uint64_t ia[5] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars };
            OCERZ_LOG("dynamic: running libSystem_initializer at %#llx\n", (unsigned long long)init);
            ocerz_vm_call(vm, init, ia, 5, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: libSystem_initializer returned\n");
            ran_init = 1;
        } else {
            OCERZ_LOG("dynamic: no libSystem initializer found\n");
        }
    }

    OCERZ_LOG("dynamic: load_base=%#llx slide=%#llx main=%#llx\n",
              (unsigned long long)img.load_base, (unsigned long long)img.slide,
              (unsigned long long)img.main_entry);

    if (ran_init) {
        uint64_t margs[4] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr };
        uint64_t rv = ocerz_vm_call(vm, img.main_entry, margs, 4, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
        uint64_t exit_fn = ocerz_cache_resolve(&cache, "_exit");
        if (exit_fn) {
            uint64_t ea[1] = { rv & 0xff };
            ocerz_vm_call(vm, exit_fn, ea, 1, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
        }
        return (int)(rv & 0xff);
    }

    vm->cpu.gpr[OCERZ_RDI] = fr.argc;
    vm->cpu.gpr[OCERZ_RSI] = fr.argv_arr;
    vm->cpu.gpr[OCERZ_RDX] = fr.envp_arr;
    vm->cpu.gpr[OCERZ_RCX] = fr.apple_arr;
    vm->cpu.gpr[OCERZ_R8] = fr.progvars;
    uint64_t rsp = (fr.stack_top & ~0xfull) - 8;
    ocerz_st(rsp, 8, fr.exit_stub);
    vm->cpu.gpr[OCERZ_RSP] = rsp;
    vm->cpu.rip = img.main_entry;
    return ocerz_vm_run(vm);
}
