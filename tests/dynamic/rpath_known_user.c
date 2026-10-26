#include <unistd.h>

int known_value(void)
{
    return getpagesize();
}
