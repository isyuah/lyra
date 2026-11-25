module;
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

export module lyra.packages;
import lyra.platform;
import lyra.source;

export namespace lyra {

struct PackageOptions { bool locked = false, offline = false, update = false, readOnly = false; };
struct PackageError { std::string code, message; std::optional<Span> span{}; };
struct ResolvedPackage {
    std::string id, name, version;
    std::filesystem::path root;
    std::map<std::string, std::string> dependencies;
    bool library{};
    std::map<std::string, std::map<std::string, std::filesystem::path>> macroInputs{};
};
struct PackageGraph {
    std::filesystem::path workspace;
    std::string entry;
    std::vector<ResolvedPackage> packages;
    std::vector<std::string> warnings, missingOutputs, members;
};
struct ManifestCompletion { Span replacement; std::string label, text, detail; bool value{}; };

PackageGraph resolvePackages(const std::filesystem::path& directory, PackageOptions options = {});
PackageGraph indexPackages(const std::filesystem::path& directory);
std::vector<std::filesystem::path> indexLocalPackageRoots(const std::filesystem::path& directory);
std::vector<ManifestCompletion> completeManifest(const Source& source, std::size_t offset);
std::optional<Diagnostic> validateManifest(const Source& source);
int packageCommand(const std::vector<std::string_view>& args);

}

namespace lyra {
namespace {
namespace fs = std::filesystem;
namespace json = llvm::json;
using std::string;
using std::string_view;
using Strings = std::vector<string>;
using Files = std::map<string, string>;
constexpr char manifestFile[] = "lyra.json5";
constexpr auto maxBytes = 64u * 1024u * 1024u;
constexpr auto maxFiles = 8192u;

[[noreturn]] void fail(string code, string message) { throw PackageError{std::move(code), std::move(message)}; }
[[noreturn]] void invalid(const string& message) { fail("LYK0001", message); }
[[noreturn]] void io(const string& message) { fail("LYK0006", message); }

bool letter(char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_'; }
bool digit(char value) { return value >= '0' && value <= '9'; }
bool word(char value) { return letter(value) || digit(value); }
bool simpleName(string_view text, bool hyphens = false) {
    return !text.empty() && letter(text.front()) && std::all_of(text.begin() + 1, text.end(), [hyphens](char ch) { return word(ch) || (hyphens && ch == '-'); });
}
Strings split(string_view text, char separator) {
    Strings parts;
    for (;;) {
        auto end = text.find(separator);
        parts.emplace_back(text.substr(0, end));
        if (end == string_view::npos) return parts;
        text.remove_prefix(end + 1);
    }
}
void packageName(const string& name) {
    auto parts = split(name, '/');
    if (name.size() > 192 || !std::all_of(parts.begin(), parts.end(), [](const string& part) { return simpleName(part, true); })) invalid("无效的包名：" + name);
}
bool safeRelative(string_view name) {
    if (name.empty() || name.size() > 2048 || name.find_first_of("\\:<>\"|?*") != string_view::npos || !json::isUTF8(name)) return false;
    for (char ch : name) if (static_cast<unsigned char>(ch) < 32 || ch == 127) return false;
    auto parts = split(name, '/');
    return std::none_of(parts.begin(), parts.end(), [](const string& part) {
        if (part.empty() || part.back() == '.' || part.back() == ' ') return true;
        auto stem = llvm::StringRef(part).split('.').first.rtrim(' ');
        auto device = stem.upper();
        if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" || device == "CONIN$" || device == "CONOUT$") return true;
        if (!device.starts_with("COM") && !device.starts_with("LPT")) return false;
        auto suffix = string_view(device).substr(3);
        return (suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') || suffix == "¹" || suffix == "²" || suffix == "³";
    });
}

string readFile(const fs::path& path) {
    if (!fs::is_regular_file(path)) io("找不到文件：" + pathText(path));
    auto size = fs::file_size(path);
    if (size > maxBytes) io("文件过大：" + pathText(path));
    std::ifstream input(path, std::ios::binary);
    string bytes(size, '\0');
    if (!input || !input.read(bytes.data(), bytes.size()) || input.peek() != std::char_traits<char>::eof() || input.bad()) io("无法完整读取文件：" + pathText(path));
    return bytes;
}

void writeFile(const fs::path& path, string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
    output.close();
    if (!output) io("写入失败：" + pathText(path));
}

void replaceFile(const fs::path& path, string_view bytes) {
    StagingDirectory scratch(path.parent_path());
    auto temp = scratch.path / "contents";
    writeFile(temp, bytes);
    fs::rename(temp, path);
}

string hashOf(string_view bytes) {
    return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(bytes)), true);
}
void validateHash(const string& hash) {
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char ch) { return digit(ch) || (ch >= 'a' && ch <= 'f'); })) invalid("无效的 SHA-256：" + hash);
}

string render(const json::Value& value) { return llvm::formatv("{0:2}\n", value).str(); }

enum class ManifestScope { Root, Workspace, Dependencies, Dependency, Scripts, Script, Macros, Inputs, Kind, Other };
struct ManifestField { string_view name, detail, initial; ManifestScope scope = ManifestScope::Other; };
constexpr std::array<string_view, 2> packageKinds{"app", "lib"};

std::span<const ManifestField> manifestFields(ManifestScope scope) {
    using enum ManifestScope;
    static constexpr ManifestField root[] = {
        {"name", "包名；可使用组织名/包名", "\"\""}, {"version", "SemVer 版本", "\"0.1.0\""},
        {"kind", "app 可执行包（默认）；lib 库", "\"app\"", Kind}, {"dependencies", "按简称组织的依赖", "{}", Dependencies},
        {"scripts", "显式运行的本地脚本和产物声明", "{}", Scripts}, {"macroInputs", "按宏名授权的包内只读输入", "{}", Macros},
        {"workspace", "由多个本地包组成的工作区", "{ members: [] }", Workspace},
    };
    static constexpr ManifestField workspace[] = {{"members", "工作区内的包目录", "[]"}};
    static constexpr ManifestField dependency[] = {
        {"name", "依赖的包名", "\"\""}, {"version", "精确版本或 ^SemVer 范围", "\"^0.1.0\""},
        {"path", "本地依赖目录；与 registry 二选一", "\"\""}, {"registry", "依赖的注册中心；与 path 二选一", "\"\""},
    };
    static constexpr ManifestField script[] = {{"command", "显式执行的本地命令", "\"\""}, {"outputs", "包内产物路径列表", "[]"}};
    switch (scope) {
        case Root: return root;
        case Workspace: return workspace;
        case Dependency: return dependency;
        case Script: return script;
        default: return {};
    }
}

ManifestScope fieldScope(ManifestScope scope, string_view key) {
    using enum ManifestScope;
    if (scope == Dependencies) return Dependency;
    if (scope == Scripts) return Script;
    if (scope == Macros) return Inputs;
    auto fields = manifestFields(scope);
    auto field = std::ranges::find(fields, key, &ManifestField::name);
    return field == fields.end() ? Other : field->scope;
}

using FieldPath = std::vector<string>;
using ManifestSites = std::map<FieldPath, Span>;

