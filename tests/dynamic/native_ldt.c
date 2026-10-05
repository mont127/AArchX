#include <architecture/i386/table.h>
#include <errno.h>
#include <i386/user_ldt.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    union ldt_entry set, got;
    memset(&set, 0, sizeof set);
    set.data.limit00 = 0xffff;
    set.data.limit16 = 0xf;
    set.data.type = DESC_CODE_READ;
    set.data.dpl = USER_PRIV;
    set.data.present = 1;
    set.data.stksz = DESC_DATA_32B;
    set.data.granular = DESC_GRAN_PAGE;
    int idx = i386_set_ldt(16, &set, 1);
    memset(&got, 0, sizeof got);
    int n = idx >= 0 ? i386_get_ldt(idx, &got, 1) : -1;
    errno = 0;
    int bad = i386_set_ldt(0, NULL, 1);
    unsigned long long a, b;
    memcpy(&a, &set, 8);
    memcpy(&b, &got, 8);
    printf("ldt set=%d read=%d set=%#llx got=%#llx bad=%d errno=%s\n", idx, n, a, b, bad,
           errno == EINVAL ? "EINVAL" : "other");
    return 0;
}
