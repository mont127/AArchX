#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
static jmp_buf jb;
static int depth(int n) { if (n == 0) longjmp(jb, 7); return depth(n - 1) + 1; }
int main(void) {
    volatile int kept = 11;
    int r = setjmp(jb);
    if (r == 0) { kept = 12; depth(50); puts("not reached"); }
    printf("longjmp %d kept %d\n", r, kept);
    sigjmp_buf sb; if (sigsetjmp(sb, 1) == 0) siglongjmp(sb, 0); else puts("siglongjmp 0 -> 1");
    jmp_buf j2; volatile int n = 0; if (_setjmp(j2) < 3) { n++; _longjmp(j2, n); } printf("loop %d\n", n);
    printf("signal %d\n", signal(SIGUSR1, SIG_IGN) != SIG_ERR);
    struct sigaction sa = { 0 }, old; sa.sa_handler = SIG_IGN; printf("sigaction %d\n", sigaction(SIGPIPE, &sa, &old));
    return 0;
}
