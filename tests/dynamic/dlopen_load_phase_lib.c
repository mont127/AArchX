#include <CoreServices/CoreServices.h>

int load_phase_lib_touch(void)
{
    return (int)CFGetTypeID(CFSTR("x"));
}
