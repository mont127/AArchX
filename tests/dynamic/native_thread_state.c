/*
 * Debug-register thread state, the flavors Wine's macOS wineserver reads for
 * every context request that wants debug registers: x86_DEBUG_STATE64 and
 * x86_DEBUG_STATE, whose header names the 64-bit flavor.  Setting them back to
 * zero is a context restore.  x86_64 only, so Rosetta is the oracle.
 */
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    thread_act_t self = mach_thread_self();
    x86_debug_state64_t d64;
    memset(&d64, 0xff, sizeof d64);
    mach_msg_type_number_t n = x86_DEBUG_STATE64_COUNT;
    kern_return_t k1 = thread_get_state(self, x86_DEBUG_STATE64, (thread_state_t)&d64, &n);
    int zero = 1;
    for (unsigned i = 0; i < sizeof d64; i++)
        zero &= ((unsigned char *)&d64)[i] == 0;
    x86_debug_state_t d;
    memset(&d, 0xff, sizeof d);
    mach_msg_type_number_t m = x86_DEBUG_STATE_COUNT;
    kern_return_t k2 = thread_get_state(self, x86_DEBUG_STATE, (thread_state_t)&d, &m);
    memset(&d64, 0, sizeof d64);
    kern_return_t k3 = thread_set_state(self, x86_DEBUG_STATE64, (thread_state_t)&d64, x86_DEBUG_STATE64_COUNT);
    printf("debug64 kr=%d count=%u zero=%d; debug kr=%d count=%u header=%d/%d; set zero kr=%d\n", k1, n, zero, k2, m,
           d.dsh.flavor, d.dsh.count, k3);
    return 0;
}
