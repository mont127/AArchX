#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <stdio.h>

__attribute__((naked, noinline)) static kern_return_t invalid_trap(void)
{
    __asm__("movl $0x1000005, %eax\n"
            "syscall\n"
            "ret\n");
}

int main(void)
{
    mach_port_t task = mach_task_self();
    semaphore_t s1 = MACH_PORT_NULL, s2 = MACH_PORT_NULL;
    if (semaphore_create(task, &s1, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS ||
        semaphore_create(task, &s2, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        printf("BAD semaphore_create\n");
        return 1;
    }
    kern_return_t kr = semaphore_signal_thread(s1, MACH_PORT_NULL);
    if (kr != KERN_NOT_WAITING) {
        printf("BAD semaphore_signal_thread %d\n", kr);
        return 2;
    }
    mach_timespec_t ms = { 0, 1000000 };
    kr = semaphore_timedwait_signal(s1, s2, ms);
    if (kr != KERN_OPERATION_TIMED_OUT) {
        printf("BAD semaphore_timedwait_signal %d\n", kr);
        return 3;
    }
    mach_timespec_t zero = { 0, 0 };
    kr = semaphore_timedwait(s2, zero);
    if (kr != KERN_SUCCESS) {
        printf("BAD semaphore signal from timedwait_signal %d\n", kr);
        return 4;
    }
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) {
        printf("BAD mach_port_allocate\n");
        return 5;
    }
    mach_port_context_t guard = 0x6f63657a;
    kr = mach_port_guard(task, port, guard, FALSE);
    if (kr == KERN_SUCCESS)
        kr = mach_port_unguard(task, port, guard);
    if (kr != KERN_SUCCESS) {
        printf("BAD mach_port_unguard %d\n", kr);
        return 6;
    }
    kr = clock_sleep_trap(MACH_PORT_NULL, TIME_RELATIVE, 0, 1000000, NULL);
    if (kr != KERN_SUCCESS) {
        printf("BAD clock_sleep_trap %d\n", kr);
        return 7;
    }
    uint8_t recipe[256];
    mach_msg_type_number_t recipe_size = sizeof recipe;
    kr = mach_voucher_extract_attr_recipe_trap(MACH_PORT_NULL, MACH_VOUCHER_ATTR_KEY_BANK, recipe, &recipe_size);
    if (kr != MACH_SEND_INVALID_DEST) {
        printf("BAD mach_voucher_extract_attr_recipe_trap %d\n", kr);
        return 8;
    }
    kr = invalid_trap();
    if (kr == KERN_SUCCESS) {
        printf("BAD invalid trap %d\n", kr);
        return 9;
    }
    printf("OK\n");
    return 0;
}
