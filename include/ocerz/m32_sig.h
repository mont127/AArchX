/*
 * m32's parsed notations (src/m32_cross.c), shared by the crossing and the callbacks.
 */
#ifndef OCERZ_M32_SIG_H
#define OCERZ_M32_SIG_H

#include <stdint.h>

#define M32_MAX_MEMBERS 16

typedef struct M32Type {
    char cls;                 /* the class letter, '{' for a structure */
    uint8_t size, align;
    uint8_t n;                /* structures: flattened members */
    char mcls[M32_MAX_MEMBERS];
    uint8_t msize[M32_MAX_MEMBERS];
    uint16_t moff[M32_MAX_MEMBERS];
    char *cb;                 /* c{...} and k{...}: the inner notation */
    uint16_t ssize;           /* R{...} / r{...}: the size of the structure pointed at */
} M32Type;

typedef struct M32Sig {
    M32Type ret;
    M32Type arg[16];
    int nargs;
} M32Sig;

M32Sig *m32_sig_parse(const char *notation, int guest);   /* guest: i386 sizes and layout */
int m32_is_hfa(const M32Type *t);
int m32_struct_in_regs(const M32Type *t);                  /* i386 Darwin returns it in EAX:EDX or ST0 */
uint64_t m32_scalar_in(char g, char h, uint64_t raw, const char *fn, int argno);   /* guest value -> host */
uint64_t m32_scalar_out(char g, char h, uint64_t raw);                             /* host value -> guest */

#endif
