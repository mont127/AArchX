/*
 * A dylib built for macOS 10.5, in the format that predates dyld's compressed
 * fixup information.  It carries a pointer to its own data, which only a
 * rebase through its local relocations makes right, a pointer to strlen, which
 * only an external relocation binds, and calls through lazy symbol pointers.
 * See legacy_format.c.
 */
#include <string.h>

static int value = 7;
int *legacy_value_ptr = &value;
size_t (*legacy_strlen_ptr)(const char *) = strlen;

int legacy_check(void)
{
    int bad = 0;
    if (!legacy_value_ptr || *legacy_value_ptr != 7)
        bad |= 1;
    if (legacy_strlen_ptr != strlen || legacy_strlen_ptr("abcd") != 4)
        bad |= 2;
    if (strlen("abc") != 3)
        bad |= 4;
    return bad;
}
