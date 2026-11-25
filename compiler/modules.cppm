module;
#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <llvm/Support/SHA256.h>

export module lyra.modules;
import lyra.generics;
import lyra.packages;
import lyra.macros;
import lyra.numbers;
import lyra.platform;
import lyra.syntax;

export namespace lyra {
struct Compilation { std::vector<Source> sources; Module syntax; std::vector<std::filesystem::path> macroInputs{}; std::vector<NameReference> references{}; };
struct ParsedFileSyntax { std::string text; Module syntax; };
struct ParseCache { std::map<std::string, ParsedFileSyntax> files; };
Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<std::filesystem::path>& testFile = {}, const SourceOverlay* overlays = nullptr, ParseCache* parseCache = nullptr);
}

namespace lyra {
namespace fs = std::filesystem;
namespace {

struct LinkedDeclaration { SourceAnchor anchor; bool visible; };
enum class SymbolKind { Type, Callable, Constant, Extension };
struct Symbol {
    std::string name;
    SymbolKind kind;
    bool visible, imported{};
    std::vector<LinkedDeclaration> targets{};
    std::optional<SourceAnchor> renameTarget{};
};
struct Space {
    const ResolvedPackage* package;
    std::map<std::string, Symbol> definitions{};
    std::map<std::string, Import> imports{};
};

struct Linker {
    const std::vector<Source>& sources;
    std::map<std::string, Space>& spaces;
    const std::map<std::string, std::string>& prefixes;
    std::string entry;
    std::vector<NameReference>& references;
    bool indexing;

    void declare(std::size_t file, Identifier& id, bool visible, bool rewriteName, SymbolKind kind = SymbolKind::Type) {
        auto scope = sources[file].moduleId;
        auto& defs = spaces.at(scope).definitions;
        bool callable = kind == SymbolKind::Callable;
        auto name = scope == entry && callable && id.text == "main" ? id.text : scope + "::" + id.text;
        auto [symbol, inserted] = defs.try_emplace(id.text, Symbol{name, kind, visible});
        if (!inserted) {
            if (!callable || symbol->second.kind != SymbolKind::Callable || name == "main") throw at(sources[file], id.span, "LYN0002", "名称已声明：" + id.text);
            symbol->second.visible |= visible;
        }
        if (indexing) symbol->second.targets.push_back({{file, id.span}, visible});
        if (indexing) references.push_back({{file, id.span}, {file, id.span}, {}});
        if (rewriteName) id.text = std::move(name);
    }

    Symbol resolve(const Import& use, std::set<const Import*>& active) const {
        const auto& source = sources[use.file];
        if (!active.insert(&use).second) throw at(source, use.alias.span, "LYN0004", "重导出形成循环：" + use.alias.text);
        const auto& from = *spaces.at(source.moduleId).package;
        const auto& first = use.path.front();
        if (use.path.size() < 2) throw at(source, first.span, "LYN0003", "import 需要包、可选模块路径和声明名");
        auto package = from.id;
        std::size_t firstDir = 1;
        if (first.text == "std" && use.path.size() >= 3) {
            package = "$std/" + use.path[1].text;
            firstDir = 2;
        } else if (first.text != "self") {
            auto found = from.dependencies.find(first.text);
            if (found == from.dependencies.end()) throw at(source, first.span, "LYN0003", "清单没有此依赖简称：" + first.text);
            package = found->second;
        }
        auto target = prefixes.at(package);
        for (auto index = firstDir; index + 1 < use.path.size(); ++index) target += '/' + use.path[index].text;
        auto& name = use.path.back();
        auto found = spaces.find(target);
        if (found == spaces.end()) throw at(source, name.span, "LYN0003", "导入的源码模块不存在");
        auto& space = found->second;
        if (auto hit = space.definitions.find(name.text); hit != space.definitions.end() && hit->second.visible && !hit->second.imported) {
            auto result = hit->second;
            result.visible = use.isPublic;
            result.imported = true;
            std::erase_if(result.targets, [](const auto& target) { return !target.visible; });
            return result;
        }
        if (auto alias = space.imports.find(name.text); alias != space.imports.end() && alias->second.isPublic) {
            auto symbol = resolve(alias->second, active);
            symbol.visible = use.isPublic;
            if (indexing && alias->second.alias.span != alias->second.path.back().span)
                symbol.renameTarget = SourceAnchor{alias->second.file, alias->second.alias.span};
            return symbol;
        }
        throw at(source, name.span, "LYN0003", "目标模块没有公开声明：" + name.text);
    }

