module;
#include <algorithm>
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
#include <vector>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SHA256.h>

export module lyra.doc;
import lyra.check;
import lyra.generics;
import lyra.macros;
import lyra.modules;
import lyra.packages;
import lyra.platform;

export namespace lyra {
int documentReference(std::span<const std::string_view> args, std::string_view version, AnalysisDiagnostics& diagnostics);
}

namespace lyra {
namespace {
namespace fs = std::filesystem;
namespace json = llvm::json;

struct DocParameter {
    std::string name, spelling, type;
    Passing passing{};
    bool variadic{};
    std::optional<std::string> defaultValue{};
    std::string documentation{};
};

struct Entry {
    std::string name, spelling, kind, prefix{}, suffix{};
    std::optional<SourceAnchor> anchor{};
    std::string documentation{}, result{}, receiver{}, context{}, overload{};
    std::vector<DocParameter> parameters{};
    std::vector<std::string> typeParameters{}, throwsTypes{};
    std::vector<Entry> members{};
    std::string implementationSignature{};
};

std::string sourceTextAt(const Source& source, Span span) {
    auto text = std::string_view(source.text).substr(span.begin, span.end - span.begin);
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\r') result += text[index];
        else if (index + 1 == text.size() || text[index + 1] != '\n') result += '\n';
    }
    return result;
}

std::string bareName(std::string_view name) {
    auto separator = name.rfind("::");
    return std::string(separator == name.npos ? name : name.substr(separator + 2));
}

Entry declared(std::string kind, std::string prefix, const Identifier& name, const Source& source, std::size_t file) {
    auto spelling = source.origin ? editorIdentifier(bareName(name.text)) : std::string{};
    if (spelling.empty()) spelling = editorSpelling(source, name);
    return {bareName(name.text), std::move(spelling), std::move(kind), std::move(prefix), {}, SourceAnchor{file, name.span},
        documentationText(source, name.documentation)};
}

std::string typeSpelling(const Source& source, const Identifier& type) {
    return type.span.begin < type.span.end ? sourceTextAt(source, type.span) : bareName(type.text);
}

std::string generics(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) {
        result += result.empty() ? '<' : ',';
        if (result.back() == ',') result += ' ';
        result += name;
    }
    if (!result.empty()) result += '>';
    return result;
}

void typeParameters(Entry& entry, const Source& source, std::span<const Identifier> parameters, std::span<const Bound> bounds = {}, std::size_t start = 0) {
    for (std::size_t index = start; index < parameters.size(); ++index) {
        auto parameter = editorSpelling(source, parameters[index]);
        bool first = true;
        for (const auto& bound : bounds) if (bound.parameter == index) {
            parameter += std::exchange(first, false) ? ": " : " & ";
            parameter += typeSpelling(source, bound.trait);
            if (!bound.bindings.empty()) {
                std::vector<std::string> bindings;
                for (const auto& binding : bound.bindings)
                    bindings.push_back(editorSpelling(source, binding.member) + " = " + typeSpelling(source, binding.type));
                parameter += generics(bindings);
            }
        }
        entry.typeParameters.push_back(std::move(parameter));
    }
    entry.suffix += generics(entry.typeParameters);
}

DocParameter parameter(const Source& source, const Identifier* name, const Identifier& type) {
    return {name ? name->text : "", name ? editorSpelling(source, *name) : "", typeSpelling(source, type), {}, false, {},
        name ? documentationText(source, name->documentation) : ""};
}

std::string arguments(const std::vector<DocParameter>& params) {
    std::string result = "(";
    for (const auto& param : params) {
        if (&param != &params.front()) result += ", ";
        if (param.passing != Passing::Value) result += std::string(passingName(param.passing)) + ' ';
        if (!param.spelling.empty()) result += param.spelling + ": ";
        result += param.type;
        if (param.variadic) result += "...";
        if (param.defaultValue) result += " = " + *param.defaultValue;
    }
    return result + ')';
}

void functionParameters(Entry& entry, const Source& source, std::span<const Parameter> parameters, std::span<const Expr> expressions = {}) {
    for (const auto& param : parameters) {
        auto item = parameter(source, &param.name, param.type);
        item.passing = param.passing;
        item.variadic = param.variadic;
        if (param.defaultValue) item.defaultValue = sourceTextAt(source, expressions[*param.defaultValue].span);
        entry.parameters.push_back(std::move(item));
    }
    entry.suffix += arguments(entry.parameters);
}