template<class Validate>
decltype(auto) validateField(const ManifestSites* sites, std::initializer_list<string_view> path, Validate&& validate) {
    try { return validate(); }
    catch (PackageError& error) {
        if (sites && !error.span) {
            FieldPath keys(path.begin(), path.end());
            if (auto site = sites->find(keys); site != sites->end()) error.span = site->second;
            else while (!keys.empty()) {
                keys.pop_back();
                if (auto parent = sites->find(keys); parent != sites->end()) {
                    error.span = Span{parent->second.begin, parent->second.begin + 1};
                    break;
                }
            }
        }
        throw;
    }
}

struct ManifestCursor { std::size_t offset; std::vector<ManifestCompletion> items{}; };
struct ManifestCompleted {};

class ManifestParser {
public:
    ManifestParser(string_view input, const string& where, ManifestSites* sites = nullptr, ManifestCursor* cursor = nullptr)
        : input(input), where(where), sites(sites), cursor(cursor), token(next()) {}

    json::Object parse() {
        FieldPath path;
        if (token.kind != '{') failAt(token.span, "应为对象");
        auto parsed = value(ManifestScope::Root, path, 0);
        if (token.kind) failAt(token.span, "清单末尾有多余内容");
        return std::move(*parsed.getAsObject());
    }

private:
    struct Token { char kind; Span span; bool closed = true; };
    string_view input;
    const string& where;
    ManifestSites* sites;
    ManifestCursor* cursor;
    std::size_t position{}, previous{};
    Token token;

    [[noreturn]] void failAt(Span span, string message) const {
        throw PackageError{"LYK0001", where + "：" + std::move(message), span};
    }

    Token next() {
        constexpr string_view spaces = " \t\r\n", symbols = "{}[],:";
        for (;;) {
            position = input.find_first_not_of(spaces, position);
            if (position == input.npos) { position = input.size(); return {0, {position, position}}; }
            auto start = position;
            if (input.substr(start).starts_with("//")) {
                auto end = input.find_first_of("\r\n", start + 2);
                position = end == input.npos ? input.size() : end;
                if (cursor && start <= cursor->offset && cursor->offset <= position) throw ManifestCompleted{};
            } else if (input.substr(start).starts_with("/*")) {
                auto end = input.find("*/", start + 2);
                if (cursor && start <= cursor->offset && (end == input.npos || cursor->offset < end + 2)) throw ManifestCompleted{};
                if (end == input.npos) failAt({start, input.size()}, "注释未闭合");
                position = end + 2;
            } else break;
        }
        auto start = position;
        auto first = input[position++];
        if (symbols.contains(first)) return {first, {start, position}};
        if (first == '"' || first == '\'') {
            while (position < input.size()) {
                auto next = input[position++];
                if (next == first) return {'s', {start, position}};
                if (next == '\\' && position < input.size()) ++position;
                else if (next == '\r' || next == '\n') { --position; break; }
            }
            return {'s', {start, position}, false};
        }
        if (letter(first)) {
            while (position < input.size() && word(input[position])) ++position;
            return {'w', {start, position}};
        }
        while (position < input.size() && !spaces.contains(input[position]) && !symbols.contains(input[position]) &&
            !string_view("\"'/").contains(input[position])) ++position;
        return {'v', {start, position}};
    }

    Token take() { auto result = token; previous = token.span.end; token = next(); return result; }
    void require(char kind, string message) { if (token.kind != kind) failAt(token.span, std::move(message)); take(); }
    string_view text(Token item) const { return input.substr(item.span.begin, item.span.end - item.span.begin); }

    json::Value scalar(Token item) const {
        auto raw = text(item);
        string normalized;
        if (item.kind == 's') {
            if (!item.closed) failAt(item.span, "字符串未闭合");
            normalized.push_back('"');
            for (std::size_t index = 1; index + 1 < raw.size(); ++index) {
                auto letter = raw[index];
                if (letter == '\\') {
                    auto escaped = raw[++index];
                    if (escaped != '\'') normalized.push_back('\\');
                    normalized.push_back(escaped);
                } else {
                    if (letter == '"') normalized.push_back('\\');
                    normalized.push_back(letter);
                }
            }
            normalized.push_back('"');
            raw = normalized;
        }
        auto parsed = json::parse(raw);
        if (!parsed) failAt(item.span, "无效的 JSON5 值：" + llvm::toString(parsed.takeError()));
        return std::move(*parsed);
    }

    bool atCursor() const {
        if (!cursor || cursor->offset < previous) return false;
        if (cursor->offset <= token.span.begin) return true;
        return (token.kind == 's' || token.kind == 'w') && cursor->offset <= token.span.end;
    }

    void complete(ManifestScope scope, const json::Object* object = nullptr) {
        if (!atCursor()) return;
        auto offset = cursor->offset;
        auto replace = Span{offset, offset};
        string_view prefix;
        char delimiter = '\0';
        bool colon = token.kind == ':';
        if (token.span.begin <= offset && (token.kind == 's' || token.kind == 'w')) {
            replace = token.span;
            auto raw = text(token);
            prefix = raw.substr(0, offset - replace.begin);
            if (token.kind == 's') {
                delimiter = raw.front();
                if (!prefix.empty()) prefix.remove_prefix(1);
                if (token.closed && offset == replace.end && !prefix.empty()) prefix.remove_suffix(1);
            }
            colon = next().kind == ':';
        }
        auto offer = [&](string_view label, string_view detail, string_view initial, bool value) {
            if (!label.starts_with(prefix) || (object && object->get(label))) return;
            string insertion(label);
            if (delimiter || value) {
                auto quote = delimiter ? delimiter : '"';
                insertion = quote + insertion + quote;
            }
            if (!value && !colon) insertion += ": " + string(initial);
            cursor->items.push_back({replace, string(label), std::move(insertion), string(detail), value});
        };
        if (scope == ManifestScope::Kind) for (auto kind : packageKinds)
            offer(kind, kind == "app" ? "可执行包（默认）" : "库", {}, true);
        else for (const auto& field : manifestFields(scope)) offer(field.name, field.detail, field.initial, false);
        throw ManifestCompleted{};
    }

    json::Value value(ManifestScope scope, FieldPath& path, unsigned depth) {
        if (depth >= 128) failAt(token.span, "清单嵌套过深");
        if (scope == ManifestScope::Kind) complete(scope);
        auto begin = token.span.begin;
        auto result = [&]() -> json::Value {
            if (token.kind == '{') {
                take();
                json::Object object;
                complete(scope, &object);
                while (token.kind != '}') {
                    if (token.kind != 'w' && token.kind != 's') failAt(token.span, "需要清单字段名");
                    auto item = take();
                    auto key = item.kind == 's' ? scalar(item).getAsString()->str() : string(text(item));
                    require(':', "字段名后需要 :");
                    path.push_back(key);
                    object[key] = value(fieldScope(scope, key), path, depth + 1);
                    path.pop_back();
                    if (token.kind != ',') break;
                    take();
                    complete(scope, &object);
                }
                require('}', "对象字段之间需要逗号或 }");
                return object;
            }
            if (token.kind == '[') {
                take();
                json::Array array;
                while (token.kind != ']') {
                    path.push_back(std::to_string(array.size()));
                    array.push_back(value(ManifestScope::Other, path, depth + 1));
                    path.pop_back();
                    if (token.kind != ',') break;
                    take();
                }
                require(']', "数组元素之间需要逗号或 ]");
                return array;
            }
            if (token.kind != 's' && token.kind != 'v' && token.kind != 'w') failAt(token.span, "需要 JSON5 值");
            return scalar(take());
        }();
        if (sites) sites->insert_or_assign(path, Span{begin, previous});
        return result;
    }
};