    Symbol import(const Import& use) {
        std::set<const Import*> active;
        auto result = resolve(use, active);
        if (indexing && result.targets.size() == 1) {
            auto target = result.targets.front().anchor;
            references.push_back({{use.file, use.path.back().span}, target, result.renameTarget});
            if (use.alias.span != use.path.back().span) {
                result.renameTarget = SourceAnchor{use.file, use.alias.span};
                references.push_back({*result.renameTarget, target, result.renameTarget});
            }
        }
        return result;
    }

    void collect(Module& syntax, bool rewriteNames) {
        for (auto& trait : syntax.traits) declare(trait.file, trait.name, trait.isPublic, rewriteNames);
        for (auto& record : syntax.aggregates) declare(record.file, record.name, record.isPublic, rewriteNames);
        for (auto& item : syntax.enums) declare(item.file, item.name, item.isPublic, rewriteNames);
        for (auto& item : syntax.exceptions) declare(item.file, item.name, item.isPublic, rewriteNames);
        for (auto& item : syntax.aliases) declare(item.file, item.name, item.isPublic, rewriteNames);
        for (auto& extension : syntax.extensions) declare(extension.file, extension.name, extension.isPublic, rewriteNames, SymbolKind::Extension);
        for (auto& fn : syntax.functions) if (!fn.owner && !fn.extensionGroup)
            declare(fn.file, fn.name, fn.isPublic, rewriteNames, fn.constant == ConstKind::Value ? SymbolKind::Constant : SymbolKind::Callable);
        for (const auto& use : syntax.imports) {
            const auto& source = sources[use.file];
            if (source.origin) continue;
            auto& space = spaces.at(source.moduleId);
            if (space.definitions.contains(use.alias.text) || !space.imports.emplace(use.alias.text, use).second) {
                throw at(source, use.alias.span, "LYN0002", "导入名称已被使用：" + use.alias.text);
            }
        }
    }

