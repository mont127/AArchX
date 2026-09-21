#include <stdexcept>

extern "C" void guest_cxx_throw()
{
    throw std::runtime_error("guest dylib");
}