json::Object objectFrom(string_view input, const string& where) {
    auto parsed = json::parse(input);
    if (!parsed) invalid(where + "：" + llvm::toString(parsed.takeError()));
    if (!parsed->getAsObject()) invalid(where + "：应为对象");
    return std::move(*parsed->getAsObject());
}
string required(const json::Object& obj, string_view key, const string& where) {
    if (auto value = obj.getString(key)) return value->str();
    invalid(where + "：缺少字符串字段 " + string(key));
}
const json::Object& asObject(const json::Value& value, const string& where) {
    if (auto obj = value.getAsObject()) return *obj;
    invalid(where + "：应为对象");
}
const json::Array& arrayField(const json::Object& obj, string_view key, const string& where) {
    if (auto array = obj.getArray(key)) return *array;
    invalid(where + "：缺少数组字段 " + string(key));
}
Strings stringList(const json::Object& obj, string_view key, const string& where) {
    Strings items;
    for (const auto& item : arrayField(obj, key, where)) {
        if (!item.getAsString()) invalid(where + "：" + string(key) + " 的元素应是字符串");
        items.push_back(item.getAsString()->str());
    }
    return items;
}

struct Version {
    std::uint64_t major, minor, patch;
    Strings pre;
    string text;
    std::strong_ordering compare(const Version& other) const {
        if (auto result = major <=> other.major; result != 0) return result;
        if (auto result = minor <=> other.minor; result != 0) return result;
        if (auto result = patch <=> other.patch; result != 0) return result;
        if (pre.empty() != other.pre.empty()) return pre.empty() <=> other.pre.empty();
        for (std::size_t index = 0; index < std::min(pre.size(), other.pre.size()); ++index) {
            const auto& left = pre[index]; const auto& right = other.pre[index];
            bool ln = std::all_of(left.begin(), left.end(), digit), rn = std::all_of(right.begin(), right.end(), digit);
            if (ln != rn) return rn <=> ln;
            if (ln && left.size() != right.size()) return left.size() <=> right.size();
            if (left != right) return left <=> right;
        }
        return pre.size() <=> other.pre.size();
    }
};

Version versionOf(const string& text) {
    auto bad = [&] { invalid("无效 SemVer：" + text); };
    if (text.size() > 192) bad();
    string_view core = text;
    Strings pre;
    auto identifiers = [&](char sign, bool prerelease) {
        auto pos = core.find(sign);
        if (pos == string_view::npos) return;
        auto parts = split(core.substr(pos + 1), '.'); core = core.substr(0, pos);
        for (const auto& part : parts) {
            if (part.empty() || !std::all_of(part.begin(), part.end(), [](char ch) { return digit(ch) || (letter(ch) && ch != '_') || ch == '-'; })) bad();
            if (prerelease && part.size() > 1 && part.front() == '0' && std::all_of(part.begin(), part.end(), digit)) bad();
        }
        if (prerelease) pre = std::move(parts);
    };
    identifiers('+', false); identifiers('-', true);
    auto parts = split(core, '.');
    if (parts.size() != 3) bad();
    auto number = [&](const string& part) {
        std::uint64_t result = 0;
        auto end = part.data() + part.size(); auto parsed = std::from_chars(part.data(), end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end || (part.size() > 1 && part.front() == '0')) bad();
        return result;
    };
    return {number(parts[0]), number(parts[1]), number(parts[2]), std::move(pre), text};
}

struct Range {
    bool caret;
    Version base;
    bool accepts(const Version& ver) const {
        if (!caret) return base.text == ver.text;
        if (base.compare(ver) > 0 || base.major != ver.major) return false;
        if (!ver.pre.empty() && (base.pre.empty() || base.minor != ver.minor || base.patch != ver.patch)) return false;
        return base.major != 0 || (base.minor == ver.minor && (base.minor != 0 || base.patch == ver.patch));
    }
};
Range rangeOf(const string& text) { bool caret = text.starts_with('^'); return {caret, versionOf(text.substr(caret ? 1 : 0))}; }

struct Dependency { string name, range, registry; fs::path path; };
struct Script { string name, command; Strings outputs; };
struct Manifest {
    string name, version;
    std::map<string, Dependency> dependencies;
    std::vector<Script> scripts;
    bool library{};
    std::map<string, std::map<string, fs::path>> macroInputs{};
};

Manifest manifestFromObject(const json::Object& obj, const string& where, const ManifestSites* sites = nullptr) {
    auto name = validateField(sites, {"name"}, [&] { auto value = required(obj, "name", where); packageName(value); return value; });
    auto version = validateField(sites, {"version"}, [&] { auto value = required(obj, "version", where); versionOf(value); return value; });
    Manifest manifest{std::move(name), std::move(version), {}, {}};
    if (obj.get("kind")) validateField(sites, {"kind"}, [&] {
        auto kind = required(obj, "kind", where);
        if (std::ranges::find(packageKinds, kind) == packageKinds.end()) invalid(where + "：kind 只能是 app 或 lib");
        manifest.library = kind == "lib";
    });
    if (auto value = obj.get("dependencies")) {
        auto& dependencies = validateField(sites, {"dependencies"}, [&]() -> const json::Object& { return asObject(*value, where + ": dependencies"); });
        for (const auto& [key, item] : dependencies) {
            auto alias = key.str(); auto location = where + ": dependencies." + alias;
            auto field = [&](string_view name, auto validate) { return validateField(sites, {"dependencies", alias, name}, validate); };
            validateField(sites, {"dependencies", alias}, [&] {
                if (!simpleName(alias)) invalid(location + "：依赖简称须是标识符");
                const auto& dep = asObject(item, location);
                auto name = field("name", [&] { auto value = required(dep, "name", location); packageName(value); return value; });
                auto range = field("version", [&] { auto value = required(dep, "version", location); rangeOf(value); return value; });
                Dependency dependency{std::move(name), std::move(range), {}, {}};
                if (dep.get("path")) field("path", [&] {
                    auto path = required(dep, "path", location);
                    if (path.empty() || path.find('\0') != string::npos) invalid(location + "：无效本地路径");
                    dependency.path = fromUtf8(path);
                });
                if (dep.get("registry")) field("registry", [&] {
                    dependency.registry = required(dep, "registry", location);
                    if (dependency.registry.empty()) invalid(location + "：注册中心为空");
                });
                if (dependency.path.empty() == dependency.registry.empty()) invalid(location + "：必须指定 path 或 registry 其中一个");
                manifest.dependencies.emplace(alias, std::move(dependency));
            });
        }
    }
    if (auto value = obj.get("scripts")) {
        auto& scripts = validateField(sites, {"scripts"}, [&]() -> const json::Object& { return asObject(*value, where + ": scripts"); });
        for (const auto& [key, item] : scripts) {
            auto name = key.str();
            validateField(sites, {"scripts", name}, [&] {
                const auto& desc = asObject(item, where + ": scripts." + name);
                auto command = validateField(sites, {"scripts", name, "command"}, [&] { return required(desc, "command", where); });
                Script script{name, std::move(command), {}};
                if (desc.get("outputs")) validateField(sites, {"scripts", name, "outputs"}, [&] {
                    script.outputs = stringList(desc, "outputs", where);
                    for (std::size_t index = 0; index < script.outputs.size(); ++index) if (!safeRelative(script.outputs[index]))
                        validateField(sites, {"scripts", name, "outputs", std::to_string(index)}, [&] { invalid(where + "：脚本产物必须是包内相对路径"); });
                });
                manifest.scripts.push_back(std::move(script));
            });
        }
    }
    if (auto macros = obj.get("macroInputs")) {
        auto& entries = validateField(sites, {"macroInputs"}, [&]() -> const json::Object& { return asObject(*macros, where + ": macroInputs"); });
        for (const auto& [nameKey, inputs] : entries) {
            auto name = nameKey.str();
            auto& granted = manifest.macroInputs[name];
            auto& inputFields = validateField(sites, {"macroInputs", name}, [&]() -> const json::Object& { return asObject(inputs, where + ": macroInputs." + name); });
            for (const auto& [key, file] : inputFields) {
                auto alias = key.str();
                validateField(sites, {"macroInputs", name, alias}, [&] {
                    auto path = file.getAsString();
                    if (!path || !safeRelative(path->str())) invalid(where + "：宏输入必须是包内相对文件路径");
                    granted.emplace(alias, fromUtf8(path->str()));
                });
            }
        }
    }
    return manifest;
}

