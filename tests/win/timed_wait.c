/* Timed waits with no waker must return at (not before, not never) their
 * deadline.  V8's safepoint relies on background threads' timed condition-
 * variable waits waking to re-check the safepoint flag and park; an emulator
 * that never delivers the timeout hangs the whole isolate.  Exercises the
 * WaitOnAddress, SleepConditionVariableSRW, SleepConditionVariableCS and
 * Sleep timeout paths (all of which reach NtWaitForAlertByThreadId /
 * RtlWaitOnAddress with a timeout under Wine).  Prints OK when each returns
 * within a sane window of its deadline. */
#include <windows.h>
#include <stdio.h>

static LONGLONG now_ms(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return c.QuadPart * 1000 / f.QuadPart;
}

static int check(const char *name, LONGLONG el, DWORD want)
{
    /* must not return early (>= 0.5x) and must actually return (< 8x + 500ms) */
    int ok = el >= (LONGLONG)want / 2 && el < (LONGLONG)want * 8 + 500;
    printf("  %-26s want~%lums got %lldms %s\n", name, (unsigned long)want, el, ok ? "" : "<<< BAD");
    return ok;
}

int main(void)
{
    int ok = 1;
    const DWORD T = 200;

    /* 1. WaitOnAddress: wait while *addr==compare; nobody changes it -> timeout */
    {
        volatile LONG v = 7; LONG cmp = 7;
        LONGLONG t0 = now_ms();
        BOOL r = WaitOnAddress((volatile VOID *)&v, &cmp, sizeof cmp, T);
        LONGLONG el = now_ms() - t0;
        ok &= check("WaitOnAddress", el, T);
        if (r) { printf("  WaitOnAddress returned TRUE unexpectedly\n"); ok = 0; }
    }

    /* 2. SleepConditionVariableSRW with timeout, no wake */
    {
        SRWLOCK lk = SRWLOCK_INIT; CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
        AcquireSRWLockExclusive(&lk);
        LONGLONG t0 = now_ms();
        BOOL r = SleepConditionVariableSRW(&cv, &lk, T, 0);
        LONGLONG el = now_ms() - t0;
        ReleaseSRWLockExclusive(&lk);
        ok &= check("SleepCondVarSRW", el, T);
        if (r) { printf("  SleepConditionVariableSRW returned TRUE unexpectedly\n"); ok = 0; }
    }

    /* 3. SleepConditionVariableCS with timeout, no wake */
    {
        CRITICAL_SECTION cs; InitializeCriticalSection(&cs);
        CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
        EnterCriticalSection(&cs);
        LONGLONG t0 = now_ms();
        BOOL r = SleepConditionVariableCS(&cv, &cs, T);
        LONGLONG el = now_ms() - t0;
        LeaveCriticalSection(&cs);
        ok &= check("SleepCondVarCS", el, T);
        if (r) { printf("  SleepConditionVariableCS returned TRUE unexpectedly\n"); ok = 0; }
    }

    /* 4. plain Sleep */
    {
        LONGLONG t0 = now_ms();
        Sleep(T);
        ok &= check("Sleep", now_ms() - t0, T);
    }

    /* 5. repeated short timed waits (the poll-with-timeout loop V8 uses) */
    {
        SRWLOCK lk = SRWLOCK_INIT; CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
        AcquireSRWLockExclusive(&lk);
        LONGLONG t0 = now_ms();
        int iters = 0;
        while (now_ms() - t0 < 1000) { SleepConditionVariableSRW(&cv, &lk, 50, 0); iters++; }
        ReleaseSRWLockExclusive(&lk);
        LONGLONG el = now_ms() - t0;
        int good = iters >= 8 && iters <= 40;   /* ~1000/50 = 20 loops */
        printf("  poll-loop 50ms x1s: %d iters in %lldms %s\n", iters, el, good ? "" : "<<< BAD");
        ok &= good;
    }

    printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
