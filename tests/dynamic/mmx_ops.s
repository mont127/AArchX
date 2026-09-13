    .text

.macro RR name, op
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %mm2
    movq 8(%rdi), %mm5
    \op %mm5, %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

.macro RM name, op
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %mm2
    \op 8(%rdi), %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

.macro RRI name, op, imm
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %mm2
    movq 8(%rdi), %mm5
    \op $\imm, %mm5, %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

.macro RMI name, op, imm
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %mm2
    \op $\imm, 8(%rdi), %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

.macro SI name, op, imm
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %mm2
    \op $\imm, %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

.macro TOXMM name, op, src
    .globl _\name
    .p2align 4
_\name:
    movq (%rdi), %xmm3
    movhps 16(%rdi), %xmm3
    movq 8(%rdi), %mm5
    \op \src, %xmm3
    movq %xmm3, (%rdi)
    movhps %xmm3, 16(%rdi)
    emms
    ret
.endm

.macro TOMMX name, op, src
    .globl _\name
    .p2align 4
_\name:
    movq 8(%rdi), %xmm3
    movhps 16(%rdi), %xmm3
    movq (%rdi), %mm2
    \op \src, %mm2
    movq %mm2, (%rdi)
    emms
    ret
.endm

    RR k_punpcklbw, punpcklbw
    RM k_punpcklbw_m, punpcklbw
    RR k_punpcklwd, punpcklwd
    RM k_punpcklwd_m, punpcklwd
    RR k_punpckldq, punpckldq
    RM k_punpckldq_m, punpckldq
    RR k_packsswb, packsswb
    RM k_packsswb_m, packsswb
    RR k_pcmpgtb, pcmpgtb
    RM k_pcmpgtb_m, pcmpgtb
    RR k_pcmpgtw, pcmpgtw
    RM k_pcmpgtw_m, pcmpgtw
    RR k_pcmpgtd, pcmpgtd
    RM k_pcmpgtd_m, pcmpgtd
    RR k_packuswb, packuswb
    RM k_packuswb_m, packuswb
    RR k_punpckhbw, punpckhbw
    RM k_punpckhbw_m, punpckhbw
    RR k_punpckhwd, punpckhwd
    RM k_punpckhwd_m, punpckhwd
    RR k_punpckhdq, punpckhdq
    RM k_punpckhdq_m, punpckhdq
    RR k_packssdw, packssdw
    RM k_packssdw_m, packssdw
    RR k_pcmpeqb, pcmpeqb
    RM k_pcmpeqb_m, pcmpeqb
    RR k_pcmpeqw, pcmpeqw
    RM k_pcmpeqw_m, pcmpeqw
    RR k_pcmpeqd, pcmpeqd
    RM k_pcmpeqd_m, pcmpeqd
    RR k_psrlw, psrlw
    RM k_psrlw_m, psrlw
    RR k_psrld, psrld
    RM k_psrld_m, psrld
    RR k_psrlq, psrlq
    RM k_psrlq_m, psrlq
    RR k_paddq, paddq
    RM k_paddq_m, paddq
    RR k_pmullw, pmullw
    RM k_pmullw_m, pmullw
    RR k_psubusb, psubusb
    RM k_psubusb_m, psubusb
    RR k_psubusw, psubusw
    RM k_psubusw_m, psubusw
    RR k_pminub, pminub
    RM k_pminub_m, pminub
    RR k_pand, pand
    RM k_pand_m, pand
    RR k_paddusb, paddusb
    RM k_paddusb_m, paddusb
    RR k_paddusw, paddusw
    RM k_paddusw_m, paddusw
    RR k_pmaxub, pmaxub
    RM k_pmaxub_m, pmaxub
    RR k_pandn, pandn
    RM k_pandn_m, pandn
    RR k_pavgb, pavgb
    RM k_pavgb_m, pavgb
    RR k_psraw, psraw
    RM k_psraw_m, psraw
    RR k_psrad, psrad
    RM k_psrad_m, psrad
    RR k_pavgw, pavgw
    RM k_pavgw_m, pavgw
    RR k_pmulhuw, pmulhuw
    RM k_pmulhuw_m, pmulhuw
    RR k_pmulhw, pmulhw
    RM k_pmulhw_m, pmulhw
    RR k_psubsb, psubsb
    RM k_psubsb_m, psubsb
    RR k_psubsw, psubsw
    RM k_psubsw_m, psubsw
    RR k_pminsw, pminsw
    RM k_pminsw_m, pminsw
    RR k_por, por
    RM k_por_m, por
    RR k_paddsb, paddsb
    RM k_paddsb_m, paddsb
    RR k_paddsw, paddsw
    RM k_paddsw_m, paddsw
    RR k_pmaxsw, pmaxsw
    RM k_pmaxsw_m, pmaxsw
    RR k_pxor, pxor
    RM k_pxor_m, pxor
    RR k_psllw, psllw
    RM k_psllw_m, psllw
    RR k_pslld, pslld
    RM k_pslld_m, pslld
    RR k_psllq, psllq
    RM k_psllq_m, psllq
    RR k_pmuludq, pmuludq
    RM k_pmuludq_m, pmuludq
    RR k_pmaddwd, pmaddwd
    RM k_pmaddwd_m, pmaddwd
    RR k_psadbw, psadbw
    RM k_psadbw_m, psadbw
    RR k_psubb, psubb
    RM k_psubb_m, psubb
    RR k_psubw, psubw
    RM k_psubw_m, psubw
    RR k_psubd, psubd
    RM k_psubd_m, psubd
    RR k_psubq, psubq
    RM k_psubq_m, psubq
    RR k_paddb, paddb
    RM k_paddb_m, paddb
    RR k_paddw, paddw
    RM k_paddw_m, paddw
    RR k_paddd, paddd
    RM k_paddd_m, paddd
    RR k_pshufb, pshufb
    RM k_pshufb_m, pshufb
    RR k_phaddw, phaddw
    RM k_phaddw_m, phaddw
    RR k_phaddd, phaddd
    RM k_phaddd_m, phaddd
    RR k_phaddsw, phaddsw
    RM k_phaddsw_m, phaddsw
    RR k_pmaddubsw, pmaddubsw
    RM k_pmaddubsw_m, pmaddubsw
    RR k_phsubw, phsubw
    RM k_phsubw_m, phsubw
    RR k_phsubd, phsubd
    RM k_phsubd_m, phsubd
    RR k_phsubsw, phsubsw
    RM k_phsubsw_m, phsubsw
    RR k_psignb, psignb
    RM k_psignb_m, psignb
    RR k_psignw, psignw
    RM k_psignw_m, psignw
    RR k_psignd, psignd
    RM k_psignd_m, psignd
    RR k_pmulhrsw, pmulhrsw
    RM k_pmulhrsw_m, pmulhrsw
    RR k_pabsb, pabsb
    RM k_pabsb_m, pabsb
    RR k_pabsw, pabsw
    RM k_pabsw_m, pabsw
    RR k_pabsd, pabsd
    RM k_pabsd_m, pabsd

    RRI k_pshufw_1b, pshufw, 0x1b
    RRI k_pshufw_a5, pshufw, 0xa5
    RRI k_pshufw_00, pshufw, 0x00
    RRI k_pshufw_ff, pshufw, 0xff
    RRI k_palignr_00, palignr, 0x00
    RRI k_palignr_03, palignr, 0x03
    RRI k_palignr_08, palignr, 0x08
    RRI k_palignr_0d, palignr, 0x0d
    RRI k_palignr_10, palignr, 0x10
    RRI k_palignr_14, palignr, 0x14
    RMI k_pshufw_m4e, pshufw, 0x4e
    RMI k_palignr_m05, palignr, 0x05
    SI k_psrlw_i3, psrlw, 3
    SI k_psrlw_i16, psrlw, 16
    SI k_psraw_i15, psraw, 15
    SI k_psraw_i40, psraw, 40
    SI k_psllw_i1, psllw, 1
    SI k_psrld_i7, psrld, 7
    SI k_psrad_i31, psrad, 31
    SI k_psrad_i0, psrad, 0
    SI k_pslld_i32, pslld, 32
    SI k_psrlq_i17, psrlq, 17
    SI k_psllq_i63, psllq, 63
    SI k_psrlq_i64, psrlq, 64

    TOXMM k_cvtpi2ps, cvtpi2ps, %mm5
    TOXMM k_cvtpi2ps_m, cvtpi2ps, 8(%rdi)
    TOXMM k_cvtpi2pd, cvtpi2pd, %mm5
    TOXMM k_cvtpi2pd_m, cvtpi2pd, 8(%rdi)
    TOMMX k_cvtps2pi, cvtps2pi, %xmm3
    TOMMX k_cvtps2pi_m, cvtps2pi, 8(%rdi)
    TOMMX k_cvttps2pi, cvttps2pi, %xmm3
    TOMMX k_cvttps2pi_m, cvttps2pi, 8(%rdi)
    TOMMX k_cvtpd2pi, cvtpd2pi, %xmm3
    TOMMX k_cvtpd2pi_m, cvtpd2pi, 8(%rdi)
    TOMMX k_cvttpd2pi, cvttpd2pi, %xmm3
    TOMMX k_cvttpd2pi_m, cvttpd2pi, 8(%rdi)

    .globl _k_movd_in
    .p2align 4
