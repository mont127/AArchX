#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint8_t g_fxbuf[512] __attribute__((aligned(64)));

#define KERNELS(X) \
    X(punpcklbw, 0x4db30c19e7765dc9ull) \
    X(punpcklbw_m, 0x4db30c19e7765dc9ull) \
    X(punpcklwd, 0x84d7ce91590eb0abull) \
    X(punpcklwd_m, 0x84d7ce91590eb0abull) \
    X(punpckldq, 0xf8490d28a9b286b6ull) \
    X(punpckldq_m, 0xf8490d28a9b286b6ull) \
    X(packsswb, 0x664ed1b6e277e87bull) \
    X(packsswb_m, 0x664ed1b6e277e87bull) \
    X(pcmpgtb, 0x5f9a8b8d0fd32956ull) \
    X(pcmpgtb_m, 0x5f9a8b8d0fd32956ull) \
    X(pcmpgtw, 0x0e498227fe1ce619ull) \
    X(pcmpgtw_m, 0x0e498227fe1ce619ull) \
    X(pcmpgtd, 0x9ec4cb55960e2e69ull) \
    X(pcmpgtd_m, 0x9ec4cb55960e2e69ull) \
    X(packuswb, 0x21285ceb0ba27326ull) \
    X(packuswb_m, 0x21285ceb0ba27326ull) \
    X(punpckhbw, 0xb93f25f372ab9972ull) \
    X(punpckhbw_m, 0xb93f25f372ab9972ull) \
    X(punpckhwd, 0xd3b7825d57e42708ull) \
    X(punpckhwd_m, 0xd3b7825d57e42708ull) \
    X(punpckhdq, 0x5f4486063c503eeaull) \
    X(punpckhdq_m, 0x5f4486063c503eeaull) \
    X(packssdw, 0x7c7544a239eab27dull) \
    X(packssdw_m, 0x7c7544a239eab27dull) \
    X(pcmpeqb, 0x2d86ed2f124b6d81ull) \
    X(pcmpeqb_m, 0x2d86ed2f124b6d81ull) \
    X(pcmpeqw, 0x6669d34c58119353ull) \
    X(pcmpeqw_m, 0x6669d34c58119353ull) \
    X(pcmpeqd, 0x201b27f0a6e948c4ull) \
    X(pcmpeqd_m, 0x201b27f0a6e948c4ull) \
    X(psrlw, 0x10e4008dc3389cb1ull) \
    X(psrlw_m, 0x10e4008dc3389cb1ull) \
    X(psrld, 0x5db427afc357481bull) \
    X(psrld_m, 0x5db427afc357481bull) \
    X(psrlq, 0x52ccffe16dae215cull) \
    X(psrlq_m, 0x52ccffe16dae215cull) \
    X(paddq, 0x06f719c6b08058c1ull) \
    X(paddq_m, 0x06f719c6b08058c1ull) \
    X(pmullw, 0xcd425ddbe724693cull) \
    X(pmullw_m, 0xcd425ddbe724693cull) \
    X(psubusb, 0x171f7c81114ab70cull) \
    X(psubusb_m, 0x171f7c81114ab70cull) \
    X(psubusw, 0x7774efae989333caull) \
    X(psubusw_m, 0x7774efae989333caull) \
    X(pminub, 0x0ca8789f576a6511ull) \
    X(pminub_m, 0x0ca8789f576a6511ull) \
    X(pand, 0x240a0a03d310bfa1ull) \
    X(pand_m, 0x240a0a03d310bfa1ull) \
    X(paddusb, 0xd4a84749bad9aaebull) \
    X(paddusb_m, 0xd4a84749bad9aaebull) \
    X(paddusw, 0x8df1197a29302c1eull) \
    X(paddusw_m, 0x8df1197a29302c1eull) \
    X(pmaxub, 0x1eeed46b56ccae48ull) \
    X(pmaxub_m, 0x1eeed46b56ccae48ull) \
    X(pandn, 0xab4f93151fcaefadull) \
    X(pandn_m, 0xab4f93151fcaefadull) \
    X(pavgb, 0x6bc21d5f71bd4e0eull) \
    X(pavgb_m, 0x6bc21d5f71bd4e0eull) \
    X(psraw, 0xa60d16556b375734ull) \
    X(psraw_m, 0xa60d16556b375734ull) \
    X(psrad, 0x96b4e3489f2b730dull) \
    X(psrad_m, 0x96b4e3489f2b730dull) \
    X(pavgw, 0x7a4ff0af542f5b58ull) \
    X(pavgw_m, 0x7a4ff0af542f5b58ull) \
    X(pmulhuw, 0x0d20c54714b8cfe5ull) \
    X(pmulhuw_m, 0x0d20c54714b8cfe5ull) \
    X(pmulhw, 0xcab00958900db52eull) \
    X(pmulhw_m, 0xcab00958900db52eull) \
    X(psubsb, 0x1524cf7eda40d1abull) \
    X(psubsb_m, 0x1524cf7eda40d1abull) \
    X(psubsw, 0xbd2a50bde277eecaull) \
    X(psubsw_m, 0xbd2a50bde277eecaull) \
    X(pminsw, 0xc59f1ab247c3e948ull) \
    X(pminsw_m, 0xc59f1ab247c3e948ull) \
    X(por, 0x04480b42587d47e6ull) \
    X(por_m, 0x04480b42587d47e6ull) \
    X(paddsb, 0x12a34c118f629029ull) \
    X(paddsb_m, 0x12a34c118f629029ull) \
    X(paddsw, 0xd8f59ad5db1e8bc2ull) \
    X(paddsw_m, 0xd8f59ad5db1e8bc2ull) \
    X(pmaxsw, 0x71a746ce9cab1bcaull) \
    X(pmaxsw_m, 0x71a746ce9cab1bcaull) \
    X(pxor, 0x73b7839f33a3ba40ull) \
    X(pxor_m, 0x73b7839f33a3ba40ull) \
    X(psllw, 0x7e88d0011fa26d2full) \
    X(psllw_m, 0x7e88d0011fa26d2full) \
    X(pslld, 0xf704dc6149ae53efull) \
    X(pslld_m, 0xf704dc6149ae53efull) \
    X(psllq, 0xd0a4f16f0039b3b4ull) \
    X(psllq_m, 0xd0a4f16f0039b3b4ull) \
    X(pmuludq, 0xa7fcb0c73ede85dcull) \
    X(pmuludq_m, 0xa7fcb0c73ede85dcull) \
    X(pmaddwd, 0xcf729e754e9c7ebaull) \
    X(pmaddwd_m, 0xcf729e754e9c7ebaull) \
    X(psadbw, 0xf7d485745378526aull) \
    X(psadbw_m, 0xf7d485745378526aull) \
    X(psubb, 0xa47bee8751a33d32ull) \
    X(psubb_m, 0xa47bee8751a33d32ull) \
    X(psubw, 0xcaf311b5925b4283ull) \
    X(psubw_m, 0xcaf311b5925b4283ull) \
    X(psubd, 0x3df24cd321dd47c9ull) \
    X(psubd_m, 0x3df24cd321dd47c9ull) \
    X(psubq, 0xab279f47c7c4fa53ull) \
    X(psubq_m, 0xab279f47c7c4fa53ull) \
    X(paddb, 0xd1bc29d8120ccee4ull) \
    X(paddb_m, 0xd1bc29d8120ccee4ull) \
    X(paddw, 0x62c0d2eb6c517cb3ull) \
    X(paddw_m, 0x62c0d2eb6c517cb3ull) \
    X(paddd, 0x95a1256764bcd922ull) \
    X(paddd_m, 0x95a1256764bcd922ull) \
    X(pshufb, 0xb2f78c4bf24fabacull) \
    X(pshufb_m, 0xb2f78c4bf24fabacull) \
    X(phaddw, 0x1d2a3d7ab3946271ull) \
    X(phaddw_m, 0x1d2a3d7ab3946271ull) \
    X(phaddd, 0x10751d1370651aefull) \
    X(phaddd_m, 0x10751d1370651aefull) \
    X(phaddsw, 0xcac632709026baf0ull) \
    X(phaddsw_m, 0xcac632709026baf0ull) \
    X(pmaddubsw, 0x0dfdbf8ce73b97cfull) \
    X(pmaddubsw_m, 0x0dfdbf8ce73b97cfull) \
    X(phsubw, 0xf966d5df556e59c4ull) \
    X(phsubw_m, 0xf966d5df556e59c4ull) \
    X(phsubd, 0xa60ed7d8a410683cull) \
    X(phsubd_m, 0xa60ed7d8a410683cull) \
    X(phsubsw, 0xc15b109a030226ceull) \
    X(phsubsw_m, 0xc15b109a030226ceull) \
    X(psignb, 0x0918e24a3e847573ull) \
    X(psignb_m, 0x0918e24a3e847573ull) \
    X(psignw, 0xcf653fa686cd005cull) \
    X(psignw_m, 0xcf653fa686cd005cull) \
    X(psignd, 0xc518e542e80b36c8ull) \
    X(psignd_m, 0xc518e542e80b36c8ull) \
    X(pmulhrsw, 0x3bac7addbc482873ull) \
    X(pmulhrsw_m, 0x3bac7addbc482873ull) \
    X(pabsb, 0xcd7fd80d67828718ull) \
    X(pabsb_m, 0xcd7fd80d67828718ull) \
    X(pabsw, 0x3eb307ffc4cf1200ull) \
    X(pabsw_m, 0x3eb307ffc4cf1200ull) \
    X(pabsd, 0xe989b3075f31e826ull) \
    X(pabsd_m, 0xe989b3075f31e826ull) \
    X(pshufw_1b, 0x6390463136aa0437ull) \
    X(pshufw_a5, 0x504db3207579ba00ull) \
    X(pshufw_00, 0xcc48aab9290a98e9ull) \
    X(pshufw_ff, 0x310dc7fbdc764863ull) \
    X(palignr_00, 0xda894f0aa2483756ull) \
    X(palignr_03, 0xd5b03173dce475d1ull) \
    X(palignr_08, 0xf26a9022f6c4bf0aull) \
    X(palignr_0d, 0x2a1df9a519a07f53ull) \
    X(palignr_10, 0x31b5320f0337db82ull) \
    X(palignr_14, 0x31b5320f0337db82ull) \
    X(pshufw_m4e, 0x5270c42d7b3a31edull) \
    X(palignr_m05, 0x5aba3c182c64a1b4ull) \
    X(psrlw_i3, 0x4c239605b50e5c0eull) \
    X(psrlw_i16, 0x31b5320f0337db82ull) \
    X(psraw_i15, 0xe28bf135430eddbeull) \
    X(psraw_i40, 0xe28bf135430eddbeull) \
    X(psllw_i1, 0x28774f477534e99eull) \
    X(psrld_i7, 0x42cb94e9bb22dcd8ull) \
    X(psrad_i31, 0xf3c9ac8f689db609ull) \
    X(psrad_i0, 0xf26a9022f6c4bf0aull) \
    X(pslld_i32, 0x31b5320f0337db82ull) \
    X(psrlq_i17, 0xbecabf52c21ceecaull) \
    X(psllq_i63, 0x939805a5f98f7749ull) \
    X(psrlq_i64, 0x31b5320f0337db82ull) \
    X(cvtpi2ps, 0xeee2e0e46614d492ull) \
    X(cvtpi2ps_m, 0xeee2e0e46614d492ull) \
    X(cvtpi2pd, 0x49036443a6561496ull) \
    X(cvtpi2pd_m, 0x49036443a6561496ull) \
    X(cvtps2pi, 0xb7e1804bcca87d4dull) \
    X(cvtps2pi_m, 0xb7e1804bcca87d4dull) \
    X(cvttps2pi, 0x6d9e8e36b22a26baull) \
    X(cvttps2pi_m, 0x6d9e8e36b22a26baull) \
    X(cvtpd2pi, 0xe251fa46969d1ca1ull) \
    X(cvtpd2pi_m, 0xe251fa46969d1ca1ull) \
    X(cvttpd2pi, 0x3f8745dad2dc68a3ull) \
    X(cvttpd2pi_m, 0x3f8745dad2dc68a3ull) \
    X(movd_in, 0xceb5d5ce43318832ull) \
    X(movd_out, 0x8e96697ea686cff8ull) \
    X(movq_in, 0x2808274e69ed3890ull) \
    X(movq_out, 0xd2ba9f8f9ca58949ull) \
    X(movd_mem, 0x844d55642f94ab89ull) \
    X(movq_7f, 0x1fd31539502556d2ull) \
    X(movq_store, 0xad4a39699ba5983full) \
    X(rex_ignored, 0x7b9aaf0bca90e9c5ull) \
    X(r9base, 0x93a0ce003c6ca581ull) \
    X(pinsrw, 0xa120bb5a7740fbd7ull) \
    X(pextrw, 0x1da1bddc0657e3d1ull) \
    X(pmovmskb, 0x268c13b331a14523ull) \
    X(movq2dq, 0x3f5ed7e786fa011aull) \
    X(movdq2q, 0xda894f0aa2483756ull) \
    X(flags, 0x6e0ae4550d563fdbull) \
    X(blend, 0x9a40629a41562bd8ull) \
    X(loop, 0xcd55f407ec40c753ull)

