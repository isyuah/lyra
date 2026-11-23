#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

import lyra.build;
import lyra.check;
import lyra.modules;
import lyra.packages;

namespace fs = std::filesystem;
using namespace lyra;

namespace {
constexpr auto usage = "lyra new <目录> [--name <包名>] [--lib]\n"
                       "lyra check|build|run|test <包目录> [--locked] [--offline] [--diagnostics=json]\n"
                       "lyra build <包目录> [-o <产物>] [--release] | run <包目录> [--release] [-- <程序实参>...]\n"
                       "lyra lock|fetch <目录> [--locked] [--offline] | lock <目录> --update\n"
                       "lyra pack <目录> [-o <源码包>|--registry <本地目录>]";

struct Options {
    std::string_view command;
    fs::path directory;
    std::optional<fs::path> output;
    PackageOptions packages{};
    bool release{};
    std::vector<std::string_view> arguments{};
};

Options options(const std::vector<std::string_view>& args, const std::vector<std::string_view>& arguments, bool separator) {
    auto command = args.front();
    if (command != "check" && command != "build" && command != "run" && command != "test") throw Diagnostic{"LYC0001", usage};
    if (separator && command != "run") throw Diagnostic{"LYC0001", usage};
    Options result{command, {}, {}, {}, false, arguments};
    for (std::size_t index = 1; index < args.size(); ++index) {
        auto arg = args[index];
        if (arg == "--locked" && !result.packages.locked) result.packages.locked = true;
        else if (arg == "--offline" && !result.packages.offline) result.packages.offline = true;
        else if (arg == "--release" && command != "check" && !result.release) result.release = true;
        else if (arg == "-o" && command == "build" && !result.output && index + 1 < args.size() && !args[index + 1].starts_with('-')) result.output = args[++index];
        else if (!arg.empty() && !arg.starts_with('-') && result.directory.empty()) result.directory = arg;
        else throw Diagnostic{"LYC0001", usage};
    }
    if (result.directory.empty()) throw Diagnostic{"LYC0001", usage};
    return result;
}

int compile(const Options& opts, Compilation program, bool library, fs::path output) {
    auto module = check(std::move(program.syntax), program.sources, !library);
    if (opts.command == "check") return 0;
    if (library) throw Diagnostic{"LYC0002", "库没有可执行入口；使用 lyra check 或 lyra test 检查库"};
    build(module, program.sources, output, opts.release);
    return opts.command == "build" ? 0 : execute(output, opts.arguments);
}

int project(const Options& opts, bool json) {
    auto binary = [&](const fs::path& root) { return opts.output.value_or(root / "build" / (opts.release ? "release" : "debug") / "app"); };
    if (!fs::exists(opts.directory / "lyra.json5")) {
        if (opts.packages.locked || opts.packages.offline || opts.command == "test") throw Diagnostic{"LYK0001", "此操作需要包清单 lyra.json5"};
        auto sources = loadSources(opts.directory);
        auto module = parse(sources);
        if (!module.imports.empty()) throw at(sources[module.imports.front().file], module.imports.front().path.front().span, "LYN0003", "使用 import 需要包清单 lyra.json5");
        return compile(opts, {std::move(sources), std::move(module)}, false, binary(opts.directory));
    }
    auto graph = resolvePackages(opts.directory, opts.packages);
    if (!json) for (const auto& warning : graph.warnings) std::cerr << "warning: " << warning << '\n';
    if (!graph.missingOutputs.empty()) throw Diagnostic{"LYK0007", "未执行构建脚本，" + graph.missingOutputs.front()};
    if (graph.entry.empty() && opts.command != "check" && opts.command != "test") throw Diagnostic{"LYC0002", "请指定要运行的 workspace 成员目录"};
    if (opts.output && fs::exists(*opts.output)) {
        for (const auto& package : graph.packages) if (fs::equivalent(*opts.output, package.root / "lyra.json5")) throw Diagnostic{"LYI0001", "输出不能覆盖包清单"};
        if (fs::equivalent(*opts.output, graph.workspace / "lyra.lock")) throw Diagnostic{"LYI0001", "输出不能覆盖包锁"};
    }
    auto entries = graph.entry.empty() ? graph.members : std::vector<std::string>{graph.entry};
    std::size_t tests = 0;
    for (const auto& id : entries) {
        const auto& package = *std::ranges::find(graph.packages, id, &ResolvedPackage::id);
        if (opts.command != "test") {
            auto code = compile(opts, loadProgram(graph, id), package.library, binary(package.root));
            if (code) return code;
            continue;
        }
        auto dir = package.root / "tests";
        std::vector<fs::path> cases;
        if (fs::is_directory(dir)) for (const auto& file : fs::directory_iterator(dir)) {
            if (file.is_regular_file() && file.path().extension() == ".ly") cases.push_back(file.path());
        }
        std::sort(cases.begin(), cases.end());
        for (const auto& path : cases) {
            auto program = loadProgram(graph, id, path);
            auto output = package.root / "build" / "test" / (opts.release ? "release" : "debug") / path.stem();
            int status = compile(opts, std::move(program), false, output);
            if (status) throw Diagnostic{"LYC0003", path.string() + " 测试退出码：" + std::to_string(status)};
            ++tests;
            std::cout << "ok " << package.name << '/' << path.filename().string() << '\n';
        }
    }
    if (opts.command == "test" && !tests) throw Diagnostic{"LYC0003", "没有找到 tests/*.ly 测试入口"};
    return 0;
}
}

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    auto separator = std::ranges::find(args, "--");
    bool forwards = separator != args.end();
    std::vector<std::string_view> arguments;
    if (forwards) { arguments.assign(separator + 1, args.end()); args.erase(separator, args.end()); }
    bool json = std::erase(args, "--diagnostics=json") > 0;
    try {
        if (args.size() == 1 && args[0] == "--help") { std::cout << usage << '\n'; return 0; }
        if (args.empty()) throw Diagnostic{"LYC0001", usage};
        int code;
        if (args[0] == "new" || args[0] == "lock" || args[0] == "fetch" || args[0] == "pack") {
            if (forwards) throw Diagnostic{"LYC0001", usage};
            code = packageCommand(args);
        } else code = project(options(args, arguments, forwards), json);
        if (json) std::cerr << "[]\n";
        return code;
    } catch (const Diagnostic& error) { report(error, json); }
    catch (const PackageError& error) { report({error.code, error.message}, json); }
    catch (const fs::filesystem_error& error) { report({"LYI0001", error.what()}, json); }
    return 1;
}