Entry functionEntry(const Function& fn, const Source& source, bool member, std::size_t enclosing = 0) {
    auto prefix = fn.extensionGroup ? std::string{} : std::string("pub ");
    if (fn.constant != ConstKind::None) prefix += "const ";
    if (fn.owner && !fn.hasReceiver && !fn.initializer) prefix += "static ";
    if (fn.isAsync) prefix += "async ";
    if (fn.foreignSymbol) prefix += "extern ";
    if (fn.property) prefix += "val ";
    else if (!fn.conversion && !fn.initializer && fn.constant != ConstKind::Value) prefix += "fn ";
    auto kind = fn.initializer ? "constructor" : fn.conversion ? "conversion" : fn.property ? "property" :
        fn.constant == ConstKind::Value ? "constant" : !member ? "function" : fn.hasReceiver ? "method" : "staticMethod";
    auto item = declared(kind, std::move(prefix), fn.name, source, fn.file);
    if (fn.conversion) item.name = item.spelling = "implicit";
    if (fn.initializer) item.name = item.spelling = "init";
    typeParameters(item, source, fn.typeParameters, fn.bounds, enclosing);
    if (fn.conversion) item.suffix += ' ';
    if (!fn.property && fn.constant != ConstKind::Value)
        functionParameters(item, source, std::span(fn.parameters).subspan(fn.hasReceiver), fn.expressions);
    item.result = fn.initializer ? bareName(fn.result.text) : typeSpelling(source, fn.result);
    if (!fn.initializer && !item.result.empty()) item.suffix += ": " + item.result;
    for (const auto& type : fn.throwsTypes) {
        item.suffix += item.throwsTypes.empty() ? " throws " : ", ";
        item.throwsTypes.push_back(typeSpelling(source, type));
        item.suffix += item.throwsTypes.back();
    }
    return item;
}

std::string hashShape(std::span<const std::string> shape) {
    llvm::SHA256 hash;
    for (const auto& part : shape) {
        hash.update(std::to_string(part.size()));
        hash.update(":");
        hash.update(part);
    }
    return llvm::toHex(hash.final(), true).substr(0, 24);
}

std::string overloadId(const ReferenceCatalog& catalog, const std::vector<std::string>& shape) {
    std::vector<std::string> stable{shape.front()};
    for (auto part : std::span(shape).subspan(1)) {
        auto start = part.starts_with("->") ? 2 : part.find(':') + 1;
        auto type = std::string_view(part).substr(start);
        bool variadic = type.ends_with("...");
        if (variadic) type.remove_suffix(3);
        stable.push_back(part.substr(0, start) + mapTypeNames(typeForm(type), [&](const std::string& name) {
            auto separator = name.find("::");
            if (separator == name.npos) return name;
            return catalog.modules.at(name.substr(0, separator)).name + name.substr(separator);
        }) + (variadic ? "..." : ""));
    }
    return hashShape(stable);
}

