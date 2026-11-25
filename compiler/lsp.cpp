module;
#include <algorithm>
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    std::vector<Diagnostic> warnings;
    std::optional<Diagnostic> error;
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

Workspace loadWorkspace(const fs::path& root, const SourceOverlay& overlays, const std::optional<PackageGraph>& graph, ParseCache& parseCache, EditorProbe* probe = nullptr) {
    Workspace workspace;
    workspace.editor.probe = probe;
    try {
        Module syntax;
        if (graph) {
            auto program = loadProgram(*graph, graph->entry, {}, &overlays, &parseCache, probe != nullptr);
            syntax = std::move(program.syntax);
            workspace.sources = std::move(program.sources);
            workspace.editor.references = std::move(program.references);
        } else {
            workspace.sources = loadSources(root);
            for (auto& source : workspace.sources) if (auto found = overlays.find(source.path); found != overlays.end()) source = sourceText(source.path, found->second, {});
            if (probe) {
                auto found = std::ranges::find(workspace.sources, probe->path, &Source::path);
                if (found == workspace.sources.end()) return workspace;
                workspace.sources = {std::move(*found)};
                syntax = indexFileSyntax(workspace.sources.front());
            } else syntax = parse(workspace.sources);
        }
        if (!probe) workspace.generated = editorDeclarations(syntax, workspace.sources, true);
        auto checked = check(std::move(syntax), workspace.sources, false, nullptr, &workspace.editor);
        workspace.warnings = std::move(checked.warnings);
    } catch (const Diagnostic& error) { workspace.error = error; }
    catch (const EditorProbeFinished&) {}
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
        for (const auto* name : {".macros", ".expressions", ".decorators"}) {
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
    if (previous.inputs != inputs || previous.overlays == overlays || previous.workspace.error)
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

const NameReference* referenceAt(const Workspace& workspace, const fs::path& path, std::size_t offset) {
    auto matches = [&](SourceAnchor anchor) {
        if (anchor.file >= workspace.sources.size()) return false;
        const auto& source = workspace.sources[anchor.file];
        auto site = source.userSite(anchor.span);
        return pathKey(fromUtf8(site.file)) == pathKey(path) && site.start.offset <= offset && (offset < site.end.offset || site.start.offset == site.end.offset);
    };
    for (const auto& reference : workspace.editor.references) if (matches(reference.use)) return &reference;
    for (const auto& reference : workspace.editor.references) if (matches(reference.declaration)) return &reference;
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
    for (const auto& reference : workspace.editor.references) if (renameAnchor(reference) == target) append(reference.use);
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

json::Object automaticImport(const Source& source, const ImportAnchor& anchor, const EditorImport& suggestion) {
    std::string insertion;
    if (!anchor.startsOnNewLine) insertion += anchor.newline;
    insertion += "import ";
    if (suggestion.kind == ImportKind::Macro) insertion += "macro ";
    if (suggestion.kind == ImportKind::Decorator) insertion += "decorator ";
    insertion += qualifiedPath(suggestion.path);
    insertion += anchor.newline;
    return json::Object{{"range", jsonRange(source, {anchor.offset, anchor.offset})}, {"newText", std::move(insertion)}};
}

bool typeSymbol(EditorKind kind) {
    return kind == EditorKind::Class || kind == EditorKind::Struct || kind == EditorKind::Enum || kind == EditorKind::Trait ||
        kind == EditorKind::TypeAlias || kind == EditorKind::TypeParameter;
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
            item["additionalTextEdits"] = json::Array{automaticImport(source, *anchor, *imported)};
        }
        items.push_back(std::move(item));
    };
    if (probe) {
        const auto& symbols = place == Member ? probe->members : probe->labels;
        for (const auto& symbol : symbols) append(symbol, "0");
    }
    for (const auto& symbol : syntax.suggestions) append(symbol, symbol.kind == EditorKind::Keyword ? "2" : "1");
    if (place == Statement || place == Expression || place == Type || place == Constraint) {
        auto accepts = [&](const EditorSymbol& symbol) {
            if (place == Constraint) return symbol.kind == EditorKind::Trait;
            if (place == Type) return typeSymbol(symbol.kind) && symbol.kind != EditorKind::Trait;
            return symbol.kind != EditorKind::Trait && symbol.kind != EditorKind::Extension;
        };
        for (const auto& symbol : generated) if (accepts(symbol)) append(symbol, "1");
        if (catalog) for (const auto& symbol : catalog->visible) if (accepts(symbol)) append(symbol, "1");
        for (const auto& builtin : editorBuiltins(place == Type, place == Constraint))
            if (builtin.label != "awaitOutcome" || syntax.asynchronous) append(builtin, "2");
        if (catalog && !prefix.empty()) for (const auto& candidate : catalog->imports)
            if (candidate.kind == ImportKind::Runtime && candidate.automatic && accepts(candidate.symbol) && !seen.contains({candidate.symbol.label, ""}))
                append(candidate.symbol, "3", &candidate);
    }
    if (catalog && place == Import) for (const auto& candidate : catalog->imports) {
        if (candidate.kind != syntax.importKind || candidate.path.size() != syntax.importPath.size() + 1) continue;
        if (!std::equal(syntax.importPath.begin(), syntax.importPath.end(), candidate.path.begin(), [](const Identifier& word, const std::string& segment) { return word.text == segment; })) continue;
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

std::string_view tokenText(const Source& source, const SyntaxToken& token) {
    return std::string_view(source.text).substr(token.span.begin, token.span.end - token.span.begin);
}

char closingDelimiter(std::string_view token) {
    if (token == "(") return ')';
    if (token == "[") return ']';
    if (token == "{") return '}';
    return '\0';
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
    std::map<fs::path, Snapshot> snapshots;
    ParseCache parseCache;
    EditorParseCache editorParseCache;
    std::map<fs::path, PackageGraph> catalogPackages;
    std::map<fs::path, EditorModuleIndex> catalogs;
    std::map<std::string, std::vector<EditorSymbol>> generated;
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

    WorkspaceInputs collectInputs(const fs::path& root, const Documents& documents) {
        WorkspaceInputs result;
        auto& inputs = result.inputs;
        auto& graph = result.graph;
        auto& overlays = result.overlays;
        std::vector<fs::path> extraOverlayPaths;
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

    const Workspace& analyze(const fs::path& root, const Documents& documents) {
        auto [inputs, graph, overlays] = collectInputs(root, documents);

        auto found = snapshots.find(root);
        if (found != snapshots.end() && found->second.inputs == inputs) {
            auto& previous = found->second;
            if (previous.overlays == overlays) {
                trace("Lyra analysis: reused package analysis for " + pathText(root));
                return previous.workspace;
            }
            if (auto updates = triviaOnlyUpdates(previous, inputs, overlays)) {
                for (auto& [index, source] : *updates) previous.workspace.sources[index] = std::move(source);
                previous.overlays = std::move(overlays);
                trace("Lyra analysis: reused checked workspace after trivia-only document edits for " + pathText(root));
                return previous.workspace;
            }
        }
        trace("Lyra analysis: rebuilt workspace and reran semantic checks for " + pathText(root));
        auto workspace = loadWorkspace(root, overlays, graph, parseCache);
        return snapshots.insert_or_assign(root, Snapshot{std::move(inputs), std::move(overlays), std::move(workspace)}).first->second.workspace;
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

    std::optional<json::Value> handle(std::string_view method, const json::Object& params, const json::Value* id) {
        if (method == "initialize") {
            auto capabilities = params.getObject("capabilities");
            auto workspace = capabilities ? capabilities->getObject("workspace") : nullptr;
            auto watch = workspace ? workspace->getObject("didChangeWatchedFiles") : nullptr;
            relativeWatchers = watch && watch->getBoolean("dynamicRegistration").value_or(false) && watch->getBoolean("relativePatternSupport").value_or(false);
            return json::Object{{"capabilities", json::Object{
            {"positionEncoding", "utf-16"}, {"textDocumentSync", json::Object{{"openClose", true}, {"change", 2}}},
            {"completionProvider", json::Object{{"triggerCharacters", json::Array{".", "@", "#"}}}},
            {"signatureHelpProvider", json::Object{{"triggerCharacters", json::Array{"(", ",", ":"}}, {"retriggerCharacters", json::Array{")"}}}},
            {"hoverProvider", true}, {"definitionProvider", true}, {"referencesProvider", true},
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

    fs::path requestPath(const json::Object& params) const {
        auto document = params.getObject("textDocument");
        auto uri = document ? document->getString("uri") : std::nullopt;
        if (!uri) throw Diagnostic{"LYLSP0001", "缺少 textDocument.uri"};
        return decodeUri(*uri);
    }

    static json::Value unavailable(std::string_view method) {
        if (method == "textDocument/completion") return json::Object{{"isIncomplete", false}, {"items", json::Array{}}};
        if (method == "textDocument/references" || method == "textDocument/inlayHint" || method == "textDocument/formatting") return json::Array{};
        return nullptr;
    }

    EditorProbe probe(const Source& source, std::size_t offset, const SyntaxCompletion& syntax, bool signature, const Documents& documents) {
        EditorProbe result{source.path, "__lyraEditorCursor"};
        while (source.text.contains(result.marker)) result.marker += '_';
        auto replacement = probeText(source, offset, syntax, result, signature);
        if (!replacement) return result;
        try {
            auto root = packageRoot(source.path);
            auto [inputs, graph, overlays] = collectInputs(root, documents);
            overlays.insert_or_assign(source.path, std::move(*replacement));
            loadWorkspace(root, overlays, graph, parseCache, &result);
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
            const EditorModuleIndex* index = nullptr;
            if (syntax.place == Statement || syntax.place == Expression || syntax.place == Type || syntax.place == Constraint || syntax.place == Import ||
                syntax.place == DeclarationMacro || syntax.place == ExpressionMacro || syntax.place == Decorator) {
                try { index = &catalog(source.path); }
                catch (const PackageError&) {}
                catch (const fs::filesystem_error&) {}
            }
            std::vector<EditorSymbol> expanded;
            {
                std::lock_guard lock(workMutex);
                if (revision == generatedRevision) if (auto found = generated.find(key); found != generated.end()) expanded = found->second;
            }
            if (syntax.place == Member || syntax.argumentLabel) {
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
        if (method != "textDocument/inlayHint" && method != "textDocument/definition" && method != "textDocument/references" &&
            method != "textDocument/rename" && method != "textDocument/hover") return unavailable(method);
        enqueue(id, unavailable(method), [this, method = std::string(method), params, key, documents = documents] {
            return checkedRequest(method, params, documents.at(key), documents);
        });
        return {};
    }

    json::Value checkedRequest(std::string_view method, const json::Object& params, const Document& document, const Documents& documents) {
        const auto& path = document.path;
        const auto& workspace = analyze(packageRoot(fromUtf8(pathKey(path))), documents);
        auto source = sourceFor(workspace, path);
        if (!source || source->text != document.text) return unavailable(method);
        if (method == "textDocument/inlayHint") {
            auto range = params.getObject("range");
            if (!range) throw Diagnostic{"LYLSP0001", "缺少 range"};
            return inlayHints(workspace, *source, {requestOffset(*source, range->getObject("start")), requestOffset(*source, range->getObject("end"))});
        }
        auto offset = requestOffset(*source, params.getObject("position"));
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
            if (workspace.error || !reference) return unavailable(method);
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
        auto append = [&](const Diagnostic& error, const Document& owner, int severity = 1) {
            const Document* target = &owner;
            Span span{0, 0};
            if (error.site) {
                auto document = documents.find(pathKey(fromUtf8(error.site->file)));
                if (document == documents.end()) return;
                target = &document->second;
                span = {error.site->start.offset, error.site->end.offset};
            }
            auto source = sourceText(target->path, target->text, {});
            json::Value item = json::Object{{"severity", severity}, {"code", error.code}, {"message", error.message}, {"range", jsonRange(source, span)}};
            auto& diagnostics = byUri[target->uri];
            if (std::ranges::find(diagnostics, item) == diagnostics.end()) diagnostics.push_back(std::move(item));
        };
        std::map<std::string, std::vector<EditorSymbol>> expanded;
        for (const auto& [root, owner] : roots) {
            try {
                const auto& workspace = analyze(root, documents);
                for (const auto& warning : workspace.warnings) append(warning, *owner, 2);
                if (workspace.error) append(*workspace.error, *owner);
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
