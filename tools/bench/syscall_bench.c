/*
 * Guest-side syscall throughput loop for tools/bench/syscall_bench.sh.
 *
 * argv[1] picks the workload and argv[2] the iteration count: getpid is the
 * cheapest successful BSD call, badwrite a write to fd -1 that takes the error
 * return, gtod the intercepted gettimeofday, machself the task_self Mach trap.
 * Each run does nothing but enter ocerz's syscall boundary, so the difference
 * between a long and a zero-length run is the per-call cost of that path.
 */
#include "../../tests/guest/gsys.h"

static g_u64 parse(const char *s)
{
    g_u64 v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (g_u64)(*s++ - '0');
    return v;
}

int main(int argc, char **argv, char **envp)
{
    if (argc < 3)
        return 2;
    const char *w = argv[1];
    g_u64 n = parse(argv[2]);
    g_u64 tv[2];
    g_i64 acc = 0;
    if (w[0] == 'g' && w[1] == 'e')
        for (g_u64 i = 0; i < n; i++)
            acc += sys_getpid();
    else if (w[0] == 'b')
        for (g_u64 i = 0; i < n; i++)
            acc += sys_write(-1, tv, 1);
    else if (w[0] == 'g' && w[1] == 't')
        for (g_u64 i = 0; i < n; i++)
            acc += sys_gettimeofday(tv, 0);
    else if (w[0] == 'm')
        for (g_u64 i = 0; i < n; i++)
            acc += g_syscall0(0x1000000 | 28);
    else
        return 2;
    return acc == 0x7fffffffffffffffL;
}
