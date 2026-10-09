#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ocerz/a64emit.h"

#define EQUIV_ROUNDS 20000u
#define PERF_CALLS 10000000u
#define PATCH_WORDS ((1u << 26) + 1u)
#define PATCH_ORIGIN (1u << 25)

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t hash_state = UINT64_C(14695981039346656037);
static volatile uint64_t perf_sink;

static uint64_t next_u64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static uint32_t next_u32(void)
{
    return (uint32_t)next_u64();
}

static int random_bool(void)
{
    return (int)(next_u32() & 1u);
}

static int random_reg(void)
{
    uint32_t r = next_u32() & 31u;
    return (int)(r + ((next_u32() & 7u) == 0 ? 32u : 0u));
}

static int random_size(void)
{
    static const int sizes[] = { 1, 2, 4, 8, 16, 3, 5, 32 };
    return sizes[next_u32() & 7u];
}

static int random_mode(void)
{
    return (int)(next_u32() % 5u);
}

static int random_small(void)
{
    static const int values[] = { 0, 1, 2, 3, 4, 7, 15, 31, 32, 63, 64, 127, 255 };
    return values[next_u32() % (sizeof(values) / sizeof(values[0]))];
}

static int32_t random_branch_offset(void)
{
    return (int32_t)(next_u32() & 0x0fffffffu) - (int32_t)0x08000000u;
}

static void hash_u32(uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        hash_state ^= (uint8_t)(value >> (i * 8));
        hash_state *= UINT64_C(1099511628211);
    }
}

static void hash_u64(uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        hash_state ^= (uint8_t)(value >> (i * 8));
        hash_state *= UINT64_C(1099511628211);
    }
}

static void hash_emitted(A64Buf *b, uint32_t *words)
{
    for (uint32_t *p = words; p < b->p; p++)
        hash_u32(*p);
    hash_u32((uint32_t)b->overflow);
}

#include "a64emit_calls.h"

static void run_equivalence(A64Buf *b, uint32_t *words, uint32_t *patch_words)
{
    for (uint32_t round = 0; round < EQUIV_ROUNDS; round++) {
        cover_all_a64emit_functions(b, words, patch_words, round);
    }
}

static double elapsed_ns(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) * 1e9
        + (double)(end.tv_nsec - start.tv_nsec);
}

typedef void (*bench_fn)(void);

static double best_of_five(bench_fn fn)
{
    double best = 1e300;
    for (int i = 0; i < 5; i++) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        fn();
        clock_gettime(CLOCK_MONOTONIC, &end);
        double ns_per_call = elapsed_ns(start, end) / PERF_CALLS;
        if (ns_per_call < best)
            best = ns_per_call;
    }
    return best;
}

static uint64_t perf_values[4096];
static uint32_t perf_words[16];
static A64Buf perf_buffer;

static const uint64_t logical_values[] = {
    UINT64_C(0x5555555555555555), UINT64_C(0xaaaaaaaaaaaaaaaa),
    UINT64_C(0x00000000ffffffff), UINT64_C(0x00ff00ff00ff00ff),
    UINT64_C(0xf0f0f0f0f0f0f0f0), UINT64_C(0x0000000000000001),
    UINT64_C(0x8000000000000000), UINT64_C(0x00000000ffff0000),
};

static void prepare_perf_values(void)
{
    rng_state = UINT64_C(0x3141592653589793);
    for (size_t i = 0; i < sizeof(perf_values) / sizeof(perf_values[0]); i++) {
        perf_values[i] = (i & 3u) == 0
            ? logical_values[(i >> 2) % (sizeof(logical_values) / sizeof(logical_values[0]))]
            : next_u64();
    }
    perf_buffer = (A64Buf) {
        .start = perf_words,
        .p = perf_words,
        .end = perf_words + 16,
        .overflow = 0,
        .sink = 0,
    };
}

static void bench_mov_imm64(void)
{
    for (uint32_t i = 0; i < PERF_CALLS; i++) {
        perf_buffer.p = perf_words;
        a64_mov_imm64(&perf_buffer, (int)(i & 31u), perf_values[i & 4095u]);
    }
    perf_sink ^= perf_words[0] ^ (uintptr_t)perf_buffer.p;
}

static void bench_try_immediates(void)
{
    for (uint32_t i = 0; i < PERF_CALLS; i++) {
        uint64_t imm = perf_values[i & 4095u];
        perf_buffer.p = perf_words;
        (void)a64_try_and_imm(&perf_buffer, 1, 1, 2, imm);
        perf_buffer.p = perf_words;
        (void)a64_try_ands_imm(&perf_buffer, 1, 3, 4, imm);
        perf_buffer.p = perf_words;
        (void)a64_try_orr_imm(&perf_buffer, 1, 5, 6, imm);
        perf_buffer.p = perf_words;
        (void)a64_try_eor_imm(&perf_buffer, 1, 7, 8, imm);
    }
    perf_sink ^= perf_words[0] ^ (uintptr_t)perf_buffer.p;
}

