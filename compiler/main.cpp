#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <llvm/Support/ConvertUTF.h>
#endif

import lyra.build;
import lyra.c_import;
import lyra.check;
import lyra.format;
import lyra.lsp;
import lyra.modules;
import lyra.native_packages;
import lyra.packages;
import lyra.platform;

namespace fs = std::filesystem;
namespace json = llvm::json;
using namespace lyra;

namespace {
constexpr auto usage = "lyra --help | --version\n"
                       "lyra lsp --stdio\n"
                       "lyra fmt [--check] <文件.ly>...\n"
                       "lyra import-c <头文件> [-I <包含目录>] [-D <宏[=值]>] [-o <新文件.ly> [--adapter <新文件.c> [--varargs <调用形状>]...]]\n"
                       "lyra new <目录> [--name <包名>] [--lib]\n"
                       "lyra check|build|run|test <包目录> [--locked] [--offline] [--diagnostics=json]\n"
                       "lyra test <包目录> [--list] [--filter <片段>] [--case <ID>]... [--format=json] [--timeout-seconds <秒>]\n"
                       "lyra build <包目录> [-o <产物>] [--release] [--debug-info] [--link <C库>] | run <包目录> [--release] [--debug-info] [--link <C库>] [-- <程序实参>...]\n"
                       "lyra lock|fetch <目录> [--locked] [--offline] | lock <目录> --update\n"
                       "lyra pack <目录> [-o <源码包>|--registry <本地目录>]";

struct Options {
    std::string_view command;
    fs::path directory;
    std::optional<fs::path> output;
    PackageOptions packages{};
    NativeBuildOptions native{};
    std::vector<std::string_view> arguments{};
    std::vector<fs::path> libraries{};
    bool listTests{}, testJson{};
    std::optional<std::string_view> testFilter{};
    std::vector<std::string_view> testCases{};
    unsigned testTimeout{};
};

Options options(const std::vector<std::string_view>& args, const std::vector<std::string_view>& arguments, bool separator) {
    auto command = args.front();
    if (command != "check" && command != "build" && command != "run" && command != "test") throw Diagnostic{"LYC0001", usage};
    if (separator && command != "run") throw Diagnostic{"LYC0001", usage};
    Options result{command, {}, {}, {}, {}, arguments};
    for (std::size_t index = 1; index < args.size(); ++index) {
        auto arg = args[index];
        if (arg == "--locked" && !result.packages.locked) result.packages.locked = true;
        else if (arg == "--offline" && !result.packages.offline) result.packages.offline = true;
        else if (arg == "--list" && command == "test" && !result.listTests) result.listTests = true;
        else if (arg == "--format=json" && command == "test" && !result.testJson) result.testJson = true;
        else if (arg == "--filter" && command == "test" && !result.testFilter && index + 1 < args.size()) result.testFilter = args[++index];
        else if (arg == "--case" && command == "test" && index + 1 < args.size()) result.testCases.push_back(args[++index]);
        else if (arg == "--timeout-seconds" && command == "test" && !result.testTimeout && index + 1 < args.size()) {
            auto value = args[++index];
            auto last = value.data() + value.size();
            auto [end, error] = std::from_chars(value.data(), last, result.testTimeout);
            if (error != std::errc{} || end != last || !result.testTimeout) throw Diagnostic{"LYC0001", "测试超时必须为正整数秒"};
        }
        else if (arg == "--release" && command != "check" && !result.native.release) result.native.release = true;
        else if (arg == "--debug-info" && command != "check" && !result.native.debugInfo) result.native.debugInfo = true;
        else if (arg == "-o" && command == "build" && !result.output && index + 1 < args.size() && !args[index + 1].starts_with('-')) result.output = fromUtf8(args[++index]);
        else if (arg == "--link" && command != "check" && index + 1 < args.size() && !args[index + 1].starts_with('-')) {
            auto path = fs::absolute(fromUtf8(args[++index]));
            if (!fs::is_regular_file(path)) throw Diagnostic{"LYI0001", "找不到原生链接输入：" + pathText(path)};
            auto name = pathText(path.filename());
            auto extension = pathText(path.extension());
            if constexpr (windows) {
                if (extension != ".obj" && extension != ".lib" && extension != ".a")
                    throw Diagnostic{"LYC0001", "--link 只接受 x64 COFF 目标文件 .obj、静态库或导入库 .lib/.a：" + name};
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

void compile(const Options& opts, Compilation program, bool library, const fs::path& output, std::vector<Diagnostic>& warnings,
    const PackageGraph* graph = nullptr, std::string_view entry = {}) {
    auto module = check(std::move(program.syntax), program.sources, !library);
    warnings.insert(warnings.end(), module.warnings.begin(), module.warnings.end());
    if (library) throw Diagnostic{"LYC0002", "库没有可执行入口；使用 lyra check 或 lyra test 检查库"};
    if (fs::exists(output)) for (const auto& input : program.macroInputs) {
        if (fs::equivalent(input, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖宏或其只读输入：" + pathText(output)};
    }
    auto native = graph ? buildNativePackages(*graph, entry, output, opts.native) : NativeLibraries{};
    auto libraries = opts.libraries;
    libraries.insert(libraries.end(), native.libraries.begin(), native.libraries.end());
    build(module, program.sources, output, opts.native, libraries);
}

struct TestEntry { std::string id, name, packageId; fs::path root, path; };

int runTests(const Options& opts, bool jsonDiagnostics, AnalysisDiagnostics& diagnostics) {
    if (!fs::is_regular_file(opts.directory / "lyra.json5")) throw Diagnostic{"LYK0001", "测试需要包清单 lyra.json5"};
    std::vector<TestEntry> entries;
    auto append = [&](const fs::path& workspace, const fs::path& root, const std::string& name, const std::string& packageId) {
        for (const auto& path : testEntries(root)) {
            auto id = pathText(path.lexically_relative(workspace));
            auto label = name + '/' + pathText(path.filename());
            entries.push_back({std::move(id), std::move(label), packageId, root, path});
        }
    };
    PackageGraph graph;
    if (opts.listTests) {
        auto local = localEntryPackages(opts.directory);
        for (const auto& package : local.packages) append(local.workspace, package.root, package.name, {});
    } else {
        graph = resolvePackages(opts.directory, opts.packages);
        if (!jsonDiagnostics) for (const auto& warning : graph.warnings) std::cerr << "warning: " << warning << '\n';
        if (!graph.missingOutputs.empty()) throw Diagnostic{"LYK0007", "未执行构建脚本，" + graph.missingOutputs.front()};
        auto ids = graph.entry.empty() ? graph.members : std::vector<std::string>{graph.entry};
        for (const auto& id : ids) {
            const auto& package = *std::ranges::find(graph.packages, id, &ResolvedPackage::id);
            append(graph.workspace, package.root, package.name, id);
        }
    }
    std::ranges::sort(entries, {}, &TestEntry::id);
    for (auto id : opts.testCases) if (std::ranges::find(entries, id, &TestEntry::id) == entries.end())
        throw Diagnostic{"LYC0003", "找不到测试入口：" + std::string(id)};
    std::erase_if(entries, [&](const TestEntry& entry) {
        return (!opts.testCases.empty() && std::ranges::find(opts.testCases, entry.id) == opts.testCases.end()) ||
            (opts.testFilter && !entry.name.contains(*opts.testFilter) && !entry.id.contains(*opts.testFilter));
    });
    auto readOutput = [](const fs::path& path, bool& truncated) {
        constexpr std::size_t limit = 1024 * 1024;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::string{};
        std::string text(limit + 1, '\0');
        input.read(text.data(), text.size());
        auto count = static_cast<std::size_t>(input.gcount());
        truncated |= count > limit;
        text.resize(std::min(count, limit));
        return llvm::json::fixUTF8(text);
    };
    json::Array results;
    for (const auto& entry : entries) {
        json::Object item{{"id", entry.id}, {"name", entry.name}, {"file", pathText(entry.path)}, {"status", "listed"}};
        if (opts.listTests) {
            if (!opts.testJson) std::cout << entry.id << '\n';
        } else {
            auto start = std::chrono::steady_clock::now();
            auto name = entry.path.stem();
            if constexpr (windows) name += ".exe";
            auto binary = entry.root / "build" / "test" / (opts.native.release ? "release" : "debug") / name;
            const char* phase = "build";
            auto failure = [&](Diagnostic error) {
                json::Object detail{{"code", error.code}, {"message", error.message}, {"phase", phase}};
                if (error.site) {
                    detail["file"] = error.site->file;
                    detail["line"] = error.site->start.line;
                    detail["column"] = error.site->start.column;
                }
                item["status"] = "failed";
                item["error"] = std::move(detail);
                diagnostics.add(std::move(error));
            };
            try {
                compile(opts, loadProgram(graph, entry.packageId, entry.path), false, binary, diagnostics.warnings, &graph, entry.packageId);
                phase = "run";
                int status;
                if (opts.testJson) {
                    StagingDirectory staging(binary.parent_path());
                    NativeOutput streams{staging.path / "stdout", staging.path / "stderr"};
                    std::optional<Diagnostic> problem;
                    try { status = execute(binary, {}, &streams, opts.testTimeout); }
                    catch (const Diagnostic& error) { problem = error; }
                    bool truncated = false;
                    item["stdout"] = readOutput(streams.standard, truncated);
                    item["stderr"] = readOutput(streams.error, truncated);
                    if (truncated) item["outputTruncated"] = true;
                    if (problem) throw *problem;
                } else status = execute(binary, {}, nullptr, opts.testTimeout);
                item["exitCode"] = status;
                if (status) throw Diagnostic{"LYC0003", pathText(entry.path) + " 测试退出码：" + std::to_string(status)};
                item["status"] = "passed";
                if (!opts.testJson) std::cout << "ok " << entry.name << '\n';
            } catch (const Diagnostic& error) { failure(error); }
            catch (const fs::filesystem_error& error) { failure({"LYI0001", error.what()}); }
            item["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        }
        if (opts.testJson) results.emplace_back(std::move(item));
    }
    if (opts.testJson) std::cout << llvm::formatv("{0}\n", json::Value(json::Object{{"tests", std::move(results)}})).str();
    if (!opts.listTests && entries.empty()) throw Diagnostic{"LYC0003", "没有匹配的 tests/*.ly 测试入口"};
    return !diagnostics.errors.empty();
}

int project(const Options& opts, bool json, AnalysisDiagnostics& diagnostics) {
    if (opts.command == "test") return runTests(opts, json, diagnostics);
    auto analyze = [&](bool library, auto&& load) {
        AnalysisDiagnostics current;
        try {
            auto program = load(current);
            if (!program.incompleteDeclaration) checkForDiagnostics(std::move(program.syntax), program.sources, !library, current);
        } catch (const Diagnostic& error) { current.add(error); }
        for (auto& error : current.errors) diagnostics.errors.push_back(std::move(error));
        for (auto& warning : current.warnings) diagnostics.warnings.push_back(std::move(warning));
    };
    auto binary = [&](const fs::path& root) { return opts.output.value_or(executablePath(root / "build" / (opts.native.release ? "release" : "debug") / "app")); };
    if (!fs::exists(opts.directory / "lyra.json5")) {
        if (opts.packages.locked || opts.packages.offline) throw Diagnostic{"LYK0001", "此操作需要包清单 lyra.json5"};
        if (opts.command == "check") {
            analyze(false, [&](auto& current) { return loadLooseProgram(loadSources(opts.directory), current); });
            return !diagnostics.errors.empty();
        }
        auto sources = loadSources(opts.directory);
        auto module = parse(sources);
        if (!module.imports.empty()) throw at(sources[module.imports.front().file], module.imports.front().path.front().span, "LYN0003", "使用 import 需要包清单 lyra.json5");
        auto output = binary(opts.directory);
        compile(opts, {std::move(sources), std::move(module)}, false, output, diagnostics.warnings);
        return opts.command == "build" ? 0 : execute(output, opts.arguments);
    }
    auto graph = resolvePackages(opts.directory, opts.packages);
    if (!json) for (const auto& warning : graph.warnings) std::cerr << "warning: " << warning << '\n';
    if (!graph.missingOutputs.empty()) throw Diagnostic{"LYK0007", "未执行构建脚本，" + graph.missingOutputs.front()};
    if (graph.entry.empty() && opts.command != "check") throw Diagnostic{"LYC0002", "请指定要运行的 workspace 成员目录"};
    if (opts.output && fs::exists(*opts.output)) {
        for (const auto& package : graph.packages) if (fs::equivalent(*opts.output, package.root / "lyra.json5")) throw Diagnostic{"LYI0001", "输出不能覆盖包清单"};
        if (fs::equivalent(*opts.output, graph.workspace / "lyra.lock")) throw Diagnostic{"LYI0001", "输出不能覆盖包锁"};
    }
    auto entries = graph.entry.empty() ? graph.members : std::vector<std::string>{graph.entry};
    for (const auto& id : entries) {
        const auto& package = *std::ranges::find(graph.packages, id, &ResolvedPackage::id);
        if (opts.command == "check") {
            analyze(package.library, [&](auto& current) { return loadProgram(graph, id, {}, nullptr, nullptr, false, &current); });
            continue;
        }
        auto output = binary(package.root);
        compile(opts, loadProgram(graph, id), package.library, output, diagnostics.warnings, &graph, id);
        return opts.command == "build" ? 0 : execute(output, opts.arguments);
    }
    return !diagnostics.errors.empty();
}

int format(const std::vector<std::string_view>& args) {
    std::vector<fs::path> files;
    bool checkOnly = false;
    for (auto arg : std::views::drop(args, 1)) {
        if (arg == "--check" && !checkOnly) checkOnly = true;
        else if (!arg.empty() && !arg.starts_with('-')) files.push_back(fs::absolute(fromUtf8(arg)));
        else throw Diagnostic{"LYC0001", usage};
    }
    if (files.empty()) throw Diagnostic{"LYC0001", usage};
    for (const auto& file : files) formatFile(file, checkOnly);
    return 0;
}

int run(std::vector<std::string_view> args) {
    auto separator = std::ranges::find(args, "--");
    bool forwards = separator != args.end();
    std::vector<std::string_view> arguments;
    if (forwards) { arguments.assign(separator + 1, args.end()); args.erase(separator, args.end()); }
    bool json = std::erase(args, "--diagnostics=json") > 0;
    AnalysisDiagnostics diagnostics;
    auto failure = [&](Diagnostic error) {
        diagnostics.errors.push_back(std::move(error));
        reportDiagnostics(diagnostics.warnings, diagnostics.errors, json);
    };
    try {
        if (args.size() == 1 && args[0] == "--version" && !forwards) { std::cout << "lyra " LYRA_TOOLCHAIN_VERSION "\n"; return 0; }
        if (args.size() == 1 && args[0] == "--help") { std::cout << usage << '\n'; return 0; }
        if (args.empty()) throw Diagnostic{"LYC0001", usage};
        if (args.size() == 2 && args[0] == "lsp" && args[1] == "--stdio" && !forwards && !json) return languageServer();
        if (args[0] == "import-c") {
            if (forwards) throw Diagnostic{"LYC0001", usage};
            return importC(args, json);
        }
        int code;
        if (args[0] == "fmt") {
            if (forwards) throw Diagnostic{"LYC0001", usage};
            code = format(args);
        } else if (args[0] == "new" || args[0] == "lock" || args[0] == "fetch" || args[0] == "pack") {
            if (forwards) throw Diagnostic{"LYC0001", usage};
            code = packageCommand(args);
        } else code = project(options(args, arguments, forwards), json, diagnostics);
        diagnostics.sort();
        reportDiagnostics(diagnostics.warnings, diagnostics.errors, json);
        return code;
    } catch (const Diagnostic& error) { failure(error); }
    catch (const PackageError& error) { failure({error.code, error.message}); }
    catch (const fs::filesystem_error& error) { failure({"LYI0001", error.what()}); }
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
