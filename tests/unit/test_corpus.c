/* Decoder length cross-validation against the host toolchain. */
#include "ocerz/decode.h"

#include <stdio.h>
#include <string.h>

typedef struct CorpusEntry {
    const char *mnem;
    unsigned len;
    unsigned char bytes[15];
    unsigned nbytes;
} CorpusEntry;

#define X(MNEM, LEN, ...) \
    { (MNEM), (LEN), { __VA_ARGS__ }, (unsigned)(sizeof((unsigned char[]){ __VA_ARGS__ })) },

static const CorpusEntry corpus[] = {
#include "../corpus/corpus.inc"
};

#undef X

#define CORPUS_COUNT ((int)(sizeof(corpus) / sizeof(corpus[0])))

static void hex_bytes(char *dst, size_t cap, const unsigned char *b, unsigned n)
{
    size_t pos = 0;
    for (unsigned i = 0; i < n && pos + 4 < cap; i++)
        pos += (size_t)snprintf(dst + pos, cap - pos, "%02x ", b[i]);
    if (pos > 0 && dst[pos - 1] == ' ')
        dst[pos - 1] = '\0';
    else if (cap > 0)
        dst[0] = '\0';
}

int main(void)
{
    int failures = 0;

    for (int i = 0; i < CORPUS_COUNT; i++) {
        const CorpusEntry *e = &corpus[i];
        unsigned char buf[15];
        char hexbuf[64];

        memset(buf, 0, sizeof buf);
        memcpy(buf, e->bytes, e->nbytes);

        X86Insn out;
        memset(&out, 0, sizeof out);

        int rc = ocerz_decode(buf, sizeof buf, 0x140000000ull, &out);

        if (rc != OCERZ_OK) {
            hex_bytes(hexbuf, sizeof hexbuf, e->bytes, e->nbytes);
            fprintf(stderr,
                    "FAIL %-24s [%s]: decode rc=%d (expected OCERZ_OK), len=%u\n",
                    e->mnem, hexbuf, rc, e->len);
            failures++;
            continue;
        }

        if (out.len != e->len) {
            hex_bytes(hexbuf, sizeof hexbuf, e->bytes, e->nbytes);
            fprintf(stderr,
                    "FAIL %-24s [%s]: len got=%u expected=%u\n",
                    e->mnem, hexbuf, (unsigned)out.len, e->len);
            failures++;
            continue;
        }
    }

    if (failures == 0)
        printf("test_corpus: OK (%d instructions)\n", CORPUS_COUNT);
    else
        fprintf(stderr, "test_corpus: %d/%d FAILED\n", failures, CORPUS_COUNT);

    return failures;
}
