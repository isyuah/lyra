module;
#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module lyra.modules;
import lyra.generics;
import lyra.packages;
import lyra.macros;
import lyra.platform;
import lyra.syntax;

export namespace lyra {
struct Compilation { std::vector<Source> sources; Module syntax; std::vector<std::filesystem::path> macroInputs{}; };
Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<std::filesystem::path>& testFile = {});
}

namespace lyra {
namespace fs = std::filesystem;
namespace {

struct Symbol { std::string name; bool visible; bool callable; bool imported{}; };
struct Space {
    const ResolvedPackage* package;
    std::map<std::string, Symbol> definitions{};
    std::map<std::string, const Import*> imports{};
};

struct Linker {
    const std::vector<Source>& sources;
    std::map<std::string, Space>& spaces;
    const std::map<std::string, std::string>& prefixes;
    std::string entry;

    void declare(std::size_t file, Identifier& id, bool visible, bool callable) {
        auto scope = sources[file].moduleId;
        auto& defs = spaces.at(scope).definitions;
        auto name = scope == entry && callable && id.text == "main" ? id.text : scope + "::" + id.text;
        auto [symbol, inserted] = defs.try_emplace(id.text, Symbol{name, visible, callable});
        if (!inserted) {
            if (!callable || !symbol->second.callable || name == "main") throw at(sources[file], id.span, "LYN0002", "名称已声明：" + id.text);
            symbol->second.visible |= visible;
        }
        id.text = std::move(name);
    }

    Symbol resolve(const Import& use, std::set<const Import*>& active) {
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
        if (auto hit = space.definitions.find(name.text); hit != space.definitions.end() && hit->second.visible) return {hit->second.name, use.isPublic, hit->second.callable, true};
        if (auto alias = space.imports.find(name.text); alias != space.imports.end() && alias->second->isPublic) {
            auto symbol = resolve(*alias->second, active);
            return {std::move(symbol.name), use.isPublic, symbol.callable, true};
        }
        throw at(source, name.span, "LYN0003", "目标模块没有公开声明：" + name.text);
    }

