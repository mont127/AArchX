/*
 * The weak-def name filter kept beside the translation store.  The library
 * (weak_bloom_lib.cpp) binds 200 weak definitions of its own through weak-def
 * lookups; past 128 of them ocerz builds a filter of every name the shared
 * cache's weak-defining images export, and keeps it for the next process.
 * The filter only answers "not in the cache", so a wrong one would bind
 * these to nothing or to the wrong place, and the sum would be wrong; this
 * checks the sum against the same arithmetic done here.  The script runs it
 * with a fresh store, again to read the kept filter, and after corrupting the
 * file, which must be rejected and written again.
 */
#include <stdio.h>

long weak_bloom_sum(void);

int main(void)
{
    long want = 0;
    for (int i = 0; i < 200; i++) {
        int n = i + 10;
        want += (long)i * n + (n ^ 0x5a);
    }
    long got = weak_bloom_sum();
    if (got != want) {
        printf("sum %ld, want %ld\n", got, want);
        return 1;
    }
    printf("OK\n");
    return 0;
}
