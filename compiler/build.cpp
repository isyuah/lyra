module;
#include <filesystem>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <system_error>
#include <vector>

module lyra.build;
import lyra.check;
import lyra.lowering;
import lyra.platform;

namespace lyra {
namespace fs = std::filesystem;
namespace {

void runClang(const std::string& driver, const std::vector<std::string>& options, const std::string& log) {
    std::vector<llvm::StringRef> args{driver};
    for (const auto& option : options) args.emplace_back(option);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef{}, llvm::StringRef{}, llvm::StringRef(log)};
    std::string reason;
    int status = llvm::sys::ExecuteAndWait(driver, args, std::nullopt, redirects, 0, 0, &reason);
    if (status != 0) {
        auto captured = llvm::MemoryBuffer::getFile(log);
        if (captured) reason += (*captured)->getBuffer().str();
        throw Diagnostic{"LYB0002", std::string(nativeDriver) + " 失败（退出码 " + std::to_string(status) + "）：" + reason};
    }
}

std::string compilerVersion(const std::string& driver, const fs::path& output, const std::string& log) {
    std::vector<llvm::StringRef> args{driver, "--version"};
    auto outputText = pathText(output);
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(outputText), llvm::StringRef(log)};
    std::string reason;
    int status = llvm::sys::ExecuteAndWait(driver, args, std::nullopt, redirects, 0, 0, &reason);
    if (status != 0) throw Diagnostic{"LYB0002", std::string(nativeDriver) + " 无法报告工具链版本：" + reason};
    auto version = llvm::MemoryBuffer::getFile(pathText(output));
    if (!version) throw fs::filesystem_error("read compiler version", output, version.getError());
    return (*version)->getBuffer().str();
}

std::string objectChecksum(llvm::StringRef key, llvm::StringRef bytes) {
    llvm::SHA256 digest;
    digest.update(key);
    digest.update(bytes);
    return llvm::toHex(digest.final(), true);
}

bool writeObject(llvm::StringRef path, llvm::StringRef bytes, llvm::StringRef checksum = {}) {
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_None);
    if (error) return false;
    if (!checksum.empty()) stream << checksum << '\n';
    stream << bytes;
    stream.close();
    if (!stream.has_error()) return true;
    stream.clear_error();
    return false;
}

bool restoreObject(const fs::path& cache, llvm::StringRef key, llvm::StringRef object) {
    std::error_code error;
    if (!fs::is_regular_file(cache, error)) return false;
    auto input = llvm::MemoryBuffer::getFile(pathText(cache));
    if (!input) return false;
    auto [checksum, bytes] = (*input)->getBuffer().split('\n');
    return checksum.size() == 64 && !bytes.empty() && checksum == objectChecksum(key, bytes) && writeObject(object, bytes);
}

void storeObject(const fs::path& cache, llvm::StringRef key, llvm::StringRef object) {
    auto input = llvm::MemoryBuffer::getFile(object);
    if (!input) return;
    auto bytes = (*input)->getBuffer();
    try {
        StagingDirectory staging(cache.parent_path());
        auto pending = staging.path / cache.filename();
        if (writeObject(pathText(pending), bytes, objectChecksum(key, bytes))) {
            std::error_code ignored;
            fs::rename(pending, cache, ignored);
        }
    } catch (const fs::filesystem_error&) {
        return;
    }
}