    void rewrite(Module& syntax) {
        for (auto& trait : syntax.traits) declare(trait.file, trait.name, trait.isPublic, false);
        for (auto& record : syntax.aggregates) declare(record.file, record.name, record.isPublic, false);
        for (auto& item : syntax.enums) declare(item.file, item.name, item.isPublic, false);
        for (auto& fn : syntax.functions) if (!fn.owner) declare(fn.file, fn.name, fn.isPublic, true);
        std::map<std::size_t, std::map<std::string, const Import*>> generatedImports;
        for (const auto& use : syntax.imports) {
            const auto& source = sources[use.file];
            if (source.origin) {
                if (!generatedImports[use.file].emplace(use.alias.text, &use).second) throw at(source, use.alias.span, "LYN0002", "生成代码中重复的导入名称：" + use.alias.text);
                continue;
            }
            auto& space = spaces.at(source.moduleId);
            if (space.definitions.contains(use.alias.text) || !space.imports.emplace(use.alias.text, &use).second) {
                throw at(source, use.alias.span, "LYN0002", "导入名称已被使用：" + use.alias.text);
            }
        }
        for (auto& [moduleId, space] : spaces) {
            for (const auto& [name, use] : space.imports) {
                std::set<const Import*> active;
                space.definitions.emplace(name, resolve(*use, active));
            }
        }
        std::map<std::size_t, std::map<std::string, Symbol>> generatedScopes;
        for (const auto& [file, imports] : generatedImports) {
            auto& scope = generatedScopes[file] = spaces.at(sources[file].moduleId).definitions;
            for (const auto& [name, use] : imports) { std::set<const Import*> active; scope[name] = resolve(*use, active); }
        }
        auto scopeOf = [&](std::size_t file) -> const auto& {
            auto generated = generatedScopes.find(file);
            return generated == generatedScopes.end() ? spaces.at(sources[file].moduleId).definitions : generated->second;
        };
        auto renameTypeIn = [&](std::size_t file, Identifier& type, std::span<const Identifier> parameters = {}) {
            const auto& scope = scopeOf(file);
            type.text = mapTypeNames(typeForm(type.text), [&](const std::string& name) {
                if (std::ranges::any_of(parameters, [&](const auto& parameter) { return parameter.text == name; })) return name;
                auto found = scope.find(name); return found == scope.end() ? name : found->second.name;
            });
        };
        for (auto& record : syntax.aggregates) for (auto& field : record.fields) renameTypeIn(record.file, field.type, record.typeParameters);
        for (auto& item : syntax.enums) for (auto& entry : item.cases) for (auto& field : entry.fields) renameTypeIn(item.file, field.type);
        for (auto& trait : syntax.traits) for (auto& method : trait.methods) {
            renameTypeIn(trait.file, method.result);
            for (auto& parameter : method.parameters) renameTypeIn(trait.file, parameter.type);
        }
        for (auto& fn : syntax.functions) {
            const auto& scope = scopeOf(fn.file);
            auto rename = [&](Identifier& name) {
                if (auto found = scope.find(name.text); found != scope.end()) name.text = found->second.name;
            };
            auto renameType = [&](Identifier& name) {
                name.text = mapTypeNames(typeForm(name.text), [&](const std::string& part) {
                    if (genericParameter(fn, part)) return part;
                    auto found = scope.find(part); return found == scope.end() ? part : found->second.name;
                });
            };
            if (fn.owner) rename(*fn.owner);
            for (auto& bound : fn.bounds) rename(bound.trait);
            renameType(fn.result);
            for (auto& param : fn.parameters) renameType(param.type);
            auto reference = [&](Identifier& name) {
                if (auto found = scope.find(name.text); found != scope.end()) {
                    name.linked = found->second.name;
                    name.exportedOnly = found->second.imported;
                }
                else if (name.text == "main") name.linked = sources[fn.file].moduleId + "::main";
            };
            auto renamePattern = [&](auto&& self, Pattern& pattern) -> void {
                if (auto* name = std::get_if<PatternName>(&pattern.node)) {
                    if (name->owner) reference(*name->owner);
                    else if (name->labels) reference(name->name);
                }
                for (auto& part : pattern.parts) self(self, part);
            };
            for (auto& expr : fn.expressions) {
                if (auto* name = std::get_if<Identifier>(&expr.node)) reference(*name);
                if (auto* call = std::get_if<Call>(&expr.node)) {
                    if (call->typeOwner) {
                        reference(call->typeOwner->name);
                        for (auto& type : call->typeOwner->arguments) renameType(type);
                    } else if (!call->receiver && !call->function) reference(call->name);
                    for (auto& type : call->typeArguments) renameType(type);
                }
                if (auto* binding = std::get_if<Binding>(&expr.node); binding && binding->annotation) renameType(*binding->annotation);
                if (auto* attempt = std::get_if<Attempt>(&expr.node); attempt && attempt->binding) renameType(attempt->binding->type);
                if (auto* match = std::get_if<Match>(&expr.node)) for (auto& arm : match->arms) renamePattern(renamePattern, arm.pattern);
                if (auto* lambda = std::get_if<Lambda>(&expr.node)) {
                    for (auto& param : lambda->parameters) if (param.type) renameType(*param.type);
                    if (lambda->result) renameType(*lambda->result);
                }
            }
        }
    }
};

}

Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<fs::path>& testFile) {
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
        for (auto item = fs::recursive_directory_iterator(directory); item != fs::recursive_directory_iterator{}; ++item) {
            if (item->is_directory() && pathText(item->path().filename()).starts_with('.')) { item.disable_recursion_pending(); continue; }
            if (item->is_symlink()) throw Diagnostic{"LYI0001", "源码不允许符号链接：" + pathText(item->path())};
            if (item->is_regular_file() && item->path().extension() == ".ly") paths.push_back(item->path());
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            auto module = prefixes.at(id);
            auto relative = path.parent_path().lexically_relative(directory);
            if (relative != ".") module += '/' + pathText(relative);
            spaces.try_emplace(module, Space{package});
            program.sources.push_back(loadSource(path, module));
        }
    }
    auto root = prefixes.at(entry);
    if (testFile) {
        root += "#test";
        spaces.emplace(root, Space{packages.at(entry)});
        program.sources.push_back(loadSource(*testFile, root));
    }
    std::map<std::string, MacroPackage> macroPackages;
    for (const auto& [module, space] : spaces) macroPackages.emplace(module, MacroPackage{space.package->root, space.package->macroInputs});
    for (std::size_t file = 0; file < program.sources.size(); ++file) {
        auto& source = program.sources[file];
        parsePackageFile(program.syntax, source, file, macroPackages.at(source.moduleId), program.macroInputs);
    }
    expandMacros(program.syntax, program.sources, macroPackages, program.macroInputs);
    std::map<std::string, ResolvedPackage> bundled;
    auto standard = sdkRoot() / "share" / "lyra" / "std";
    for (std::size_t index = 0; index < program.syntax.imports.size(); ++index) {
        const auto& use = program.syntax.imports[index];
        if (use.path.size() < 3 || use.path[0].text != "std") continue;
        auto name = use.path[1].text;
        auto key = "$std/" + name;
        if (bundled.contains(key)) continue;
        auto packageRoot = standard / name;
        if (!fs::is_directory(packageRoot / "src")) throw at(program.sources[use.file], use.path[1].span, "LYN0003", "找不到随工具链安装的标准包：" + name);
        auto& package = bundled.emplace(key, ResolvedPackage{key, name, "builtin", packageRoot, {}, true}).first->second;
        prefixes.emplace(key, key);
        spaces.emplace(key, Space{&package});
        auto loaded = loadSources(packageRoot);
        for (auto& source : loaded) {
            source.moduleId = key;
            program.sources.push_back(std::move(source));
            parseFile(program.syntax, program.sources.back(), program.sources.size() - 1);
        }
    }
    Linker{program.sources, spaces, prefixes, root}.rewrite(program.syntax);
    return program;
}

}
