module;
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <algorithm>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <system_error>
#include <vector>

module lyra.build;
import lyra.check;
import lyra.lowering;
import lyra.platform;

namespace lyra {
namespace fs = std::filesystem;
namespace {

void linkNative(const fs::path& sdk, const std::vector<std::string>& options, const std::string& log) {
    auto linkerPath = sdk / "libexec/lyra" / (windows ? "lld-link.exe" : "ld.lld");
    auto linker = pathText(linkerPath);
    if (!fs::is_regular_file(linkerPath)) throw Diagnostic{"LYB0002", "Lyra SDK 缺少链接器：" + linker};
    auto hostLibraries = pathText(sdk / "lib/lyra/host");
    std::string_view program = windows ? std::string_view(linker) : linuxLoader;
    std::vector<llvm::StringRef> args{program};
    if constexpr (!windows) args.insert(args.end(), {"--library-path", hostLibraries, linker});
    for (const auto& option : options) args.emplace_back(option);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef{}, llvm::StringRef{}, llvm::StringRef(log)};
    std::string reason;
    int status = llvm::sys::ExecuteAndWait(program, args, std::nullopt, redirects, 0, 0, &reason);
    if (status != 0) {
        auto captured = llvm::MemoryBuffer::getFile(log);
        if (captured) reason += (*captured)->getBuffer().str();
        throw Diagnostic{"LYB0002", "LLD 链接失败（退出码 " + std::to_string(status) + "）：" + reason};
    }
}

constexpr auto targetCpu = "x86-64";

std::unique_ptr<llvm::TargetMachine> nativeMachine(bool release) {
    static const auto& target = []() -> const llvm::Target& {
        if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter())
            throw Diagnostic{"LYB0001", "LLVM 不支持 SDK 的原生目标"};
        std::string reason;
        auto* target = llvm::TargetRegistry::lookupTarget(nativeTriple, reason);
        if (!target) throw Diagnostic{"LYB0001", "无法选择 LLVM 目标：" + reason};
        return *target;
    }();
    return std::unique_ptr<llvm::TargetMachine>(target.createTargetMachine(nativeTriple, targetCpu, "", {}, llvm::Reloc::PIC_,
        std::nullopt, release ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::None));
}

std::string objectKey(const llvm::Module& module, bool release) {
    std::string ir;
    llvm::raw_string_ostream stream(ir);
    module.print(stream, nullptr);
    llvm::SHA256 digest;
    digest.update("lyra-native-v2\n" LLVM_VERSION_STRING "\n");
    digest.update(nativeTriple);
    digest.update("\n");
    digest.update(targetCpu);
    digest.update(release ? "\nO2 PIC\n" : "\nO0 PIC\n");
    digest.update(ir);
    return llvm::toHex(digest.final(), true);
}

