/*
 * The guest's own executable path must be ABSOLUTE, as it is on real macOS,
 * where argv[0] is always resolved before the process sees it.  Both
 * _dyld_get_image_name(0) and _NSGetExecutablePath must start with '/' however
 * the program was launched.  Launched as `./prog` they came back relative, and
 * Steam's tier1 built "/../Steam.AppBundle/..." out of one until
 * V_RemoveDotSlashes asserted.
 */
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    const char *name = _dyld_get_image_name(0);
    char buf[4096];
    uint32_t sz = sizeof buf;

    if (_NSGetExecutablePath(buf, &sz) != 0) {
        printf("BAD nsget\n");
        return 1;
    }
    if (!name || name[0] != '/') {
        printf("BAD image_name '%s'\n", name ? name : "(null)");
        return 1;
    }
    if (buf[0] != '/') {
        printf("BAD exec_path '%s'\n", buf);
        return 1;
    }
    printf("OK\n");
    return 0;
}
