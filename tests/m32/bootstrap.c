/* bootstrap_look_up and bootstrap_check_in from an i386 guest: the service name is a name_t (char[128]) the API
 * database cannot describe, so m32 answers them itself. A service that exists comes back with a port, one that does
 * not with BOOTSTRAP_UNKNOWN_SERVICE and the port left null. Builds for i386 and x86_64; bootstrap.expected is the
 * x86_64 build's output under Rosetta. */
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <stdio.h>

int main(void)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &port);
    printf("notification_center: kr %d port %s\n", kr, port != MACH_PORT_NULL ? "set" : "null");
    port = 0x1234;
    kr = bootstrap_look_up(bootstrap_port, "org.aarchx.no-such-service", &port);
    printf("no-such-service: kr %d port %s\n", kr, port == MACH_PORT_NULL ? "null" : port == 0x1234 ? "untouched" : "set");
    port = MACH_PORT_NULL;
    kr = bootstrap_check_in(bootstrap_port, "org.aarchx.no-such-service", &port);
    printf("check_in unknown: kr %s\n", kr != KERN_SUCCESS ? "refused" : "ok");
    return 0;
}
