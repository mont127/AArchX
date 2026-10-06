// Apple's libc++abi exports operator new, new[], delete and delete[] taking a
// std::__type_descriptor_t (the four the SDK lists), and macOS 26 libraries
// import them - D3DMetal's libdxccontainer.dylib does - so the guest C++
// runtime ocerz builds has to export them too.  Each must behave as the plain
// operator it stands for.
#include <cstdio>
#include <cstring>
#include <new>

namespace std {
enum class __type_descriptor_t : unsigned long long;
}

void *operator new(std::size_t, std::__type_descriptor_t);
void *operator new[](std::size_t, std::__type_descriptor_t);
void operator delete(void *, std::__type_descriptor_t) noexcept;
void operator delete[](void *, std::__type_descriptor_t) noexcept;

int main()
{
    const auto t = static_cast<std::__type_descriptor_t>(0x1234);
    char *one = static_cast<char *>(operator new(48, t));
    char *many = static_cast<char *>(operator new[](4096, t));
    std::memset(one, 'a', 48);
    std::memset(many, 'b', 4096);
    std::printf("typed operator new and delete: %d %d\n", one[47] == 'a', many[4095] == 'b');
    operator delete(one, t);
    operator delete[](many, t);
    return 0;
}
