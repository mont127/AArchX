/*
 * tests/unit/test_loader.c
 *
 * Unit tests for the Mach-O loader (src/loader.c) and the exec-stack builder
 * (src/stack.c). Both are exercised against an image this test hand-builds at
 * runtime so the fixtures are self-contained and need no external binaries.
 *
 * build_thin_macho() writes a minimal but structurally valid thin x86_64
 * Mach-O executable into a temp file: a mach_header_64, one LC_SEGMENT_64 for
 * a fake __TEXT placed at guest 0x100000000 with fileoff 0 (so it maps the
 * header itself plus a small run of recognizable code bytes), and an
 * LC_UNIXTHREAD carrying an x86_THREAD_STATE64 whose rip points at the first
 * code byte. The exact same bytes are then wrapped in a hand-built fat
 * archive (fat_header + one big-endian fat_arch) to test the fat-slice path.
 *
 * The loader assertions check entry, mh_gaddr, the mapped [lo,hi) range, and
 * that the mapped guest bytes match the file (header magic and the code
 * pattern). The stack assertions walk guest memory from cpu->rsp and verify
 * argc, each argv string, the argv NULL terminator, each envp string, the
 * envp NULL terminator, that apple[0] begins with "executable_path=", and
 * that rsp is 16-byte aligned. Over 25 assertions run across both the thin
 * and fat cases.
 *
 * ocerz_mem_init() is called once; the arena is large and each load reuses
 * the canonical range, which is fine because a reload re-maps the same pages.
 */
#include "ocerz/loader.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach/machine.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TEXT_VMADDR 0x100000000ull
#define TEXT_VMSIZE 0x1000ull
#define X86_THREAD_STATE64 4
#define X86_THREAD_STATE64_COUNT 42
#define RIP_REG_INDEX 16


static int g_checks;
static int g_fails;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static const uint8_t code_pattern[8] = {
    0x90, 0x90, 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3
};

