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
Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<std::filesystem::path>& testFile = {}, const SourceOverlay* overlays = nullptr, ParseCache* parseCache = nullptr, bool editorProbe = false);
struct EditorFileSyntax { std::string text; Module syntax; std::vector<ExpressionExpansion> expressions; };
struct EditorParseCache { std::map<std::filesystem::path, EditorFileSyntax> files; };
struct EditorDeclaration { std::size_t file; std::string name; EditorSymbol symbol; bool visible; };
struct EditorImport { ImportKind kind; std::vector<std::string> path; EditorSymbol symbol; bool automatic{}; std::optional<MacroKind> macro{}; std::string module{}; };
struct EditorNamespace { std::string module; std::vector<EditorSymbol> members; };
struct EditorMacro { MacroKind kind; EditorSymbol symbol; };
struct EditorDefinition { Span use; std::filesystem::path path; Span target; EditorSymbol symbol; };
struct EditorModuleIndex {
    std::vector<EditorSymbol> visible;
    std::vector<EditorMacro> macros;
    std::vector<EditorImport> imports;
    std::vector<EditorDefinition> definitions;
    std::set<std::string> occupied;
    std::map<std::string, EditorNamespace> namespaces;
};
std::vector<EditorDeclaration> editorDeclarations(const Module& syntax, std::span<const Source> sources, bool generatedOnly = false);
EditorModuleIndex indexModule(const PackageGraph& graph, const std::filesystem::path& path, const SourceOverlay& overlays, EditorParseCache& cache);
}

