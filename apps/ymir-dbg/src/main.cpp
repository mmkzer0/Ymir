#include <cxxopts.hpp>
#include <fmt/format.h>

int main(int argc, char **argv) {
    cxxopts::Options options{"ymir-dbg", "Ymir headless debug CLI (stub)"};
    options.add_options()("h,help", "Show help");

    const auto result = options.parse(argc, argv);
    if (result.count("help") > 0) {
        fmt::print("{}", options.help());
        return 0;
    }

    fmt::println("ymir-dbg: Ymir headless debug CLI (stub)");
    return 0;
}
