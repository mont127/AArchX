/* i386diff -- 32-bit (i386) decode conformance harness.
 *
 * Sibling of tools/decodiff.c, and deliberately the opposite kind of gate:
 *
 *   decodiff  proves a decode.c patch changed NOTHING in 64-bit mode
 *             (self-comparison of two trees, no external truth needed).
 *   i386diff  measures how much of 32-bit mode ocerz decodes CORRECTLY,
 *             against an external oracle (capstone CS_MODE_32).
 *
 * Like decodiff this links against src/decode.o ALONE (that object needs only
 * libc), so it is native arm64 -- no Rosetta, no wine, no i386 toolchain.
 *
 * This C half only sweeps and records what ocerz's decoder says.  The oracle,
 * the comparison and the classified report live in tools/i386diff.py; the two
 * halves are wired together by tools/i386diff.sh.  The sweep parameters
 * (opening widths, tails, EIP) are printed by `i386diff params` so that python
 * reconstructs byte-for-byte the same probes from a single source of truth.
 *
 *   i386diff params              -- sweep parameters, key=value, for python
 *   i386diff names               -- "<id> <name>" for every OcerzOp
 *   i386diff sweep 32 <out.bin>  -- sweep in 32-bit mode, write records
 *                                  (trailing "quick" = 2-byte openings only)
 *   i386diff sweep 64 <out.bin>  -- ditto in 64-bit mode (oracle calibration)
 *   i386diff probe <index>       -- print the 16 probe bytes for one index
 *   i386diff line 32 <index>     -- decode one probe, human readable
 *   i386diff bytes 32 <hex...>   -- decode an ad-hoc byte string
 *   i386diff selftest            -- the documented 32-bit oracle answers
 *
 * ---------------------------------------------------------------------------
 * ENTRY POINT
 * ---------------------------------------------------------------------------
 * ocerz has no 32-bit decode entry point yet; stage 3 adds one.  Rather than
 * guess silently, this file compiles against whichever of a small set of
 * candidate symbols actually exists in the decode.o under test.  The driver
 * script picks with nm(1) and passes -DI386DIFF_ENTRY=N:
 *
 *   1  int ocerz_decode_mode(const uint8_t *code, size_t avail, uint64_t rip,
 *                            X86Insn *out, int mode32);      <- ASSUMED API
 *   2  int ocerz_decode32(const uint8_t *code, size_t avail, uint64_t eip,
 *                         X86Insn *out);
 *   0  no 32-bit entry point found: fall back to the 64-bit ocerz_decode() and
 *      feed it 32-bit input.  That is WRONG BY CONSTRUCTION and is exactly the
 *      stage-3 baseline this harness exists to measure.
 *
 * If stage 3 lands a third spelling, add a branch here and a case to the nm
 * probe in i386diff.sh -- nothing else in the harness cares.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ocerz/decode.h"
#include "ocerz/cpu.h"   /* OCERZ_REG_NONE only */

/* ---- sweep shape ------------------------------------------------------- */

#define SWEEP3_BITS 24
#define SWEEP3_N    (1u << SWEEP3_BITS)          /* every 3-byte opening      */
#define SWEEP2_BITS 16
#define SWEEP2_N    (1u << SWEEP2_BITS)          /* every 2-byte opening      */
#define PROBE_N     (SWEEP3_N + SWEEP2_N)
#define PROBE_LEN   16                           /* decoder's own max length  */

/* Two different tails.  A single tail can accidentally mask a length bug
 * (e.g. an immediate read at the wrong width that happens to land on equal
 * bytes), so phase B repeats the 2-byte openings against unrelated filler. */
static const uint8_t TAIL_A[13] = {
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd
};
static const uint8_t TAIL_B[14] = {
    0x37,0xf1,0x2a,0x5c,0xd3,0x08,0xe6,0x9b,0x4f,0xa7,0x60,0x13,0xbe,0xc5
};

/* Nonzero and plausible: a classic i386 PE image base + .text offset, and the
 * 64-bit RIP decodiff uses.  Only affects riprel folding / branch targets. */
