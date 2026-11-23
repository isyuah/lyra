#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <llvm/Support/ConvertUTF.h>
#endif

import lyra.build;
import lyra.check;
import lyra.modules;
import lyra.packages;
import lyra.platform;

namespace fs = std::filesystem;
using namespace lyra;

namespace {
constexpr auto usage = "lyra --help | --version\n"
                       "lyra new <目录> [--name <包名>] [--lib]\n"
                       "lyra check|build|run|test <包目录> [--locked] [--offline] [--diagnostics=json]\n"
                       "lyra build <包目录> [-o <产物>] [--release] [--link <C库>] | run <包目录> [--release] [--link <C库>] [-- <程序实参>...]\n"
                       "lyra lock|fetch <目录> [--locked] [--offline] | lock <目录> --update\n"
                       "lyra pack <目录> [-o <源码包>|--registry <本地目录>]";

struct Options {
    std::string_view command;
    fs::path directory;
    std::optional<fs::path> output;
    PackageOptions packages{};
    bool release{};
    std::vector<std::string_view> arguments{};
    std::vector<fs::path> libraries{};
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
        else if (arg == "-o" && command == "build" && !result.output && index + 1 < args.size() && !args[index + 1].starts_with('-')) result.output = fromUtf8(args[++index]);
        else if (arg == "--link" && command != "check" && index + 1 < args.size() && !args[index + 1].starts_with('-')) {
            auto path = fs::absolute(fromUtf8(args[++index]));
            if (!fs::is_regular_file(path)) throw Diagnostic{"LYI0001", "找不到原生链接输入：" + pathText(path)};
            auto name = pathText(path.filename());
            auto extension = pathText(path.extension());
            if constexpr (windows) {
                if (extension != ".obj" && extension != ".lib") throw Diagnostic{"LYC0001", "--link 只接受 .obj、静态库或导入库 .lib：" + name};
            } else {
                auto version = name.rfind(".so.");
                bool soname = version != name.npos && version + 4 < name.size() && name.find_first_not_of("0123456789.", version + 4) == name.npos;
                if (!soname && extension != ".a" && extension != ".o" && extension != ".so") throw Diagnostic{"LYC0001", "--link 只接受目标文件、静态库或共享库：" + name};
            }
            result.libraries.push_back(std::move(path));
        }
        else if (!arg.empty() && !arg.starts_with('-') && result.directory.empty()) result.directory = fromUtf8(arg);
        else throw Diagnostic{"LYC0001", usage};
    }
    if (result.directory.empty()) throw Diagnostic{"LYC0001", usage};
    return result;
}

int compile(const Options& opts, Compilation program, bool library, fs::path output) {
    auto module = check(std::move(program.syntax), program.sources, !library);
    if (opts.command == "check") return 0;
    if (library) throw Diagnostic{"LYC0002", "库没有可执行入口；使用 lyra check 或 lyra test 检查库"};
    if (fs::exists(output)) for (const auto& input : program.macroInputs) {
        if (fs::equivalent(input, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖宏或其只读输入：" + pathText(output)};
    }
    build(module, program.sources, output, opts.release, opts.libraries);
    return opts.command == "build" ? 0 : execute(output, opts.arguments);
}

int project(const Options& opts, bool json) {
    auto binary = [&](const fs::path& root) { return opts.output.value_or(executablePath(root / "build" / (opts.release ? "release" : "debug") / "app")); };
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
            auto name = path.stem();
            if constexpr (windows) name += ".exe";
            auto output = package.root / "build" / "test" / (opts.release ? "release" : "debug") / name;
            int status = compile(opts, std::move(program), false, output);
            if (status) throw Diagnostic{"LYC0003", pathText(path) + " 测试退出码：" + std::to_string(status)};
            ++tests;
            std::cout << "ok " << package.name << '/' << pathText(path.filename()) << '\n';
        }
    }
    if (opts.command == "test" && !tests) throw Diagnostic{"LYC0003", "没有找到 tests/*.ly 测试入口"};
    return 0;
}

int run(std::vector<std::string_view> args) {
    auto separator = std::ranges::find(args, "--");
    bool forwards = separator != args.end();
    std::vector<std::string_view> arguments;
    if (forwards) { arguments.assign(separator + 1, args.end()); args.erase(separator, args.end()); }
    bool json = std::erase(args, "--diagnostics=json") > 0;
    try {
        if (args.size() == 1 && args[0] == "--version" && !forwards) { std::cout << "lyra " LYRA_TOOLCHAIN_VERSION "\n"; return 0; }
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
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> words(argc - 1);
    for (int index = 1; index < argc; ++index) {
        if (!llvm::convertWideToUTF8(argv[index], words[index - 1])) {
            report({"LYC0001", "命令行包含无效的 UTF-16"}, false);
            return 1;
        }
    }
    return run({words.begin(), words.end()});
}
#else
int main(int argc, char** argv) { return run({argv + 1, argv + argc}); }
#endif
