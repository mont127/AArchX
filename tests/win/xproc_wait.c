/*
 * Cross-process wakes, the way services.exe starts a service: it creates the
 * service's control pipe, posts an overlapped ConnectNamedPipe and waits for
 * either the pipe's event or the new process for ten seconds.  The event is
 * set by wineserver when the child's CreateFile completes the connect.  With
 * msync a one-handle wait sleeps on the object's word in shared memory and a
 * multi-handle wait registers with the server and sleeps on a word of its own
 * thread, so the three cases here are three different wake paths: an event a
 * second process sets (single and multiple handles) and an event the server
 * sets.  Each wait must end shortly after its waker, never at its timeout.
 *
 *   xproc_wait.exe            parent: runs every case and prints OK or BAD
 *   xproc_wait.exe child N    the child for case N
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PIPE_NAME "\\\\.\\pipe\\ocerz_xproc_wait"
#define EV_A "ocerz_xproc_wait_a"
#define EV_B "ocerz_xproc_wait_b"
#define DELAY 700
#define LIMIT 8000

static LONGLONG now_ms(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return c.QuadPart * 1000 / f.QuadPart;
}

static HANDLE spawn(int which)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" child %d", exe, which);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int report(const char *name, DWORD rc, DWORD want, LONGLONG el)
{
    int ok = rc == want && el < LIMIT;
    printf("  %-34s rc=%lu after %lldms %s\n", name, (unsigned long)rc, el, ok ? "" : "<<< BAD");
    return ok;
}

static int child(int which)
{
    Sleep(DELAY);
    if (which >= 2) {
        HANDLE h = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            return 3;
        Sleep(DELAY);
        if (which == 3) {
            DWORD n;
            WriteFile(h, "reply", 5, &n, NULL);
            Sleep(DELAY);
        }
        CloseHandle(h);
        return 0;
    }
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, which ? EV_B : EV_A);
    if (!ev)
        return 3;
    SetEvent(ev);
    Sleep(DELAY);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "child"))
        return child(atoi(argv[2]));

    int ok = 1;
    setvbuf(stdout, NULL, _IONBF, 0);

    HANDLE a = CreateEventA(NULL, TRUE, FALSE, EV_A);
    HANDLE p = spawn(0);
    LONGLONG t = now_ms();
    DWORD rc = WaitForSingleObject(a, LIMIT * 2);
    ok &= report("single handle, set by a process", rc, WAIT_OBJECT_0, now_ms() - t);
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);

    HANDLE b = CreateEventA(NULL, TRUE, FALSE, EV_B);
    p = spawn(1);
    HANDLE two[2] = { b, p };
    t = now_ms();
    rc = WaitForMultipleObjects(2, two, FALSE, LIMIT * 2);
    ok &= report("two handles, set by a process", rc, WAIT_OBJECT_0, now_ms() - t);
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);

    HANDLE pipe = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 10000, NULL);
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    p = spawn(2);
    t = now_ms();
    if (!ConnectNamedPipe(pipe, &ov) && GetLastError() == ERROR_IO_PENDING) {
        HANDLE w[2] = { ov.hEvent, p };
        rc = WaitForMultipleObjects(2, w, FALSE, LIMIT * 2);
    } else {
        rc = GetLastError() == ERROR_PIPE_CONNECTED ? WAIT_OBJECT_0 : GetLastError();
    }
    ok &= report("two handles, pipe connect (server)", rc, WAIT_OBJECT_0, now_ms() - t);
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);
    CloseHandle(pipe);

    for (int k = 0; k < 600; k++)
        CreateEventA(NULL, TRUE, FALSE, NULL);
    pipe = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 10000, NULL);
    p = spawn(3);
    ConnectNamedPipe(pipe, NULL);
    char buf[16];
    OVERLAPPED rd = { 0 };
    rd.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = now_ms();
    if (!ReadFile(pipe, buf, sizeof buf, NULL, &rd) && GetLastError() == ERROR_IO_PENDING)
        rc = WaitForSingleObject(rd.hEvent, LIMIT * 2);
    else
        rc = GetLastError() ? GetLastError() : WAIT_OBJECT_0;
    ok &= report("one handle, overlapped read (server)", rc, WAIT_OBJECT_0, now_ms() - t);
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);

    printf("%s\n", ok ? "OK" : "BAD");
    return ok ? 0 : 1;
}
