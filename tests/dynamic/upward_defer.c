/*
 * An upward dependency is initialized after the whole walk from the program,
 * not right after the library that declares it.  libud_core depends on
 * libud_mid, libud_mid links libud_top upward, and libud_top depends on
 * libud_core, the shape of CoreFoundation, libobjc and Foundation on macOS
 * 26.7.1, where libobjc links libswiftCore upward and libswiftCore links
 * Foundation upward.  Every initializer logs its name.  Under Rosetta the order
 * is "mid core main top": libud_top runs last, after the program's own
 * initializer.  Initializing it right after libud_mid gave "mid top core main",
 * libud_top before its own dependency, which on macOS 26.7.1 let SkyLight
 * allocate through CoreFoundation before CoreFoundation was initialized and
 * crashed sw_vers.  Prints OK under Rosetta and ocerz.
 */
#include <stdio.h>
#include <string.h>

const char *ud_order(void);
void ud_log(const char *name);

__attribute__((constructor)) static void ud_main_init(void)
{
    ud_log("main");
}

int main(void)
{
    const char *got = ud_order();
    if (strcmp(got, "mid core main top") == 0)
        printf("OK\n");
    else
        printf("order: %s\n", got);
    return 0;
}
