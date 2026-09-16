/*
 * The synthesized libSystem, checked as the loader will actually read it.
 *
 * Native mode maps no shared cache, so ocerz_vdylib_image builds an x86_64
 * Mach-O for /usr/lib/libSystem.B.dylib in a malloc buffer and the ordinary
 * loader maps that buffer.  Every property asserted here is one some other
 * piece of ocerz keys on, so each is read back through the reader that keys on
 * it rather than through a copy written for the test: the export trie is
 * walked with ocerz_dyld_trie_resolve, the exact function import resolution
 * calls, and the twelve bytes behind each export are read with ocerz_decode,
 * the same decoder the interpreter and the JIT will see them through.  A
 * hand-rolled walker here could agree with an image the real one gets wrong.
 *
 * The trie is the case worth having.  Its reader matches a child edge by
 * prefix, takes the first match and never backtracks, so an image that files
 * every export as one flat level of children loses _writev the moment _write
 * is listed ahead of it: the reader takes the _write edge, arrives at a leaf
 * with a "v" still to spend, finds no children and gives up.  Order the two
 * the other way and both work, which is exactly what makes it the kind of bug
 * that survives a casual test.  The export list carries five such
 * proper-prefix pairs on purpose; each is asserted to resolve at both lengths
 * and to reach two different stubs, so a reader that loses the longer name and
 * one that aliases it to the shorter are both caught.  _wri, _writ, _memcp,
 * _writev2 and _strlength are asserted to reach nothing at all.
 *
 * A resolved address is header-relative: the trie stores offsets from the mach
 * header, and __TEXT carries that header because its fileoff is zero, so a
 * stub's offset in the buffer is its resolved address minus whatever load base
 * the caller passed - here an arbitrary one, since nothing is mapped.  The
 * jump's displacement is followed the same way into __DATA, where the slot has
 * to hold OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF: the one address inside the trap
 * window that turns a guest call to a system function into a bridge dispatch,
 * and the reason a synthesized image needs no fixups at all.
 *
 * Both of __TEXT's protection words are asserted to be exactly read and
 * execute and not write, and both are asserted rather than one, because
 * protect_ro_segments tests those three bits in the word at segment offset 56
 * - maxprot, whatever its local there is called - and silently leaves the
 * stubs mapped writable if any of them is off.  An image that set only the
 * word the segment structure calls initprot would pass a reading of that code
 * and still not be re-protected.
 */
#include "ocerz/vdylib.h"
#include "ocerz/dyld.h"
#include "ocerz/dyldapi.h"
#include "ocerz/decode.h"
#include "ocerz/cpu.h"

#include <mach-o/loader.h>
#include <mach/machine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD_BASE       0x0000000210000000ull
#define MOV_LEN         6
#define JMP_LEN         6
#define STUB_LEN        (MOV_LEN + JMP_LEN)
#define LC_EXPORTS_TRIE 0x80000033u
#define IMAGE_MAX       (16u * 1024u * 1024u)

static int checks;
static int failures;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static const char *const kLib = "/usr/lib/libSystem.B.dylib";

static const char *const kExports[] = {
    "___bzero", "_memcpy", "_memcmp", "_memmove", "_memset",
    "_strcmp", "_strncmp", "_strcpy", "_strlen",
    "_write", "_writev", "_read", "_readv",
    "_open", "_opendir", "_close", "_closedir",
    "_time", "_times", "_exit", "_malloc", "_free",
};
#define NEXPORTS (sizeof kExports / sizeof kExports[0])

static const char *const kPrefixPairs[][2] = {
    { "_write", "_writev" },
    { "_read",  "_readv" },
    { "_open",  "_opendir" },
    { "_close", "_closedir" },
    { "_time",  "_times" },
};
#define NPAIRS (sizeof kPrefixPairs / sizeof kPrefixPairs[0])

static const char *const kNotExported[] = {
    "_wri", "_writ", "_memcp", "_writev2", "_strlength",
    "_ocerz_no_such_export_xyzzy",
};
#define NNOT (sizeof kNotExported / sizeof kNotExported[0])

typedef struct {
    int found;
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
} SegInfo;

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void seg_from(const uint8_t *lc, SegInfo *s)
{
    struct segment_command_64 sc;
    memcpy(&sc, lc, sizeof sc);
    s->found = 1;
    s->vmaddr = sc.vmaddr;
    s->vmsize = sc.vmsize;
    s->fileoff = sc.fileoff;
    s->filesize = sc.filesize;
    s->maxprot = (uint32_t)sc.maxprot;
    s->initprot = (uint32_t)sc.initprot;
}