std::map<SourceAnchor, Entry> declarations(const Compilation& program, const ReferenceCatalog& catalog, const ReferenceOverloads& overloads) {
    const auto& syntax = program.syntax;
    const auto& sources = program.sources;
    std::map<SourceAnchor, Entry> entries;
    std::map<std::string, std::vector<const Function*>> members;
    for (const auto& fn : syntax.functions) {
        if (fn.owner) members[fn.owner->text].push_back(&fn);
        else if (fn.extensionGroup) members[fn.extensionGroup->text].push_back(&fn);
    }
    auto store = [&](Entry entry) { entries.emplace(*entry.anchor, std::move(entry)); };
    auto callable = [&](const Function& fn, bool member, std::size_t enclosing = 0) {
        auto entry = functionEntry(fn, sources[fn.file], member, enclosing);
        if (entry.kind != "constant" && entry.kind != "property") entry.overload = overloadId(catalog, overloads.at(*entry.anchor));
        return entry;
    };
    auto methods = [&](Entry& entry, const auto& item) {
        for (const auto* fn : members[item.name.text]) if (fn->isPublic) entry.members.push_back(callable(*fn, true, item.typeParameters.size()));
    };
    for (const auto& fn : syntax.functions) if (fn.isPublic && !fn.owner && !fn.extensionGroup) store(callable(fn, false));
    for (const auto& item : syntax.aliases) if (item.isPublic) {
        const auto& source = sources[item.file];
        auto entry = declared("typeAlias", "pub type ", item.name, source, item.file);
        typeParameters(entry, source, item.typeParameters);
        entry.result = typeSpelling(source, item.target);
        entry.suffix += " = " + entry.result;
        store(std::move(entry));
    }
    for (const auto& item : syntax.exceptions) if (item.isPublic) {
        auto entry = declared("exception", "pub exception ", item.name, sources[item.file], item.file);
        Entry constructor{entry.name, entry.spelling, "constructor", {}, "(message: String)", entry.anchor, entry.documentation, entry.name};
        constructor.parameters.push_back({"message", "message", "String"});
        entry.members.push_back(std::move(constructor));
        store(std::move(entry));
    }
    for (const auto& item : syntax.aggregates) if (item.isPublic) {
        const auto& source = sources[item.file];
        auto entry = declared(item.reference ? "class" : "struct", item.reference ? "pub class " : "pub struct ", item.name, source, item.file);
        typeParameters(entry, source, item.typeParameters, item.bounds);
        bool init = std::ranges::any_of(members[item.name.text], [](const Function* fn) { return fn->initializer; });
        bool publicConstructor = !init;
        Entry constructor{entry.name, entry.spelling, "constructor", {}, {}, entry.anchor, entry.documentation, entry.name};
        for (const auto& field : item.fields) {
            if (!field.isPublic && !field.defaultValue) publicConstructor = false;
            if (!field.isPublic) continue;
            auto arg = parameter(source, &field.name, field.type);
            if (field.defaultValue) {
                const auto& fn = syntax.functions[field.defaultValue->function];
                arg.defaultValue = sourceTextAt(sources[fn.file], fn.expressions[fn.body].span);
            }
            auto member = declared("field", field.writable ? "pub var " : "pub val ", field.name, source, item.file);
            member.result = arg.type;
            member.suffix = ": " + arg.type;
            if (arg.defaultValue) member.suffix += " = " + *arg.defaultValue;
            constructor.parameters.push_back(std::move(arg));
            entry.members.push_back(std::move(member));
        }
        if (publicConstructor) {
            constructor.suffix = arguments(constructor.parameters);
            entry.members.push_back(std::move(constructor));
        }
        for (const auto& type : item.associated) if (type.isPublic) {
            auto member = declared("associatedType", "pub type ", type.name, source, item.file);
            member.result = typeSpelling(source, type.type);
            member.suffix = " = " + member.result;
            entry.members.push_back(std::move(member));
        }
        methods(entry, item);
        store(std::move(entry));
    }
    for (const auto& item : syntax.enums) if (item.isPublic) {
        const auto& source = sources[item.file];
        auto entry = declared("enum", "pub enum ", item.name, source, item.file);
        typeParameters(entry, source, item.typeParameters, item.bounds);
        for (const auto& variant : item.cases) {
            auto member = declared("case", "", variant.name, source, item.file);
            for (const auto& field : variant.fields) member.parameters.push_back(parameter(source, field.name ? &*field.name : nullptr, field.type));
            if (!member.parameters.empty()) member.suffix = arguments(member.parameters);
            entry.members.push_back(std::move(member));
        }
        methods(entry, item);
        store(std::move(entry));
    }
    for (const auto& item : syntax.traits) if (item.isPublic) {
        const auto& source = sources[item.file];
        auto entry = declared("trait", "pub trait ", item.name, source, item.file);
        for (const auto& type : item.associated) entry.members.push_back(declared("associatedType", "type ", type, source, item.file));
        for (const auto& method : item.methods) {
            auto member = declared("method", method.asynchronous ? "async fn " : "fn ", method.name, source, item.file);
            typeParameters(member, source, method.typeParameters, method.bounds);
            functionParameters(member, source, method.parameters);
            member.result = typeSpelling(source, method.result);
            member.suffix += ": " + member.result;
            entry.members.push_back(std::move(member));
        }
        store(std::move(entry));
    }
    for (const auto& item : syntax.extensions) if (item.isPublic) {
        const auto& source = sources[item.file];
        auto entry = declared("extension", "pub extension ", item.name, source, item.file);
        typeParameters(entry, source, item.typeParameters, item.bounds);
        entry.receiver = typeSpelling(source, item.receiver);
        entry.suffix += " for " + entry.receiver;
        methods(entry, item);
        for (auto& member : entry.members) member.receiver = entry.receiver;
        store(std::move(entry));
    }
    return entries;
}

