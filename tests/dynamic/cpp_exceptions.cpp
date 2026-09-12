/*
 * C++ exception unwinding across library boundaries: exceptions thrown from
 * libc++ (std::stoi, vector::at), a deep unwind through 50 frames, a nested
 * rethrow, plus virtual dispatch and STL algorithms.  Exercises the Itanium
 * unwinder, __cxa_throw/catch and RTTI type matching in translated code.
 */
#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct Base {
    virtual int f() const { return 1; }
    virtual ~Base() {}
};
struct Derived : Base {
    int f() const override { return 42; }
};

static int deep(int n)
{
    if (n == 0)
        throw std::runtime_error("boom");
    return deep(n - 1) + 1;
}

int main()
{
    int caught = 0;
    try { std::string s = "xyz"; (void)std::stoi(s); }
    catch (const std::invalid_argument &) { caught |= 1; }
    try { std::vector<int> v{ 1, 2, 3 }; (void)v.at(10); }
    catch (const std::out_of_range &) { caught |= 2; }
    try { deep(50); }
    catch (const std::runtime_error &e) { caught |= (std::string(e.what()) == "boom") ? 4 : 0; }
    try {
        try { throw 7; }
        catch (int x) { throw std::logic_error("n" + std::to_string(x)); }
    } catch (const std::logic_error &e) {
        caught |= (std::string(e.what()) == "n7") ? 8 : 0;
    }

    std::vector<Base *> objs{ new Base, new Derived, new Base };
    int sum = 0;
    for (auto *o : objs)
        sum += o->f();
    std::map<std::string, int> m;
    for (int i = 0; i < 5; i++)
        m["k" + std::to_string(i)] = i * i;
    int msum = 0;
    std::for_each(m.begin(), m.end(), [&](auto &p) { msum += p.second; });
    for (auto *o : objs)
        delete o;

    if (caught == 15 && sum == 44 && msum == 30) {
        printf("OK\n");
        return 0;
    }
    printf("BAD caught=%d sum=%d msum=%d\n", caught, sum, msum);
    return 1;
}