#define DECLARE(n, h) void k_##n(uint64_t *io);
KERNELS(DECLARE)
void k_x87tag(uint64_t *io);

struct kernel {
    const char *name;
    void (*fn)(uint64_t *io);
    uint64_t want;
};

#define ENTRY(n, h) { #n, k_##n, h },
static const struct kernel kernels[] = {
    KERNELS(ENTRY)
};

static const uint64_t edge[] = {
    0x0000000000000000ull, 0xffffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull,
    0x0123456789abcdefull, 0xfedcba9876543210ull, 0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full,
    0x00ff00ff00ff00ffull, 0xff00ff00ff00ff00ull, 0x8000800080008000ull, 0x7fff7fff7fff7fffull,
    0x8000000080000000ull, 0x7fffffff7fffffffull, 0x0000000000000003ull, 0x000000000000000full,
    0x0000000000000010ull, 0x000000000000001full, 0x0000000000000020ull, 0x000000000000003full,
    0x0000000000000040ull, 0x0102030405060708ull, 0x8182838485868788ull, 0x0f0e0d0c0b0a0908ull,
    0xc02000003fc00000ull, 0x402000004f000000ull, 0xcf0000007fc00000ull, 0x3f000000bf000000ull,
    0x40600000501502f9ull, 0x3ff8000000000000ull, 0xc004000000000000ull, 0x41dfffffffe00000ull,
    0x7ff8000000000000ull, 0x400c000000000000ull, 0xc1e0000000100000ull, 0x00000000ffff8000ull,
};