    void rewrite(Module& syntax) {
        collect(syntax, true);
        std::map<std::size_t, std::map<std::string, const Import*>> generatedImports;
        for (const auto& use : syntax.imports) if (const auto& source = sources[use.file]; source.origin) {
            if (!generatedImports[use.file].emplace(use.alias.text, &use).second) throw at(source, use.alias.span, "LYN0002", "生成代码中重复的导入名称：" + use.alias.text);
        }
        for (auto& [moduleId, space] : spaces) {
            for (const auto& [name, use] : space.imports) {
                space.definitions.emplace(name, import(use));
            }
        }
        std::map<std::size_t, std::map<std::string, Symbol>> generatedScopes;
        for (const auto& [file, imports] : generatedImports) {
            auto& scope = generatedScopes[file] = spaces.at(sources[file].moduleId).definitions;
            for (const auto& [name, use] : imports) scope[name] = import(*use);
        }
        auto scopeOf = [&](std::size_t file) -> const auto& {
            auto generated = generatedScopes.find(file);
            return generated == generatedScopes.end() ? spaces.at(sources[file].moduleId).definitions : generated->second;
        };
        auto renameTypeIn = [&](std::size_t file, Identifier& type, std::span<const Identifier> parameters = {}) {
            const auto& scope = scopeOf(file);
            if (indexing && type.span.end > type.span.begin) {
                const auto& source = sources[file];
                auto text = source.text.substr(type.span.begin, type.span.end - type.span.begin);
                auto fragment = sourceText(source.path, text, {});
                bool projection = false;
                for (const auto& token : syntaxTokens(fragment, false, true)) {
                    auto name = std::string_view(text).substr(token.span.begin, token.span.end - token.span.begin);
                    if (name == ".") { projection = true; continue; }
                    if (std::exchange(projection, false) || token.kind != "name") continue;
                    auto after = text.find_first_not_of(" \t\r\n", token.span.end);
                    if (after != text.npos && text[after] == '=') continue;
                    if (name.starts_with('`')) name = name.substr(1, name.size() - 2);
                    if (std::ranges::any_of(parameters, [&](const auto& parameter) { return parameter.text == name; })) continue;
                    if (auto found = scope.find(std::string(name)); found != scope.end() && found->second.kind == SymbolKind::Type && found->second.targets.size() == 1)
                        references.push_back({{file, {type.span.begin + token.span.begin, type.span.begin + token.span.end}}, found->second.targets.front().anchor, found->second.renameTarget});
                }
            }
            type.text = mapTypeNames(typeForm(type.text), [&](const std::string& name) {
                if (std::ranges::any_of(parameters, [&](const auto& parameter) { return parameter.text == name; })) return name;
                auto found = scope.find(name); return found == scope.end() ? name : found->second.name;
            });
        };
        syntax.importedExtensions.resize(sources.size());
        if (!syntax.extensions.empty()) for (std::size_t file = 0; file < sources.size(); ++file) {
            std::set<std::string> groups;
            for (const auto& [alias, symbol] : scopeOf(file)) if (symbol.imported && symbol.kind == SymbolKind::Extension) groups.insert(symbol.name);
            syntax.importedExtensions[file].assign(groups.begin(), groups.end());
        }
        for (auto& extension : syntax.extensions) {
            renameTypeIn(extension.file, extension.receiver, extension.typeParameters);
            for (auto& bound : extension.bounds) renameTypeIn(extension.file, bound.trait);
        }
        for (auto& record : syntax.aggregates) {
            for (auto& bound : record.bounds) renameTypeIn(record.file, bound.trait);
            for (auto& field : record.fields) renameTypeIn(record.file, field.type, record.typeParameters);
            for (auto& member : record.associated) renameTypeIn(record.file, member.type, record.typeParameters);
        }
        for (auto& item : syntax.enums) {
            for (auto& bound : item.bounds) renameTypeIn(item.file, bound.trait);
            for (auto& entry : item.cases) for (auto& field : entry.fields) renameTypeIn(item.file, field.type, item.typeParameters);
        }
        for (auto& alias : syntax.aliases) renameTypeIn(alias.file, alias.target);
        for (auto& trait : syntax.traits) for (auto& method : trait.methods) {
            renameTypeIn(trait.file, method.result, trait.associated);
            for (auto& parameter : method.parameters) renameTypeIn(trait.file, parameter.type, trait.associated);
        }
        for (auto& fn : syntax.functions) {
            const auto& scope = scopeOf(fn.file);
            auto rename = [&](Identifier& name) {
                if (auto found = scope.find(name.text); found != scope.end()) name.text = found->second.name;
            };
            auto renameType = [&](Identifier& name) {
                renameTypeIn(fn.file, name, fn.typeParameters);
            };
            if (fn.owner) rename(*fn.owner);
            if (fn.extensionGroup) rename(*fn.extensionGroup);
            for (auto& bound : fn.bounds) rename(bound.trait);
            if (!fn.result.text.empty()) renameType(fn.result);
            for (auto& type : fn.throwsTypes) renameType(type);
            for (auto& param : fn.parameters) renameType(param.type);
            auto reference = [&](Identifier& name, std::optional<SymbolKind> kind = {}) {
                if (auto found = scope.find(name.text); found != scope.end()) {
                    if (kind && found->second.kind != *kind) return;
                    name.linked = found->second.name;
                    name.exportedOnly = found->second.imported;
                    name.renameTarget = found->second.renameTarget;
                }
                else if (!kind && name.text == "main") name.linked = sources[fn.file].moduleId + "::main";
            };
            auto renamePattern = [&](auto&& self, Pattern& pattern) -> void {
                if (auto* name = std::get_if<PatternName>(&pattern.node)) {
                    if (name->owner) reference(*name->owner, SymbolKind::Type);
                    else if (name->labels) reference(name->name, SymbolKind::Type);
                    else if (!name->escaped) reference(name->name, SymbolKind::Constant);
                }
                for (auto& part : pattern.parts) self(self, part);
            };
            for (auto& expr : fn.expressions) {
                if (auto* name = std::get_if<Identifier>(&expr.node)) reference(*name);
                auto renameQualifier = [&](TypeQualifier& owner) {
                    reference(owner.name);
                    for (auto& type : owner.arguments) renameType(type);
                };
                if (auto* member = std::get_if<QualifiedMember>(&expr.node)) renameQualifier(member->owner);
                if (auto* call = std::get_if<Call>(&expr.node)) {
                    if (call->typeOwner) renameQualifier(*call->typeOwner);
                    else if (!call->receiver && !call->function) reference(call->name);
                    for (auto& type : call->typeArguments) renameType(type);
                }
                if (auto* binding = std::get_if<Binding>(&expr.node); binding && binding->annotation) renameType(*binding->annotation);
                if (auto* binding = std::get_if<PatternBinding>(&expr.node)) {
                    if (binding->annotation) renameType(*binding->annotation);
                    renamePattern(renamePattern, binding->pattern);
                }
                if (auto* operation = std::get_if<TypeOperation>(&expr.node)) renameType(operation->type);
                if (auto* attempt = std::get_if<Attempt>(&expr.node)) for (auto& arm : attempt->arms) if (arm.binding) renameType(arm.binding->type);
                if (auto* match = std::get_if<Match>(&expr.node)) for (auto& arm : match->arms) renamePattern(renamePattern, arm.pattern);
                if (auto* loop = std::get_if<For>(&expr.node)) renamePattern(renamePattern, loop->item);
                if (auto* loop = std::get_if<While>(&expr.node); loop && loop->pattern) renamePattern(renamePattern, *loop->pattern);
                if (auto* lambda = std::get_if<Lambda>(&expr.node)) {
                    for (auto& param : lambda->parameters) if (param.type) renameType(*param.type);
                    if (lambda->result) renameType(*lambda->result);
                }
            }
        }
    }
};

struct DeclarationTypes {
    const Linker& linker;
    std::map<std::string, TypeAlias> aliases;
    struct Shape { std::size_t arity; bool trait{}; std::set<std::string> members{}; };
    std::map<std::string, Shape> shapes;

