#include <stdio.h>
#include <stdlib.h>

void *swift_retain(void *p)
{
    (void)p;
    puts("the bundled libswiftCore was loaded");
    exit(1);
}