_k_movd_in:
    movq (%rdi), %mm3
    movl 16(%rdi), %eax
    movd %eax, %mm3
    movq %mm3, (%rdi)
    emms
    ret

    .globl _k_movd_out
    .p2align 4
_k_movd_out:
    movq (%rdi), %mm4
    movq $-1, %rax
    movd %mm4, %eax
    movq %rax, 24(%rdi)
    emms
    ret

    .globl _k_movq_in
    .p2align 4
_k_movq_in:
    movq 16(%rdi), %rax
    movq %rax, %mm6
    movq %mm6, (%rdi)
    emms
    ret

    .globl _k_movq_out
    .p2align 4
_k_movq_out:
    movq (%rdi), %mm1
    movq %mm1, %rcx
    movq %rcx, 24(%rdi)
    emms
    ret

    .globl _k_movd_mem
    .p2align 4
_k_movd_mem:
    movq (%rdi), %mm0
    movd 16(%rdi), %mm0
    movd %mm0, 28(%rdi)
    movq %mm0, (%rdi)
    emms
    ret

    .globl _k_movq_7f
    .p2align 4
_k_movq_7f:
    movq 8(%rdi), %mm5
    movq (%rdi), %mm2
    .byte 0x0f, 0x7f, 0xea
    movq %mm2, 16(%rdi)
    emms
    ret

    .globl _k_movq_store
    .p2align 4