void emitObject(llvm::Module& module, llvm::TargetMachine& machine, const fs::path& object, bool release) {
    if (release) {
        llvm::LoopAnalysisManager loops;
        llvm::FunctionAnalysisManager functions;
        llvm::CGSCCAnalysisManager callGraph;
        llvm::ModuleAnalysisManager modules;
        llvm::PassBuilder pipeline(&machine);
        pipeline.registerModuleAnalyses(modules);
        pipeline.registerCGSCCAnalyses(callGraph);
        pipeline.registerFunctionAnalyses(functions);
        pipeline.registerLoopAnalyses(loops);
        pipeline.crossRegisterProxies(loops, functions, callGraph, modules);
        pipeline.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(module, modules);
    }
    std::error_code error;
    llvm::raw_fd_ostream stream(pathText(object), error, llvm::sys::fs::OF_None);
    if (error) throw fs::filesystem_error("open", object, error);
    llvm::legacy::PassManager codegen;
    if (machine.addPassesToEmitFile(codegen, stream, nullptr, llvm::CodeGenFileType::ObjectFile))
        throw Diagnostic{"LYB0001", "LLVM 目标不支持输出原生目标文件"};
    codegen.run(module);
    stream.close();
    if (stream.has_error()) {
        auto writeError = stream.error();
        stream.clear_error();
        throw fs::filesystem_error("write", object, writeError);
    }
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

std::vector<NativeArtifact> emitExecutable(llvm::Module& module, llvm::TargetMachine& machine, const fs::path& output, bool release, bool symbols,
                    const std::vector<fs::path>& libraries) {
    auto sdk = sdkRoot();
    auto lib = sdk / "lib";
    auto target = lib / "lyra/target";
    StagingDirectory staging(output.parent_path());
    auto object = staging.path / (windows ? "module.obj" : "module.o");
    auto objectText = pathText(object);
    auto executable = executablePath(staging.path / "linked" / output.filename());
    fs::create_directory(executable.parent_path());
    auto log = pathText(staging.path / "link.log");
    auto cacheKey = objectKey(module, release);
    auto cacheBase = cacheOverride();
    auto cacheRoot = cacheBase.empty() ? output.parent_path() / ".lyra-cache" / "objects" : cacheBase / "compiler" / "objects";
    auto cacheObject = cacheRoot / fromUtf8(cacheKey + ".object");
    bool cacheMiss = !restoreObject(cacheObject, cacheKey, objectText);
    if (cacheMiss) emitObject(module, machine, object, release);

    std::vector<std::string> link;
    auto append = [&](const fs::path& directory, std::initializer_list<const char*> filenames) {
        for (auto name : filenames) {
            auto input = directory / name;
            if (!fs::is_regular_file(input)) throw Diagnostic{"LYB0002", "Lyra SDK 缺少链接库：" + pathText(input)};
            link.push_back(pathText(input));
        }
    };
    fs::path stagedSymbols;
    if constexpr (windows) {
        link = {"/lldmingw", "/lldignoreenv", "/machine:x64", "/subsystem:console", "/entry:mainCRTStartup",
            "/nodefaultlib", "/alternatename:__chkstk=___chkstk_ms", "/out:" + pathText(executable)};
        if (symbols) {
            llvm::SmallString<128> name;
            llvm::sys::fs::createUniquePath(pathText(executable.stem()) + ".%%%%%%%%%%%%%%%%.pdb", name, false);
            stagedSymbols = executable.parent_path() / fromUtf8(name.str().str());
            link.insert(link.end(), {"/debug", "/pdb:" + pathText(stagedSymbols), "/pdbaltpath:%_PDB%"});
        }
        append(target, {"crt2u.o", "crtbegin.o"});
    } else {
        link = {"-m", "elf_x86_64", "-pie", "--eh-frame-hdr", "--build-id", "-z", "relro", "--hash-style=gnu",
            "--dynamic-linker", std::string(linuxLoader), "--as-needed", "-o", pathText(executable)};
        append(target, {"Scrt1.o", "crti.o", "crtbeginS.o"});
    }
    link.push_back(objectText);
    for (const auto& library : libraries) link.push_back(pathText(library));
    append(lib, {"liblyraRuntime.a", "libgc.a", "libuv.a", "libllhttp.a", "libjansson.a", "libsqlite3.a"});
    auto uses = [&](llvm::StringRef prefix) {
        return std::ranges::any_of(module, [&](const auto& fn) {
            return fn.isDeclaration() && !fn.use_empty() && fn.getName().starts_with(prefix);
        });
    };
    auto client = uses("lyraHttpClient");
    auto serverTls = uses("lyraHttpTls");
    auto compression = uses("lyraGzip");
    auto tls = client || serverTls;
    if (compression) append(lib, {"liblyraCompression.a"});
    if (client) {
        append(lib, {"liblyraHttpClient.a", "libcurl.a", "libcares.a"});
    }
    if constexpr (windows) {
        if (client || compression) append(lib, {"libzs.a"});
    } else if (compression) append(lib, {"libz.a"});
    if (serverTls) append(lib, {"liblyraHttpTls.a"});
    if (tls) append(lib, {"libssl.a", "libcrypto.a"});
    if constexpr (windows) {
        if (tls) append(target, {"libbcrypt.a", "libcrypt32.a", "libgdi32.a"});
        append(target, {"libc++.a", "libunwind.a", "libmingw32.a", "libmingwex.a", "libmoldname.a", "libmsvcrt.a",
            "libclang_rt.builtins.a", "libws2_32.a", "libiphlpapi.a", "libuserenv.a", "libpsapi.a", "libadvapi32.a",
            "libuser32.a", "libdbghelp.a", "libole32.a", "libshell32.a", "libkernel32.a", "crtend.o"});
    } else {
        append(target, {"libstdc++.a", "libgcc.a", "libgcc_eh.a", "libm.so.6", "libc.so.6", "libc_nonshared.a", "crtendS.o", "crtn.o"});
    }
    linkNative(sdk, link, log);
    if (cacheMiss) storeObject(cacheObject, cacheKey, objectText);
    std::vector<NativeArtifact> artifacts{{output, fileContentKey(executable)}};
    if constexpr (windows) {
        if (symbols) {
            if (!fs::is_regular_file(stagedSymbols)) throw Diagnostic{"LYB0002", "LLD 未生成预期的 CodeView 符号文件：" + pathText(stagedSymbols)};
            auto outputSymbols = output.parent_path() / stagedSymbols.filename();
            artifacts.push_back({outputSymbols, fileContentKey(stagedSymbols)});
            publishExecutable(stagedSymbols, outputSymbols, false);
            try { publishExecutable(executable, output); }
            catch (...) {
                std::error_code ignored;
                fs::remove(outputSymbols, ignored);
                throw;
            }
            return artifacts;
        }
    }
    publishExecutable(executable, output);
    return artifacts;
}

}

