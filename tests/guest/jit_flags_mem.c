/* Memory displacements no access encodes, flags across loads, bt with only
 * CF wanted, logic flags settled inline, and stmxcsr.  Each runs once in the
 * plain memory model and again in a forked child, which is ordered.
 *
 * Displacements beyond a signed 9-bit offset (and positive ones the size does
 * not scale) are folded into the address register, up to 16 MB: [rbp-0x1538]
 * and the like, base-only and indexed, every size.  A compare whose flags a
 * jcc reads past one or two loads has them rebuilt from its operands.  A bt
 * whose flags only a jc reads keeps CF in a record of its own; one whose other
 * flags live on keeps them (ZF and PF from the and, or, xor or test before it,
 * settled without a call when that record is the one pending).  Goldens
 * come from the native binary.
 */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7

static g_u64 g_big[0x80000 / 8];
static volatile g_u64 g_vals[8] = { 0, 1, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
                                    0x1234, 0x80, 0x7f };

static g_i64 guest_fork(g_u64 *is_child)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_fork);
    register g_i64 rdx __asm__("rdx");
    unsigned char err;
    __asm__ volatile("syscall; setc %2" : "+a"(rax), "=d"(rdx), "=qm"(err) : : "rcx", "r11", "memory", "cc");
    if (err)
        return -rax;
    *is_child = (g_u64)rdx;
    return rax;
}

static g_i64 guest_wait4(g_i64 pid, int *status)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_wait4);
    register g_i64 rdi __asm__("rdi") = pid;
    register g_i64 rsi __asm__("rsi") = (g_i64)status;
    register g_i64 rdx __asm__("rdx") = 0;
    register g_i64 r10 __asm__("r10") = 0;
    __asm__ volatile("syscall" : "+a"(rax) : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10) : "rcx", "r11", "memory", "cc");
    return rax;
}

static void put(const char *tag, g_u64 v)
{
    g_puts(tag);
    g_putu64(v);
}

static g_u64 sum_big(void)
{
    g_u64 h = 1469598103934665603ull;
    for (g_u64 i = 0; i < sizeof g_big / 8; i++)
        h = (h ^ g_big[i]) * 1099511628211ull;
    return h;
}

static void far_disp(void)
{
    char *mid = (char *)g_big + 0x40000;
    for (g_u64 i = 0; i < sizeof g_big / 8; i++)
        g_big[i] = 0;
    g_u64 a = 0x0102030405060708ull, r1, r2, r3, r4, r5;
    g_u64 idx = 3;
    __asm__ volatile(
        "movq %[a], -0x1538(%[m])\n\t"
        "movl %k[a], 0x1538(%[m])\n\t"
        "movw %w[a], -0x12345(%[m])\n\t"
        "movb %b[a], 0x1fff9(%[m])\n\t"
        "movq %[a], 0x1001(%[m])\n\t"
        "movq %[a], -0x101(%[m])\n\t"
        "movq %[a], 0x2010(%[m],%[i],8)\n\t"
        "movl %k[a], -0x3ffc(%[m],%[i],4)\n\t"
        "movdqu -0x1538(%[m]), %%xmm1\n\t"
        "movdqu %%xmm1, 0x23450(%[m])\n\t"
        "movq -0x1538(%[m]), %[r1]\n\t"
        "movzwl -0x12345(%[m]), %k[r2]\n\t"
        "movq 0x2010(%[m],%[i],8), %[r3]\n\t"
        "movslq -0x3ffc(%[m],%[i],4), %[r4]\n\t"
        "movzbl 0x1fff9(%[m]), %k[r5]\n\t"
        : [r1] "=&r"(r1), [r2] "=&r"(r2), [r3] "=&r"(r3), [r4] "=&r"(r4), [r5] "=&r"(r5)
        : [a] "r"(a), [m] "r"(mid), [i] "r"(idx)
        : "xmm1", "memory");
    put("far loads ", r1 ^ (r2 << 8) ^ (r3 << 16) ^ (r4 << 24) ^ (r5 << 32));
    put("far sum ", sum_big());
}

/* cmp, sub or add, then one or two loads, then setcc: the flags cross the loads. */
static g_u64 gap_flags(g_u64 x, g_u64 y, volatile g_u64 *p)
{
    g_u64 out = 0, t0, t1;
    unsigned char b, a, l, g, e, s, o;
    __asm__ volatile("cmpq %[y], %[x]\n\tmovq (%[p]), %[t0]\n\tmovq 8(%[p]), %[t1]\n\t"
                     "setb %[b]\n\tseta %[a]\n\tsetl %[l]\n\tsetg %[g]\n\tsete %[e]\n\tsets %[s]\n\tseto %[o]"
                     : [b] "=&q"(b), [a] "=&q"(a), [l] "=&q"(l), [g] "=&q"(g), [e] "=&q"(e), [s] "=&q"(s), [o] "=&q"(o),
                       [t0] "=&r"(t0), [t1] "=&r"(t1)
                     : [x] "r"(x), [y] "r"(y), [p] "r"(p) : "cc", "memory");
    out = b | a << 1 | l << 2 | g << 3 | e << 4 | s << 5 | (g_u64)o << 6;
    g_u64 z = x;
    __asm__ volatile("subq %[y], %[z]\n\tmovq (%[p]), %[t0]\n\tsetb %[b]\n\tsetl %[l]\n\tseto %[o]\n\tsete %[e]"
                     : [z] "+r"(z), [b] "=&q"(b), [l] "=&q"(l), [o] "=&q"(o), [e] "=&q"(e), [t0] "=&r"(t0)
                     : [y] "r"(y), [p] "r"(p) : "cc", "memory");
    out |= (g_u64)(b | l << 1 | o << 2 | e << 3) << 8 | (z & 0xff) << 16;
    z = x;
    __asm__ volatile("addq %[y], %[z]\n\tmovq 8(%[p]), %[t0]\n\tsetc %[b]\n\tseto %[o]\n\tsets %[s]\n\tsete %[e]"
                     : [z] "+r"(z), [b] "=&q"(b), [o] "=&q"(o), [s] "=&q"(s), [e] "=&q"(e), [t0] "=&r"(t0)
                     : [y] "r"(y), [p] "r"(p) : "cc", "memory");
    out |= (g_u64)(b | o << 1 | s << 2 | e << 3) << 24;
    return out + t0 * 0;
}