Manifest manifestFrom(string_view bytes, const string& where) {
    return manifestFromObject(ManifestParser(bytes, where).parse(), where);
}

Files collect(const fs::path& root) {
    Files files;
    std::size_t size = 0;
    auto include = [&](const fs::path& path) {
        auto name = pathText(path.lexically_relative(root));
        if (files.contains(name)) return;
        if (fs::is_symlink(path) || !safeRelative(name) || !fs::is_regular_file(path)) invalid("源码包包含非普通文件或无效路径：" + pathText(path));
        if (fs::file_size(path) + size > maxBytes || files.size() >= maxFiles) invalid("源码包超过大小或文件数限制：" + pathText(root));
        auto bytes = readFile(path);
        size += bytes.size();
        files.emplace(std::move(name), std::move(bytes));
    };
    include(root / manifestFile);
    if (!fs::is_directory(root / "src") || fs::is_symlink(root / "src")) invalid("缺少普通 src 目录：" + pathText(root));
    for (const auto& entry : fs::recursive_directory_iterator(root / "src")) {
        if (entry.is_symlink() || !entry.is_directory()) include(entry.path());
    }
    auto manifest = manifestFrom(files.at(manifestFile), pathText(root / manifestFile));
    for (const auto& [macro, inputs] : manifest.macroInputs) for (const auto& [key, path] : inputs) {
        auto name = pathText(path);
        if (!name.starts_with("src/") && !name.starts_with("inputs/")) invalid("可发布的宏输入须放在 src/ 或 inputs/：" + name);
        for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) if (fs::is_symlink(root / parent)) invalid("宏输入目录不能是符号链接：" + pathText(parent));
        include(root / path);
    }
    return files;
}

constexpr string_view archiveMagic = "LYRA-PACK-1\n";
void length(string& output, std::size_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) output.push_back(static_cast<char>((value >> shift) & 255));
}
string archiveOf(const Files& files) {
    string archive(archiveMagic);
    for (const auto& [path, bytes] : files) {
        length(archive, path.size()); length(archive, bytes.size()); archive += path; archive += bytes;
        if (archive.size() > maxBytes) invalid("源码包超过 64 MiB");
    }
    return archive;
}

Files unpack(string_view bytes, const string& where) {
    if (!bytes.starts_with(archiveMagic)) invalid(where + "：无效源码包格式");
    bytes.remove_prefix(archiveMagic.size());
    auto take = [&](std::size_t size) { if (size > bytes.size()) invalid(where + "：源码包条目截断"); auto part = bytes.substr(0, size); bytes.remove_prefix(size); return part; };
    auto size = [&] { std::uint32_t result = 0; for (unsigned char ch : take(4)) result = (result << 8) | ch; return result; };
    Files files;
    while (!bytes.empty()) {
        auto pathSize = size(); auto dataSize = size();
        string path(take(pathSize));
        if (!safeRelative(path) || (path != manifestFile && !path.starts_with("src/") && !path.starts_with("inputs/"))) invalid(where + "：无效归档路径 " + path);
        if (files.size() >= maxFiles || (!files.empty() && path <= files.rbegin()->first)) invalid(where + "：过多、重复或未排序的源码条目");
        files.emplace(std::move(path), string(take(dataSize)));
    }
    if (!files.contains(manifestFile)) invalid(where + "：缺少包清单");
    auto manifest = manifestFrom(files.at(manifestFile), where);
    std::set<string> grants;
    for (const auto& [macro, inputs] : manifest.macroInputs) for (const auto& [key, path] : inputs) {
        auto name = pathText(path);
        if (!files.contains(name)) invalid(where + "：归档缺少声明的宏输入：" + name);
        grants.insert(std::move(name));
    }
    for (const auto& [name, body] : files) {
        if (name.starts_with("inputs/") && !grants.contains(name)) invalid(where + "：归档包含未声明的宏输入：" + name);
        for (auto prefix = fromUtf8(name).parent_path(); !prefix.empty(); prefix = prefix.parent_path()) if (files.contains(pathText(prefix))) invalid(where + "：文件与目录重名：" + pathText(prefix));
    }
    return files;
}

bool isWeb(string_view source) { return source.starts_with("https://") || source.starts_with("http://"); }
string sourceOf(string source, const fs::path& directory, const fs::path& workspace) {
    if (isWeb(source)) {
        auto authority = source.find("://") + 3;
        auto end = source.find('/', authority);
        if (source.substr(authority, end - authority).empty() || source.find_first_of("@?#\\| \r\n\t") != string::npos) invalid("无效的 HTTP(S) 注册中心：" + source);
        while (source.back() == '/') source.pop_back();
        return source;
    }
    if (source.empty() || source.find("://") != string::npos || source.find('\0') != string::npos) invalid("不支持的注册中心：" + source);
    auto path = fs::weakly_canonical(directory / fromUtf8(source));
    auto relative = path.lexically_relative(workspace);
    return "registry:" + pathText(relative.empty() ? path : relative);
}
string localSource(const fs::path& root, const fs::path& workspace) {
    auto relative = root.lexically_relative(workspace);
    return "path:" + pathText(relative.empty() ? root : relative);
}
string identity(const string& source, const string& name) { return source + "|" + name; }