_k_movq_store:
    movq 8(%rdi), %mm2
    movq %mm2, 16(%rdi)
    movq (%rdi), %mm3
    movntq %mm3, 24(%rdi)
    emms
    ret

    .globl _k_rex_ignored
    .p2align 4
_k_rex_ignored:
    movq (%rdi), %mm2
    movq 8(%rdi), %mm5
    .byte 0x45, 0x0f, 0xfc, 0xd5
    .byte 0x41, 0x0f, 0x6f, 0xca
    movq %mm1, 16(%rdi)
    movq %mm2, (%rdi)
    emms
    ret

    .globl _k_r9base
    .p2align 4
_k_r9base:
    movq %rdi, %r9
    movq (%r9), %mm2
    paddw 8(%r9), %mm2
    pmaddwd 16(%r9), %mm2
    movq %mm2, (%r9)
    emms
    ret

    .globl _k_pinsrw
    .p2align 4
_k_pinsrw:
    movq (%rdi), %mm2
    movq 16(%rdi), %rax
    pinsrw $0, %eax, %mm2
    shrq $16, %rax
    pinsrw $6, %eax, %mm2
    pinsrw $3, 8(%rdi), %mm2
    movq %mm2, (%rdi)
    emms
    ret

    .globl _k_pextrw
    .p2align 4
