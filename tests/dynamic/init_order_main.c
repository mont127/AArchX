#include <stdio.h>

extern int init_dep_ready;

static int main_ctor_ran, main_saw_dep;

__attribute__((constructor)) static void main_ctor(void)
{
    main_ctor_ran = 1;
    main_saw_dep = init_dep_ready;
}

int main(void)
{
    int fails = 0;
    if (!init_dep_ready) {
        printf("BAD the dylib's constructor did not run\n");
        fails++;
    }
    if (!main_ctor_ran) {
        printf("BAD the program's constructor did not run\n");
        fails++;
    } else if (!main_saw_dep) {
        printf("BAD the program's constructor ran before its dependency's\n");
        fails++;
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}
