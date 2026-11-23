#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

import lyra.check;
import lyra.build;

namespace fs = std::filesystem;

static constexpr auto usage = "lyra check <包目录> [--diagnostics=json]\n"
                              "lyra build <包目录> [-o <产物>] [--release] [--diagnostics=json]";

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    bool json = std::erase(args, "--diagnostics=json") > 0;
    try {
        if (args.size() == 1 && args[0] == "--help") { std::cout << usage << '\n'; return 0; }
        if (args.empty() || (args[0] != "build" && args[0] != "check")) throw lyra::Diagnostic{"LYC0001", usage};
        bool building = args[0] == "build";
        bool release = false;
        std::optional<fs::path> package, output;
        for (std::size_t index = 1; index < args.size(); ++index) {
            auto arg = args[index];
            if (building && arg == "--release" && !release) release = true;
            else if (building && arg == "-o" && !output && index + 1 < args.size() && !args[index + 1].starts_with('-')) output = args[++index];
            else if (!arg.empty() && !arg.starts_with('-') && !package) package = arg;
            else throw lyra::Diagnostic{"LYC0001", usage};
        }
        if (!package) throw lyra::Diagnostic{"LYC0001", usage};
        auto sources = lyra::loadSources(*package);
        auto module = lyra::parse(sources);
        lyra::check(module, sources);
        if (building) lyra::build(module, sources, output.value_or(*package / "build" / (release ? "release" : "debug") / "app"), release);
        if (json) std::cerr << "[]\n";
        return 0;
    } catch (const lyra::Diagnostic& error) {
        lyra::report(error, json);
    } catch (const std::filesystem::filesystem_error& error) {
        lyra::report({"LYI0001", error.what()}, json);
    }
    return 1;
}
