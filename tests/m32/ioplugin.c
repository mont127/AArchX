/* IOCreatePlugInInterfaceForService hands back a COM-style object the caller drives through its vtable, which does not
 * cross; it answers kIOReturnUnsupported and leaves the interface NULL, so a caller (SDL's and Chromium's HID code in
 * Left 4 Dead 2) goes on without the device. Expected output written by hand. */
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <stdio.h>

int main(void)
{
    IOCFPlugInInterface **plug = (IOCFPlugInInterface **)1;
    SInt32 score = 7;
    IOReturn r = IOCreatePlugInInterfaceForService(0, kIOHIDDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    printf("result %#x interface %p\n", (unsigned)r, (void *)plug);
    return 0;
}
