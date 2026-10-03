/* The order log for upward_defer: each library's initializer appends its name. */
#include <string.h>

static char order[128];

void ud_log(const char *name)
{
    if (order[0])
        strlcat(order, " ", sizeof order);
    strlcat(order, name, sizeof order);
}

const char *ud_order(void)
{
    return order;
}