#define EIP32 0x00401000ull
#define RIP64 0x0000000140001000ull

/* ---- entry point shim -------------------------------------------------- */

#ifndef I386DIFF_ENTRY
#define I386DIFF_ENTRY 0
#endif

#if I386DIFF_ENTRY == 1
extern int ocerz_decode_mode(const uint8_t *code, size_t avail, uint64_t rip,
                             X86Insn *out, int mode32);
#define ENTRY_DESC "ocerz_decode_mode(code, avail, rip, out, mode32)"
#define ENTRY_IS_REAL 1
static int decode_as(const uint8_t *c, size_t n, uint64_t ip, X86Insn *o, int m32)
{
    return ocerz_decode_mode(c, n, ip, o, m32);
}
#elif I386DIFF_ENTRY == 2
extern int ocerz_decode32(const uint8_t *code, size_t avail, uint64_t eip,
                          X86Insn *out);
#define ENTRY_DESC "ocerz_decode32(code, avail, eip, out)"
#define ENTRY_IS_REAL 1
static int decode_as(const uint8_t *c, size_t n, uint64_t ip, X86Insn *o, int m32)
{
    return m32 ? ocerz_decode32(c, n, ip, o) : ocerz_decode(c, n, ip, o);
}
#else
#define ENTRY_DESC "ocerz_decode(code, avail, rip, out) -- 64-BIT FALLBACK"
#define ENTRY_IS_REAL 0
static int decode_as(const uint8_t *c, size_t n, uint64_t ip, X86Insn *o, int m32)
{
    (void)m32;                    /* no 32-bit entry point: baseline mode */
    return ocerz_decode(c, n, ip, o);
}
#endif

/* ---- probes ------------------------------------------------------------ */

/* Probe i < 2^24: opening = the three bytes of i (little endian) + TAIL_A.
 * Probe i >= 2^24: opening = the two bytes of (i - 2^24)         + TAIL_B.
 * Always exactly PROBE_LEN bytes, so "instruction too long" is the only
 * truncation either side can report and both report it on the same inputs. */
static void build_probe(uint32_t i, uint8_t code[PROBE_LEN])
{
    if (i < SWEEP3_N) {
        code[0] = (uint8_t)i;
        code[1] = (uint8_t)(i >> 8);
        code[2] = (uint8_t)(i >> 16);
        memcpy(code + 3, TAIL_A, sizeof TAIL_A);
    } else {
        uint32_t j = i - SWEEP3_N;
        code[0] = (uint8_t)j;
        code[1] = (uint8_t)(j >> 8);
        memcpy(code + 2, TAIL_B, sizeof TAIL_B);
    }
}

/* ---- records ----------------------------------------------------------- */

/* 8 bytes per probe.  status 1 = decoded, 0 = refused; on refusal `op` carries
 * the negated ocerz error code so python can separate "unknown opcode" from
 * "ran off the end", which are very different failures. */
typedef struct Rec {
    uint8_t  status;
    uint8_t  len;
    uint8_t  flags;
    uint8_t  pad;
    uint16_t op;
    uint16_t pad2;
} Rec;

/* Operand SHAPE.  Length and mnemonic agreement is not enough: the four
 * mode-dependent operand rules below all produce instructions of the SAME
 * length with the SAME mnemonic, so a stage-4 patch could decode
 * "mov eax,[0x18]" as RIP-relative and sail through a length-only gate.  These
 * four bits are recoverable from both X86Insn and capstone's operand text
 * without reimplementing either side's formatting.
 *
 *   1  RIP-relative memory operand     (32-bit: mod=00 rm=101 is ABSOLUTE)
 *   2  16-bit address registers        (32-bit + 0x67; a different ModRM table)
 *   4  64-bit register anywhere        (must never appear in a 32-bit decode)
 *   8  AH/CH/DH/BH high byte register  (cannot exist in 64-bit mode)
 *  16  has an explicit memory operand  (not a shape claim -- it tells python
 *      when ocerz models an address at all.  ocerz leaves string ops with no
 *      operands, so without this the 0xa4..0xaf block would report a bogus
 *      address-width difference against every disassembler on earth.)
 */
