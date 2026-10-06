/*
 * A service's first act is an RPC to services.exe: StartServiceCtrlDispatcher
 * opens the service manager, which sends a request over \pipe\svcctl and
 * waits on an event wineserver sets when the reply arrives.  Under ocerz's
 * native mode with msync, PlugPlay and RpcSs sometimes waited on that event
 * forever and services.exe gave up on them after ten seconds.  This starts
 * fresh processes whose first act is OpenSCManager and reports how long each
 * call took; a call that does not come back within the limit is a lost reply.
 *
 *   first_rpc.exe [N]      parent: N children (default 12), prints OK or BAD
 *   first_rpc.exe child    open and close the service manager once
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LIMIT 8000

static LONGLONG now_ms(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return c.QuadPart * 1000 / f.QuadPart;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        LONGLONG t = now_ms();
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        LONGLONG el = now_ms() - t;
        if (m)
            CloseServiceHandle(m);
        return m ? (int)(el < 0xfff ? el : 0xfff) : 0x7fff;
    }
    int n = argc > 1 ? atoi(argv[1]) : 12, bad = 0;
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" child", exe);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int k = 0; k < n; k++) {
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("  child %d: CreateProcess failed %lu <<< BAD\n", k, GetLastError());
            bad++;
            continue;
        }
        LONGLONG t = now_ms();
        DWORD rc = WaitForSingleObject(pi.hProcess, LIMIT + 4000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        int ok = rc == WAIT_OBJECT_0 && code != 0x7fff && code < LIMIT;
        printf("  child %d: OpenSCManager %s in %lums (process %lldms) %s\n", k,
               code == 0x7fff ? "failed" : "returned", (unsigned long)code, now_ms() - t, ok ? "" : "<<< BAD");
        if (rc != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        bad += !ok;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    printf("%s\n", bad ? "BAD" : "OK");
    return bad ? 1 : 0;
}