std::string entryId(std::string_view name, const Entry& entry) {
    auto result = std::string(name) + ':' + entry.kind;
    if (!entry.overload.empty()) result += ':' + entry.overload;
    return result;
}

json::Object location(const Source& source, Span span, const ReferenceModule& module) {
    auto site = source.userSite(span);
    auto file = fromUtf8(site.file).lexically_relative(module.root);
    return json::Object{{"package", module.package}, {"file", pathText(file)}, {"line", site.start.line}, {"column", site.start.column},
        {"endLine", site.end.line}, {"endColumn", site.end.column}};
}

struct Document {
    std::map<std::string, json::Object> modules;
    std::map<std::string, json::Object> entries;

    void add(const Entry& entry, std::string_view module, std::string_view name, std::string_view spelling,
        std::string qualified, json::Value place = nullptr, bool generated = false, std::string_view owner = {},
        std::string_view origin = {}, std::string_view docs = {}) {
        auto id = entryId(qualified, entry);
        json::Array parameters;
        for (const auto& param : entry.parameters) parameters.emplace_back(json::Object{
            {"name", param.name}, {"type", param.type}, {"passing", std::string(param.passing == Passing::Value ? "value" : passingName(param.passing))},
            {"variadic", param.variadic}, {"default", param.defaultValue ? json::Value(*param.defaultValue) : json::Value(nullptr)}, {"documentation", param.documentation}});
        json::Object object{{"id", id}, {"module", std::string(module)}, {"name", std::string(name)}, {"qualifiedName", qualified}, {"kind", entry.kind}, {"visibility", "public"},
            {"signature", entry.prefix + std::string(spelling) + entry.suffix}, {"documentation", docs.empty() ? entry.documentation : std::string(docs)},
            {"parameters", std::move(parameters)}, {"typeParameters", json::Array(entry.typeParameters)}, {"throws", json::Array(entry.throwsTypes)},
            {"source", std::move(place)}, {"generated", generated}};
        if (!entry.result.empty()) object["result"] = entry.result;
        if (!entry.receiver.empty()) object["receiver"] = entry.receiver;
        if (!entry.context.empty()) object["context"] = entry.context;
        if (!entry.implementationSignature.empty()) object["implementationSignature"] = entry.implementationSignature;
        if (!owner.empty()) object["ownerId"] = std::string(owner);
        if (!origin.empty()) object["originId"] = std::string(origin);
        if (!entries.emplace(id, std::move(object)).second) throw Diagnostic{"LYC0001", "公开 API 标识重复：" + id};
    }

    void addProgram(const Compilation& program, const ReferenceCatalog& catalog, const ReferenceOverloads& overloads) {
        auto declared = declarations(program, catalog, overloads);
        std::map<SourceAnchor, const Function*> macros;
        for (const auto& fn : catalog.macros.functions) macros.emplace(SourceAnchor{fn.file, fn.name.span}, &fn);
        for (const auto& [key, scope] : catalog.modules) if (scope.selected)
            modules.try_emplace(scope.name, json::Object{{"name", scope.name}, {"package", scope.package}, {"version", scope.version}});
        for (const auto& binding : catalog.exports) {
            const auto& scope = catalog.modules.at(binding.module);
            if (!scope.selected) continue;
            const auto& sources = binding.macro ? catalog.macroSources : program.sources;
            const auto& source = sources[binding.origin.file];
            const auto& originScope = catalog.modules.at(source.moduleId);
            std::optional<Entry> macro;
            if (binding.macro) {
                auto& fn = *macros.at(binding.origin);
                auto kind = *binding.macro == MacroKind::Decorator ? "decorator" : *binding.macro == MacroKind::Expression ? "expressionMacro" :
                    *binding.macro == MacroKind::TypedDeclaration ? "typedMacro" : "macro";
                macro = functionEntry(fn, source, false);
                macro->kind = kind;
                auto prefix = fn.isPublic ? macro->prefix : std::string("fn ");
                macro->implementationSignature = prefix + macro->spelling + macro->suffix;
                macro->prefix = *binding.macro == MacroKind::Decorator ? "@" : "#";
                macro->suffix.clear();
                macro->parameters.clear();
                macro->result.clear();
            }
            const auto& entry = macro ? *macro : declared.at(binding.origin);
            auto qualified = scope.name + "::" + binding.name;
            auto original = originScope.name + "::" + entry.name;
            std::string aliasDocs, aliasSpelling;
            auto place = location(source, binding.origin.span, originScope);
            if (binding.alias) {
                const auto& alias = program.sources[binding.alias->file];
                place = location(alias, binding.alias->span, scope);
                aliasDocs = documentationText(alias, binding.documentation);
                aliasSpelling = sourceTextAt(alias, binding.alias->span);
            }
            add(entry, scope.name, binding.name, binding.alias ? aliasSpelling : entry.spelling, qualified, std::move(place),
                source.origin.has_value(), {}, binding.alias ? entryId(original, entry) : "", aliasDocs);
            for (const auto& member : entry.members) {
                const auto& file = sources[member.anchor->file];
                bool rename = binding.alias && member.kind == "constructor" && member.name == entry.name;
                const auto& name = rename ? binding.name : member.name;
                add(member, scope.name, name, rename ? aliasSpelling : member.spelling, qualified + '.' + name,
                    location(file, member.anchor->span, originScope), file.origin.has_value(), entryId(qualified, entry),
                    binding.alias ? entryId(original + '.' + member.name, member) : "");
            }
        }
    }