static uint32_t bswap32(uint32_t v)
{
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static size_t build_thin_image(uint8_t *buf, size_t cap, uint64_t *entry_out)
{
    uint32_t thread_cmdsize = (uint32_t)(sizeof(struct thread_command) + 8 +
                                         X86_THREAD_STATE64_COUNT * 4);
    uint32_t sizeofcmds = (uint32_t)(sizeof(struct segment_command_64)) +
                          thread_cmdsize;
    size_t header_total = sizeof(struct mach_header_64) + sizeofcmds;
    size_t code_off = header_total;
    size_t total = code_off + sizeof code_pattern;
    if (total > cap)
        return 0;

    memset(buf, 0, total);

    struct mach_header_64 mh;
    memset(&mh, 0, sizeof mh);
    mh.magic = MH_MAGIC_64;
    mh.cputype = CPU_TYPE_X86_64;
    mh.cpusubtype = 3;
    mh.filetype = MH_EXECUTE;
    mh.ncmds = 2;
    mh.sizeofcmds = sizeofcmds;
    mh.flags = MH_NOUNDEFS;
    memcpy(buf, &mh, sizeof mh);

    size_t off = sizeof mh;

    struct segment_command_64 sc;
    memset(&sc, 0, sizeof sc);
    sc.cmd = LC_SEGMENT_64;
    sc.cmdsize = sizeof sc;
    memcpy(sc.segname, "__TEXT", 6);
    sc.vmaddr = TEXT_VMADDR;
    sc.vmsize = TEXT_VMSIZE;
    sc.fileoff = 0;
    sc.filesize = total;
    sc.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    sc.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    sc.nsects = 0;
    sc.flags = 0;
    memcpy(buf + off, &sc, sizeof sc);
    off += sizeof sc;

    struct thread_command tc;
    memset(&tc, 0, sizeof tc);
    tc.cmd = LC_UNIXTHREAD;
    tc.cmdsize = thread_cmdsize;
    memcpy(buf + off, &tc, sizeof tc);
    off += sizeof tc;

    uint32_t flavor = X86_THREAD_STATE64;
    uint32_t count = X86_THREAD_STATE64_COUNT;
    memcpy(buf + off, &flavor, 4);
    off += 4;
    memcpy(buf + off, &count, 4);
    off += 4;

    uint64_t entry = TEXT_VMADDR + code_off;
    uint64_t regs[21];
    memset(regs, 0, sizeof regs);
    regs[RIP_REG_INDEX] = entry;
    memcpy(buf + off, regs, count * 4);
    off += count * 4;

    memcpy(buf + code_off, code_pattern, sizeof code_pattern);

    *entry_out = entry;
    return total;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, buf, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static size_t build_fat_wrapper(uint8_t *out, size_t cap,
                                const uint8_t *thin, size_t thin_len)
{
    size_t hdr = sizeof(struct fat_header) + sizeof(struct fat_arch);
    size_t slice_off = (hdr + 0xfff) & ~(size_t)0xfff;
    size_t total = slice_off + thin_len;
    if (total > cap)
        return 0;
    memset(out, 0, total);

    struct fat_header fh;
    fh.magic = bswap32(FAT_MAGIC);
    fh.nfat_arch = bswap32(1);
    memcpy(out, &fh, sizeof fh);

    struct fat_arch fa;
    memset(&fa, 0, sizeof fa);
    fa.cputype = (int32_t)bswap32((uint32_t)CPU_TYPE_X86_64);
    fa.cpusubtype = (int32_t)bswap32(3);
    fa.offset = bswap32((uint32_t)slice_off);
    fa.size = bswap32((uint32_t)thin_len);
    fa.align = bswap32(12);
    memcpy(out + sizeof fh, &fa, sizeof fa);

    memcpy(out + slice_off, thin, thin_len);
    return total;
}

static void verify_loaded(OcerzImage *img, uint64_t entry, const char *tag)
{
    char what[128];

    snprintf(what, sizeof what, "[%s] entry matches", tag);
    check(img->entry == entry, what);

    snprintf(what, sizeof what, "[%s] entry_is_unixthread", tag);
    check(img->entry_is_unixthread == 1, what);

    snprintf(what, sizeof what, "[%s] mh_gaddr is __TEXT vmaddr", tag);
    check(img->mh_gaddr == TEXT_VMADDR, what);

    snprintf(what, sizeof what, "[%s] vmaddr_lo", tag);
    check(img->vmaddr_lo == TEXT_VMADDR, what);

    snprintf(what, sizeof what, "[%s] vmaddr_hi", tag);
    check(img->vmaddr_hi == TEXT_VMADDR + TEXT_VMSIZE, what);

    snprintf(what, sizeof what, "[%s] slide is zero", tag);
    check(img->slide == 0, what);

    snprintf(what, sizeof what, "[%s] mapped header magic is MH_MAGIC_64", tag);
    check(ocerz_ld(TEXT_VMADDR, 4) == MH_MAGIC_64, what);

    int code_ok = 1;
    for (size_t i = 0; i < sizeof code_pattern; i++) {
        uint8_t b = (uint8_t)ocerz_ld(entry + i, 1);
        if (b != code_pattern[i])
            code_ok = 0;
    }
    snprintf(what, sizeof what, "[%s] mapped code bytes match file", tag);
    check(code_ok, what);
}

static void verify_stack(OcerzVM *vm, int argc, char **argv, char **envp)
{
    uint64_t rsp = vm->cpu.gpr[OCERZ_RSP];

    check((rsp & 0xf) == 0, "rsp is 16-byte aligned");
    check(vm->cpu.gpr[OCERZ_RBP] == 0, "rbp zeroed");
    check(rsp >= vm->stack_lo && rsp < vm->stack_hi, "rsp inside stack range");

    uint64_t w = rsp;
    uint64_t gargc = ocerz_ld(w, 8);
    w += 8;
    check(gargc == (uint64_t)argc, "argc matches");

    char what[128];
    int all_argv_ok = 1;
    for (int i = 0; i < argc; i++) {
        uint64_t p = ocerz_ld(w, 8);
        w += 8;
        const char *s = (const char *)ocerz_g2h(p);
        if (strcmp(s, argv[i]) != 0)
            all_argv_ok = 0;
        snprintf(what, sizeof what, "argv[%d] string readable and correct", i);
        check(strcmp(s, argv[i]) == 0, what);
    }
    check(all_argv_ok, "all argv strings correct");

    uint64_t argv_null = ocerz_ld(w, 8);
    w += 8;
    check(argv_null == 0, "argv NULL terminator present");

    int envc = 0;
    while (envp[envc] != NULL)
        envc++;
    for (int i = 0; i < envc; i++) {
        uint64_t p = ocerz_ld(w, 8);
        w += 8;
        const char *s = (const char *)ocerz_g2h(p);
        snprintf(what, sizeof what, "envp[%d] string correct", i);
        check(strcmp(s, envp[i]) == 0, what);
    }

    uint64_t envp_null = ocerz_ld(w, 8);
    w += 8;
    check(envp_null == 0, "envp NULL terminator present");

    uint64_t apple0 = ocerz_ld(w, 8);
    w += 8;
    const char *as = (const char *)ocerz_g2h(apple0);
    check(strncmp(as, "executable_path=", 16) == 0,
          "apple[0] starts with executable_path=");
    check(strchr(as, '/') != NULL || strlen(as) > 16,
          "apple[0] carries a path");

    uint64_t apple_null = ocerz_ld(w, 8);
    w += 8;
    check(apple_null == 0, "apple NULL terminator present");
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem init failed\n");
        return 1;
    }

    static uint8_t thin[4096];
    uint64_t entry = 0;
    size_t thin_len = build_thin_image(thin, sizeof thin, &entry);
    check(thin_len > 0, "thin image built");

    const char *thin_path = "/tmp/ocerz_test_thin";
    check(write_file(thin_path, thin, thin_len) == 0, "thin image written");

    OcerzImage img;
    check(ocerz_load_image(thin_path, &img) == OCERZ_OK, "thin image loaded");
    verify_loaded(&img, entry, "thin");

    char *argv[] = { (char *)"/tmp/prog", (char *)"a", (char *)"bb" };
    int argc = 3;
    char *envp[] = { (char *)"X=1", NULL };

    OcerzVM vm;
    check(ocerz_vm_init(&vm) == OCERZ_OK, "vm init");
    vm.image = img;
    check(ocerz_setup_stack(&vm, &img, argc, argv, envp) == OCERZ_OK,
          "thin stack built");
    verify_stack(&vm, argc, argv, envp);

    static uint8_t fat[8192];
    size_t fat_len = build_fat_wrapper(fat, sizeof fat, thin, thin_len);
    check(fat_len > 0, "fat image built");

    const char *fat_path = "/tmp/ocerz_test_fat";
    check(write_file(fat_path, fat, fat_len) == 0, "fat image written");

    OcerzImage fimg;
    check(ocerz_load_image(fat_path, &fimg) == OCERZ_OK, "fat image loaded");
    verify_loaded(&fimg, entry, "fat");

    OcerzVM fvm;
    check(ocerz_vm_init(&fvm) == OCERZ_OK, "fat vm init");
    fvm.image = fimg;
    check(ocerz_setup_stack(&fvm, &fimg, argc, argv, envp) == OCERZ_OK,
          "fat stack built");
    verify_stack(&fvm, argc, argv, envp);

    unlink(thin_path);
    unlink(fat_path);

    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
