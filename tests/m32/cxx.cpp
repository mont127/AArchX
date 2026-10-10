// C++ against the guest GNU libstdc++ (runtime/guest32), without its headers: operator new/delete, static
// initialization, virtual calls and __cxa_demangle.  Exceptions: cxxexc.cpp.
extern "C" int printf(const char *, ...);
extern "C" void free(void *);
namespace __cxxabiv1 { extern "C" char *__cxa_demangle(const char *, char *, unsigned long *, int *); }
struct Shape { virtual ~Shape() {} virtual int area() const = 0; };
struct Rect : Shape { int w, h; Rect(int w, int h) : w(w), h(h) {} int area() const { return w * h; } };
struct Counter { static int live; Counter() { live++; } ~Counter() { live--; } };
int Counter::live = 0;
static Counter g_counter;                       // a static constructor, run by the loader
int main() {
    Shape *s = new Rect(6, 7);
    printf("area %d live %d\n", s->area(), Counter::live);
    int status = -1;
    char *d = __cxxabiv1::__cxa_demangle("_ZN4Rect4areaEv", 0, 0, &status);
    printf("demangle %s %d\n", d ? d : "(null)", status);
    free(d);
    delete s;
    int *arr = new int[1000]; arr[999] = 5; printf("array %d\n", arr[999]); delete[] arr;
    return 0;
}
