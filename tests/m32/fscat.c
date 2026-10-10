/* FSGetCatalogInfo from an i386 guest: a directory's valence lands where the i386 FSCatalogInfo has it. */
#include <CoreServices/CoreServices.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/m32_fscat_XXXXXX";
    mkdtemp(dir);
    for (int i = 0; i < 7; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s/f%d", dir, i);
        fclose(fopen(p, "w"));
    }
    FSRef ref;
    FSCatalogInfo info;
    memset(&info, 0xee, sizeof info);
    OSStatus e = FSPathMakeRef((const UInt8 *)dir, &ref, NULL);
    OSErr r = FSGetCatalogInfo(&ref, kFSCatInfoValence | kFSCatInfoNodeFlags, &info, NULL, NULL, NULL);
    printf("err %d %d valence %u dir %d\n", (int)e, (int)r, (unsigned)info.valence, (info.nodeFlags & kFSNodeIsDirectoryMask) != 0);
    for (int i = 0; i < 7; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s/f%d", dir, i);
        unlink(p);
    }
    rmdir(dir);
    return 0;
}