static uint64_t rng_state;

static uint64_t next_rand(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dull;
}

static uint64_t fold(uint64_t h, const uint64_t *io)
{
    for (int i = 0; i < 4; i++) {
        h = (h ^ io[i]) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

static uint64_t run_kernel(void (*fn)(uint64_t *io))
{
    const size_t n = sizeof edge / sizeof edge[0];
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t io[4];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            io[0] = edge[i];
            io[1] = edge[j];
            io[2] = edge[(i * 7 + j * 3) % n];
            io[3] = edge[(i + j) % n] ^ 0x5a5a5a5a5a5a5a5aull;
            fn(io);
            h = fold(h, io);
        }
    }
    rng_state = 0x9e3779b97f4a7c15ull;
    for (int r = 0; r < 256; r++) {
        for (int k = 0; k < 4; k++)
            io[k] = next_rand();
        fn(io);
        h = fold(h, io);
    }
    return h;
}

int main(int argc, char **argv)
{
    int golden = argc > 1 && strcmp(argv[1], "golden") == 0;
    int bad = 0;
    for (size_t k = 0; k < sizeof kernels / sizeof kernels[0]; k++) {
        uint64_t h = run_kernel(kernels[k].fn);
        if (golden) {
            printf("%s 0x%016llx\n", kernels[k].name, (unsigned long long)h);
        } else if (h != kernels[k].want) {
            printf("BAD %s got 0x%016llx want 0x%016llx\n", kernels[k].name, (unsigned long long)h,
                   (unsigned long long)kernels[k].want);
            bad = 1;
        }
    }
    if (!golden) {
        uint64_t io[4] = { 0x0123456789abcdefull, 0, 0, 0 };
        k_x87tag(io);
        if (io[3] != 0x000000ff00003000ull) {
            printf("BAD x87tag got 0x%016llx want 0x000000ff00003000\n", (unsigned long long)io[3]);
            bad = 1;
        }
    }
    if (!golden && !bad)
        printf("OK\n");
    return bad;
}
