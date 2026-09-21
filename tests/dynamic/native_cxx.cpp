#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <xlocale.h>

struct GuestUnwindSections {
    const void *header;
    const void *dwarf;
    uintptr_t dwarf_size;
    const void *compact;
    uintptr_t compact_size;
};

extern "C" bool _dyld_find_unwind_sections(void *, GuestUnwindSections *);

static std::atomic<int> destroyed{0};
static std::atomic<int> constructed{0};

struct Local {
    int value = 19;
    ~Local() { destroyed.fetch_add(value); }
};

static thread_local Local local;

struct Once {
    Once() { constructed++; }
    int value = 31;
};

static Once &once()
{
    static Once instance;
    return instance;
}

struct Left { virtual ~Left() {} int x = 7; };
struct Right { virtual ~Right() {} int y = 11; };
struct Both : Left, Right { int z = 23; };

static int compare(const void *a, const void *b)
{
    try {
        throw std::runtime_error("inside callback");
    } catch (const std::exception &e) {
        assert(std::strcmp(e.what(), "inside callback") == 0);
    }
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

struct Final {
    ~Final() { std::puts("native_cxx destructor ok"); }
};

static Final final;

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "unsupported-format") == 0) {
        char buffer[16];
        return snprintf_l(buffer, sizeof buffer, nullptr, "%1$d", 7);
    }
    GuestUnwindSections sections{};
    assert(_dyld_find_unwind_sections((void *)&main, &sections));
    assert(sections.header && ((sections.dwarf && sections.dwarf_size) ||
                               (sections.compact && sections.compact_size)));
    assert(!_dyld_find_unwind_sections((void *)1, &sections));
    std::string text = "short";
    text.append(300, 'q');
    text.replace(0, 5, "long");
    assert(text.size() == 304 && text.substr(0, 4) == "long");
    std::unordered_map<std::string, int> hash;
    std::map<int, std::string> ordered;
    for (int i = 0; i < 80; i++) {
        hash[std::to_string(i)] = i * i;
        ordered[i] = std::to_string(i);
    }
    assert(hash.at("17") == 289 && ordered.at(37) == "37");
    std::stringstream stream;
    stream << 42 << ' ' << 1.25 << ' ' << "hello";
    assert(stream.str() == "42 1.25 hello");
    assert(std::regex_match("abc123", std::regex("[a-z]+[0-9]+")));
    assert(std::filesystem::path("/one/two/../three").lexically_normal() == "/one/three");
    locale_t locale = newlocale(LC_ALL_MASK, "C", nullptr);
    assert(locale);
    char formatted[32];
    int length = snprintf_l(formatted, sizeof formatted, locale, "%.*f/%lld/%s", 2, 1.25, 17LL, "ok");
    assert(length == 10 && std::strcmp(formatted, "1.25/17/ok") == 0);
    assert(snprintf_l(formatted, 5, locale, "%d/%s", 123, "abcdef") == 10);
    assert(std::strcmp(formatted, "123/") == 0);
    freelocale(locale);
    std::cout << "native_cxx containers ok\n";

    auto owner = std::make_unique<Both>();
    Left *left = owner.get();
    Right *right = dynamic_cast<Right *>(left);
    assert(right && right->y == 11 && typeid(*right) == typeid(Both));
    assert(dynamic_cast<void *>(right) == owner.get());
    std::exception_ptr saved;
    try { throw std::logic_error("saved"); } catch (...) { saved = std::current_exception(); }
    try { std::rethrow_exception(saved); } catch (const std::logic_error &e) {
        assert(std::strcmp(e.what(), "saved") == 0);
    }
    int values[] = {7, 1, 8, -2};
    std::qsort(values, 4, sizeof(int), compare);
    assert(values[0] == -2 && values[3] == 8);
    std::puts("native_cxx rtti exceptions callbacks ok");

    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    int total = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&] {
            assert(local.value == 19);
            int value = once().value;
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return ready; });
            total += value;
        });
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
    }
    condition.notify_all();
    for (auto &thread : threads)
        thread.join();
    assert(total == 124 && constructed == 1 && destroyed == 76);
    auto future = std::async(std::launch::async, []() -> int { throw std::runtime_error("async"); });
    try { (void)future.get(); assert(false); } catch (const std::runtime_error &e) {
        assert(std::strcmp(e.what(), "async") == 0);
    }
    std::puts("native_cxx threads tls futures ok");

    if (argc == 2) {
        void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::fprintf(stderr, "%s\n", dlerror());
            return 2;
        }
        auto call = (void (*)())dlsym(handle, "guest_cxx_throw");
        assert(call);
        assert(_dyld_find_unwind_sections((void *)call, &sections));
        assert(sections.header && ((sections.dwarf && sections.dwarf_size) ||
                                   (sections.compact && sections.compact_size)));
        try { call(); assert(false); } catch (const std::runtime_error &e) {
            assert(std::strcmp(e.what(), "guest dylib") == 0);
        }
        assert(dlclose(handle) == 0);
        std::puts("native_cxx dylib unwind ok");
    }
    std::puts("native_cxx ok");
}
