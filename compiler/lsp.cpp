module;
#include <algorithm>
#include <cctype>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

module lyra.lsp;
import lyra.check;
import lyra.format;
import lyra.macros;
import lyra.modules;
import lyra.packages;
import lyra.platform;
import lyra.syntax;

namespace lyra {
namespace fs = std::filesystem;
namespace {
namespace json = llvm::json;

struct Document { fs::path path; std::string uri, text; std::int64_t version{}; };
struct Workspace { std::vector<Source> sources; std::vector<NameReference> references; std::set<std::string> names; };

const Source* sourceFor(const Workspace& workspace, const fs::path& path);

std::string encodeUri(const fs::path& path) {
    auto text = pathText(fs::absolute(path).lexically_normal());
    std::string result = "file://";
    if (text.empty() || text.front() != '/') result += '/';
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char byte : text) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' || byte == '-' || byte == '_' || byte == '.' || byte == '~') result += static_cast<char>(byte);
        else { result += '%'; result += digits[byte >> 4]; result += digits[byte & 15]; }
    }
    return result;
}

fs::path decodeUri(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) throw Diagnostic{"LYLSP0001", "Lyra LSP 只接受 file URI"};
    uri.remove_prefix(prefix.size());
    std::string text;
    auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] == '%') {
            if (index + 2 >= uri.size() || hex(uri[index + 1]) < 0 || hex(uri[index + 2]) < 0) throw Diagnostic{"LYLSP0001", "file URI 包含无效的百分号转义"};
            text += static_cast<char>((hex(uri[index + 1]) << 4) | hex(uri[index + 2]));
            index += 2;
        } else text += uri[index];
    }
#ifdef _WIN32
    if (text.size() >= 3 && text.front() == '/' && std::isalpha(static_cast<unsigned char>(text[1])) && text[2] == ':') text.erase(text.begin());
#endif
    return fs::absolute(fromUtf8(text)).lexically_normal();
}

std::uint32_t decodePoint(std::string_view text, std::size_t& offset) {
    auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) return first;
    unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    std::uint32_t point = first & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    while (count--) point = (point << 6) | (static_cast<unsigned char>(text[offset++]) & 0x3f);
    return point;
}

std::size_t byteOffset(const Source& source, std::size_t line, std::size_t character) {
    if (line >= source.lineStarts.size()) return source.text.size();
    auto offset = source.lineStarts[line];
    auto end = source.text.find_first_of("\r\n", offset);
    if (end == std::string::npos) end = source.text.size();
    std::size_t units = 0;
    while (offset < end && units < character) {
        auto next = offset;
        auto point = decodePoint(source.text, next);
        auto width = point > 0xffff ? 2u : 1u;
        if (units + width > character) return offset;
        units += width;
        offset = next;
    }
    return offset;
}

Position lspPosition(const Source& source, std::size_t offset) {
    auto position = source.position(offset);
    auto start = source.lineStarts[position.line - 1];
    std::size_t units = 0;
    while (start < offset) {
        auto point = decodePoint(source.text, start);
        units += point > 0xffff ? 2 : 1;
    }
    return {offset, position.line - 1, units};
}

json::Object jsonLspPosition(const Source& source, std::size_t offset) {
    auto value = lspPosition(source, offset);
    return json::Object{{"line", value.line}, {"character", value.column}};
}

json::Object jsonRange(const Source& source, Span span) {
    return json::Object{{"start", jsonLspPosition(source, span.begin)}, {"end", jsonLspPosition(source, span.end)}};
}

json::Object jsonLocation(const Source& source, Span span) {
    return json::Object{{"uri", encodeUri(fromUtf8(source.site(span).file))}, {"range", jsonRange(source, span)}};
}

std::optional<std::pair<const Source*, Span>> resolveAnchor(const Workspace& workspace, SourceAnchor anchor) {
    if (anchor.file >= workspace.sources.size()) return {};
    const auto& source = workspace.sources[anchor.file];
    auto site = source.userSite(anchor.span);
    auto original = sourceFor(workspace, fromUtf8(site.file));
    if (!original) return {};
    return std::pair{original, Span{site.start.offset, site.end.offset}};
}

json::Object jsonLocation(const Workspace& workspace, SourceAnchor anchor) {
    auto location = resolveAnchor(workspace, anchor);
    if (!location) return {};
    return jsonLocation(*location->first, location->second);
}

std::string pathKey(const fs::path& path) {
    std::error_code error;
    auto value = fs::weakly_canonical(path, error);
    return pathText(error ? fs::absolute(path).lexically_normal() : value);
}

