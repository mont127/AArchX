// C++ against the guest LLVM libc++ (runtime/guest32), as Apple's toolchain built it for macOS 10.9 to 10.14: the
// std::__1 strings MW3's game program uses, a function-local static (__cxa_guard_*) and an exception from libc++abi.
extern "C" int printf(const char *, ...);
extern "C" unsigned long strlen(const char *);
namespace std { inline namespace __1 {
template <class C> struct char_traits;
template <class T> struct allocator;
template <class C, class T, class A> class basic_string {
public:
    void __init(const C *, unsigned long);
    basic_string(const basic_string &);
    ~basic_string();
    basic_string &append(const C *);
    const C *c_str() const { return (reinterpret_cast<const unsigned char *>(this)[0] & 1) ? *reinterpret_cast<C *const *>(reinterpret_cast<const char *>(this) + 8) : reinterpret_cast<const C *>(this) + 1; }
    unsigned long size() const { return (reinterpret_cast<const unsigned char *>(this)[0] & 1) ? reinterpret_cast<const unsigned long *>(this)[1] : reinterpret_cast<const unsigned char *>(this)[0] >> 1; }
    basic_string(const C *s) { __init(s, strlen(s)); }
private:
    unsigned long w[3];
};
typedef basic_string<char, char_traits<char>, allocator<char> > string;
} }
static int calls;
static int once() { return ++calls; }
static int cached() { static int v = once(); return v; }
int main() {
    std::string s("short");
    s.append(" and now longer than one word");
    std::string t(s);
    printf("string %lu %s\n", t.size(), t.c_str());
    printf("static %d %d calls %d\n", cached(), cached(), calls);
    try { throw 42; } catch (int e) { printf("caught %d\n", e); }
    return 0;
}
