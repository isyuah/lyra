module;
#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

export module lyra.modules;
import lyra.packages;
import lyra.syntax;

export namespace lyra {
struct Compilation { std::vector<Source> sources; Module syntax; };
Compilation loadProgram(const PackageGraph& graph, const std::string& entry, const std::optional<std::filesystem::path>& testFile = {});
}

namespace lyra {
namespace fs = std::filesystem;
namespace {

struct Symbol { std::string name; bool visible; };
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
        if (!defs.emplace(id.text, Symbol{name, visible}).second) throw at(sources[file], id.span, "LYN0002", "名称已声明：" + id.text);
        id.text = std::move(name);
    }

    Symbol resolve(const Import& use, std::set<const Import*>& active) {
        const auto& source = sources[use.file];
        if (!active.insert(&use).second) throw at(source, use.alias.span, "LYN0004", "重导出形成循环：" + use.alias.text);
        const auto& from = *spaces.at(source.moduleId).package;
        const auto& first = use.path.front();
        if (use.path.size() < 2) throw at(source, first.span, "LYN0003", "import 需要包、可选模块路径和声明名");
        auto package = from.id;
        if (first.text != "self") {
            auto found = from.dependencies.find(first.text);
            if (found == from.dependencies.end()) throw at(source, first.span, "LYN0003", "清单没有此依赖简称：" + first.text);
            package = found->second;
        }
        auto target = prefixes.at(package);
        for (std::size_t index = 1; index + 1 < use.path.size(); ++index) target += '/' + use.path[index].text;
        auto& name = use.path.back();
        auto found = spaces.find(target);
        if (found == spaces.end()) throw at(source, name.span, "LYN0003", "导入的源码模块不存在");
        auto& space = found->second;
        if (auto hit = space.definitions.find(name.text); hit != space.definitions.end() && hit->second.visible) return {hit->second.name, use.isPublic};
        if (auto alias = space.imports.find(name.text); alias != space.imports.end() && alias->second->isPublic) {
            auto symbol = resolve(*alias->second, active);
            return {std::move(symbol.name), use.isPublic};
        }
        throw at(source, name.span, "LYN0003", "目标模块没有公开声明：" + name.text);
    }

    void rewrite(Module& syntax) {
        for (auto& record : syntax.aggregates) declare(record.file, record.name, record.isPublic, false);
        for (auto& fn : syntax.functions) if (!fn.owner) declare(fn.file, fn.name, fn.isPublic, true);
        for (const auto& use : syntax.imports) {
            const auto& source = sources[use.file];
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
        for (auto& record : syntax.aggregates) {
            const auto& scope = spaces.at(sources[record.file].moduleId).definitions;
            for (auto& field : record.fields) if (auto found = scope.find(field.type.text); found != scope.end()) field.type.text = found->second.name;
        }
        for (auto& fn : syntax.functions) {
            const auto& scope = spaces.at(sources[fn.file].moduleId).definitions;
            auto rename = [&](Identifier& name) {
                if (auto found = scope.find(name.text); found != scope.end()) name.text = found->second.name;
            };
            auto renameType = [&](Identifier& name) {
                if (std::none_of(fn.typeParameters.begin(), fn.typeParameters.end(), [&](const auto& parameter) { return parameter.text == name.text; })) rename(name);
            };
            if (fn.owner) rename(*fn.owner);
            renameType(fn.result);
            for (auto& param : fn.parameters) renameType(param.type);
            for (auto& expr : fn.expressions) {
                if (auto* call = std::get_if<Call>(&expr.node)) {
                    if (!call->receiver) {
                        if (call->name.text == "main" && !scope.contains("main")) throw at(sources[fn.file], call->name.span, "LYN0001", "找不到函数：main");
                        rename(call->name);
                    }
                    for (auto& type : call->typeArguments) renameType(type);
                }
                if (auto* binding = std::get_if<Binding>(&expr.node); binding && binding->annotation) renameType(*binding->annotation);
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
        for (const auto& item : fs::recursive_directory_iterator(directory)) {
            if (item.is_regular_file() && item.path().extension() == ".ly") paths.push_back(item.path());
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            auto module = prefixes.at(id);
            auto relative = path.parent_path().lexically_relative(directory);
            if (relative != ".") module += '/' + relative.generic_string();
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
    program.syntax = parse(program.sources);
    Linker{program.sources, spaces, prefixes, root}.rewrite(program.syntax);
    return program;
}

}
