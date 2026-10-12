/* The Carbon File Manager as MW3 uses it: FSOpenFork's 16-bit fork number (32-bit on the host; the guard beside it
 * must survive), PBReadForkAsync with a completion routine (the i386 parameter block; the completion runs on another
 * thread), FSGetCatalogInfoBulk over a folder (i386 FSCatalogInfo records). fsfork.expected is the x86_64 build's
 * output under Rosetta. */
#include <CoreServices/CoreServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int completed;
static void done(ParmBlkPtr pb) { (void)pb; completed++; }

int main(void)
{
    char dir[] = "/tmp/m32fsforkXXXXXX", path[256];
    mkdtemp(dir);
    const char *names[] = { "a.ff", "b.ff", "c.iwd" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "w");
        for (int k = 0; k < 1000 * (i + 1); k++)
            fputc('a' + k % 26, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/b.ff", dir);
    FSRef ref;
    HFSUniStr255 fork;
    FSGetDataForkName(&fork);
    struct { FSIORefNum num; short guard; } open = { 0, 0x1234 };
    OSErr e = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
    e |= FSOpenFork(&ref, fork.length, fork.unicode, fsRdPerm, &open.num);
    printf("open %d guard %#x\n", e, (unsigned short)open.guard);
    char buf[64] = { 0 };
    FSForkIOParam pb;
    memset(&pb, 0, sizeof pb);
    pb.ioCompletion = NewIOCompletionUPP(done);
    pb.forkRefNum = open.num;
    pb.buffer = buf;
    pb.requestCount = 10;
    pb.positionMode = fsFromStart;
    pb.positionOffset = 1990;
    PBReadForkAsync(&pb);
    for (int i = 0; i < 500 && (pb.ioResult > 0 || !completed); i++)
        usleep(2000);
    printf("read %d actual %u [%s] completed %d\n", pb.ioResult, (unsigned)pb.actualCount, buf, completed);
    FSCloseFork(open.num);
    FSRef dref;
    FSIterator it;
    FSPathMakeRef((const UInt8 *)dir, &dref, NULL);
    FSOpenIterator(&dref, kFSIterateFlat, &it);
    FSCatalogInfo info[8];
    ItemCount n = 0;
    Boolean changed = 0;
    e = FSGetCatalogInfoBulk(it, 8, &n, &changed, kFSCatInfoDataSizes | kFSCatInfoNodeFlags, info, NULL, NULL, NULL);
    unsigned long long total = 0;
    int files = 0;
    for (ItemCount i = 0; i < n; i++) {
        total += info[i].dataLogicalSize;
        files += !(info[i].nodeFlags & kFSNodeIsDirectoryMask);
    }
    printf("bulk %d count %lu files %d bytes %llu\n", e == errFSNoMoreItems ? 0 : e, (unsigned long)n, files, total);
    FSCloseIterator(it);
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
    return 0;
}