std::optional<std::string> fileContentKey(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return {};
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    llvm::BLAKE3 digest;
    char bytes[64 * 1024];
    while (input.read(bytes, sizeof(bytes)) || input.gcount() != 0)
        digest.update(llvm::StringRef(bytes, static_cast<std::size_t>(input.gcount())));
    if (!input.eof() || input.bad()) return {};
    return llvm::toHex(digest.final(), true);
}

std::vector<NativeArtifact> build(const CheckedModule& functions, const std::vector<Source>& sources, const fs::path& output,
        NativeBuildOptions options, const std::vector<fs::path>& libraries) {
    if (fs::exists(output)) {
        for (const auto& source : sources) {
            if (!source.origin && fs::equivalent(source.path, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖输入源码：" + pathText(output)};
        }
        for (const auto& library : libraries) if (fs::equivalent(library, output)) throw Diagnostic{"LYI0001", "输出路径不能覆盖原生链接输入：" + pathText(output)};
    }
    llvm::LLVMContext context;
    const bool symbols = !options.release || options.debugInfo;
    auto machine = nativeMachine(options.release);
    auto module = lower(functions, sources, context, machine->createDataLayout(), options.release, symbols);
    llvm::ModuleAnalysisManager analyses;
    llvm::GlobalDCEPass{}.run(*module, analyses);
    return emitExecutable(*module, *machine, fs::absolute(output).lexically_normal(), options.release, symbols, libraries);
}

int execute(const fs::path& executable, const std::vector<std::string_view>& arguments, const NativeOutput* output, unsigned timeoutSeconds) {
    auto path = pathText(fs::absolute(executable));
    std::vector<llvm::StringRef> args{path};
    for (auto arg : arguments) args.emplace_back(arg);
    std::string reason;
    auto standard = output ? pathText(output->standard) : std::string{};
    auto error = output ? pathText(output->error) : std::string{};
    std::optional<llvm::StringRef> redirected[] = {llvm::StringRef{}, llvm::StringRef(standard), llvm::StringRef(error)};
    int code = llvm::sys::ExecuteAndWait(path, args, std::nullopt,
        output ? llvm::ArrayRef(redirected) : llvm::ArrayRef<std::optional<llvm::StringRef>>{}, timeoutSeconds, 0, &reason);
    if (code < 0) throw Diagnostic{"LYB0003", "程序未正常退出：" + reason};
    return code;
}

}
