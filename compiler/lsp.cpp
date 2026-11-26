module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
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
using Documents = std::map<std::string, Document>;
struct Workspace {
    std::vector<Source> sources;
    EditorIndex editor;
    AnalysisDiagnostics diagnostics;
    std::vector<EditorDeclaration> generated;
};

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

struct AnalysisView {
    fs::path root;
    std::optional<fs::path> test;
    auto operator<=>(const AnalysisView&) const = default;
};

AnalysisView analysisView(const fs::path& path) {
    auto root = packageRoot(path);
    auto test = isTestEntry(root, path) && fs::is_regular_file(root / "lyra.json5") ? std::optional(path) : std::nullopt;
    return {std::move(root), std::move(test)};
}

Workspace loadWorkspace(const fs::path& root, const SourceOverlay& overlays, const std::optional<PackageGraph>& graph, ParseCache& parseCache,
                        EditorProbe* probe = nullptr, const std::optional<fs::path>& testFile = {}) {
    Workspace workspace;
    workspace.editor.probe = probe;
    try {
        Compilation program;
        if (graph) {
            program = loadProgram(*graph, graph->entry, testFile, &overlays, &parseCache, probe != nullptr, probe ? nullptr : &workspace.diagnostics);
        } else {
            auto sources = loadSources(root);
            for (auto& source : sources) if (auto found = overlays.find(source.path); found != overlays.end()) source = sourceText(source.path, found->second, {});
            if (probe) {
                auto found = std::ranges::find(sources, probe->path, &Source::path);
                if (found == sources.end()) return workspace;
                program.sources = {std::move(*found)};
                program.syntax = indexFileSyntax(program.sources.front());
            } else program = loadLooseProgram(std::move(sources), workspace.diagnostics, &parseCache);
        }
        workspace.sources = std::move(program.sources);
        workspace.editor.references = std::move(program.references);
        if (probe) check(std::move(program.syntax), workspace.sources, false, nullptr, &workspace.editor);
        else if (!program.incompleteDeclaration) {
            workspace.generated = editorDeclarations(program.syntax, workspace.sources, true);
            checkForDiagnostics(std::move(program.syntax), workspace.sources, testFile.has_value(), workspace.diagnostics, &workspace.editor);
        }
    } catch (const Diagnostic& error) { workspace.diagnostics.add(error); }
    catch (const EditorProbeFinished&) {}
    workspace.diagnostics.sort();
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
    if (resolved != path) inputs.watchers.insert(watchFile(resolved));
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
    auto src = package / "src";
    std::vector<fs::path> modules{src};
    if (fs::is_directory(src)) for (auto item = fs::recursive_directory_iterator(src); item != fs::recursive_directory_iterator{}; ++item) {
        if (!item->is_directory()) continue;
        if (pathText(item->path().filename()).starts_with('.')) item.disable_recursion_pending();
        else if (!item->is_symlink()) modules.push_back(item->path());
    }
    for (const auto& module : modules) {
        for (const auto* name : macroSourceDirectories) {
            auto directory = module / name;
            if (!recordInput(inputs, directory, InputContents::Metadata, package)) continue;
            trackTree(inputs, directory, "*.ly");
            if (!fs::is_directory(directory)) continue;
            for (const auto& item : fs::directory_iterator(directory)) {
                const auto& path = item.path();
                if (path.extension() != ".ly" || !recordInput(inputs, path, InputContents::Macro, package)) continue;
                overlayPaths.push_back(path);
                auto resolved = fs::weakly_canonical(path);
                if (resolved.parent_path() != fs::weakly_canonical(directory)) inputs.watchers.insert(watchFile(resolved));
            }
        }
    }
}

struct Snapshot {
    AnalysisInputs inputs;
    SourceOverlay overlays;
    Workspace workspace;
};

struct WorkspaceInputs {
    AnalysisInputs inputs;
    std::optional<PackageGraph> graph;
    SourceOverlay overlays;
};

std::optional<std::vector<std::pair<std::size_t, Source>>> triviaOnlyUpdates(
    const Snapshot& previous, const AnalysisInputs& inputs, const SourceOverlay& overlays) {
    if (previous.inputs != inputs || previous.overlays == overlays || !previous.workspace.diagnostics.errors.empty())
        return {};
    for (const auto& source : previous.workspace.sources)
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
        auto found = std::ranges::find_if(previous.workspace.sources, [&](const Source& source) { return pathKey(source.path) == key; });
        if (found == previous.workspace.sources.end()) return {};
        auto index = static_cast<std::size_t>(found - previous.workspace.sources.begin());
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

std::optional<std::size_t> originalFile(const Workspace& workspace, const Source& source) {
    for (std::size_t file = 0; file < workspace.sources.size(); ++file) {
        auto original = &workspace.sources[file];
        while (original->previous) original = original->previous.get();
        if (original == &source) return file;
    }
    return {};
}

const NameReference* referenceAt(const Workspace& workspace, const Source& source, std::size_t offset) {
    auto file = originalFile(workspace, source);
    if (!file) return nullptr;
    const auto& current = workspace.sources[*file];
    auto matches = [&](SourceAnchor anchor) {
        if (anchor.file != *file) return false;
        auto span = current.authoredSpan(anchor.span);
        return span && span->begin <= offset && offset < span->end;
    };
    for (const auto& reference : workspace.editor.references) if (matches(reference.use)) return &reference;
    for (const auto& reference : workspace.editor.references) if (matches(reference.declaration)) return &reference;
    return nullptr;
}

SourceAnchor renameAnchor(const NameReference& reference) { return reference.renameTarget.value_or(reference.declaration); }

struct RenameSite {
    std::string file;
    Span span;
    auto operator<=>(const RenameSite&) const = default;
};

struct RenameIndex {
    const Workspace& workspace;
    std::vector<std::string> keys;

    explicit RenameIndex(const Workspace& workspace) : workspace(workspace) {
        for (const auto& source : workspace.sources) keys.push_back(source.origin ? std::string{} : pathKey(source.path));
    }

    std::optional<RenameSite> authored(SourceAnchor anchor) const {
        if (anchor.file >= keys.size() || keys[anchor.file].empty()) return {};
        if (auto span = workspace.sources[anchor.file].authoredSpan(anchor.span)) return RenameSite{keys[anchor.file], *span};
        return {};
    }

    const Source& source(std::size_t file) const {
        auto original = &workspace.sources[file];
        while (original->previous) original = original->previous.get();
        return *original;
    }
};

struct RenameDeclaration { bool exported{}, moduleAlias{}, implicitAlias{}; };

std::optional<RenameDeclaration> renameDeclaration(const Source& source, Span at, std::string_view newName) {
    auto syntax = indexFileSyntax(source);
    for (const auto& use : syntax.imports) if (use.alias.span == at) {
        if (use.kind != ImportKind::Runtime) return {};
        return RenameDeclaration{use.isPublic, !use.member, !use.member && use.path.back().span == at};
    }
    for (const auto& record : syntax.aggregates) {
        if (record.name.span == at) return RenameDeclaration{record.isPublic};
        for (const auto& field : record.fields) if (field.name.span == at) return RenameDeclaration{record.isPublic && field.isPublic};
        for (const auto& member : record.associated) if (member.name.span == at) return {};
    }
    for (const auto& item : syntax.enums) {
        if (item.name.span == at) return RenameDeclaration{item.isPublic};
        for (const auto& variant : item.cases) {
            if (variant.name.span == at) return RenameDeclaration{item.isPublic};
            for (const auto& field : variant.fields) if (field.name && field.name->span == at) return RenameDeclaration{item.isPublic};
        }
    }
    for (const auto& trait : syntax.traits) {
        if (trait.name.span == at) return RenameDeclaration{trait.isPublic};
        for (const auto& member : trait.associated) if (member.span == at) return {};
        for (const auto& method : trait.methods) {
            if (method.name.span == at) return {};
            for (const auto& parameter : method.parameters) if (parameter.name.span == at) return {};
        }
    }
    for (const auto& alias : syntax.aliases) if (alias.name.span == at) return RenameDeclaration{alias.isPublic};
    for (const auto& item : syntax.exceptions) if (item.name.span == at) return RenameDeclaration{item.isPublic};
    for (const auto& group : syntax.extensions) if (group.name.span == at) return RenameDeclaration{group.isPublic};
    for (const auto& fn : syntax.functions) {
        if (fn.name.span == at) {
            constexpr std::string_view protocol[] = {"equals", "hashCode", "iterator", "asyncIterator", "next", "message", "toString"};
            auto implicit = [&](std::string_view name) { return std::ranges::find(protocol, name) != std::ranges::end(protocol); };
            if (fn.initializer || fn.conversion || fn.foreignSymbol ||
                (!fn.owner && !fn.extensionGroup && (fn.name.text == "main" || newName == "main")) ||
                (fn.hasReceiver && (implicit(fn.name.text) || implicit(newName)))) return {};
            return RenameDeclaration{fn.isPublic};
        }
        for (const auto& parameter : fn.parameters) if (parameter.name.span == at) return RenameDeclaration{fn.isPublic};
    }
    return RenameDeclaration{};
}

struct RenameChange { std::string text; Span after{}; };
struct RenameFile {
    const Source& source;
    std::map<Span, RenameChange> edits{};
    std::string changed{};

    bool prepare() {
        std::size_t previous = 0;
        for (auto& [before, change] : edits) {
            if (before.begin < previous) return false;
            changed.append(source.text, previous, before.begin - previous);
            change.after.begin = changed.size();
            changed += change.text;
            change.after.end = changed.size();
            previous = before.end;
        }
        changed.append(source.text, previous, std::string::npos);
        return true;
    }

    std::optional<Span> translate(Span span) const {
        auto next = edits.lower_bound({span.begin, span.begin});
        if (next != edits.end() && next->first == span) return next->second.after;
        if (next != edits.end() && next->first == Span{span.begin, span.begin}) ++next;
        if (next != edits.end() && next->first.begin < span.end) return {};
        if (next == edits.begin()) return span;
        const auto& [before, change] = *std::prev(next);
        if (before.end > span.begin) return {};
        return Span{change.after.end + (span.begin - before.end), change.after.end + (span.end - before.end)};
    }
};
using RenameFiles = std::map<std::string, RenameFile>;

struct RenameBindings {
    std::set<RenameSite> declarations;
    std::set<std::tuple<RenameSite, std::optional<RenameSite>, std::optional<RenameSite>>> references;
    bool operator==(const RenameBindings&) const = default;
};

std::optional<RenameBindings> renameBindings(const RenameIndex& index, const RenameSite& target,
    const RenameFiles* files = nullptr, const RenameSite* alias = nullptr) {
    auto locate = [&](SourceAnchor anchor) -> std::optional<RenameSite> {
        auto site = index.authored(anchor);
        if (!site || !files) return site;
        if (alias && *site == target) return *alias;
        auto edited = files->find(site->file);
        if (edited == files->end()) return site;
        if (auto translated = edited->second.translate(site->span)) return RenameSite{std::move(site->file), *translated};
        return {};
    };
    RenameBindings result;
    for (const auto& [anchor, binding] : index.workspace.editor.bindings) {
        if (auto site = locate(anchor)) result.declarations.insert(std::move(*site));
        else if (index.authored(anchor)) return {};
    }
    for (const auto& reference : index.workspace.editor.references) {
        auto site = locate(reference.use);
        auto declaration = locate(reference.declaration);
        auto binding = locate(renameAnchor(reference));
        if ((!site && (index.authored(reference.use) || index.authored(renameAnchor(reference)) == target)) ||
            (!declaration && index.authored(reference.declaration)) || (!binding && index.authored(renameAnchor(reference)))) return {};
        if (site) result.references.emplace(std::move(*site), std::move(declaration), std::move(binding));
    }
    return result;
}

std::optional<SourceAnchor> bindingAt(const Workspace& workspace, const Source& source, std::size_t offset) {
    auto file = originalFile(workspace, source);
    if (!file) return {};
    const auto& current = workspace.sources[*file];
    const auto& bindings = workspace.editor.bindings;
    for (auto item = bindings.lower_bound({*file, {0, 0}}); item != bindings.end() && item->first.file == *file; ++item)
        if (auto span = current.authoredSpan(item->first.span); span && span->begin <= offset && offset < span->end) return item->first;
    return {};
}

json::Array highlights(const Workspace& workspace, const Source& source, SourceAnchor target) {
    auto file = originalFile(workspace, source);
    if (!file) return {};
    std::set<Span> spans;
    auto append = [&](SourceAnchor anchor) {
        if (anchor.file == *file) if (auto span = workspace.sources[*file].authoredSpan(anchor.span)) spans.insert(*span);
    };
    append(target);
    for (const auto& reference : workspace.editor.references) if (renameAnchor(reference) == target) append(reference.use);
    json::Array result;
    for (auto span : spans) result.push_back(json::Object{{"range", jsonRange(source, span)}, {"kind", 1}});
    return result;
}

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
    for (const auto& reference : workspace.editor.references)
        if (renameAnchor(reference) == target && (includeDeclaration || reference.use != target)) append(reference.use);
    return locations;
}