struct Record { Version version; string hash; bool yanked; };
std::vector<Record> recordsFrom(const json::Object& index, const string& where, const string& name) {
    if (required(index, "name", where) != name) invalid(where + "：注册记录包名不匹配：" + name);
    std::vector<Record> records;
    std::set<string> seen;
    for (const auto& value : arrayField(index, "versions", where)) {
        const auto& obj = asObject(value, where); auto version = versionOf(required(obj, "version", where));
        auto hash = required(obj, "sha256", where); validateHash(hash);
        if (!seen.insert(version.text).second) invalid(where + "：重复发布版本：" + version.text);
        auto yanked = obj.getBoolean("yanked");
        if (obj.get("yanked") && !yanked) invalid(where + "：yanked 须为布尔值");
        records.push_back({std::move(version), std::move(hash), yanked.value_or(false)});
    }
    std::sort(records.begin(), records.end(), [](const Record& left, const Record& right) {
        auto order = left.version.compare(right.version);
        return order == 0 ? left.version.text < right.version.text : order > 0;
    });
    return records;
}

struct Request { string source, name, range; fs::path path; };
struct Node { Manifest manifest; string source, hash; fs::path root; bool yanked = false; };
struct Locked { string version, hash; };
using LockedNodes = std::map<string, Locked>;

struct Store {
    fs::path workspace, cache;
    PackageOptions options;
    std::map<string, std::vector<Record>> indexes;
    std::map<string, Node> nodes;
    LockedNodes locks;

    string getRemote(const string& url, const string& label) {
        StagingDirectory scratch(cache);
        auto file = scratch.path / "download";
        auto target = pathText(file);
        auto curl = llvm::sys::findProgramByName("curl");
        if (!curl) io("下载 " + label + " 需要 curl");
        llvm::SmallVector<llvm::StringRef> args{*curl, "-q", "--silent", "--fail", "--proto", "=http,https", "--max-time", "30", "--max-filesize", "67108864", "--", url};
        std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(target), std::nullopt};
        if (llvm::sys::ExecuteAndWait(*curl, args, std::nullopt, redirects, 35) != 0) io("从只读注册中心下载失败：" + label + " (" + url + ")");
        return readFile(file);
    }

    fs::path registryPath(const string& source) const { return workspace / fromUtf8(string_view(source).substr(string_view("registry:").size())); }

    const std::vector<Record>& index(const Request& request) {
        auto id = identity(request.source, request.name);
        if (auto found = indexes.find(id); found != indexes.end()) return found->second;
        auto relative = "index/" + request.name + ".json";
        string contents;
        auto file = cache / "index" / (hashOf(id) + ".json");
        bool online = isWeb(request.source) && !options.offline;
        bool local = !isWeb(request.source) && fs::is_regular_file(registryPath(request.source) / relative);
        if (online) contents = getRemote(request.source + "/" + relative, request.name);
        else if (local) contents = readFile(registryPath(request.source) / relative);
        else if (fs::is_regular_file(file)) contents = readFile(file);
        else io(string(options.offline ? "离线缺少索引：" : "缺少包索引：") + request.name + " " + request.range + " (" + request.source + ")");
        auto records = recordsFrom(objectFrom(contents, id), id, request.name);
        if ((online || local) && !options.readOnly) replaceFile(file, contents);
        return indexes.emplace(id, std::move(records)).first->second;
    }

    const Node& local(const fs::path& directory) {
        auto root = fs::canonical(directory); auto key = "local:" + pathText(root);
        if (auto found = nodes.find(key); found != nodes.end()) return found->second;
        auto contents = readFile(root / manifestFile);
        return nodes.emplace(key, Node{manifestFrom(contents, pathText(root / manifestFile)), localSource(root, workspace), hashOf(contents), root}).first->second;
    }

    const Node& released(const Request& request, const Record& record) {
        auto id = identity(request.source, request.name); auto label = request.name + "@" + record.version.text;
        auto key = id + "@" + record.version.text;
        if (auto found = nodes.find(key); found != nodes.end()) return found->second;
        auto existing = locks.find(id);
        if (existing != locks.end() && existing->second.version == record.version.text && existing->second.hash != record.hash) fail("LYK0005", label + " 的发布摘要与锁不符；不得覆盖已发布的版本");
        auto relative = "artifacts/" + record.hash + ".lypkg";
        auto cached = cache / relative;
        string bytes;
        if (!isWeb(request.source) && fs::is_regular_file(registryPath(request.source) / relative)) bytes = readFile(registryPath(request.source) / relative);
        else if (fs::is_regular_file(cached)) bytes = readFile(cached);
        else if (!isWeb(request.source)) io("缺少本地制品：" + label + " sha256=" + record.hash);
        else if (options.offline) io("离线缺少制品：" + label + " sha256=" + record.hash);
        else bytes = getRemote(request.source + "/" + relative, label);
        if (hashOf(bytes) != record.hash) fail("LYK0005", label + " 的源码包 SHA-256 不匹配");
        auto files = unpack(bytes, label); auto manifest = manifestFrom(files.at(manifestFile), label);
        if (manifest.name != request.name || manifest.version != record.version.text) invalid(label + "：归档清单与索引不一致");
        for (const auto& [alias, dep] : manifest.dependencies) if (!dep.path.empty() || (dep.registry != "self" && !isWeb(dep.registry))) invalid(label + "：发布包不能包含本地依赖：" + alias);
        if (!options.readOnly && !fs::is_regular_file(cached)) replaceFile(cached, bytes);
        auto root = cache / "sources" / record.hash;
        if (!fs::exists(root)) {
            if (options.readOnly) io("编辑器缺少已展开的依赖：" + label + "，请先运行 lyra fetch");
            StagingDirectory scratch(root.parent_path());
            auto staged = scratch.path / "srcpkg";
            fs::create_directories(staged / "src");
            for (const auto& [file, body] : files) writeFile(staged / fromUtf8(file), body);
            if constexpr (windows) if (collect(staged) != files) invalid(label + "：源码包路径在 Windows 上重名");
            std::error_code error; fs::rename(staged, root, error);
            if (error && !fs::is_directory(root)) io("安装源码包失败：" + error.message());
        }
        if (fs::is_symlink(root) || collect(root) != files) fail("LYK0005", label + " 的展开源码缓存被修改：" + pathText(root));
        return nodes.emplace(key, Node{std::move(manifest), request.source, record.hash, std::move(root), record.yanked}).first->second;
    }

    Request dependency(const Node& owner, const Dependency& dependency) {
        if (!dependency.path.empty()) {
            auto root = fs::weakly_canonical(owner.root / dependency.path);
            if (!fs::is_regular_file(root / manifestFile)) io("本地依赖 " + dependency.name + " 缺少清单：" + pathText(root / manifestFile));
            return {localSource(root, workspace), dependency.name, dependency.range, root};
        }
        string source;
        if (dependency.registry == "self") {
            if (owner.source.starts_with("path:")) invalid(owner.manifest.name + "：本地包使用 self 时须通过注册中心消费；请指定本地中心或 HTTP(S) URL");
            source = owner.source;
        } else source = sourceOf(dependency.registry, owner.root, workspace);
        return {source, dependency.name, dependency.range, {}};
    }
};