static uint8_t shape_flags(const X86Insn *ins)
{
    uint8_t f = 0;
    for (int i = 0; i < ins->nops && i < 3; i++) {
        const X86Operand *o = &ins->ops[i];
        if (o->high8)
            f |= 8;
        if (o->kind == OCERZ_OPK_REG && o->size == 8)
            f |= 4;
        if (o->kind != OCERZ_OPK_MEM)
            continue;
        f |= 16;
        if (o->riprel) {
            f |= 1;                       /* base/index are NONE when riprel */
            continue;
        }
        if (o->base == OCERZ_REG_NONE && o->index == OCERZ_REG_NONE)
            continue;                     /* displacement only: no reg width */
        if (ins->addrsize == 2)
            f |= 2;
        else if (ins->addrsize == 8)
            f |= 4;
    }
    return f;
}

static void record(Rec *r, const uint8_t *code, uint64_t ip, int m32)
{
    X86Insn ins;
    memset(&ins, 0, sizeof ins);
    int rc = decode_as(code, PROBE_LEN, ip, &ins, m32);
    r->pad = 0;
    r->pad2 = 0;
    if (rc != OCERZ_OK) {
        r->status = 0;
        r->len = 0;
        r->flags = 0;
        r->op = (uint16_t)(-rc);
    } else {
        r->status = 1;
        r->len = ins.len;
        r->flags = shape_flags(&ins);
        r->op = ins.op;
    }
}

static int sweep(int m32, const char *path, int quick)
{
    Rec *out = calloc((size_t)PROBE_N, sizeof(Rec));
    if (!out) { perror("malloc"); return 1; }
    uint64_t ip = m32 ? EIP32 : RIP64;
    uint8_t code[PROBE_LEN];
    /* quick: two-byte openings only -- a ~1s inner-loop gate while working on
     * a specific opcode.  Phase A stays zeroed and python skips it. */
    uint32_t from = quick ? SWEEP3_N : 0;
    for (uint32_t i = from; i < PROBE_N; i++) {
        build_probe(i, code);
        record(&out[i], code, ip, m32);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); free(out); return 1; }
    if (fwrite(out, sizeof(Rec), PROBE_N, f) != PROBE_N) {
        perror(path); fclose(f); free(out); return 1;
    }
    fclose(f);
    free(out);
    fprintf(stderr, "i386diff: wrote %u %d-bit records to %s\n",
            PROBE_N, m32 ? 32 : 64, path);
    return 0;
}

/* ---- human-readable single probe --------------------------------------- */

static void show(const uint8_t *code, int nbytes, uint64_t ip, int m32)
{
    X86Insn ins;
    memset(&ins, 0, sizeof ins);
    int rc = decode_as(code, (size_t)nbytes, ip, &ins, m32);
    for (int i = 0; i < nbytes && i < PROBE_LEN; i++)
        printf("%02x", code[i]);
    if (rc != OCERZ_OK) {
        printf(" : rc=%d\n", rc);
        return;
    }
    char txt[256];
    ocerz_format_insn(&ins, txt, sizeof txt);
    printf(" : len=%u op=%s opsize=%u addrsize=%u seg=%u rep=%u lock=%u shape=%u : %s\n",
           ins.len, ocerz_op_name(ins.op), ins.opsize, ins.addrsize,
           ins.seg, ins.rep, ins.lock, shape_flags(&ins), txt);
}

static int hexbytes(const char *s, uint8_t *out, int cap)
{
    int n = 0, hi = -1;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else continue;
        if (hi < 0) { hi = v; continue; }
        if (n < cap) out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return n;
}

/* ---- selftest: the documented capstone answers ------------------------- */

