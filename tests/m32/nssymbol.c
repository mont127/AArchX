/* NSIsSymbolNameDefined, NSLookupAndBindSymbol, NSAddressOfSymbol, NSModuleForSymbol and NSLibraryNameForModule
 * from i386 code answer from the i386 images.  Miles (Left 4 Dead 2) finds its own folder this way, and its MP3
 * decoder in osx32 beside it; the host knew no i386 symbol, and the music stayed silent.  Expected written by hand. */
#include <stdio.h>
#include <string.h>

typedef void *NSSymbol, *NSModule;
int NSIsSymbolNameDefined(const char *);
NSSymbol NSLookupAndBindSymbol(const char *);
void *NSAddressOfSymbol(NSSymbol);
NSModule NSModuleForSymbol(NSSymbol);
const char *NSLibraryNameForModule(NSModule);

int m32t_marker(void) { return 7; }

int main(void)
{
    NSSymbol s = NSLookupAndBindSymbol("_m32t_marker");
    const char *lib = NSLibraryNameForModule(NSModuleForSymbol(s));
    printf("defined %d %d\n", NSIsSymbolNameDefined("_m32t_marker"), NSIsSymbolNameDefined("_m32t_missing"));
    printf("address %d calls %d\n", NSAddressOfSymbol(s) == (void *)m32t_marker, ((int (*)(void))NSAddressOfSymbol(s))());
    printf("library %s\n", lib && strrchr(lib, '/') ? strrchr(lib, '/') + 1 : "(none)");
    return 0;
}