_k_pextrw:
    movq (%rdi), %mm2
    movq $-1, %rax
    movq $-1, %rcx
    pextrw $1, %mm2, %eax
    pextrw $7, %mm2, %ecx
    shlq $32, %rcx
    orq %rcx, %rax
    movq %rax, 24(%rdi)
    emms
    ret

    .globl _k_pmovmskb
    .p2align 4
_k_pmovmskb:
    movq (%rdi), %mm2
    movq $-1, %rax
    pmovmskb %mm2, %eax
    movq %rax, 24(%rdi)
    emms
    ret

    .globl _k_movq2dq
    .p2align 4
_k_movq2dq:
    movq (%rdi), %xmm3
    movhps 16(%rdi), %xmm3
    movq 8(%rdi), %mm5
    movq2dq %mm5, %xmm3
    movq %xmm3, (%rdi)
    movhps %xmm3, 16(%rdi)
    emms
    ret

    .globl _k_movdq2q
    .p2align 4
_k_movdq2q:
    movq 8(%rdi), %xmm3
    movhps 16(%rdi), %xmm3
    movq (%rdi), %mm2
    movdq2q %xmm3, %mm2
    movq %mm2, (%rdi)
    emms
    ret

    .globl _k_x87tag
    .p2align 4
_k_x87tag:
    leaq _g_fxbuf(%rip), %rdx
    fld1
    fld1
    fnstsw %ax
    andl $0x3800, %eax
    movq %rax, %r8
    movq (%rdi), %mm0
    fnstsw %ax
    andl $0x3800, %eax
    shlq $16, %rax
    orq %rax, %r8
    fxsave (%rdx)
    movzbl 4(%rdx), %eax
    shlq $32, %rax
    orq %rax, %r8
    emms
    fxsave (%rdx)
    movzbl 4(%rdx), %eax
    shlq $40, %rax
    orq %rax, %r8
    fnstsw %ax
    andl $0x3800, %eax
    shlq $48, %rax
    orq %rax, %r8
    movq %r8, 24(%rdi)
    ret

    .globl _k_flags
    .p2align 4
_k_flags:
    movq (%rdi), %rax
    movq 8(%rdi), %rcx
    cmpq %rcx, %rax
    movq (%rdi), %mm2
    paddb 8(%rdi), %mm2
    cvtpi2ps %mm2, %xmm3
    cvtps2pi %xmm3, %mm4
    pshufw $0x1b, %mm4, %mm4
    setb %dl
    seto %r8b
    sete %r9b
    movzbl %dl, %edx
    movzbl %r8b, %r8d
    movzbl %r9b, %r9d
    shlq $8, %r8
    shlq $16, %r9
    orq %r8, %rdx
    orq %r9, %rdx
    movq %rdx, 24(%rdi)
    movq %mm4, 16(%rdi)
    movq %mm2, (%rdi)
    emms
    ret

    .globl _k_blend
    .p2align 4
_k_blend:
    pxor %mm7, %mm7
    movq (%rdi), %mm0
    movq 8(%rdi), %mm1
    movq %mm0, %mm2
    punpcklbw %mm7, %mm0
    punpckhbw %mm7, %mm2
    movq %mm1, %mm3
    punpcklbw %mm7, %mm1
    punpckhbw %mm7, %mm3
    pmullw %mm1, %mm0
    pmullw %mm3, %mm2
    psrlw $8, %mm0
    psrlw $8, %mm2
    packuswb %mm2, %mm0
    movq %mm0, (%rdi)
    movq %mm2, 16(%rdi)
    emms
    ret

    .globl _k_loop
    .p2align 4
_k_loop:
    movq (%rdi), %mm0
    movq 8(%rdi), %mm1
    movl $37, %ecx
1:
    paddq %mm1, %mm0
    psrlq $1, %mm1
    pxor %mm0, %mm1
    decl %ecx
    jnz 1b
    movq %mm0, (%rdi)
    movq %mm1, 8(%rdi)
    emms
    ret