std::size_t requestOffset(const Source& source, const json::Object* position) {
    if (!position) throw Diagnostic{"LYLSP0001", "缺少 position"};
    auto line = position->getInteger("line"), character = position->getInteger("character");
    if (!line || !character || *line < 0 || *character < 0) throw Diagnostic{"LYLSP0001", "无效的 position"};
    return byteOffset(source, static_cast<std::size_t>(*line), static_cast<std::size_t>(*character));
}

int completionKind(EditorKind kind) {
    using enum EditorKind;
    switch (kind) {
        case Method: return 2;
        case Function: case Macro: return 3;
        case Constructor: return 4;
        case Field: return 5;
        case Variable: case Parameter: return 6;
        case Class: case TypeAlias: return 7;
        case Trait: return 8;
        case Module: case Extension: return 9;
        case Property: return 10;
        case Enum: return 13;
        case Keyword: return 14;
        case EnumCase: return 20;
        case Constant: return 21;
        case Struct: return 22;
        case Decorator: return 23;
        case TypeParameter: return 25;
    }
    std::abort();
}

int symbolKind(EditorKind kind) {
    using enum EditorKind;
    switch (kind) {
        case Module: case Extension: return 2;
        case Class: case TypeAlias: return 5;
        case Method: return 6;
        case Property: return 7;
        case Field: return 8;
        case Constructor: return 9;
        case Enum: return 10;
        case Trait: return 11;
        case Function: case Macro: case Decorator: return 12;
        case Variable: case Parameter: return 13;
        case Constant: return 14;
        case EnumCase: return 22;
        case Struct: return 23;
        case TypeParameter: return 26;
        case Keyword: std::abort();
    }
    std::abort();
}

constexpr std::array<std::string_view, 15> semanticTypes{"namespace", "type", "class", "enum", "interface", "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember", "function", "method", "macro", "decorator"};

int semanticType(EditorKind kind) {
    using enum EditorKind;
    switch (kind) {
        case Module: case Extension: return 0;
        case TypeAlias: return 1;
        case Class: return 2;
        case Enum: return 3;
        case Trait: return 4;
        case Struct: return 5;
        case TypeParameter: return 6;
        case Parameter: return 7;
        case Variable: case Constant: return 8;
        case Field: case Property: return 9;
        case EnumCase: return 10;
        case Function: return 11;
        case Method: case Constructor: return 12;
        case Macro: return 13;
        case Decorator: return 14;
        case Keyword: std::abort();
    }
    std::abort();
}

json::Object semanticLegend() {
    json::Array types;
    for (auto name : semanticTypes) types.emplace_back(name);
    return json::Object{{"tokenTypes", std::move(types)}, {"tokenModifiers", json::Array{"declaration", "readonly", "static", "async"}}};
}

json::Object semanticTokens(const Workspace& workspace, const Source& source, std::span<const EditorDefinition> macros) {
    struct Coloring { EditorKind kind; unsigned modifiers; };
    std::map<Span, Coloring> colors;
    if (auto file = originalFile(workspace, source)) {
        const auto& current = workspace.sources[*file];
        const auto& bindings = workspace.editor.bindings;
        for (const auto& reference : workspace.editor.references) {
            if (reference.use.file != *file) continue;
            auto span = current.authoredSpan(reference.use.span);
            if (!span) continue;
            auto binding = bindings.find(reference.declaration);
            if (!reference.kind && binding == bindings.end()) continue;
            auto kind = reference.kind ? *reference.kind : binding->second.kind;
            auto modifiers = binding == bindings.end() ? 0u : binding->second.modifiers;
            if (reference.use == renameAnchor(reference)) modifiers |= Declaration;
            colors.try_emplace(*span, Coloring{kind, modifiers});
        }
        for (auto item = bindings.lower_bound({*file, {0, 0}}); item != bindings.end() && item->first.file == *file; ++item)
            if (auto span = current.authoredSpan(item->first.span))
                colors.insert_or_assign(*span, Coloring{item->second.kind, item->second.modifiers | Declaration});
    }
    for (const auto& macro : macros) colors.insert_or_assign(macro.use, Coloring{macro.symbol.kind, 0});
    json::Array data;
    std::size_t line = 0, character = 0;
    for (const auto& token : syntaxTokens(source, false, true)) {
        auto found = colors.find(token.span);
        if (found == colors.end()) continue;
        if (token.kind != "name" && !(token.kind == "keyword" && found->second.kind == EditorKind::Constructor)) continue;
        auto start = lspPosition(source, token.span.begin), end = lspPosition(source, token.span.end);
        if (start.line != end.line) continue;
        data.emplace_back(start.line - line);
        data.emplace_back(start.column - (start.line == line ? character : 0));
        data.emplace_back(end.column - start.column);
        data.emplace_back(semanticType(found->second.kind));
        data.emplace_back(found->second.modifiers);
        line = start.line;
        character = start.column;
    }
    return json::Object{{"data", std::move(data)}};
}

json::Array documentSymbols(const Source& source, const std::vector<OutlineSymbol>& outline) {
    json::Array result;
    for (const auto& item : outline) {
        json::Object symbol{{"name", item.name}, {"kind", symbolKind(item.kind)},
            {"range", jsonRange(source, item.extent)}, {"selectionRange", jsonRange(source, item.selection)}};
        if (!item.children.empty()) symbol["children"] = documentSymbols(source, item.children);
        result.push_back(std::move(symbol));
    }
    return result;
}

bool macroKind(EditorKind kind) { return kind == EditorKind::Macro || kind == EditorKind::Decorator; }

