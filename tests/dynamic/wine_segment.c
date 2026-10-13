/*
 * A program with a WINE_RESERVE segment is Wine's loader whatever it is
 * called: CrossOver runs its processes through hard links named after the
 * Windows program (cmd.exe), and those must get Wine's layout.  ocerz marks a
 * Wine process by giving it OCERZ_PRELOAD_OBJC, which this prints.
 */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    puts(getenv("OCERZ_PRELOAD_OBJC") ? "wine" : "plain");
    return 0;
}
