/* dyld queries that libSystem, libxpc, libsystem_trace and OpenGL make at
 * startup, checked against the executable's own load commands.  All of them
 * fell through to "unimplemented vtable slot" and answered 0 -- the program
 * SDK version above all: OpenGL read 0 as a pre-10.5 SDK and dropped its
 * software renderer, which is why Photos could not get a pixel format. */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t dyld_get_program_sdk_version(void);
extern uint32_t dyld_get_base_platform(uint32_t platform);
extern int _dyld_get_image_uuid(const struct mach_header *mh, unsigned char uuid[16]);
extern int _dyld_get_shared_cache_uuid(unsigned char uuid[16]);
extern const char *_dyld_shared_cache_real_path(const char *path);

static const struct load_command *find_lc(const struct mach_header_64 *mh, uint32_t cmd)
{
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == cmd)
            return lc;
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
    return NULL;
}

int main(void)
{
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct build_version_command *bv =
        (const struct build_version_command *)find_lc(mh, LC_BUILD_VERSION);
    uint32_t sdk = dyld_get_program_sdk_version();
    if (!bv || sdk == 0 || sdk != bv->sdk) {
        printf("BAD program sdk %#x, LC_BUILD_VERSION says %#x\n", sdk, bv ? bv->sdk : 0);
        return 1;
    }
    if (dyld_get_base_platform(PLATFORM_MACOS) != PLATFORM_MACOS ||
        dyld_get_base_platform(PLATFORM_IOSSIMULATOR) != PLATFORM_IOS) {
        printf("BAD base platform\n");
        return 2;
    }
    const struct uuid_command *uc = (const struct uuid_command *)find_lc(mh, LC_UUID);
    unsigned char u[16] = { 0 };
    if (!uc || !_dyld_get_image_uuid((const struct mach_header *)mh, u) || memcmp(u, uc->uuid, 16) != 0) {
        printf("BAD image uuid\n");
        return 3;
    }
    unsigned char cu[16] = { 0 }, zero[16] = { 0 };
    if (!_dyld_get_shared_cache_uuid(cu) || memcmp(cu, zero, 16) == 0) {
        printf("BAD shared cache uuid\n");
        return 4;
    }
    const char *rp = _dyld_shared_cache_real_path("/usr/lib/libSystem.B.dylib");
    if (!rp || !strstr(rp, "libSystem")) {
        printf("BAD shared cache real path %s\n", rp ? rp : "(null)");
        return 5;
    }
    printf("OK\n");
    return 0;
}