static void check_distinct(const char *what, const uint64_t *v, const int *ok, size_t n)
{
    size_t bad_i = n, bad_j = n;
    char msg[192];

    for (size_t i = 0; i < n && bad_i == n; i++) {
        if (!ok[i])
            continue;
        for (size_t j = i + 1; j < n; j++) {
            if (ok[j] && v[i] == v[j]) {
                bad_i = i;
                bad_j = j;
                break;
            }
        }
    }
    if (bad_i == n)
        snprintf(msg, sizeof msg, "every export has its own %s", what);
    else
        snprintf(msg, sizeof msg, "%s and %s share a %s (%#llx)",
                 kExports[bad_i], kExports[bad_j], what,
                 (unsigned long long)v[bad_i]);
    CHECK(bad_i == n, "%s", msg);
}

static int report(void)
{
    printf("test_vdylib: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

int main(void)
{
    CHECK(ocerz_vdylib_have(kLib) != 0,
          "ocerz_vdylib_have(\"%s\") says no", kLib);
    CHECK(ocerz_vdylib_have("/usr/lib/libNoSuchLibrary.dylib") == 0,
          "ocerz_vdylib_have claims a library nobody provides");
    CHECK(ocerz_vdylib_have("") == 0,
          "ocerz_vdylib_have claims the empty install name");

    size_t len = 0;
    uint8_t *img = ocerz_vdylib_image(kLib, &len);
    CHECK(img != NULL, "ocerz_vdylib_image(\"%s\") returned no buffer", kLib);
    if (!img)
        return report();

    CHECK(len > sizeof(struct mach_header_64),
          "image is %zu bytes, too small to hold a mach header", len);
    CHECK(len < IMAGE_MAX, "image is %zu bytes, which is implausible", len);
    if (len <= sizeof(struct mach_header_64) || len >= IMAGE_MAX) {
        free(img);
        return report();
    }

    struct mach_header_64 mh;
    memcpy(&mh, img, sizeof mh);
    CHECK(mh.magic == MH_MAGIC_64, "magic is %#x, want MH_MAGIC_64", mh.magic);
    CHECK(mh.cputype == CPU_TYPE_X86_64,
          "cputype is %d, want CPU_TYPE_X86_64", (int)mh.cputype);
    CHECK(mh.filetype == MH_DYLIB, "filetype is %u, want MH_DYLIB", mh.filetype);
    CHECK(mh.ncmds > 0, "ncmds is zero");
    CHECK(mh.sizeofcmds > 0, "sizeofcmds is zero");
    CHECK((uint64_t)sizeof mh + mh.sizeofcmds <= (uint64_t)len,
          "header plus sizeofcmds %u runs past the %zu byte image",
          mh.sizeofcmds, len);
    if (mh.magic != MH_MAGIC_64 || mh.ncmds == 0 ||
        (uint64_t)sizeof mh + mh.sizeofcmds > (uint64_t)len) {
        free(img);
        return report();
    }

    SegInfo text, data;
    memset(&text, 0, sizeof text);
    memset(&data, 0, sizeof data);
    uint32_t trie_off = 0, trie_size = 0;
    int trie_found = 0, id_found = 0, id_named = 0, malformed = 0;
    uint32_t walked = 0, seen = 0;
    const uint8_t *lc = img + sizeof mh;

    for (uint32_t i = 0; i < mh.ncmds; i++) {
        if ((uint64_t)walked + 8 > mh.sizeofcmds) {
            malformed = 1;
            break;
        }
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (csize < 8 || (csize & 7) != 0 ||
            (uint64_t)walked + csize > mh.sizeofcmds) {
            malformed = 1;
            break;
        }
        if (cmd == LC_SEGMENT_64 && csize >= sizeof(struct segment_command_64)) {
            if (memcmp(lc + 8, "__TEXT", 7) == 0)
                seg_from(lc, &text);
            else if (memcmp(lc + 8, "__DATA", 7) == 0)
                seg_from(lc, &data);
        } else if (cmd == LC_EXPORTS_TRIE && csize >= 16) {
            trie_found = 1;
            trie_off = rd32(lc + 8);
            trie_size = rd32(lc + 12);
        } else if (cmd == LC_ID_DYLIB && csize >= 24) {
            uint32_t noff = rd32(lc + 8);
            id_found = 1;
            if (noff >= 24 && noff < csize) {
                const char *nm = (const char *)(lc + noff);
                size_t room = csize - noff;
                id_named = strnlen(nm, room) < room && strcmp(nm, kLib) == 0;
            }
        }
        lc += csize;
        walked += csize;
        seen++;
    }

    CHECK(!malformed, "load command %u is malformed or runs past sizeofcmds", seen);
    CHECK(seen == mh.ncmds,
          "walked %u load commands, ncmds says %u", seen, mh.ncmds);
    CHECK(walked == mh.sizeofcmds,
          "load commands span %u bytes, sizeofcmds says %u", walked, mh.sizeofcmds);

    CHECK(id_found, "no LC_ID_DYLIB");
    CHECK(id_named, "LC_ID_DYLIB does not name \"%s\"", kLib);

    CHECK(trie_found, "no LC_DYLD_EXPORTS_TRIE");
    CHECK(trie_size != 0, "LC_DYLD_EXPORTS_TRIE has a zero datasize");
    CHECK((uint64_t)trie_off + trie_size <= (uint64_t)len,
          "the export trie [%u,%llu) is not inside the %zu byte image",
          trie_off, (unsigned long long)trie_off + trie_size, len);

    CHECK(text.found, "no __TEXT segment");
    CHECK(data.found, "no __DATA segment");
    if (!text.found || !data.found) {
        free(img);
        return report();
    }

    CHECK(text.fileoff == 0,
          "__TEXT fileoff is %llu, want 0", (unsigned long long)text.fileoff);
    CHECK(text.filesize != 0, "__TEXT filesize is zero");
    CHECK(text.vmsize != 0, "__TEXT vmsize is zero");
    CHECK(text.fileoff + text.filesize <= (uint64_t)len,
          "__TEXT file range ends at %llu, past the %zu byte image",
          (unsigned long long)(text.fileoff + text.filesize), len);
    CHECK((text.initprot & VM_PROT_READ) != 0,
          "__TEXT initprot %#x is not readable", text.initprot);
    CHECK((text.initprot & VM_PROT_EXECUTE) != 0,
          "__TEXT initprot %#x is not executable", text.initprot);
    CHECK((text.initprot & VM_PROT_WRITE) == 0,
          "__TEXT initprot %#x is writable", text.initprot);
    CHECK((text.maxprot & VM_PROT_READ) != 0,
          "__TEXT maxprot %#x is not readable", text.maxprot);
    CHECK((text.maxprot & VM_PROT_EXECUTE) != 0,
          "__TEXT maxprot %#x is not executable, so protect_ro_segments leaves the stubs writable",
          text.maxprot);
    CHECK((text.maxprot & VM_PROT_WRITE) == 0,
          "__TEXT maxprot %#x is writable, so protect_ro_segments leaves the stubs writable",
          text.maxprot);

    CHECK(data.filesize != 0,
          "__DATA filesize is zero, so the bridge slots are not in the image");
    CHECK(data.fileoff + data.filesize <= (uint64_t)len,
          "__DATA file range ends at %llu, past the %zu byte image",
          (unsigned long long)(data.fileoff + data.filesize), len);
    CHECK(data.vmaddr >= text.vmaddr,
          "__DATA vmaddr %#llx is below __TEXT vmaddr %#llx",
          (unsigned long long)data.vmaddr, (unsigned long long)text.vmaddr);
    if (data.vmaddr < text.vmaddr || data.filesize == 0) {
        free(img);
        return report();
    }

    const uint64_t text_lo = LOAD_BASE;
    const uint64_t text_hi = LOAD_BASE + text.vmsize;
    const uint64_t data_lo = LOAD_BASE + (data.vmaddr - text.vmaddr);
    const uint64_t data_hi = data_lo + data.filesize;
    const uint64_t want_slot = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;

    uint64_t addr[NEXPORTS], ids[NEXPORTS], slots[NEXPORTS];
    int ok[NEXPORTS];

    for (size_t i = 0; i < NEXPORTS; i++) {
        const char *sym = kExports[i];
        addr[i] = ids[i] = slots[i] = 0;
        ok[i] = 0;

        int found = 0;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, sym, &found);
        CHECK(found, "%s does not resolve through the export trie", sym);
        if (!found)
            continue;

        int in_text = a >= text_lo && a + STUB_LEN <= text_hi;
        CHECK(in_text,
              "%s resolved to %#llx, outside the mapped __TEXT [%#llx,%#llx)",
              sym, (unsigned long long)a, (unsigned long long)text_lo,
              (unsigned long long)text_hi);
        if (!in_text)
            continue;
        addr[i] = a;

        uint64_t foff = text.fileoff + (a - text_lo);
        CHECK(foff + STUB_LEN <= (uint64_t)len,
              "%s stub at offset %llu runs past the %zu byte image",
              sym, (unsigned long long)foff, len);
        if (foff + STUB_LEN > (uint64_t)len)
            continue;

        X86Insn mov;
        memset(&mov, 0, sizeof mov);
        int r = ocerz_decode(img + foff, len - (size_t)foff, a, &mov);
        CHECK(r == OCERZ_OK,
              "%s stub: the first instruction does not decode (%d)", sym, r);
        if (r != OCERZ_OK)
            continue;
        CHECK(mov.op == OCERZ_OP_MOV,
              "%s stub: the first instruction is %s, want MOV",
              sym, ocerz_op_name(mov.op));
        CHECK(mov.len == MOV_LEN,
              "%s stub: the first instruction is %u bytes, want %d",
              sym, mov.len, MOV_LEN);
        CHECK(mov.nops == 2, "%s stub: the first instruction has %u operands, want 2",
              sym, mov.nops);
        CHECK(mov.ops[0].kind == OCERZ_OPK_REG && mov.ops[0].reg == OCERZ_R11 &&
              mov.ops[0].size == 4,
              "%s stub: the first instruction does not write r11d", sym);
        CHECK(mov.ops[1].kind == OCERZ_OPK_IMM,
              "%s stub: the first instruction's source is not an immediate", sym);
        ids[i] = mov.ops[1].imm;

        X86Insn jmp;
        memset(&jmp, 0, sizeof jmp);
        r = ocerz_decode(img + foff + MOV_LEN, len - (size_t)foff - MOV_LEN,
                         a + MOV_LEN, &jmp);
        CHECK(r == OCERZ_OK,
              "%s stub: the second instruction does not decode (%d)", sym, r);
        if (r != OCERZ_OK)
            continue;
        CHECK(jmp.op == OCERZ_OP_JMP,
              "%s stub: the second instruction is %s, want JMP",
              sym, ocerz_op_name(jmp.op));
        CHECK(jmp.len == JMP_LEN,
              "%s stub: the second instruction is %u bytes, want %d",
              sym, jmp.len, JMP_LEN);
        CHECK(jmp.opsize == 8,
              "%s stub: the jump reads %u bytes, want 8", sym, jmp.opsize);
        CHECK(jmp.nops == 1 && jmp.ops[0].kind == OCERZ_OPK_MEM,
              "%s stub: the jump is not through memory", sym);
        CHECK(jmp.ops[0].riprel == 1,
              "%s stub: the jump's address is not rip-relative", sym);
        CHECK(jmp.ops[0].base == OCERZ_REG_NONE &&
              jmp.ops[0].index == OCERZ_REG_NONE,
              "%s stub: the jump's address carries a register term", sym);
        if (jmp.nops != 1 || jmp.ops[0].kind != OCERZ_OPK_MEM ||
            jmp.ops[0].riprel != 1)
            continue;

        uint64_t slot = (uint64_t)jmp.ops[0].disp;
        int in_data = slot >= data_lo && slot + 8 <= data_hi;
        CHECK(in_data,
              "%s stub: the jump reads %#llx, outside the mapped __DATA [%#llx,%#llx)",
              sym, (unsigned long long)slot, (unsigned long long)data_lo,
              (unsigned long long)data_hi);
        if (!in_data)
            continue;
        slots[i] = slot;
        ok[i] = 1;

        uint64_t soff = data.fileoff + (slot - data_lo);
        CHECK(soff + 8 <= (uint64_t)len,
              "%s stub: its __DATA slot at offset %llu is past the %zu byte image",
              sym, (unsigned long long)soff, len);
        if (soff + 8 > (uint64_t)len)
            continue;

        uint64_t got = rd64(img + soff);
        CHECK(got == want_slot,
              "%s stub: its __DATA slot holds %#llx, want %#llx",
              sym, (unsigned long long)got, (unsigned long long)want_slot);
    }

    check_distinct("stub address", addr, ok, NEXPORTS);
    check_distinct("export id", ids, ok, NEXPORTS);
    check_distinct("__DATA slot", slots, ok, NEXPORTS);

    for (size_t i = 0; i < NPAIRS; i++) {
        const char *shorter = kPrefixPairs[i][0];
        const char *longer = kPrefixPairs[i][1];
        int fs = 0, fl = 0;
        uint64_t as = ocerz_dyld_trie_resolve(img, LOAD_BASE, shorter, &fs);
        uint64_t al = ocerz_dyld_trie_resolve(img, LOAD_BASE, longer, &fl);

        CHECK(fs && fl, "%s / %s: one of the pair does not resolve", shorter, longer);
        CHECK(!(fs && fl) || as != al,
              "%s and %s both resolve to %#llx: the trie matched the shorter name by prefix",
              shorter, longer, (unsigned long long)as);
    }

    for (size_t i = 0; i < NNOT; i++) {
        int found = 1;
        uint64_t a = ocerz_dyld_trie_resolve(img, LOAD_BASE, kNotExported[i], &found);
        CHECK(!found, "%s is not an export but resolved to %#llx",
              kNotExported[i], (unsigned long long)a);
        CHECK(found || a == 0, "%s reported not-found but returned %#llx",
              kNotExported[i], (unsigned long long)a);
    }

    free(img);
    return report();
}
