#include <stdio.h>

extern void *swift_retain(void *);

int main(void)
{
    swift_retain(NULL);
    puts("the system libswiftCore was loaded");
    return 0;
}