json::Array workspaceSymbols(std::string query, const std::set<fs::path>& folders, const Documents& documents, EditorParseCache& cache) {
    auto standard = fromUtf8(pathKey(sdkRoot() / "share" / "lyra" / "std"));
    fs::path localCache;
    if (auto path = cacheOverride(); !path.empty()) localCache = fromUtf8(pathKey(path));
    auto excluded = [&](const fs::path& path) {
        return path == standard || inside(standard, path) || (!localCache.empty() && (path == localCache || inside(localCache, path)));
    };
    auto ignoredDirectory = [](const fs::path& path) {
        auto name = pathText(path.filename());
        return name.starts_with('.') || name == "build" || name == "dist" || name == "node_modules";
    };
    std::set<fs::path> searched, roots, files;
    auto addPackage = [&](const fs::path& path) {
        if (!searched.insert(path).second) return;
        for (const auto& root : indexLocalPackageRoots(path)) if (!excluded(root)) roots.insert(root);
    };
    for (const auto& folder : folders) {
        if (excluded(folder)) continue;
        if (fs::is_regular_file(folder / "lyra.json5")) { addPackage(folder); continue; }
        auto options = fs::directory_options::skip_permission_denied;
        if (fs::is_directory(folder / "src")) roots.insert(folder);
        if (!fs::is_directory(folder)) continue;
        for (auto entry = fs::recursive_directory_iterator(folder, options); entry != fs::recursive_directory_iterator{}; ++entry) {
            if (!entry->is_directory()) continue;
            const auto& path = entry->path();
            if (entry->is_symlink() || ignoredDirectory(path) || excluded(path)) { entry.disable_recursion_pending(); continue; }
            if (fs::is_regular_file(path / "lyra.json5")) { addPackage(path); entry.disable_recursion_pending(); }
        }
    }
    for (const auto& [key, document] : documents) {
        auto path = fromUtf8(key);
        if (excluded(path)) continue;
        if (path.extension() == ".ly") { files.insert(path); addPackage(packageRoot(path)); }
        else if (path.filename() == "lyra.json5") addPackage(path.parent_path());
    }
    for (const auto& root : roots) {
        auto src = root / "src";
        if (!fs::is_directory(src) || fs::is_symlink(src)) continue;
        auto options = fs::directory_options::skip_permission_denied;
        for (auto entry = fs::recursive_directory_iterator(src, options); entry != fs::recursive_directory_iterator{}; ++entry) {
            const auto& path = entry->path();
            if (entry->is_directory()) {
                bool macros = macroSourceDirectory(path.filename());
                if (entry->is_symlink() || (ignoredDirectory(path) && !macros) || excluded(path)) entry.disable_recursion_pending();
            } else if (path.extension() == ".ly" && !entry->is_symlink() && entry->is_regular_file()) files.insert(path);
        }
    }
    auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
        return text;
    };
    query = lower(std::move(query));
    json::Array result;
    for (const auto& path : files) {
        try {
            auto opened = documents.find(pathKey(path));
            auto source = opened != documents.end() ? sourceText(path, opened->second.text, {}) : loadSource(path, {});
            auto append = [&](const OutlineSymbol& symbol, const std::string& parent) {
                auto name = parent.empty() ? symbol.name : parent + '.' + symbol.name;
                if (result.size() >= 200 || (!lower(symbol.name).contains(query) && !lower(std::move(name)).contains(query))) return;
                json::Object item{{"name", symbol.name}, {"kind", symbolKind(symbol.kind)}, {"location", jsonLocation(source, symbol.selection)}};
                if (!parent.empty()) item["containerName"] = parent;
                result.push_back(std::move(item));
            };
            for (const auto& symbol : editorOutline(source, editorFileSyntax(source, cache))) {
                append(symbol, {});
                for (const auto& member : symbol.children) append(member, symbol.name);
            }
            if (result.size() >= 200) break;
        } catch (const Diagnostic&) {}
        catch (const fs::filesystem_error&) {}
    }
    return result;
}

json::Object markdown(std::string text) { return json::Object{{"kind", "markdown"}, {"value", std::move(text)}}; }

json::Value hoverText(std::string detail, std::string documentation) {
    std::string text;
    if (!detail.empty()) text = "```lyra\n" + detail + "\n```";
    if (!documentation.empty()) text += (text.empty() ? "" : "\n\n") + documentation;
    return text.empty() ? json::Value(nullptr) : json::Value(json::Object{{"contents", markdown(std::move(text))}});
}

std::string qualifiedPath(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& part : path) {
        if (!result.empty()) result += '.';
        result += editorIdentifier(part);
    }
    return result;
}

struct ImportAnchor { std::size_t offset; std::string_view newline; bool startsOnNewLine; };

ImportAnchor importAnchor(const Source& source) {
    const auto text = std::string_view(source.text);
    auto offset = text.size();
    std::size_t headerEnd = 0;
    for (const auto& token : syntaxTokens(source, true, true)) {
        if (token.kind != "comment" || text.substr(token.span.begin).starts_with("///")) {
            offset = source.lineStarts[source.position(token.span.begin).line - 1];
            if (offset < headerEnd) offset = headerEnd;
            break;
        }
        headerEnd = token.span.end;
    }
    auto newline = text.find('\n');
    std::string_view end = newline != text.npos && newline && text[newline - 1] == '\r' ? "\r\n" : "\n";
    return {offset, end, offset == 0 || text[offset - 1] == '\n'};
}

json::Object importEdit(const Source& source, const ImportAnchor& anchor, std::string directive) {
    std::string insertion;
    if (!anchor.startsOnNewLine) insertion += anchor.newline;
    insertion += std::move(directive);
    insertion += anchor.newline;
    return json::Object{{"range", jsonRange(source, {anchor.offset, anchor.offset})}, {"newText", std::move(insertion)}};
}

bool typeSymbol(EditorKind kind) {
    return kind == EditorKind::Class || kind == EditorKind::Struct || kind == EditorKind::Enum || kind == EditorKind::Trait ||
        kind == EditorKind::TypeAlias || kind == EditorKind::TypeParameter;
}

bool completionSymbol(CompletionPlace place, const EditorSymbol& symbol) {
    using enum CompletionPlace;
    if (place == Constraint || place == ConstraintMember) return symbol.kind == EditorKind::Trait || symbol.kind == EditorKind::Module;
    if (place == Type || place == TypeMember) return typeSymbol(symbol.kind) || symbol.kind == EditorKind::Module;
    return symbol.kind != EditorKind::Trait && symbol.kind != EditorKind::Extension;
}

std::string_view bareIdentifier(std::string_view name) {
    return name.starts_with('`') ? name.substr(1, name.size() - 2) : name;
}

json::Value completionItems(const Source& source, std::size_t offset, const SyntaxCompletion& syntax, const EditorModuleIndex* catalog,
    const EditorProbe* probe = nullptr, std::span<const EditorSymbol> generated = {}) {
    using enum CompletionPlace;
    const auto place = syntax.place;
    auto prefix = std::string_view(source.text).substr(syntax.replacement.begin, offset - syntax.replacement.begin);
    auto range = json::Value(jsonRange(source, syntax.replacement));
    json::Array items;
    std::set<std::pair<std::string, std::string>> seen;
    std::optional<ImportAnchor> anchor;
    auto append = [&](const EditorSymbol& symbol, std::string rank, const EditorImport* imported = nullptr) {
        auto filter = std::string_view(symbol.label);
        if (!prefix.starts_with('`') && filter.starts_with('`')) filter.remove_prefix(1);
        if (symbol.label.empty() || !filter.starts_with(prefix)) return;
        auto path = imported ? qualifiedPath(imported->path) : std::string{};
        if (!seen.emplace(symbol.label, path).second) return;
        auto insertion = symbol.insertion.empty() ? symbol.label : symbol.insertion;
        if (symbol.label.ends_with(':') && !symbol.insertion.empty()) {
            auto suffix = std::string_view(source.text).substr(syntax.replacement.end);
            auto next = suffix.find_first_not_of(" \t");
            if (next != suffix.npos && suffix[next] == ':') insertion = symbol.label.substr(0, symbol.label.size() - 1);
        }
        json::Object item{{"label", symbol.label}, {"kind", completionKind(symbol.kind)}, {"sortText", rank + symbol.label + path},
            {"textEdit", json::Object{{"range", range}, {"newText", std::move(insertion)}}}};
        if (filter != symbol.label) item["filterText"] = std::string(filter);
        if (!symbol.detail.empty()) item["detail"] = symbol.detail;
        if (!symbol.documentation.empty()) item["documentation"] = markdown(symbol.documentation);
        if (imported) {
            item["detail"] = (symbol.detail.empty() ? std::string{} : symbol.detail + " — ") + "import " + path;
            item["labelDetails"] = json::Object{{"description", path}};
            if (!anchor) anchor = importAnchor(source);
            auto directive = imported->kind == ImportKind::Macro ? "import macro " : "import decorator ";
            item["additionalTextEdits"] = json::Array{importEdit(source, *anchor, directive + path)};
        }
        items.push_back(std::move(item));
    };
    if (probe) {
        const auto& symbols = place == Member ? probe->members : probe->labels;
        for (const auto& symbol : symbols) append(symbol, "0");
    }
    for (const auto& symbol : syntax.suggestions) append(symbol, symbol.kind == EditorKind::Keyword ? "2" : "1");
    if (place == Statement || place == Expression || place == Type || place == Constraint) {
        for (const auto& symbol : generated) if (completionSymbol(place, symbol)) append(symbol, "1");
        if (catalog) for (const auto& symbol : catalog->visible) if (completionSymbol(place, symbol)) append(symbol, "1");
        for (const auto& builtin : editorBuiltins(place == Type, place == Constraint))
            if (builtin.label != "awaitOutcome" || syntax.asynchronous) append(builtin, "2");
    }
    if (catalog && syntax.qualifier && (place == Member || place == TypeMember || place == ConstraintMember) &&
        (!probe || !probe->receiverChecked || probe->namespaceReceiver)) {
        if (auto scope = catalog->namespaces.find(syntax.qualifier->text); scope != catalog->namespaces.end())
            for (const auto& member : scope->second.members) if (completionSymbol(place, member)) append(member, "1");
    }
    if (catalog && (place == Import || place == ImportMember)) for (const auto& candidate : catalog->imports) {
        if (candidate.kind != syntax.importKind || candidate.path.size() != syntax.importPath.size() + 1) continue;
        if (candidate.kind == ImportKind::Runtime && ((candidate.symbol.kind == EditorKind::Module) != (place == Import))) continue;
        if (!std::equal(syntax.importPath.begin(), syntax.importPath.end(), candidate.path.begin(), [](const Identifier& word, const std::string& segment) { return word.text == segment; })) continue;
        if (std::ranges::find(syntax.selectedImports, candidate.path.back(), &Identifier::text) != syntax.selectedImports.end()) continue;
        append(candidate.symbol, "1");
    }
    if (catalog && (place == DeclarationMacro || place == ExpressionMacro || place == Decorator)) {
        auto kind = place == DeclarationMacro ? MacroKind::Declaration : place == ExpressionMacro ? MacroKind::Expression : MacroKind::Decorator;
        for (const auto& macro : catalog->macros) if (macro.kind == kind) append(macro.symbol, "1");
        if (!prefix.empty()) for (const auto& candidate : catalog->imports)
            if (candidate.macro == kind && candidate.automatic) append(candidate.symbol, "3", &candidate);
    }
    return json::Object{{"isIncomplete", syntax.incomplete}, {"items", std::move(items)}};
}

