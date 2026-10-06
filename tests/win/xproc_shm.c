/*
 * Named shared memory across processes, the way steam.exe and its CEF
 * webhelper talk: one side creates a pagefile-backed section, the other opens
 * it by name, and each maps views and polls words the other writes.  Views are
 * mapped at offset 0 and at 64 KB, and a second view of the same section in
 * the same process must alias the first.  A copy where a shared mapping was
 * asked for shows up as a side that never sees the other's write.
 *
 *   xproc_shm.exe          parent: prints OK or BAD
 *   xproc_shm.exe child    the other side
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NAME "Local\\ocerz_xproc_shm"
#define SIZE (256 * 1024)
#define LIMIT 8000

static int wait_for(volatile LONG *p, LONG want)
{
    DWORD t = GetTickCount();
    while (*p != want)
        if (GetTickCount() - t > LIMIT)
            return 0;
        else
            Sleep(1);
    return 1;
}

static int child(void)
{
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, NAME);
    if (!m)
        return 3;
    volatile LONG *v0 = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SIZE);
    volatile LONG *v1 = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0x10000, 0x10000);
    if (!v0 || !v1)
        return 4;
    if (!wait_for(&v0[0], 1))
        return 5;
    v0[1] = 2;
    if (!wait_for(&v1[0], 3))
        return 6;
    v1[1] = 4;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child"))
        return child();
    setvbuf(stdout, NULL, _IONBF, 0);
    int ok = 1;
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, SIZE, NAME);
    volatile LONG *v0 = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SIZE) : NULL;
    volatile LONG *again = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SIZE) : NULL;
    if (!v0 || !again) {
        printf("mapping failed %lu\nBAD\n", GetLastError());
        return 1;
    }
    v0[100] = 0x1234;
    ok &= again[100] == 0x1234;
    printf("  two views in one process alias %s\n", again[100] == 0x1234 ? "" : "<<< BAD");

    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" child", exe);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed %lu\nBAD\n", GetLastError());
        return 1;
    }
    v0[0] = 1;
    int a = wait_for(&v0[1], 2);
    printf("  child saw offset 0 and answered %s\n", a ? "" : "<<< BAD");
    volatile LONG *v1 = (volatile LONG *)((char *)v0 + 0x10000);
    v1[0] = 3;
    int b = a && wait_for(&v1[1], 4);
    printf("  child saw offset 64K through its own view %s\n", b ? "" : "<<< BAD");
    WaitForSingleObject(pi.hProcess, LIMIT);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("  child exit %lu %s\n", (unsigned long)code, code == 0 ? "" : "<<< BAD");
    ok &= a && b && code == 0;
    printf("%s\n", ok ? "OK" : "BAD");
    return ok ? 0 : 1;
}