static const struct { const char *bytes; const char *want; } SELF[] = {
    { "60",             "pushal, len 1"          },
    { "61",             "popal, len 1"           },
    { "37",             "aaa, len 1"             },
    { "40",             "inc eax, len 1"         },
    { "a118000000",     "mov eax,[0x18], len 5"  },
    { "64a118000000",   "mov eax,fs:[0x18], len 6 -- the TEB read" },
    { "ff20",           "jmp dword ptr [eax], len 2" },
    { "66ffe0",         "jmp ax, len 3"          },
    { "e800000000",     "call rel32, len 5"      },
    { "66e80000",       "call rel16, len 4"      },
    { "9a1122334455",   "lcall far ptr, len 7"   },
    { "62c0",           "bound, len 2"           },
    { "c400",           "les, len 2"             },
    { "d40a",           "aam 0xa, len 2"         },
};

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "params")) {
        printf("sweep3_bits=%d\n", SWEEP3_BITS);
        printf("sweep2_bits=%d\n", SWEEP2_BITS);
        printf("probe_n=%u\n", PROBE_N);
        printf("probe_len=%d\n", PROBE_LEN);
        printf("tail_a=");
        for (size_t i = 0; i < sizeof TAIL_A; i++) printf("%02x", TAIL_A[i]);
        printf("\ntail_b=");
        for (size_t i = 0; i < sizeof TAIL_B; i++) printf("%02x", TAIL_B[i]);
        printf("\neip32=%llx\n", (unsigned long long)EIP32);
        printf("rip64=%llx\n", (unsigned long long)RIP64);
        printf("entry=%s\n", ENTRY_DESC);
        printf("entry_is_real=%d\n", ENTRY_IS_REAL);
        printf("rec_size=%zu\n", sizeof(Rec));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "names")) {
        for (unsigned i = 0; i < OCERZ_OP_COUNT; i++)
            printf("%u %s\n", i, ocerz_op_name(i));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "sweep")) {
        int m32 = !strcmp(argv[2], "32");
        int quick = argc >= 5 && !strcmp(argv[4], "quick");
        return sweep(m32, argv[3], quick);
    }
    if (argc >= 3 && !strcmp(argv[1], "probe")) {
        uint32_t i = (uint32_t)strtoul(argv[2], NULL, 0);
        uint8_t code[PROBE_LEN];
        if (i >= PROBE_N) { fprintf(stderr, "index out of range\n"); return 2; }
        build_probe(i, code);
        for (int k = 0; k < PROBE_LEN; k++) printf("%02x", code[k]);
        printf("\n");
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "line")) {
        int m32 = !strcmp(argv[2], "32");
        uint32_t i = (uint32_t)strtoul(argv[3], NULL, 0);
        uint8_t code[PROBE_LEN];
        if (i >= PROBE_N) { fprintf(stderr, "index out of range\n"); return 2; }
        build_probe(i, code);
        show(code, PROBE_LEN, m32 ? EIP32 : RIP64, m32);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "bytes")) {
        int m32 = !strcmp(argv[2], "32");
        uint8_t code[PROBE_LEN];
        memset(code, 0x90, sizeof code);
        int n = hexbytes(argv[3], code, PROBE_LEN);
        if (n <= 0) { fprintf(stderr, "no hex bytes\n"); return 2; }
        show(code, PROBE_LEN, m32 ? EIP32 : RIP64, m32);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        printf("entry: %s\n", ENTRY_DESC);
        for (size_t i = 0; i < sizeof SELF / sizeof SELF[0]; i++) {
            uint8_t code[PROBE_LEN];
            memset(code, 0x90, sizeof code);
            hexbytes(SELF[i].bytes, code, PROBE_LEN);
            printf("  want %-42s got ", SELF[i].want);
            show(code, PROBE_LEN, EIP32, 1);
        }
        return 0;
    }
    fprintf(stderr,
        "usage: i386diff params | names | sweep <32|64> <out.bin> [quick]\n"
        "       i386diff probe <index> | line <32|64> <index>\n"
        "       i386diff bytes <32|64> <hex> | selftest\n");
    return 2;
}
