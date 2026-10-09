/*
 * A library with 200 exported weak definitions of its own, each bound through
 * a weak-def lookup, which is more than the 128 after which ocerz builds its
 * filter of the shared cache's weak-def names.  See weak_bloom.c.
 */
template <int N> __attribute__((noinline)) int weak_fn(int x) { return x * N + (N ^ 0x5a); }

#define I1(n) template int weak_fn<n>(int);
#define I10(n) I1(n##0) I1(n##1) I1(n##2) I1(n##3) I1(n##4) I1(n##5) I1(n##6) I1(n##7) I1(n##8) I1(n##9)
I10(1) I10(2) I10(3) I10(4) I10(5) I10(6) I10(7) I10(8) I10(9) I10(10)
I10(11) I10(12) I10(13) I10(14) I10(15) I10(16) I10(17) I10(18) I10(19) I10(20)

typedef int (*fn_t)(int);
#define P1(n) weak_fn<n>,
#define P10(n) P1(n##0) P1(n##1) P1(n##2) P1(n##3) P1(n##4) P1(n##5) P1(n##6) P1(n##7) P1(n##8) P1(n##9)

extern "C" long weak_bloom_sum(void)
{
    static fn_t fns[] = { P10(1) P10(2) P10(3) P10(4) P10(5) P10(6) P10(7) P10(8) P10(9) P10(10)
                          P10(11) P10(12) P10(13) P10(14) P10(15) P10(16) P10(17) P10(18) P10(19) P10(20) };
    long s = 0;
    for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; i++)
        s += fns[i]((int)i);
    return s;
}