json::Object codeAction(const Document& document, std::string title, std::string kind, json::Array edits) {
    return json::Object{{"title", std::move(title)}, {"kind", std::move(kind)}, {"edit", json::Object{
        {"documentChanges", json::Array{json::Object{{"textDocument", json::Object{{"uri", document.uri}, {"version", document.version}}}, {"edits", std::move(edits)}}}}
    }}};
}

json::Array importActions(const Source& source, const Document& document, const MissingName& missing, const EditorModuleIndex& catalog,
    Span diagnostic, Span requested) {
    const auto tokens = syntaxTokens(source, false, true);
    const auto text = std::string_view(source.text);
    std::optional<Span> site;
    bool receiver = false;
    for (auto current = tokens.begin(); current != tokens.end(); ++current) {
        const auto& token = *current;
        if (token.kind != "name" || token.span.begin < diagnostic.begin || token.span.end > diagnostic.end) continue;
        if (bareIdentifier(text.substr(token.span.begin, token.span.end - token.span.begin)) != missing.name) continue;
        if (site) return {};
        site = token.span;
        const auto next = std::next(current);
        receiver = next != tokens.end() && text.substr(next->span.begin, next->span.end - next->span.begin) == ".";
    }
    if (!site || requested.end < site->begin || requested.begin > site->end) return {};
    auto syntax = completeSyntax(source, site->end);
    auto unshadowed = [&](const std::string& alias) {
        return std::ranges::none_of(syntax.suggestions, [&](const auto& symbol) { return bareIdentifier(symbol.label) == alias; });
    };
    auto accepts = [&](EditorKind kind) {
        switch (missing.kind) {
            case MissingNameKind::Type: return typeSymbol(kind);
            case MissingNameKind::Constraint: return kind == EditorKind::Trait;
            case MissingNameKind::Call: return kind == EditorKind::Function || kind == EditorKind::Class || kind == EditorKind::Struct;
            case MissingNameKind::Value: return kind == EditorKind::Function || kind == EditorKind::Constant ||
                (receiver && (kind == EditorKind::Class || kind == EditorKind::Struct || kind == EditorKind::Enum));
        }
        std::abort();
    };
    json::Array reused, added;
    std::set<std::string> seen;
    auto occupied = catalog.occupied;
    for (bool types : {false, true}) for (const auto& builtin : editorBuiltins(types)) occupied.insert(builtin.label);
    for (const auto& candidate : catalog.imports) {
        if (candidate.kind != ImportKind::Runtime || candidate.module.empty() || !accepts(candidate.symbol.kind) ||
            candidate.path.back() != missing.name) continue;
        auto route = candidate.path;
        route.pop_back();
        auto path = qualifiedPath(route);
        const auto existing = std::ranges::find_if(catalog.namespaces, [&](const auto& entry) {
            return entry.second.module == candidate.module && unshadowed(entry.first);
        });
        auto alias = existing == catalog.namespaces.end() ? route.back() : existing->first;
        auto directive = "import " + path;
        if (existing == catalog.namespaces.end()) {
            for (unsigned suffix = 2; occupied.contains(alias) || !unshadowed(alias); ++suffix) alias = route.back() + std::to_string(suffix);
            if (alias != route.back()) directive += " as " + editorModuleAlias(alias);
        }
        auto name = editorModuleAlias(alias) + '.' + candidate.symbol.label;
        if (!seen.insert(existing == catalog.namespaces.end() ? path + ':' + name : name).second) continue;
        json::Array edits{json::Object{{"range", jsonRange(source, *site)}, {"newText", name}}};
        if (existing == catalog.namespaces.end()) edits.push_back(importEdit(source, importAnchor(source), directive));
        auto title = existing == catalog.namespaces.end() ? "添加导入：" + directive + " → " + name : "使用已导入的 " + name;
        auto& actions = existing == catalog.namespaces.end() ? added : reused;
        actions.push_back(codeAction(document, std::move(title), "quickfix", std::move(edits)));
    }
    for (auto& action : added) reused.push_back(std::move(action));
    return reused;
}

json::Array diagnosticActions(const Source& source, const Document& document, const std::vector<Diagnostic>& errors,
    const EditorModuleIndex* catalog, const json::Object& params) {
    const auto* context = params.getObject("context");
    const auto* only = context ? context->getArray("only") : nullptr;
    auto allows = [&](std::string_view kind) {
        return std::ranges::any_of(*only, [&](const auto& item) {
            auto requested = item.getAsString();
            return requested && (requested->empty() || std::string_view(*requested) == kind ||
                (kind.starts_with(*requested) && kind[requested->size()] == '.'));
        });
    };
    bool quick = !only || allows("quickfix"), fixAll = only && allows("source.fixAll.lyra");
    const auto* range = params.getObject("range");
    if (!range || (!quick && !fixAll)) return {};
    const Span requested{requestOffset(source, range->getObject("start")), requestOffset(source, range->getObject("end"))};
    const auto* selected = context ? context->getArray("diagnostics") : nullptr;
    auto key = pathKey(source.path);
    json::Array result;
    std::set<std::size_t> semicolons;
    for (const auto& error : errors) {
        if (!error.site || pathKey(fromUtf8(error.site->file)) != key) continue;
        const Span span{error.site->start.offset, error.site->end.offset};
        bool semicolon = error.removeTailSemicolon && span.begin < source.text.size() && span.end == span.begin + 1 && source.text[span.begin] == ';';
        if (fixAll && semicolon) semicolons.insert(span.begin);
        if (!quick || requested.end < span.begin || requested.begin > span.end) continue;
        if (selected && !selected->empty() && std::ranges::none_of(*selected, [&](const auto& item) {
            const auto* diag = item.getAsObject();
            const auto* selectedRange = diag ? diag->getObject("range") : nullptr;
            return selectedRange && diag->getString("code") == error.code && *selectedRange == jsonRange(source, span);
        })) continue;
        if (semicolon) result.push_back(codeAction(document, "删除末尾分号", "quickfix", json::Array{
            json::Object{{"range", jsonRange(source, span)}, {"newText", ""}}}));
        if (error.missingName && catalog)
            for (auto& action : importActions(source, document, *error.missingName, *catalog, span, requested)) result.push_back(std::move(action));
    }
    if (!semicolons.empty()) {
        json::Array edits;
        for (auto offset : semicolons) edits.push_back(json::Object{{"range", jsonRange(source, {offset, offset + 1})}, {"newText", ""}});
        result.push_back(codeAction(document, "删除错误的块尾分号", "source.fixAll.lyra", std::move(edits)));
    }
    return result;
}

std::string_view tokenText(const Source& source, const SyntaxToken& token) {
    return std::string_view(source.text).substr(token.span.begin, token.span.end - token.span.begin);
}

char closingDelimiter(std::string_view token) {
    if (token == "(") return ')';
    if (token == "[") return ']';
    if (token == "{") return '}';
    return '\0';
}

json::Array foldingRanges(const Source& source, const Module& syntax) {
    std::set<std::tuple<std::size_t, std::size_t, std::string_view>> ranges;
    auto lineAt = [&](std::size_t offset) { return static_cast<std::size_t>(std::ranges::upper_bound(source.lineStarts, offset) - source.lineStarts.begin() - 1); };
    auto fold = [&](std::size_t start, std::size_t end, std::string_view kind = {}) {
        if (start < end) ranges.emplace(start, end, kind);
    };
    std::vector<std::pair<char, std::size_t>> blocks;
    std::optional<std::pair<std::size_t, std::size_t>> lineComments;
    auto flushComments = [&] {
        if (lineComments) fold(lineComments->first, lineComments->second, "comment");
        lineComments.reset();
    };
    for (const auto& token : syntaxTokens(source, true, true)) {
        auto word = tokenText(source, token);
        auto line = lineAt(token.span.begin);
        if (token.kind == "comment") {
            auto prefix = std::string_view(source.text).substr(source.lineStarts[line], token.span.begin - source.lineStarts[line]);
            if (word.starts_with("//") && prefix.find_first_not_of(" \t") == prefix.npos) {
                if (lineComments && lineComments->second + 1 == line) lineComments->second = line;
                else { flushComments(); lineComments = {line, line}; }
                continue;
            }
            flushComments();
            fold(line, lineAt(token.span.end - 1), "comment");
            continue;
        }
        flushComments();
        if (token.kind != "token" || word.size() != 1) continue;
        if (auto close = closingDelimiter(word)) blocks.emplace_back(close, line);
        else if (word == "}" || word == ")" || word == "]") {
            auto open = std::ranges::find(blocks | std::views::reverse, word.front(), &std::pair<char, std::size_t>::first);
            if (open == blocks.rend()) continue;
            fold(open->second, line);
            blocks.erase(std::prev(open.base()), blocks.end());
        }
    }
    flushComments();
    std::map<std::size_t, std::size_t> imports;
    for (const auto& use : syntax.imports) {
        auto first = lineAt(use.path.front().span.begin);
        auto last = lineAt(std::max(use.alias.span.end, use.member ? use.member->span.end : 0));
        imports[first] = std::max(imports[first], last);
    }
    for (auto item = imports.begin(); item != imports.end();) {
        const auto first = item->first;
        auto last = item++->second;
        while (item != imports.end() && item->first <= last + 1) last = std::max(last, item++->second);
        fold(first, last, "imports");
    }
    json::Array result;
    for (const auto& [start, end, kind] : ranges) {
        json::Object range{{"startLine", start}, {"endLine", end}};
        if (!kind.empty()) range["kind"] = std::string(kind);
        result.push_back(std::move(range));
    }
    return result;
}

std::optional<Span> signatureOpening(const Source& source, const std::vector<SyntaxToken>& tokens, std::size_t offset) {
    struct Opening { char close; std::optional<Span> call; };
    std::vector<Opening> open;
    const SyntaxToken* previous = nullptr;
    for (const auto& token : tokens) {
        if (token.span.end > offset) break;
        auto word = tokenText(source, token);
        if (auto close = closingDelimiter(word)) {
            std::optional<Span> call;
            if (word == "(" && previous && (previous->kind == "name" || tokenText(source, *previous) == ")" ||
                tokenText(source, *previous) == "]" || tokenText(source, *previous).ends_with('>'))) call = token.span;
            open.push_back({close, call});
        } else if (!open.empty() && word.size() == 1 && word.front() == open.back().close) open.pop_back();
        previous = &token;
    }
    for (auto opening = open.rbegin(); opening != open.rend(); ++opening) if (opening->call) return opening->call;
    return {};
}

