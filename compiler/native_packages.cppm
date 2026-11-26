module;
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>

export module lyra.native_packages;
import lyra.build;
import lyra.packages;
import lyra.platform;
import lyra.source;

namespace lyra {
namespace fs = std::filesystem;

export struct NativeLibraries {
    std::unique_ptr<StagingDirectory> staging;
    std::vector<fs::path> libraries;
};

namespace {

void runTool(std::string_view tool, const std::vector<std::string>& command, const fs::path& stderrFile, const std::string& label) {
    auto binary = executablePath(sdkRoot() / "libexec/lyra" / fromUtf8(tool));
    if (!fs::is_regular_file(binary)) throw Diagnostic{"LYB0004", "SDK 缺少 C 工具：" + pathText(binary)};
    auto binaryText = pathText(binary);
    auto hostLibraries = pathText(sdkRoot() / "lib/lyra/host");
    std::string_view program = windows ? std::string_view(binaryText) : linuxLoader;
    std::vector<llvm::StringRef> arguments{program};
    if constexpr (!windows) arguments.insert(arguments.end(), {"--library-path", hostLibraries, binaryText});
    for (const auto& argument : command) arguments.emplace_back(argument);
    auto log = pathText(stderrFile);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef{}, std::nullopt, llvm::StringRef(log)};
    std::string reason;
    auto status = llvm::sys::ExecuteAndWait(program, arguments, std::nullopt, redirects, 120, 0, &reason);
    if (status == 0) return;
    if (auto captured = llvm::MemoryBuffer::getFile(log)) reason += (*captured)->getBuffer().str();
    throw Diagnostic{"LYB0004", label + "（退出码 " + std::to_string(status) + "）：\n" + reason};
}

void verifyArchive(std::string_view bytes, const std::string& where) {
    auto fail = [&](const std::string& detail) { throw Diagnostic{"LYB0004", "原生归档 " + where + "：" + detail}; };
    auto archive = llvm::object::Archive::create(llvm::MemoryBufferRef(bytes, where));
    if (!archive) fail(llvm::toString(archive.takeError()));
    if ((*archive)->isThin()) fail("不接受依赖外部目标文件的 thin archive");
    std::string invalid;
    llvm::Error error = llvm::Error::success();
    for (const auto& member : (*archive)->children(error)) {
        auto binary = member.getAsBinary();
        if (!binary) { invalid = llvm::toString(binary.takeError()); break; }
        const auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
        if (!object || !(windows ? object->isCOFF() : object->isELF()) || !object->isRelocatableObject() || object->getArch() != llvm::Triple::x86_64) {
            invalid = "只接受当前目标的 x64 可重定位 C 目标文件，不接受导入库和 LLVM bitcode";
            break;
        }
    }
    if (error) fail(llvm::toString(std::move(error)));
    if (!invalid.empty()) fail(invalid);
}

std::vector<const ResolvedPackage*> nativeOrder(const PackageGraph& graph, std::string_view entry) {
    std::map<std::string_view, const ResolvedPackage*> packages;
    for (const auto& package : graph.packages) packages.emplace(package.id, &package);
    std::set<std::string_view> visited;
    std::vector<const ResolvedPackage*> order;
    auto visit = [&](auto&& self, std::string_view id) -> void {
        if (!visited.insert(id).second) return;
        auto* package = packages.at(id);
        for (const auto& [alias, dependency] : package->dependencies) self(self, dependency);
        if (!package->native.sources.empty() || !package->native.archives.empty()) order.push_back(package);
    };
    visit(visit, entry);
    std::ranges::reverse(order);
    return order;
}

}

export NativeLibraries buildNativePackages(const PackageGraph& graph, std::string_view entry, const fs::path& output, NativeBuildOptions options) {
    NativeLibraries result;
    auto order = nativeOrder(graph, entry);
    if (order.empty()) return result;
    result.staging = std::make_unique<StagingDirectory>(fs::absolute(output).parent_path());
    const auto& stage = result.staging->path;
    auto log = stage / "tools.log";
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto& package = *order[index];
        const auto& native = package.native;
        auto root = stage / ("pkg" + std::to_string(index));
        auto archive = native.archives.find(nativePackageTarget);
        if (!native.archives.empty() && archive == native.archives.end())
            throw Diagnostic{"LYB0004", package.name + " 未声明目标 " + nativePackageTarget + " 的静态库"};
        std::size_t total = 0;
        std::set<fs::path> staged;
        auto copy = [&](const fs::path& file, bool binary) {
            if (fs::exists(output) && fs::equivalent(output, package.root / file))
                throw Diagnostic{"LYI0001", "输出不能覆盖原生输入：" + pathText(package.root / file)};
            if (!staged.insert(file).second) {
                if (binary) throw Diagnostic{"LYB0004", "同一个原生文件不能同时用作头文件和静态库：" + pathText(package.root / file)};
                return;
            }
            auto data = readNativePackageFile(package.root, file);
            total += data.size();
            if (total > 64 * 1024 * 1024) throw Diagnostic{"LYB0004", package.name + " 的原生输入超过 64 MiB"};
            if (binary) verifyArchive(data, pathText(package.root / file));
            auto target = root / file;
            fs::create_directories(target.parent_path());
            std::ofstream stream(target, std::ios::binary);
            stream.write(data.data(), data.size());
            stream.close();
            if (!stream) throw Diagnostic{"LYB0004", "无法写入 C 构建暂存文件：" + pathText(target)};
        };
        if (!native.sources.empty()) {
            for (const auto& file : native.sources) copy(file, false);
            for (const auto& file : native.headers) copy(file, false);
            auto flags = nativeCFlags();
            flags.insert(flags.end(), {"--no-default-config", "-fno-autolink", "-fintegrated-as", "-fno-crash-diagnostics", "-fPIC", "-Werror=date-time", options.release ? "-O2" : "-O0",
                "-fdebug-compilation-dir=.",
                "-ffile-prefix-map=" + pathText(root) + '=' + pathText(package.root), "-I", pathText(root / "native")});
            if (!options.release || options.debugInfo) {
                flags.emplace_back("-g");
                if constexpr (windows) flags.emplace_back("-gcodeview");
            }
            for (const auto& directory : native.includeDirs) {
                if (!fs::is_directory(root / directory)) throw Diagnostic{"LYB0004", "没有已声明头文件的包含目录：" + pathText(package.root / directory)};
                flags.insert(flags.end(), {"-I", pathText(root / directory)});
            }
            std::vector<std::string> archived{"rcsD", pathText(root / "libnative.a")};
            for (std::size_t source = 0; source < native.sources.size(); ++source) {
                auto object = root / ("source" + std::to_string(source) + (windows ? ".obj" : ".o"));
                const auto& file = native.sources[source];
                auto command = flags;
                command.insert(command.end(), {"-c", pathText(root / file), "-o", pathText(object)});
                runTool("lyra-clang", command, log, "C 编译失败：" + pathText(package.root / file));
                archived.push_back(pathText(object));
            }
            runTool("llvm-ar", archived, log, "归档 C 目标文件失败：" + package.name);
            result.libraries.push_back(root / "libnative.a");
        }
        if (archive != native.archives.end()) {
            for (const auto& file : archive->second) {
                copy(file, true);
                result.libraries.push_back(root / file);
            }
        }
    }
    return result;
}

}
