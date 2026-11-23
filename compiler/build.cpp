module;
#include <cerrno>
#include <cstdlib>
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

namespace lyra {
namespace fs = std::filesystem;
namespace {

class StagingDirectory {
public:
    explicit StagingDirectory(const fs::path& parent) {
        auto pattern = (parent / ".lyra-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw fs::filesystem_error("mkdtemp", parent, std::error_code(errno, std::generic_category()));
        path = pattern;
    }
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    ~StagingDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

void runClang(const std::string& driver, const std::vector<std::string>& options, const std::string& log) {
    std::vector<llvm::StringRef> args{driver};
    for (const auto& option : options) args.emplace_back(option);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef{}, llvm::StringRef{}, llvm::StringRef(log)};
    std::string reason;
    int status = llvm::sys::ExecuteAndWait(driver, args, std::nullopt, redirects, 0, 0, &reason);
    if (status != 0) {
        auto captured = llvm::MemoryBuffer::getFile(log);
        if (captured) reason += (*captured)->getBuffer().str();
        throw Diagnostic{"LYB0002", "clang-18 失败（退出码 " + std::to_string(status) + "）：" + reason};
    }
}

void emitExecutable(const llvm::Module& module, const fs::path& output, bool release) {
    auto driver = llvm::sys::findProgramByName("clang-18");
    if (!driver) throw Diagnostic{"LYB0002", "找不到 clang-18；请在构建机安装并加入 PATH"};
    auto runtime = fs::canonical("/proc/self/exe").parent_path().parent_path() / "lib" / "liblyraRuntime.a";
    if (!fs::is_regular_file(runtime)) throw Diagnostic{"LYB0002", "找不到 Lyra 运行时：" + runtime.string()};
    fs::create_directories(output.parent_path());
    StagingDirectory staging(output.parent_path());
    auto ir = (staging.path / "module.ll").string();
    auto object = (staging.path / "module.o").string();
    auto executable = staging.path / "app";
    auto log = (staging.path / "clang.log").string();
    std::error_code error;
    llvm::raw_fd_ostream stream(ir, error, llvm::sys::fs::OF_None);
    if (error) throw fs::filesystem_error("open", ir, error);
    module.print(stream, nullptr);
    stream.close();
    if (stream.has_error()) {
        auto writeError = stream.error();
        stream.clear_error();
        throw fs::filesystem_error("write", ir, writeError);
    }
    auto target = "--target=" + module.getTargetTriple();
    runClang(*driver, {target, release ? "-O2" : "-O0", "-c", "-x", "ir", ir, "-o", object}, log);
    auto lib = runtime.parent_path();
    runClang(*driver, {target, object, runtime.string(), (lib / "libgc.a").string(), (lib / "libuv.a").string(), (lib / "libllhttp.a").string(),
                       "-pthread", "-ldl", "-lrt", "-Wl,--as-needed", "-lsqlite3", "-o", executable.string()}, log);
    fs::rename(executable, output);
}

}

void build(const CheckedModule& functions, const std::vector<Source>& sources, const fs::path& output, bool release) {
    if (fs::exists(output)) {
        for (const auto& source : sources) {
            if (fs::equivalent(source.path, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖输入源码：" + output.string()};
        }
    }
    llvm::LLVMContext context;
    auto module = lower(functions, sources, context);
    emitExecutable(*module, fs::absolute(output), release);
}

int execute(const fs::path& executable, const std::vector<std::string_view>& arguments) {
    auto path = fs::absolute(executable).string();
    std::vector<llvm::StringRef> args{path};
    for (auto arg : arguments) args.emplace_back(arg);
    std::string reason;
    int code = llvm::sys::ExecuteAndWait(path, args, std::nullopt, {}, 0, 0, &reason);
    if (code < 0) throw Diagnostic{"LYB0003", "程序未正常退出：" + reason};
    return code;
}

}
