#include <cstdio>
#include <string>
#include <vector>

struct Holder {
    std::vector<std::string> *names;
    Holder() : names(new std::vector<std::string>{"alpha", "beta"}) {}
};

static Holder holder;

int main()
{
    if (!holder.names || holder.names->size() != 2 || (*holder.names)[1] != "beta") {
        std::printf("BAD the global constructor did not run\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
