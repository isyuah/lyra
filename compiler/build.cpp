module;
#include <filesystem>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
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

void emitExecutable(const llvm::Module& module, const fs::path& output, bool release, const std::vector<fs::path>& libraries) {
    auto driver = llvm::sys::findProgramByName(nativeDriver);
    if (!driver) throw Diagnostic{"LYB0002", std::string("找不到 ") + nativeDriver + "；请在构建机安装并加入 PATH"};
    auto lib = sdkRoot() / "lib";
    auto runtime = lib / (windows ? "lyraRuntime.lib" : "liblyraRuntime.a");
    if (!fs::is_regular_file(runtime)) throw Diagnostic{"LYB0002", "找不到 Lyra 运行时：" + pathText(runtime)};
    StagingDirectory staging(output.parent_path());
    auto irFile = staging.path / "module.ll";
    auto ir = pathText(irFile);
    auto object = pathText(staging.path / (windows ? "module.obj" : "module.o"));
    auto executable = executablePath(staging.path / "app");
    auto log = pathText(staging.path / "clang.log");
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
    runClang(*driver, {target, release ? "-O2" : "-O0", "-c", "-x", "ir", ir, "-o", object}, log);
    std::vector<std::string> link{target, object};
    if constexpr (windows) link.insert(link.end(), {"-fuse-ld=lld", "-fms-runtime-lib=static"});
    else link.push_back("-Wl,--as-needed");
    for (const auto& library : libraries) link.push_back(pathText(library));
    link.push_back(pathText(runtime));
    if constexpr (windows) {
        for (auto name : {"gc.lib", "libuv.lib", "llhttp.lib", "jansson.lib", "sqlite3.lib"}) link.push_back(pathText(lib / name));
        link.insert(link.end(), {"-lws2_32", "-liphlpapi", "-luserenv", "-lpsapi", "-ladvapi32", "-luser32", "-ldbghelp", "-lole32", "-lshell32"});
    } else {
        for (auto name : {"libgc.a", "libuv.a", "libllhttp.a", "libjansson.a"}) link.push_back(pathText(lib / name));
        link.insert(link.end(), {"-pthread", "-ldl", "-lrt", "-lsqlite3"});
    }
    link.insert(link.end(), {"-o", pathText(executable)});
    runClang(*driver, link, log);
    publishExecutable(executable, output);
}

}

void build(const CheckedModule& functions, const std::vector<Source>& sources, const fs::path& output, bool release, const std::vector<fs::path>& libraries) {
    if (fs::exists(output)) {
        for (const auto& source : sources) {
            if (!source.origin && fs::equivalent(source.path, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖输入源码：" + pathText(output)};
        }
        for (const auto& library : libraries) if (fs::equivalent(library, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖原生链接输入：" + pathText(output)};
    }
    llvm::LLVMContext context;
    auto module = lower(functions, sources, context);
    emitExecutable(*module, fs::absolute(output), release, libraries);
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
