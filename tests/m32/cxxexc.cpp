// C++ exceptions through the guest libstdc++ and m32's unwinder (src/m32_unwind.c): a throw caught frames up with
// destructors run on the way (cleanup landing pads and _Unwind_Resume), a rethrow, the matching catch clause, and
// values the catching function keeps in callee-saved registers across the throw.
extern "C" int printf(const char *, ...);
struct Guard {
    static int live;
    int n;
    Guard(int n) : n(n) { live++; }
    ~Guard() { live--; printf("unwound %d\n", n); }
};
int Guard::live = 0;
struct Error { int code; };
__attribute__((noinline)) static void deep(int n)
{
    Guard g(n);
    if (n == 0)
        throw Error{ 42 };
    deep(n - 1);
}
__attribute__((noinline)) static int twice(int x) { if (x > 100) throw x; return 2 * x; }
int main(int argc, char **argv)
{
    try { deep(3); } catch (const Error &e) { printf("caught %d live %d\n", e.code, Guard::live); }
    try {
        try { throw "text"; } catch (const char *s) { printf("inner %s\n", s); throw; }
    } catch (const char *s) { printf("rethrown %s\n", s); }
    try { throw 7; } catch (long) { printf("wrong\n"); } catch (int i) { printf("int %d\n", i); }
    int a = twice(argc), b = twice(argc + 1), c = twice(argc + 2), d = 0;
    for (int i = 0; i < 3; i++) {
        try { d += twice(a + b + c + 200 * i); } catch (int v) { printf("kept %d %d %d %d caught %d\n", a, b, c, d, v); }
    }
    try { throw Error{ 9 }; } catch (...) { printf("any\n"); }
    return 0;
}
