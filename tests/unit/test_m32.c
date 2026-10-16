/*
 * m32's specials and raw system calls, called directly on a 32-bit cpu over the 4 GB window.  These are the calls
 * the i386 programs in tests/m32 cannot pin on a machine without Xcode 26, whose ld-classic is the last linker
 * that links i386 (tests/m32/build.sh): the raw system calls a self-loading program makes and the dyld m32 hands
 * it (src/m32_selfload.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/mem.h"
#include "ocerz/vm.h"

#define STACK    0x00800000u
#define DATA     0x00900000u
#define RET_ADDR 0x00123456u

static OcerzVM g_vm;
static int g_checks, g_failures;

static void check(const char *what, int ok)
{
    g_checks++;
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        g_failures++;
    }
}

static OcerzCPU *cpu(void) { return &g_vm.cpu; }

/* a raw system call by sysenter: number in eax, arguments above the return address at ecx, back to edx */
static uint32_t sysenter(uint32_t eax, int n, const uint32_t *args)
{
    m32_wr(STACK, 0x0badf00du);
    for (int i = 0; i < n; i++)
        m32_wr(STACK + 4 + 4 * (uint32_t)i, args[i]);
    cpu()->cc_op = OCERZ_CC_NONE;
    cpu()->gpr[OCERZ_RAX] = eax;
    cpu()->gpr[OCERZ_RCX] = STACK;
    cpu()->gpr[OCERZ_RDX] = RET_ADDR;
    cpu()->gpr[OCERZ_RSP] = 0x1234u;
    m32_raw_syscall(&g_vm, cpu(), 1);
    check("sysenter returns to edx with the stack at ecx", cpu()->rip == RET_ADDR && cpu()->gpr[OCERZ_RSP] == STACK);
    return (uint32_t)cpu()->gpr[OCERZ_RAX];
}

static int carry(void) { return (cpu()->rflags & OCERZ_CF) != 0; }

static void put_str(uint32_t g, const char *s) { memcpy(m32_h(g), s, strlen(s) + 1); }

static void test_raw_syscalls(void)
{
    check("getpid", sysenter(20, 0, NULL) == (uint32_t)getpid() && !carry());
    uint32_t mp[3] = { DATA, 0x1000, PROT_READ };
    check("the argument size above the number is ignored", sysenter(74 | 12u << 16, 3, mp) == 0 && !carry());
    check("an unknown call fails with ENOSYS and the carry set", sysenter(9999, 0, NULL) == ENOSYS && carry());
    uint32_t mm[7] = { 0, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0xffffffffu, 0, 0 };
    uint32_t g = sysenter(197, 7, mm);
    check("mmap maps in the window", !carry() && g && ocerz_addr_readable(g) && ocerz_addr_readable(g + 0x3fff));
    put_str(DATA + 0x700, "/usr/lib/dyld");
    uint32_t op[3] = { DATA + 0x700, O_RDONLY, 0 };
    int fd = (int)sysenter(5, 3, op);
    check("opening the dyld gives m32's own", !carry() && fd >= 0);
    uint8_t h[0x200];
    check("... which reads", fd >= 0 && pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h);
    uint32_t w[8];
    memcpy(w, h, sizeof w);
    check("... an i386 dylinker", w[0] == MH_MAGIC && w[1] == CPU_TYPE_I386 && w[3] == MH_DYLINKER);
    uint32_t eip;
    memcpy(&eip, h + 28 + 56 + 16 + 4 * 10, 4);
    check("... whose entry is the dyld trap", eip == M32_TRAP(M32_ID_DYLD));
    if (fd >= 0)
        close(fd);
}

int main(void)
{
    if (ocerz_mem_init(0, M32_WINDOW) != OCERZ_OK ||
        ocerz_map_fixed(STACK - 0x10000, 0x10000 + 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(DATA, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "test_m32: cannot set up the window\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;
    ocerz_cpu_reset(cpu());
    cpu()->mode32 = 1;
    test_raw_syscalls();
    printf("test_m32: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
