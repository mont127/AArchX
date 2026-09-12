/*
 * Win32 synchronization stress: the primitives Chromium and V8 park on
 * (SleepConditionVariableSRW / WakeConditionVariable, WaitOnAddress /
 * WakeByAddress*, SRWLOCK, SleepConditionVariableCS, events), each hammered by
 * a pool of threads in producer/consumer handoffs.  On Windows and Wine these
 * never stall.  A watchdog thread reports "STALL <phase> at <n>" and exits 2 if
 * a phase makes no progress for 8 s; "OK" and exit 0 otherwise.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o sync_stress.exe sync_stress.c -lsynchronization
 *
 * The phases are, in order: an SRWLOCK + CONDITION_VARIABLE queue
 * (base::ConditionVariable), a WaitOnAddress/WakeByAddressSingle ping-pong
 * (RtlWaitOnAddress), SRWLOCK exclusive/shared contention, CRITICAL_SECTION +
 * SleepConditionVariableCS barrier rounds, and an auto-reset event chain
 * (kernel objects).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 8
#define ITERS    150000

static volatile LONG g_progress;
static volatile LONG g_phase;
static const char *g_phase_name = "start";

static SRWLOCK cv_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE cv_notempty = CONDITION_VARIABLE_INIT, cv_notfull = CONDITION_VARIABLE_INIT;
static int cv_queue[64], cv_head, cv_tail, cv_count, cv_done;

static DWORD WINAPI cv_producer(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    for (int i = 0; i < ITERS; i++) {
        AcquireSRWLockExclusive(&cv_lock);
        while (cv_count == 64) SleepConditionVariableSRW(&cv_notfull, &cv_lock, INFINITE, 0);
        cv_queue[cv_tail] = id * ITERS + i; cv_tail = (cv_tail + 1) % 64; cv_count++;
        ReleaseSRWLockExclusive(&cv_lock);
        WakeConditionVariable(&cv_notempty);
    }
    return 0;
}
static DWORD WINAPI cv_consumer(LPVOID arg)
{
    LONG *sum = (LONG *)arg;
    for (;;) {
        AcquireSRWLockExclusive(&cv_lock);
        while (cv_count == 0 && !cv_done) SleepConditionVariableSRW(&cv_notempty, &cv_lock, INFINITE, 0);
        if (cv_count == 0 && cv_done) { ReleaseSRWLockExclusive(&cv_lock); return 0; }
        int v = cv_queue[cv_head]; cv_head = (cv_head + 1) % 64; cv_count--;
        ReleaseSRWLockExclusive(&cv_lock);
        WakeConditionVariable(&cv_notfull);
        InterlockedAdd(sum, v & 0xff);
        InterlockedIncrement(&g_progress);
    }
}

static volatile LONG wa_turn;
static DWORD WINAPI wa_worker(LPVOID arg)
{
    LONG id = (LONG)(INT_PTR)arg;
    for (int i = 0; i < ITERS / 8; i++) {
        LONG cur;
        while ((cur = wa_turn) != id) WaitOnAddress((volatile VOID *)&wa_turn, &cur, sizeof cur, INFINITE);
        InterlockedExchange(&wa_turn, (id + 1) % NTHREADS);
        WakeByAddressAll((PVOID)&wa_turn);
        InterlockedIncrement(&g_progress);
    }
    return 0;
}

static SRWLOCK srw = SRWLOCK_INIT;
static volatile LONGLONG srw_counter;
static DWORD WINAPI srw_worker(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    for (int i = 0; i < ITERS; i++) {
        if ((i + id) & 3) {
            AcquireSRWLockShared(&srw);
            volatile LONGLONG v = srw_counter; (void)v;
            ReleaseSRWLockShared(&srw);
        } else {
            AcquireSRWLockExclusive(&srw);
            srw_counter++;
            ReleaseSRWLockExclusive(&srw);
        }
        if ((i & 255) == 0) InterlockedIncrement(&g_progress);
    }
    return 0;
}

static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cs_cv = CONDITION_VARIABLE_INIT;
static int cs_arrived, cs_round;
static DWORD WINAPI cs_worker(LPVOID arg)
{
    (void)arg;
    for (int r = 0; r < ITERS / 20; r++) {
        EnterCriticalSection(&cs);
        int my = cs_round;
        if (++cs_arrived == NTHREADS) { cs_arrived = 0; cs_round++; WakeAllConditionVariable(&cs_cv); }
        else while (cs_round == my) SleepConditionVariableCS(&cs_cv, &cs, INFINITE);
        LeaveCriticalSection(&cs);
        InterlockedIncrement(&g_progress);
    }
    return 0;
}

static HANDLE ev[NTHREADS];
static DWORD WINAPI ev_worker(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    for (int i = 0; i < ITERS / 10; i++) {
        WaitForSingleObject(ev[id], INFINITE);
        SetEvent(ev[(id + 1) % NTHREADS]);
        InterlockedIncrement(&g_progress);
    }
    return 0;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    LONG last = -1; int idle = 0;
    for (;;) {
        Sleep(1000);
        LONG p = g_progress;
        if (p == last) { if (++idle >= 8) { printf("STALL %s at %ld\n", g_phase_name, (long)p); fflush(stdout); ExitProcess(2); } }
        else { idle = 0; last = p; }
    }
}

static void run(const char *name, LPTHREAD_START_ROUTINE fn, void *arg_base, int per_thread_arg, int n)
{
    HANDLE th[NTHREADS];
    g_phase_name = name;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < n; i++)
        th[i] = CreateThread(NULL, 0, fn, per_thread_arg ? (LPVOID)(INT_PTR)i : arg_base, 0, NULL);
    for (int i = 0; i < n; i++) WaitForSingleObject(th[i], INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(th[i]);
    printf("%s: %lu ms\n", name, (unsigned long)(GetTickCount() - t0)); fflush(stdout);
}

int main(void)
{
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    InitializeCriticalSection(&cs);

    {
        HANDLE th[NTHREADS]; LONG sum = 0;
        g_phase_name = "condvar-srw";
        DWORD t0 = GetTickCount();
        for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, cv_consumer, &sum, 0, NULL);
        for (int i = 4; i < 8; i++) th[i] = CreateThread(NULL, 0, cv_producer, (LPVOID)(INT_PTR)(i - 4), 0, NULL);
        for (int i = 4; i < 8; i++) WaitForSingleObject(th[i], INFINITE);
        AcquireSRWLockExclusive(&cv_lock); cv_done = 1; ReleaseSRWLockExclusive(&cv_lock);
        WakeAllConditionVariable(&cv_notempty);
        for (int i = 0; i < 4; i++) WaitForSingleObject(th[i], INFINITE);
        printf("condvar-srw: %lu ms sum=%ld\n", (unsigned long)(GetTickCount() - t0), (long)sum); fflush(stdout);
    }
    run("waitonaddress", wa_worker, NULL, 1, NTHREADS);
    run("srwlock", srw_worker, NULL, 1, NTHREADS);
    run("condvar-cs", cs_worker, NULL, 0, NTHREADS);
    for (int i = 0; i < NTHREADS; i++) ev[i] = CreateEvent(NULL, FALSE, i == 0, NULL);
    run("events", ev_worker, NULL, 1, NTHREADS);

    printf("srw_counter=%lld OK\n", (long long)srw_counter); fflush(stdout);
    return 0;
}
