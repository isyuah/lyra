module;
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <llvm/ADT/StringExtras.h>
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

export module lyra.build_cache;
import lyra.build;
import lyra.packages;
import lyra.platform;
import lyra.source;

namespace lyra {
namespace fs = std::filesystem;
namespace json = llvm::json;
namespace {

std::optional<std::string> fileStamp(const fs::path& file) {
#ifdef _WIN32
    auto handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    FILE_ID_INFO identity;
    FILE_BASIC_INFO times;
    FILE_STANDARD_INFO standard;
    wchar_t filesystem[16];
    auto ok = GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof(times)) &&
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) &&
        GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 16);
    CloseHandle(handle);
    if (!ok || (std::wstring_view(filesystem) != L"NTFS" && std::wstring_view(filesystem) != L"ReFS") || times.ChangeTime.QuadPart <= 0) return {};
    return llvm::toHex(llvm::ArrayRef(identity.FileId.Identifier), true) + ':' + std::to_string(identity.VolumeSerialNumber) + ':' +
        std::to_string(standard.EndOfFile.QuadPart) + ':' + std::to_string(times.ChangeTime.QuadPart) + ':' + std::to_string(times.LastWriteTime.QuadPart);
#else
    struct stat status;
    if (stat(file.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || !status.st_ctim.tv_sec) return {};
    return std::to_string(status.st_dev) + ':' + std::to_string(status.st_ino) + ':' + std::to_string(status.st_size) + ':' +
        std::to_string(status.st_ctim.tv_sec) + ':' + std::to_string(status.st_ctim.tv_nsec) + ':' +
        std::to_string(status.st_mtim.tv_sec) + ':' + std::to_string(status.st_mtim.tv_nsec);
#endif
}

std::optional<json::Value> loadRecord(const fs::path& record) {
    std::error_code error;
    if (!fs::is_regular_file(record, error) || fs::file_size(record, error) > 8 * 1024 * 1024) return {};
    auto bytes = llvm::MemoryBuffer::getFile(pathText(record));
    if (!bytes) return {};
    auto value = json::parse((*bytes)->getBuffer());
    if (!value) { llvm::consumeError(value.takeError()); return {}; }
    auto* object = value->getAsObject();
    if (!object || object->getInteger("format") != 1) return {};
    return std::move(*value);
}

bool standaloneLinkInput(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    char magic[64];
    input.read(magic, sizeof(magic));
    auto prefix = llvm::StringRef(magic, static_cast<std::size_t>(input.gcount()));
    if (prefix.starts_with("!<thin>\n")) return false;
    switch (llvm::identify_magic(prefix)) {
        case llvm::file_magic::archive: case llvm::file_magic::elf_relocatable: case llvm::file_magic::elf_shared_object:
        case llvm::file_magic::coff_object: case llvm::file_magic::coff_import_library: return true;
        default: return false;
    }
}

json::Object positionRecord(Position position) {
    return json::Object{{"offset", position.offset}, {"line", position.line}, {"column", position.column}};
}

json::Value siteRecord(const std::optional<Site>& site) {
    if (!site) return nullptr;
    return json::Object{{"file", site->file}, {"start", positionRecord(site->start)}, {"end", positionRecord(site->end)},
        {"text", site->lineText}, {"lineStart", site->lineStartOffset}};
}

json::Object warningRecord(const Diagnostic& warning) {
    json::Array notes;
    for (const auto& note : warning.notes) notes.push_back(json::Object{{"site", siteRecord(note.site)}, {"message", note.message}});
    return json::Object{{"code", warning.code}, {"message", warning.message}, {"site", siteRecord(warning.site)},
        {"expected", warning.expected}, {"actual", warning.actual}, {"hint", warning.hint},
        {"generated", siteRecord(warning.generated)}, {"notes", std::move(notes)}};
}

bool readPosition(const json::Object* object, Position& position) {
    if (!object) return false;
    auto offset = object->getInteger("offset"), line = object->getInteger("line"), column = object->getInteger("column");
    if (!offset || *offset < 0 || !line || *line < 1 || !column || *column < 1) return false;
    position = {static_cast<std::size_t>(*offset), static_cast<std::size_t>(*line), static_cast<std::size_t>(*column)};
    return true;
}

bool readSite(const json::Value* value, std::optional<Site>& site) {
    if (!value) return false;
    if (value->getAsNull()) return true;
    auto* object = value->getAsObject();
    if (!object) return false;
    auto file = object->getString("file"), text = object->getString("text");
    auto lineStart = object->getInteger("lineStart");
    Site result;
    if (!file || !text || !lineStart || *lineStart < 0 || !readPosition(object->getObject("start"), result.start) ||
        !readPosition(object->getObject("end"), result.end) || result.start.offset > result.end.offset ||
        static_cast<uint64_t>(*lineStart) > result.start.offset || result.start.offset - *lineStart > text->size()) return false;
    result.file = file->str();
    result.lineText = text->str();
    result.lineStartOffset = static_cast<std::size_t>(*lineStart);
    site = std::move(result);
    return true;
}

