/* Zlib streams from an i386 guest (the z_stream layout differs from the host's). */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void)
{
    char text[4000], packed[4000], back[4000];
    for (int i = 0; i < (int)sizeof text; i++)
        text[i] = "batman arkham asylum "[i % 21];
    z_stream d;
    memset(&d, 0, sizeof d);
    deflateInit2(&d, 6, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
    d.next_in = (Bytef *)text;
    d.avail_in = sizeof text;
    d.next_out = (Bytef *)packed;
    d.avail_out = sizeof packed;
    int r1 = deflate(&d, Z_FINISH);
    unsigned packed_len = (unsigned)d.total_out;
    deflateEnd(&d);
    z_stream i;
    memset(&i, 0, sizeof i);
    inflateInit(&i);
    i.next_in = (Bytef *)packed;
    i.avail_in = packed_len;
    i.next_out = (Bytef *)back;
    i.avail_out = 1000;   /* two steps: the stream keeps its place between calls */
    int r2 = inflate(&i, Z_NO_FLUSH);
    i.avail_out = sizeof back - 1000;
    int r3 = inflate(&i, Z_FINISH);
    unsigned out = (unsigned)i.total_out;
    inflateEnd(&i);
    printf("deflate %d small %d inflate %d %d out %u same %d\n", r1, packed_len < 200, r2, r3, out,
           out == sizeof text && !memcmp(text, back, sizeof text));
    return 0;
}