bool inside(const fs::path& directory, const fs::path& path) {
    auto relative = path.lexically_relative(directory);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

fs::path packageRoot(fs::path file) {
    auto directory = file.parent_path();
    for (auto current = directory; !current.empty(); current = current.parent_path()) {
        if (fs::is_regular_file(current / "lyra.json5")) return current;
        if (current == current.root_path()) break;
    }
    for (auto current = directory; !current.empty(); current = current.parent_path()) {
        if (current.filename() == "src") return current.parent_path();
        if (current == current.root_path()) break;
    }
    return directory;
}

Workspace loadWorkspace(const fs::path& root, const SourceOverlay& overlays, const std::optional<PackageGraph>& graph, ParseCache& parseCache) {
    Workspace workspace;
    if (graph) {
        if (graph->entry.empty()) throw Diagnostic{"LYK0001", "workspace 根没有可分析的 Lyra 包"};
        auto program = loadProgram(*graph, graph->entry, {}, &overlays, &parseCache);
        auto checked = check(program.syntax, program.sources, false);
        workspace.sources = std::move(program.sources);
        workspace.references = std::move(program.references);
        workspace.references.insert(workspace.references.end(), checked.references.begin(), checked.references.end());
    } else {
        auto sources = loadSources(root);
        for (auto& source : sources) if (auto found = overlays.find(source.path); found != overlays.end()) source = sourceText(source.path, found->second, {});
        auto syntax = parse(sources);
        auto checked = check(std::move(syntax), sources, false);
        workspace.sources = std::move(sources);
        workspace.references = std::move(checked.references);
    }
    for (const auto& source : workspace.sources) for (const auto& token : syntaxTokens(source, false, true)) {
        if (token.kind != "name" && token.kind != "keyword") continue;
        auto value = source.text.substr(token.span.begin, token.span.end - token.span.begin);
        if (!value.empty() && value.front() == '`') value = value.substr(1, value.size() - 2);
        workspace.names.insert(std::move(value));
    }
    return workspace;
}

struct WatchPattern {
    fs::path directory;
    std::string pattern;
    auto operator<=>(const WatchPattern&) const = default;
};

std::string literalGlob(const fs::path& path) {
    std::string result;
    for (auto character : pathText(path)) {
        if (std::string_view("[]{}*?").contains(character)) result += std::string("[") + character + ']';
        else result += character;
    }
    return result;
}

WatchPattern watchFile(const fs::path& path) {
    auto directory = path.parent_path();
    while (!fs::is_directory(directory) && directory != directory.root_path()) directory = directory.parent_path();
    return {directory, literalGlob(path.lexically_relative(directory))};
}

struct AnalysisInputs {
    std::map<fs::path, std::string> files;
    std::map<fs::path, fs::path> sourceTrees;
    std::set<fs::path> watchedFiles;
    std::set<WatchPattern> watchers;
    std::vector<std::string> packageOrder;
    std::string entry;
    bool operator==(const AnalysisInputs&) const = default;
};

enum class InputContents { Metadata, File, Macro };

bool recordInput(AnalysisInputs& inputs, const fs::path& path, InputContents contents = InputContents::File, const fs::path& boundary = {}) {
    auto& value = inputs.files[path];
    if (!boundary.empty()) {
        auto resolved = fs::weakly_canonical(path);
        value = pathText(resolved) + '\0';
        if (!inside(boundary, resolved)) return false;
    } else value.clear();
    auto status = fs::symlink_status(path);
    value += std::to_string(static_cast<int>(status.type()));
    value += '\0';
    if (status.type() == fs::file_type::symlink) value += pathText(fs::read_symlink(path)) + '\0';
    if (contents == InputContents::Metadata || !fs::is_regular_file(path)) return true;
    if (contents == InputContents::Macro && fs::file_size(path) > macroInputByteLimit) {
        value += "oversize";
        return true;
    }
    std::ifstream file(path, std::ios::binary);
    char buffer[8192];
    std::size_t remaining = contents == InputContents::Macro ? macroInputByteLimit + 1 : std::string::npos;
    while (remaining && (file.read(buffer, std::min(remaining, sizeof buffer)) || file.gcount() > 0)) {
        value.append(buffer, file.gcount());
        remaining -= file.gcount();
    }
    value += '\0';
    value += (!file.eof() || file.bad()) ? "incomplete" : "complete";
    return true;
}

bool trackFile(AnalysisInputs& inputs, const fs::path& path, InputContents contents = InputContents::File, const fs::path& boundary = {}) {
    if (!recordInput(inputs, path, contents, boundary)) return false;
    inputs.watchers.insert(watchFile(path));
    auto resolved = fromUtf8(pathKey(path));
    if (resolved != path) {
        inputs.watchedFiles.insert(resolved);
        inputs.watchers.insert(watchFile(resolved));
    }
    return true;
}

void trackTree(AnalysisInputs& inputs, const fs::path& directory, std::string_view pattern) {
    auto resolved = fromUtf8(pathKey(directory));
    inputs.sourceTrees.emplace(directory, resolved);
    auto shallow = watchFile(directory);
    shallow.pattern += '/';
    shallow.pattern += pattern;
    inputs.watchers.insert(std::move(shallow));
    if (resolved != directory) inputs.watchers.insert({std::move(resolved), std::string(pattern)});
}

void recordTree(AnalysisInputs& inputs, const fs::path& directory, bool packaged = false) {
    trackTree(inputs, directory, "**/*.ly");
    recordInput(inputs, directory, InputContents::Metadata);
    if (!fs::is_directory(directory)) return;
    for (auto item = fs::recursive_directory_iterator(directory); item != fs::recursive_directory_iterator{}; ++item) {
        if (packaged && item->is_directory() && pathText(item->path().filename()).starts_with('.')) {
            item.disable_recursion_pending();
            continue;
        }
        if (item->is_symlink()) recordInput(inputs, item->path(), packaged ? InputContents::Metadata : InputContents::File);
        else if (item->path().extension() == ".ly") recordInput(inputs, item->path());
    }
}

void recordMacroSources(AnalysisInputs& inputs, const fs::path& package, std::vector<fs::path>& overlayPaths) {
    for (const auto* name : {".macros", ".expressions", ".decorators"}) {
        auto directory = package / "src" / name;
        if (!recordInput(inputs, directory, InputContents::Metadata, package)) continue;
        trackTree(inputs, directory, "*.ly");
        if (!fs::is_directory(directory)) continue;
        for (const auto& item : fs::directory_iterator(directory)) {
            const auto& path = item.path();
            if (path.extension() != ".ly" || !recordInput(inputs, path, InputContents::Macro, package)) continue;
            overlayPaths.push_back(path);
            auto resolved = fs::weakly_canonical(path);
            if (resolved.parent_path() != fs::weakly_canonical(directory)) {
                inputs.watchedFiles.insert(resolved);
                inputs.watchers.insert(watchFile(resolved));
            }
        }
    }
}

bool watchesPath(const AnalysisInputs& inputs, const fs::path& path) {
    if (inputs.files.contains(path) || inputs.watchedFiles.contains(path)) return true;
    if (path.extension() != ".ly") return false;
    return std::ranges::any_of(inputs.sourceTrees, [&](const auto& tree) { return inside(tree.first, path) || inside(tree.second, path); });
}

struct Snapshot {
    AnalysisInputs inputs;
    SourceOverlay overlays;
    std::optional<Workspace> workspace;
    std::optional<Diagnostic> error;

    const Workspace& result() const {
        if (error) throw *error;
        return *workspace;
    }
};

std::optional<std::vector<std::pair<std::size_t, Source>>> triviaOnlyUpdates(
    const Snapshot& previous, const AnalysisInputs& inputs, const SourceOverlay& overlays) {
    if (previous.inputs != inputs || previous.overlays == overlays || !previous.workspace || previous.error)
        return {};
    for (const auto& source : previous.workspace->sources)
        if (source.text.find_first_of("#@") != std::string::npos) return {};

    std::map<std::string, fs::path> oldOverlays, newOverlays;
    for (const auto& entry : previous.overlays) oldOverlays.emplace(pathKey(entry.first), entry.first);
    for (const auto& entry : overlays) newOverlays.emplace(pathKey(entry.first), entry.first);
    std::set<std::string> changed;
    for (const auto& [key, path] : oldOverlays) {
        auto current = newOverlays.find(key);
        auto oldText = previous.overlays.find(path);
        auto newText = current == newOverlays.end() ? overlays.end() : overlays.find(current->second);
        if (current == newOverlays.end() || oldText == previous.overlays.end() || newText == overlays.end() || oldText->second != newText->second)
            changed.insert(key);
    }
    for (const auto& [key, path] : newOverlays) if (!oldOverlays.contains(key)) changed.insert(key);
    if (changed.empty()) return {};

    std::vector<std::pair<std::size_t, Source>> updates;
    for (const auto& key : changed) {
        auto found = std::ranges::find_if(previous.workspace->sources, [&](const Source& source) { return pathKey(source.path) == key; });
        if (found == previous.workspace->sources.end()) return {};
        auto index = static_cast<std::size_t>(found - previous.workspace->sources.begin());
        auto overlay = std::ranges::find_if(overlays, [&](const auto& entry) { return pathKey(entry.first) == key; });
        Source current;
        try {
            current = overlay == overlays.end() ? loadSource(found->path, found->moduleId) : sourceText(found->path, overlay->second, found->moduleId);
            if (current.text.find_first_of("#@") != std::string::npos || current.lineStarts != found->lineStarts) return {};
            auto oldTokens = syntaxTokens(*found);
            auto newTokens = syntaxTokens(current);
            if (oldTokens.size() != newTokens.size()) return {};
            for (std::size_t token = 0; token < oldTokens.size(); ++token) {
                const auto& oldToken = oldTokens[token];
                const auto& newToken = newTokens[token];
                if (oldToken.kind != newToken.kind || oldToken.span != newToken.span ||
                    found->text.substr(oldToken.span.begin, oldToken.span.end - oldToken.span.begin) !=
                        current.text.substr(newToken.span.begin, newToken.span.end - newToken.span.begin)) return {};
            }
        } catch (const Diagnostic&) {
            return {};
        }
        updates.emplace_back(index, std::move(current));
    }
    return updates;
}

const Source* sourceFor(const Workspace& workspace, const fs::path& path) {
    auto key = pathKey(path);
    auto found = std::ranges::find_if(workspace.sources, [&](const Source& source) { return !source.origin && pathKey(source.path) == key; });
    if (found == workspace.sources.end()) return nullptr;
    auto original = &*found;
    while (original->previous) original = original->previous.get();
    return original;
}

const NameReference* referenceAt(const Workspace& workspace, const fs::path& path, std::size_t offset) {
    auto matches = [&](SourceAnchor anchor) {
        if (anchor.file >= workspace.sources.size()) return false;
        const auto& source = workspace.sources[anchor.file];
        auto site = source.userSite(anchor.span);
        return pathKey(fromUtf8(site.file)) == pathKey(path) && site.start.offset <= offset && (offset < site.end.offset || site.start.offset == site.end.offset);
    };
    for (const auto& reference : workspace.references) if (matches(reference.use)) return &reference;
    for (const auto& reference : workspace.references) if (matches(reference.declaration)) return &reference;
    return nullptr;
}

SourceAnchor renameAnchor(const NameReference& reference) { return reference.renameTarget.value_or(reference.declaration); }

json::Array locationsFor(const Workspace& workspace, SourceAnchor target, bool includeDeclaration) {
    std::set<std::tuple<std::string, std::size_t, std::size_t>> seen;
    json::Array locations;
    auto append = [&](SourceAnchor anchor) {
        auto location = resolveAnchor(workspace, anchor);
        if (!location) return;
        const auto& [source, span] = *location;
        auto site = source->site(span);
        auto key = std::tuple{pathKey(fromUtf8(site.file)), site.start.offset, site.end.offset};
        if (seen.insert(key).second) locations.push_back(jsonLocation(workspace, anchor));
    };
    if (includeDeclaration) append(target);
    for (const auto& reference : workspace.references) if (renameAnchor(reference) == target) append(reference.use);
    return locations;
}

std::string nameAt(std::string_view text, std::size_t offset, std::size_t& begin, std::size_t& end) {
    begin = std::min(offset, text.size());
    end = begin;
    auto nameByte = [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch >= 0x80; };
    while (begin && nameByte(static_cast<unsigned char>(text[begin - 1]))) --begin;
    while (end < text.size() && nameByte(static_cast<unsigned char>(text[end]))) ++end;
    return std::string(text.substr(begin, end - begin));
}

json::Value errorResponse(const json::Value& id, int code, std::string message) {
    return json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", json::Object{{"code", code}, {"message", std::move(message)}}}};
}

class Server {
public:
    int run() {
        std::string header;
        while (std::getline(std::cin, header)) {
            if (!header.empty() && header.back() == '\r') header.pop_back();
            std::size_t length = 0;
            bool foundLength = false;
            while (!header.empty()) {
                if (header.empty()) break;
                auto colon = header.find(':');
                if (colon == std::string::npos) return 1;
                auto key = header.substr(0, colon);
                auto value = header.substr(colon + 1);
                if (!value.empty() && value.front() == ' ') value.erase(value.begin());
                std::ranges::transform(key, key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (key == "content-length") { length = static_cast<std::size_t>(std::stoull(value)); foundLength = true; }
                if (!std::getline(std::cin, header)) return 1;
                if (!header.empty() && header.back() == '\r') header.pop_back();
                if (header.empty()) break;
            }
            if (!foundLength || length > 64 * 1024 * 1024) return 1;
            std::string body(length, '\0');
            if (!std::cin.read(body.data(), static_cast<std::streamsize>(length))) return 1;
            auto message = json::parse(body);
            if (!message) return 1;
            auto object = message->getAsObject();
            if (!object) continue;
            auto id = object->get("id");
            auto method = object->getString("method");
            auto params = object->getObject("params");
            if (!method) {
                if (id && !object->get("result") && !object->get("error")) send(errorResponse(*id, -32600, "invalid request"));
                continue;
            }
            try {
                auto response = handle(*method, params ? *params : json::Object{}, id != nullptr);
                if (id) send(json::Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(response)}});
                if (exited) return shutdown ? 0 : 1;
            } catch (const Diagnostic& error) {
                if (id) send(errorResponse(*id, -32602, error.message));
                else publishFailure(error);
            } catch (const PackageError& error) {
                if (id) send(errorResponse(*id, -32603, error.message));
                else publishFailure({error.code, error.message});
            } catch (const std::exception& error) {
                if (id) send(errorResponse(*id, -32603, error.what()));
            }
        }
        return 0;
    }

private:
    std::map<std::string, Document> documents;
    std::map<fs::path, Snapshot> snapshots;
    ParseCache parseCache;
    std::map<WatchPattern, std::string> activeWatchers;
    std::size_t nextWatcher{};
    std::size_t nextRequest{};
    bool relativeWatchers{};
    bool shutdown{};
    bool exited{};
    bool traceVerbose{};

    const Workspace& analyze(const fs::path& root) {
        AnalysisInputs inputs;
        std::vector<fs::path> extraOverlayPaths;
        std::optional<PackageGraph> graph;
        trackFile(inputs, root / "lyra.json5");
        trackFile(inputs, root / "lyra.lock");
        recordTree(inputs, root / "src", true);
        if (fs::is_regular_file(root / "lyra.json5")) {
            PackageOptions options;
            options.readOnly = true;
            graph = resolvePackages(root, options);
            if (graph->workspace != root) {
                trackFile(inputs, graph->workspace / "lyra.json5");
                trackFile(inputs, graph->workspace / "lyra.lock");
            }
            inputs.entry = graph->entry;
            if (graph->entry.empty()) throw Diagnostic{"LYK0001", "workspace 根没有可分析的 Lyra 包"};
            std::map<std::string, const ResolvedPackage*> packages;
            for (const auto& package : graph->packages) {
                inputs.packageOrder.push_back(package.id);
                packages.emplace(package.id, &package);
            }
            std::vector<std::string> pending{graph->entry};
            std::set<std::string> reachable;
            for (std::size_t index = 0; index < pending.size(); ++index) {
                if (!reachable.insert(pending[index]).second) continue;
                const auto& package = *packages.at(pending[index]);
                if (package.root != root) {
                    trackFile(inputs, package.root / "lyra.json5");
                    recordTree(inputs, package.root / "src", true);
                }
                recordMacroSources(inputs, package.root, extraOverlayPaths);
                for (const auto& [alias, target] : package.dependencies) pending.push_back(target);
                for (const auto& [macro, files] : package.macroInputs) for (const auto& [alias, path] : files) {
                    auto grant = package.root / path;
                    if (trackFile(inputs, grant, InputContents::Macro, package.root)) extraOverlayPaths.push_back(std::move(grant));
                }
            }
            recordTree(inputs, sdkRoot() / "share" / "lyra" / "std");
        }
        SourceOverlay overlays;
        for (const auto& [key, document] : documents) {
            auto path = fromUtf8(key);
            if (path.extension() != ".ly") continue;
            for (const auto& [directory, resolved] : inputs.sourceTrees) if (inside(resolved, path))
                overlays.emplace(directory / path.lexically_relative(resolved), document.text);
        }
        for (const auto& path : extraOverlayPaths) if (auto document = documents.find(pathKey(path)); document != documents.end())
            overlays.emplace(path, document->second.text);

        auto found = snapshots.find(root);
        if (found != snapshots.end() && found->second.inputs == inputs) {
            auto& previous = found->second;
            if (previous.overlays == overlays) {
                trace("Lyra analysis: reused package analysis for " + pathText(root));
                return previous.result();
            }
            if (auto updates = triviaOnlyUpdates(previous, inputs, overlays)) {
                for (auto& [index, source] : *updates) previous.workspace->sources[index] = std::move(source);
                previous.overlays = std::move(overlays);
                trace("Lyra analysis: reused checked workspace after trivia-only document edits for " + pathText(root));
                return *previous.workspace;
            }
        }
        trace("Lyra analysis: rebuilt workspace and reran semantic checks for " + pathText(root));
        Snapshot fresh{std::move(inputs), std::move(overlays), {}, {}};
        try { fresh.workspace = loadWorkspace(root, fresh.overlays, graph, parseCache); }
        catch (const Diagnostic& error) { fresh.error = error; }
        return snapshots.insert_or_assign(root, std::move(fresh)).first->second.result();
    }

    static void send(const json::Value& value) {
        auto body = llvm::formatv("{0}", value).str();
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
    }

    void trace(std::string message) const {
        if (traceVerbose) send(json::Object{{"jsonrpc", "2.0"}, {"method", "$/logTrace"}, {"params", json::Object{{"message", std::move(message)}}}});
    }

    void requestClient(std::string method, json::Object params) {
        send(json::Object{{"jsonrpc", "2.0"}, {"id", "lyra-" + std::to_string(nextRequest++)}, {"method", std::move(method)}, {"params", std::move(params)}});
    }

    void synchronizeWatchers(const std::map<fs::path, const Document*>& roots) {
        if (!relativeWatchers) return;
        std::set<WatchPattern> desired;
        for (const auto& [root, owner] : roots) {
            if (auto found = snapshots.find(root); found != snapshots.end()) {
                const auto& watchers = found->second.inputs.watchers;
                desired.insert(watchers.begin(), watchers.end());
            } else {
                desired.insert({root, "src/**/*.ly"});
                for (const auto* name : {"lyra.json5", "lyra.lock"}) desired.insert({root, name});
            }
        }
        const std::string method = "workspace/didChangeWatchedFiles";
        json::Array registrations, removals;
        std::erase_if(activeWatchers, [&](const auto& entry) {
            if (desired.contains(entry.first)) return false;
            removals.push_back(json::Object{{"id", entry.second}, {"method", method}});
            return true;
        });
        for (const auto& pattern : desired) {
            if (activeWatchers.contains(pattern)) continue;
            auto id = "lyra-watch-" + std::to_string(nextWatcher++);
            activeWatchers.emplace(pattern, id);
            json::Object glob{{"baseUri", encodeUri(pattern.directory)}, {"pattern", pattern.pattern}};
            auto watchers = json::Array{json::Object{{"globPattern", std::move(glob)}}};
            registrations.push_back(json::Object{{"id", std::move(id)}, {"method", method}, {"registerOptions", json::Object{{"watchers", std::move(watchers)}}}});
        }
        if (!registrations.empty()) requestClient("client/registerCapability", json::Object{{"registrations", std::move(registrations)}});
        if (!removals.empty()) requestClient("client/unregisterCapability", json::Object{{"unregisterations", std::move(removals)}});
    }

    std::optional<json::Value> handle(std::string_view method, const json::Object& params, bool request) {
        if (method == "initialize") {
            auto capabilities = params.getObject("capabilities");
            auto workspace = capabilities ? capabilities->getObject("workspace") : nullptr;
            auto watch = workspace ? workspace->getObject("didChangeWatchedFiles") : nullptr;
            relativeWatchers = watch && watch->getBoolean("dynamicRegistration").value_or(false) && watch->getBoolean("relativePatternSupport").value_or(false);
            return json::Object{{"capabilities", json::Object{
            {"positionEncoding", "utf-16"}, {"textDocumentSync", json::Object{{"openClose", true}, {"change", 2}}},
            {"completionProvider", json::Object{{"triggerCharacters", json::Array{".", "@", "#"}}}},
            {"hoverProvider", true}, {"definitionProvider", true}, {"referencesProvider", true},
            {"renameProvider", true}, {"documentFormattingProvider", true}}}};
        }
        if (method == "initialized" || method == "$/cancelRequest") return {};
        if (method == "$/setTrace") {
            auto value = params.getString("value");
            traceVerbose = value && *value == "verbose";
            return {};
        }
        if (method == "shutdown") { shutdown = true; return json::Value(nullptr); }
        if (method == "exit") { exited = true; return {}; }
        if (method == "workspace/didChangeWatchedFiles") {
            auto changes = params.getArray("changes");
            if (!changes) return {};
            for (const auto& change : *changes) {
                auto entry = change.getAsObject();
                auto uri = entry ? entry->getString("uri") : std::nullopt;
                if (!uri || !uri->starts_with("file://")) continue;
                auto path = decodeUri(*uri);
                auto canonical = fromUtf8(pathKey(path));
                if (std::ranges::any_of(snapshots, [&](const auto& item) {
                    return watchesPath(item.second.inputs, path) || watchesPath(item.second.inputs, canonical);
                }) || ((path.filename() == "lyra.json5" || path.filename() == "lyra.lock") && std::ranges::any_of(documents, [&](const auto& item) {
                    return inside(path.parent_path(), fromUtf8(item.first)) || inside(canonical.parent_path(), fromUtf8(item.first));
                }))) {
                    publishDiagnostics(path);
                    break;
                }
            }
            return {};
        }
        if (method == "textDocument/didOpen") {
            auto document = params.getObject("textDocument");
            if (!document) return {};
            auto uri = document->getString("uri"), text = document->getString("text");
            if (!uri || !text) return {};
            auto path = decodeUri(*uri);
            documents[pathKey(path)] = {path, uri->str(), text->str(), document->getInteger("version").value_or(0)};
            publishDiagnostics(path);
            return {};
        }
        if (method == "textDocument/didChange") {
            auto document = params.getObject("textDocument");
            auto changes = params.getArray("contentChanges");
            if (!document || !changes) return {};
            auto uri = document->getString("uri");
            if (!uri) return {};
            auto path = decodeUri(*uri);
            auto found = documents.find(pathKey(path));
            if (found == documents.end()) return {};
            auto version = document->getInteger("version");
            if (!version || *version <= found->second.version) return {};
            for (const auto& change : *changes) {
                auto item = change.getAsObject();
                if (!item) continue;
                auto text = item->getString("text");
                if (!text) continue;
                auto range = item->getObject("range");
                if (!range) found->second.text = text->str();
                else {
                    auto start = range->getObject("start"), end = range->getObject("end");
                    if (!start || !end) continue;
                    auto startLine = start->getInteger("line").value_or(0), startCharacter = start->getInteger("character").value_or(0);
                    auto endLine = end->getInteger("line").value_or(0), endCharacter = end->getInteger("character").value_or(0);
                    auto current = sourceText(path, found->second.text, {});
                    auto begin = byteOffset(current, static_cast<std::size_t>(startLine), static_cast<std::size_t>(startCharacter));
                    auto finish = byteOffset(current, static_cast<std::size_t>(endLine), static_cast<std::size_t>(endCharacter));
                    found->second.text.replace(begin, finish - begin, text->str());
                }
            }
            found->second.version = *version;
            publishDiagnostics(path);
            return {};
        }
        if (method == "textDocument/didClose") {
            auto document = params.getObject("textDocument");
            if (!document) return {};
            auto uri = document->getString("uri");
            if (!uri) return {};
            auto path = decodeUri(*uri);
            documents.erase(pathKey(path));
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", uri->str()}, {"diagnostics", json::Array{}}}}});
            publishDiagnostics(path);
            return {};
        }
        if (method.starts_with("textDocument/")) return documentRequest(method, params);
        if (request) return json::Value(nullptr);
        return {};
    }

    fs::path requestPath(const json::Object& params) const {
        auto document = params.getObject("textDocument");
        auto uri = document ? document->getString("uri") : std::nullopt;
        if (!uri) throw Diagnostic{"LYLSP0001", "缺少 textDocument.uri"};
        return decodeUri(*uri);
    }

    std::pair<const Document&, const Source&> documentSource(const fs::path& path, const Workspace& workspace) const {
        auto doc = documents.find(pathKey(path));
        auto source = sourceFor(workspace, path);
        if (doc == documents.end() || !source) throw Diagnostic{"LYLSP0001", "文档没有打开或不属于当前 Lyra 包"};
        return {doc->second, *source};
    }

    std::optional<json::Value> documentRequest(std::string_view method, const json::Object& params) {
        auto path = requestPath(params);
        auto key = pathKey(path);
        auto opened = documents.find(key);
        if (opened == documents.end()) throw Diagnostic{"LYLSP0001", "文档没有打开"};
        if (method == "textDocument/formatting") {
            FormatOptions options;
            if (const auto* requested = params.getObject("options")) {
                auto tabSize = requested->getInteger("tabSize").value_or(options.tabSize);
                if (tabSize >= 1 && tabSize <= 32) options.tabSize = static_cast<unsigned>(tabSize);
                options.insertSpaces = requested->getBoolean("insertSpaces").value_or(options.insertSpaces);
            }
            auto source = sourceText(opened->second.path, opened->second.text, {});
            auto output = formatSource(source, options);
            if (output == source.text) return json::Array{};
            return json::Array{json::Object{{"range", jsonRange(source, {0, source.text.size()})}, {"newText", std::move(output)}}};
        }
        auto root = packageRoot(fromUtf8(key));
        const Workspace* analysis;
        try { analysis = &analyze(root); }
        catch (const Diagnostic& error) { if (method == "textDocument/completion") return json::Object{{"isIncomplete", false}, {"items", json::Array{}}}; throw error; }
        const auto& workspace = *analysis;
        const auto& [document, source] = documentSource(path, workspace);
        auto position = params.getObject("position");
        if (!position) throw Diagnostic{"LYLSP0001", "缺少 position"};
        auto line = position->getInteger("line").value_or(0), character = position->getInteger("character").value_or(0);
        auto offset = byteOffset(source, static_cast<std::size_t>(line), static_cast<std::size_t>(character));
        auto reference = referenceAt(workspace, path, offset);
        if (method == "textDocument/definition") {
            if (!reference || reference->declaration.file >= workspace.sources.size()) return json::Value(nullptr);
            return json::Array{jsonLocation(workspace, reference->declaration)};
        }
        if (method == "textDocument/references") {
            if (!reference) return json::Array{};
            auto context = params.getObject("context");
            auto include = context && context->getBoolean("includeDeclaration").value_or(false);
            return locationsFor(workspace, renameAnchor(*reference), include);
        }
        if (method == "textDocument/rename") {
            if (!reference) throw Diagnostic{"LYLSP0002", "游标处没有已绑定的 Lyra 符号"};
            auto newName = params.getString("newName");
            if (!newName || newName->empty() || (!std::isalpha(static_cast<unsigned char>((*newName)[0])) && (*newName)[0] != '_')) throw Diagnostic{"LYLSP0001", "新名称必须是有效 Lyra 标识符"};
            if (!std::all_of(newName->begin() + 1, newName->end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; })) throw Diagnostic{"LYLSP0001", "新名称必须是有效 Lyra 标识符"};
            auto target = renameAnchor(*reference);
            auto edits = locationsFor(workspace, target, true);
            json::Object changes;
            std::map<std::string, json::Array> grouped;
            for (const auto& location : edits) {
                auto item = location.getAsObject();
                auto uri = item->getString("uri");
                auto range = item->getObject("range");
                if (uri && range) {
                    json::Object change;
                    auto start = range->getObject("start"), end = range->getObject("end");
                    if (!start || !end) continue;
                    json::Object clonedRange;
                    clonedRange["start"] = json::Object{{"line", start->getInteger("line").value_or(0)}, {"character", start->getInteger("character").value_or(0)}};
                    clonedRange["end"] = json::Object{{"line", end->getInteger("line").value_or(0)}, {"character", end->getInteger("character").value_or(0)}};
                    change["range"] = std::move(clonedRange);
                    change["newText"] = newName->str();
                    grouped[uri->str()].push_back(std::move(change));
                }
            }
            for (auto& [uri, changesForUri] : grouped) changes[uri] = std::move(changesForUri);
            return json::Object{{"changes", std::move(changes)}};
        }
        if (method == "textDocument/hover") {
            if (!reference) return json::Value(nullptr);
            auto resolved = resolveAnchor(workspace, reference->declaration);
            if (!resolved) return json::Value(nullptr);
            const auto& definition = *resolved->first;
            auto site = definition.site(resolved->second);
            auto start = definition.lineStarts[site.start.line - 1];
            auto end = definition.text.find_first_of("\r\n", start);
            if (end == std::string::npos) end = definition.text.size();
            auto signature = std::string_view(definition.text).substr(start, end - start);
            return json::Object{{"contents", json::Object{{"kind", "markdown"}, {"value", "```lyra\n" + std::string(signature) + "\n```"}}}};
        }
        if (method == "textDocument/completion") {
            std::size_t begin, end;
            auto prefix = nameAt(document.text, offset, begin, end);
            auto candidates = workspace.names;
            static constexpr std::string_view keywords[] = {"async", "await", "break", "catch", "class", "continue", "defer", "else", "enum", "false", "for", "fn", "if", "import", "in", "is", "match", "null", "pub", "return", "spawn", "static", "struct", "this", "throw", "true", "try", "val", "var", "while"};
            for (auto keyword : keywords) candidates.emplace(keyword);
            json::Array items;
            for (const auto& candidate : candidates) if (prefix.empty() || candidate.starts_with(prefix)) items.push_back(json::Object{{"label", candidate}});
            return json::Object{{"isIncomplete", false}, {"items", std::move(items)}};
        }
        return json::Value(nullptr);
    }

    void publishFailure(const Diagnostic& error) {
        if (documents.empty()) return;
        auto& document = documents.begin()->second;
        auto source = sourceText(document.path, document.text, {});
        json::Array diagnostics;
        json::Object item{{"severity", 1}, {"code", error.code}, {"message", error.message}, {"range", jsonRange(source, {0, 0})}};
        diagnostics.push_back(std::move(item));
        send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", document.uri}, {"diagnostics", std::move(diagnostics)}}}});
    }

    void publishDiagnostics(const fs::path& changed) {
        std::map<std::string, json::Array> byUri;
        auto changedKey = pathKey(changed);
        std::map<fs::path, const Document*> roots;
        for (const auto& [key, document] : documents) {
            auto path = fromUtf8(key);
            if (path.extension() != ".ly") continue;
            auto root = packageRoot(path);
            auto relative = path.lexically_relative(root);
            if (std::ranges::any_of(relative, [](const auto& part) { return pathText(part).starts_with('.'); })) continue;
            if (key == changedKey) roots.insert_or_assign(std::move(root), &document);
            else roots.try_emplace(std::move(root), &document);
        }
        std::erase_if(snapshots, [&](const auto& entry) { return !roots.contains(entry.first); });
        auto append = [&](const Diagnostic& error, const Document& owner) {
            const Document* target = &owner;
            Span span{0, 0};
            if (error.site) {
                auto document = documents.find(pathKey(fromUtf8(error.site->file)));
                if (document == documents.end()) return;
                target = &document->second;
                span = {error.site->start.offset, error.site->end.offset};
            }
            auto source = sourceText(target->path, target->text, {});
            json::Value item = json::Object{{"severity", 1}, {"code", error.code}, {"message", error.message}, {"range", jsonRange(source, span)}};
            auto& diagnostics = byUri[target->uri];
            if (std::ranges::find(diagnostics, item) == diagnostics.end()) diagnostics.push_back(std::move(item));
        };
        for (const auto& [root, owner] : roots) {
            try { static_cast<void>(analyze(root)); }
            catch (const Diagnostic& error) { append(error, *owner); }
            catch (const PackageError& error) { append({error.code, error.message}, *owner); }
            catch (const fs::filesystem_error& error) { append({"LYI0001", error.what()}, *owner); }
        }
        synchronizeWatchers(roots);
        for (const auto& [key, document] : documents) {
            auto found = byUri.find(document.uri);
            auto diagnostics = found == byUri.end() ? json::Array{} : std::move(found->second);
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", document.uri}, {"version", document.version}, {"diagnostics", std::move(diagnostics)}}}});
        }
    }
};
}

int languageServer() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Server server;
    return server.run();
}
}