bool readWarning(const json::Value& value, Diagnostic& warning) {
    auto* object = value.getAsObject();
    if (!object) return false;
    for (auto [key, output] : {std::pair{"code", &warning.code}, {"message", &warning.message}, {"expected", &warning.expected},
                              {"actual", &warning.actual}, {"hint", &warning.hint}}) {
        auto field = object->getString(key);
        if (!field) return false;
        *output = field->str();
    }
    if (!readSite(object->get("site"), warning.site) || !readSite(object->get("generated"), warning.generated)) return false;
    auto* notes = object->getArray("notes");
    if (!notes) return false;
    for (const auto& note : *notes) {
        auto* fields = note.getAsObject();
        if (!fields) return false;
        auto message = fields->getString("message");
        std::optional<Site> site;
        if (!message || !readSite(fields->get("site"), site) || !site) return false;
        warning.notes.push_back({std::move(*site), message->str()});
    }
    return true;
}

}

export class BuildCache {
public:
    BuildCache(const fs::path& directory, const PackageGraph* graph, std::string_view entry, const fs::path& output,
        NativeBuildOptions options, const std::vector<fs::path>& libraries, const fs::path& test = {}, bool rebuild = false)
        : directory(directory), graph(graph), entry(entry),
          output(fs::absolute(output).lexically_normal()), options(options), libraries(libraries), test(test) {
        llvm::BLAKE3 identity;
        identity.update(pathText(this->output));
        identity.update(entry);
        identity.update(pathText(test));
        auto base = cacheOverride();
        record = (base.empty() ? this->output.parent_path() / ".lyra-cache" : base / "compiler") /
            "builds" / (llvm::toHex(identity.final(), true) + ".json");
        auto sdkName = pathText(sdkRoot());
        auto sdkDirectory = (base.empty() ? this->output.parent_path() / ".lyra-cache" : base / "compiler") / "sdk";
        llvm::BLAKE3 sdkKey;
        sdkKey.update(sdkName);
        sdkRecord = sdkDirectory / (llvm::toHex(sdkKey.final(), true) + ".json");
        prior = loadRecord(record);
        auto sdk = rebuild ? std::optional<json::Value>{} : loadRecord(sdkRecord);
        if (sdk) if (auto* entries = sdk->getAsObject()->getObject("files")) sdkDigests = *entries;
        key = fingerprint();
    }

