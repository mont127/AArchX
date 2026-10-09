/*
 * __divdc3 and __divsc3, which the arm64 libSystem does not export.  x86_64
 * code calls these compiler-rt builtins for complex division (Unity 2019's
 * libmonobdwgc imports __divdc3), and the libSystem database bridges them to
 * the host, whose RTLD_DEFAULT lookup searches ocerz itself first and finds
 * these.  arm64 has __muldc3 and __mulsc3, so only the divisions are here.  The
 * algorithm is C99 Annex G's (G.5.1): scaled division, then recovery of the
 * infinities and zeros that the plain quotient turns into NaN.
 */
#include <math.h>

typedef struct { double re, im; } OcerzCDouble;
typedef struct { float re, im; } OcerzCFloat;

OcerzCDouble __divdc3(double a, double b, double c, double d)
{
    int ilogbw = 0;
    double logbw = logb(fmax(fabs(c), fabs(d)));
    if (isfinite(logbw)) {
        ilogbw = (int)logbw;
        c = scalbn(c, -ilogbw);
        d = scalbn(d, -ilogbw);
    }
    double denom = c * c + d * d;
    double x = scalbn((a * c + b * d) / denom, -ilogbw);
    double y = scalbn((b * c - a * d) / denom, -ilogbw);
    if (isnan(x) && isnan(y)) {
        if (denom == 0.0 && (!isnan(a) || !isnan(b))) {
            x = copysign(INFINITY, c) * a;
            y = copysign(INFINITY, c) * b;
        } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            x = INFINITY * (a * c + b * d);
            y = INFINITY * (b * c - a * d);
        } else if (isinf(logbw) && logbw > 0.0 && isfinite(a) && isfinite(b)) {
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            x = 0.0 * (a * c + b * d);
            y = 0.0 * (b * c - a * d);
        }
    }
    return (OcerzCDouble){ x, y };
}

OcerzCFloat __divsc3(float a, float b, float c, float d)
{
    OcerzCDouble r = __divdc3(a, b, c, d);   /* float operands are exact in double */
    return (OcerzCFloat){ (float)r.re, (float)r.im };
}
