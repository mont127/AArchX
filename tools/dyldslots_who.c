/*
 * Names the function each address given on the command line lies in, with
 * dladdr, for tools/dyldslots.sh.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    for (int k = 1; k < argc; k++) {
        Dl_info info;
        void *p = (void *)(unsigned long)strtoull(argv[k], NULL, 16);
        if (dladdr(p, &info) && info.dli_sname)
            printf("%s %s\n", argv[k], info.dli_sname);
        else
            printf("%s ?\n", argv[k]);
    }
    return 0;
}
