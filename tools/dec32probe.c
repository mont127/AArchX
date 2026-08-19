/* dec32probe -- read hex instruction bytes, print one canonical decode record.
 *
 * The i386 counterpart of decodiff: where decodiff proves 64-bit decoding is
 * unchanged, this feeds a real disassembler (capstone CS_MODE_32, driven by
 * tools/dec32-oracle.py) the same byte strings and compares field by field.
 * Links against src/decode.o alone, native arm64.
 *
 *   dec32probe                    -- decode stdin lines in 32-bit mode
 *   dec32probe 64                 -- ... in 64-bit mode (A/B on shared paths)
 *   dec32probe sweep <M> <out>    -- exhaustive 2^24 length sweep, one byte
 *                                    per three-byte opening (0 == decode
 *                                    error), for dec32-oracle.py's sweep suite
 *
 * Each stdin line is whitespace-separated hex bytes.  Output is one line per
 * input line: either "ERR rc=N" or a canonical field dump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ocerz/decode.h"

#define RIP 0x00401000ull

int main(int argc, char **argv)
{
    int mode32 = 1;
    if (argc >= 2 && !strcmp(argv[1], "64"))
        mode32 = 0;

    if (argc >= 4 && !strcmp(argv[1], "sweep")) {
        static const uint8_t TAIL[13] = {
            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd
        };
        int m32 = atoi(argv[2]) == 32;
        uint32_t n = 1u << 24;
        uint8_t *out = malloc(n);
        if (!out) { perror("malloc"); return 1; }
        uint8_t code[16];
        memcpy(code + 3, TAIL, 13);
        for (uint32_t i = 0; i < n; i++) {
            code[0] = (uint8_t)i;
            code[1] = (uint8_t)(i >> 8);
            code[2] = (uint8_t)(i >> 16);
            X86Insn ins;
            memset(&ins, 0, sizeof ins);
            out[i] = ocerz_decode_mode(code, 16, RIP, &ins, m32) ? 0 : ins.len;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) { perror(argv[3]); return 1; }
        fwrite(out, 1, n, f);
        fclose(f);
        fprintf(stderr, "dec32probe: swept %u %d-bit openings\n", n,
                m32 ? 32 : 64);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        uint8_t code[32];
        size_t n = 0;
        const char *p = line;
        while (n < 16) {
            char *q;
            unsigned long v = strtoul(p, &q, 16);
            if (q == p)
                break;
            code[n++] = (uint8_t)v;
            p = q;
        }
        if (n == 0)
            continue;
        /* Pad to 16 with NOPs so a short test string is never a spurious
         * ETRUNC; the decoder must still report the true length. */
        while (n < 16)
            code[n++] = 0x90;

        X86Insn ins;
        memset(&ins, 0, sizeof ins);
        int rc = ocerz_decode_mode(code, 16, RIP, &ins, mode32);
        if (rc != 0) {
            printf("ERR rc=%d\n", rc);
            fflush(stdout);
            continue;
        }
        printf("OK len=%u op=%s opsize=%u addrsize=%u rep=%u lock=%u seg=%u "
               "cc=%u nops=%u",
               ins.len, ocerz_op_name(ins.op), ins.opsize, ins.addrsize,
               ins.rep, ins.lock, ins.seg, ins.cc, ins.nops);
        for (int i = 0; i < ins.nops && i < 3; i++) {
            const X86Operand *o = &ins.ops[i];
            printf(" |%d k=%u r=%u sz=%u h8=%u b=%u ix=%u sc=%u rip=%u "
                   "d=%lld imm=%llu",
                   i, o->kind, o->reg, o->size, o->high8, o->base, o->index,
                   o->scale, o->riprel, (long long)o->disp,
                   (unsigned long long)o->imm);
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