struct ArgumentRange { Span range; bool closed; };

ArgumentRange argumentRange(const Source& source, const std::vector<SyntaxToken>& tokens, Span open, std::size_t offset, bool preserveLabel) {
    std::vector<char> delimiters;
    auto start = open.end;
    std::optional<std::size_t> end;
    bool closed = false;
    for (const auto& token : tokens) {
        if (token.span.begin < open.end) continue;
        auto word = tokenText(source, token);
        if (delimiters.empty()) {
            if (word == ",") {
                if (token.span.end <= offset) start = token.span.end;
                else if (!end) end = token.span.begin;
                continue;
            }
            if (word == ")" || word == "}" || word == ";") {
                if (!end) end = token.span.begin;
                closed = word == ")";
                break;
            }
        }
        if (auto close = closingDelimiter(word)) delimiters.push_back(close);
        else if (!delimiters.empty() && word.size() == 1 && word.front() == delimiters.back()) delimiters.pop_back();
    }
    if (!closed) {
        auto line = source.text.find_first_of("\r\n", offset);
        auto boundary = std::min(line, end.value_or(source.text.size()));
        end = std::max(boundary, offset);
    }
    auto first = std::ranges::find_if(tokens, [&](const auto& token) { return token.span.begin >= start; });
    if (first != tokens.end() && first->span.begin <= *end) {
        start = first->span.begin;
        auto next = std::next(first);
        if (preserveLabel && first->kind == "name" && next != tokens.end() && next->span.end <= offset && tokenText(source, *next) == ":")
            start = next->span.end;
    }
    return {{std::min(start, *end), *end}, closed};
}

std::optional<std::string> probeText(const Source& source, std::size_t offset, const SyntaxCompletion& syntax, const EditorProbe& probe, bool signature) {
    auto text = source.text;
    if (!signature && syntax.place == CompletionPlace::Member) {
        text.replace(syntax.replacement.begin, syntax.replacement.end - syntax.replacement.begin, probe.marker);
        return text;
    }
    auto tokens = syntaxTokens(source, false, true);
    auto open = syntax.callOpen;
    if (!open && signature) open = signatureOpening(source, tokens, offset);
    if (!open) return {};
    auto argument = argumentRange(source, tokens, *open, offset, signature || !syntax.argumentLabel);
    if (offset > argument.range.end || offset < open->end) return {};
    auto replacement = probe.marker;
    if (!argument.closed) replacement += ')';
    text.replace(argument.range.begin, argument.range.end - argument.range.begin, replacement);
    return text;
}

json::Value signatureHelp(const EditorProbe& probe) {
    if (probe.signatures.empty()) return nullptr;
    json::Array signatures;
    for (const auto& signature : probe.signatures) {
        json::Array parameters;
        for (const auto& parameter : signature.parameters) parameters.push_back(json::Object{{"label", parameter.label}});
        json::Object information{{"label", signature.label}, {"parameters", std::move(parameters)}};
        if (signature.active) information["activeParameter"] = *signature.active;
        if (!signature.documentation.empty()) information["documentation"] = markdown(signature.documentation);
        signatures.push_back(std::move(information));
    }
    json::Object result{{"signatures", std::move(signatures)}, {"activeSignature", 0}};
    if (probe.signatures.front().active) result["activeParameter"] = *probe.signatures.front().active;
    return result;
}

json::Array inlayHints(const Workspace& workspace, const Source& source, Span range) {
    std::map<std::size_t, std::string_view> labels;
    for (const auto& hint : workspace.editor.inferredTypes) {
        const auto& authored = workspace.sources[hint.anchor.file];
        if (authored.path != source.path) continue;
        auto offset = authored.site(hint.anchor.span).end.offset;
        if (range.begin <= offset && offset < range.end) labels.try_emplace(offset, hint.type);
    }
    json::Array hints;
    for (const auto& [offset, type] : labels)
        hints.push_back(json::Object{{"position", jsonLspPosition(source, offset)}, {"label", ": " + std::string(type)}, {"kind", 1}});
    return hints;
}

json::Value errorResponse(const json::Value& id, int code, std::string message) {
    return json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", json::Object{{"code", code}, {"message", std::move(message)}}}};
}

class Server {
public:
    ~Server() { finishWorker(); }