    DeclarationTypes(const Linker& linker, const Module& syntax) : linker(linker) {
        auto name = [&](const auto& item) { return linker.sources[item.file].moduleId + "::" + item.name.text; };
        for (const auto& alias : syntax.aliases) aliases.emplace(name(alias), alias);
        for (const auto& item : syntax.aggregates) shapes.emplace(name(item), Shape{item.typeParameters.size()});
        for (const auto& item : syntax.enums) shapes.emplace(name(item), Shape{item.typeParameters.size()});
        for (const auto& item : syntax.exceptions) shapes.emplace(name(item), Shape{0});
        for (const auto& item : syntax.traits) {
            auto& shape = shapes.emplace(name(item), Shape{item.associated.size(), true}).first->second;
            for (const auto& member : item.associated) shape.members.insert(member.text);
        }
    }

    std::string fingerprint() const {
        llvm::SHA256 hash;
        auto text = [&](std::string_view value) { hash.update(std::to_string(value.size()) + ':'); hash.update(value); };
        for (const auto& [scope, space] : linker.spaces) {
            text("module"); text(scope); text(space.package->id);
            for (const auto& [alias, dependency] : space.package->dependencies) {
                text("dependency"); text(alias); text(dependency);
            }
            for (const auto& [name, symbol] : space.definitions) if (symbol.kind == SymbolKind::Type) {
                text("type"); text(name); text(symbol.visible ? "public" : "private");
            }
            for (const auto& [name, use] : space.imports) {
                text("import"); text(name); text(use.isPublic ? "public" : "private");
                text(std::to_string(use.path.size()));
                for (const auto& part : use.path) text(part.text);
            }
        }
        for (const auto& [name, alias] : aliases) { text("alias"); text(name); text(alias.target.text); }
        for (const auto& [name, shape] : shapes) {
            text("shape"); text(name); text(std::to_string(shape.arity)); text(shape.trait ? "trait" : "value");
            for (const auto& member : shape.members) text(member);
        }
        const auto digest = hash.final();
        return {reinterpret_cast<const char*>(digest.data()), digest.size()};
    }

