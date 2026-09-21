#include <string.h>

int COLOR_PAIR(int value)
{
    char text[64];
    memset(text, 'x', sizeof text - 1);
    text[sizeof text - 1] = 0;
    return value * 17 + (int)strlen(text) - 60;
}