struct Need { Request request; Strings path; Strings ids; };
struct Conflict { string message; };
struct State {
    std::map<string, const Node*> chosen;
    std::map<string, Strings> paths;
};
string joined(const Strings& path) {
    string result;
    for (const auto& step : path) { if (!result.empty()) result += " → "; result += step; }
    return result;
}
void expand(Store& store, const Node& node, const Strings& path, const Strings& ids, std::vector<Need>& pending) {
    for (const auto& [alias, dep] : node.manifest.dependencies) {
        auto request = store.dependency(node, dep);
        auto chain = path; chain.push_back(request.name + " " + request.range);
        pending.push_back({std::move(request), std::move(chain), ids});
    }
}

std::variant<State, Conflict> solve(Store& store, State state, std::vector<Need> pending, std::size_t& attempts) {
    if (pending.empty()) return state;
    if (++attempts > 20000 || state.chosen.size() > 256 || pending.size() > 1024) invalid("依赖图超过当前解析器上限");
    auto need = std::move(pending.back()); pending.pop_back();
    auto& req = need.request; auto id = identity(req.source, req.name); auto range = rangeOf(req.range);
    if (std::find(need.ids.begin(), need.ids.end(), id) != need.ids.end()) return Conflict{"包依赖环：" + joined(need.path)};
    if (auto found = state.chosen.find(id); found != state.chosen.end()) {
        if (!range.accepts(versionOf(found->second->manifest.version))) return Conflict{"没有共同版本：\n  " + joined(state.paths.at(id)) + "（已选 " + found->second->manifest.version + "）\n  " + joined(need.path)};
        return solve(store, std::move(state), std::move(pending), attempts);
    }
    auto lock = store.locks.find(id);
    bool pinned = lock != store.locks.end();
    if (store.options.locked && !pinned) fail("LYK0003", "锁中缺少依赖：" + joined(need.path));
    need.ids.push_back(id);
    auto tryNode = [&](const Node& node) -> std::variant<State, Conflict> {
        if (node.manifest.name != req.name) invalid("本地依赖包名不符：需要 " + req.name + "，实际为 " + node.manifest.name);
        if (!range.accepts(versionOf(node.manifest.version))) return Conflict{"版本约束不满足：" + joined(need.path) + "，找到 " + node.manifest.version};
        auto next = state; next.chosen.emplace(id, &node); next.paths.emplace(id, need.path);
        auto work = pending; expand(store, node, need.path, need.ids, work);
        return solve(store, std::move(next), std::move(work), attempts);
    };
    if (!req.path.empty()) return tryNode(store.local(req.path));
    std::vector<Record> versions;
    bool localIndex = !isWeb(req.source) && fs::is_regular_file(store.registryPath(req.source) / "index" / (req.name + ".json"));
    if (store.options.offline && pinned && !store.options.update && range.accepts(versionOf(lock->second.version)) && !localIndex &&
        !fs::is_regular_file(store.cache / "index" / (hashOf(id) + ".json"))) {
        versions.push_back({versionOf(lock->second.version), lock->second.hash, false});
    } else versions = store.index(req);
    if (pinned && !store.options.update) {
        auto prev = std::find_if(versions.begin(), versions.end(), [&](const Record& record) { return record.version.text == lock->second.version; });
        if (prev != versions.end()) std::rotate(versions.begin(), prev, prev + 1);
    }
    Conflict conflict{"没有匹配的已发布版本：" + joined(need.path)};
    for (const auto& record : versions) {
        bool samePin = pinned && record.version.text == lock->second.version;
        if (!range.accepts(record.version) || (store.options.locked && !samePin) || (record.yanked && (!samePin || store.options.update))) continue;
        auto answer = tryNode(store.released(req, record));
        if (std::holds_alternative<State>(answer)) return answer;
        conflict = std::move(std::get<Conflict>(answer));
    }
    return conflict;
}

LockedNodes readLock(const json::Object& obj, const string& where) {
    if (obj.getInteger("format") != 1) invalid(where + "：不支持的锁文件版本");
    LockedNodes result;
    for (const auto& item : arrayField(obj, "packages", where)) {
        const auto& entry = asObject(item, where);
        auto id = required(entry, "id", where); auto version = required(entry, "version", where); versionOf(version);
        auto hash = required(entry, "sha256", where); validateHash(hash);
        if (!result.emplace(id, Locked{std::move(version), std::move(hash)}).second) invalid(where + "：重复的包：" + id);
    }
    return result;
}

struct Layout { fs::path root, entry; std::vector<fs::path> members; };
std::optional<std::vector<fs::path>> membersOfObject(const json::Object& obj, const fs::path& root, const ManifestSites* sites = nullptr) {
    auto field = obj.get("workspace"); if (!field) return std::nullopt;
    auto where = pathText(root);
    const auto& ws = validateField(sites, {"workspace"}, [&]() -> const json::Object& { return asObject(*field, where); });
    auto declared = validateField(sites, {"workspace", "members"}, [&] { return stringList(ws, "members", where); });
    std::vector<fs::path> members;
    if (obj.get("name")) members.push_back(root);
    for (std::size_t index = 0; index < declared.size(); ++index) validateField(sites, {"workspace", "members", std::to_string(index)}, [&] {
        const auto& member = declared[index];
        if (!safeRelative(member)) invalid("workspace 成员须为内部目录：" + member);
        auto resolved = fs::weakly_canonical(root / fromUtf8(member));
        if (!safeRelative(pathText(resolved.lexically_relative(root)))) invalid("workspace 成员指向外部目录：" + member);
        if (std::find(members.begin(), members.end(), resolved) != members.end()) invalid("重复 workspace 成员：" + member);
        if (!fs::is_regular_file(resolved / manifestFile)) invalid("workspace 成员缺少清单：" + member);
        members.push_back(std::move(resolved));
    });
    if (members.empty()) validateField(sites, {"workspace", "members"}, [&] { invalid("workspace 至少需要一个包"); });
    std::sort(members.begin(), members.end());
    return members;
}

std::optional<std::vector<fs::path>> membersOf(const fs::path& root) {
    auto file = root / manifestFile;
    return membersOfObject(ManifestParser(readFile(file), pathText(file)).parse(), root);
}
Layout layout(const fs::path& directory) {
    auto dir = fs::canonical(directory);
    if (!fs::is_regular_file(dir / manifestFile)) invalid("找不到包清单：" + pathText(dir / manifestFile));
    for (auto candidate = dir; ; candidate = candidate.parent_path()) {
        if (fs::is_regular_file(candidate / manifestFile)) {
            auto members = membersOf(candidate);
            if (members && (candidate == dir || std::find(members->begin(), members->end(), dir) != members->end())) {
                auto entry = std::find(members->begin(), members->end(), dir) == members->end() ? fs::path{} : dir;
                return {candidate, entry, std::move(*members)};
            }
        }
        if (candidate == candidate.root_path()) break;
    }
    return {dir, dir, {dir}};
}

