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
 * X64 captures align entries to 64 bytes and retain native relocations. Each
 * PID owns an append-only stream (including across exec). The sole implicit
 * native pointer, a validated MOVZ/MOVK x16 + BLR of vdylib_fastcall, gets an
 * audit-only descriptor: kind 128, form 2, arg (word_count << 32) | symbol 1.
 * These extra descriptors never enter tc_bind or the production disk cache.
 */
#include "ocerz/jit_internal.h"
#include <errno.h>
#include <limits.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The emission audit stream requires a little-endian host
#endif

static FILE *audit_file;
static int audit_initialized;
static pid_t audit_pid;

int ocerz_jit_emit_audit_begin(OcerzJit *jit)
{
    if (!getenv("OCERZ_JIT_AUDIT_X64")) return 0;
    uintptr_t aligned = ((uintptr_t)jit->code_cur + 63) & ~(uintptr_t)63;
    if (aligned < (uintptr_t)jit->code_end) jit->code_cur = (uint32_t *)aligned;
    return 1;
}

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

static int audit_native_reloc(const uint32_t *code, uint32_t nwords, uint32_t off,
                              const TcReloc *rel, uint32_t nrel, TcReloc *out)
{
    if ((code[off] & 0xffe0001f) != 0xd2800010) return 0;
    uint64_t value = (code[off] >> 5) & 0xffff;
    uint32_t end = off + 1, shift = 0;
    while (end < nwords && end - off < 4 && (code[end] & 0xff80001f) == 0xf2800010) {
        uint32_t next = (code[end] >> 21) & 3;
        if (next <= shift) return 0;
        shift = next;
        value |= (uint64_t)((code[end++] >> 5) & 0xffff) << (shift * 16);
    }
    if (end >= nwords || code[end] != 0xd63f0200 || value != (uintptr_t)&ocerz_vdylib_fastcall) return 0;
    for (uint32_t i = 0; i < nrel; i++) if (rel[i].off == off) return 0;
    *out = (TcReloc){ .off = off, .kind = 128, .form = 2, .arg = ((uint64_t)(end - off) << 32) | 1 };
    return 1;
}

static void audit_reloc(const TcReloc *rel)
{
    audit_write(&rel->off, 4);
    audit_write(&rel->kind, 1);
    audit_write(&rel->form, 1);
    audit_write(&rel->arg, 8);
}

void ocerz_jit_emit_audit(uint64_t rip, const uint32_t *code, uint32_t nwords,
                          const X86Insn *insns, uint32_t ninsns,
                          const TcReloc *rel, uint32_t nrel)
{
    if (!audit_initialized || (audit_pid != getpid() && getenv("OCERZ_JIT_AUDIT_X64"))) {
        const char *path = getenv("OCERZ_JIT_EMIT_AUDIT");
        audit_initialized = 1;
        if (!path || !*path) return;
        char process_path[PATH_MAX];
        if (getenv("OCERZ_JIT_AUDIT_X64")) {
            int size = snprintf(process_path, sizeof process_path, "%s.%d", path, getpid());
            if (size < 0 || (size_t)size >= sizeof process_path) { errno = ENAMETOOLONG; audit_fail("path"); }
            path = process_path;
        }
        if (audit_file) {
            if (fclose(audit_file) != 0) audit_fail("fork close");
        } else if (atexit(audit_close) != 0) audit_fail("atexit");
        audit_pid = getpid();
        int per_process = getenv("OCERZ_JIT_AUDIT_X64") != NULL;
        audit_file = fopen(path, per_process ? "ab+" : "wb");
        if (!audit_file) audit_fail("open");
        if (per_process && fseeko(audit_file, 0, SEEK_END) != 0) audit_fail("seek");
        off_t size = ftello(audit_file);
        if (size < 0) audit_fail("tell");
        if (!size) audit_write("AXJITA01", 8);
    }
    if (!audit_file) return;
    uint32_t total = nrel;
    TcReloc native_rel;
    if (getenv("OCERZ_JIT_AUDIT_X64"))
        for (uint32_t off = 0; off < nwords; off++)
            total += audit_native_reloc(code, nwords, off, rel, nrel, &native_rel);
    audit_write(&rip, 8);
    audit_write(&nwords, 4);
    audit_write(&total, 4);
    audit_write(&ninsns, 4);
    audit_write(code, (size_t)nwords * 4);
    for (uint32_t i = 0; i < nrel; i++) audit_reloc(&rel[i]);
    if (total != nrel)
        for (uint32_t off = 0; off < nwords; off++)
            if (audit_native_reloc(code, nwords, off, rel, nrel, &native_rel)) audit_reloc(&native_rel);
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
