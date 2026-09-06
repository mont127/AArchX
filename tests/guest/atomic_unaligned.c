/* LOCK-prefixed read-modify-writes on misaligned addresses.  x86 makes them
 * atomic at any alignment; ARM64's LSE atomics fault on a misaligned
 * address, and so did the interpreter the JIT diverted them to.  Wine's
 * CRITICAL_SECTION inside a packed Valve object sits at +0x446, so every
 * EnterCriticalSection in Steam's CEF host was a `lock cmpxchg` on a dword
 * that is 2 mod 4 -> access violation.  Each shape below runs at a naturally
 * misaligned offset, one that crosses a 16-byte granule and one that crosses
 * a 4 KB page.  Results and flags are printed; golden from the native run. */
#include "gsys.h"

static unsigned char buf[3 * 4096] __attribute__((aligned(4096)));

#define FLAGS(z, c, s, o) \
    "setz %b[z]\n\tsetc %b[c]\n\tsets %b[s]\n\tseto %b[o]\n\t"
#define FLAGOUT(z, c, s, o) [z] "=&r"(z), [c] "=&r"(c), [s] "=&r"(s), [o] "=&r"(o)

static void show(const char *what, g_u64 v)
{
    g_puts(what); g_puts(" "); g_putu64(v);
}

static void flags_line(const char *what, unsigned char z, unsigned char c, unsigned char s, unsigned char o)
{
    g_puts(what); g_puts(" flags z c s o = ");
    g_putu64_nonl(z); g_puts(" "); g_putu64_nonl(c); g_puts(" "); g_putu64_nonl(s); g_puts(" "); g_putu64(o);
}