void visitGraph(const string& id, const std::map<string, ResolvedPackage>& packages, std::set<string>& done, Strings& visiting) {
    if (done.contains(id)) return;
    if (auto found = std::find(visiting.begin(), visiting.end(), id); found != visiting.end()) {
        Strings chain;
        for (auto iter = found; iter != visiting.end(); ++iter) chain.push_back(packages.at(*iter).name);
        chain.push_back(packages.at(id).name);
        fail("LYK0002", "包依赖环：" + joined(chain));
    }
    visiting.push_back(id);
    for (const auto& [alias, target] : packages.at(id).dependencies) visitGraph(target, packages, done, visiting);
    visiting.pop_back(); done.insert(id);
}

}

PackageGraph resolvePackages(const fs::path& directory, PackageOptions options) {
    if (options.readOnly) options.offline = true;
    if (options.update && options.locked) fail("LYK0001", "--update 与 --locked 不能并用");
    auto ws = layout(directory); auto lockPath = ws.root / "lyra.lock";
    fs::path cache = ws.root / ".lyra" / "cache";
    if (auto override = cacheOverride(); !override.empty()) cache = fs::absolute(override);
    if (options.locked && !fs::is_regular_file(lockPath)) fail("LYK0003", "锁定构建缺少 " + pathText(lockPath));
    std::optional<json::Object> old;
    Store store{ws.root, cache, options, {}, {}, {}};
    if (fs::exists(lockPath)) {
        old = objectFrom(readFile(lockPath), pathText(lockPath));
        store.locks = readLock(*old, pathText(lockPath));
    }
    PackageGraph graph{ws.root, {}, {}, {}, {}, {}};
    std::vector<Need> pending;
    for (const auto& member : ws.members) {
        const auto& node = store.local(member); const auto& manifest = node.manifest;
        string id = identity(node.source, manifest.name);
        graph.members.push_back(id);
        pending.push_back({{node.source, manifest.name, manifest.version, member}, {manifest.name + " " + manifest.version}, {}});
        if (member == ws.entry) graph.entry = id;
    }
    std::size_t attempts = 0;
    auto resolution = solve(store, {}, std::move(pending), attempts);
    if (auto error = std::get_if<Conflict>(&resolution)) fail(options.locked ? "LYK0003" : "LYK0002", error->message);
    auto state = std::move(std::get<State>(resolution));
    json::Array entries;
    std::map<string, ResolvedPackage> packages;
    for (const auto& [id, node] : state.chosen) {
        const auto& manifest = node->manifest;
        ResolvedPackage package{id, manifest.name, manifest.version, node->root, {}, manifest.library, manifest.macroInputs};
        for (const auto& [alias, dep] : manifest.dependencies) {
            auto req = store.dependency(*node, dep);
            package.dependencies.emplace(alias, identity(req.source, req.name));
        }
        if (node->yanked) graph.warnings.push_back("沿用已撤回的锁定版本：" + manifest.name + "@" + manifest.version);
        for (const auto& script : manifest.scripts) {
            auto label = manifest.name + "@" + manifest.version + " (" + node->source + ") " + script.name;
            graph.warnings.push_back("已声明但不会执行脚本：" + label + "：" + script.command);
            for (const auto& output : script.outputs) if (!fs::is_regular_file(node->root / output)) graph.missingOutputs.push_back(label + "：缺少 " + output);
        }
        json::Object aliases;
        for (const auto& [alias, target] : package.dependencies) aliases[alias] = target;
        entries.emplace_back(json::Object{{"id", id}, {"source", node->source}, {"name", manifest.name}, {"version", manifest.version}, {"sha256", node->hash}, {"dependencies", std::move(aliases)}});
        packages.emplace(id, std::move(package));
    }
    std::set<string> done; Strings visiting;
    for (const auto& [id, package] : packages) visitGraph(id, packages, done, visiting);
    json::Array members;
    for (const auto& member : ws.members) members.emplace_back(pathText(member.lexically_relative(ws.root)));
    json::Object lock{{"format", 1}, {"members", std::move(members)}, {"packages", std::move(entries)}};
    if (!old || *old != lock) {
        if (options.locked) fail("LYK0003", "清单与锁文件不符：" + pathText(lockPath));
        if (!options.readOnly) replaceFile(lockPath, render(json::Value(std::move(lock))));
    }
    for (auto& [id, package] : packages) graph.packages.push_back(std::move(package));
    return graph;
}

PackageGraph indexPackages(const fs::path& directory) {
    try { return resolvePackages(directory, {.readOnly = true}); }
    catch (const PackageError&) {}
    auto workspace = directory;
    try { workspace = layout(directory).root; }
    catch (const PackageError&) {}
    auto cache = cacheOverride();
    if (cache.empty()) cache = workspace / ".lyra" / "cache";
    Store store{workspace, cache, {.offline = true, .readOnly = true}, {}, {}, {}};
    auto lock = workspace / "lyra.lock";
    if (fs::is_regular_file(lock)) {
        try { store.locks = readLock(objectFrom(readFile(lock), pathText(lock)), pathText(lock)); }
        catch (const PackageError&) {}
    }
    PackageGraph graph{workspace, "$editor", {{"$editor", "", "", directory, {}, false}}, {}, {}, {}};
    std::map<string, ResolvedPackage> packages;
    auto add = [&](auto&& visit, const Node& node) -> void {
        const auto& manifest = node.manifest;
        auto id = identity(node.source, manifest.name);
        auto [entry, fresh] = packages.try_emplace(id, ResolvedPackage{id, manifest.name, manifest.version,
            node.root, {}, manifest.library, manifest.macroInputs});
        if (!fresh) return;
        for (const auto& [alias, dependency] : manifest.dependencies) {
            auto& target = entry->second.dependencies[alias];
            try {
                auto request = store.dependency(node, dependency);
                const Node* child = nullptr;
                if (!request.path.empty()) child = &store.local(request.path);
                else {
                    auto locked = store.locks.find(identity(request.source, request.name));
                    if (locked != store.locks.end()) child = &store.released(request, {versionOf(locked->second.version), locked->second.hash, false});
                }
                if (child && child->manifest.name == request.name && rangeOf(request.range).accepts(versionOf(child->manifest.version))) {
                    target = identity(child->source, child->manifest.name);
                    visit(visit, *child);
                }
            } catch (const PackageError&) {}
            catch (const fs::filesystem_error&) {}
        }
    };
    if (!fs::is_regular_file(directory / manifestFile)) return graph;
    try {
        const auto& entry = store.local(directory);
        graph.entry = identity(entry.source, entry.manifest.name);
        add(add, entry);
        graph.packages.clear();
        for (auto& [id, package] : packages) graph.packages.push_back(std::move(package));
    } catch (const PackageError&) {}
    return graph;
}

std::vector<fs::path> indexLocalPackageRoots(const fs::path& directory) {
    std::set<fs::path> seen;
    auto visit = [&](auto&& walk, const fs::path& root) -> void {
        if (!seen.insert(root).second || !fs::is_regular_file(root / manifestFile)) return;
        try {
            auto manifest = manifestFrom(readFile(root / manifestFile), pathText(root / manifestFile));
            for (const auto& [alias, dependency] : manifest.dependencies) if (!dependency.path.empty())
                walk(walk, fs::weakly_canonical(root / dependency.path));
        } catch (const PackageError&) {}
        catch (const fs::filesystem_error&) {}
    };
    try {
        for (const auto& member : layout(directory).members) visit(visit, member);
    } catch (const PackageError&) {}
    catch (const fs::filesystem_error&) {}
    visit(visit, directory);
    return {seen.begin(), seen.end()};
}