void emitExecutable(const llvm::Module& module, const fs::path& output, bool release, bool symbols, const std::vector<fs::path>& libraries) {
    auto driver = llvm::sys::findProgramByName(nativeDriver);
    if (!driver) throw Diagnostic{"LYB0002", std::string("找不到 ") + nativeDriver + "；请在构建机安装并加入 PATH"};
    auto lib = sdkRoot() / "lib";
    auto runtime = lib / (windows ? "lyraRuntime.lib" : "liblyraRuntime.a");
    if (!fs::is_regular_file(runtime)) throw Diagnostic{"LYB0002", "找不到 Lyra 运行时：" + pathText(runtime)};
    StagingDirectory staging(output.parent_path());
    auto irFile = staging.path / "module.ll";
    auto ir = pathText(irFile);
    auto object = pathText(staging.path / (windows ? "module.obj" : "module.o"));
    auto executable = executablePath(staging.path / "linked" / output.filename());
    fs::create_directory(executable.parent_path());
    auto log = pathText(staging.path / "clang.log");
    auto versionFile = staging.path / "clang.version";
    std::error_code error;
    llvm::raw_fd_ostream stream(ir, error, llvm::sys::fs::OF_None);
    if (error) throw fs::filesystem_error("open", irFile, error);
    module.print(stream, nullptr);
    stream.close();
    if (stream.has_error()) {
        auto writeError = stream.error();
        stream.clear_error();
        throw fs::filesystem_error("write", irFile, writeError);
    }
    auto target = "--target=" + module.getTargetTriple();
    std::vector<std::string> compile{target, release ? "-O2" : "-O0"};
    if (symbols) {
        compile.push_back("-g");
        if constexpr (windows) compile.push_back("-gcodeview");
    }
    auto version = compilerVersion(*driver, versionFile, log);
    auto input = llvm::MemoryBuffer::getFile(ir);
    if (!input) throw fs::filesystem_error("read generated LLVM IR", irFile, input.getError());
    llvm::SHA256 digest;
    digest.update((*input)->getBuffer());
    digest.update(*driver);
    digest.update(version);
    for (const auto& option : compile) digest.update(option);
    auto cacheKey = llvm::toHex(digest.final(), true);
    auto cacheBase = cacheOverride();
    auto cacheRoot = cacheBase.empty() ? output.parent_path() / ".lyra-cache" / "objects" : cacheBase / "compiler" / "objects";
    auto cacheObject = cacheRoot / fromUtf8(cacheKey + ".object");
    bool cacheMiss = !restoreObject(cacheObject, cacheKey, object);
    if (cacheMiss) {
        compile.insert(compile.end(), {"-c", "-x", "ir", ir, "-o", object});
        runClang(*driver, compile, log);
    }
    std::vector<std::string> link{target, object};
    if (symbols) link.push_back("-g");
    if constexpr (windows) link.insert(link.end(), {"-fuse-ld=lld", "-fms-runtime-lib=static"});
    else link.push_back("-Wl,--as-needed");
    if constexpr (windows) if (symbols) link.push_back("-Wl,/pdbaltpath:%_PDB%");
    for (const auto& library : libraries) link.push_back(pathText(library));
    link.push_back(pathText(runtime));
    if constexpr (windows) {
        for (auto name : {"gc.lib", "libuv.lib", "llhttp.lib", "jansson.lib", "sqlite3.lib"}) link.push_back(pathText(lib / name));
        link.insert(link.end(), {"-lws2_32", "-liphlpapi", "-luserenv", "-lpsapi", "-ladvapi32", "-luser32", "-ldbghelp", "-lole32", "-lshell32"});
    } else {
        for (auto name : {"libgc.a", "libuv.a", "libllhttp.a", "libjansson.a"}) link.push_back(pathText(lib / name));
        link.insert(link.end(), {"-pthread", "-ldl", "-lrt", "-lsqlite3", "-lm"});
    }
    link.insert(link.end(), {"-o", pathText(executable)});
    runClang(*driver, link, log);
    if (cacheMiss) storeObject(cacheObject, cacheKey, object);
    if constexpr (windows) {
        if (symbols) {
            auto stagedSymbols = executable;
            stagedSymbols.replace_extension(".pdb");
            auto outputSymbols = output;
            outputSymbols.replace_extension(".pdb");
            if (!fs::is_regular_file(stagedSymbols)) throw Diagnostic{"LYB0002", "Clang 未生成预期的 CodeView 符号文件：" + pathText(stagedSymbols)};
            publishExecutable(stagedSymbols, outputSymbols);
        }
    }
    publishExecutable(executable, output);
}

}

void build(const CheckedModule& functions, const std::vector<Source>& sources, const fs::path& output,
        NativeBuildOptions options, const std::vector<fs::path>& libraries) {
    if (fs::exists(output)) {
        for (const auto& source : sources) {
            if (!source.origin && fs::equivalent(source.path, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖输入源码：" + pathText(output)};
        }
        for (const auto& library : libraries) if (fs::equivalent(library, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖原生链接输入：" + pathText(output)};
    }
    llvm::LLVMContext context;
    const bool symbols = !options.release || options.debugInfo;
    auto module = lower(functions, sources, context, options.release, symbols);
    emitExecutable(*module, fs::absolute(output), options.release, symbols, libraries);
}

int execute(const fs::path& executable, const std::vector<std::string_view>& arguments) {
    auto path = pathText(fs::absolute(executable));
    std::vector<llvm::StringRef> args{path};
    for (auto arg : arguments) args.emplace_back(arg);
    std::string reason;
    int code = llvm::sys::ExecuteAndWait(path, args, std::nullopt, {}, 0, 0, &reason);
    if (code < 0) throw Diagnostic{"LYB0003", "程序未正常退出：" + reason};
    return code;
}

}
