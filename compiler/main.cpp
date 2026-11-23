#include "frontend.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    bool json = std::erase(args, "--diagnostics=json") > 0;
    try {
        if (args.size() != 2 || args[0] != "check") throw lyra::Diagnostic{"LYC0001", "用法：lyra check <包目录> [--diagnostics=json]"};
        auto sources = lyra::loadSources(args[1]);
        auto module = lyra::parse(sources);
        lyra::check(module, sources);
        if (json) std::cerr << "[]\n";
        return 0;
    } catch (const lyra::Diagnostic& error) {
        lyra::report(error, json);
    } catch (const std::filesystem::filesystem_error& error) {
        lyra::report({"LYI0001", error.what()}, json);
    }
    return 1;
}
