/* Float-to-integer conversions over the values where x86 and arm64 disagree:
 * the in-range cases, both 2^31 boundaries, values far outside int64, the
 * infinities and a NaN, each converted to long and int from both double and
 * float.  x86 produces the "integer indefinite" (INT_MIN) for anything NaN or
 * out of range where arm64's fcvtzs saturates, so every one of these is a case
 * the JIT has to correct.  Results are folded into one checksum; golden from
 * the native run. */
#include "gsys.h"
static g_u64 acc; static void mix(g_u64 v){ acc=(acc^v)*0x9E3779B97F4A7C15ULL; acc^=acc>>29; }
int main(void){ volatile double big=1e308; double inf=big*10, nan=inf-inf; volatile double dv[10]={0.0,1.5,-1.5,2147483647.0,2147483648.0,-2147483649.0,9.2e18,-9.3e18,inf,-inf};
 for(int i=0;i<10;i++){ mix((g_u64)(long)dv[i]); mix((g_u64)(g_u32)(int)dv[i]); mix((g_u64)(long)(float)dv[i]); mix((g_u64)(g_u32)(int)(float)dv[i]); }
 mix((g_u64)(long)nan); mix((g_u64)(g_u32)(int)nan); mix((g_u64)(long)(float)nan); mix((g_u64)(g_u32)(int)(float)nan);
 g_putu64(acc); return 0; }