    bool reuse(std::vector<Diagnostic>& warnings) const {
        if (!key || !prior) return false;
        std::error_code error;
        const auto* object = prior->getAsObject();
        if (object->getString("inputs") != *key) return false;
        auto* artifacts = object->getArray("artifacts");
        const auto expected = windows && (!options.release || options.debugInfo) ? 2u : 1u;
        if (!artifacts || artifacts->size() != expected) return false;
        for (std::size_t index = 0; index < artifacts->size(); ++index) {
            auto* artifact = (*artifacts)[index].getAsObject();
            if (!artifact) return false;
            auto name = artifact->getString("file"), hash = artifact->getString("hash");
            if (!name || !hash) return false;
            auto file = fromUtf8(*name);
            if ((index == 0 && file != output) || (index != 0 && (file.parent_path() != output.parent_path() || file.extension() != ".pdb" ||
                !pathText(file.filename()).starts_with(pathText(output.stem()) + '.')))) return false;
            auto current = fileContentKey(file);
            if (!current || *current != *hash) return false;
            if constexpr (!windows) if (index == 0 && (fs::status(file, error).permissions() & fs::perms::owner_exec) == fs::perms::none) return false;
        }
        auto* saved = object->getArray("warnings");
        if (!saved) return false;
        std::vector<Diagnostic> restored;
        for (const auto& value : *saved) {
            Diagnostic warning;
            if (!readWarning(value, warning)) return false;
            restored.push_back(std::move(warning));
        }
        warnings.insert(warnings.end(), std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
        return true;
    }

    void store(const std::vector<NativeArtifact>& files, std::span<const Diagnostic> warnings) {
        if (!key) return;
        json::Array artifacts, messages;
        for (const auto& [file, hash] : files) {
            if (!hash) return;
            artifacts.push_back(json::Object{{"file", pathText(file)}, {"hash", *hash}});
        }
        if (fingerprint() != key) return;
        for (const auto& warning : warnings) messages.push_back(warningRecord(warning));
        writeRecord(record, json::Object{{"format", 1}, {"inputs", *key}, {"artifacts", std::move(artifacts)}, {"warnings", std::move(messages)}});
        if (sdkChanged) writeRecord(sdkRecord, json::Object{{"format", 1}, {"files", std::move(sdkDigests)}});
    }

private:
    fs::path directory;
    const PackageGraph* graph;
    std::string entry;
    fs::path output;
    NativeBuildOptions options;
    std::vector<fs::path> libraries;
    fs::path test, record, sdkRecord;
    std::optional<json::Value> prior;
    json::Object sdkDigests;
    bool sdkChanged{};
    std::optional<std::string> key;

    static void writeRecord(const fs::path& path, json::Object value) {
        try {
            StagingDirectory stage(path.parent_path());
            auto pending = stage.path / "record.json";
            std::ofstream stream(pending, std::ios::binary);
            stream << llvm::formatv("{0}\n", json::Value(std::move(value))).str();
            stream.close();
            if (stream) publishExecutable(pending, path);
        } catch (const fs::filesystem_error&) {
            return;
        }
    }

    std::optional<std::string> sdkDigest(const fs::path& file) {
        auto before = fileStamp(file);
        auto name = pathText(file);
        if (before) if (auto* prior = sdkDigests.getObject(name); prior && prior->getString("stamp") == *before) {
            if (auto hash = prior->getString("hash"); hash && hash->size() == 64) return hash->str();
        }
        auto digest = fileContentKey(file);
        if (digest && before && fileStamp(file) == before) {
            sdkDigests[name] = json::Object{{"stamp", *before}, {"hash", *digest}};
            sdkChanged = true;
        }
        return digest;
    }

    std::optional<std::string> fingerprint() {
        try {
            std::set<fs::path> files;
            auto add = [&](const fs::path& path) { files.insert(fs::absolute(path).lexically_normal()); };
            auto tree = [&](const fs::path& path, bool sourceOnly, bool recursive = true) {
                add(path);
                if (!fs::is_directory(path)) return;
                for (auto iterator = fs::recursive_directory_iterator(path); iterator != fs::recursive_directory_iterator{}; ++iterator) {
                    if (!recursive && iterator->is_directory()) iterator.disable_recursion_pending();
                    if (iterator->is_symlink() || (iterator->is_regular_file() && (!sourceOnly || iterator->path().extension() == ".ly"))) add(iterator->path());
                }
            };
            add(directory / "lyra.json5");
            if (graph) {
                add(graph->workspace / "lyra.json5");
                add(graph->workspace / "lyra.lock");
                std::map<std::string_view, const ResolvedPackage*> packages;
                for (const auto& package : graph->packages) packages.emplace(package.id, &package);
                std::vector<std::string_view> pending{entry};
                std::set<std::string_view> visited;
                while (!pending.empty()) {
                    auto id = pending.back();
                    pending.pop_back();
                    if (!visited.insert(id).second) continue;
                    const auto& package = *packages.at(id);
                    add(package.root / "lyra.json5");
                    tree(package.root / "src", true);
                    for (const auto& [alias, target] : package.dependencies) pending.push_back(target);
                    for (const auto& [macro, inputs] : package.macroInputs) for (const auto& [name, file] : inputs) add(package.root / file);
                    if (!package.native.sources.empty()) return {};
                    for (const auto& file : package.native.headers) add(package.root / file);
                    if (auto archive = package.native.archives.find(nativePackageTarget); archive != package.native.archives.end())
                        for (const auto& file : archive->second) add(package.root / file);
                }
            } else tree(directory / "src", true, false);
            if (!test.empty()) add(test);
            for (const auto& file : libraries) {
                if (!standaloneLinkInput(file)) return {};
                add(file);
            }
            std::set<fs::path> projectInputs;
            files.swap(projectInputs);
            const auto& sdk = sdkRoot();
            add(fromUtf8(llvm::sys::fs::getMainExecutable(nullptr, nullptr)));
            add(sdk / "libexec/lyra" / (windows ? "lld-link.exe" : "ld.lld"));
            if constexpr (windows) {
                for (auto name : {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}) {
                    add(sdk / "bin" / name);
                    add(sdk / "libexec/lyra" / name);
                }
            } else add(fromUtf8(linuxLoader));
            for (const auto& file : fs::directory_iterator(sdk / "lib")) if (file.path().extension() == ".a") add(file.path());
            tree(sdk / "lib/lyra/target", false, false);
            tree(sdk / "lib/lyra/host", false);
            tree(sdk / "share/lyra/std", true);
            llvm::BLAKE3 digest;
            auto text = [&](llvm::StringRef value) { digest.update(std::to_string(value.size())); digest.update(":"); digest.update(value); };
            text("lyra-build-v1"); text(entry); text(pathText(directory)); text(pathText(output)); text(nativeTriple);
            text(options.release ? "release" : "debug"); text(options.debugInfo ? "symbols" : "default-symbols");
            text(std::to_string(libraries.size()));
            for (const auto& library : libraries) text(pathText(library));
            auto sdkInputs = files;
            files.merge(projectInputs);
            for (const auto& file : files) {
                text(pathText(file));
                const bool sdkInput = sdkInputs.contains(file) && !projectInputs.contains(file);
                if (!sdkInput) text(pathText(fs::weakly_canonical(file)));
                auto status = fs::symlink_status(file);
                if (!fs::exists(status)) { text("missing"); continue; }
                if (fs::is_symlink(status)) { text("symlink"); text(pathText(fs::read_symlink(file))); }
                if (fs::is_regular_file(file)) {
                    auto hash = sdkInput ? sdkDigest(file) : fileContentKey(file);
                    if (!hash) return {};
                    text("file"); text(*hash);
                } else if (fs::is_directory(file)) text("directory");
                else return {};
            }
            return llvm::toHex(digest.final(), true);
        } catch (const fs::filesystem_error&) {
            return {};
        }
    }
};

}
