/*
 * src/dyldapi.c
 *
 * The dyld runtime API shim (dyldapi.h): install a process-wide "dyld APIs"
 * object so the genuine libdyld.dylib trampolines work, and implement the
 * dyld services they invoke natively in Ocerz.
 *
 * Setup. The libdyld trampolines all begin `mov rdi,[apis]; mov rax,[rdi];
 * jmp [rax+slot]`, loading one global. ocerz_dyldapi_setup finds that global
 * robustly by resolving _dyld_get_active_platform and decoding its first
 * rip-relative load (`48 8b 3d <disp32>` after the `55 48 89 e5` prologue) to
 * recover the global's address, then builds two guest objects: a vtable of
 * 0x2000 bytes whose every 8-byte slot at offset OFF holds the native-thunk
 * address OCERZ_DYLDAPI_LO+OFF, and an APIs object whose first qword points at
 * that vtable. The object pointer is written into the libdyld global. From
 * then on any `jmp [vtable+OFF]` lands on a native-thunk address.
 *
 * Dispatch. The interpreter recognises an rip in [OCERZ_DYLDAPI_LO,
 * OCERZ_DYLDAPI_HI) and calls ocerz_dyldapi_dispatch, where the vtable byte
 * offset is rip-OCERZ_DYLDAPI_LO. Because the trampolines shift arguments down
 * by one (the object becomes `this`=rdi), a method sees this=rdi and its own
 * arguments in rsi/rdx/rcx/r8/r9. Each handler computes a result into rax and
 * returns to the guest by popping the caller's return address off the stack
 * (the trampoline tail-called us, so [rsp] is the original caller). Only the
 * slots a running program actually reaches are implemented; an unimplemented
 * slot logs its offset and returns 0, which is enough to surface the next slot
 * without derailing — slots that matter are then given real behaviour.
 *
 * The platform is reported as PLATFORM_MACOS (1). Image/dlopen/TLV services
 * are added here as programs reach them.
 *
 * The objc<->dyld handshake. libobjc's _objc_init registers callbacks through
 * slot 0x358 (_dyld_objc_register_callbacks) passing a version-4 struct
 * {version@0, mapped@8, init@0x10, unmapped@0x18, patches@0x20}. Real dyld
 * answers by synchronously invoking the `mapped` callback with the
 * already-loaded objc images, which is what drives objc's _read_images /
 * preopt_init / class realization; without it objc later dereferences a NULL
 * shared-cache selector table. Ocerz replicates this: it builds an array of
 * 32-byte _dyld_objc_notify_mapped_info entries {mach_header@0, objcImageInfo@8,
 * path@0x10, sectionLocations@0x18} for every loaded objc image (libobjc plus
 * the /usr/lib/system objc dylibs in the libSystem closure) and calls `mapped`
 * via a nested ocerz_vm_call. map_images_nolock then asks dyld for the
 * shared-cache objc-optimization roots, which Ocerz derives from libobjc's
 * __TEXT,__objc_opt_ro (an objc_opt_t v16 whose i32 fields at +0x0c/+0x18 give
 * the header-info RO/RW tables): slot 0x3a8 (header_opt_rw) and 0x3b0
 * (header_opt_ro) return those, slot 0x1f8 returns the shared-cache range, and
 * slot 0x378 (per-image section getter) returns 0 so objc walks the Mach-O.
 */
#include "ocerz/dyldapi.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/interp.h"

#include <sys/mman.h>
#include <string.h>
#include <mach-o/loader.h>

#define DYLDAPI_VTABLE_SIZE 0x2000

static uint64_t g_apis_global;
static struct OcerzCache *g_cache;
static uint64_t g_headeropt_rw;
static uint64_t g_headeropt_ro;
static uint64_t g_cache_start;
static uint64_t g_cache_size;

static uint64_t find_section_any(uint64_t mh, const char *sect)
{
    const struct mach_header_64 *h = (const void *)(uintptr_t)mh;
    if (!mh || h->magic != MH_MAGIC_64)
        return 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++)
                if (strncmp(sc[j].sectname, sect, 16) == 0)
                    return sc[j].addr;
        }
        lc += l->cmdsize;
    }
    return 0;
}

static int is_loaded_objc_image(const char *path, uint64_t mh)
{
    if (!path)
        return 0;
    if (strncmp(path, "/usr/lib/system/", 16) != 0 &&
        strcmp(path, "/usr/lib/libobjc.A.dylib") != 0)
        return 0;
    return find_section_any(mh, "__objc_imageinfo") != 0;
}

