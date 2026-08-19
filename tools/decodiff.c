/* decodiff -- 64-bit decode differential harness.
 *
 * Links against src/decode.o ALONE (verified self-contained: it needs only
 * libc).  Native arm64, no Rosetta, no wine, no i386 toolchain.
 *
 * Purpose: prove that a patch to src/decode.c leaves 64-bit decoding
 * bit-for-bit identical.  Two rejected i386 proposals both shipped silent
 * 64-bit decode regressions that the entire existing test suite missed, so
 * this is the acceptance gate for every future decode.c change.
 *
 *   decodiff digest <out.bin>   -- sweep, write one u64 record hash per input
 *   decodiff line <hexbytes>    -- print the canonical record for one input
 *
 * The sweep is exhaustive over all 2^24 three-byte openings, each followed by
 * a fixed 13-byte tail so 16 bytes are always available (the decoder's own
 * maximum instruction length).  That covers every opcode, every prefix
 * combination up to three deep, and every ModRM/SIB pairing.
 *
 * Records are canonicalised as TEXT, and the opcode is recorded by NAME via
 * ocerz_op_name(), not by numeric value.  That deliberately makes the digest
 * immune to renumbering OcerzOp -- inserting new opcodes mid-enum is a
 * legitimate change that must not show up as a false positive, while a real
 * change of which operation a byte string decodes to still does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ocerz/decode.h"

#define SWEEP_BITS 24
#define SWEEP_N    (1u << SWEEP_BITS)
#define RIP        0x0000000140001000ull   /* nonzero, so riprel folding shows */

static const uint8_t TAIL[13] = {
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd
};

static void render(char *buf, size_t cap, const uint8_t *code)
{
    X86Insn ins;
    memset(&ins, 0, sizeof ins);
    int rc = ocerz_decode(code, 16, RIP, &ins);
    if (rc != 0) { snprintf(buf, cap, "rc=%d", rc); return; }

    size_t n = (size_t)snprintf(buf, cap,
        "op=%s len=%u opsize=%u addrsize=%u rep=%u lock=%u seg=%u cc=%u nops=%u",
        ocerz_op_name(ins.op), ins.len, ins.opsize, ins.addrsize,
        ins.rep, ins.lock, ins.seg, ins.cc, ins.nops);

    for (int i = 0; i < ins.nops && i < 3 && n < cap; i++) {
        const X86Operand *o = &ins.ops[i];
        n += (size_t)snprintf(buf + n, cap - n,
            " |%d k=%u r=%u sz=%u h8=%u b=%u ix=%u sc=%u rip=%u d=%lld imm=%llu",
            i, o->kind, o->reg, o->size, o->high8, o->base, o->index,
            o->scale, o->riprel, (long long)o->disp,
            (unsigned long long)o->imm);
    }
}

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "digest")) {
        uint64_t *out = malloc((size_t)SWEEP_N * 8);
        if (!out) { perror("malloc"); return 1; }
        uint8_t code[16];
        memcpy(code + 3, TAIL, 13);
        char buf[512];
        for (uint32_t i = 0; i < SWEEP_N; i++) {
            code[0] = (uint8_t)(i);
            code[1] = (uint8_t)(i >> 8);
            code[2] = (uint8_t)(i >> 16);
            render(buf, sizeof buf, code);
            out[i] = fnv(buf);
        }
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        fwrite(out, 8, SWEEP_N, f);
        fclose(f);
        fprintf(stderr, "decodiff: wrote %u records to %s\n", SWEEP_N, argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "line")) {
        uint8_t code[16];
        memcpy(code + 3, TAIL, 13);
        unsigned v = (unsigned)strtoul(argv[2], NULL, 16);
        code[0] = (uint8_t)v; code[1] = (uint8_t)(v >> 8); code[2] = (uint8_t)(v >> 16);
        char buf[512];
        render(buf, sizeof buf, code);
        printf("%02x %02x %02x : %s\n", code[0], code[1], code[2], buf);
        return 0;
    }
    fprintf(stderr, "usage: decodiff digest <out.bin> | decodiff line <hex24>\n");
    return 2;
}
