/*
 * ocerz given an application bundle runs the executable the bundle names.
 *
 * Steam substitutes a game's .app bundle for %command% in its launch options,
 * so "ocerz %command%" handed ocerz a directory and it stopped with "cannot
 * read".  The harness builds this program into Probe.app as
 * Contents/MacOS/probe_exe, a name unlike the bundle's so only Info.plist can
 * supply it, and runs ocerz on the bundle.  The program prints the last two
 * components of its argv[0], which is the executable's path for a bundle the
 * system starts.
 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *p = argv[0];
    const char *last = strrchr(p, '/');
    if (!last) {
        printf("app_bundle bad: argv[0] is %s\n", p);
        return 0;
    }
    const char *dir = last;
    while (dir > p && dir[-1] != '/')
        dir--;
    printf("%.*s %s\n", (int)(last - dir), dir, last + 1);
    return 0;
}
