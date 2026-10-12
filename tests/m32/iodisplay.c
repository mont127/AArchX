/* IODisplayCreateInfoDictionary (IOKit/graphics/IOGraphicsLib.h): SDL 2 names each display with it and releases the
 * result unchecked. On Apple silicon CGDisplayIOServicePort answers 0, for which the host still makes a dictionary. */
#include <IOKit/graphics/IOGraphicsLib.h>
#include <stdio.h>

int main(void)
{
    CFDictionaryRef d = IODisplayCreateInfoDictionary(MACH_PORT_NULL, kIODisplayOnlyPreferredName);
    printf("info %s\n", d && CFGetTypeID(d) == CFDictionaryGetTypeID() ? "dictionary" : "missing");
    if (d)
        CFRelease(d);
    return 0;
}