    int run() {
        worker = std::thread([this] { work(); });
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
                auto response = handle(*method, params ? *params : json::Object{}, id);
                if (id && response) respond(*id, std::move(*response));
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
    Documents documents;
    std::set<fs::path> workspaceFolders;
    std::map<AnalysisView, Snapshot> snapshots;
    ParseCache parseCache;
    EditorParseCache editorParseCache;
    EditorParseCache workspaceSymbolCache;
    std::map<fs::path, PackageGraph> catalogPackages;
    std::map<fs::path, EditorModuleIndex> catalogs;
    std::map<std::string, std::vector<EditorSymbol>> generated;
    std::set<std::string> publishedClosed;
    std::map<WatchPattern, std::string> activeWatchers;
    std::size_t nextWatcher{};
    std::size_t nextRequest{};
    bool relativeWatchers{};
    bool shutdown{};
    bool exited{};
    std::atomic<bool> traceVerbose{};
    using Clock = std::chrono::steady_clock;
    struct DiagnosticsJob { std::size_t revision; fs::path changed; Documents documents; Clock::time_point ready; };
    std::mutex workMutex;
    std::condition_variable workReady;
    std::deque<std::function<void()>> requests;
    std::optional<DiagnosticsJob> pendingDiagnostics;
    std::size_t revision{};
    std::size_t generatedRevision{};
    bool stopping{};
    std::thread worker;

    void finishWorker() {
        if (!worker.joinable()) return;
        {
            std::lock_guard lock(workMutex);
            stopping = true;
        }
        workReady.notify_one();
        worker.join();
    }

    void work() {
        std::unique_lock lock(workMutex);
        while (true) {
            if (!requests.empty()) {
                auto request = std::move(requests.front());
                requests.pop_front();
                lock.unlock();
                request();
                lock.lock();
            } else if (pendingDiagnostics && (stopping || Clock::now() >= pendingDiagnostics->ready)) {
                auto job = std::move(*pendingDiagnostics);
                pendingDiagnostics.reset();
                lock.unlock();
                publishDiagnostics(job);
                lock.lock();
            } else if (stopping) return;
            else if (pendingDiagnostics) {
                auto ready = pendingDiagnostics->ready;
                workReady.wait_until(lock, ready);
            } else workReady.wait(lock);
        }
    }

    void scheduleDiagnostics(const fs::path& changed) {
        catalogs.clear();
        {
            std::lock_guard lock(workMutex);
            pendingDiagnostics = DiagnosticsJob{revision, changed, documents, Clock::now() + std::chrono::milliseconds(45)};
        }
        workReady.notify_one();
    }

    void invalidateAnalysis() {
        std::lock_guard lock(workMutex);
        ++revision;
        pendingDiagnostics.reset();
    }

    void enqueue(const json::Value& id, json::Value empty, std::function<json::Value()> task) {
        {
            std::lock_guard lock(workMutex);
            requests.push_back([this, id, empty = std::move(empty), task = std::move(task), requested = revision] {
                {
                    std::lock_guard lock(workMutex);
                    if (requested != revision) { respond(id, empty); return; }
                }
                try {
                    auto result = task();
                    std::lock_guard lock(workMutex);
                    respond(id, requested == revision ? std::move(result) : json::Value(empty));
                } catch (const Diagnostic&) { respond(id, empty); }
                catch (const PackageError&) { respond(id, empty); }
                catch (const fs::filesystem_error&) { respond(id, empty); }
                catch (const std::exception& error) { send(errorResponse(id, -32603, error.what())); }
            });
        }
        workReady.notify_one();
    }

    const EditorModuleIndex& catalog(const fs::path& path) {
        auto found = catalogs.find(path);
        if (found != catalogs.end()) return found->second;
        auto root = packageRoot(path);
        auto graph = catalogPackages.find(root);
        if (graph == catalogPackages.end()) graph = catalogPackages.emplace(root, indexPackages(root)).first;
        SourceOverlay overlays;
        for (const auto& [key, document] : documents) if (document.path.extension() == ".ly") overlays.emplace(fromUtf8(key), document.text);
        return catalogs.emplace(path, indexModule(graph->second, path, overlays, editorParseCache)).first->second;
    }

    WorkspaceInputs collectInputs(const AnalysisView& view, const Documents& documents) {
        const auto& root = view.root;
        WorkspaceInputs result;
        auto& inputs = result.inputs;
        auto& graph = result.graph;
        auto& overlays = result.overlays;
        std::vector<fs::path> extraOverlayPaths;
        trackFile(inputs, root / "lyra.json5");
        trackFile(inputs, root / "lyra.lock");
        recordTree(inputs, root / "src", true);
        if (view.test) {
            trackFile(inputs, *view.test);
            extraOverlayPaths.push_back(*view.test);
        }
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
        for (const auto& [key, document] : documents) {
            auto path = fromUtf8(key);
            if (path.extension() != ".ly") continue;
            for (const auto& [directory, resolved] : inputs.sourceTrees) if (inside(resolved, path))
                overlays.emplace(directory / path.lexically_relative(resolved), document.text);
        }
        for (const auto& path : extraOverlayPaths) if (auto document = documents.find(pathKey(path)); document != documents.end())
            overlays.emplace(path, document->second.text);
        return result;
    }

    const Workspace& analyze(const AnalysisView& view, const Documents& documents) {
        auto [inputs, graph, overlays] = collectInputs(view, documents);
        auto label = pathText(view.test.value_or(view.root));

        auto found = snapshots.find(view);
        if (found != snapshots.end() && found->second.inputs == inputs) {
            auto& previous = found->second;
            if (previous.overlays == overlays) {
                trace("Lyra analysis: reused package analysis for " + label);
                return previous.workspace;
            }
            if (auto updates = triviaOnlyUpdates(previous, inputs, overlays)) {
                for (auto& [index, source] : *updates) previous.workspace.sources[index] = std::move(source);
                previous.overlays = std::move(overlays);
                trace("Lyra analysis: reused checked workspace after trivia-only document edits for " + label);
                return previous.workspace;
            }
        }
        trace("Lyra analysis: rebuilt workspace and reran semantic checks for " + label);
        auto workspace = loadWorkspace(view.root, overlays, graph, parseCache, nullptr, view.test);
        return snapshots.insert_or_assign(view, Snapshot{std::move(inputs), std::move(overlays), std::move(workspace)}).first->second.workspace;
    }

    json::Value checkedRename(const AnalysisView& primary, const Documents& documents, const Workspace& workspace, SourceAnchor anchor,
                              std::string_view newName) {
        const auto& root = primary.root;
        RenameIndex initial(workspace);
        auto target = initial.authored(anchor);
        if (!target || newName == "_") return nullptr;
        auto declaration = renameDeclaration(initial.source(anchor.file), target->span, newName);
        if (!declaration) return nullptr;
        auto spelling = declaration->moduleAlias ? editorModuleAlias(newName) : editorIdentifier(newName);
        if (spelling.empty()) return nullptr;

        auto [inputs, graph, overlays] = collectInputs(primary, documents);
        const auto& snapshot = snapshots.at(primary);
        if (inputs != snapshot.inputs || overlays != snapshot.overlays) return nullptr;
        if (primary.test && pathKey(*primary.test) == target->file) declaration->exported = false;
        auto owner = packageRoot(fromUtf8(target->file));
        if ((!declaration->exported || !graph) && owner != root) return nullptr;
        std::set<fs::path> editable{root};
        std::vector<fs::path> additionalRoots;
        if (declaration->exported && graph) {
            if (graph->workspace == root && graph->members.size() == 1) return nullptr;
            auto package = std::ranges::find(graph->packages, owner, &ResolvedPackage::root);
            if (package == graph->packages.end() || std::ranges::find(graph->members, package->id) == graph->members.end()) return nullptr;
            std::map<std::string, const ResolvedPackage*> packages;
            for (const auto& member : graph->packages) packages.emplace(member.id, &member);
            std::map<std::string, bool> reachesOwner;
            auto reaches = [&](auto&& self, const std::string& id) -> bool {
                auto [cached, inserted] = reachesOwner.try_emplace(id, false);
                if (inserted) cached->second = id == package->id || std::ranges::any_of(packages.at(id)->dependencies,
                    [&](const auto& dependency) { return self(self, dependency.second); });
                return cached->second;
            };
            for (const auto& id : graph->members) if (reaches(reaches, id)) {
                const auto& member = packages.at(id)->root;
                editable.insert(member);
                if (member != root) additionalRoots.push_back(member);
            }
        }

        struct View {
            fs::path root;
            std::optional<PackageGraph> graph;
            SourceOverlay overlays;
            std::vector<std::pair<std::optional<fs::path>, const Workspace*>> checks{};
        };
        std::deque<Workspace> additional;
        std::vector<View> views;
        views.push_back({root, std::move(graph), std::move(overlays), {{primary.test, &workspace}}});
        for (const auto& member : additionalRoots) {
            auto current = collectInputs({member, {}}, documents);
            additional.push_back(loadWorkspace(member, current.overlays, current.graph, parseCache));
            views.push_back({member, std::move(current.graph), std::move(current.overlays), {{std::nullopt, &additional.back()}}});
        }
        if (declaration->exported && views.front().graph) for (auto& view : views) {
            if (view.root == root && primary.test) {
                additional.push_back(loadWorkspace(root, view.overlays, view.graph, parseCache));
                view.checks.emplace_back(std::nullopt, &additional.back());
            }
            auto paths = testEntries(view.root);
            std::set<fs::path> tests(paths.begin(), paths.end());
            for (const auto& [key, document] : documents) if (auto path = fromUtf8(key); isTestEntry(view.root, path)) tests.insert(std::move(path));
            for (const auto& path : tests) {
                if (view.root == root && primary.test == path) continue;
                if (auto document = documents.find(pathKey(path)); document != documents.end()) view.overlays.insert_or_assign(path, document->second.text);
                additional.push_back(loadWorkspace(view.root, view.overlays, view.graph, parseCache, nullptr, path));
                view.checks.emplace_back(path, &additional.back());
            }
        }

        RenameFiles files;
        auto add = [&](const RenameIndex& index, SourceAnchor use) {
            auto site = index.authored(use);
            if (!site || !std::ranges::any_of(editable, [&](const fs::path& package) {
                auto path = fromUtf8(site->file);
                return inside(package / "src", path) || inside(package / "tests", path);
            })) return false;
            const auto& source = index.source(use.file);
            auto [file, inserted] = files.try_emplace(site->file, RenameFile{source});
            if (!inserted && file->second.source.text != source.text) return false;
            bool insertAlias = declaration->implicitAlias && *site == *target;
            auto span = insertAlias ? Span{site->span.end, site->span.end} : site->span;
            file->second.edits.try_emplace(span, RenameChange{insertAlias ? " as " + spelling : spelling});
            return true;
        };
        if (!add(initial, anchor)) return nullptr;
        for (const auto& view : views) for (const auto& [testFile, before] : view.checks) {
            if (!before->diagnostics.errors.empty()) return nullptr;
            RenameIndex index(*before);
            for (const auto& reference : before->editor.references) if (index.authored(renameAnchor(reference)) == target) {
                if (!index.authored(reference.declaration) || !add(index, reference.use)) return nullptr;
            }
        }
        for (auto& [file, edits] : files) if (!edits.prepare()) return nullptr;

        const auto& targetFile = files.at(target->file);
        auto renamed = *target;
        if (declaration->implicitAlias) {
            auto insertion = targetFile.edits.at({target->span.end, target->span.end}).after;
            renamed.span = {insertion.end - spelling.size(), insertion.end};
        } else renamed.span = targetFile.edits.at(target->span).after;
        for (const auto& view : views) for (const auto& [testFile, before] : view.checks) {
            RenameIndex index(*before);
            auto expected = renameBindings(index, *target, &files, declaration->implicitAlias ? &renamed : nullptr);
            if (!expected) return nullptr;
            auto proposed = view.overlays;
            for (std::size_t file = 0; file < index.keys.size(); ++file)
                if (auto edited = files.find(index.keys[file]); edited != files.end()) proposed.insert_or_assign(index.source(file).path, edited->second.changed);
            auto checked = loadWorkspace(view.root, proposed, view.graph, parseCache, nullptr, testFile);
            if (!checked.diagnostics.errors.empty() || renameBindings(RenameIndex(checked), renamed) != expected) return nullptr;
        }

        json::Array changes;
        for (const auto& [file, edits] : files) {
            auto document = documents.find(file);
            auto path = fromUtf8(file);
            if (document == documents.end() && loadSource(path, {}).text != edits.source.text) return nullptr;
            if (document != documents.end() && document->second.text != edits.source.text) return nullptr;
            json::Array replacements;
            for (const auto& [span, change] : edits.edits)
                replacements.push_back(json::Object{{"range", jsonRange(edits.source, span)}, {"newText", change.text}});
            auto uri = document == documents.end() ? encodeUri(path) : document->second.uri;
            auto version = document == documents.end() ? json::Value(nullptr) : json::Value(document->second.version);
            changes.push_back(json::Object{{"textDocument", json::Object{{"uri", std::move(uri)}, {"version", std::move(version)}}},
                {"edits", std::move(replacements)}});
        }
        return json::Object{{"documentChanges", std::move(changes)}};
    }

    static void send(const json::Value& value) {
        auto body = llvm::formatv("{0}", value).str();
        static std::mutex output;
        std::lock_guard lock(output);
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
    }

    static void respond(const json::Value& id, json::Value result) {
        send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    }

    void trace(std::string message) const {
        if (traceVerbose) send(json::Object{{"jsonrpc", "2.0"}, {"method", "$/logTrace"}, {"params", json::Object{{"message", std::move(message)}}}});
    }

    void requestClient(std::string method, json::Object params) {
        send(json::Object{{"jsonrpc", "2.0"}, {"id", "lyra-" + std::to_string(nextRequest++)}, {"method", std::move(method)}, {"params", std::move(params)}});
    }

    void synchronizeWatchers(const std::map<AnalysisView, const Document*>& roots) {
        if (!relativeWatchers) return;
        std::set<WatchPattern> desired;
        for (const auto& [view, owner] : roots) {
            if (auto found = snapshots.find(view); found != snapshots.end()) {
                const auto& watchers = found->second.inputs.watchers;
                desired.insert(watchers.begin(), watchers.end());
            } else {
                desired.insert({view.root, "src/**/*.ly"});
                for (const auto* name : {"lyra.json5", "lyra.lock"}) desired.insert({view.root, name});
                if (view.test) desired.insert(watchFile(*view.test));
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

    std::optional<json::Value> handle(std::string_view method, const json::Object& params, const json::Value* id) {
        if (method == "initialize") {
            auto capabilities = params.getObject("capabilities");
            auto workspace = capabilities ? capabilities->getObject("workspace") : nullptr;
            auto watch = workspace ? workspace->getObject("didChangeWatchedFiles") : nullptr;
            relativeWatchers = watch && watch->getBoolean("dynamicRegistration").value_or(false) && watch->getBoolean("relativePatternSupport").value_or(false);
            if (const auto* folders = params.getArray("workspaceFolders")) updateWorkspaceFolders(*folders, true);
            else if (auto root = params.getString("rootUri"); root && root->starts_with("file://")) workspaceFolders.insert(fromUtf8(pathKey(decodeUri(*root))));
            return json::Object{{"capabilities", json::Object{
            {"positionEncoding", "utf-16"}, {"textDocumentSync", json::Object{{"openClose", true}, {"change", 2}}},
            {"completionProvider", json::Object{{"triggerCharacters", json::Array{".", "@", "#", "{", ":", "'", "\""}}}},
            {"codeActionProvider", json::Object{{"codeActionKinds", json::Array{"quickfix", "source.fixAll.lyra"}}}},
            {"signatureHelpProvider", json::Object{{"triggerCharacters", json::Array{"(", ",", ":"}}, {"retriggerCharacters", json::Array{")"}}}},
            {"hoverProvider", true}, {"definitionProvider", true}, {"referencesProvider", true},
            {"typeDefinitionProvider", true}, {"documentHighlightProvider", true}, {"documentSymbolProvider", true},
            {"foldingRangeProvider", true}, {"workspaceSymbolProvider", true},
            {"workspace", json::Object{{"workspaceFolders", json::Object{{"supported", true}, {"changeNotifications", true}}}}},
            {"semanticTokensProvider", json::Object{{"legend", semanticLegend()}, {"full", true}}},
            {"renameProvider", true}, {"documentFormattingProvider", true}, {"inlayHintProvider", true}}}};
        }
        if (method == "initialized" || method == "$/cancelRequest") return {};
        if (method == "$/setTrace") {
            auto value = params.getString("value");
            traceVerbose = value && *value == "verbose";
            return {};
        }
        if (method == "shutdown") { finishWorker(); shutdown = true; return json::Value(nullptr); }
        if (method == "exit") { exited = true; return {}; }
        if (method == "workspace/didChangeWorkspaceFolders") {
            if (const auto* event = params.getObject("event")) {
                if (const auto* removed = event->getArray("removed")) updateWorkspaceFolders(*removed, false);
                if (const auto* added = event->getArray("added")) updateWorkspaceFolders(*added, true);
            }
            return {};
        }
        if (id && method == "workspace/symbol") {
            auto query = params.getString("query").value_or("").str();
            enqueue(*id, json::Array{}, [this, query = std::move(query), folders = workspaceFolders, documents = documents] {
                return workspaceSymbols(query, folders, documents, workspaceSymbolCache);
            });
            return {};
        }
        if (method == "workspace/didChangeWatchedFiles") {
            auto changes = params.getArray("changes");
            if (!changes) return {};
            for (const auto& change : *changes) {
                auto entry = change.getAsObject();
                auto uri = entry ? entry->getString("uri") : std::nullopt;
                if (!uri || !uri->starts_with("file://")) continue;
                auto path = decodeUri(*uri);
                invalidateAnalysis();
                catalogPackages.clear();
                scheduleDiagnostics(path);
                break;
            }
            return {};
        }
        if (method == "textDocument/didOpen") {
            auto document = params.getObject("textDocument");
            if (!document) return {};
            auto uri = document->getString("uri"), text = document->getString("text");
            if (!uri || !text) return {};
            auto path = decodeUri(*uri);
            invalidateAnalysis();
            documents[pathKey(path)] = {path, uri->str(), text->str(), document->getInteger("version").value_or(0)};
            scheduleDiagnostics(path);
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
            invalidateAnalysis();
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
            scheduleDiagnostics(path);
            return {};
        }
        if (method == "textDocument/didClose") {
            auto document = params.getObject("textDocument");
            if (!document) return {};
            auto uri = document->getString("uri");
            if (!uri) return {};
            auto path = decodeUri(*uri);
            invalidateAnalysis();
            documents.erase(pathKey(path));
            scheduleDiagnostics(path);
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", uri->str()}, {"diagnostics", json::Array{}}}}});
            return {};
        }
        if (id && method.starts_with("textDocument/")) return documentRequest(method, params, *id);
        if (id) return json::Value(nullptr);
        return {};
    }

    void updateWorkspaceFolders(const json::Array& folders, bool added) {
        for (const auto& item : folders) if (const auto* folder = item.getAsObject()) {
            auto uri = folder->getString("uri");
            if (!uri || !uri->starts_with("file://")) continue;
            auto path = fromUtf8(pathKey(decodeUri(*uri)));
            if (added) workspaceFolders.insert(std::move(path));
            else workspaceFolders.erase(path);
        }
    }

    fs::path requestPath(const json::Object& params) const {
        auto document = params.getObject("textDocument");
        auto uri = document ? document->getString("uri") : std::nullopt;
        if (!uri) throw Diagnostic{"LYLSP0001", "缺少 textDocument.uri"};
        return decodeUri(*uri);
    }

    static json::Value unavailable(std::string_view method) {
        if (method == "textDocument/completion") return json::Object{{"isIncomplete", false}, {"items", json::Array{}}};
        if (method == "textDocument/semanticTokens/full") return json::Object{{"data", json::Array{}}};
        if (method == "textDocument/references" || method == "textDocument/inlayHint" || method == "textDocument/formatting" || method == "textDocument/codeAction" ||
            method == "textDocument/documentSymbol" || method == "textDocument/foldingRange" || method == "textDocument/documentHighlight") return json::Array{};
        return nullptr;
    }

    EditorProbe probe(const Source& source, std::size_t offset, const SyntaxCompletion& syntax, bool signature, const Documents& documents) {
        EditorProbe result{source.path, "__lyraEditorCursor"};
        while (source.text.contains(result.marker)) result.marker += '_';
        auto replacement = probeText(source, offset, syntax, result, signature);
        if (!replacement) return result;
        try {
            auto view = analysisView(source.path);
            auto [inputs, graph, overlays] = collectInputs(view, documents);
            overlays.insert_or_assign(source.path, std::move(*replacement));
            loadWorkspace(view.root, overlays, graph, parseCache, &result, view.test);
        } catch (const PackageError&) {}
        catch (const fs::filesystem_error&) {}
        return result;
    }

    std::optional<json::Value> documentRequest(std::string_view method, const json::Object& params, const json::Value& id) {
        auto path = requestPath(params);
        auto key = pathKey(path);
        auto opened = documents.find(key);
        if (opened == documents.end()) return unavailable(method);
        const auto& document = opened->second;
        if (path.filename() == "lyra.json5") {
            if (method != "textDocument/completion") return unavailable(method);
            auto source = sourceText(document.path, document.text, {});
            auto offset = requestOffset(source, params.getObject("position"));
            json::Array items;
            for (auto& entry : completeManifest(source, offset)) items.push_back(json::Object{
                {"label", std::move(entry.label)}, {"kind", entry.value ? 20 : 10}, {"detail", std::move(entry.detail)},
                {"textEdit", json::Object{{"range", jsonRange(source, entry.replacement)}, {"newText", std::move(entry.text)}}}});
            return json::Object{{"isIncomplete", false}, {"items", std::move(items)}};
        }
        if (method == "textDocument/documentSymbol" || method == "textDocument/foldingRange") {
            auto source = sourceText(document.path, document.text, {});
            const auto& syntax = editorFileSyntax(source, editorParseCache);
            if (method == "textDocument/documentSymbol") return documentSymbols(source, editorOutline(source, syntax));
            return foldingRanges(source, syntax);
        }
        if (method == "textDocument/completion" || method == "textDocument/signatureHelp") {
            auto source = sourceText(fromUtf8(key), document.text, {});
            auto offset = requestOffset(source, params.getObject("position"));
            auto syntax = completeSyntax(source, offset);
            if (method == "textDocument/signatureHelp") {
                enqueue(id, unavailable(method), [this, source = std::move(source), offset, syntax = std::move(syntax), documents = documents] {
                    return signatureHelp(probe(source, offset, syntax, true, documents));
                });
                return {};
            }
            using enum CompletionPlace;
            if (const auto* context = params.getObject("context"); syntax.place != ImportMember && context && context->getString("triggerCharacter") == "{")
                return unavailable(method);
            const EditorModuleIndex* index = nullptr;
            if (syntax.place == Statement || syntax.place == Expression || syntax.place == Type || syntax.place == Constraint || syntax.place == Import ||
                syntax.place == ImportMember || syntax.qualifier || syntax.place == DeclarationMacro || syntax.place == ExpressionMacro || syntax.place == Decorator) {
                try { index = &catalog(source.path); }
                catch (const PackageError&) {}
                catch (const fs::filesystem_error&) {}
            }
            std::vector<EditorSymbol> expanded;
            {
                std::lock_guard lock(workMutex);
                if (revision == generatedRevision) if (auto found = generated.find(key); found != generated.end()) expanded = found->second;
            }
            bool moduleMember = index && syntax.qualifier && index->namespaces.contains(syntax.qualifier->text);
            if ((syntax.place == Member && (!moduleMember || syntax.implicitReceiver)) || syntax.argumentLabel) {
                std::optional<EditorModuleIndex> copied = index ? std::optional(*index) : std::nullopt;
                enqueue(id, unavailable(method), [this, source = std::move(source), offset, syntax = std::move(syntax),
                    index = std::move(copied), expanded = std::move(expanded), documents = documents] {
                    auto query = probe(source, offset, syntax, false, documents);
                    return completionItems(source, offset, syntax, index ? &*index : nullptr, &query, expanded);
                });
                return {};
            }
            return completionItems(source, offset, syntax, index, nullptr, expanded);
        }
        if (method == "textDocument/codeAction") {
            std::optional<EditorModuleIndex> index;
            try { index = catalog(document.path); }
            catch (const PackageError&) {}
            catch (const fs::filesystem_error&) {}
            enqueue(id, unavailable(method), [this, params, key, index = std::move(index), documents = documents] {
                const auto& document = documents.at(key);
                const auto& workspace = analyze(analysisView(fromUtf8(key)), documents);
                auto source = sourceFor(workspace, document.path);
                if (!source || source->text != document.text) return json::Array{};
                return diagnosticActions(*source, document, workspace.diagnostics.errors, index ? &*index : nullptr, params);
            });
            return {};
        }
        if (method == "textDocument/formatting") {
            FormatOptions options;
            if (const auto* requested = params.getObject("options")) {
                auto tabSize = requested->getInteger("tabSize").value_or(options.tabSize);
                if (tabSize >= 1 && tabSize <= 32) options.tabSize = static_cast<unsigned>(tabSize);
                options.insertSpaces = requested->getBoolean("insertSpaces").value_or(options.insertSpaces);
            }
            auto source = sourceText(document.path, document.text, {});
            auto output = formatSource(source, options);
            if (output == source.text) return json::Array{};
            return json::Array{json::Object{{"range", jsonRange(source, {0, source.text.size()})}, {"newText", std::move(output)}}};
        }
        if (method == "textDocument/hover" || method == "textDocument/definition") {
            auto source = sourceText(document.path, document.text, {});
            auto offset = requestOffset(source, params.getObject("position"));
            try {
                const auto& definitions = catalog(fromUtf8(key)).definitions;
                json::Array locations;
                for (const auto& definition : definitions) if (definition.use.begin <= offset && offset < definition.use.end) {
                    if (method == "textDocument/hover") return hoverText(definition.symbol.detail, definition.symbol.documentation);
                    auto opened = documents.find(pathKey(definition.path));
                    auto target = opened != documents.end() ? sourceText(definition.path, opened->second.text, {}) : loadSource(definition.path, {});
                    locations.push_back(jsonLocation(target, definition.target));
                }
                if (!locations.empty()) return locations;
            } catch (const Diagnostic&) {}
            catch (const PackageError&) {}
            catch (const fs::filesystem_error&) {}
        }
        if (method == "textDocument/semanticTokens/full" || method == "textDocument/documentHighlight") {
            std::vector<EditorDefinition> macros;
            if (document.text.find_first_of("#@") != std::string::npos || document.text.contains("import macro") || document.text.contains("import decorator")) {
                try { for (const auto& item : catalog(path).definitions) if (macroKind(item.symbol.kind)) macros.push_back(item); }
                catch (const Diagnostic&) {}
                catch (const PackageError&) {}
                catch (const fs::filesystem_error&) {}
            }
            enqueue(id, unavailable(method), [this, method = std::string(method), params, key, macros = std::move(macros), documents = documents] {
                return checkedRequest(method, params, documents.at(key), documents, macros);
            });
            return {};
        }
        if (method != "textDocument/inlayHint" && method != "textDocument/definition" && method != "textDocument/references" &&
            method != "textDocument/rename" && method != "textDocument/hover" && method != "textDocument/typeDefinition") return unavailable(method);
        enqueue(id, unavailable(method), [this, method = std::string(method), params, key, documents = documents] {
            return checkedRequest(method, params, documents.at(key), documents);
        });
        return {};
    }

    json::Value checkedRequest(std::string_view method, const json::Object& params, const Document& document, const Documents& documents,
        std::span<const EditorDefinition> macros = {}) {
        const auto& path = document.path;
        auto view = analysisView(fromUtf8(pathKey(path)));
        const auto& workspace = analyze(view, documents);
        auto source = sourceFor(workspace, path);
        if (!source || source->text != document.text) return unavailable(method);
        if (method == "textDocument/semanticTokens/full") return semanticTokens(workspace, *source, macros);
        if (method == "textDocument/inlayHint") {
            auto range = params.getObject("range");
            if (!range) throw Diagnostic{"LYLSP0001", "缺少 range"};
            return inlayHints(workspace, *source, {requestOffset(*source, range->getObject("start")), requestOffset(*source, range->getObject("end"))});
        }
        auto offset = requestOffset(*source, params.getObject("position"));
        auto reference = referenceAt(workspace, *source, offset);
        if (method == "textDocument/documentHighlight") {
            auto target = std::ranges::find_if(macros, [&](const EditorDefinition& item) { return item.use.begin <= offset && offset < item.use.end; });
            if (target != macros.end()) {
                json::Array matches;
                for (const auto& item : macros) if (item.path == target->path && item.target == target->target)
                    matches.push_back(json::Object{{"range", jsonRange(*source, item.use)}, {"kind", 1}});
                return matches;
            }
            auto symbol = reference ? std::optional(renameAnchor(*reference)) : bindingAt(workspace, *source, offset);
            return symbol ? highlights(workspace, *source, *symbol) : json::Array{};
        }
        if (method == "textDocument/typeDefinition") {
            auto symbol = reference ? std::optional(reference->declaration) : bindingAt(workspace, *source, offset);
            if (symbol) if (auto binding = workspace.editor.bindings.find(*symbol); binding != workspace.editor.bindings.end() && binding->second.typeDefinition) {
                auto location = jsonLocation(workspace, *binding->second.typeDefinition);
                if (!location.empty()) return json::Array{std::move(location)};
            }
            return json::Value(nullptr);
        }
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
            if (!workspace.diagnostics.errors.empty()) return nullptr;
            auto newName = params.getString("newName");
            auto target = reference ? std::optional(renameAnchor(*reference)) : bindingAt(workspace, *source, offset);
            return newName && target ? checkedRename(view, documents, workspace, *target, *newName) : json::Value(nullptr);
        }
        if (method == "textDocument/hover") {
            if (!reference) return json::Value(nullptr);
            if (auto description = std::ranges::find(workspace.editor.descriptions, reference->declaration, &EditorDescription::anchor);
                description != workspace.editor.descriptions.end()) return hoverText(description->detail, description->documentation);
            auto resolved = resolveAnchor(workspace, reference->declaration);
            if (!resolved) return json::Value(nullptr);
            const auto& definition = *resolved->first;
            auto site = definition.site(resolved->second);
            auto start = definition.lineStarts[site.start.line - 1];
            auto end = definition.text.find_first_of("\r\n", start);
            if (end == std::string::npos) end = definition.text.size();
            auto signature = std::string_view(definition.text).substr(start, end - start);
            return hoverText(std::string(signature), editorDocumentation(definition, resolved->second.begin));
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

    void publishDiagnostics(const DiagnosticsJob& job) {
        std::map<std::string, json::Array> byUri;
        auto changedKey = pathKey(job.changed);
        const auto& documents = job.documents;
        std::map<AnalysisView, const Document*> roots;
        for (const auto& [key, document] : documents) {
            auto path = fromUtf8(key);
            if (path.extension() != ".ly") continue;
            auto view = analysisView(path);
            auto relative = path.lexically_relative(view.root);
            if (std::ranges::any_of(relative, [](const auto& part) { return pathText(part).starts_with('.'); })) continue;
            if (key == changedKey) roots.insert_or_assign(std::move(view), &document);
            else roots.try_emplace(std::move(view), &document);
        }
        std::erase_if(snapshots, [&](const auto& entry) { return !roots.contains(entry.first); });
        std::map<std::string, std::optional<Source>> reportSources;
        auto sourceAt = [&](const std::string& file) -> const Source* {
            auto path = fromUtf8(file);
            if (!path.is_absolute()) return nullptr;
            auto key = pathKey(path);
            auto [entry, inserted] = reportSources.try_emplace(key);
            if (inserted) {
                auto open = documents.find(key);
                try {
                    if (open != documents.end()) entry->second = sourceText(path, open->second.text, {});
                    else if (path.extension() == ".ly" && fs::is_regular_file(path)) entry->second = loadSource(path, {});
                } catch (const Diagnostic&) {}
                catch (const fs::filesystem_error&) {}
            }
            return entry->second ? &*entry->second : nullptr;
        };
        auto append = [&](const Diagnostic& error, const Document& owner, int severity = 1) {
            auto uri = owner.uri;
            const Source* source = sourceAt(pathText(owner.path));
            Span span{0, 0};
            if (error.site) {
                auto document = documents.find(pathKey(fromUtf8(error.site->file)));
                if (auto located = sourceAt(error.site->file); located && error.site->end.offset <= located->text.size()) {
                    source = located;
                    uri = document != documents.end() ? document->second.uri : encodeUri(source->path);
                    span = {error.site->start.offset, error.site->end.offset};
                }
            }
            auto message = error.message;
            if (!error.hint.empty()) message += "\n" + error.hint;
            json::Object serialized{{"severity", severity}, {"source", "lyra"}, {"code", error.code}, {"message", std::move(message)}, {"range", jsonRange(*source, span)}};
            json::Array related;
            for (const auto& note : error.notes) if (auto target = sourceAt(note.site.file)) {
                if (note.site.end.offset <= target->text.size()) related.push_back(json::Object{
                    {"location", jsonLocation(*target, {note.site.start.offset, note.site.end.offset})}, {"message", note.message}});
            }
            if (!related.empty()) serialized["relatedInformation"] = std::move(related);
            json::Value item(std::move(serialized));
            auto& diagnostics = byUri[uri];
            if (std::ranges::find(diagnostics, item) == diagnostics.end()) diagnostics.push_back(std::move(item));
        };
        std::map<std::string, std::vector<EditorSymbol>> expanded;
        for (const auto& [key, document] : documents) if (document.path.filename() == "lyra.json5") {
            auto source = sourceText(document.path, document.text, {});
            if (auto error = validateManifest(source)) append(*error, document);
        }
        for (const auto& [view, owner] : roots) {
            try {
                const auto& workspace = analyze(view, documents);
                for (const auto& warning : workspace.diagnostics.warnings) append(warning, *owner, 2);
                for (const auto& error : workspace.diagnostics.errors) append(error, *owner);
                for (const auto& [key, document] : documents) if (auto source = sourceFor(workspace, document.path))
                    for (const auto& declaration : workspace.generated) if (workspace.sources[declaration.file].moduleId == source->moduleId)
                        expanded[key].push_back(declaration.symbol);
            }
            catch (const Diagnostic& error) { append(error, *owner); }
            catch (const PackageError& error) { append({error.code, error.message}, *owner); }
            catch (const fs::filesystem_error& error) { append({"LYI0001", error.what()}, *owner); }
        }
        std::lock_guard lock(workMutex);
        if (job.revision != revision) return;
        generated = std::move(expanded);
        generatedRevision = job.revision;
        synchronizeWatchers(roots);
        for (const auto& [key, document] : documents) {
            auto found = byUri.find(document.uri);
            auto diagnostics = found == byUri.end() ? json::Array{} : std::move(found->second);
            if (found != byUri.end()) byUri.erase(found);
            publishedClosed.erase(document.uri);
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", document.uri}, {"version", document.version}, {"diagnostics", std::move(diagnostics)}}}});
        }
        for (const auto& uri : publishedClosed) if (!byUri.contains(uri))
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", uri}, {"diagnostics", json::Array{}}}}});
        publishedClosed.clear();
        for (auto& [uri, diagnostics] : byUri) {
            publishedClosed.insert(uri);
            send(json::Object{{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", json::Object{{"uri", uri}, {"diagnostics", std::move(diagnostics)}}}});
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
