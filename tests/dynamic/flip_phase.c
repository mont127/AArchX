/*
 * Loops whose forward branches change their hot side after a first phase, as
 * winbench's structs does: the arrays start beyond the limit, so the inline
 * side of each branch is hot for the first pass and cold for the rest.  The
 * superblock probes keep the branches during the first pass, keep counting
 * the taken side, probe again once it is hot (2^WATCH_BIT) and invert them
 * (src/jit.c flip_decide_locked, flip_side_hit).  A float compare, an integer
 * compare and a test; prints each checksum, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

struct particle { float x, y, z, vx, vy, vz; int id; };
static struct particle ps[4096];
static int32_t ints[4096];
static uint32_t bits[4096];

__attribute__((noinline)) static unsigned long long floats(int rounds)
{
    unsigned long long acc = 0;
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < 4096; i++) {
            struct particle *p = &ps[i];
            p->x += p->vx; p->y += p->vy; p->z += p->vz;
            if (p->x > 100.0f) { p->x = 0.0f; p->vx = -p->vx; }
            if (p->y < -100.0f) { p->y = 0.0f; p->vy = -p->vy; }
            acc += (unsigned long long)(p->id ^ (int)p->x);
        }
    return acc;
}

__attribute__((noinline)) static unsigned long long integers(int rounds)
{
    unsigned long long acc = 0;
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < 4096; i++) {
            int32_t v = ints[i] + 3;
            if (v > 300) { v = i & 7; acc += 17; }
            ints[i] = v;
            uint32_t b = bits[i] + 1;
            if (b & 0x40000000u) { b = 0; acc ^= (unsigned long long)i << 3; }
            bits[i] = b;
            acc = acc * 3 + (unsigned)v;
        }
    return acc;
}

int main(void)
{
    for (int i = 0; i < 4096; i++) {
        ps[i].x = (float)i; ps[i].y = (float)-i; ps[i].z = 0.5f;
        ps[i].vx = 0.25f; ps[i].vy = -0.5f; ps[i].vz = 0.125f; ps[i].id = i;
        ints[i] = 1000 + i;
        bits[i] = 0x40000000u | (uint32_t)i;
    }
    printf("floats %llu\n", floats(160));
    printf("integers %llu\n", integers(160));
    unsigned long long h = 0;
    for (int i = 0; i < 4096; i++) {
        uint32_t xb, yb;
        __builtin_memcpy(&xb, &ps[i].x, 4);
        __builtin_memcpy(&yb, &ps[i].y, 4);
        h = h * 1000003 + xb + yb + (uint32_t)ints[i] + bits[i];
    }
    printf("state %016llx\n", h);
    return 0;
}