/* bt, then only CF read (jc / adc); and bts/btr with the same. */
static g_u64 bt_cf(g_u64 v, g_u64 n)
{
    g_u64 r = 0, w = v;
    __asm__ volatile("btq %[n], %[v]\n\tjnc 1f\n\torq $1, %[r]\n1:\n\t"
                     "btsq %[n], %[w]\n\tadcq $0, %[r]\n\t"
                     "btrq $3, %[w]\n\tjc 2f\n\torq $16, %[r]\n2:\n\t"
                     "btl %k[n], %k[v]\n\tsbbq $0, %[r]\n\t"
                     : [r] "+r"(r), [w] "+r"(w) : [v] "r"(v), [n] "r"(n) : "cc");
    return r ^ (w << 8);
}

/* A logic op, then bt with every flag live after it: pushfq reads them all.  SF and OF
 * are undefined after bt, and Rosetta clears SF where ocerz keeps it, so neither is compared. */
#define CF 0x1
#define PF 0x4
#define ZF 0x40
#define LOGIC_BT(name, op)                                                                    \
    static g_u64 name(g_u64 x, g_u64 m, g_u64 bits, g_u64 n)                                       \
    {                                                                                              \
        g_u64 f, t = x;                                                                            \
        __asm__ volatile(op "\n\tbtl %k[n], %k[b]\n\tleaq -128(%%rsp), %%rsp\n\tpushfq\n\tpopq %[f]\n\tleaq 128(%%rsp), %%rsp"                         \
                         : [f] "=r"(f), [t] "+r"(t) : [m] "r"(m), [b] "r"(bits), [n] "r"(n) : "cc"); \
        return (f & (CF | PF | ZF)) ^ (t << 12);                                         \
    }
LOGIC_BT(and8, "andb %b[m], %b[t]")
LOGIC_BT(or16, "orw %w[m], %w[t]")
LOGIC_BT(xor32, "xorl %k[m], %k[t]")
LOGIC_BT(test64, "testq %[m], %[t]")
LOGIC_BT(and64, "andq %[m], %[t]")

static void stmx(void)
{
    static const unsigned vals[4] = { 0x1f80, 0x9fc0, 0x3f80, 0x7f80 };
    unsigned saved, got[4], far;
    char *mid = (char *)g_big + 0x40000;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    for (int k = 0; k < 4; k++) {
        __asm__ volatile("ldmxcsr %1\n\tstmxcsr %0" : "=m"(got[k]) : "m"(vals[k]));
        __asm__ volatile("stmxcsr -0x2468(%1)\n\tmovl -0x2468(%1), %0" : "=r"(far) : "r"(mid) : "memory");
        got[k] ^= far << 16;
    }
    __asm__ volatile("ldmxcsr %0" : : "m"(saved));
    for (int k = 0; k < 4; k++)
        put("stmxcsr ", got[k]);
}

static void run(const char *who)
{
    g_puts(who);
    g_puts("\n");
    far_disp();
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j += 3)
            put("gap ", gap_flags(g_vals[i], g_vals[j], &g_vals[(i + j) & 6]));
    for (g_u64 n = 0; n < 70; n += 7)
        put("bt ", bt_cf(0xa5a5f00f0ff0c33cull, n));
    for (int i = 0; i < 8; i++) {
        g_u64 x = g_vals[i], m = g_vals[(i + 3) & 7];
        put("lbt ", and8(x, m, 0xf0f0, (g_u64)i * 3) ^ or16(x, m, 0x5, (g_u64)i) << 20);
        put("lbt ", xor32(x, m, 0x80000001u, (g_u64)i * 5) ^ test64(x, m, 0x2, 1) << 20);
        put("lbt ", and64(x, m, 0x40, (g_u64)i + 4));
    }
    stmx();
}

int main(int argc, char **argv, char **envp)
{
    run("plain");
    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("fork failed\n");
        return 1;
    }
    if (is_child) {
        run("child");
        sys_exit(0);
    }
    int st = 0;
    guest_wait4(pid, &st);
    put("child status ", (g_u64)st);
    return 0;
}
