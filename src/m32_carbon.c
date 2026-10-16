/*
 * Carbon for i386 programs, on the HIToolbox Apple silicon still has (include/ocerz/m32.h).
 *
 * Much of Carbon's window, menu, control and event API is declared for 32-bit code alone yet still exported, and
 * working, in arm64 HIToolbox; tools/sdkgen gives those functions an arm64 declaration made from the i386 one, so
 * they cross like any other.  What differs is wrapped here, at the host function an export calls (m32_carbon_wrap,
 * beside the HUD's wrap in src/m32_bridge.c):
 *
 * - CreateNewWindow: 64-bit HIToolbox makes only compositing windows.  Without kWindowCompositingAttribute it
 *   answers errUnsupportedWindowAttributesForClass (-5601) for every class and attribute set, where 32-bit Carbon
 *   made a classic window that the program drew into with QuickDraw.  A window an i386 program creates gets the
 *   attribute.  QuickDraw itself is not on Apple silicon, and m32 does not provide it yet (docs/m32.md, Limits).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/m32.h"

#define M32_kWindowCompositingAttribute (1u << 19)

static int32_t (*g_create_new_window)(uint32_t, uint32_t, const void *, void **);

static int32_t create_new_window(uint32_t window_class, uint32_t attributes, const void *bounds, void **out)
{
    return g_create_new_window(window_class, attributes | M32_kWindowCompositingAttribute, bounds, out);
}

void *m32_carbon_wrap(const char *symbol, void *fn)
{
    if (!fn || getenv("OCERZ_NO_M32_CARBON_WRAP"))
        return fn;
    if (!strcmp(symbol, "CreateNewWindow")) {
        g_create_new_window = (int32_t (*)(uint32_t, uint32_t, const void *, void **))fn;
        return (void *)create_new_window;
    }
    return fn;
}
