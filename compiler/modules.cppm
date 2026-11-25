module;
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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

struct ImportTarget { std::string package, module; fs::path directory; };
using SymbolDependencies = std::map<std::string, std::set<std::string>>;

const std::string& declarationGroup(const Function& function) {
    return function.owner ? function.owner->text : function.extensionGroup ? function.extensionGroup->text : function.name.text;
}

ImportTarget importTarget(const Source& source, const ResolvedPackage& from, const Import& use,
    const std::map<std::string, std::string>& prefixes) {
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
    auto prefix = prefixes.find(package);
    if (prefix == prefixes.end()) throw at(source, first.span, "LYN0003", "导入的包不在当前编译阶段：" + package);
    ImportTarget target{std::move(package), prefix->second, {}};
    for (auto index = firstDir; index + 1 < use.path.size(); ++index) {
        target.module += '/' + use.path[index].text;
        target.directory /= fromUtf8(use.path[index].text);
    }
    return target;
}

struct Linker {
    const std::vector<Source>& sources;
    std::map<std::string, Space>& spaces;
    const std::map<std::string, std::string>& prefixes;
    std::string entry;
    std::vector<NameReference>& references;
    bool indexing;
    bool macroPhase{};

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
        auto target = importTarget(source, *spaces.at(source.moduleId).package, use, prefixes);
        auto& name = use.path.back();
        auto found = spaces.find(target.module);
        if (found == spaces.end()) throw at(source, name.span, macroPhase ? "LYM0004" : "LYN0003", "导入的手写源码模块不存在");
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
        throw at(source, name.span, macroPhase ? "LYM0004" : "LYN0003", "目标模块没有可用的公开声明：" + name.text);
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
            if (use.kind != ImportKind::Runtime) continue;
            const auto& source = sources[use.file];
            if (source.origin) continue;
            auto& space = spaces.at(source.moduleId);
            if (space.definitions.contains(use.alias.text) || !space.imports.emplace(use.alias.text, use).second) {
                throw at(source, use.alias.span, "LYN0002", "导入名称已被使用：" + use.alias.text);
            }
        }
    }

    void rewrite(Module& syntax, SymbolDependencies* dependencies = nullptr) {
        collect(syntax, true);
        if (macroPhase) {
            auto& scope = spaces.at("$macroImplementation");
            for (const auto& [name, symbol] : spaces.at("$macroPrelude").definitions)
                if (scope.imports.contains(name) || !scope.definitions.emplace(name, symbol).second)
                    throw at(sources[1], {0, 0}, "LYM0002", "宏预定义名称不能被重定义：" + name);
        }
        std::map<std::size_t, std::map<std::string, const Import*>> generatedImports;
        for (const auto& use : syntax.imports) if (const auto& source = sources[use.file]; use.kind == ImportKind::Runtime && source.origin) {
            if (!generatedImports[use.file].emplace(use.alias.text, &use).second) throw at(source, use.alias.span, "LYN0002", "生成代码中重复的导入名称：" + use.alias.text);
        }
        for (auto& [moduleId, space] : spaces) {
            for (const auto& [name, use] : space.imports) {
                try { space.definitions.emplace(name, import(use)); }
                catch (const Diagnostic&) {
                    if (!macroPhase || use.file < 2) throw;
                    space.definitions.emplace(name, Symbol{"$unexpanded::" + name, SymbolKind::Callable, false, true});
                }
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
        std::set<std::string>* needs{};
        auto group = [&](const auto& name) { if (dependencies) needs = &(*dependencies)[name]; };
        std::map<std::string, std::vector<std::string>> moduleExtensions;
        if (dependencies) for (const auto& extension : syntax.extensions)
            moduleExtensions[sources[extension.file].moduleId].push_back(extension.name.text);
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
                auto found = scope.find(name);
                if (found == scope.end()) return name;
                if (needs) needs->insert(found->second.name);
                return found->second.name;
            });
        };
        syntax.importedExtensions.resize(sources.size());
        if (!syntax.extensions.empty()) for (std::size_t file = 0; file < sources.size(); ++file) {
            std::set<std::string> groups;
            for (const auto& [alias, symbol] : scopeOf(file)) if (symbol.imported && symbol.kind == SymbolKind::Extension) groups.insert(symbol.name);
            syntax.importedExtensions[file].assign(groups.begin(), groups.end());
        }
        auto renameBounds = [&](const auto& declaration, auto& bounds) {
            for (auto& bound : bounds) {
                renameTypeIn(declaration.file, bound.trait);
                for (auto& binding : bound.bindings) renameTypeIn(declaration.file, binding.type, declaration.typeParameters);
            }
        };
        for (auto& extension : syntax.extensions) {
            group(extension.name.text);
            renameTypeIn(extension.file, extension.receiver, extension.typeParameters);
            renameBounds(extension, extension.bounds);
        }
        for (auto& record : syntax.aggregates) {
            group(record.name.text);
            renameBounds(record, record.bounds);
            for (auto& field : record.fields) {
                renameTypeIn(record.file, field.type, record.typeParameters);
                if (needs && field.defaultValue) needs->insert(syntax.functions[field.defaultValue->function].name.text);
            }
            for (auto& member : record.associated) renameTypeIn(record.file, member.type, record.typeParameters);
        }
        for (auto& item : syntax.enums) {
            group(item.name.text);
            renameBounds(item, item.bounds);
            for (auto& entry : item.cases) for (auto& field : entry.fields) renameTypeIn(item.file, field.type, item.typeParameters);
        }
        for (auto& alias : syntax.aliases) {
            group(alias.name.text);
            renameTypeIn(alias.file, alias.target, alias.typeParameters);
        }
        for (auto& trait : syntax.traits) {
            group(trait.name.text);
            for (auto& method : trait.methods) {
                renameTypeIn(trait.file, method.result, trait.associated);
                for (auto& parameter : method.parameters) renameTypeIn(trait.file, parameter.type, trait.associated);
            }
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
            group(declarationGroup(fn));
            if (needs) {
                for (const auto& name : syntax.importedExtensions[fn.file]) needs->insert(name);
                if (auto found = moduleExtensions.find(sources[fn.file].moduleId); found != moduleExtensions.end())
                    needs->insert(found->second.begin(), found->second.end());
            }
            renameBounds(fn, fn.bounds);
            if (!fn.result.text.empty()) renameType(fn.result);
            for (auto& type : fn.throwsTypes) renameType(type);
            for (auto& param : fn.parameters) renameType(param.type);
            auto reference = [&](Identifier& name, std::optional<SymbolKind> kind = {}) {
                if (auto found = scope.find(name.text); found != scope.end()) {
                    if (kind && found->second.kind != *kind) return;
                    name.linked = found->second.name;
                    name.exportedOnly = found->second.imported;
                    name.renameTarget = found->second.renameTarget;
                    if (needs) needs->insert(found->second.name);
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
        for (const auto& [name, alias] : aliases) {
            text("alias"); text(name); text(alias.target.text); text(std::to_string(alias.typeParameters.size()));
            for (const auto& parameter : alias.typeParameters) text(parameter.text);
        }
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
        using Bindings = std::map<std::string, MacroType>;
        auto visit = [&](auto&& self, const TypeForm& form, std::size_t from, Span span, const Bindings& generics, unsigned depth) -> MacroType {
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
            if (!dynamic && form.arguments.empty()) if (auto parameter = generics.find(name); parameter != generics.end()) return parameter->second;
            const auto& scope = linker.spaces.at(source.moduleId);
            std::optional<Symbol> symbol;
            if (auto found = scope.definitions.find(lookup); found != scope.definitions.end()) symbol = found->second;
            else if (auto found = scope.imports.find(lookup); found != scope.imports.end()) {
                std::set<const Import*> imports;
                symbol = linker.resolve(found->second, imports);
            }
            if (symbol && symbol->kind == SymbolKind::Type) {
                auto separator = symbol->name.rfind("::");
                if (!dynamic) if (auto alias = aliases.find(symbol->name); alias != aliases.end()) {
                    const auto& target = alias->second;
                    if (form.arguments.size() != target.typeParameters.size()) throw at(source, span, "LYT0003", "类型别名的类型实参数量不符：" + lookup);
                    Bindings mapped;
                    for (std::size_t slot = 0; slot < form.arguments.size(); ++slot)
                        mapped.emplace(target.typeParameters[slot].text, self(self, form.arguments[slot], from, span, generics, depth + 1));
                    if (std::ranges::contains(active, symbol->name)) throw at(source, span, "LYT0003", "透明类型别名形成循环：" + lookup);
                    active.push_back(symbol->name);
                    auto result = self(self, typeForm(target.target.text), target.file, target.target.span, mapped, depth + 1);
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
                if (form.arguments.size() == 1 && (name == "List" || name == "FrozenList" || name == "Option" || name == "Range" || name == "Task" || name == "TaskLocal" || name == "Channel" || name == "TaskOutcome"))
                    return make("builtin", name);
                if (form.arguments.size() == 2 && name == "Result") return make("builtin", name);
            }
            throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + lookup);
        };
        Bindings generics;
        for (const auto& param : parameters) generics.emplace(param.text, MacroType{"parameter", param.text, {}});
        return visit(visit, typeForm(annotation.text), file, annotation.span, generics, 0);
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

struct MacroImports {
    const std::vector<Source>& sources;
    const std::map<std::string, Space>& spaces;
    const std::map<std::string, const ResolvedPackage*>& packages;
    const std::map<std::string, std::string>& prefixes;
    const SourceOverlay* overlays;
    std::map<std::tuple<std::string, ImportKind, std::string>, const Import*> aliases{};
    std::map<std::pair<const Import*, MacroKind>, std::optional<MacroOrigin>> bindings{};
    std::map<fs::path, bool> exports{};

    static ImportKind family(MacroKind kind) { return kind == MacroKind::Decorator ? ImportKind::Decorator : ImportKind::Macro; }

    bool present(const fs::path& path) const { return (overlays && overlays->contains(path)) || fs::is_regular_file(path); }

    std::optional<MacroOrigin> definition(const ResolvedPackage& package, const fs::path& directory, const Source& source,
        const Identifier& name, MacroKind kind, bool imported) {
        auto path = package.root / "src" / directory / ('.' + std::string(macroDirectory(kind))) / (name.text + ".ly");
        if (!present(path)) return {};
        auto relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(package.root));
        if (relative.empty() || *relative.begin() == "..") throw at(source, name.span, "LYM0002", "宏实现必须位于声明包内");
        if (imported) {
            auto found = exports.find(path);
            if (found == exports.end()) {
                auto size = overlays && overlays->contains(path) ? overlays->at(path).size() : fs::file_size(path);
                if (size > macroInputByteLimit) throw at(source, name.span, "LYM0002", "宏实现超过大小限制");
                auto implementation = loadSource(path, {}, overlays);
                Module code;
                std::vector<ExpressionExpansion> calls;
                parseFile(code, implementation, 0, &calls, true);
                bool visible = std::ranges::any_of(code.functions, [&](const auto& fn) {
                    return !fn.owner && !fn.extensionGroup && fn.name.text == name.text && fn.isPublic;
                });
                found = exports.emplace(path, visible).first;
            }
            if (!found->second) return {};
        }
        return MacroOrigin{std::move(path), package.id, name.text, imported};
    }

    std::optional<MacroOrigin> resolve(const Import& use, MacroKind kind, std::set<const Import*>& active) {
        auto key = std::pair{&use, kind};
        if (auto bound = bindings.find(key); bound != bindings.end()) return bound->second;
        const auto& source = sources[use.file];
        if (!active.insert(&use).second) throw at(source, use.alias.span, "LYM0004", "编译期宏重导出形成依赖环：" + use.alias.text);
        auto target = importTarget(source, *spaces.at(source.moduleId).package, use, prefixes);
        const auto& name = use.path.back();
        std::optional<MacroOrigin> origin;
        if (auto alias = aliases.find({target.module, use.kind, name.text}); alias != aliases.end() && alias->second->isPublic)
            origin = resolve(*alias->second, kind, active);
        else if (auto provider = packages.find(target.package); provider != packages.end())
            origin = definition(*provider->second, target.directory, source, name, kind, true);
        active.erase(&use);
        return bindings.emplace(key, std::move(origin)).first->second;
    }

    void collect(const Module& syntax) {
        for (const auto& use : syntax.imports) if (use.kind != ImportKind::Runtime) {
            const auto& source = sources[use.file];
            if (!aliases.emplace(std::tuple{source.moduleId, use.kind, use.alias.text}, &use).second)
                throw at(source, use.alias.span, "LYN0002", "宏或装饰器导入名称已被使用：" + use.alias.text);
        }
        for (const auto& [name, use] : aliases) {
            std::set<const Import*> active;
            bool found = false;
            for (auto kind : {MacroKind::Declaration, MacroKind::Expression, MacroKind::Decorator})
                if (family(kind) == use->kind) found |= resolve(*use, kind, active).has_value();
            if (!found) throw at(sources[use->file], use->path.back().span, "LYM0001", "找不到以 pub fn 导出的宏或装饰器：" + use->path.back().text);
        }
    }

    MacroOrigin resolve(const Source& source, const Identifier& name, MacroKind kind) {
        auto fail = [&] { return at(source, name.span, "LYM0001", "找不到此位置所需的" + std::string(kind == MacroKind::Decorator ? "装饰器 @" : kind == MacroKind::Expression ? "表达式宏 #" : "声明宏 #") + name.text); };
        if (auto alias = aliases.find({source.moduleId, family(kind), name.text}); alias != aliases.end()) {
            if (const auto& origin = bindings.at({alias->second, kind})) return *origin;
            throw fail();
        }
        const auto& package = *spaces.at(source.moduleId).package;
        auto directory = source.path.parent_path().lexically_relative(package.root / "src");
        if (!directory.empty() && directory != "." && *directory.begin() != "..")
            if (auto local = definition(package, directory, source, name, kind, false)) return *local;
        if (auto local = definition(package, {}, source, name, kind, false)) return *local;
        auto bundled = sdkRoot() / "share/lyra/std/meta" / macroDirectory(kind) / (name.text + ".ly");
        if (!present(bundled)) {
            if (kind == MacroKind::Decorator && name.text == "derive")
                throw at(source, name.span, "LYM0001", "derive 是声明宏；请写 #derive(Json)");
            throw fail();
        }
        return {std::move(bundled), package.id, name.text, false};
    }
};

void keepMacroDependencies(Module& syntax, const SymbolDependencies& dependencies) {
    std::set<std::string> kept;
    std::vector<std::string> work;
    for (const auto& [name, needs] : dependencies)
        if (name.starts_with("$macroImplementation::") || name.starts_with("$macroPrelude::")) work.push_back(name);
    for (std::size_t index = 0; index < work.size(); ++index) {
        const auto& name = work[index];
        if (kept.insert(name).second) if (auto found = dependencies.find(name); found != dependencies.end())
            work.insert(work.end(), found->second.begin(), found->second.end());
    }
    auto remove = [&](auto& declarations) { std::erase_if(declarations, [&](const auto& item) { return !kept.contains(item.name.text); }); };
    remove(syntax.aggregates); remove(syntax.enums); remove(syntax.aliases); remove(syntax.traits); remove(syntax.exceptions); remove(syntax.extensions);
    std::vector<std::size_t> relocated(syntax.functions.size());
    std::vector<Function> functions;
    for (std::size_t index = 0; index < syntax.functions.size(); ++index) {
        auto& fn = syntax.functions[index];
        if (fn.file < 2 || kept.contains(declarationGroup(fn))) {
            relocated[index] = functions.size();
            functions.push_back(std::move(fn));
        }
    }
    syntax.functions = std::move(functions);
    for (auto& item : syntax.aggregates) for (auto& field : item.fields) if (field.defaultValue)
        field.defaultValue->function = relocated[field.defaultValue->function];
    for (auto& extension : syntax.extensions) for (auto& property : extension.properties) property.getter = relocated[property.getter];
    for (auto& groups : syntax.importedExtensions) std::erase_if(groups, [&](const auto& name) { return !kept.contains(name); });
    syntax.expansions.clear();
}

struct MacroDependencies {
    const std::vector<Source>& sources;
    const std::vector<Module>& files;
    const std::map<std::string, Space>& spaces;
    const std::map<std::string, const ResolvedPackage*>& packages;
    const std::map<std::string, std::string>& prefixes;
    const SourceOverlay* overlays;

    std::string link(const MacroOrigin& origin, Module& syntax, std::vector<Source>& code) const {
        auto package = packages.at(origin.package);
        std::map<std::string, Space> scopes{{"$macroPrelude", {package}}, {"$macroImplementation", {package}}};
        auto names = prefixes;
        std::map<std::string, ResolvedPackage> bundled;
        std::set<std::string> read;
        auto append = [&](const Source& source, const Module& parsed) {
            code.push_back(source);
            appendCachedSyntax(syntax, parsed, code.size() - 1);
        };
        for (std::size_t index = 0; index < syntax.imports.size(); ++index) {
            const auto use = syntax.imports[index];
            if (use.kind != ImportKind::Runtime) continue;
            const auto& source = code[use.file];
            const auto& from = *scopes.at(source.moduleId).package;
            if (use.path.size() >= 3 && use.path.front().text == "std") {
                auto name = use.path[1].text;
                auto key = "$std/" + name;
                if (!names.contains(key)) {
                    auto root = sdkRoot() / "share/lyra/std" / name;
                    if (fs::is_directory(root / "src")) {
                        auto& stdPackage = bundled.emplace(key, ResolvedPackage{key, name, "builtin", root, {}, true}).first->second;
                        names.emplace(key, key);
                        scopes.emplace(key, Space{&stdPackage});
                        read.insert(key);
                        for (auto input : loadSources(root)) {
                            input.moduleId = key;
                            if (overlays && overlays->contains(input.path)) input = loadSource(input.path, key, overlays);
                            Module parsed;
                            std::vector<ExpressionExpansion> calls;
                            parseFile(parsed, input, 0, &calls);
                            append(input, parsed);
                        }
                    }
                }
            }
            std::optional<ImportTarget> target;
            try { target = importTarget(code[use.file], from, use, names); }
            catch (const Diagnostic&) { if (use.file == 1) throw; }
            if (!target || !read.insert(target->module).second) continue;
            auto space = spaces.find(target->module);
            if (space == spaces.end()) continue;
            scopes.emplace(target->module, Space{space->second.package});
            for (std::size_t file = 0; file < sources.size(); ++file)
                if (sources[file].moduleId == target->module) append(sources[file], files[file]);
        }
        llvm::SHA256 hash;
        auto text = [&](std::string_view value) { hash.update(std::to_string(value.size()) + ':'); hash.update(value); };
        for (const auto& [name, space] : scopes) {
            text(name); text(space.package->id);
            for (const auto& [alias, target] : space.package->dependencies) { text(alias); text(target); }
        }
        for (std::size_t file = 2; file < code.size(); ++file) {
            text(code[file].moduleId); text(pathText(code[file].path)); text(code[file].text);
        }
        std::vector<NameReference> references;
        SymbolDependencies dependencies;
        Linker{code, scopes, names, "$macroImplementation", references, false, true}.rewrite(syntax, &dependencies);
        keepMacroDependencies(syntax, dependencies);
        auto digest = hash.final();
        return {reinterpret_cast<const char*>(digest.data()), digest.size()};
    }
};

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
    Module authored;
    std::vector<Module> fileSyntax;
    std::vector<bool> expressionMacros;
    auto parseAuthored = [&](std::size_t file) {
        const auto& source = program.sources[file];
        std::vector<ExpressionExpansion> calls;
        Module syntax;
        if (parseCache && source.text.find_first_of("#@") == std::string::npos) syntax = cachedFileSyntax(source, *parseCache);
        else parseFile(syntax, source, file, &calls);
        appendCachedSyntax(authored, syntax, file);
        fileSyntax.push_back(std::move(syntax));
        expressionMacros.push_back(!calls.empty());
    };
    for (std::size_t file = 0; file < program.sources.size(); ++file) parseAuthored(file);
    std::map<std::string, ResolvedPackage> bundled;
    auto standard = sdkRoot() / "share" / "lyra" / "std";
    std::size_t loadedImports = 0;
    auto loadBundled = [&](Module& syntax, bool handwritten) {
        for (; loadedImports < syntax.imports.size(); ++loadedImports) {
            const auto& use = syntax.imports[loadedImports];
            if (use.kind != ImportKind::Runtime || use.path.size() < 3 || use.path[0].text != "std") continue;
            auto name = use.path[1].text;
            auto key = "$std/" + name;
            if (bundled.contains(key)) continue;
            auto packageRoot = standard / name;
            if (!fs::is_directory(packageRoot / "src")) throw at(program.sources[use.file], use.path[1].span, "LYN0003", "找不到随工具链安装的标准包：" + name);
            auto& package = bundled.emplace(key, ResolvedPackage{key, name, "builtin", packageRoot, {}, true}).first->second;
            packages.emplace(key, &package);
            prefixes.emplace(key, key);
            spaces.emplace(key, Space{&package});
            for (auto source : loadSources(packageRoot)) {
                source.moduleId = key;
                if (overlays) if (auto found = overlays->find(source.path); found != overlays->end())
                    source = sourceText(source.path, found->second, key);
                program.sources.push_back(std::move(source));
                auto file = program.sources.size() - 1;
                if (handwritten) parseAuthored(file);
                else if (parseCache && program.sources.back().text.find_first_of("#@") == std::string::npos)
                    appendCachedSyntax(program.syntax, cachedFileSyntax(program.sources.back(), *parseCache), file);
                else parseFile(program.syntax, program.sources.back(), file);
            }
        }
    };
    loadBundled(authored, true);
    auto hasExpressions = std::ranges::any_of(expressionMacros, [](bool present) { return present; });
    if (!hasExpressions && authored.expansions.empty() && std::ranges::none_of(authored.imports, [](const auto& use) { return use.kind != ImportKind::Runtime; })) {
        program.syntax = std::move(authored);
        Linker{program.sources, spaces, prefixes, root, program.references, overlays != nullptr}.rewrite(program.syntax);
        return program;
    }
    const auto authoredSources = program.sources;
    MacroImports imports{authoredSources, spaces, packages, prefixes, overlays};
    imports.collect(authored);
    MacroDependencies dependencies{authoredSources, fileSyntax, spaces, packages, prefixes, overlays};
    const MacroResolver resolveMacro = [&](const auto& source, const auto& name, auto kind) { return imports.resolve(source, name, kind); };
    const MacroLinker linkMacro = [&](const auto& origin, auto& syntax, auto& sources) { return dependencies.link(origin, syntax, sources); };
    const MacroEnvironment environment{resolveMacro, linkMacro};
    std::map<std::string, MacroPackage> macroPackages;
    auto macroCache = overlays ? fs::path{} : cacheOverride();
    if (!overlays) macroCache = (macroCache.empty() ? graph.workspace / ".lyra" / "cache" : macroCache) / "compiler" / "macros";
    for (const auto& [module, space] : spaces) macroPackages.emplace(module, MacroPackage{space.package->root, space.package->macroInputs, overlays, macroCache});
    for (std::size_t file = 0; file < program.sources.size(); ++file) {
        auto& source = program.sources[file];
        if (expressionMacros[file]) {
            expandExpressions(source, file, macroPackages.at(source.moduleId), program.macroInputs, environment);
            parseFile(program.syntax, source, file);
        } else appendCachedSyntax(program.syntax, fileSyntax[file], file);
    }
    if (!program.syntax.expansions.empty()) {
        auto declarations = spaces;
        Linker linker{program.sources, declarations, prefixes, root, program.references, false};
        linker.collect(program.syntax, false);
        DeclarationTypes types(linker, program.syntax);
        expandMacros(program.syntax, program.sources, macroPackages, program.macroInputs,
            [&](auto file, const auto& annotation, auto parameters) { return types.resolve(file, annotation, parameters); }, types.fingerprint(), environment);
        loadBundled(program.syntax, false);
    }
    Linker{program.sources, spaces, prefixes, root, program.references, overlays != nullptr}.rewrite(program.syntax);
    return program;
}

}
