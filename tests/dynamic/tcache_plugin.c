/*
 * The bundle tcache_plugin_main dlopens: a few shapes of code (a loop, a
 * table of functions, recursion) that exist only once the bundle is loaded,
 * so the program's own image and its static dependencies do not reach them.
 */
typedef unsigned long ul;
static ul p_add(ul a, ul b) { return a + b * 3; }
static ul p_xor(ul a, ul b) { return (a ^ b) + 7; }
static ul p_rot(ul a, ul b) { return (a << (b & 15)) | (a >> (64 - (b & 15) - 1) >> 1); }
static ul (*const ops[3])(ul, ul) = { p_add, p_xor, p_rot };

static ul depth(ul n) { return n < 2 ? n + 1 : depth(n - 1) + depth(n - 2) % 1000003; }

ul plugin_work(ul n)
{
    ul acc = 1;
    for (ul i = 0; i < n; i++)
        acc = ops[i % 3](acc, i) % 0xfffffffbul;
    return acc + depth(18);
}