int ocerz_dyldapi_setup(struct OcerzCache *cache)
{
    uint64_t fn = ocerz_cache_resolve(cache, "_dyld_get_active_platform");
    if (fn == 0)
        return OCERZ_EUNSUP;
    const uint8_t *p = (const uint8_t *)(uintptr_t)fn;
    if (p[0] != 0x55 || p[1] != 0x48 || p[2] != 0x89 || p[3] != 0xe5)
        return OCERZ_EUNSUP;
    if (p[4] != 0x48 || p[5] != 0x8b || p[6] != 0x3d)
        return OCERZ_EUNSUP;
    int32_t disp;
    memcpy(&disp, p + 7, 4);
    g_apis_global = fn + 11 + (int64_t)disp;

    uint64_t vtable = ocerz_map_anywhere(DYLDAPI_VTABLE_SIZE, PROT_READ | PROT_WRITE);
    uint64_t obj = ocerz_map_anywhere(0x400, PROT_READ | PROT_WRITE);
    if (vtable == 0 || obj == 0)
        return OCERZ_ENOMEM;
    for (uint64_t off = 0; off < DYLDAPI_VTABLE_SIZE; off += 8)
        ocerz_st(vtable + off, 8, OCERZ_DYLDAPI_LO + off);
    ocerz_st(obj, 8, vtable);
    ocerz_st(g_apis_global, 8, obj);

    g_cache = cache;
    g_cache_start = cache->base;
    g_cache_size = 0x40000000000ull;

    uint64_t objc_mh = 0;
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (mh && path && strcmp(path, "/usr/lib/libobjc.A.dylib") == 0) {
            objc_mh = mh;
            break;
        }
    }
    uint64_t opt = objc_mh ? find_section_any(objc_mh, "__objc_opt_ro") : 0;
    if (opt) {
        int32_t ro_off = 0, rw_off = 0;
        memcpy(&ro_off, (const void *)(uintptr_t)(opt + 0x0c), 4);
        memcpy(&rw_off, (const void *)(uintptr_t)(opt + 0x18), 4);
        g_headeropt_ro = ro_off ? opt + (int64_t)ro_off : 0;
        g_headeropt_rw = rw_off ? opt + (int64_t)rw_off : 0;
    }

    OCERZ_LOG("dyldapi: apis_global=%#llx obj=%#llx vtable=%#llx opt=%#llx rw=%#llx ro=%#llx\n",
              (unsigned long long)g_apis_global, (unsigned long long)obj,
              (unsigned long long)vtable, (unsigned long long)opt,
              (unsigned long long)g_headeropt_rw, (unsigned long long)g_headeropt_ro);
    return OCERZ_OK;
}

static void api_return(OcerzCPU *cpu, uint64_t result)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = result;
}

static int api_objc_register_callbacks(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t cb = cpu->gpr[OCERZ_RSI];
    uint64_t mapped = cb ? ocerz_ld(cb + 0x08, 8) : 0;
    if (!mapped || !g_cache) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }

    uint64_t mhs[64], paths[64], iis[64];
    int n = 0;
    for (uint32_t i = 0; i < g_cache->images_cnt && n < 64; i++) {
        const char *path = NULL;
        uint64_t mh = ocerz_cache_image_addr(g_cache, i, &path);
        if (!is_loaded_objc_image(path, mh))
            continue;
        mhs[n] = mh;
        paths[n] = ocerz_h2g(path);
        iis[n] = find_section_any(mh, "__objc_imageinfo");
        n++;
    }
    if (n == 0) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }

    uint64_t infos = ocerz_map_anywhere((uint64_t)n * 0x20, PROT_READ | PROT_WRITE);
    if (!infos) {
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
    for (int k = 0; k < n; k++) {
        uint64_t e = infos + (uint64_t)k * 0x20;
        ocerz_st(e + 0x00, 8, mhs[k]);
        ocerz_st(e + 0x08, 8, iis[k]);
        ocerz_st(e + 0x10, 8, paths[k]);
        ocerz_st(e + 0x18, 8, 0);
    }

    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    uint64_t caller_ret = ocerz_ld(rsp, 8);
    uint64_t ret_rsp = rsp + 8;

    OCERZ_LOG("dyldapi: objc map_images driving %d images (infos=%#llx)\n",
              n, (unsigned long long)infos);

    uint64_t args[3] = { (uint64_t)n, infos, 0 };
    ocerz_vm_call(vm, mapped, args, 3, ret_rsp);

    cpu->rip = caller_ret;
    cpu->gpr[OCERZ_RSP] = ret_rsp;
    cpu->gpr[OCERZ_RAX] = 0;
    return OCERZ_STEP_OK;
}

int ocerz_dyldapi_dispatch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t off = cpu->rip - OCERZ_DYLDAPI_LO;

    switch (off) {
    case 0x210:
        api_return(cpu, 1);
        return OCERZ_STEP_OK;
    case 0x18: {
        uint32_t idx = (uint32_t)cpu->gpr[OCERZ_RSI];
        api_return(cpu, idx == 0 ? ocerz_arena_lo : 0);
        return OCERZ_STEP_OK;
    }
    case 0x1f8: {
        uint64_t size_out = cpu->gpr[OCERZ_RSI];
        if (size_out)
            ocerz_st(size_out, 8, g_cache_size);
        api_return(cpu, g_cache_start);
        return OCERZ_STEP_OK;
    }
    case 0x358:
        return api_objc_register_callbacks(vm, cpu);
    case 0x378:
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    case 0x3a8:
        api_return(cpu, g_headeropt_rw);
        return OCERZ_STEP_OK;
    case 0x3b0:
        api_return(cpu, g_headeropt_ro);
        return OCERZ_STEP_OK;
    default:
        OCERZ_LOG("dyldapi: unimplemented vtable slot +%#llx (this=%#llx a0=%#llx a1=%#llx)\n",
                  (unsigned long long)off, (unsigned long long)cpu->gpr[OCERZ_RDI],
                  (unsigned long long)cpu->gpr[OCERZ_RSI], (unsigned long long)cpu->gpr[OCERZ_RDX]);
        api_return(cpu, 0);
        return OCERZ_STEP_OK;
    }
}
