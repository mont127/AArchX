#define ACCELERATE_NEW_LAPACK
#define ACCELERATE_LAPACK_ILP64
#include <Accelerate/Accelerate.h>
#include <Security/SecAsn1Coder.h>
#include <Security/SecAsn1Templates.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    float a[6] = { 1, 2, 3, 4, 5, 6 }, b[6] = { 7, 8, 9, 10, 11, 12 }, c[4] = { 0 };
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 2, 2, 3, 1.0f, a, 3, b, 2, 0.0f, c, 2);
    printf("sgemm %.0f %.0f %.0f %.0f\n", c[0], c[1], c[2], c[3]);
    double m[4] = { 4, 3, 6, 3 }, rhs[2] = { 10, 12 };
    __LAPACK_int n = 2, nrhs = 1, lda = 2, ldb = 2, info = 0, ipiv[2];
    dgesv_(&n, &nrhs, m, &lda, ipiv, rhs, &ldb, &info);
    printf("dgesv info=%ld x=%.3f,%.3f\n", (long)info, rhs[0], rhs[1]);
    SecAsn1CoderRef coder = NULL;
    OSStatus st = SecAsn1CoderCreate(&coder);
    static const uint8_t der[] = { 0x04, 0x05, 'h', 'e', 'l', 'l', 'o' };
    SecAsn1Item out = { 0, NULL };
    OSStatus st2 = SecAsn1Decode(coder, der, sizeof der, kSecAsn1OctetStringTemplate, &out);
    printf("asn1 %d %d len=%zu value=%.*s\n", (int)st, (int)st2, out.Length, (int)out.Length, (const char *)out.Data);
    SecAsn1Item encoded = { 0, NULL };
    SecAsn1Item in = { 3, (uint8_t *)"abc" };
    OSStatus st3 = SecAsn1EncodeItem(coder, &in, kSecAsn1OctetStringTemplate, &encoded);
    printf("asn1 encode %d len=%zu first=%02x\n", (int)st3, encoded.Length, encoded.Length ? encoded.Data[0] : 0);
    SecAsn1CoderRelease(coder);
    return 0;
}
