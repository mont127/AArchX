/*
 * The CoreFoundation headers sdkgen parses, which is the framework's own
 * umbrella plus the one public header the umbrella leaves out.
 *
 * CoreFoundation.h includes every CF*.h but CFPlugInCOM.h, the COM-style
 * interface macros and the IUnknown vtable for plug-ins, which declares no
 * function of its own today but is part of the framework's public surface and
 * costs nothing to parse.  The file is parsed as C, not Objective-C, because
 * the x86 programs native mode runs call CoreFoundation through its C
 * interface, and the Objective-C spellings of the same types differ only in
 * sugar that the generator strips anyway.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
