/* finite(), the old BSD name for isfinite that today's SDK no longer declares: Left 4 Dead 2's physics checks every
 * velocity and position with it, and unresolved it answered 0 for all of them, so ragdolls froze and jiggled. Expected
 * output written by hand. */
#include <float.h>
#include <math.h>
#include <stdio.h>

int finite(double);
static int (*volatile check)(double) = finite;   /* called through a pointer, as clang would inline a direct call */

int main(void)
{
    volatile double zero = 0;
    double v[] = { 1.5, 0.0, -2e300, DBL_MAX, DBL_MIN / 4, 1 / zero, -1 / zero, zero / zero };
    printf("finite");
    for (int i = 0; i < 8; i++)
        printf(" %d", check(v[i]) != 0);
    printf("\n");
    return 0;
}
