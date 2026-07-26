/* Recursion test: naive recursive fib(30). */
#include "gsys.h"

static g_u64 fib(unsigned n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv, char **envp)
{
    g_putu64(fib(30));
    return 0;
}
