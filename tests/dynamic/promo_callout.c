/* A spliced callee's push/pop pairs are renamed into scratch registers, and
 * in the Wine mode's low shadow window the nested calls inside the splice go
 * through a C call-out that clobbers those registers.  Built as a main at
 * 0x200000000 this printed a different wrong sum on every run; the right one
 * is what Rosetta prints. */
#include <stdio.h>

typedef unsigned long long u64;

__attribute__((noinline)) static u64 leaf1(u64 x) { return (x * 2654435761ull) ^ (x >> 7); }
__attribute__((noinline)) static u64 leaf2(u64 x) { return leaf1(x) + leaf1(x ^ 0x55); }
__attribute__((noinline)) static u64 run(u64 n)
{
    u64 acc = 0;
    for (u64 i = 0; i < n; i++)
        acc = leaf2(acc + i);
    return acc;
}

int main(int argc, char **argv)
{
    (void)argv;
    printf("%llu\n", run(100000 * (u64)argc));
    return 0;
}
