/* x87 control/state instructions: TOP tracking, FNSTCW/FLDCW, FNSTENV/FLDENV,
 * FNSAVE/FRSTOR, FXSAVE/FXRSTOR, FFREE/FINCSTP/FDECSTP, FNINIT and FNCLEX.
 *
 * Masking: Rosetta writes zeros into the FIP/FCS/FOP/FDP/FDS fields of
 * FNSTENV and FNSAVE (verified identical on every run), so those are printed
 * raw.  FXSAVE stores the real instruction and data pointers (bytes 8..23),
 * which change with ASLR, so those 16 bytes are masked to "xx".  The
 * reserved bytes after each ST image in FXSAVE are not printed.  The sticky
 * exception bits of MXCSR (low 6 bits) depend on earlier SSE code and are
 * masked.  The contents of EMPTY registers (FNSAVE/FXSAVE images) are stale
 * data from earlier code and are printed as "(empty)". */
#include "x87_common.h"

typedef struct { uint64_t m; uint16_t se; } f80;

static void pr_env(const char *label, const uint32_t *e)
{
    printf("%s: fcw=%04x fsw=%04x ftw=%04x fip=%08x fcs/op=%08x fdp=%08x fds=%08x (pads %04x %04x %04x)\n", label,
           e[0] & 0xffff, e[1] & 0xffff, e[2] & 0xffff, e[3], e[4], e[5], e[6], e[0] >> 16, e[1] >> 16, e[2] >> 16);
}
static void pr_bytes(const char *label, const uint8_t *p, int n)
{
    printf("%s:", label);
    for (int k = 0; k < n; k++) printf("%s%02x", k % 4 == 0 ? " " : "", p[k]);
    printf("\n");
}

/* ST(k) images of an FNSAVE (img + 28) or FXSAVE (fx + 32, stride 16) area; empty registers are not printed. */
static void pr_sts(const char *label, int top, unsigned tagbits, int abridged, const uint8_t *st, int stride)
{
    for (int k = 0; k < 8; k++) {
        int phys = (top + k) & 7;
        int empty = abridged ? !((tagbits >> phys) & 1) : ((tagbits >> (2 * phys)) & 3) == 3;
        char l[64];
        snprintf(l, sizeof l, "%s st(%d)", label, k);
        if (empty) printf("%s: (empty)\n", l); else pr_bytes(l, st + stride * k, 10);
    }
}
static void pr_save(const char *label, const uint8_t *img)
{
    char l[64];
    snprintf(l, sizeof l, "%s env", label);
    pr_bytes(l, img, 28);
    unsigned fsw = img[4] | img[5] << 8, ftw = img[8] | img[9] << 8;
    pr_sts(label, (fsw >> 11) & 7, ftw, 0, img + 28, 10);
}

static void top_tracking(void)
{
    uint16_t sw[17]; double one = 1.0;
    __asm__ volatile("fninit\n\tfnstsw 0(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 2(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 4(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 6(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 8(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 10(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 12(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 14(%[p])\n\t"
                     "fldl %[o]\n\tfnstsw 16(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 18(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 20(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 22(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 24(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 26(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 28(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 30(%[p])\n\t"
                     "fstp %%st(0)\n\tfnstsw 32(%[p])"
                     :: [p] "r"(sw), [o] "m"(one) : X87CLOB);
    printf("top after fninit: %u\n", (sw[0] >> 11) & 7);
    for (int k = 1; k <= 8; k++) printf("top after push %d: %u fsw=%04x\n", k, (sw[k] >> 11) & 7, sw[k]);
    for (int k = 9; k <= 16; k++) printf("top after pop %d: %u fsw=%04x\n", k - 8, (sw[k] >> 11) & 7, sw[k]);
}

static void control_word(void)
{
    static const uint16_t cws[] = { 0x037f, 0x027f, 0x0c7f, 0x0f7f, 0x0000, 0x003f, 0x0340, 0x0b3f, 0x077f, 0x1f7f };
    uint16_t back[sizeof cws / sizeof *cws];
    for (unsigned k = 0; k < sizeof cws / sizeof *cws; k++)
        __asm__ volatile("fldcw %1\n\tfnstcw %0" : "=m"(back[k]) : "m"(cws[k]) : "memory");
    x87_init(); /* back to the default word before printing */
    for (unsigned k = 0; k < sizeof cws / sizeof *cws; k++) printf("fldcw %04x -> fnstcw %04x\n", cws[k], back[k]);
    printf("after fninit cw=%04x\n", x87_cw());
}