    MacroType resolve(std::size_t file, const Identifier& annotation, std::span<const Identifier> parameters) const {
        std::vector<std::string> active;
        std::size_t visited = 0;
        auto visit = [&](auto&& self, const TypeForm& form, std::size_t from, Span span, std::span<const Identifier> generics, unsigned depth) -> MacroType {
            const auto& source = linker.sources[from];
            if (depth >= 128 || ++visited > 4096) throw at(source, span, "LYM0002", "声明类型查询超过嵌套或节点预算");
            auto make = [&](std::string kind, std::string name, std::string module = {}) {
                MacroType result{std::move(kind), std::move(name), std::move(module)};
                for (const auto& argument : form.arguments) result.arguments.push_back(self(self, argument, from, span, generics, depth + 1));
                return result;
            };
            const auto& name = form.name;
            if (name == "$fn") return make("function", "");
            if (name.starts_with("$assoc:")) return make("projection", name.substr(7));
            if (name.starts_with("$bind:")) return make("binding", name.substr(6));
            const bool dynamic = name.starts_with("$dyn:");
            auto lookup = dynamic ? name.substr(5) : name;
            if (!dynamic && form.arguments.empty() && std::ranges::find(generics, name, &Identifier::text) != generics.end()) return make("parameter", name);
            const auto& scope = linker.spaces.at(source.moduleId);
            std::optional<Symbol> symbol;
            if (auto found = scope.definitions.find(lookup); found != scope.definitions.end()) symbol = found->second;
            else if (auto found = scope.imports.find(lookup); found != scope.imports.end()) {
                std::set<const Import*> imports;
                symbol = linker.resolve(found->second, imports);
            }
            if (symbol && symbol->kind == SymbolKind::Type) {
                auto separator = symbol->name.rfind("::");
                if (!dynamic && form.arguments.empty()) if (auto alias = aliases.find(symbol->name); alias != aliases.end()) {
                    if (std::ranges::contains(active, symbol->name)) throw at(source, span, "LYT0003", "透明类型别名形成循环：" + lookup);
                    active.push_back(symbol->name);
                    const auto& target = alias->second;
                    auto result = self(self, typeForm(target.target.text), target.file, target.target.span, std::span<const Identifier>{}, depth + 1);
                    active.pop_back();
                    return result;
                }
                auto shape = shapes.find(symbol->name);
                if (shape == shapes.end() || shape->second.trait != dynamic || shape->second.arity != form.arguments.size())
                    throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + lookup);
                if (dynamic) for (const auto& binding : form.arguments) if (!shape->second.members.contains(binding.name.substr(6)))
                    throw at(source, span, "LYG0007", "动态接口没有关联类型：" + binding.name.substr(6));
                auto module = symbol->name.substr(0, separator);
                auto standard = module.starts_with("$std/");
                if (standard) module = "std." + module.substr(5);
                else {
                    auto path = module.find('/');
                    auto prefix = linker.spaces.at(module).package->name;
                    module = path == std::string::npos ? std::move(prefix) : prefix + module.substr(path);
                    std::ranges::replace(module, '/', '.');
                }
                return make(dynamic ? "dyn" : standard ? "standard" : "named", symbol->name.substr(separator + 2), std::move(module));
            }
            if (!symbol && !dynamic) {
                if (form.arguments.empty() && (numberType(name) || name == "Bool" || name == "String" || name == "Char" || name == "Bytes" || name == "Unit" || name == "Never" || name == "CPtr" || name == "CCallback" || name == "Exception" || name == "ChannelClosed" || name == "Any" || name == "Error"))
                    return make("builtin", name);
                if (form.arguments.size() == 1 && (name == "List" || name == "Option" || name == "Range" || name == "Task" || name == "TaskLocal" || name == "Channel"))
                    return make("builtin", name);
                if (form.arguments.size() == 2 && name == "Result") return make("builtin", name);
            }
            throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + lookup);
        };
        return visit(visit, typeForm(annotation.text), file, annotation.span, parameters, 0);
    }
};

