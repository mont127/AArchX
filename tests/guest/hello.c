/* Smallest end-to-end guest test: load, run, write, exit. */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    g_puts("hello from x86_64 guest\n");
    return 0;
}