static void bench_random_mix(void)
{
    rng_state = UINT64_C(0x2718281828459045);
    for (uint32_t i = 0; i < PERF_CALLS; i++) {
        uint32_t r = next_u32();
        perf_buffer.p = perf_words;
        switch (r & 15u) {
        case 0: a64_mov_imm64(&perf_buffer, (int)((r >> 5) & 31u), next_u64()); break;
        case 1: a64_add_imm(&perf_buffer, (int)(r & 1u), 1, 2, r >> 8); break;
        case 2: a64_sub_reg(&perf_buffer, (int)(r & 1u), 1, 2, 3, (int)(r >> 16)); break;
        case 3: a64_b(&perf_buffer, random_branch_offset()); break;
        case 4: a64_tbz(&perf_buffer, (int)(r & 31u), (int)((r >> 8) & 63u), (int32_t)(r >> 12)); break;
        case 5: a64_fadd_s(&perf_buffer, (int)(r & 1u), 1, 2, 3); break;
        case 6: a64_v_add(&perf_buffer, (int)((r >> 4) & 3u), 1, 2, 3); break;
        case 7: a64_ldr(&perf_buffer, (int[]){1, 2, 4, 8}[r & 3u], 1, 2, r >> 8); break;
        case 8: a64_str(&perf_buffer, (int[]){1, 2, 4, 8}[r & 3u], 3, 4, r >> 8); break;
        case 9: a64_movz(&perf_buffer, 5, (uint16_t)r, (int)((r >> 16) & 3u)); break;
        case 10: a64_csel(&perf_buffer, (int)(r & 1u), 1, 2, 3, (int)((r >> 8) & 15u)); break;
        case 11: a64_lslv(&perf_buffer, (int)(r & 1u), 1, 2, 3); break;
        case 12: a64_ands_reg(&perf_buffer, (int)(r & 1u), 1, 2, 3, (int)(r >> 16)); break;
        case 13: a64_try_orr_imm(&perf_buffer, 1, 1, 2, next_u64()); break;
        case 14: a64_stp_off(&perf_buffer, 1, 2, 3, (int16_t)r); break;
        default: a64_ret(&perf_buffer); break;
        }
    }
    perf_sink ^= perf_words[0] ^ (uintptr_t)perf_buffer.p;
}

static void run_overflow_and_sink(void)
{
    uint32_t word = UINT32_C(0x12345678);
    A64Buf tiny = {
        .start = &word,
        .p = &word,
        .end = &word,
        .overflow = 0,
        .sink = UINT32_C(0xa5a5a5a5),
    };
    uint32_t *label = a64_label(&tiny);
    hash_u32(label == &tiny.sink ? 1u : 0u);
    a64_emit32(&tiny, UINT32_C(0xdeadbeef));
    hash_u32(word);
    hash_u32(tiny.overflow);
    hash_u32(tiny.sink);
    hash_u32(a64_label(&tiny) == &tiny.sink ? 1u : 0u);

    tiny.end = &word + 1;
    tiny.p = &word;
    tiny.overflow = 0;
    a64_emit32(&tiny, UINT32_C(0x0badc0de));
    a64_emit32(&tiny, UINT32_C(0xcafef00d));
    hash_u32(word);
    hash_u32(tiny.overflow);
    hash_u32(a64_label(&tiny) == &tiny.sink ? 1u : 0u);
}

int main(void)
{
    if (A64EMIT_COVERED_FUNCTIONS != A64EMIT_HEADER_PROTOTYPES) {
        fprintf(stderr, "a64emit coverage mismatch: %d != %d\n",
                A64EMIT_COVERED_FUNCTIONS, A64EMIT_HEADER_PROTOTYPES);
        return 1;
    }

    uint32_t words[16] = {0};
    uint32_t *patch_words = calloc(PATCH_WORDS, sizeof(*patch_words));
    if (!patch_words) {
        perror("calloc patch scratch");
        return 1;
    }
    A64Buf b = {
        .start = words,
        .p = words,
        .end = words + 16,
        .overflow = 0,
        .sink = 0,
    };
    run_equivalence(&b, words, patch_words);
    run_overflow_and_sink();
    printf("covered=%d hash=%016" PRIx64 "\n",
           A64EMIT_COVERED_FUNCTIONS, hash_state);
    free(patch_words);

    prepare_perf_values();
    double mov_ns = best_of_five(bench_mov_imm64);
    double try_ns = best_of_five(bench_try_immediates);
    double mix_ns = best_of_five(bench_random_mix);
    printf("mov_imm64_ns=%.3f try_imm_ns=%.3f random_mix_ns=%.3f sink=%016" PRIx64 "\n",
           mov_ns, try_ns / 4.0, mix_ns, perf_sink);
    return 0;
}
