/*
 * Which mach VM replies move the process to ordered memory.  ocerz moves a
 * mapping the kernel placed outside the guest arena into it; that used to ask
 * for ordered memory every time, in case the mapping was shared with another
 * process.  Now only a mapping that can have another observer does:
 *   private  - an anonymous purgeable mapping through the MIG mach_vm_map, as
 *              libsystem_trace makes one in nearly every program: private to
 *              this task, so it must stay on plain memory;
 *   shared   - a named memory entry mapped in: shared by nature, so ordered;
 *   cow      - a copy-on-write mapping of a memory entry, writable: ordered,
 *              conservatively.
 * The script runs each mode with OCERZ_ORDERLOG=1 and checks the log; this
 * program checks the memory itself.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <string.h>

typedef kern_return_t (*map_fn)(vm_map_t, mach_vm_address_t *, mach_vm_size_t, mach_vm_offset_t, int,
                                mem_entry_name_port_t, memory_object_offset_t, boolean_t, vm_prot_t, vm_prot_t,
                                vm_inherit_t);

static int fill_check(mach_vm_address_t a, mach_vm_size_t sz, unsigned char v)
{
    memset((void *)a, v, sz);
    for (mach_vm_size_t i = 0; i < sz; i += 4096)
        if (((volatile unsigned char *)a)[i] != v)
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "private";
    mach_vm_size_t sz = 0x40000;
    mach_vm_address_t a = 0x1000, src = 0;
    kern_return_t kr;
    if (!strcmp(mode, "private")) {
        map_fn mig = (map_fn)dlsym(RTLD_DEFAULT, "_kernelrpc_mach_vm_map");
        if (!mig) {
            printf("no _kernelrpc_mach_vm_map\n");
            return 1;
        }
        kr = mig(mach_task_self(), &a, sz, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE | VM_MAKE_TAG(78),
                 MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS || !fill_check(a, sz, 0x33)) {
            printf("private: kr %d\n", kr);
            return 1;
        }
    } else {
        memory_object_size_t esz = sz;
        mach_port_t entry = MACH_PORT_NULL;
        int cow = !strcmp(mode, "cow");
        kr = mach_vm_allocate(mach_task_self(), &src, sz, VM_FLAGS_ANYWHERE);
        if (kr == KERN_SUCCESS)
            memset((void *)src, 0x21, sz);
        a = 0;
        if (kr == KERN_SUCCESS)
            kr = cow ? mach_make_memory_entry_64(mach_task_self(), &esz, src, VM_PROT_DEFAULT | MAP_MEM_VM_COPY,
                                                 &entry, MACH_PORT_NULL)
                     : mach_make_memory_entry_64(mach_task_self(), &esz, 0, MAP_MEM_NAMED_CREATE | VM_PROT_DEFAULT,
                                                 &entry, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS)
            kr = mach_vm_map(mach_task_self(), &a, sz, 0, VM_FLAGS_ANYWHERE, entry, 0, cow ? TRUE : FALSE,
                             VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS) {
            printf("%s: kr %d\n", mode, kr);
            return 1;
        }
        if (cow && ((volatile unsigned char *)a)[100] != 0x21) {
            printf("cow: the copy does not hold the source's bytes\n");
            return 1;
        }
        if (!fill_check(a, sz, 0x44) || (cow && ((volatile unsigned char *)src)[100] != 0x21)) {
            printf("%s: writes went wrong\n", mode);
            return 1;
        }
    }
    printf("OK\n");
    return 0;
}