    void addBuiltins() {
        constexpr auto name = "builtin";
        modules.try_emplace(name, json::Object{{"name", name}, {"package", name}, {"version", "builtin"}});
        auto builtins = builtinReference();
        std::map<std::string, std::string> owners;
        for (const auto& item : builtins) if (item.owner.empty()) owners.emplace(item.name, item.kind);
        for (const auto& item : builtins) {
            auto qualified = std::string(name) + "::" + (item.owner.empty() ? item.name : item.owner + '.' + item.name);
            auto owner = item.owner.empty() ? "" : std::string(name) + "::" + item.owner + ':' + owners.at(item.owner);
            Entry entry{item.name, item.name, item.kind};
            entry.context = item.context;
            entry.result = item.result;
            entry.documentation = item.documentation;
            entry.typeParameters = item.typeParameters;
            if (item.kind == "type" || item.kind == "enum" || item.kind == "trait" || item.kind == "exception" || item.kind == "typeAlias") {
                entry.prefix = (item.kind == "typeAlias" ? "type" : item.kind) + std::string(" ");
                entry.suffix = generics(entry.typeParameters);
                if (!entry.result.empty()) entry.suffix += " = " + entry.result;
            } else {
                for (const auto& param : item.parameters) entry.parameters.push_back({param.name, param.name, param.type, {}, false, param.defaultValue});
                if (item.kind == "function" || item.kind == "staticMethod" || item.kind == "method") entry.prefix = item.kind == "staticMethod" ? "static fn " : "fn ";
                else if (item.kind == "property") entry.prefix = "val ";
                if (item.kind != "property" && (item.kind != "case" || !entry.parameters.empty()))
                    entry.suffix = generics(entry.typeParameters) + arguments(entry.parameters);
                if (!entry.result.empty() && item.kind != "case") entry.suffix += ": " + entry.result;
                if (item.kind != "property" && item.kind != "case") {
                    std::vector<std::string> shape{std::to_string(entry.typeParameters.size())};
                    for (const auto& param : entry.parameters) shape.push_back(param.type);
                    entry.overload = hashShape(shape);
                }
            }
            add(entry, name, entry.name, entry.spelling, std::move(qualified), nullptr, false, owner);
        }
        auto source = macroPreludeSource();
        Module prelude;
        parseMacroPrelude(prelude, source);
        for (const auto& item : prelude.aggregates) if (item.isPublic) {
            auto entry = declared("type", "type ", item.name, source, item.file);
            entry.context = "macro";
            add(entry, name, entry.name, entry.spelling, std::string(name) + "::" + entry.name);
        }
        for (const auto& fn : prelude.functions) if (!fn.name.text.starts_with('$') && fn.name.text != "httpRouteOrigin") {
            auto entry = functionEntry(fn, source, false);
            entry.prefix = "fn ";
            entry.context = "macro";
            add(entry, name, entry.name, entry.spelling, std::string(name) + "::" + entry.name);
        }
    }

