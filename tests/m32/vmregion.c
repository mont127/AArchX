/* mach_vm_region from i386 (64-bit address and size cells): a walk over the address space ends, sees only the 4 GB
 * window, and finds the heap. MW3 walks its address space this way; the host's answers kept it walking forever. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    char *heap = malloc(1 << 20);
    heap[0] = 1;
    mach_vm_address_t a = 0, h = (mach_vm_address_t)(uintptr_t)heap;
    int n = 0, below = 1, found = 0, ended = 0;
    while (n < 100000) {
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) {
            ended = 1;
            break;
        }
        below &= a + sz <= 0x100000000ull;
        found |= h >= a && h < a + sz && (info.protection & VM_PROT_WRITE);
        a += sz;
        n++;
    }
    printf("walk ends %d below 4 GB %d heap found %d\n", ended, below, found);
    return 0;
}
