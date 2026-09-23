/*
 * The probe tools/dyldslots.sh runs once per libdyld export: it looks the name
 * up with dlsym, prints a marker, calls it with every argument zero and reports
 * that it returned.  The call's first arrival in ocerz's dyld API table, which
 * OCERZ_DYLDAPI_TRACE prints, is the export's slot.
 */
#include <dlfcn.h>
#include <stdio.h>

typedef long (*Fn)(long, long, long, long, long, long);

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *name = argv[1][0] == '_' ? argv[1] + 1 : argv[1];
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        fprintf(stderr, "PROBE nosym %s\n", argv[1]);
        return 0;
    }
    fprintf(stderr, "PROBE call %s\n", argv[1]);
    fflush(stderr);
    long r = ((Fn)p)(0, 0, 0, 0, 0, 0);
    fprintf(stderr, "PROBE returned %s %ld\n", argv[1], r);
    return 0;
}