std::string parsedFileKey(const fs::path& path) {
    std::error_code error;
    auto canonical = fs::weakly_canonical(path, error);
    return pathText(error ? fs::absolute(path).lexically_normal() : canonical);
}

const Module& cachedFileSyntax(const Source& source, ParseCache& cache) {
    auto key = parsedFileKey(source.path);
    auto found = cache.files.find(key);
    if (found != cache.files.end() && found->second.text == source.text) return found->second.syntax;
    if (cache.files.size() >= 4096) cache.files.clear();
    Module syntax;
    parseFile(syntax, source, 0);
    auto [entry, inserted] = cache.files.insert_or_assign(std::move(key), ParsedFileSyntax{source.text, std::move(syntax)});
    return entry->second.syntax;
}

void appendCachedSyntax(Module& target, const Module& cached, std::size_t file) {
    const auto functionOffset = target.functions.size();
    const auto aggregateOffset = target.aggregates.size();
    const auto enumOffset = target.enums.size();
    for (auto function : cached.functions) {
        function.file = file;
        target.functions.push_back(std::move(function));
    }
    for (auto aggregate : cached.aggregates) {
        aggregate.file = file;
        for (auto& field : aggregate.fields) if (field.defaultValue) field.defaultValue->function += functionOffset;
        target.aggregates.push_back(std::move(aggregate));
    }
    for (auto import : cached.imports) {
        import.file = file;
        target.imports.push_back(std::move(import));
    }
    for (auto expansion : cached.expansions) {
        expansion.file = file;
        expansion.target += expansion.kind == ExpansionTarget::Aggregate ? aggregateOffset
            : expansion.kind == ExpansionTarget::Enumeration ? enumOffset : functionOffset;
        target.expansions.push_back(std::move(expansion));
    }
    for (auto trait : cached.traits) {
        trait.file = file;
        target.traits.push_back(std::move(trait));
    }
    for (auto item : cached.enums) {
        item.file = file;
        target.enums.push_back(std::move(item));
    }
    for (auto extension : cached.extensions) {
        extension.file = file;
        for (auto& property : extension.properties) property.getter += functionOffset;
        target.extensions.push_back(std::move(extension));
    }
    for (auto exception : cached.exceptions) {
        exception.file = file;
        target.exceptions.push_back(std::move(exception));
    }
    for (auto alias : cached.aliases) {
        alias.file = file;
        target.aliases.push_back(std::move(alias));
    }
}

}

Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<fs::path>& testFile, const SourceOverlay* overlays, ParseCache* parseCache) {
    std::map<std::string, const ResolvedPackage*> packages;
    std::map<std::string, std::string> prefixes;
    for (const auto& package : graph.packages) {
        prefixes.emplace(package.id, "pkg" + std::to_string(packages.size()));
        packages.emplace(package.id, &package);
    }
    std::set<std::string> reachable;
    std::vector<std::string> todo{entry};
    while (!todo.empty()) {
        auto id = todo.back(); todo.pop_back();
        if (reachable.insert(id).second) for (const auto& [alias, target] : packages.at(id)->dependencies) todo.push_back(target);
    }
    Compilation program;
    std::map<std::string, Space> spaces;
    for (const auto& id : reachable) {
        auto package = packages.at(id);
        auto directory = package->root / "src";
        std::vector<fs::path> paths;
        auto first = fs::is_directory(directory) ? fs::recursive_directory_iterator(directory) : fs::recursive_directory_iterator{};
        for (auto item = std::move(first); item != fs::recursive_directory_iterator{}; ++item) {
            if (item->is_directory() && pathText(item->path().filename()).starts_with('.')) { item.disable_recursion_pending(); continue; }
            if (item->is_symlink()) throw Diagnostic{"LYI0001", "源码不允许符号链接：" + pathText(item->path())};
            if (item->is_regular_file() && item->path().extension() == ".ly") paths.push_back(item->path());
        }
        if (overlays) for (const auto& [path, text] : *overlays) {
            auto relative = path.lexically_relative(directory);
            if (relative.empty() || relative == "." || *relative.begin() == ".." || path.extension() != ".ly") continue;
            if (std::ranges::any_of(relative, [](const auto& part) { return pathText(part).starts_with('.'); })) continue;
            if (std::ranges::find(paths, path) == paths.end()) paths.push_back(path);
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            auto module = prefixes.at(id);
            auto relative = path.parent_path().lexically_relative(directory);
            if (relative != ".") module += '/' + pathText(relative);
            spaces.try_emplace(module, Space{package});
            program.sources.push_back(loadSource(path, module, overlays));
        }
    }
    auto root = prefixes.at(entry);
    if (testFile) {
        root += "#test";
        spaces.emplace(root, Space{packages.at(entry)});
        program.sources.push_back(loadSource(*testFile, root, overlays));
    }
    std::map<std::string, MacroPackage> macroPackages;
    auto macroCache = overlays ? fs::path{} : cacheOverride();
    if (!overlays) macroCache = (macroCache.empty() ? graph.workspace / ".lyra" / "cache" : macroCache) / "compiler" / "macros";
    for (const auto& [module, space] : spaces) macroPackages.emplace(module, MacroPackage{space.package->root, space.package->macroInputs, overlays, macroCache});
    for (std::size_t file = 0; file < program.sources.size(); ++file) {
        auto& source = program.sources[file];
        if (source.text.find_first_of("#@") == std::string::npos) {
            if (parseCache) appendCachedSyntax(program.syntax, cachedFileSyntax(source, *parseCache), file);
            else parseFile(program.syntax, source, file);
        }
        else parsePackageFile(program.syntax, source, file, macroPackages.at(source.moduleId), program.macroInputs);
    }
    std::map<std::string, ResolvedPackage> bundled;
    auto standard = sdkRoot() / "share" / "lyra" / "std";
    std::size_t loadedImports = 0;
    auto loadBundled = [&] {
        for (; loadedImports < program.syntax.imports.size(); ++loadedImports) {
            const auto& use = program.syntax.imports[loadedImports];
            if (use.path.size() < 3 || use.path[0].text != "std") continue;
            auto name = use.path[1].text;
            auto key = "$std/" + name;
            if (bundled.contains(key)) continue;
            auto packageRoot = standard / name;
            if (!fs::is_directory(packageRoot / "src")) throw at(program.sources[use.file], use.path[1].span, "LYN0003", "找不到随工具链安装的标准包：" + name);
            auto& package = bundled.emplace(key, ResolvedPackage{key, name, "builtin", packageRoot, {}, true}).first->second;
            prefixes.emplace(key, key);
            spaces.emplace(key, Space{&package});
            for (auto source : loadSources(packageRoot)) {
                source.moduleId = key;
                if (overlays) if (auto found = overlays->find(source.path); found != overlays->end())
                    source = sourceText(source.path, found->second, key);
                program.sources.push_back(std::move(source));
                auto file = program.sources.size() - 1;
                if (parseCache && program.sources.back().text.find_first_of("#@") == std::string::npos)
                    appendCachedSyntax(program.syntax, cachedFileSyntax(program.sources.back(), *parseCache), file);
                else parseFile(program.syntax, program.sources.back(), file);
            }
        }
    };
    loadBundled();
    if (!program.syntax.expansions.empty()) {
        auto declarations = spaces;
        Linker linker{program.sources, declarations, prefixes, root, program.references, false};
        linker.collect(program.syntax, false);
        DeclarationTypes types(linker, program.syntax);
        expandMacros(program.syntax, program.sources, macroPackages, program.macroInputs,
            [&](auto file, const auto& annotation, auto parameters) { return types.resolve(file, annotation, parameters); }, types.fingerprint());
        loadBundled();
    }
    Linker{program.sources, spaces, prefixes, root, program.references, overlays != nullptr}.rewrite(program.syntax);
    return program;
}

}
