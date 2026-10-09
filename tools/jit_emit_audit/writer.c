/*
 * Audit-only C ABI sink, shared by C and Rust translators. Normal builds do
 * not link this file or call the sink. An instrumented translator calls it
 * while holding its existing translation lock, after all words/relocations
 * are finalized and before tc_bind rewrites process-dependent addresses.
 * OCERZ_JIT_EMIT_AUDIT selects the output file; an unset/empty value disables
 * recording. The runner always sets it for each isolated corpus process.
 *
 * AXJITA01 is a little-endian stream without C struct padding. Each record
 * begins with rip:u64, nwords:u32, nrel:u32, ninsns:u32, followed by raw arm64
 * words, relocations {off:u32, kind:u8, form:u8, arg:u64}, then instructions
 * {rip:u64, len:u8, textlen:u16, text:textlen bytes}. The reader validates and
 * masks only address payloads; raw bytes remain available for diagnostics.
 * Formatting is diagnostic only and is not part of the equality predicate.
 * Flush each block so even a subsequent guest crash leaves useful evidence.
 */
#include "ocerz/jit_internal.h"
#include <errno.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The emission audit stream requires a little-endian host
#endif

static FILE *audit_file;
static int audit_initialized;

static void audit_fail(const char *operation)
{
    fprintf(stderr, "jit_emit_audit: %s failed: %s\n", operation, strerror(errno));
    _Exit(2);
}

static void audit_close(void)
{
    if (fclose(audit_file) != 0) audit_fail("close");
}

static void audit_write(const void *data, size_t size)
{
    if (fwrite(data, 1, size, audit_file) != size) audit_fail("write");
}

void ocerz_jit_emit_audit(uint64_t rip, const uint32_t *code, uint32_t nwords,
                          const X86Insn *insns, uint32_t ninsns,
                          const TcReloc *rel, uint32_t nrel)
{
    if (!audit_initialized) {
        const char *path = getenv("OCERZ_JIT_EMIT_AUDIT");
        audit_initialized = 1;
        if (!path || !*path) return;
        audit_file = fopen(path, "wb");
        if (!audit_file) audit_fail("open");
        if (atexit(audit_close) != 0) audit_fail("atexit");
        audit_write("AXJITA01", 8);
    }
    if (!audit_file) return;
    audit_write(&rip, 8);
    audit_write(&nwords, 4);
    audit_write(&nrel, 4);
    audit_write(&ninsns, 4);
    audit_write(code, (size_t)nwords * 4);
    for (uint32_t i = 0; i < nrel; i++) {
        audit_write(&rel[i].off, 4);
        audit_write(&rel[i].kind, 1);
        audit_write(&rel[i].form, 1);
        audit_write(&rel[i].arg, 8);
    }
    for (uint32_t i = 0; i < ninsns; i++) {
        char text[256];
        ocerz_format_insn(&insns[i], text, sizeof text);
        uint16_t len = (uint16_t)strlen(text);
        audit_write(&insns[i].rip, 8);
        audit_write(&insns[i].len, 1);
        audit_write(&len, 2);
        audit_write(text, len);
    }
    if (fflush(audit_file) != 0) audit_fail("flush");
}
