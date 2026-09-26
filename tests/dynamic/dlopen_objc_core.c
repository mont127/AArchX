#include "placeholder_check.h"

int main(void)
{
    void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
    if (!sc) {
        printf("BAD dlopen: %s\n", dlerror());
        return 1;
    }
    if (!placeholders_ok("after SystemConfiguration"))
        return 1;
    printf("OK\n");
    return 0;
}