std::vector<ManifestCompletion> completeManifest(const Source& source, std::size_t offset) {
    ManifestCursor cursor{offset};
    auto where = pathText(source.path);
    try { ManifestParser(source.text, where, nullptr, &cursor).parse(); }
    catch (const ManifestCompleted&) {}
    catch (const PackageError&) {}
    return std::move(cursor.items);
}

std::optional<Diagnostic> validateManifest(const Source& source) {
    auto where = pathText(source.path);
    ManifestSites sites;
    try {
        auto object = ManifestParser(source.text, where, &sites).parse();
        if (object.get("name") || !object.get("workspace")) manifestFromObject(object, where, &sites);
        if (object.get("workspace")) membersOfObject(object, fs::weakly_canonical(source.path.parent_path()), &sites);
    } catch (const PackageError& error) {
        return at(source, error.span.value_or(Span{0, 0}), error.code, error.message);
    } catch (const fs::filesystem_error& error) {
        return at(source, {0, 0}, "LYK0006", error.what());
    }
    return {};
}

namespace {
void newPackage(const fs::path& directory, const string& name, bool library) {
    packageName(name);
    auto root = fs::absolute(directory).lexically_normal();
    if (fs::exists(root)) invalid("目标目录已存在：" + pathText(root));
    StagingDirectory scratch(root.parent_path()); auto stage = scratch.path / "new";
    writeFile(stage / manifestFile, render(json::Object{{"name", name}, {"version", "0.1.0"}, {"kind", library ? "lib" : "app"}, {"dependencies", json::Object{}}}));
    string source = "pub fn answer(): Int { 42 }\n";
    if (!library) source += "\nfn main(): Unit { print(answer()) }\n";
    writeFile(stage / "src" / (library ? "lib.ly" : "main.ly"), source);
    writeFile(stage / "tests" / "answer.ly", "import self.{answer}\n\n"
              "fn main(): Unit {\n    #assert(answer() == 42, \"expected 42\")\n}\n");
    fs::rename(stage, root);
    std::cout << pathText(root) << '\n';
}

struct PublishLock {
    fs::path path;
    explicit PublishLock(const fs::path& registry) : path(registry / ".publish-lock") {
        fs::create_directories(registry);
        if (!fs::create_directory(path)) io("注册中心正在发布或有未清理的 .publish-lock：" + pathText(registry));
    }
    ~PublishLock() { std::error_code error; fs::remove(path, error); }
};

void packPackage(const fs::path& directory, const fs::path& output, const fs::path& registry) {
    auto root = fs::canonical(directory);
    auto files = collect(root); auto manifest = manifestFrom(files.at(manifestFile), pathText(root / manifestFile));
    for (const auto& [alias, dep] : manifest.dependencies) if (!dep.path.empty() || (dep.registry != "self" && !isWeb(dep.registry))) invalid("发布包不能包含本地依赖：" + alias + " (" + dep.name + ")");
    auto archive = archiveOf(files); auto hash = hashOf(archive);
    if (registry.empty()) {
        auto basename = split(manifest.name, '/').back() + "-" + manifest.version + ".lypkg";
        auto target = fs::absolute(output.empty() ? root / "dist" / basename : output);
        if (fs::exists(target) && (!fs::is_regular_file(target) || readFile(target) != archive)) invalid("拒绝覆盖已有且内容不同的文件：" + pathText(target));
        if (!fs::exists(target)) replaceFile(target, archive);
        std::cout << pathText(target) << " sha256=" << hash << '\n';
        return;
    }
    auto center = fs::absolute(registry); PublishLock guard(center);
    auto index = center / "index" / (manifest.name + ".json");
    json::Object object{{"name", manifest.name}, {"versions", json::Array{}}};
    if (fs::exists(index)) object = objectFrom(readFile(index), pathText(index));
    auto published = recordsFrom(object, pathText(index), manifest.name);
    if (std::any_of(published.begin(), published.end(), [&](const Record& item) { return item.version.text == manifest.version; })) fail("LYK0004", "不能覆盖已发布版本：" + manifest.name + "@" + manifest.version);
    auto artifact = center / "artifacts" / (hash + ".lypkg");
    if (fs::exists(artifact)) { if (hashOf(readFile(artifact)) != hash) fail("LYK0005", "注册中心现有制品 SHA-256 不匹配：" + pathText(artifact)); }
    else replaceFile(artifact, archive);
    object.getArray("versions")->emplace_back(json::Object{{"version", manifest.version}, {"sha256", hash}, {"yanked", false}});
    replaceFile(index, render(json::Value(std::move(object))));
    std::cout << manifest.name << '@' << manifest.version << " sha256=" << hash << '\n';
}

constexpr auto packageUsage = "lyra new <目录> [--name <包名>] [--lib] | lock <目录> [--locked|--update] [--offline] | fetch <目录> [--locked] [--offline] | pack <目录> [-o <源码包>|--registry <本地目录>]";
}

int packageCommand(const std::vector<std::string_view>& args) {
    if (args.empty()) invalid(packageUsage);
    auto command = args.front();
    string name; bool library = false;
    fs::path directory, output, registry; PackageOptions options;
    for (std::size_t index = 1; index < args.size(); ++index) {
        auto arg = args[index];
        auto value = [&] { if (index + 1 >= args.size() || args[index + 1].empty() || args[index + 1].starts_with('-')) invalid(packageUsage); return string(args[++index]); };
        if (command == "new" && arg == "--lib" && !library) library = true;
        else if (command == "new" && arg == "--name" && name.empty()) name = value();
        else if (command == "pack" && arg == "-o" && output.empty() && registry.empty()) output = fromUtf8(value());
        else if (command == "pack" && arg == "--registry" && output.empty() && registry.empty()) { auto dir = value(); if (dir.find("://") != string::npos) invalid("HTTP(S) 注册中心只读，--registry 需要本地目录"); registry = fromUtf8(dir); }
        else if ((command == "lock" || command == "fetch") && arg == "--locked" && !options.locked) options.locked = true;
        else if ((command == "lock" || command == "fetch") && arg == "--offline" && !options.offline) options.offline = true;
        else if (command == "lock" && arg == "--update" && !options.update) options.update = true;
        else if (directory.empty() && !arg.empty() && arg.front() != '-') directory = fromUtf8(arg);
        else invalid(packageUsage);
    }
    if (directory.empty()) invalid(packageUsage);
    if (command == "new") newPackage(directory, name.empty() ? pathText(directory.lexically_normal().filename()) : name, library);
    else if (command == "pack") packPackage(directory, output, registry);
    else if (command == "fetch" || command == "lock") {
        auto graph = resolvePackages(directory, options);
        for (const auto& message : graph.warnings) std::cout << "warning: " << message << '\n';
        std::cout << pathText(graph.workspace / "lyra.lock") << " (" << graph.packages.size() << ")\n";
    } else invalid(packageUsage);
    return 0;
}

}