namespace lyra {
namespace fs = std::filesystem;
namespace {

struct LinkedDeclaration { SourceAnchor anchor; bool visible; };
enum class SymbolKind { Type, Callable, Constant, Extension, Module };
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
    std::map<std::string, Import> exports{};
};

struct ImportTarget { std::string package, module; fs::path directory; };
using SymbolDependencies = std::map<std::string, std::set<std::string>>;

std::vector<fs::path> sourcePaths(const fs::path& directory, const SourceOverlay* overlays, bool strict, bool recursive = true) {
    std::set<fs::path> paths;
    auto first = fs::is_directory(directory) ? fs::recursive_directory_iterator(directory) : fs::recursive_directory_iterator{};
    for (auto item = std::move(first); item != fs::recursive_directory_iterator{}; ++item) {
        if (item->is_symlink() && strict) throw Diagnostic{"LYI0001", "源码不允许符号链接：" + pathText(item->path())};
        if (item->is_directory() && (!recursive || pathText(item->path().filename()).starts_with('.'))) { item.disable_recursion_pending(); continue; }
        if (!item->is_symlink() && item->is_regular_file() && item->path().extension() == ".ly") paths.insert(item->path());
    }
    if (overlays) for (const auto& [path, text] : *overlays) {
        auto relative = path.lexically_relative(directory);
        if (relative.empty() || relative == "." || *relative.begin() == ".." || path.extension() != ".ly") continue;
        if (!recursive && relative.has_parent_path()) continue;
        if (std::ranges::any_of(relative, [](const auto& part) { return pathText(part).starts_with('.'); })) continue;
        paths.insert(path);
    }
    return {paths.begin(), paths.end()};
}

std::string directoryModule(std::string prefix, const fs::path& directory, const fs::path& source) {
    auto relative = source.parent_path().lexically_relative(directory);
    if (relative != ".") prefix += '/' + pathText(relative);
    return prefix;
}

const std::string& declarationGroup(const Function& function) {
    return function.owner ? function.owner->text : function.extensionGroup ? function.extensionGroup->text : function.name.text;
}

ImportTarget importTarget(const Source& source, const ResolvedPackage& from, std::span<const Identifier> path,
    const std::map<std::string, std::string>& prefixes) {
    const auto& first = path.front();
    auto package = from.id;
    std::size_t firstDir = 1;
    if (first.text == "std") {
        if (path.size() < 2) throw at(source, first.span, "LYN0003", "请在 std 后指定标准库模块，例如 import std.http");
        package = "$std/" + path[1].text;
        firstDir = 2;
    } else if (first.text != "self") {
        auto found = from.dependencies.find(first.text);
        if (found == from.dependencies.end()) throw at(source, first.span, "LYN0003", "清单没有此依赖简称：" + first.text);
        package = found->second;
    }
    auto prefix = prefixes.find(package);
    if (prefix == prefixes.end()) throw at(source, first.span, "LYN0003", "导入的包不在当前编译阶段：" + package);
    ImportTarget target{std::move(package), prefix->second, {}};
    for (auto index = firstDir; index < path.size(); ++index) {
        target.module += '/' + path[index].text;
        target.directory /= fromUtf8(path[index].text);
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
    std::map<std::size_t, std::map<std::string, Import>> fileImports{};
    std::map<const Import*, Symbol> resolvedImports{};

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

    std::optional<Symbol> exported(const Space& space, const std::string& name, std::set<const Import*>& active) const {
        if (auto hit = space.definitions.find(name); hit != space.definitions.end() && hit->second.visible) {
            auto result = hit->second;
            result.imported = true;
            std::erase_if(result.targets, [](const auto& target) { return !target.visible; });
            return result;
        }
        if (auto use = space.exports.find(name); use != space.exports.end()) {
            auto result = resolve(use->second, active);
            if (indexing && use->second.alias.span != use->second.member->span)
                result.renameTarget = SourceAnchor{use->second.file, use->second.alias.span};
            return result;
        }
        return {};
    }

    Symbol resolve(const Import& use, std::set<const Import*>& active) const {
        const auto& source = sources[use.file];
        if (!active.insert(&use).second) throw at(source, use.alias.span, "LYN0004", "重导出形成循环：" + use.alias.text);
        auto target = importTarget(source, *spaces.at(source.moduleId).package, use.path, prefixes);
        auto found = spaces.find(target.module);
        auto code = macroPhase ? "LYM0004" : "LYN0003";
        if (found == spaces.end()) {
            auto error = at(source, use.path.back().span, code, "找不到导入的模块：" + use.path.back().text);
            auto rootSize = use.path.front().text == "std" ? 2u : 1u;
            if (!use.member && use.path.size() > rootSize) {
                const auto parent = spaces.find(target.module.substr(0, target.module.rfind('/')));
                if (parent != spaces.end() && (parent->second.definitions.contains(use.path.back().text) || parent->second.exports.contains(use.path.back().text))) {
                    auto name = editorSpelling(source, use.path.back());
                    auto alias = use.alias.span == use.path.back().span ? "" : " as " + editorSpelling(source, use.alias);
                    error.message = "此路径指向声明而非模块；请选择 .{" + name + alias + '}';
                }
            }
            throw error;
        }
        if (!use.member) return {std::move(target.module), SymbolKind::Module, false, true, {}, SourceAnchor{use.file, use.alias.span}};
        if (auto symbol = exported(found->second, use.member->text, active)) return std::move(*symbol);
        throw at(source, use.member->span, code, "目标模块没有可用的公开声明：" + use.member->text);
    }

    Symbol resolve(const Import& use) const {
        std::set<const Import*> active;
        return resolve(use, active);
    }

    void validateImport(const Import& use) {
        auto result = [&] {
            try { return resolve(use); }
            catch (const Diagnostic&) {
                if (!macroPhase || use.file < 2) throw;
                return Symbol{"$unexpanded::" + use.alias.text, SymbolKind::Callable, false, true};
            }
        }();
        if (indexing && result.kind == SymbolKind::Module) {
            auto anchor = *result.renameTarget;
            references.push_back({anchor, anchor, anchor});
        } else if (indexing && result.targets.size() == 1) {
            auto target = result.targets.front().anchor;
            references.push_back({{use.file, use.member->span}, target, result.renameTarget});
            if (use.alias.span != use.member->span) {
                result.renameTarget = SourceAnchor{use.file, use.alias.span};
                references.push_back({*result.renameTarget, target, result.renameTarget});
            }
        }
        resolvedImports.emplace(&use, std::move(result));
    }

    void addImport(const Import& use) {
        const auto& source = sources[use.file];
        auto& space = spaces.at(source.moduleId);
        auto& imports = use.isPublic ? space.exports : fileImports[use.file];
        if (space.definitions.contains(use.alias.text) || space.exports.contains(use.alias.text) || !imports.emplace(use.alias.text, use).second)
            throw at(source, use.alias.span, "LYN0002", "导入名称已被使用：" + use.alias.text);
    }

    std::optional<Symbol> lookup(std::size_t file, const std::string& name) const {
        auto binding = [&](const Import& use) {
            if (auto found = resolvedImports.find(&use); found != resolvedImports.end()) return found->second;
            auto symbol = resolve(use);
            if (indexing && use.member && use.alias.span != use.member->span) symbol.renameTarget = SourceAnchor{use.file, use.alias.span};
            return symbol;
        };
        for (auto current = std::optional(file); current; current = sources[*current].importParent) {
            if (auto imports = fileImports.find(*current); imports != fileImports.end())
                if (auto use = imports->second.find(name); use != imports->second.end()) return binding(use->second);
        }
        const auto& space = spaces.at(sources[file].moduleId);
        if (auto found = space.definitions.find(name); found != space.definitions.end()) return found->second;
        if (auto use = space.exports.find(name); use != space.exports.end()) return binding(use->second);
        return {};
    }

    std::optional<Symbol> exported(const std::string& module, const std::string& name) const {
        std::set<const Import*> active;
        return exported(spaces.at(module), name, active);
    }

    TypeForm linkType(std::size_t file, TypeForm form, Span span, std::span<const Identifier> parameters,
        std::set<std::string>* needs = nullptr) const {
        auto parameter = [&](const std::string& name) { return std::ranges::find(parameters, name, &Identifier::text) != parameters.end(); };
        auto qualified = [&](const std::string& alias, const std::string& name) -> std::optional<Symbol> {
            if (parameter(alias)) return {};
            auto scope = lookup(file, alias);
            if (!scope || scope->kind != SymbolKind::Module) return {};
            if (auto symbol = exported(scope->name, name); symbol && symbol->kind == SymbolKind::Type) return symbol;
            throw at(sources[file], span, "LYN0003", "模块没有可用的公开类型：" + alias + '.' + name);
        };
        const bool dynamic = form.name.starts_with("$dyn:");
        auto name = dynamic ? form.name.substr(5) : form.name;
        std::optional<Symbol> target;
        if (name.starts_with("$assoc:") && form.arguments.front().arguments.empty()) {
            target = qualified(form.arguments.front().name, name.substr(7));
            if (target) form.arguments.clear();
        } else if (auto dot = name.find('.'); dot != name.npos) {
            target = qualified(name.substr(0, dot), name.substr(dot + 1));
        } else if (!name.starts_with('$') && (dynamic || !parameter(name))) {
            target = lookup(file, name);
            if (target && target->kind != SymbolKind::Type) target.reset();
        }
        if (target) {
            if (needs) needs->insert(target->name);
            form.name = dynamic ? "$dyn:" + target->name : target->name;
        }
        for (auto& part : form.arguments) part = linkType(file, std::move(part), span, parameters, needs);
        return form;
    }

    void indexTypeNames(std::size_t file, Span span, std::span<const Identifier> parameters) {
        const auto& source = sources[file];
        auto text = std::string_view(source.text).substr(span.begin, span.end - span.begin);
        auto tokens = syntaxTokens(sourceText(source.path, std::string(text), {}), false, true);
        auto spelling = [&](std::size_t index) { const auto token = tokens[index].span; return text.substr(token.begin, token.end - token.begin); };
        auto identifier = [&](std::size_t index) {
            auto word = spelling(index);
            return std::string(word.starts_with('`') ? word.substr(1, word.size() - 2) : word);
        };
        auto anchor = [&](std::size_t index) {
            auto token = tokens[index].span;
            return SourceAnchor{file, {span.begin + token.begin, span.begin + token.end}};
        };
        auto indexSymbol = [&](std::size_t index, const Symbol& symbol) {
            if (symbol.kind == SymbolKind::Type && symbol.targets.size() == 1)
                references.push_back({anchor(index), symbol.targets.front().anchor, symbol.renameTarget});
        };
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (tokens[index].kind != "name" || (index > 0 && spelling(index - 1) == ".") ||
                (index + 1 < tokens.size() && spelling(index + 1) == "=")) continue;
            auto name = identifier(index);
            if (std::ranges::find(parameters, name, &Identifier::text) != parameters.end()) continue;
            auto symbol = lookup(file, name);
            if (!symbol) continue;
            if (symbol->kind == SymbolKind::Module && index + 2 < tokens.size() && spelling(index + 1) == "." && tokens[index + 2].kind == "name") {
                references.push_back({anchor(index), *symbol->renameTarget, symbol->renameTarget});
                if (auto member = exported(symbol->name, identifier(index + 2))) indexSymbol(index + 2, *member);
            } else indexSymbol(index, *symbol);
        }
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
        for (bool shared : {true, false}) for (const auto& use : syntax.imports)
            if (use.kind == ImportKind::Runtime && use.isPublic == shared) addImport(use);
    }

    void rewrite(Module& syntax, SymbolDependencies* dependencies = nullptr) {
        collect(syntax, true);
        if (macroPhase) {
            auto& scope = spaces.at("$macroImplementation");
            for (const auto& [name, symbol] : spaces.at("$macroPrelude").definitions)
                if (lookup(1, name) || !scope.definitions.emplace(name, symbol).second)
                    throw at(sources[1], {0, 0}, "LYM0002", "宏预定义名称不能被重定义：" + name);
        }
        for (const auto& [module, space] : spaces) for (const auto& [name, use] : space.exports) validateImport(use);
        for (const auto& [file, imports] : fileImports) for (const auto& [name, use] : imports) validateImport(use);
        std::set<std::string>* needs{};
        auto group = [&](const auto& name) { if (dependencies) needs = &(*dependencies)[name]; };
        std::map<std::string, std::vector<std::string>> moduleExtensions;
        if (dependencies) for (const auto& extension : syntax.extensions)
            moduleExtensions[sources[extension.file].moduleId].push_back(extension.name.text);
        auto renameTypeIn = [&](std::size_t file, Identifier& type, std::span<const Identifier> parameters = {}) {
            if (indexing && type.span.end > type.span.begin) indexTypeNames(file, type.span, parameters);
            type.text = mapTypeNames(linkType(file, typeForm(type.text), type.span, parameters, needs), [](const auto& name) { return name; });
        };
        syntax.importedExtensions.resize(sources.size());
        if (!syntax.extensions.empty()) for (std::size_t file = 0; file < sources.size(); ++file) {
            std::set<std::string> groups;
            std::set<std::string> names;
            for (const auto& [name, use] : spaces.at(sources[file].moduleId).exports) names.insert(name);
            for (auto current = std::optional(file); current; current = sources[*current].importParent)
                if (auto imports = fileImports.find(*current); imports != fileImports.end())
                    for (const auto& [name, use] : imports->second) names.insert(name);
            for (const auto& name : names) if (auto symbol = lookup(file, name); symbol->kind == SymbolKind::Extension) groups.insert(symbol->name);
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
            auto rename = [&](Identifier& name) {
                const auto& definitions = spaces.at(sources[fn.file].moduleId).definitions;
                if (auto found = definitions.find(name.text); found != definitions.end()) name.text = found->second.name;
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
            auto link = [&](Identifier& name, const Symbol& symbol) {
                if (symbol.kind == SymbolKind::Module) {
                    name.moduleAlias = symbol.renameTarget;
                    return;
                }
                name.linked = symbol.name;
                name.exportedOnly = symbol.imported;
                name.renameTarget = symbol.renameTarget;
                if (needs) needs->insert(symbol.name);
            };
            auto reference = [&](Identifier& name, std::optional<SymbolKind> kind = {}) {
                if (auto found = lookup(fn.file, name.text)) {
                    if (!kind || found->kind == *kind) link(name, *found);
                }
                else if (!kind && name.text == "main") name.linked = sources[fn.file].moduleId + "::main";
            };
            auto qualify = [&](const Identifier& alias, Identifier& name, std::optional<SymbolKind> kind = {}) {
                auto scope = lookup(fn.file, alias.text);
                if (!scope || scope->kind != SymbolKind::Module) return false;
                name.moduleMember = true;
                if (auto found = exported(scope->name, name.text); found && (!kind || found->kind == *kind) && found->kind != SymbolKind::Extension) link(name, *found);
                return true;
            };
            auto qualifyReceiver = [&](ExprId receiver, Identifier& name, std::optional<SymbolKind> kind = {}) {
                if (auto* alias = std::get_if<Identifier>(&fn.expressions[receiver].node)) qualify(*alias, name, kind);
            };
            auto renamePattern = [&](auto&& self, Pattern& pattern) -> void {
                if (auto* name = std::get_if<PatternName>(&pattern.node)) {
                    if (name->module) {
                        reference(*name->module, SymbolKind::Module);
                        qualify(*name->module, *name->owner, SymbolKind::Type);
                    } else if (name->owner && qualify(*name->owner, name->name, name->labels ? SymbolKind::Type : SymbolKind::Constant)) {
                        name->module = std::exchange(name->owner, {});
                        reference(*name->module, SymbolKind::Module);
                    } else if (name->owner) reference(*name->owner, SymbolKind::Type);
                    else if (name->labels) reference(name->name, SymbolKind::Type);
                    else if (!name->escaped) reference(name->name, SymbolKind::Constant);
                }
                for (auto& part : pattern.parts) self(self, part);
            };
            for (auto& expr : fn.expressions) {
                if (auto* name = std::get_if<Identifier>(&expr.node)) reference(*name);
                auto renameQualifier = [&](TypeQualifier& owner) {
                    if (owner.moduleReceiver) qualifyReceiver(*owner.moduleReceiver, owner.name, SymbolKind::Type);
                    else reference(owner.name);
                    for (auto& type : owner.arguments) renameType(type);
                };
                if (auto* member = std::get_if<Member>(&expr.node)) qualifyReceiver(member->receiver, member->name);
                if (auto* member = std::get_if<QualifiedMember>(&expr.node)) renameQualifier(member->owner);
                if (auto* call = std::get_if<Call>(&expr.node)) {
                    if (call->typeOwner) renameQualifier(*call->typeOwner);
                    else if (call->receiver) qualifyReceiver(*call->receiver, call->name);
                    else if (!call->function) reference(call->name);
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
        auto import = [&](const std::string& name, const Import& use) {
            text("import"); text(name);
            text(std::to_string(use.path.size()));
            for (const auto& part : use.path) text(part.text);
            text(use.member ? use.member->text : "");
        };
        for (const auto& [scope, space] : linker.spaces) {
            text("module"); text(scope); text(space.package->id);
            for (const auto& [alias, dependency] : space.package->dependencies) {
                text("dependency"); text(alias); text(dependency);
            }
            for (const auto& [name, symbol] : space.definitions) if (symbol.kind == SymbolKind::Type) {
                text("type"); text(name); text(symbol.visible ? "public" : "private");
            }
            for (const auto& [name, use] : space.exports) import(name, use);
        }
        for (const auto& [file, imports] : linker.fileImports) {
            text("file"); text(pathText(linker.sources[file].path));
            for (const auto& [name, use] : imports) import(name, use);
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
            if (auto separator = lookup.rfind("::"); separator != lookup.npos) {
                if (!dynamic) if (auto alias = aliases.find(lookup); alias != aliases.end()) {
                    const auto& target = alias->second;
                    if (form.arguments.size() != target.typeParameters.size()) throw at(source, span, "LYT0003", "类型别名的类型实参数量不符：" + lookup);
                    Bindings mapped;
                    for (std::size_t slot = 0; slot < form.arguments.size(); ++slot)
                        mapped.emplace(target.typeParameters[slot].text, self(self, form.arguments[slot], from, span, generics, depth + 1));
                    if (std::ranges::find(active, lookup) != active.end()) throw at(source, span, "LYT0003", "透明类型别名形成循环：" + lookup);
                    active.push_back(lookup);
                    auto linked = linker.linkType(target.file, typeForm(target.target.text), target.target.span, target.typeParameters);
                    auto result = self(self, linked, target.file, target.target.span, mapped, depth + 1);
                    active.pop_back();
                    return result;
                }
                auto shape = shapes.find(lookup);
                if (shape == shapes.end() || shape->second.trait != dynamic || shape->second.arity != form.arguments.size())
                    throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + lookup);
                if (dynamic) for (const auto& binding : form.arguments) if (!shape->second.members.contains(binding.name.substr(6)))
                    throw at(source, span, "LYG0007", "动态接口没有关联类型：" + binding.name.substr(6));
                auto module = lookup.substr(0, separator);
                auto standard = module.starts_with("$std/");
                if (standard) module = "std." + module.substr(5);
                else {
                    auto path = module.find('/');
                    auto prefix = linker.spaces.at(module).package->name;
                    module = path == std::string::npos ? std::move(prefix) : prefix + module.substr(path);
                    std::ranges::replace(module, '/', '.');
                }
                return make(dynamic ? "dyn" : standard ? "standard" : "named", lookup.substr(separator + 2), std::move(module));
            }
            if (!dynamic) {
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
        auto linked = linker.linkType(file, typeForm(annotation.text), annotation.span, parameters);
        return visit(visit, linked, file, annotation.span, generics, 0);
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
        auto target = importTarget(source, *spaces.at(source.moduleId).package, use.path, prefixes);
        const auto& name = *use.member;
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
            if (!found) throw at(sources[use->file], use->member->span, "LYM0001", "找不到以 pub fn 导出的宏或装饰器：" + use->member->text);
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
            if (use.path.size() >= 2 && use.path.front().text == "std") {
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
            try { target = importTarget(code[use.file], from, use.path, names); }
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

std::vector<EditorDeclaration> editorDeclarations(const Module& syntax, std::span<const Source> sources, bool generatedOnly) {
    std::vector<EditorDeclaration> result;
    auto add = [&](const auto& item, EditorKind kind, std::string detail = {}, std::string documentation = {}) {
        const auto& source = sources[item.file];
        if (generatedOnly && !source.origin) return;
        auto separator = item.name.text.rfind("::");
        auto name = item.name.text.substr(separator == std::string::npos ? 0 : separator + 2);
        if (name == "_" || name.contains('$') || item.name.span.begin == item.name.span.end) return;
        auto label = editorSpelling(source, item.name);
        if (source.origin && label.starts_with('`')) {
            constexpr std::array<std::string_view, 8> contextual{"None", "Self", "blocking", "parallel", "quote", "select", "taskGroup", "unsafe"};
            if (std::ranges::find(contextual, name) == contextual.end()) label = editorIdentifier(name);
        }
        if (documentation.empty()) documentation = editorDocumentation(source, item.name.span.begin);
        result.push_back({item.file, std::move(name), {std::move(label), kind, item.name.span, std::move(detail), std::move(documentation)}, item.isPublic});
    };
    for (const auto& record : syntax.aggregates) add(record, record.reference ? EditorKind::Class : EditorKind::Struct);
    for (const auto& item : syntax.enums) add(item, EditorKind::Enum);
    for (const auto& trait : syntax.traits) add(trait, EditorKind::Trait);
    for (const auto& alias : syntax.aliases) add(alias, EditorKind::TypeAlias, "type " + editorSpelling(sources[alias.file], alias.name) + " = " + editorSpelling(sources[alias.file], alias.target));
    for (const auto& exception : syntax.exceptions) add(exception, EditorKind::Class, "exception " + editorSpelling(sources[exception.file], exception.name));
    for (const auto& group : syntax.extensions) add(group, EditorKind::Extension, "extension " + editorSpelling(sources[group.file], group.name) + " for " + editorSpelling(sources[group.file], group.receiver));
    for (const auto& function : syntax.functions) {
        if (function.owner || function.extensionGroup || function.conversion || (generatedOnly && !sources[function.file].origin)) continue;
        const auto& source = sources[function.file];
        if (function.constant == ConstKind::Value)
            add(function, EditorKind::Constant, "const " + editorSpelling(source, function.name) + ": " + editorSpelling(source, function.result));
        else if (!function.name.text.contains('$')) {
            auto signature = describeFunction(source, function, editorSpelling(source, function.name));
            add(function, EditorKind::Function, std::move(signature.label), std::move(signature.documentation));
        }
    }
    return result;
}

namespace {

const EditorFileSyntax& indexedFile(const Source& source, EditorParseCache& cache) {
    auto entry = cache.files.find(source.path);
    if (entry != cache.files.end() && entry->second.text == source.text) return entry->second;
    EditorFileSyntax file{source.text, {}, {}};
    file.syntax = indexFileSyntax(source, &file.expressions);
    return cache.files.insert_or_assign(source.path, std::move(file)).first->second;
}

struct ModuleCatalog {
    ModuleCatalog(const PackageGraph& graph, const fs::path& path, const SourceOverlay& overlays, EditorParseCache& cache, const ResolvedPackage& entry)
        : graph(graph), path(path), overlays(overlays), cache(cache), entry(entry) {}

    const PackageGraph& graph;
    const fs::path& path;
    const SourceOverlay& overlays;
    EditorParseCache& cache;
    const ResolvedPackage& entry;
    fs::path standard = sdkRoot() / "share" / "lyra" / "std";
    EditorModuleIndex index{};
    std::map<std::string, ResolvedPackage> bundled;
    std::map<std::string, std::string> prefixes;
    std::map<std::string, const ResolvedPackage*> packages;
    std::map<std::string, Space> spaces;
    std::map<std::string, fs::path> directories;
    std::vector<Source> sources;
    std::vector<NameReference> unusedReferences;
    std::map<std::pair<std::size_t, std::size_t>, EditorSymbol> symbols;
    std::multimap<std::string, std::vector<std::string>> routes;
    std::set<std::pair<ImportKind, std::vector<std::string>>> suggestedModules;
    std::vector<Import> imports;
    std::optional<std::size_t> current;
    Linker linker{sources, spaces, prefixes, "", unusedReferences, true};
    MacroImports macroImports{sources, spaces, packages, prefixes, &overlays};
    std::map<std::tuple<std::string, MacroKind, std::string>, EditorDefinition> authoredMacros;
    std::map<std::pair<MacroKind, std::string>, EditorDefinition> builtinMacros;

    void moduleCandidate(ImportKind kind, std::vector<std::string> route) {
        auto label = editorIdentifier(route.back());
        if (!label.empty() && suggestedModules.emplace(kind, route).second)
            index.imports.push_back({kind, std::move(route), {std::move(label), EditorKind::Module}});
    }

    void addPackage(const ResolvedPackage& package, std::string prefix) {
        packages.emplace(package.id, &package);
        prefixes.emplace(package.id, std::move(prefix));
    }

    std::string addModule(const ResolvedPackage& package, fs::path relative) {
        auto module = prefixes.at(package.id);
        if (relative == ".") relative.clear();
        if (!relative.empty()) module += '/' + pathText(relative);
        spaces.try_emplace(module, Space{&package});
        directories.try_emplace(module, std::move(relative));
        return module;
    }

    void loadFile(const fs::path& filePath, std::string module) {
        try { sources.push_back(loadSource(filePath, std::move(module), &overlays)); }
        catch (const Diagnostic&) { return; }
        auto file = sources.size() - 1;
        if (filePath == path) current = file;
        const auto& source = sources.back();
        const auto& parsed = indexedFile(source, cache);
        for (auto use : parsed.syntax.imports) { use.file = file; imports.push_back(std::move(use)); }
        for (auto& declaration : editorDeclarations(parsed.syntax, std::span(&source, 1))) {
            auto kind = declaration.symbol.kind;
            auto spaceKind = kind == EditorKind::Function ? SymbolKind::Callable : kind == EditorKind::Constant ? SymbolKind::Constant
                : kind == EditorKind::Extension ? SymbolKind::Extension : SymbolKind::Type;
            Identifier name{declaration.name, declaration.symbol.name};
            try { linker.declare(file, name, declaration.visible, false, spaceKind); }
            catch (const Diagnostic&) { continue; }
            symbols.emplace(std::pair{file, name.span.begin}, std::move(declaration.symbol));
        }
    }

    void loadPackages() {
        for (const auto& package : graph.packages) addPackage(package, "pkg" + std::to_string(prefixes.size()));
        routes.emplace(entry.id, std::vector<std::string>{"self"});
        for (const auto& [alias, id] : entry.dependencies) routes.emplace(id, std::vector<std::string>{alias});
        for (const auto& [id, route] : routes)
            for (auto kind : {ImportKind::Runtime, ImportKind::Macro, ImportKind::Decorator}) moduleCandidate(kind, route);
        moduleCandidate(ImportKind::Runtime, {"std"});
        std::vector<std::string> pending{entry.id};
        if (fs::is_directory(standard)) for (const auto& item : fs::directory_iterator(standard)) {
            if (!fs::is_directory(item.path() / "src")) continue;
            auto name = pathText(item.path().filename());
            if (editorIdentifier(name).empty()) continue;
            auto id = "$std/" + name;
            auto& package = bundled.emplace(id, ResolvedPackage{id, name, "builtin", item.path(), {}, true}).first->second;
            addPackage(package, id);
            routes.emplace(id, std::vector<std::string>{"std", name});
            pending.push_back(id);
        }
        std::set<std::string> reachable;
        for (std::size_t next = 0; next < pending.size(); ++next) {
            auto id = pending[next];
            if (!reachable.insert(id).second || !packages.contains(id)) continue;
            const auto& package = *packages.at(id);
            for (const auto& [alias, target] : package.dependencies) pending.push_back(target);
            auto directory = package.root / "src";
            auto paths = sourcePaths(directory, &overlays, false);
            addModule(package, {});
            if (id == entry.id && std::ranges::find(paths, path) == paths.end()) paths.push_back(path);
            for (const auto& filePath : paths) {
                auto relative = filePath.parent_path().lexically_relative(directory);
                if (!relative.empty() && *relative.begin() == "..") relative.clear();
                loadFile(filePath, addModule(package, relative));
            }
            if (id.starts_with("$std/")) continue;
            if (fs::is_directory(directory)) for (auto item = fs::recursive_directory_iterator(directory); item != fs::recursive_directory_iterator{}; ++item) {
                if (!item->is_directory()) continue;
                auto name = pathText(item->path().filename());
                if (item->is_symlink() || name.starts_with('.')) {
                    item.disable_recursion_pending();
                    if (!item->is_symlink() && (name == ".macros" || name == ".expressions" || name == ".decorators"))
                        addModule(package, item->path().parent_path().lexically_relative(directory));
                }
            }
            for (const auto& [openPath, text] : overlays) {
                auto folder = openPath.parent_path();
                if (folder.filename() != ".macros" && folder.filename() != ".expressions" && folder.filename() != ".decorators") continue;
                auto relative = folder.parent_path().lexically_relative(directory);
                if (!relative.empty() && *relative.begin() != "..") addModule(package, relative);
            }
        }
    }

    void runtimeSymbols();
    void macroSymbols();
};

void ModuleCatalog::runtimeSymbols() {
    const auto& source = sources[*current];
    for (bool shared : {true, false}) for (const auto& use : imports) {
        if (use.isPublic != shared) continue;
        const auto& module = sources[use.file].moduleId;
        if (use.kind == ImportKind::Runtime) {
            if ((use.isPublic && module == source.moduleId) || use.file == *current) index.occupied.insert(use.alias.text);
            try { linker.addImport(use); }
            catch (const Diagnostic&) { continue; }
        } else macroImports.aliases.try_emplace(std::tuple{module, use.kind, use.alias.text}, &use);
    }
    auto describe = [&](const Symbol& symbol, const std::string& alias) {
        auto anchor = symbol.targets.front().anchor;
        auto description = symbols.at({anchor.file, anchor.span.begin});
        description.label = editorIdentifier(alias);
        return description;
    };
    auto listExports = [&](const std::string& module, const auto& emit) {
        const auto& space = spaces.at(module);
        auto selected = [&](const std::string& name) {
            try { if (auto symbol = linker.exported(module, name)) emit(name, *symbol); }
            catch (const Diagnostic&) { return; }
        };
        for (const auto& [name, symbol] : space.definitions) if (symbol.visible) selected(name);
        for (const auto& [name, use] : space.exports) selected(name);
    };
    const auto& own = spaces.at(source.moduleId);
    for (const auto& [name, symbol] : own.definitions) {
        index.occupied.insert(name);
        index.visible.push_back(describe(symbol, name));
    }
    auto imported = [&](const Import& use) {
        try {
            auto symbol = linker.resolve(use);
            if (symbol.kind == SymbolKind::Module) {
                auto& nameSpace = index.namespaces.emplace(use.alias.text, EditorNamespace{symbol.name, {}}).first->second;
                listExports(symbol.name, [&](const auto& name, const auto& item) { nameSpace.members.push_back(describe(item, name)); });
                auto pathSpan = Span{use.path.front().span.begin, use.path.back().span.end};
                index.visible.push_back({editorModuleAlias(use.alias.text), EditorKind::Module, use.alias.span,
                    "import " + editorSpelling(source, Identifier{"", pathSpan})});
                return;
            }
            index.visible.push_back(describe(symbol, use.alias.text));
            if (use.file != *current) return;
            for (const auto& target : symbol.targets) {
                auto anchor = target.anchor;
                auto description = symbols.at({anchor.file, anchor.span.begin});
                index.definitions.push_back({use.member->span, sources[anchor.file].path, anchor.span, description});
                if (use.alias.span != use.member->span) index.definitions.push_back({use.alias.span, sources[anchor.file].path, anchor.span, description});
            }
        } catch (const Diagnostic&) { return; }
    };
    for (const auto& [name, use] : own.exports) imported(use);
    if (auto file = linker.fileImports.find(*current); file != linker.fileImports.end()) {
        for (const auto& [name, use] : file->second) imported(use);
    }
    for (const auto& [module, space] : spaces) {
        auto [first, last] = routes.equal_range(space.package->id);
        if (first == last) continue;
        std::vector<std::pair<std::string, EditorSymbol>> exports;
        listExports(module, [&](const auto& name, const auto& symbol) { exports.emplace_back(name, describe(symbol, name)); });
        for (auto base = first; base != last; ++base) {
            auto route = base->second;
            bool standardLibrary = route.front() == "std";
            if (standardLibrary) moduleCandidate(ImportKind::Runtime, route);
            for (const auto& component : directories.at(module)) {
                route.push_back(pathText(component));
                if (!editorIdentifier(route.back()).empty()) {
                    moduleCandidate(ImportKind::Runtime, route);
                    if (!standardLibrary) for (auto kind : {ImportKind::Macro, ImportKind::Decorator}) moduleCandidate(kind, route);
                }
            }
            if (std::ranges::any_of(route, [](const auto& word) { return editorIdentifier(word).empty(); })) continue;
            for (const auto& [name, symbol] : exports) {
                auto qualified = route;
                qualified.push_back(name);
                index.imports.push_back({ImportKind::Runtime, std::move(qualified), symbol, false, {}, module});
            }
        }
    }
}

void ModuleCatalog::macroSymbols() {
    const auto& source = sources[*current];
    auto read = [&](const fs::path& file, MacroKind kind) -> std::optional<std::pair<EditorDefinition, bool>> {
        auto text = overlays.find(file);
        if ((text != overlays.end() ? text->second.size() : fs::file_size(file)) > macroInputByteLimit) return {};
        try {
            auto implementation = loadSource(file, {}, &overlays);
            const auto& syntax = indexedFile(implementation, cache).syntax;
            auto name = pathText(file.stem());
            auto declaration = std::ranges::find_if(syntax.functions, [&](const auto& fn) {
                return fn.name.text == name && !fn.owner && !fn.extensionGroup;
            });
            if (declaration == syntax.functions.end()) return {};
            auto spelling = editorIdentifier(name);
            auto sigil = kind == MacroKind::Decorator ? '@' : '#';
            auto description = EditorSymbol{spelling, kind == MacroKind::Decorator ? EditorKind::Decorator : EditorKind::Macro,
                declaration->name.span, sigil + spelling, editorDocumentation(implementation, declaration->name.span.begin)};
            return std::pair{EditorDefinition{{}, file, declaration->name.span, std::move(description)}, declaration->isPublic};
        } catch (const Diagnostic&) { return {}; }
    };
    auto scan = [&](const fs::path& folder, const auto& accept) {
        auto paths = sourcePaths(folder, &overlays, false, false);
        for (const auto& file : paths) if (!editorIdentifier(pathText(file.stem())).empty()) accept(file);
    };
    for (auto kind : {MacroKind::Declaration, MacroKind::Expression, MacroKind::Decorator}) {
        auto directory = std::string(macroDirectory(kind));
        scan(standard / "meta" / directory, [&](const auto& file) {
            if (auto definition = read(file, kind)) builtinMacros.emplace(std::pair{kind, pathText(file.stem())}, std::move(definition->first));
        });
        for (const auto& [module, relative] : directories) {
            const auto& package = *spaces.at(module).package;
            if (package.id.starts_with("$std/")) continue;
            scan(package.root / "src" / relative / ('.' + directory), [&](const auto& file) {
                auto pathInPackage = fs::weakly_canonical(file).lexically_relative(package.root);
                if (pathInPackage.empty() || *pathInPackage.begin() == "..") return;
                if (auto definition = read(file, kind)) {
                    macroImports.exports[file] = definition->second;
                    authoredMacros.emplace(std::tuple{module, kind, pathText(file.stem())}, std::move(definition->first));
                }
            });
        }
    }
    auto resolveImport = [&](const Import& use, MacroKind kind) -> std::optional<EditorDefinition> {
        std::set<const Import*> active;
        try {
            if (auto origin = macroImports.resolve(use, kind, active)) if (auto definition = read(origin->path, kind)) return std::move(definition->first);
        } catch (const Diagnostic&) {}
        return {};
    };
    auto family = MacroImports::family;
    std::map<std::pair<MacroKind, std::string>, EditorDefinition> visible;
    auto rootModule = prefixes.at(spaces.at(source.moduleId).package->id);
    for (const auto& [key, definition] : builtinMacros) visible.emplace(key, definition);
    for (const auto& module : {rootModule, source.moduleId}) for (const auto& [key, definition] : authoredMacros) {
        const auto& [owner, kind, name] = key;
        if (owner == module) visible.insert_or_assign({kind, name}, definition);
    }
    for (const auto& [key, use] : macroImports.aliases) {
        const auto& [module, kind, alias] = key;
        for (auto macro : {MacroKind::Declaration, MacroKind::Expression, MacroKind::Decorator}) {
            if (family(macro) != kind) continue;
            auto resolved = resolveImport(*use, macro);
            if (module == source.moduleId) {
                visible.erase({macro, alias});
                if (resolved) visible.emplace(std::pair{macro, alias}, *resolved);
            }
            if (resolved && use->file == *current) {
                resolved->use = use->member->span;
                index.definitions.push_back(*resolved);
                if (use->alias.span != resolved->use) { resolved->use = use->alias.span; index.definitions.push_back(*resolved); }
            }
        }
    }
    for (const auto& [key, definition] : visible) {
        auto symbol = definition.symbol;
        symbol.label = editorIdentifier(key.second);
        index.macros.push_back({key.first, std::move(symbol)});
    }
    const auto& parsed = indexedFile(source, cache);
    auto link = [&](const Identifier& name, MacroKind kind) {
        if (auto target = visible.find({kind, name.text}); target != visible.end()) {
            auto definition = target->second;
            definition.use = name.span;
            index.definitions.push_back(std::move(definition));
        }
    };
    for (const auto& expansion : parsed.syntax.expansions) link(expansion.name, expansion.decorator ? MacroKind::Decorator : MacroKind::Declaration);
    for (const auto& expression : parsed.expressions) link(expression.name, MacroKind::Expression);
    auto exported = [&](const std::string& module, MacroKind kind, const std::string& name, const EditorDefinition& definition) {
        const auto& space = spaces.at(module);
        auto [first, last] = routes.equal_range(space.package->id);
        for (auto base = first; base != last; ++base) {
            if (base->second.front() == "std") continue;
            auto route = base->second;
            for (const auto& component : directories.at(module)) route.push_back(pathText(component));
            route.push_back(name);
            if (std::ranges::any_of(route, [](const auto& word) { return editorIdentifier(word).empty(); })) continue;
            auto description = definition.symbol;
            description.label = editorIdentifier(name);
            bool automatic = !visible.contains({kind, name}) && !macroImports.aliases.contains({source.moduleId, family(kind), name}) &&
                std::ranges::none_of(visible, [&](const auto& item) { return item.first.first == kind && item.second.path == definition.path; });
            index.imports.push_back({family(kind), std::move(route), std::move(description), automatic, kind});
        }
    };
    for (const auto& [key, definition] : authoredMacros) {
        const auto& [module, kind, name] = key;
        if (!macroImports.exports.at(definition.path) || macroImports.aliases.contains({module, family(kind), name})) continue;
        exported(module, kind, name, definition);
    }
    for (const auto& [key, use] : macroImports.aliases) if (use->isPublic) {
        const auto& [module, kind, alias] = key;
        for (auto macro : {MacroKind::Declaration, MacroKind::Expression, MacroKind::Decorator}) if (family(macro) == kind)
            if (auto definition = resolveImport(*use, macro)) exported(module, macro, alias, *definition);
    }
}

}

EditorModuleIndex indexModule(const PackageGraph& graph, const fs::path& path, const SourceOverlay& overlays, EditorParseCache& cache) {
    if (cache.files.size() >= 4096) cache.files.clear();
    auto entry = std::ranges::find(graph.packages, graph.entry, &ResolvedPackage::id);
    if (entry == graph.packages.end()) return {};
    ModuleCatalog catalog{graph, path, overlays, cache, *entry};
    catalog.loadPackages();
    if (catalog.current) { catalog.runtimeSymbols(); catalog.macroSymbols(); }
    return std::move(catalog.index);
}

Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<fs::path>& testFile, const SourceOverlay* overlays, ParseCache* parseCache, bool editorProbe) {
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
        spaces.try_emplace(prefixes.at(id), Space{package});
        for (const auto& path : sourcePaths(directory, overlays, true)) {
            auto module = directoryModule(prefixes.at(id), directory, path);
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
        if (editorProbe) syntax = indexFileSyntax(source, &calls);
        else if (parseCache && source.text.find_first_of("#@") == std::string::npos) syntax = cachedFileSyntax(source, *parseCache);
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
            if (use.kind != ImportKind::Runtime || use.path.size() < 2 || use.path[0].text != "std") continue;
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
            if (editorProbe) appendCachedSyntax(program.syntax, indexFileSyntax(source), file);
            else parseFile(program.syntax, source, file);
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
