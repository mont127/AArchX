/*
 * rep movs and rep stos as src/interp_ext.c runs them: in bulk where the range
 * is one piece of host memory, element by element otherwise.  Every element
 * size forward and backward, a movs whose destination starts inside its source
 * (which copies a pattern forward), a stos into a page that faults until a
 * SIGSEGV handler opens it (the instruction restarts and finishes).  rsi, rdi
 * and rcx are printed after each, against Rosetta.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t sum(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

struct regs { uint64_t si, di, cx; };

#define MOVS(insn, d, s, n, back)                                                          \
    ({ struct regs r_; void *d_ = (d); const void *s_ = (s); uint64_t n_ = (n);              \
       if (back) __asm__ volatile("std\n\t" insn "\n\tcld" : "+S"(s_), "+D"(d_), "+c"(n_) : : "memory"); \
       else      __asm__ volatile(insn : "+S"(s_), "+D"(d_), "+c"(n_) : : "memory");          \
       r_.si = (uint64_t)s_; r_.di = (uint64_t)d_; r_.cx = n_; r_; })
#define STOS(insn, d, v, n, back)                                                          \
    ({ struct regs r_ = { 0, 0, 0 }; void *d_ = (d); uint64_t n_ = (n);                      \
       if (back) __asm__ volatile("std\n\t" insn "\n\tcld" : "+D"(d_), "+c"(n_) : "a"((uint64_t)(v)) : "memory"); \
       else      __asm__ volatile(insn : "+D"(d_), "+c"(n_) : "a"((uint64_t)(v)) : "memory"); \
       r_.di = (uint64_t)d_; r_.cx = n_; r_; })

static uint8_t buf[1 << 16], src[1 << 16];

static void show(const char *what, struct regs r, const uint8_t *base, int with_si)
{
    printf("%-22s di=+%llx cx=%llu", what, (unsigned long long)(r.di - (uint64_t)base), (unsigned long long)r.cx);
    if (with_si)
        printf(" si=+%llx", (unsigned long long)(r.si - (uint64_t)src));
    printf(" mem=%016llx\n", (unsigned long long)sum(buf, sizeof buf));
}

static volatile uint8_t *g_fault_page;
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    if ((uint8_t *)si->si_addr >= g_fault_page && (uint8_t *)si->si_addr < g_fault_page + 0x4000)
        mprotect((void *)g_fault_page, 0x4000, PROT_READ | PROT_WRITE);
    else
        _exit(9);
}

int main(void)
{
    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)(i * 7 + 3);

    memset(buf, 0, sizeof buf);
    show("movsb 4000", MOVS("rep movsb", buf + 100, src + 7, 4000, 0), buf, 1);
    show("movsw 3000", MOVS("rep movsw", buf + 9000, src + 2, 3000, 0), buf, 1);
    show("movsd 2000", MOVS("rep movsl", buf + 20001, src + 3, 2000, 0), buf, 1);
    show("movsq 1500", MOVS("rep movsq", buf + 32768, src + 64, 1500, 0), buf, 1);
    show("movsb backward", MOVS("rep movsb", buf + 50000, src + 6000, 700, 1), buf, 1);
    show("movsq backward", MOVS("rep movsq", buf + 60000, src + 8000, 300, 1), buf, 1);
    show("movsb 20", MOVS("rep movsb", buf + 44, src + 1, 20, 0), buf, 1);

    buf[200] = 0xab; buf[201] = 0xcd;
    show("movsb pattern", MOVS("rep movsb", buf + 202, buf + 200, 3000, 0), buf, 0);
    show("movsq pattern", MOVS("rep movsq", buf + 4008, buf + 4000, 400, 0), buf, 0);
    show("movsb overlap down", MOVS("rep movsb", buf + 100, buf + 1100, 5000, 0), buf, 0);

    show("stosb 5000", STOS("rep stosb", buf + 3, 0x5a, 5000, 0), buf, 0);
    show("stosw 4000", STOS("rep stosw", buf + 10001, 0x1234, 4000, 0), buf, 0);
    show("stosd 3000", STOS("rep stosl", buf + 30000, 0xdeadbeef, 3000, 0), buf, 0);
    show("stosq 2000", STOS("rep stosq", buf + 45000, 0x0123456789abcdefull, 2000, 0), buf, 0);
    show("stosd backward", STOS("rep stosl", buf + 60000, 0xcafef00d, 1000, 1), buf, 0);

    /* A stos that runs into a page it may not write until the handler opens it. */
    uint8_t *area = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_fault_page = area + 0x4000;
    mprotect(area + 0x4000, 0x4000, PROT_READ);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    struct regs r = STOS("rep stosb", area + 0x1000, 0x77, 0x6000, 0);
    printf("%-22s di=+%llx cx=%llu mem=%016llx\n", "stosb across fault", (unsigned long long)(r.di - (uint64_t)area),
           (unsigned long long)r.cx, (unsigned long long)sum(area, 0x8000));

    return 0;
}
