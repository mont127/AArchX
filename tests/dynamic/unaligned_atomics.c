/* Misaligned LOCK-prefixed read-modify-writes must be atomic across threads,
 * as they are on x86 (Wine's CRITICAL_SECTION in a packed Valve struct lives
 * at +0x446: a dword lock word that is 2 mod 4).  Four threads hammer
 * counters at misaligned offsets with lock xadd / lock add / lock inc, take a
 * cmpxchg spinlock whose word is misaligned (released with xchg, as Wine's
 * LeaveCriticalSection does), and bump a plain counter under it.  Every total must be exact.  Prints OK. */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define THREADS 4
#define ITERS 200000

static unsigned char area[4096 * 3] __attribute__((aligned(4096)));

/* misaligned sites: dword 2 mod 4, dword crossing a 16-byte granule, qword 4 mod 8,
 * qword crossing a page, and the lock word at the Steam offset */
#define P_D2   (area + 0x102)
#define P_D14  (area + 0x10e)
#define P_Q4   (area + 0x204)
#define P_QPG  (area + 4096 - 3)
#define P_LOCK (area + 0x446)
#define P_PLAIN (area + 0x800)

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        unsigned one = 1;
        __asm__ __volatile__("lock xaddl %0, (%1)" : "+r"(one) : "r"(P_D2) : "memory", "cc");
        __asm__ __volatile__("lock addl $1, (%0)" : : "r"(P_D14) : "memory", "cc");
        __asm__ __volatile__("lock incq (%0)" : : "r"(P_Q4) : "memory", "cc");
        __asm__ __volatile__("lock addq $3, (%0)" : : "r"(P_QPG) : "memory", "cc");
        /* spinlock: lock cmpxchg 0 -> 1 on the misaligned word */
        for (;;) {
            unsigned expect = 0;
            __asm__ __volatile__("lock cmpxchgl %2, (%1)" : "+a"(expect) : "r"(P_LOCK), "r"(1u) : "memory", "cc");
            if (expect == 0) break;
            __asm__ __volatile__("pause");
        }
        uint64_t v; memcpy(&v, P_PLAIN, 8); v++; memcpy(P_PLAIN, &v, 8);
        /* release the way Wine's LeaveCriticalSection does: an interlocked
         * op on the (misaligned) lock word, a full barrier on x86 */
        unsigned zero = 0;
        __asm__ __volatile__("xchgl %0, (%1)" : "+r"(zero) : "r"(P_LOCK) : "memory");
    }
    return NULL;
}

int main(void)
{
    memset(area, 0, sizeof area);
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    unsigned d2, d14; uint64_t q4, qpg, plain;
    memcpy(&d2, P_D2, 4); memcpy(&d14, P_D14, 4); memcpy(&q4, P_Q4, 8); memcpy(&qpg, P_QPG, 8); memcpy(&plain, P_PLAIN, 8);
    const uint64_t want = (uint64_t)THREADS * ITERS;
    int ok = d2 == want && d14 == want && q4 == want && qpg == 3 * want && plain == want;
    /* neighbours untouched */
    for (size_t i = 0; i < sizeof area; i++) {
        if ((i >= 0x102 && i < 0x106) || (i >= 0x10e && i < 0x112) || (i >= 0x204 && i < 0x20c) ||
            (i >= 4093 && i < 4101) || (i >= 0x446 && i < 0x44a) || (i >= 0x800 && i < 0x808)) continue;
        if (area[i]) { ok = 0; printf("stray byte at %zu\n", i); break; }
    }
    if (!ok)
        printf("d2=%u d14=%u q4=%llu qpg=%llu plain=%llu want=%llu\n", d2, d14,
               (unsigned long long)q4, (unsigned long long)qpg, (unsigned long long)plain, (unsigned long long)want);
    printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
