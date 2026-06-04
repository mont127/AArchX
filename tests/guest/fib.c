/*
 * tests/guest/fib.c
 *
 * A focused recursion test: naive recursive fib(30). This drives a deep
 * call/ret tree with a large dynamic instruction count (over two million
 * guest calls), which stresses stack push/pop, return-address handling, and,
 * under the JIT tier, repeated block lookups and chaining of the same small
 * blocks. The single printed line is fib(30) = 832040, a fixed deterministic
 * value.
 */
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