#define MIXED_SETUP \
    "fninit\n\tfldl %[n]\n\tfldl %[z]\n\tfldl %[i]\n\tfldl %[q]\n\tfldt %[d]\n\t"
static const f80 DEN80 = { 0x0000000000000001ull, 0x0000 }; /* 80-bit denormal */

static void env_tests(void)
{
    uint32_t e[7]; double n = 1.5, z = 0.0, inf = INFINITY, q = __builtin_nan("0x1"), m1 = -1.0, one = 1.0, three = 3.0;

    __asm__ volatile("fninit\n\tfnstenv %0" : "=m"(e) :: X87CLOB);
    pr_env("env after fninit", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(n) : X87CLOB);
    pr_env("env, 3 pushed (top=5)", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(n) : X87CLOB);
    pr_env("env, 1 pushed (top=7)", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfnstenv %0\n\tfninit"
                     : "=m"(e) : "m"(n) : X87CLOB);
    pr_env("env, 8 pushed (top=0)", e);
    /* tag classes: valid / zero / special */
    __asm__ volatile(MIXED_SETUP "fnstenv %[e]\n\tfninit" : [e] "=m"(e) : [n] "m"(n), [z] "m"(z), [i] "m"(inf), [q] "m"(q), [d] "m"(DEN80) : X87CLOB);
    pr_env("env, normal/zero/inf/nan/denormal (top=3)", e);
    /* exception flags in FSW */
    __asm__ volatile("fninit\n\tfldl %1\n\tfsqrt\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(m1) : X87CLOB);
    pr_env("env after sqrt(-1)", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfdivl %2\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(one), "m"(z) : X87CLOB);
    pr_env("env after 1/0", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfdivl %2\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(one), "m"(three) : X87CLOB);
    e[1] &= ~0x200u; /* C1 says which way 1/3 was rounded, which depends on 64- or 53-bit precision */
    pr_env("env after 1/3", e);
    __asm__ volatile("fninit\n\tfldl %2\n\tfldl %1\n\tfcompp\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(one), "m"(n) : X87CLOB);
    pr_env("env after fcompp (st0 < st1, C0 set)", e);

    /* FNSTENV masks all exceptions afterwards (SDM: fcw after = image | 0x3f; Rosetta leaves it unchanged) */
    static const uint16_t cws[] = { 0x0300, 0x0000, 0x0c00, 0x0340, 0x037f };
    for (unsigned k = 0; k < sizeof cws / sizeof *cws; k++) {
        uint16_t after;
        __asm__ volatile("fninit\n\tfldcw %2\n\tfnstenv %0\n\tfnstcw %1" : "=m"(e), "=m"(after) : "m"(cws[k]) : X87CLOB);
        x87_init();
        printf("fcw=%04x: image fcw=%04x, fcw after fnstenv=%04x\n", cws[k], e[0] & 0xffff, after);
    }

    /* FLDENV round trip */
    uint32_t e1[7], e2[7]; uint16_t sw, cw;
    __asm__ volatile(MIXED_SETUP "fnstenv %[e]\n\tfninit\n\tfldenv %[e]\n\tfnstsw %[s]\n\tfnstcw %[c]\n\tfnstenv %[f]\n\tfninit"
                     : [e] "=m"(e1), [s] "=m"(sw), [c] "=m"(cw), [f] "=m"(e2)
                     : [n] "m"(n), [z] "m"(z), [i] "m"(inf), [q] "m"(q), [d] "m"(DEN80) : X87CLOB);
    pr_env("saved ", e1);
    pr_env("reload", e2);
    printf("fldenv round trip: images %s, fsw=%04x top=%u fcw=%04x\n", memcmp(e1, e2, 28) ? "DIFFER" : "equal", sw, (sw >> 11) & 7, cw);
    /* hand-built environment images */
    static const uint32_t custom[][3] = {
        { 0x0c7f, 0x1800, 0xffff }, /* RC=up-ish control word, top=3, all empty */
        { 0x037f, 0x4700, 0xffff }, /* C3 C2 C1 C0 set, top 0 */
        { 0x027f, 0x3800, 0xffff }, /* top=7 */
        { 0x037f, 0x0000, 0xfffc }, /* register 0 tagged valid */
        { 0x0f7f, 0x2000, 0x03ff }, /* top=4, registers 5..7 valid */
    };
    for (unsigned k = 0; k < sizeof custom / sizeof *custom; k++) {
        uint32_t in[7] = { custom[k][0], custom[k][1], custom[k][2], 0, 0, 0, 0 }, out[7]; uint16_t s2, c2;
        __asm__ volatile("fninit\n\tfldenv %3\n\tfnstsw %0\n\tfnstcw %1\n\tfnstenv %2\n\tfninit" : "=m"(s2), "=m"(c2), "=m"(out) : "m"(in) : X87CLOB);
        printf("custom fldenv fcw=%04x fsw=%04x ftw=%04x -> fnstsw=%04x fnstcw=%04x ", in[0], in[1], in[2], s2, c2);
        /* Not ftw: Rosetta echoes the loaded tag word, where hardware rebuilds it from the registers. */
        printf("env fcw=%04x fsw=%04x\n", out[0] & 0xffff, out[1] & 0xffff);
    }
    /* after fldenv with top=3 a push goes to top=2 */
    { uint32_t in[7] = { 0x037f, 0x1800, 0xffff, 0, 0, 0, 0 }; uint16_t s2;
      __asm__ volatile("fninit\n\tfldenv %1\n\tfldl %2\n\tfnstsw %0\n\tfninit" : "=m"(s2) : "m"(in), "m"(n) : X87CLOB);
      printf("push after fldenv(top=3): top=%u\n", (s2 >> 11) & 7); }
}

static void save_tests(void)
{
    uint8_t img[108]; uint16_t sw, cw, sw3; uint32_t e[7];
    double a = 1.5, b = -2.0, c = 4.25, junk = 99.0, r[4];
    /* FNSAVE with TOP != 0 */
    __asm__ volatile("fninit\n\tfldl %[a]\n\tfldl %[b]\n\tfldl %[c]\n\tfnsave %[img]\n\tfnstsw %[s]\n\tfnstcw %[w]\n\tfnstenv %[e]"
                     : [img] "=m"(img), [s] "=m"(sw), [w] "=m"(cw), [e] "=m"(e) : [a] "m"(a), [b] "m"(b), [c] "m"(c) : X87CLOB);
    pr_save("fnsave top=5", img);
    printf("after fnsave: fsw=%04x fcw=%04x\n", sw, cw);
    pr_env("env after fnsave", e);
    /* FRSTOR over a different state, then read back */
    __asm__ volatile("fninit\n\tfldl %[j]\n\tfldl %[j]\n\tfrstor %[img]\n\tfnstsw %[s]\n\tfnstcw %[w]\n\t"
                     "fstpl %[r0]\n\tfstpl %[r1]\n\tfstpl %[r2]\n\tfnstsw %[s2]\n\tfnstenv %[e]"
                     : [s] "=m"(sw), [w] "=m"(cw), [r0] "=m"(r[0]), [r1] "=m"(r[1]), [r2] "=m"(r[2]), [s2] "=m"(sw3), [e] "=m"(e)
                     : [j] "m"(junk), [img] "m"(img) : X87CLOB);
    printf("frstor: fsw=%04x top=%u fcw=%04x st0=%g st1=%g st2=%g fsw after pops=%04x\n", sw, (sw >> 11) & 7, cw, r[0], r[1], r[2],
           sw3);
    pr_env("env after frstor+3 pops", e);
    /* mixed tag state, saved with FNSAVE */
    double n = 1.5, z = 0.0, inf = INFINITY, q = __builtin_nan("0x1");
    __asm__ volatile(MIXED_SETUP "fnsave %[img]" : [img] "=m"(img) : [n] "m"(n), [z] "m"(z), [i] "m"(inf), [q] "m"(q), [d] "m"(DEN80) : X87CLOB);
    pr_save("fnsave mixed", img);
    /* the 8-deep stack and an empty stack */
    double v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    __asm__ volatile("fninit\n\tfldl 0(%1)\n\tfldl 8(%1)\n\tfldl 16(%1)\n\tfldl 24(%1)\n\tfldl 32(%1)\n\tfldl 40(%1)\n\tfldl 48(%1)\n\tfldl 56(%1)\n\tfnsave %0"
                     : "=m"(img) : "r"(v) : X87CLOB);
    pr_save("fnsave full", img);
    __asm__ volatile("fninit\n\tfnsave %0" : "=m"(img) :: X87CLOB);
    pr_save("fnsave empty", img);
    /* frstor of the full image restores all eight */
    /* an empty-stack image restores an empty stack, so refill from the full image first */
    __asm__ volatile("fninit\n\tfldl 0(%1)\n\tfldl 8(%1)\n\tfldl 16(%1)\n\tfldl 24(%1)\n\tfldl 32(%1)\n\tfldl 40(%1)\n\tfldl 48(%1)\n\tfldl 56(%1)\n\tfnsave %0"
                     : "=m"(img) : "r"(v) : X87CLOB);
    __asm__ volatile("fninit\n\tfrstor %8\n\tfstpl %0\n\tfstpl %1\n\tfstpl %2\n\tfstpl %3\n\tfstpl %4\n\tfstpl %5\n\tfstpl %6\n\tfstpl %7"
                     : "=m"(v[0]), "=m"(v[1]), "=m"(v[2]), "=m"(v[3]), "=m"(v[4]), "=m"(v[5]), "=m"(v[6]), "=m"(v[7]) : "m"(img) : X87CLOB);
    printf("frstor full: %g %g %g %g %g %g %g %g\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

static void fx_print(const char *label, const uint8_t *p)
{
    printf("%s: fcw=%02x%02x fsw=%02x%02x ftw8=%02x rsv=%02x fop=%02x%02x ptrs=masked mxcsr=%02x%02x%02x%02x(sticky bits masked) mxcsr_mask=%02x%02x%02x%02x\n", label,
           p[1], p[0], p[3], p[2], p[4], p[5], p[7], p[6], p[27], p[26], p[25], p[24] & 0xc0, p[31], p[30], p[29], p[28]);
    pr_sts(label, ((p[2] | p[3] << 8) >> 11) & 7, p[4], 1, p + 32, 16);
}

static void fxsave_tests(void)
{
    static uint8_t fx[512] __attribute__((aligned(16))), fy[512] __attribute__((aligned(16)));
    double a = 1.5, b = -2.0, c = 4.25, junk = 99.0, r[4]; uint16_t sw;
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %2\n\tfldl %3\n\tfxsave %0\n\tfninit" : "=m"(fx) : "m"(a), "m"(b), "m"(c) : X87CLOB);
    fx_print("fxsave top=5", fx);
    /* FXRSTOR round trip */
    __asm__ volatile("fninit\n\tfldl %[j]\n\tfxrstor %[fx]\n\tfnstsw %[s]\n\tfxsave %[fy]\n\tfstpl %[r0]\n\tfstpl %[r1]\n\tfstpl %[r2]\n\tfnstsw %[s2]"
                     : [s] "=m"(sw), [fy] "=m"(fy), [r0] "=m"(r[0]), [r1] "=m"(r[1]), [r2] "=m"(r[2]), [s2] "=m"(r[3])
                     : [j] "m"(junk), [fx] "m"(fx) : X87CLOB);
    printf("fxrstor: fsw=%04x top=%u st0=%g st1=%g st2=%g\n", sw, (sw >> 11) & 7, r[0], r[1], r[2]);
    printf("fxsave after fxrstor: fcw/fsw/ftw/ST images %s\n",
           (memcmp(fx, fy, 6) || memcmp(fx + 32, fy + 32, 128)) ? "DIFFER" : "equal");
    /* mixed tags: normal, zero, inf, qnan, 80-bit denormal; abridged tag byte has 1 bits for non-empty */
    double n = 1.5, z = 0.0, inf = INFINITY, q = __builtin_nan("0x1");
    __asm__ volatile(MIXED_SETUP "fxsave %[fx]\n\tfninit" : [fx] "=m"(fx) : [n] "m"(n), [z] "m"(z), [i] "m"(inf), [q] "m"(q), [d] "m"(DEN80) : X87CLOB);
    fx_print("fxsave mixed", fx);
    /* an empty stack and a full stack */
    __asm__ volatile("fninit\n\tfxsave %0" : "=m"(fx) :: X87CLOB);
    fx_print("fxsave empty", fx);
    double v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    __asm__ volatile("fninit\n\tfldl 0(%1)\n\tfldl 8(%1)\n\tfldl 16(%1)\n\tfldl 24(%1)\n\tfldl 32(%1)\n\tfldl 40(%1)\n\tfldl 48(%1)\n\tfldl 56(%1)\n\tfxsave %0\n\tfninit"
                     : "=m"(fx) : "r"(v) : X87CLOB);
    fx_print("fxsave full", fx);
    /* FXRSTOR of an image whose FCW was edited (RC = down, 64-bit precision): the stack must come back too */
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %2\n\tfldl %3\n\tfxsave %0\n\tfninit" : "=m"(fy) : "m"(a), "m"(b), "m"(c) : X87CLOB);
    fy[0] = 0x7f; fy[1] = 0x07;
    uint16_t cw; uint32_t e[7];
    __asm__ volatile("fninit\n\tfxrstor %5\n\tfnstsw %0\n\tfnstcw %1\n\tfnstenv %2\n\tfstpl %3\n\tfstpl %4\n\tfninit"
                     : "=m"(sw), "=m"(cw), "=m"(e), "=m"(r[0]), "=m"(r[1]) : "m"(fy) : X87CLOB);
    printf("fxrstor custom fcw=077f: fsw=%04x fcw=%04x st0=%g st1=%g\n", sw, cw, r[0], r[1]);
    e[2] = 0; /* Rosetta's tag word here calls the three registers it just restored empty or zero */
    pr_env("env", e);
}

#define ENVCAP(k) "fnstenv " #k "*28(%[e])\n\t"
static void stack_ops(void)
{
    uint32_t e[8][7]; double v[3] = { 1.0, 2.0, 3.0 };
    __asm__ volatile("fninit\n\tfldl 0(%[v])\n\tfldl 8(%[v])\n\tfldl 16(%[v])\n\t" ENVCAP(0)
                     "ffree %%st(1)\n\t" ENVCAP(1)
                     "ffree %%st(0)\n\t" ENVCAP(2)
                     "fincstp\n\t" ENVCAP(3)
                     "fincstp\n\t" ENVCAP(4)
                     "fdecstp\n\t" ENVCAP(5)
                     "fdecstp\n\t" ENVCAP(6)
                     "fdecstp\n\t" ENVCAP(7)
                     "fninit"
                     :: [e] "r"(e), [v] "r"(v) : X87CLOB);
    static const char *nm[] = { "3 pushed", "ffree st(1)", "ffree st(0)", "fincstp", "fincstp", "fdecstp", "fdecstp", "fdecstp" };
    for (int k = 0; k < 8; k++) printf("%-12s fsw=%04x top=%u ftw=%04x\n", nm[k], e[k][1] & 0xffff, (e[k][1] >> 11) & 7, e[k][2] & 0xffff);
    /* ffree on every register, then re-push: tag word follows */
    __asm__ volatile("fninit\n\tfldl 0(%[v])\n\tfldl 8(%[v])\n\tfldl 16(%[v])\n\t"
                     "ffree %%st(2)\n\t" ENVCAP(0)
                     "ffree %%st(1)\n\t" ENVCAP(1)
                     "ffree %%st(0)\n\t" ENVCAP(2)
                     "fldl 0(%[v])\n\t" ENVCAP(3)
                     "fninit\n\tfldl 0(%[v])\n\tfincstp\n\tfincstp\n\tfincstp\n\tfincstp\n\tfincstp\n\tfincstp\n\tfincstp\n\tfincstp\n\t" ENVCAP(4)
                     "fdecstp\n\tfdecstp\n\t" ENVCAP(5)
                     "fninit\n\tfdecstp\n\t" ENVCAP(6)
                     "fldl 0(%[v])\n\tfincstp\n\t" ENVCAP(7)
                     "fninit"
                     :: [e] "r"(e), [v] "r"(v) : X87CLOB);
    static const char *nm2[] = { "ffree st(2)", "ffree st(1)", "ffree st(0)", "push again", "8x fincstp", "2x fdecstp", "fdecstp (empty)", "push+fincstp" };
    for (int k = 0; k < 8; k++) printf("%-16s fsw=%04x top=%u ftw=%04x\n", nm2[k], e[k][1] & 0xffff, (e[k][1] >> 11) & 7, e[k][2] & 0xffff);
}

static void init_clex(void)
{
    uint32_t e[7]; uint16_t sw, cw; double one = 1.0, z = 0.0, m1 = -1.0, three = 3.0;
    /* FNINIT after dirtying everything */
    __asm__ volatile("fninit\n\tfldcw %4\n\tfldl %5\n\tfldl %6\n\tfdivl %7\n\tfsqrt\n\tfldl %5\n\tfcompp\n\tfldl %5\n\t"
                     "fnstsw %0\n\tfnstcw %1\n\tfninit\n\tfnstsw %2\n\tfnstcw %3"
                     : "=m"(sw), "=m"(cw), "=m"(e[0]), "=m"(e[1]) : "m"(*(const uint16_t[]){ 0x0c7f }), "m"(one), "m"(m1), "m"(z) : X87CLOB);
    printf("dirty before fninit: fsw=%04x fcw=%04x; after fninit: fsw=%04x fcw=%04x\n", sw, cw, e[0] & 0xffff, e[1] & 0xffff);
    __asm__ volatile("fninit\n\tfldcw %1\n\tfldl %2\n\tfsqrt\n\tfninit\n\tfnstenv %0" : "=m"(e) : "m"(*(const uint16_t[]){ 0x0c7f }), "m"(m1) : X87CLOB);
    pr_env("env after dirty fninit", e);
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %1\n\tfldl %1\n\tfninit\n\tfldl %1\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(one) : X87CLOB);
    printf("push after fninit: top=%u\n", (sw >> 11) & 7);
    /* FNCLEX: clears exception flags (bits 0-7) and B (bit 15), keeps TOP and C0-C3 */
    uint16_t s0, s1, s2, s3;
    __asm__ volatile("fninit\n\tfldl %[o]\n\tfdivl %[z]\n\tfldl %[m]\n\tfsqrt\n\t.byte 0xd8, 0xd1\n\t" /* fcom st(1) */
                     "fnstsw %[s0]\n\tfnclex\n\tfnstsw %[s1]\n\tfnclex\n\tfnstsw %[s2]\n\tfldl %[o]\n\tfdivl %[three]\n\tfnstsw %[s3]\n\tfnclex\n\tfninit"
                     : [s0] "=m"(s0), [s1] "=m"(s1), [s2] "=m"(s2), [s3] "=m"(s3)
                     : [o] "m"(one), [z] "m"(z), [m] "m"(m1), [three] "m"(three) : X87CLOB);
    s3 &= ~0x200; /* C1, as above */
    printf("fnclex: before fsw=%04x after fsw=%04x again fsw=%04x; 1/3 sets fsw=%04x\n", s0, s1, s2, s3);
    __asm__ volatile("fninit\n\tfldl %1\n\tfsqrt\n\tfnclex\n\tfnstenv %0\n\tfninit" : "=m"(e) : "m"(m1) : X87CLOB);
    pr_env("env after sqrt(-1)+fnclex", e);
}

int main(void)
{
    x87_init();
    top_tracking();
    x87_group_end("top");
    control_word();
    x87_group_end("cw");
    env_tests();
    x87_group_end("env");
    save_tests();
    x87_group_end("save");
    fxsave_tests();
    x87_group_end("fxsave");
    stack_ops();
    x87_group_end("stack-ops");
    init_clex();
    x87_group_end("init-clex");
    return 0;
}
