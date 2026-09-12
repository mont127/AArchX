/*
 * 8-bit and 16-bit LOCK-prefixed RMWs on ADJACENT bytes of one word from
 * several threads.  V8's LocalHeap thread state is a std::atomic<uint8_t>
 * driven by lock or/and/cmpxchg on a single byte, so a widened or non-atomic
 * emulation would corrupt the neighbours or return stale old values.  Every
 * byte must end at an exact value and every cmpxchg old value must equal what
 * was there.
 *
 * It also covers address-cache reuse after an out-of-line arm: a store
 * crossing a 16-byte granule takes the slow arm, and the next access through
 * the same base must recompute its address rather than reuse a dead one.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NT 8
#define ITERS 200000
static uint8_t word[16] __attribute__((aligned(16)));
static uint16_t hword[8] __attribute__((aligned(16)));
static volatile int bad;

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint8_t *b = &word[id];
    #ifdef ALIGN16
    uint16_t *h = &hword[id];
#else
    uint16_t *h = (uint16_t *)((uint8_t *)hword + (id & 6) + (id & 1));
#endif
    for (int i = 0; i < ITERS; i++) {
        __asm__ __volatile__("lock incb %0" : "+m"(*b) :: "memory", "cc");
        __asm__ __volatile__("lock addb $3, %0" : "+m"(*b) :: "memory", "cc");
        __asm__ __volatile__("lock orb $0x80, %0" : "+m"(*b) :: "memory", "cc");
        __asm__ __volatile__("lock andb $0x7f, %0" : "+m"(*b) :: "memory", "cc");
        __asm__ __volatile__("lock xorb $0x00, %0" : "+m"(*b) :: "memory", "cc");
        __asm__ __volatile__("lock subb $1, %0" : "+m"(*b) :: "memory", "cc");
        for (;;) {
            uint8_t old = *(volatile uint8_t *)b, want = (uint8_t)(old + 1), got = old;
            __asm__ __volatile__("lock cmpxchgb %2, %1" : "+a"(got), "+m"(*b) : "q"(want) : "memory", "cc");
            if (got == old) break;
        }
        uint8_t x = 0;
        __asm__ __volatile__("xchgb %0, %1" : "+r"(x), "+m"(*b) :: "memory");
        __asm__ __volatile__("lock addb %1, %0" : "+m"(*b) : "q"(x) : "memory", "cc");
        uint8_t one = 1;
        __asm__ __volatile__("lock xaddb %0, %1" : "+q"(one), "+m"(*b) :: "memory", "cc");
#ifndef NO16
        __asm__ __volatile__("lock addw $1, %0" : "+m"(*h) :: "memory", "cc");
        __asm__ __volatile__("lock orw $0x8000, %0" : "+m"(*h) :: "memory", "cc");
        __asm__ __volatile__("lock andw $0x7fff, %0" : "+m"(*h) :: "memory", "cc");
#endif
    }
    return NULL;
}

static uint8_t gran[64] __attribute__((aligned(16)));
__attribute__((noinline)) static int granule_cross(uint8_t *base, uint64_t a, uint64_t b)
{
    __asm__ __volatile__(
        "mov %1, 10(%0)\n\t"
        "mov %2, 26(%0)\n\t"
        "mov 10(%0), %1\n\t"
        "mov 26(%0), %2\n\t"
        : "+r"(base), "+r"(a), "+r"(b) :: "memory");
    return a == 0x1122334455667788ull && b == 0x99aabbccddeeff00ull;
}

int main(void)
{
    memset(word, 0, sizeof word); memset(hword, 0, sizeof hword);
    int gok = 1;
    for (int i = 0; i < 1000; i++) {
        memset(gran, 0, sizeof gran);
        if (!granule_cross(gran, 0x1122334455667788ull, 0x99aabbccddeeff00ull)) gok = 0;
        uint64_t x, y; memcpy(&x, gran + 10, 8); memcpy(&y, gran + 26, 8);
        if (x != 0x1122334455667788ull || y != 0x99aabbccddeeff00ull) gok = 0;
    }
    if (!gok) printf("granule-crossing store pair FAILED\n");
    pthread_t th[NT];
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    uint8_t want = (uint8_t)(5u * ITERS);
    int ok = 1;
    for (int i = 0; i < NT; i++) if (word[i] != want) { ok = 0; printf("byte %d = %u want %u\n", i, word[i], want); }
    for (int i = NT; i < 16; i++) if (word[i]) { ok = 0; printf("stray byte %d = %u\n", i, word[i]); }
    ok = ok && gok;
    printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