/* ---- 32-bit ---- */
static void test32(const char *tag, unsigned char *p)
{
    unsigned char z, c, s, o;
    unsigned old, r;
    g_puts("== dword "); g_puts(tag); g_puts("\n");

    __builtin_memcpy(p, "\x78\x56\x34\x12", 4);              /* 0x12345678 */
    old = 0x12345678u; r = 0;
    __asm__ __volatile__("lock cmpxchgl %[n], (%[p])\n\t" FLAGS(z, c, s, o)
                         : "+a"(old), FLAGOUT(z, c, s, o) : [n] "r"(0xdeadbeefu), [p] "r"(p) : "memory", "cc");
    show("cmpxchg-hit old", old); __builtin_memcpy(&r, p, 4); show("cmpxchg-hit mem", r); flags_line("cmpxchg-hit", z, c, s, o);

    old = 0x11111111u;
    __asm__ __volatile__("lock cmpxchgl %[n], (%[p])\n\t" FLAGS(z, c, s, o)
                         : "+a"(old), FLAGOUT(z, c, s, o) : [n] "r"(0x55555555u), [p] "r"(p) : "memory", "cc");
    show("cmpxchg-miss old", old); __builtin_memcpy(&r, p, 4); show("cmpxchg-miss mem", r); flags_line("cmpxchg-miss", z, c, s, o);

    unsigned v = 0x7fffffffu;
    __asm__ __volatile__("lock xaddl %[v], (%[p])\n\t" FLAGS(z, c, s, o)
                         : [v] "+r"(v), FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    show("xadd ret", v); __builtin_memcpy(&r, p, 4); show("xadd mem", r); flags_line("xadd", z, c, s, o);

    v = 0xa5a5a5a5u;
    __asm__ __volatile__("xchgl %[v], (%[p])" : [v] "+r"(v) : [p] "r"(p) : "memory");
    show("xchg ret", v); __builtin_memcpy(&r, p, 4); show("xchg mem", r);

    __asm__ __volatile__("lock addl $0x5a5a5a5b, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("add mem", r); flags_line("add", z, c, s, o);
    __asm__ __volatile__("lock subl $1, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("sub mem", r); flags_line("sub", z, c, s, o);
    __asm__ __volatile__("lock orl $0x80000000, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("or mem", r); flags_line("or", z, c, s, o);
    __asm__ __volatile__("lock xorl $0xffffffff, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("xor mem", r); flags_line("xor", z, c, s, o);
    __asm__ __volatile__("lock andl $0x0f0f0f0f, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("and mem", r); flags_line("and", z, c, s, o);
    __asm__ __volatile__("lock incl (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("inc mem", r); flags_line("inc", z, c, s, o);
    __asm__ __volatile__("lock decl (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("dec mem", r); flags_line("dec", z, c, s, o);
    __asm__ __volatile__("lock negl (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("neg mem", r); flags_line("neg", z, c, s, o);
    __asm__ __volatile__("lock notl (%[p])" : : [p] "r"(p) : "memory");
    __builtin_memcpy(&r, p, 4); show("not mem", r);
    __asm__ __volatile__("lock btsl $3, (%[p])\n\tsetc %b[c]" : [c] "=&r"(c) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 4); show("bts mem", r); show("bts carry", c);
}

/* ---- 64-bit ---- */
static void test64(const char *tag, unsigned char *p)
{
    unsigned char z, c, s, o;
    g_u64 old, r, v;
    g_puts("== qword "); g_puts(tag); g_puts("\n");

    v = 0x0123456789abcdefull; __builtin_memcpy(p, &v, 8);
    old = v;
    __asm__ __volatile__("lock cmpxchgq %[n], (%[p])\n\t" FLAGS(z, c, s, o)
                         : "+a"(old), FLAGOUT(z, c, s, o) : [n] "r"(0xfedcba9876543210ull), [p] "r"(p) : "memory", "cc");
    show("cmpxchg-hit old", old); __builtin_memcpy(&r, p, 8); show("cmpxchg-hit mem", r); flags_line("cmpxchg-hit", z, c, s, o);
    old = 1;
    __asm__ __volatile__("lock cmpxchgq %[n], (%[p])\n\t" FLAGS(z, c, s, o)
                         : "+a"(old), FLAGOUT(z, c, s, o) : [n] "r"(2ull), [p] "r"(p) : "memory", "cc");
    show("cmpxchg-miss old", old); __builtin_memcpy(&r, p, 8); show("cmpxchg-miss mem", r); flags_line("cmpxchg-miss", z, c, s, o);

    v = 0x7fffffffffffffffull;
    __asm__ __volatile__("lock xaddq %[v], (%[p])\n\t" FLAGS(z, c, s, o)
                         : [v] "+r"(v), FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    show("xadd ret", v); __builtin_memcpy(&r, p, 8); show("xadd mem", r); flags_line("xadd", z, c, s, o);
    v = 0x5a5a5a5a5a5a5a5aull;
    __asm__ __volatile__("xchgq %[v], (%[p])" : [v] "+r"(v) : [p] "r"(p) : "memory");
    show("xchg ret", v); __builtin_memcpy(&r, p, 8); show("xchg mem", r);
    __asm__ __volatile__("lock addq $-1, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("add mem", r); flags_line("add", z, c, s, o);
    __asm__ __volatile__("lock orq $0x7f, (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("or mem", r); flags_line("or", z, c, s, o);
    __asm__ __volatile__("lock incq (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("inc mem", r); flags_line("inc", z, c, s, o);
    __asm__ __volatile__("lock negq (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("neg mem", r); flags_line("neg", z, c, s, o);

    /* cmpxchg8b: hit then miss */
    v = 0x1122334455667788ull; __builtin_memcpy(p, &v, 8);
    unsigned lo = 0x55667788u, hi = 0x11223344u;
    __asm__ __volatile__("lock cmpxchg8b (%[p])\n\tsetz %b[z]"
                         : "+a"(lo), "+d"(hi), [z] "=&r"(z) : "b"(0xaaaaaaaau), "c"(0xbbbbbbbbu), [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("cmpxchg8b-hit mem", r); show("cmpxchg8b-hit zf", z); show("cmpxchg8b-hit eax", lo); show("cmpxchg8b-hit edx", hi);
    lo = 1; hi = 2;
    __asm__ __volatile__("lock cmpxchg8b (%[p])\n\tsetz %b[z]"
                         : "+a"(lo), "+d"(hi), [z] "=&r"(z) : "b"(3u), "c"(4u), [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 8); show("cmpxchg8b-miss mem", r); show("cmpxchg8b-miss zf", z); show("cmpxchg8b-miss eax", lo); show("cmpxchg8b-miss edx", hi);
}

/* ---- 16-bit ---- */
static void test16(const char *tag, unsigned char *p)
{
    unsigned char z, c, s, o;
    unsigned short old, r, v;
    g_puts("== word "); g_puts(tag); g_puts("\n");
    __builtin_memcpy(p, "\x34\x12", 2);
    old = 0x1234;
    __asm__ __volatile__("lock cmpxchgw %[n], (%[p])\n\t" FLAGS(z, c, s, o)
                         : "+a"(old), FLAGOUT(z, c, s, o) : [n] "r"((unsigned short)0xbeef), [p] "r"(p) : "memory", "cc");
    show("cmpxchg-hit old", old); __builtin_memcpy(&r, p, 2); show("cmpxchg-hit mem", r); flags_line("cmpxchg-hit", z, c, s, o);
    v = 0x8000;
    __asm__ __volatile__("lock xaddw %[v], (%[p])\n\t" FLAGS(z, c, s, o)
                         : [v] "+r"(v), FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    show("xadd ret", v); __builtin_memcpy(&r, p, 2); show("xadd mem", r); flags_line("xadd", z, c, s, o);
    __asm__ __volatile__("lock decw (%[p])\n\t" FLAGS(z, c, s, o) : FLAGOUT(z, c, s, o) : [p] "r"(p) : "memory", "cc");
    __builtin_memcpy(&r, p, 2); show("dec mem", r); flags_line("dec", z, c, s, o);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (unsigned char)i;
    test32("2 mod 4", buf + 0x446);                 /* the Steam shape */
    test32("crosses 16-byte granule", buf + 0x10e);
    test32("crosses page", buf + 4096 - 2);
    test64("4 mod 8", buf + 0x104);
    test64("crosses 16-byte granule", buf + 0x10c);
    test64("crosses page", buf + 8192 - 3);
    test16("1 mod 2", buf + 0x201);
    test16("crosses page", buf + 4095);              /* last byte of page 0 + first of page 1 */
    /* the bytes around every site are untouched */
    g_u64 sum = 0;
    for (int i = 0; i < (int)sizeof buf; i++) sum = sum * 31 + buf[i];
    show("checksum", sum);
    return 0;
}