    std::string render(std::string_view version, bool standard) {
        json::Array api, scopes, missing;
        for (auto& [name, item] : modules) scopes.emplace_back(std::move(item));
        for (auto& [id, item] : entries) {
            if (item.getString("documentation")->empty()) missing.emplace_back(id);
            api.emplace_back(std::move(item));
        }
        json::Object result{{"schemaVersion", 1}, {"compilerVersion", std::string(version)}, {"modules", std::move(scopes)},
            {"entries", std::move(api)}, {"missingDocumentation", std::move(missing)}};
        if (standard) {
            auto keywords = syntaxKeywords();
            std::ranges::sort(keywords);
            json::Array reserved, contextual, operators;
            for (auto word : keywords) reserved.emplace_back(std::string(word));
            for (auto word : syntaxContextualKeywords()) contextual.emplace_back(std::string(word));
            for (const auto& item : syntaxOperators()) operators.emplace_back(json::Object{{"spelling", std::string(item.spelling)}, {"precedence", item.precedence},
                {"associativity", std::string(item.associativity)}, {"fixity", std::string(item.fixity)}});
            result["syntax"] = json::Object{{"keywords", std::move(reserved)}, {"contextualKeywords", std::move(contextual)}, {"operators", std::move(operators)}};
        }
        return llvm::formatv("{0:2}\n", json::Value(std::move(result))).str();
    }
};

void protectOutput(const fs::path& output, const fs::path& input) {
    if (!output.empty() && fs::exists(output) && fs::exists(input) && fs::equivalent(output, input))
        throw Diagnostic{"LYI0001", "文档输出不能覆盖编译输入：" + pathText(input)};
}

}

int documentReference(std::span<const std::string_view> args, std::string_view version, AnalysisDiagnostics& diagnostics) {
    fs::path root, output;
    PackageOptions options;
    bool standard = false;
    auto usage = [] { throw Diagnostic{"LYC0001", "用法：lyra doc (<包目录> [--locked] [--offline] | --stdlib) [-o <文件>]"}; };
    for (std::size_t index = 0; index < args.size(); ++index) {
        auto arg = args[index];
        if (arg == "--stdlib" && !standard) standard = true;
        else if (arg == "--locked" && !options.locked) options.locked = true;
        else if (arg == "--offline" && !options.offline) options.offline = true;
        else if (arg == "-o" && output.empty() && index + 1 < args.size() && !args[index + 1].empty() && !args[index + 1].starts_with('-'))
            output = fs::absolute(fromUtf8(args[++index]));
        else if (root.empty() && !arg.empty() && !arg.starts_with('-')) root = fromUtf8(arg);
        else usage();
    }
    if (standard ? (!root.empty() || options.locked || options.offline) : root.empty()) usage();
    Document doc;
    auto collect = [&](const Compilation& program, const ReferenceCatalog& catalog) {
        for (const auto& source : program.sources) if (!source.origin) protectOutput(output, source.path);
        for (const auto& source : catalog.macroSources) protectOutput(output, source.path);
        for (const auto& input : program.macroInputs) protectOutput(output, input);
        ReferenceOverloads overloads;
        auto checked = checkReference(program.syntax, program.sources, overloads);
        for (auto& warning : checked.warnings) diagnostics.warnings.push_back(std::move(warning));
        doc.addProgram(program, catalog, overloads);
    };
    if (standard) {
        ReferenceCatalog catalog;
        auto program = loadStandardReference(catalog);
        collect(program, catalog);
        doc.addBuiltins();
    } else {
        auto graph = resolvePackages(root, options);
        if (!graph.missingOutputs.empty()) throw Diagnostic{"LYK0007", "未执行构建脚本，" + graph.missingOutputs.front()};
        protectOutput(output, graph.workspace / "lyra.lock");
        for (const auto& pkg : graph.packages) protectOutput(output, pkg.root / "lyra.json5");
        for (const auto& warning : graph.warnings) diagnostics.warnings.push_back({"LYK0007", warning});
        auto entries = graph.entry.empty() ? graph.members : std::vector<std::string>{graph.entry};
        for (const auto& id : entries) {
            ReferenceCatalog catalog;
            auto program = loadProgram(graph, id, {}, nullptr, nullptr, false, nullptr, nullptr, &catalog);
            collect(program, catalog);
        }
    }
    auto bytes = doc.render(version, standard);
    if (output.empty()) {
        std::cout << bytes;
        std::cout.flush();
        if (!std::cout) throw Diagnostic{"LYI0001", "无法写入文档输出"};
    } else {
        StagingDirectory staging(output.parent_path());
        auto temporary = staging.path / output.filename();
        std::ofstream stream(temporary, std::ios::binary);
        stream << bytes;
        stream.close();
        if (!stream) throw Diagnostic{"LYI0001", "无法写入文档输出：" + pathText(output)};
        publishExecutable(temporary, output);
    }
    return 0;
}

}
